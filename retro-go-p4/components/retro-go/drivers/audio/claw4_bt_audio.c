/*
 * Claw4 (ESP32-P4) 音频 sink —— I2S → 板载蓝牙音频编解码芯片（UART2 AT 命令控制）。
 *
 * 一切参数都抄自**官方 MetalioClaw4 固件**（CloudZao/MetalioClaw4，实测出声的那一份），
 * 不是我猜的 —— 这是本项目"官方 demo 不会错"原则的落实：
 *   main/boards/common/bt_audio_codec.cc        I2S 参数（★ 从机 / 32bit 槽 / 软件音量）
 *   main/boards/metalio-claw-4/metalio-claw-4.cc:209  InitializeBTAudio()：UART2 + 默认模式
 *   main/display/screen/bluetooth_screen/bluetooth_screen.cc:795  ApplyDefaultMode()
 *                                                 ⇒ "AT+RX=2 → 700ms → AT+MODE=1"
 *   main/boards/common/IOExpander.hpp            PA=P1-0（audio PA enable）、BT_POWER=P0-6
 *   main/boards/metalio-claw-4/config.h          DOUT=9 / BCLK=12 / WS=10 / UART2 TX26 RX27
 *
 * 四条硬事实（每条都曾是"猜错就完全没声/怪声"的坑）：
 *   1. I2S 必须【从机】(I2S_ROLE_SLAVE)：BCLK/WS 由芯片给 ⇒ 采样率由芯片决定（官方 16kHz）。
 *   2. 数据必须【32bit 槽、样本乘音量后左对齐】(官方 Write() 就是 int16 × (vol/100)² × 65536)。
 *   3. 芯片必须先收到 AT+RX=2 → 700ms → AT+MODE=1，否则不出声。
 *   4. PA 与蓝牙芯片供电都在 TCA9555 上（P1-0 / P0-6），不是 GPIO。
 *
 * 定拍来源：i2s_channel_write() 阻塞在 I2S DMA 上 —— 与 Tab5 的 esp_codec_dev_write() 同机制。
 * 安全铁律：任何一次等待都用有限超时；芯片没时钟时宁可静音也【绝不】卡住核心循环
 * 。
 */
#include "rg_audio.h"
#include "rg_system.h"

#if RG_AUDIO_USE_CLAW4_BT

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>   /* PRIu32：本平台 uint32_t 是 long unsigned，用 %u 会被 -Werror=format 拦下 */
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"

#define CLAW4_AUDIO_I2S_PORT    I2S_NUM_0
#define CLAW4_AUDIO_UART_PORT   UART_NUM_2
#define CLAW4_AUDIO_UART_BAUD   115200
#define CLAW4_AUDIO_UART_TX     26
#define CLAW4_AUDIO_UART_RX     27
#define CLAW4_AUDIO_BCLK_GPIO   12
#define CLAW4_AUDIO_WS_GPIO     10
#define CLAW4_AUDIO_DOUT_GPIO   9
#define CLAW4_AUDIO_DMA_DESC    6
#define CLAW4_AUDIO_DMA_FRAME   240
#define CLAW4_AUDIO_FALLBACK_RATE 16000 /* 芯片侧固定 16kHz（官方 config.h 的 AUDIO_OUTPUT_SAMPLE_RATE）*/
#define CLAW4_AUDIO_WRITE_MS    200     /* 单次 DMA 等待上限。
                                         * 实测结论（2026-10-11）：这个等待**同时兼任模拟器的定拍**。
                                         *   200ms 档 ⇒ 速度正确(音频满)、但 BUSY 33%/FPS 39（等待与渲染串行）
                                         *   3ms  档 ⇒ BUSY 98%/FPS 72（跑快 20%）且 85% 音频被丢 ⇒ 断续
                                         * 正解 = 让等待与渲染并行（独立写任务 + 环形缓冲），需要内部 RAM。 */
#define CLAW4_AUDIO_FAIL_LIMIT  4       /* 连续失败这么多次就判定链路不可用，静音降级 */

/* TCA9555：与显示/触摸同一条 I2C 总线、同一颗芯片（官方注释：不可各建各的 I2C 外设） */
extern i2c_master_bus_handle_t claw4_get_i2c_bus(void);
#define CLAW4_TCA_ADDR          0x20
#define CLAW4_TCA_OUT_PORT0     0x02
#define CLAW4_TCA_OUT_PORT1     0x03
#define CLAW4_TCA_CFG_PORT0     0x06    /* 方向寄存器：本板 PA/BT_POWER 已是输出，这里只写电平 */

