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
#include "esp_ldo_regulator.h"     /* DSI PHY 供电（chan3 @2500mV） */
#include "driver/ledc.h"           /* 背光 PWM */
#include "driver/i2c_master.h"     /* TCA9555（SD 卡槽供电使能） */
#include "esp_lcd_nv3051f.h"       /* 厂商面板驱动（组件根目录，第三方 vendor 代码） */

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
#define CLAW4_BPP               16        /* RGB565（vendor 驱动据此写 colmod 0x55） */

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

/* 面板亮度 / CABC（真机 2026-10-11 反馈"屏幕有点暗"的**真根因**，与我们的 PWM 无关）：
 * 厂商 DCS 序列里写着 `0x51 = 0x83`（≈51% 亮度）且 `0x55 = 0xD2`（开 CABC 内容自适应调光）——
 * 于是"软件 80% × 面板 51%"≈ 41%，暗是必然。
 * init 之后我们改回：0x51 = 0xFF（拉满）、0x55 = 0x00（关 CABC，暗场景不再被进一步压暗）。
 * 想用软件调光就改 CLAW4_DCS_BRIGHTNESS（0x00..0xFF）；设置里的背光档仍然只动 PWM。 */
#define CLAW4_DCS_BRIGHTNESS    0xFF
#define CLAW4_DCS_CABC_OFF      0x00

static esp_lcd_panel_handle_t claw4_panel = NULL;
static esp_lcd_panel_io_handle_t claw4_io = NULL;   /* DBI io：init 之后还要发亮度 DCS */
static uint16_t *claw4_fb = NULL;      /* DPI 帧缓冲（PSRAM，720×720 RGB565，行跨距 720px） */
static int claw4_fb_bytes = 0;
/* 收集逻辑行用；长度必须编译期 ⇒ 按 geom.h 的 LCD_BUFFER_LENGTH_MAX 分配（720×32）。
 * 64 字节对齐：msync 只按 128B 对齐切分，但源缓冲对齐能让 memcpy 走向量路径。 */
static uint16_t claw4_line_buffer[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(64)));
static int claw4_win_left = 0, claw4_win_top = 0, claw4_win_width = 0;
static bool claw4_backlight_ready = false;

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
static void claw4_sd_power_enable(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CLAW4_I2C_PORT,
        .sda_io_num = CLAW4_PIN_I2C_SDA,
        .scl_io_num = CLAW4_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK || !bus)
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
    if (!claw4_io)
        return;
    const uint8_t b = CLAW4_DCS_BRIGHTNESS;
    const uint8_t c = CLAW4_DCS_CABC_OFF;
    esp_err_t e1 = esp_lcd_panel_io_tx_param(claw4_io, 0x51, &b, 1);   /* Write Display Brightness */
    esp_err_t e2 = esp_lcd_panel_io_tx_param(claw4_io, 0x55, &c, 1);   /* Write CABC Control */
    RG_LOGI("claw4: panel brightness 0x51=0x%02X (was 0x83), CABC 0x55=0x%02X (was 0xD2) — tx err b=0x%x c=0x%x\n",
            b, c, (int)e1, (int)e2);
}

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

    /* RGB565：像素格式 + in/out 颜色格式都设 RGB565（IDF 5.5 用 _CF 变体给 in_color_format） */
    esp_lcd_dpi_panel_config_t dpi_cfg = NV3051F_720_720_PANEL_60HZ_DPI_CONFIG_CF(LCD_COLOR_FMT_RGB565);
    dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
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
        .bits_per_pixel = CLAW4_BPP,     /* 16 ⇒ colmod 0x55（与 dpi_cfg 的 RGB565 必须一致） */
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
        claw4_fb = (uint16_t *)fb;
        claw4_fb_bytes = CLAW4_PHYS_W * CLAW4_PHYS_H * 2;
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

    lcd_set_backlight(80);
    RG_LOGI("claw4: DSI ready — NV3051F %dx%d, %d lanes @%dMbps, DPI %dMHz, RGB565, fb=%p\n",
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
    if (claw4_win_left < 0 || claw4_win_left + claw4_win_width > CLAW4_PHYS_W ||
        claw4_win_top < 0 || claw4_win_top + rows > CLAW4_PHYS_H)
    {
        RG_LOGW("claw4: drop block out of panel: x=%d y=%d w=%d rows=%d\n",
                claw4_win_left, claw4_win_top, claw4_win_width, rows);
        return;
    }

    /* 源是 565 大端 → 面板要小端：逐像素换回（tab5 同约定）。
     * 写成"按行 memcpy + 就地交换"而不是逐像素写目标：目标是 PSRAM，随机访问慢，
     * 顺序写整行再整体 msync 才是快路径。 */
    uint16_t *dst = claw4_fb + (size_t)claw4_win_top * CLAW4_PHYS_W + claw4_win_left;
    for (int r = 0; r < rows; ++r)
    {
        uint16_t *src_row = buffer + (size_t)r * claw4_win_width;
        uint16_t *dst_row = dst + (size_t)r * CLAW4_PHYS_W;
        for (int x = 0; x < claw4_win_width; ++x)
            dst_row[x] = claw4_swap16(src_row[x]);
    }

    /* cache 写回：msync 要求偏移与长度都是 **128 字节的整数倍**，否则静默失败。
     * 窗口偏移不保证对齐（720px 行 = 1440B，1440%128=32）⇒ 向外扩到 128B 边界，
     * 多写几个字节无害（同一张帧缓冲、都是我们要显示的内容）。 */
    const size_t off = (size_t)((uintptr_t)dst - (uintptr_t)claw4_fb);
    const size_t len = (size_t)claw4_win_width * 2;
    size_t start = off & ~(size_t)127;
    size_t end = (off + (size_t)(rows - 1) * CLAW4_PHYS_W * 2 + len + 127) & ~(size_t)127;
    if (end > (size_t)claw4_fb_bytes)
        end = (size_t)claw4_fb_bytes;
    esp_cache_msync((void *)((uintptr_t)claw4_fb + start), end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}
