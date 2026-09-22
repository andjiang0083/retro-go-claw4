/* Configuration for M5Stack Tab5 (ESP32-P4)
 *
 * 显示：官方 Tab5 BSP（vendor/m5stack_tab5）+ MIPI DSI/ST7123，走 RG_SCREEN_DRIVER 2。
 * 面板原生 720x1280 竖屏；逻辑画面 1280x720 横向，90° 映射在驱动里做（见驱动头部注释）。
 * 构建：python rg_tool.py --target tab5 build-img launcher gbsp --no-networking
 */

/****************************************************************************
 * Target definition                                                        *
 ****************************************************************************/
#define RG_TARGET_NAME             "M5Stack Tab5"


/****************************************************************************
 * Storage (SD 卡, SDMMC 4-bit) — 引脚与 Tab5 硬件一致                       *
 ****************************************************************************/
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_HIGHSPEED
#define RG_GPIO_SDMMC_CLK           GPIO_NUM_43
#define RG_GPIO_SDMMC_CMD           GPIO_NUM_44
#define RG_GPIO_SDMMC_D0            GPIO_NUM_39
#define RG_GPIO_SDMMC_D1            GPIO_NUM_40
#define RG_GPIO_SDMMC_D2            GPIO_NUM_41
#define RG_GPIO_SDMMC_D3            GPIO_NUM_42


/****************************************************************************
 * Audio — P1 全禁（ES8388 要走新 I2S API 适配，放到最后阶段）                *
 ****************************************************************************/
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable


/****************************************************************************
 * Video — MIPI DSI / ST7123, 走官方 Tab5 BSP                                *
 ****************************************************************************/
#define RG_SCREEN_DRIVER            2   // 2 = MIPI DSI (Tab5 BSP)，0=SPI, 99=SDL2
#define RG_SCREEN_BACKLIGHT         1
/* 逻辑分辨率（用户自然持机的横向视角）。物理面板是 720x1280 竖屏，
 * 驱动按 90° 映射写入，所以这里必须是横向的那一组数。 */
#define RG_SCREEN_WIDTH             1280
#define RG_SCREEN_HEIGHT            720
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}  // Left, Top, Right, Bottom
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}  // Left, Top, Right, Bottom
#define RG_SCREEN_PARTIAL_UPDATES   1
/* 无 SPI 命令序列：面板初始化在驱动里走 Tab5 BSP（lcd_init 会调用本宏） */
#define RG_SCREEN_INIT()


/****************************************************************************
 * Input — P1 无输入（Tab5 只有 Reset/Boot，没有用户按键；                   *
 *          触摸虚拟手柄在 P2 做，见 rg_input 的触摸驱动）                    *
 ****************************************************************************/


/****************************************************************************
 * Miscellaneous                                                            *
 ****************************************************************************/
/* 没有 RG_RECOVERY_BTN：Tab5 无用户按键，进不了 recovery（P2 再决定用触摸长按替代） */
#define RG_CUSTOM_PLATFORM_INIT() \
    /* Arbitrary code executed very early during retro-go init */

// See components/retro-go/config.h for more things you can define here!
