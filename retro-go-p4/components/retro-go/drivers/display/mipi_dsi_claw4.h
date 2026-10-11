/* ============================================================================
 * Metalio Claw4 (ESP32-P4 方屏 720×720) 显示驱动 — MIPI DSI / **NV3051F**
 * ----------------------------------------------------------------------------
 * 与 Tab5 驱动（drivers/display/mipi_dsi_tab5_p.h）的关系：
 *   **只复用"块推送"这条已验证路径**，tab5 那套专属机制一律不带过来 ——
 *     · 无 PPA 硬件旋转（本机不需要旋转）
 *     · 无 AXI-ICM QoS 调优 / 无 E0 带宽探针（那些是 tab5 抢带宽的产物，没证据不要搬）
 *     · 无 90° 转置映射（本机逻辑 = 物理、**线性 1:1**）
 *     · 无 M5Stack BSP 依赖（面板初始化走厂商序列 esp_lcd_nv3051f.c）
 *
 * 映射：逻辑 (lx, ly) -> 面板帧缓冲 (lx, ly)，帧缓冲行跨距 = 720px（无填充）。
 *   游戏区 240×160 @3x = 720×480 贴顶（y[0,480)），下方 240px 是虚拟手柄控制带；
 *   几何在 targets/claw4/geom.h，键位在 targets/claw4/touch_layout.h。
 *
 * 字节序：rg_display.c 对 RG_PIXEL_565_LE 源已交换过一次 ⇒ 到这里是 565 **大端**，
 *   DSI 面板要小端 ⇒ 逐像素换回（与 tab5 同一约定，见 claw4_swap16）。
 *
 * 关键坑（照 tab5 的经验，别踩第二遍）：
 *  - **背光必须在任何 set 之前初始化，且先置 0**（否则上电白闪）。
 *  - **必须调 RG_SCREEN_INIT()**（本 target 里是空宏，作为显式约定钩子；漏掉 = 显示驱动永不执行）。
 *  - esp_cache_msync 的**偏移与长度都必须是 128 字节的整数倍**，否则静默失败
 *    （720px 行 = 1440B，1440%128=32 ⇒ 直接拿窗口范围去 msync 一定踩坑，必须向外对齐）。
 *  - 不要自己写 esp_lcd_new_panel_dpi + DBI 初始化序列：交给厂商驱动，序列是厂里给的。
 *
 * 面板备选：厂商源码里另有 FL7707N（48MHz/RGB888）一套参数。若点不亮/花屏，
 *   第二个要试的就是它（另一套 DPI 时序 + 另一份 DCS 序列），不是先怀疑硬件。
 * 像素格式：厂商默认 24bpp（RGB888，93MB/s@60Hz）；我们走 **16bpp RGB565**
 *   （62MB/s，colmod 0x55）—— 由 vendor 驱动的 bits_per_pixel=16 自动配置。
 * ==========================================================================*/

#pragma once

#include <string.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"      /* esp_lcd_panel_io_tx_param：init 后改面板亮度 DCS */
#include "esp_lcd_mipi_dsi.h"      /* DPI 面板 + esp_lcd_dpi_panel_get_frame_buffer */
#include "esp_cache.h"             /* esp_cache_msync：写帧缓冲后必须写回 */
#include "esp_timer.h"             /* 诊断用一次性定时器（CLAW4_DIAG_MARKERS） */
#include "esp_ldo_regulator.h"     /* DSI PHY 供电（chan3 @2500mV） */
#include "driver/ledc.h"           /* 背光 PWM */
#include "driver/i2c_master.h"     /* TCA9555（SD 卡槽供电使能） */
#include "esp_lcd_nv3051f.h"       /* 厂商面板驱动（组件根目录，第三方 vendor 代码） */
#include "esp_heap_caps.h"         /* 电量灯影子缓冲（PSRAM） */
#include "rg_touch_overlay.h"      /* 虚拟按键可视层（共享模块；本 target 已定义键位表） */

/* ──────────────────────────── 硬件常量（厂商 config.h / 板级源码）──────────── */
#define CLAW4_PHYS_W            720        /* 面板物理宽（= 逻辑宽，无旋转） */
#define CLAW4_PHYS_H            720
#define CLAW4_PIN_LCD_RST       GPIO_NUM_3
#define CLAW4_PIN_BACKLIGHT     GPIO_NUM_52   /* PWM，高电平有效（厂商：不反相） */
#define CLAW4_DSI_LDO_CHAN      3
#define CLAW4_DSI_LDO_MV        2500
#define CLAW4_DSI_LANES         2
#define CLAW4_DSI_LANE_MBPS     1000
/* DPI 像素时钟：厂商**运行中**的板级代码用 48MHz（面板头里的 60Hz 宏是 36MHz）。
 * 取 48 追随"实际能跑的配置"；若点屏异常，第一档就试 36。 */
#define CLAW4_DPI_CLK_MHZ       48
/* 像素格式（**这里踩过坑，改之前先读完**）：
 *   fb 侧 = RGB565（retro-go 的原生格式 ⇒ 推送路径不用做 CPU 转换，内存带宽减半）
 *   面板侧 = RGB888 / colmod 0x77（**跟厂商基线一致**）
 * 为什么面板侧不能是 16bpp：真机实测（2026-10-11）按 16bpp 配置时症状是
 *   「颜色错 + 画面被斜切成漏斗 + 整体发暗」—— 面板按 24bpp 解读 16bpp 流，
 *   每行像素数与时序都对不上，瞳孔里看到的就是这种斜切。
 *   ⇒ 面板侧回到厂商的 24bpp；**转换交给 DSI 桥的硬件色彩转换**（out_color_format），
 *     我们自己不写 565→888 的循环（那是每个像素的 CPU 税，P4 上白交）。
 * 若颜色变成 R/B 互换，改 dev_cfg.rgb_ele_order（一个开关，不用动别处）。 */