static i2s_chan_handle_t tx_chan;
static int16_t *tx_buf;                 /* 32bit 槽的中间缓冲：count * 2 个 int32 */
static size_t tx_buf_frames;            /* 缓冲能装几帧 */
static int cur_volume = 50;             /* 0..100（官方纯软件音量） */
static int cur_rate = 0;
static int fail_streak = 0;
static uint32_t submit_count = 0, drop_count = 0;
static bool chip_inited = false;

/* ───────────────────────── TCA9555：拉高 PA(P1-0) 与 BT_POWER(P0-6) ─────────────────────────
 * 官方 metalio-claw-4.cc：BT 芯片供电 P0-6、PA=P1-0；PA 官方是开机 5 秒后再拉高。
 * ⚠ 必须保住 P0-3=0（SD 卡槽供电使能，见显示驱动）—— 读改写、不做整口覆盖。
 * 失败只告警：音频不通不该把游戏拖黑（与 SD 供电同款策略）。 */
static void claw4_audio_enable_rails(void)
{
    i2c_master_bus_handle_t bus = claw4_get_i2c_bus();
    if (!bus)
    {
        RG_LOGW("claw4 audio: i2c bus 不可用，跳过 PA/BT_POWER\n");
        return;
    }
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CLAW4_TCA_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK || !dev)
    {
        RG_LOGW("claw4 audio: TCA9555 未找到，跳过 PA/BT_POWER\n");
        return;
    }

    uint8_t reg = CLAW4_TCA_OUT_PORT0;
    uint8_t cur[2] = {0, 0};
    if (i2c_master_transmit_receive(dev, &reg, 1, cur, 2, 100) != ESP_OK)
    {
        RG_LOGW("claw4 audio: TCA9555 读回失败，用已知良好值兜底\n");
        cur[0] = 0xF7;  /* 显示驱动写的值：P0-3=0(SD 使能)，其余为 1 */
        cur[1] = 0x00;
    }
    const uint8_t want[3] = {
        CLAW4_TCA_OUT_PORT0,
        (uint8_t)(cur[0] | (1 << 6)),   /* P0-6 BT_POWER = 1 */
        (uint8_t)(cur[1] | (1 << 0)),   /* P1-0 PA       = 1 */
    };
    esp_err_t err = i2c_master_transmit(dev, want, sizeof(want), 100);
    RG_LOGI("claw4 audio: TCA9555 rails: P0=0x%02x P1=0x%02x (%s)\n",
            want[1], want[2], esp_err_to_name(err));
    i2c_master_bus_rm_device(dev);
}

/* ───────────────────────── UART2：给音频芯片发 AT 命令 ───────────────────────── */
static void claw4_audio_uart_write(const char *s)
{
    uart_write_bytes(CLAW4_AUDIO_UART_PORT, s, strlen(s));
    uart_wait_tx_done(CLAW4_AUDIO_UART_PORT, pdMS_TO_TICKS(200));
    RG_LOGI("claw4 audio: AT>>> %s", s);
}

