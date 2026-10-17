#pragma once
/* ============================================================================
 * Metalio Claw4 · 触摸 IC 初始化（板级文件，P3-b 从 rg_input.c 迁出）
 * ----------------------------------------------------------------------------
 * 触摸 IC = **GT911**（与厂商面板同款）；参数照官方板级源码
 * （official/main/boards/metalio-claw-4/metalio-claw-4.cc → InitializeTouch）：
 *   · I2C 地址先探测 0x5D，失败再试 0x14；不接 RST（GPIO_NUM_NC）；INT = GPIO33 低有效；
 *   · 200kHz（与电量计同总线；官方注释：400k 在总线抖动时更容易 clear-bus 失败）；
 *   · x_max/y_max = 720（方屏）。
 * ⚠ I2C 控制器**复用显示驱动建好的那条**（claw4_get_i2c_bus）—— 官方板级注释：
 *   两个 I2C 外设会抢总线，GT911 与 TCA9555 会一起通信失败。
 * 由 rg_input.c 以"能力"的名义 include（同一个 TU ⇒ 静态函数与内联性质不变）。
 * ==========================================================================*/

#include "esp_lcd_touch_gt911.h"      /* claw4 的触摸 IC = GT911（厂商面板同款） */
#include "driver/i2c_master.h"        /* 复用显示驱动建好的 I2C 总线（TCA9555 同一条） */

/* Metalio Claw4 的触摸 IC = **GT911**。参数照官方板级源码
 * （official/main/boards/metalio-claw-4/metalio-claw-4.cc → InitializeTouch）：
 *   · I2C 地址先探测 0x5D，失败再试 0x14；
 *   · 不接 RST（GPIO_NUM_NC）；INT = GPIO33，**低有效**；
 *   · 200kHz（与电量计 BQ27220 同总线；官方注释：400k 在总线抖动时更容易 clear-bus 失败）；
 *   · x_max/y_max = 720（方屏）。
 * ⚠ I2C 控制器**复用显示驱动建好的那条**（claw4_get_i2c_bus）—— 官方板级注释：
 *   两个 I2C 外设会抢总线，GT911 与 TCA9555 会一起通信失败。 */
extern i2c_master_bus_handle_t claw4_get_i2c_bus(void);
static esp_err_t rg_touch_init(void)
{
    i2c_master_bus_handle_t bus = claw4_get_i2c_bus();
    if (!bus)
        return ESP_ERR_INVALID_STATE;

    uint8_t dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(bus, dev_addr, 100) != ESP_OK)
    {
        if (i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK)
        {
            dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
            RG_LOGI("claw4: GT911 在备用地址 0x%02X\n", dev_addr);
        }
        else
        {
            RG_LOGW("claw4: GT911 无应答（0x%02X/0x%02X），触摸不可用\n",
                    ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
            return ESP_ERR_NOT_FOUND;
        }
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = dev_addr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {.disable_control_phase = 1},
    };
    io_config.scl_speed_hz = 200000;
    esp_err_t err = esp_lcd_new_panel_io_i2c_v2(bus, &io_config, &io);
    if (err != ESP_OK)
        return err;

    const esp_lcd_touch_config_t tp_config = {
        .x_max = RG_TOUCH_PHYS_W,
        .y_max = RG_TOUCH_PHYS_H,
        .rst_gpio_num = -1,          /* 板上未接 */
        .int_gpio_num = 33,
        .levels = { .reset = 0, .interrupt = 0 },   /* INT 低有效 */
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    err = esp_lcd_touch_new_i2c_gt911(io, &tp_config, &touch_handle);
    if (err != ESP_OK || touch_handle == NULL)
    {
        esp_lcd_panel_io_del(io);
        touch_handle = NULL;
        return err != ESP_OK ? err : ESP_FAIL;
    }
    RG_LOGI("claw4: GT911 就绪 @0x%02X (INT 33, 200kHz)\n", dev_addr);
    return ESP_OK;
}