#define CLAW4_PANEL_BPP         24        /* → vendor 驱动据此写 colmod 0x77 */
#define CLAW4_FB_BPP            24        /* 帧缓冲也 888：与面板一致，转换自己做（见下） */
/* RGB888 帧缓冲的字节序：1 = B,G,R（IDF RGB888 帧缓冲 / LVGL RGB888 的内存布局，厂商同款）。
 * 真机若出现红蓝互换，改这里为 0 即可（只影响推送路径的 3 字节顺序）。 */
#define CLAW4_RGB888_BGR        1

/* 背光 PWM */
#define CLAW4_LEDC_TIMER        LEDC_TIMER_0
#define CLAW4_LEDC_CHANNEL      LEDC_CHANNEL_0
#define CLAW4_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define CLAW4_LEDC_DUTY_RES     LEDC_TIMER_10_BIT   /* 0..1023 */
#define CLAW4_LEDC_FREQ_HZ      5000

/* I2C（TCA9555：SD 卡槽供电 P0-3 必须拉低才使能；厂商 README） */
#define CLAW4_I2C_PORT          1
#define CLAW4_PIN_I2C_SDA       GPIO_NUM_7
#define CLAW4_PIN_I2C_SCL       GPIO_NUM_8
#define CLAW4_TCA9555_ADDR      0x20
#define CLAW4_TCA_OUT_PORT0     0x02        /* 输出寄存器 P0 */
#define CLAW4_TCA_CFG_PORT0     0x06        /* 方向寄存器 P0（0 = 输出） */

/* 面板显示开/关（DCS 0x29/0x28）。厂商板级代码里这行是**注释掉的**（= 不调），
 * 所以默认也跟随：置 1 试不通就说明该保持 0。 */
#define CLAW4_CALL_DISP_ON_OFF  0

/* ══════════════ 临时诊断（"界面不出来"排查；结论落地后删除）══════════════
 * 目的：一眼区分 —— ① launcher 的界面帧根本没进驱动  ② 进了但被跳过。
 * 两个视觉标记（都画在**保留带**里，不会被界面覆盖）：
 *   · 洋红方块（左下角 12x12）= 任何一次 lcd_send_buffer 调用
 *   · 青色方块（右上角 12x12）= **整宽**推送（宽 720 的行块 = 界面帧的特征）
 * 另：开机 3 秒后把整屏行校验和清零，逼 launcher 下一帧重推整屏
 *   （若界面因此出现 ⇒ 推送链路是通的，只是开机那几帧被跳过了）。 */
#define CLAW4_DIAG_MARKERS      1
#define CLAW4_DIAG_REPUSH_MS    3000

/* 面板亮度 / CABC（真机 2026-10-11 反馈"屏幕有点暗"的**真根因**，与我们的 PWM 无关）：
 * 厂商 DCS 序列里写着 `0x51 = 0x83`（≈51% 亮度）且 `0x55 = 0xD2`（开 CABC 内容自适应调光）——
 * 于是"软件 80% × 面板 51%"≈ 41%，暗是必然。
 * init 之后我们改回：0x51 = 0xFF（拉满）、0x55 = 0x00（关 CABC，暗场景不再被进一步压暗）。
 * 想用软件调光就改 CLAW4_DCS_BRIGHTNESS（0x00..0xFF）；设置里的背光档仍然只动 PWM。 */
#define CLAW4_DCS_BRIGHTNESS_FIX 0        /* 0 = 不动（**厂商原值**，官方 demo 就是这个值）
                                           * 1 = init 后改 0x51/0x55（见下） */
#define CLAW4_DCS_BRIGHTNESS    0xFF      /* 仅在 FIX=1 时使用 */
#define CLAW4_DCS_CABC_OFF      0x00

static esp_lcd_panel_handle_t claw4_panel = NULL;
static esp_lcd_panel_io_handle_t claw4_io = NULL;   /* DBI io：init 之后还要发亮度 DCS */
/* DPI 帧缓冲（PSRAM，720×720 **RGB888**，行跨距 = 720*3 字节，无填充）。
 * 为什么 fb 也必须是 888：面板的 DSI 接收器按 24bpp 配置（厂商基线 colmod 0x77），
 * 而 IDF 的 dpi_panel_draw_bitmap **不做格式转换**（按 fb 的 bpp 直接 memcpy），
 * 所以"fb 565 + 流 888"只剩 DSI 桥的硬件转换一条路 —— 实测那条路是**蓝屏（欠载）**。
 * ⇒ fb 与流都 888，565→888 由本文件在推送路径里自己展开。 */
static uint8_t *claw4_fb = NULL;
static int claw4_fb_bytes = 0;
/* 收集逻辑行用；长度必须编译期 ⇒ 按 geom.h 的 LCD_BUFFER_LENGTH_MAX 分配（720×32）。
 * 64 字节对齐：msync 只按 128B 对齐切分，但源缓冲对齐能让 memcpy 走向量路径。 */
static uint16_t claw4_line_buffer[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(64)));
/* 叠加层合成暂存块（565 原生，**块内相对**索引）：rg_display 交来的是大端，
 * 共享叠加层模块只认原生 565 ⇒ 先还原进这里、画完按键再转 888。
 * 大小 = LCD_BUFFER_LENGTH_MAX（与 line_buffer 同量级，片内 RAM 合计约 92KB）。 */