static bool claw4_audio_chip_init(void)
{
    if (chip_inited)
        return true;

    const uart_config_t ucfg = {
        .baud_rate = CLAW4_AUDIO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(CLAW4_AUDIO_UART_PORT, 512, 0, 0, NULL, 0) != ESP_OK)
        return false;
    if (uart_param_config(CLAW4_AUDIO_UART_PORT, &ucfg) != ESP_OK)
        return false;
    if (uart_set_pin(CLAW4_AUDIO_UART_PORT, CLAW4_AUDIO_UART_TX, CLAW4_AUDIO_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
        return false;

    /* 官方 ApplyDefaultMode()：开机默认"模式 1（接收）"，两条命令间隔 700ms（协议要求） */
    claw4_audio_uart_write("AT+RX=2\r\n");
    rg_task_delay(700);
    claw4_audio_uart_write("AT+MODE=1\r\n");
    rg_task_delay(200);

    chip_inited = true;
    RG_LOGI("claw4 audio: 音频芯片已切到模式 1\n");
    return true;
}

/* ───────────────────────── I2S：从机 + 32bit 槽（照抄官方 bt_audio_codec.cc） ─────────────────────────
 * ⚠ 从机模式 ⇒ 采样率由芯片给；我们这里报的速率只是为了告诉核心"按 16k 喂数据"。 */
static bool claw4_audio_i2s_start(void)
{
    if (tx_chan)
        return true;

    const i2s_chan_config_t chan_cfg = {
        .id = CLAW4_AUDIO_I2S_PORT,
        .role = I2S_ROLE_SLAVE,             /* ★ 官方如此：时钟由编解码芯片提供 */
        .dma_desc_num = CLAW4_AUDIO_DMA_DESC,
        .dma_frame_num = CLAW4_AUDIO_DMA_FRAME,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK)
    {
        tx_chan = NULL;
        return false;
    }
    const i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = CLAW4_AUDIO_FALLBACK_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,   /* ★ 官方是 32bit 槽 */
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* 板上没有 MCLK */
            .bclk = CLAW4_AUDIO_BCLK_GPIO,
            .ws = CLAW4_AUDIO_WS_GPIO,
            .dout = CLAW4_AUDIO_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,         /* 本轮只做播放 */
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK ||
        i2s_channel_enable(tx_chan) != ESP_OK)
    {
        if (tx_chan) { i2s_del_channel(tx_chan); tx_chan = NULL; }
        return false;
    }
    return true;
}

/* ───────────────────────── sink 接口 ───────────────────────── */
static bool claw4_audio_init(int device, int sample_rate)
{
    (void)device;
    cur_rate = sample_rate;
    RG_LOGI("claw4 audio: init (核心要求 %dHz；芯片侧固定 %dHz ⇒ 已由 RG_AUDIO_FORCE_SAMPLE_RATE 对齐)\n",
            sample_rate, CLAW4_AUDIO_FALLBACK_RATE);

    claw4_audio_enable_rails();
    if (!claw4_audio_chip_init())
    {
        RG_LOGW("claw4 audio: UART 初始化失败，保持静音（不影响游戏）\n");
        return false;
    }
    if (!claw4_audio_i2s_start())
    {
        RG_LOGW("claw4 audio: I2S 启动失败，保持静音（不影响游戏）\n");
        return false;
    }
    fail_streak = 0;
    return true;
}

static bool claw4_audio_deinit(void)
{
    if (tx_chan)
    {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    if (tx_buf)
    {
        heap_caps_free(tx_buf);
        tx_buf = NULL;
        tx_buf_frames = 0;
    }
    return true;
}

static bool claw4_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    if (!tx_chan || !frames || !count)
        return true;                        /* 静音降级：绝不在这里断言/阻塞 */

    if (count > tx_buf_frames)
    {
        /* 中间缓冲放 PSRAM（内部 SRAM 只剩几百字节，见 docs/CLAW4-INTERNAL-HEAP-PLAN.md） */
        int16_t *nb = (int16_t *)heap_caps_realloc(tx_buf, count * 2 * sizeof(int32_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb)
        {
            static bool warned;
            if (!warned) { warned = true; RG_LOGW("claw4 audio: PSRAM 缓冲分配失败，静音\n"); }
            return true;
        }
        tx_buf = nb;
        tx_buf_frames = count;
    }

    /* 官方 Write()：int32 = int16 × (vol/100)² × 65536 ⇒ 32bit 槽里左对齐 */
    const int vol = (cur_volume < 0) ? 0 : (cur_volume > 100 ? 100 : cur_volume);
    const int64_t factor = (int64_t)((double)vol / 100.0 * (double)vol / 100.0 * 65536.0);
    int32_t *out = (int32_t *)tx_buf;

    /* ★ 重采样（实测必需，2026-10-11）：核心按【自己的原生率】喂数据（GBA=32768Hz，546 样本/帧），
     * 而音频芯片固定 16000Hz（实测：写入耗时 ÷ 样本数 = 16000）。两端不一致时产出是消耗的 2 倍
     * ⇒ DMA 永远满 ⇒ 每次写入都等整段播完 ⇒ BUSY 33%、FPS 39（手感"像 CPU 降频"）。
     * 用线性插值换算（32768→16000 是非整数比，最近邻会有明显噪声）。 */
    const int out_rate = CLAW4_AUDIO_FALLBACK_RATE;
    size_t n_out = count;
    if (cur_rate > 0 && cur_rate != out_rate)
        n_out = (size_t)((int64_t)count * out_rate / cur_rate);
    if (n_out == 0)
        return true;
    if (n_out == count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            int64_t l = (int64_t)frames[i].left * factor;
            int64_t r = (int64_t)frames[i].right * factor;
            out[i * 2 + 0] = (l > INT32_MAX) ? INT32_MAX : (l < INT32_MIN ? INT32_MIN : (int32_t)l);
            out[i * 2 + 1] = (r > INT32_MAX) ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
        }
    }
    else
    {
        for (size_t i = 0; i < n_out; ++i)
        {
            const int64_t num = (int64_t)i * cur_rate;
            const size_t i0 = (size_t)(num / out_rate);
            const size_t i1 = (i0 + 1 < count) ? (i0 + 1) : i0;
            const int64_t frac = (num % out_rate) * 256 / out_rate;   /* 0..255 */
            const int32_t sl = frames[i0].left + (int32_t)(((int64_t)(frames[i1].left - frames[i0].left) * frac) >> 8);
            const int32_t sr = frames[i0].right + (int32_t)(((int64_t)(frames[i1].right - frames[i0].right) * frac) >> 8);
            int64_t l = (int64_t)sl * factor;
            int64_t r = (int64_t)sr * factor;
            out[i * 2 + 0] = (l > INT32_MAX) ? INT32_MAX : (l < INT32_MIN ? INT32_MIN : (int32_t)l);
            out[i * 2 + 1] = (r > INT32_MAX) ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
        }
    }

    size_t written = 0;
    const int64_t t0 = rg_system_timer();
    esp_err_t err = i2s_channel_write(tx_chan, out, n_out * 2 * sizeof(int32_t),
                                      &written, pdMS_TO_TICKS(CLAW4_AUDIO_WRITE_MS));
    const int64_t took_us = rg_system_timer() - t0;
    const int64_t data_us = (int64_t)n_out * 1000000 / (CLAW4_AUDIO_FALLBACK_RATE ? CLAW4_AUDIO_FALLBACK_RATE : 1);
    if (err != ESP_OK && written == 0)
    {
        if (++fail_streak >= CLAW4_AUDIO_FAIL_LIMIT)
        {
            static bool warned;
            if (!warned)
            {
                warned = true;
                RG_LOGW("claw4 audio: I2S 连续 %d 次写失败(%s)，判定链路不可用 ⇒ 静音但不再拖慢游戏\n",
                        fail_streak, esp_err_to_name(err));
            }
        }
    }
    else
    {
        fail_streak = 0;
        if (written < n_out * 2 * sizeof(int32_t))
            drop_count++;
    }
    if ((++submit_count % 240) == 0)
        RG_LOGI("claw4 audio: submits=%" PRIu32 " drops=%" PRIu32
                " | 核心%dHz→芯片%dHz 重采样 %d→%d 样本，写入 %dus / 应播 %dus（比值 %d%%）\n",
                submit_count, drop_count, cur_rate, CLAW4_AUDIO_FALLBACK_RATE, (int)count, (int)n_out,
                (int)took_us, (int)data_us, (int)(data_us > 0 ? (took_us * 100 / data_us) : 0));
    return true;    /* 永远成功：音频问题绝不上升为游戏问题 */
}

static bool claw4_audio_set_mute(bool mute)
{
    (void)mute;     /* 静音用 set_volume(0) 表达，芯片侧无静音寄存器（官方也没有） */
    return true;
}

static bool claw4_audio_set_volume(int percent)
{
    cur_volume = percent;
    return true;
}

static bool claw4_audio_set_sample_rate(int sample_rate)
{
    cur_rate = sample_rate;
    RG_LOGI("claw4 audio: 核心改用 %dHz（芯片侧固定 %dHz）\n", sample_rate, CLAW4_AUDIO_FALLBACK_RATE);
    return true;
}

const rg_audio_driver_t rg_audio_driver_claw4_bt = {
    .name = "Claw4 BT Audio",
    .init = claw4_audio_init,
    .deinit = claw4_audio_deinit,
    .submit = claw4_audio_submit,
    .set_mute = claw4_audio_set_mute,
    .set_volume = claw4_audio_set_volume,
    .set_sample_rate = claw4_audio_set_sample_rate,
};

#endif // RG_AUDIO_USE_CLAW4_BT