static uint16_t claw4_ovl_scratch[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(64)));
static int claw4_win_left = 0, claw4_win_top = 0, claw4_win_width = 0;
static bool claw4_backlight_ready = false;

/* 565 → 面板 888 的一像素落盘。内存序 B,G,R 与 IDF/LVGL RGB888 一致（开关见 CLAW4_RGB888_BGR）。
 * **主写路径与电量灯路径共用这一份** —— 两处各写一遍必然漂移（红蓝互换只会在其中一处出现）。 */
static inline void claw4_put_pixel888(uint8_t *d, uint16_t v)
{
    const uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
    const uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
    const uint8_t b5 = (uint8_t)(v & 0x1F);
    const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
#if CLAW4_RGB888_BGR
    d[0] = b8; d[1] = g8; d[2] = r8;
#else
    d[0] = r8; d[1] = g8; d[2] = b8;
#endif
}

static inline uint16_t claw4_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* ──────────────────────────── 背光（LEDC PWM）──────────────────────────── */
static void claw4_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = CLAW4_LEDC_MODE,
        .duty_resolution = CLAW4_LEDC_DUTY_RES,
        .timer_num = CLAW4_LEDC_TIMER,
        .freq_hz = CLAW4_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK)
    {
        RG_LOGW("claw4: ledc timer config failed, backlight unavailable\n");
        return;
    }
    const ledc_channel_config_t ch = {
        .gpio_num = CLAW4_PIN_BACKLIGHT,
        .speed_mode = CLAW4_LEDC_MODE,
        .channel = CLAW4_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = CLAW4_LEDC_TIMER,
        .duty = 0,          /* ⚠ 先灭：任何 set 之前都不能亮，否则上电白闪 */
        .hpoint = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK)
    {
        RG_LOGW("claw4: ledc channel config failed\n");
        return;
    }
    claw4_backlight_ready = true;
}

/* ──────────────────────────── SD 卡槽供电（TCA9555 P0-3）──────────────────── */
/* 为什么放在显示驱动里做：厂商板级初始化顺序就是"I2C → 扩展器 → 屏"，而且
 * 卡槽供电不依赖显示，早做一步让 rg_storage 挂载时卡已经上电。
 * ⚠ **失败只告警**：本轮是点屏验证，SD 出问题不该把屏一起拖黑。 */
/* I2C 总线句柄（进程内唯一）：TCA9555 / 电量计 / GT911 触摸都挂在同一条总线上。
 * ⚠ 官方 demo 的板级注释：GT911 与 TCA9555 若各建各的 I2C 外设会**抢总线** ⇒ 通信失败。
 * 所以总线只在这里建一次，触摸初始化（rg_input.c）通过 claw4_get_i2c_bus() 复用。 */
static i2c_master_bus_handle_t claw4_i2c_bus = NULL;

i2c_master_bus_handle_t claw4_get_i2c_bus(void)
{
    if (claw4_i2c_bus)
        return claw4_i2c_bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CLAW4_I2C_PORT,
        .sda_io_num = CLAW4_PIN_I2C_SDA,
        .scl_io_num = CLAW4_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &claw4_i2c_bus) != ESP_OK)
        claw4_i2c_bus = NULL;
    return claw4_i2c_bus;
}

static void claw4_sd_power_enable(void)
{
    i2c_master_bus_handle_t bus = claw4_get_i2c_bus();
    if (!bus)
    {
        RG_LOGW("claw4: i2c bus init failed (SD 可能不可用；不影响显示)\n");
        return;
    }
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CLAW4_TCA9555_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK || !dev)
    {
        RG_LOGW("claw4: TCA9555 not found (SD 供电未知)\n");
        return;
    }
    /* 方向寄存器：P0 全输出（0）；输出寄存器：bit3 = 0（拉低使能 SD），其余保持高 */
    const uint8_t cfg_out[3] = {CLAW4_TCA_CFG_PORT0, 0x00, 0x00};
    const uint8_t out_val[3] = {CLAW4_TCA_OUT_PORT0, 0xF7, 0xFF};   /* bit3=0 ⇒ SD 使能 */
    esp_err_t e1 = i2c_master_transmit(dev, cfg_out, sizeof(cfg_out), 100);
    esp_err_t e2 = i2c_master_transmit(dev, out_val, sizeof(out_val), 100);
    RG_LOGI("claw4: TCA9555 SD power enable: cfg=0x%x out=0x%x\n", (int)e1, (int)e2);
}

/* ──────────────────────────── 面板初始化 ───────────────────────────────── */
/* 面板亮度修正：见 CLAW4_DCS_BRIGHTNESS 上方注释（厂商序列把亮度设成 51% 且开了 CABC）。 */
static void claw4_panel_brightness_fix(void)
{
#if !CLAW4_DCS_BRIGHTNESS_FIX
    return;                       /* 默认跟官方一致：面板亮度寄存器不动（0x51=0x83, 0x55=0xD2） */
#else
    if (!claw4_io)
        return;
    const uint8_t b = CLAW4_DCS_BRIGHTNESS;
    const uint8_t c = CLAW4_DCS_CABC_OFF;
    esp_err_t e1 = esp_lcd_panel_io_tx_param(claw4_io, 0x51, &b, 1);   /* Write Display Brightness */
    esp_err_t e2 = esp_lcd_panel_io_tx_param(claw4_io, 0x55, &c, 1);   /* Write CABC Control */
    RG_LOGI("claw4: panel brightness 0x51=0x%02X (was 0x83), CABC 0x55=0x%02X (was 0xD2) — tx err b=0x%x c=0x%x\n",
            b, c, (int)e1, (int)e2);
#endif
}

#if CLAW4_DIAG_MARKERS
/* 视觉探针：把 12x12 的方块画进**面板保留带**（界面与游戏都写不到那里）。
 * BGR 三字节序（CLAW4_RGB888_BGR=1）：洋红 = B=FF,G=00,R=FF；青 = B=FF,G=FF,R=00。 */
static uint32_t claw4_diag_calls, claw4_diag_fullw, claw4_diag_rows;
static int claw4_diag_last_x, claw4_diag_last_y, claw4_diag_last_w, claw4_diag_last_h;
static bool claw4_diag_after_boot;   /* 3 秒后的推送（= 界面帧是否还在来） */
static uint32_t claw4_diag_fullw_boot;   /* 3 秒前的整宽推送次数 */
static int claw4_diag_dump_blocks;       /* >0 = 还要逐块统计内容（强制重推时打开） */
static const bool claw4_diag_tag_pixels = false;  /* 画角标记（污染画面，只用字符照片验证时关掉） */
static uint32_t claw4_diag_widths[8];    /* 见过的块宽直方图（诊断用） */

static void claw4_diag_markers(bool full_width_push)
{
    if (!claw4_fb)
        return;
    claw4_diag_calls++;
    if (full_width_push)
    {
        claw4_diag_fullw++;
        if (!claw4_diag_after_boot)
            claw4_diag_fullw_boot++;
    }
    const int w = 12;
    for (int yy = 0; yy < w; ++yy)
    {
        for (int xx = 0; xx < w; ++xx)
        {
            uint8_t *m = claw4_fb + ((size_t)(CLAW4_PHYS_H - w + yy) * CLAW4_PHYS_W + xx) * 3;   /* 左下角 */
            m[0] = 0xFF; m[1] = 0x00; m[2] = 0xFF;                                               /* 洋红 */
            if (full_width_push)
            {
                uint8_t *c = claw4_fb + ((size_t)yy * CLAW4_PHYS_W + (CLAW4_PHYS_W - w + xx)) * 3; /* 右上角 */
                /* 时间编码：开机 3 秒内的整宽推送 = 蓝（B=FF,G=00,R=00）；
                 * 3 秒之后 = 青（B=FF,G=FF,R=00）—— 只有"青"才能说明界面帧**现在还在来**。 */
                c[0] = 0xFF; c[1] = claw4_diag_after_boot ? 0xFF : 0x00; c[2] = 0x00;
            }
        }
    }
    /* 两条带都要写回（128B 对齐：整行 2160B 不是 128 的整数倍，按整行范围向外扩） */
    esp_cache_msync((void *)((uintptr_t)claw4_fb), (size_t)w * CLAW4_PHYS_W * 3 + 128,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync((void *)((uintptr_t)claw4_fb + (size_t)(CLAW4_PHYS_H - w) * CLAW4_PHYS_W * 3),
                    (size_t)w * CLAW4_PHYS_W * 3 + 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

/* 3 秒后作废整屏行校验和：逼 launcher 下一帧重推整屏（只清数组，不跨任务重画界面）。 */
void rg_display_invalidate_lines(int top, int count);   /* 定义在 rg_display.c 后半（同 TU） */
static void claw4_diag_repush(void *arg)
{
    (void)arg;
    claw4_diag_after_boot = true;                 /* 之后的整宽推送会画成青色 */
    claw4_diag_dump_blocks = 20;                  /* 把接下来 20 块的内容统计出来 */
    rg_display_invalidate_lines(0, CLAW4_PHYS_H);
    RG_LOGI("claw4-diag: 3s 到 —— 行校验和已清零，且之后的整宽推送改画青色\n");
}

/* 把帧缓冲（= 面板此刻扫描的内容）打成 ASCII 字符画。
 * 用途：色块已证明"写进帧缓冲 = 面板显示"，所以这张字符画等价于给屏幕拍照，
 * 我不用问用户就能看清面板上到底是什么。
 * 每字符 = 8x12 像素，90 列 x 60 行覆盖 720x720。 */
static void claw4_diag_dump_fb(void)
{
    if (!claw4_fb)
        return;
    const int cw = 8, ch = 8;
    const int cols = CLAW4_PHYS_W / cw, rowsf = CLAW4_PHYS_H / ch;
    char lineb[160];
    RG_LOGI("claw4-diag-fb: 帧缓冲字符画（1 字符 = 8x12 px；' '=全黑 .=零星 :=少 +=中 #=多 @=很亮）\n");
    for (int cy = 0; cy < rowsf; ++cy)
    {
        int n = 0;
        if ((cy % 5) == 0 && cy > 0)
            RG_LOGI("fb|---- y=%d ----\n", cy * ch);
        for (int cx = 0; cx < cols; ++cx)
        {
            int cnt = 0;
            for (int y = 0; y < ch; ++y)
            {
                const uint8_t *p = claw4_fb + ((size_t)(cy * ch + y) * CLAW4_PHYS_W + cx * cw) * 3;
                for (int x = 0; x < cw; ++x, p += 3)
                    if (p[0] | p[1] | p[2]) cnt++;
            }
            const int pct = cnt * 100 / (cw * ch);
            lineb[n++] = cnt == 0 ? ' ' : pct < 5 ? '.' : pct < 25 ? ':' : pct < 50 ? '+' : pct < 75 ? '#' : '@';
        }
        lineb[n] = 0;
        RG_LOGI("fb|%s|\n", lineb);
    }
}

/* 每秒报一次：calls=总推送次数, fullw=整宽推送次数(其中 boot=3s前的), rows=累计行数, last=最后一块窗口 */
static void claw4_diag_report(void *arg)
{
    (void)arg;
    static int tick = 0;
    ++tick;
    if (tick == 6)                        /* 6 秒拍一张静态快照 */
        claw4_diag_dump_fb();
    RG_LOGI("claw4-diag: calls=%u fullw=%u boot_fullw=%u rows=%u after_boot=%d last=(%d,%d %dx%d)\n",
            (unsigned)claw4_diag_calls, (unsigned)claw4_diag_fullw, (unsigned)claw4_diag_fullw_boot,
            (unsigned)claw4_diag_rows, (int)claw4_diag_after_boot,
            claw4_diag_last_x, claw4_diag_last_y, claw4_diag_last_w, claw4_diag_last_h);
    RG_LOGI("claw4-diag-wh: 块宽直方图 = %u x%u, %u x%u, %u x%u, %u x%u\n",
            (unsigned)claw4_diag_widths[0], (unsigned)claw4_diag_widths[1],
            (unsigned)claw4_diag_widths[2], (unsigned)claw4_diag_widths[3],
            (unsigned)claw4_diag_widths[4], (unsigned)claw4_diag_widths[5],
            (unsigned)claw4_diag_widths[6], (unsigned)claw4_diag_widths[7]);
}
static esp_timer_handle_t claw4_diag_rep_timer = NULL;
static esp_timer_handle_t claw4_diag_timer = NULL;
#endif /* CLAW4_DIAG_MARKERS */

#ifndef RG_BATT_LED_ENABLE
/* 电量圆灯总开关（默认 1 = 原有行为）。claw4 在电量计（BQ27220）接上之前置 0：
 * 没数据会被当成 0% —— 既画错灯（红闪），又会把整条灯带擦成背景色 ⇒ 按键下方留一条黑带。 */
#define RG_BATT_LED_ENABLE 1
#endif

#if RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE
/* ── 电量圆灯（控制带里那颗）───────────────────────────────────────────────
 * 为什么必须由驱动直接画：灯在**控制带**（claw4 逻辑 y=692），而显示层只推视口（游戏区
 * y<480），叠加层与边框面板的推帧都刷不到它 —— 与 Tab5 的结论一致（见 rg_touch_overlay.h
 * 的 rg_batt_led_* 说明）。
 * 与 Tab5 的差别：Tab5 帧缓冲是 565，直接往 fb 上画；claw4 帧缓冲是 888 ⇒
 *   ① 灯画进一块 565 影子缓冲。高度按**绝对行号**开：rg_batt_led_draw() 内部用 (ly,lx)
 *      直接寻址，所以宁可多占 PSRAM，也不做"指针前移 by0 行"那种越界技巧（可读性优先）。
 *   ② 只把条带那几行 565→888 转进帧缓冲（复用主路径的 claw4_put_pixel888，防两处口径漂移）。 */
static uint16_t *claw4_led_shadow = NULL;
static esp_timer_handle_t claw4_led_timer = NULL;
static int64_t claw4_led_last_write = 0;
static uint8_t *claw4_led_last_fb = NULL;

/* 本块是否压到灯条带（灯在左下角、x 很窄，只判 y 足够） */
static bool claw4_led_overlaps(int top, int rows)
{
    int x0, y0, x1, y1;
    rg_batt_led_get_band(&x0, &y0, &x1, &y1);
    return (top < y1) && (top + rows > y0);
}

static void claw4_batt_led_refresh(bool force)
{
    if (!claw4_fb)
        return;
    const int64_t now = esp_timer_get_time();
    /* 换了帧缓冲（进出 app 会重挂显示）或超 5s 强制重画：否则"静止的灯"可能一直不刷新 */
    const bool stale = (claw4_fb != claw4_led_last_fb) || (now - claw4_led_last_write > 5000000);
    if (!force && !stale && !rg_batt_led_refresh_needed())
        return;
    claw4_led_last_fb = claw4_fb;
    claw4_led_last_write = now;

    int bx0, by0, bx1, by1;
    rg_batt_led_get_band(&bx0, &by0, &bx1, &by1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > CLAW4_PHYS_W) bx1 = CLAW4_PHYS_W;
    if (by1 > CLAW4_PHYS_H) by1 = CLAW4_PHYS_H;
    if (bx0 >= bx1 || by0 >= by1)
        return;

    if (!claw4_led_shadow)
    {
        claw4_led_shadow = (uint16_t *)heap_caps_malloc((size_t)CLAW4_PHYS_H * CLAW4_PHYS_W * 2,
                                                        MALLOC_CAP_SPIRAM);
        if (!claw4_led_shadow)
        {
            RG_LOGW("claw4: 电量灯影子缓冲申请失败（灯不显示，其余不受影响）\n");
            return;
        }
    }

    /* 条带先擦成**皮肤底色**（不是纯黑：非黑面板上会割裂画面，Tab5 真机反馈过的坑） */
    const uint16_t bg = rg_batt_led_band_bg();
    for (int y = by0; y < by1; ++y)
    {
        uint16_t *row = claw4_led_shadow + (size_t)y * CLAW4_PHYS_W;
        for (int x = bx0; x < bx1; ++x)
            row[x] = bg;
    }
    rg_batt_led_draw(claw4_led_shadow, CLAW4_PHYS_W);   /* 线性映射：逻辑 = 面板 */

    for (int y = by0; y < by1; ++y)
    {
        const uint16_t *s = claw4_led_shadow + (size_t)y * CLAW4_PHYS_W + bx0;
        uint8_t *d = claw4_fb + ((size_t)y * CLAW4_PHYS_W + bx0) * 3;
        for (int x = 0, n = bx1 - bx0; x < n; ++x, d += 3)
            claw4_put_pixel888(d, s[x]);
    }
    /* 定点写回（msync 要求 128 对齐 ⇒ 向外扩） */
    const size_t off = ((size_t)by0 * CLAW4_PHYS_W + bx0) * 3;
    const size_t end = ((size_t)(by1 - 1) * CLAW4_PHYS_W + bx1) * 3;
    const size_t a0 = off & ~(size_t)127;
    size_t a1 = (end + 127) & ~(size_t)127;
    if (a1 > (size_t)claw4_fb_bytes) a1 = (size_t)claw4_fb_bytes;
    if (a1 > a0)
        esp_cache_msync((void *)((uintptr_t)claw4_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void claw4_led_timer_cb(void *arg)
{
    (void)arg;
    claw4_batt_led_refresh(false);
}
#endif /* RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE */

static void lcd_init(void)
{
    claw4_backlight_init();
    lcd_set_backlight(0);            /* 明确先灭（init 期间不闪） */

    claw4_sd_power_enable();         /* 非致命 */

    /* DSI PHY 供电：P4 上 DSI 必须靠片上 LDO 供 PHY（chan3 @2500mV，厂商口径） */
    esp_ldo_channel_handle_t phy_ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = CLAW4_DSI_LDO_CHAN,
        .voltage_mv = CLAW4_DSI_LDO_MV,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo);
    if (err != ESP_OK)
        RG_LOGW("claw4: DSI PHY LDO acquire failed (0x%x)\n", (int)err);

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = NV3051F_PANEL_BUS_DSI_2CH_CONFIG();
    err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (err != ESP_OK)
    {
        RG_LOGE("claw4: esp_lcd_new_dsi_bus failed (0x%x)\n", (int)err);
        return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = NV3051F_PANEL_IO_DBI_CONFIG();
    err = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io);
    if (err != ESP_OK)
    {
        RG_LOGE("claw4: esp_lcd_new_panel_io_dbi failed (0x%x)\n", (int)err);
        return;
    }

    /* 全链路 RGB888 = 官方 MetalioClaw4 板级代码的**逐字段同款**（main/boards/metalio-claw-4/
     * metalio-claw-4.cc: pixel_format/in/out 全 RGB888、48MHz、use_dma2d、colmod 24bpp）。
     * ⚠ 别退回 fb=RGB565 + out=RGB888 那条"让 DSI 桥做硬件转换"的路：
     *   IDF 的 dpi_panel_draw_bitmap **不做格式转换**（按 fb 的 bpp 直接 memcpy），
     *   实测那样是**蓝屏（DSI 欠载）**。fb 565 + 流 565（colmod 0x55）则是"颜色错 + 漏斗"，
     *   因为面板的 DSI 接收器是按 24bpp 配的，16bpp 流会让它的行时序逐行漂移。 */
    esp_lcd_dpi_panel_config_t dpi_cfg = NV3051F_720_720_PANEL_60HZ_DPI_CONFIG_CF(LCD_COLOR_FMT_RGB888);
    dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB888;   /* 与 fb 一致 = 不做桥转换（厂商基线） */
    dpi_cfg.num_fbs = 1;             /* 只一张帧缓冲：我们自己往里写，不要厂商那套三缓冲 */
    dpi_cfg.flags.use_dma2d = true;
    dpi_cfg.dpi_clock_freq_mhz = CLAW4_DPI_CLK_MHZ;   /* 覆盖宏里的 36（见文件头说明） */

    nv3051f_vendor_config_t vendor_cfg = {
        .init_cmds = NULL,           /* NULL = 用厂商默认 DCS 序列 */
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = CLAW4_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = CLAW4_PANEL_BPP,   /* 24 ⇒ colmod 0x77，与 DSI 流的 out_color_format 一致 */
        .vendor_config = &vendor_cfg,
    };
    err = esp_lcd_new_panel_nv3051f(io, &dev_cfg, &claw4_panel);
    if (err != ESP_OK || !claw4_panel)
    {
        RG_LOGE("claw4: esp_lcd_new_panel_nv3051f failed (0x%x)\n", (int)err);
        return;
    }
    claw4_io = io;
    esp_lcd_panel_reset(claw4_panel);
    esp_lcd_panel_init(claw4_panel);
    claw4_panel_brightness_fix();
#if CLAW4_CALL_DISP_ON_OFF
    esp_lcd_panel_disp_on_off(claw4_panel, true);
#endif

    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(claw4_panel, 1, &fb) == ESP_OK && fb)
    {
        claw4_fb = (uint8_t *)fb;
        claw4_fb_bytes = CLAW4_PHYS_W * CLAW4_PHYS_H * (CLAW4_FB_BPP / 8);
        memset(claw4_fb, 0, claw4_fb_bytes);          /* 先黑屏，避免看到上电垃圾数据 */
        esp_cache_msync(claw4_fb, claw4_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    else
    {
        RG_LOGE("claw4: frame buffer unavailable\n");
    }

    claw4_win_left = claw4_win_top = 0;
    claw4_win_width = CLAW4_PHYS_W;

    RG_SCREEN_INIT();                /* 约定钩子（本 target 为空宏；漏掉=显示驱动永不执行） */

#if RG_OVERLAY_ENABLED
    /* 虚拟按键预渲染（一次性，约 100ms）：标签/边框/透明度形状在这一步烘成掩码，
     * 之后每帧只做查表混合。必须等显示起来后建（建层要读键位表/存 NVS）。 */
    rg_overlay_init();
#endif

    lcd_set_backlight(80);

#if RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE
    /* 电量灯刷新（4Hz）：样子没变时 rg_batt_led_refresh_needed() 直接返回 false，几乎零代价。 */
    if (!claw4_led_timer)
    {
        const esp_timer_create_args_t largs = {.callback = claw4_led_timer_cb, .name = "claw4-led"};
        if (esp_timer_create(&largs, &claw4_led_timer) == ESP_OK)
            esp_timer_start_periodic(claw4_led_timer, 250000);
    }
#endif

#if CLAW4_DIAG_MARKERS
    if (!claw4_diag_timer)
    {
        const esp_timer_create_args_t targs = {.callback = claw4_diag_repush, .name = "claw4-diag"};
        if (esp_timer_create(&targs, &claw4_diag_timer) == ESP_OK)
            esp_timer_start_once(claw4_diag_timer, (uint64_t)CLAW4_DIAG_REPUSH_MS * 1000);
    }
    if (!claw4_diag_rep_timer)
    {
        const esp_timer_create_args_t rargs = {.callback = claw4_diag_report, .name = "claw4-diag-rep"};
        if (esp_timer_create(&rargs, &claw4_diag_rep_timer) == ESP_OK)
            esp_timer_start_periodic(claw4_diag_rep_timer, 1000000);
    }
#endif
    RG_LOGI("claw4: DSI ready — NV3051F %dx%d, %d lanes @%dMbps, DPI %dMHz, RGB888, fb=%p\n",
            CLAW4_PHYS_W, CLAW4_PHYS_H, CLAW4_DSI_LANES, CLAW4_DSI_LANE_MBPS,
            CLAW4_DPI_CLK_MHZ, (void *)claw4_fb);
}

static void lcd_deinit(void)
{
    /* 常驻：本 target 不做 deinit（与 tab5 竖屏版一致）。 */
}

static void lcd_sync(void)
{
    /* 无待发送队列：lcd_send_buffer 是同步写帧缓冲 + msync，这里无事可做。
     * （面板扫描在后台按 DPI 时序持续读帧缓冲，写完即见。） */
}

static void lcd_set_rotation(int rotation)
{
    if (rotation)
        RG_LOGW("claw4: rotation %d ignored (方屏线性 1:1，不支持旋转)\n", rotation);
}

static void lcd_set_backlight(float percent)
{
    if (!claw4_backlight_ready)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    const uint32_t max_duty = (1u << CLAW4_LEDC_DUTY_RES) - 1u;
    const uint32_t duty = (uint32_t)(max_duty * (percent / 100.0f));
    ledc_set_duty(CLAW4_LEDC_MODE, CLAW4_LEDC_CHANNEL, duty);
    ledc_update_duty(CLAW4_LEDC_MODE, CLAW4_LEDC_CHANNEL);
    RG_LOGI("backlight set to %d%%\n", (int)percent);
}

static void lcd_set_window(int left, int top, int width, int height)
{
    (void)height;
    claw4_win_left = left;
    claw4_win_top = top;
    claw4_win_width = width;
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    (void)length;
    return claw4_line_buffer;
}


/* 把一块**逻辑行**写进面板帧缓冲：逻辑 (lx,ly) = 物理 (lx,ly) ⇒ 直接按行拷贝。
 * length 单位是像素（= 宽度 × 行数），与 rg_display.c 的约定一致；length=0 = 空操作。 */
static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (!claw4_fb || !buffer || !length || claw4_win_width <= 0)
        return;

    const int rows = (int)(length / (size_t)claw4_win_width);
    if (rows <= 0)
        return;
#if CLAW4_DIAG_MARKERS
    claw4_diag_last_x = claw4_win_left; claw4_diag_last_y = claw4_win_top;
    claw4_diag_last_w = claw4_win_width; claw4_diag_last_h = rows;
    claw4_diag_rows += (uint32_t)rows;
    {
        int slot = -1;
        for (int i = 0; i < 8; ++i)
            if (claw4_diag_widths[i * 2] == (uint32_t)claw4_win_width) { slot = i; break; }
            else if (claw4_diag_widths[i * 2] == 0) { slot = i; break; }
        if (slot >= 0) { claw4_diag_widths[slot * 2] = (uint32_t)claw4_win_width; claw4_diag_widths[slot * 2 + 1]++; }
    }
    if (claw4_diag_dump_blocks > 0)
    {
        /* 统计本块的内容：非黑像素数 + 非黑区域包围盒（源是 565 大端，黑 = 0x0000） */
        uint32_t nonblack = 0;
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
        for (int r = 0; r < rows; ++r)
        {
            const uint16_t *row = buffer + (size_t)r * claw4_win_width;
            for (int x = 0; x < claw4_win_width; ++x)
            {
                if (row[x] != 0)
                {
                    nonblack++;
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (r < by0) by0 = r;
                    if (r > by1) by1 = r;
                }
            }
        }
        claw4_diag_dump_blocks--;
        RG_LOGI("claw4-diag-blk: y=%d w=%d rows=%d nonblack=%u bbox=(%d,%d)-(%d,%d)\n",
                claw4_win_top, claw4_win_width, rows, (unsigned)nonblack, bx0, by0, bx1, by1);
    }
#endif
    if (claw4_win_left < 0 || claw4_win_left + claw4_win_width > CLAW4_PHYS_W ||
        claw4_win_top < 0 || claw4_win_top + rows > CLAW4_PHYS_H)
    {
        RG_LOGW("claw4: drop block out of panel: x=%d y=%d w=%d rows=%d\n",
                claw4_win_left, claw4_win_top, claw4_win_width, rows);
        return;
    }

    /* 源是 565 大端（rg_display 已交换）→ 展开成 BGR888 写进面板帧缓冲。
     * 字节序 B,G,R：与 IDF RGB888 帧缓冲/LVGL RGB888 的内存布局一致（厂商同款）。
     * 若真机出现红蓝互换 ⇒ 把 CLAW4_RGB888_BGR 改 0（一处开关，别改别处）。
     * 顺序写整行（目标是 PSRAM，随机访问慢），最后整体 msync。 */
    /* ── 虚拟按键叠加层：必须在**推给面板前**合成 ──────────────────────────────
     * 为什么放在显示层：GUI 是立即模式，按键画在 GUI 层会被下一次重绘盖掉（实机表现为闪烁）。
     * 与 Tab5 的差别：共享模块只认 565 原生，而 rg_display 交来的是 565 **大端**
     *   （Tab5 是 565 帧缓冲，driver 里换个字节序直接画；claw4 是 888）。
     * 做法：本块先还原成 565 原生进暂存块 → 叠加层画在暂存块上 → 再整体转 888。
     * 索引是**块内相对**的（与 Tab5 用 tab5_stage 的调用方式一致）。 */
    const uint16_t *src_block = buffer;
    bool src_native = false;
#if RG_OVERLAY_ENABLED
    if ((size_t)rows * (size_t)claw4_win_width <= (size_t)LCD_BUFFER_LENGTH_MAX)
    {
        for (int r = 0; r < rows; ++r)
        {
            const uint16_t *s = buffer + (size_t)r * claw4_win_width;
            uint16_t *d = claw4_ovl_scratch + (size_t)r * claw4_win_width;
            for (int x = 0; x < claw4_win_width; ++x)
                d[x] = claw4_swap16(s[x]);
        }
        rg_overlay_blit_linear(claw4_ovl_scratch, claw4_win_width, claw4_win_left, claw4_win_top,
                               claw4_win_width, rows, CLAW4_PHYS_W);
        src_block = claw4_ovl_scratch;
        src_native = true;
    }
#endif

    uint8_t *dst = claw4_fb + ((size_t)claw4_win_top * CLAW4_PHYS_W + claw4_win_left) * 3;
    for (int r = 0; r < rows; ++r)
    {
        const uint16_t *src_row = src_block + (size_t)r * claw4_win_width;
        uint8_t *dst_row = dst + (size_t)r * CLAW4_PHYS_W * 3;
        for (int x = 0; x < claw4_win_width; ++x)
            claw4_put_pixel888(dst_row + x * 3,
                               src_native ? src_row[x] : claw4_swap16(src_row[x]));
    }

    /* cache 写回：msync 要求偏移与长度都是 **128 字节的整数倍**，否则静默失败。
     * 窗口偏移不保证对齐（720px 行 = 1440B，1440%128=32）⇒ 向外扩到 128B 边界，
     * 多写几个字节无害（同一张帧缓冲、都是我们要显示的内容）。 */
    const size_t off = (size_t)((uintptr_t)dst - (uintptr_t)claw4_fb);
    const size_t px_bytes = CLAW4_FB_BPP / 8;
    const size_t stride = (size_t)CLAW4_PHYS_W * px_bytes;
    const size_t len = (size_t)claw4_win_width * px_bytes;
    size_t start = off & ~(size_t)127;
    size_t end = (off + (size_t)(rows - 1) * stride + len + 127) & ~(size_t)127;
    if (end > (size_t)claw4_fb_bytes)
        end = (size_t)claw4_fb_bytes;
    esp_cache_msync((void *)((uintptr_t)claw4_fb + start), end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

#if CLAW4_DIAG_MARKERS
    if (claw4_diag_tag_pixels)
        claw4_diag_markers(claw4_win_width >= CLAW4_PHYS_W);
#endif
#if RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE
    /* 本块压到电量灯条带上 ⇒ 立刻把灯补画回去（否则被面板底图擦掉，要等下一拍定时器）。
     * 只在相交时做，正常游戏帧（游戏区 y<480）不付这个代价。 */
    if (claw4_led_overlaps(claw4_win_top, rows))
        claw4_batt_led_refresh(true);
#endif

    /* ★★ 连续推送时窗口向下推进 ★★
     * rg_display.c 的流式路径只在"窗口不连续"时才调 lcd_set_window（见 rg_display.c:450
     * 的 `if (top != window_top)` 记账）—— 它**假定驱动内部有写游标**会在每次推送后下移。
     * 漏掉这一行的后果：整屏每一块都写回同一个 win_top（我实测 dump 出 15 块全是 y=0），
     * 最后一块覆盖前面的 ⇒ launcher 界面永远出不来（只有直写路径的沙漏能看见）。
     * 语义与 Tab5 驱动 mipi_dsi_tab5.h:1165 一字不差，别删。 */
    claw4_win_top += rows;
}
