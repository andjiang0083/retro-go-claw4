#pragma once
/* ============================================================================
 * 板级显示驱动**契约**（port 层）—— 把"只写在注释里的契约"变成编译期可检查
 * ----------------------------------------------------------------------------
 * 为什么不是函数指针表（vtable）：显示驱动都在 drivers/display/ 下，由 rg_display.c
 * 用 `#include` 拉进**同一个 TU**，函数是 `static` / `static inline`，热路径（每帧几十次
 * block 推送）靠内联。改成 vtable = 间接调用 + 丢内联 ⇒ 显示通路性能回退。
 * 所以这里的做法是：**同一套原型声明**，谁实现谁就把它们定义出来 —— 少一个函数、
 * 或参数类型不一致，**编译期就报错**（不再是靠人读注释/runtime 才知道对不上）。
 *
 * 谁要实现（四份，几何/后端各不同，接口相同）：
 *   drivers/display/mipi_dsi_tab5.h      Tab5 横屏后端（被 tab5_l_api.c 包一层）
 *   drivers/display/mipi_dsi_tab5_p.h    Tab5 竖屏后端（被 tab5_p_api.c 包一层）
 *   drivers/display/tab5_dispatch.h      Tab5 单 app：运行时选上面两份（转发器）
 *   drivers/display/mipi_dsi_claw4.h     Metalio Claw4 方屏（单方向、线性 1:1）
 *
 * ⚠ 本头文件**只能被"确实定义了这 8 个函数"的 TU 包含**（现在是上面四份各自包含）——
 *   它声明的是 `static` 函数，在没有定义的 TU 里包含会得到"声明了但未定义"的编译错误。
 *   这正是契约的一部分：想让别的目标也纳入契约，就先把那 8 个函数补齐。
 *
 * 上游遗留驱动（ili9341.h / sdl2.h / dummy.h）暂未纳入本契约（它们的符号集不同），
 * 归属见台账 P4；rg_display.c 的 `#if` 分发仍兼容它们的老编号。
 * ==========================================================================*/

#include <stddef.h>
#include <stdint.h>

/* ── 驱动代号：把 rg_display.c / 各 driver 里的"魔法数字"变成具名常量 ──────────
 * 取值沿用上游 `RG_SCREEN_DRIVER` 的编号（不改数值 ⇒ 行为零变化），
 * 新代码一律用名字，不再直接写 2/3/4/5。 */
#define RG_DISPLAY_DRIVER_ILI9341    0    /* SPI 小屏（上游，ILI9341/ST7789） */
#define RG_DISPLAY_DRIVER_TAB5_L     2    /* Tab5 横屏后端（90° 映射） */
#define RG_DISPLAY_DRIVER_TAB5_P     3    /* Tab5 竖屏后端（线性） */
#define RG_DISPLAY_DRIVER_TAB5_BOTH  4    /* Tab5 单 app：两份都编，运行时由分发层选 */
#define RG_DISPLAY_DRIVER_CLAW4      5    /* Metalio Claw4 方屏（单方向、线性 1:1） */
#define RG_DISPLAY_DRIVER_SDL2       99   /* 宿主（macOS/Linux 预览） */
#define RG_DISPLAY_DRIVER_DUMMY      100  /* 无显示（"以上都不是"） */

/* ── 契约本体：这 8 个符号必须由本 TU 的驱动实现 ─────────────────────────────
 * 参数类型逐字对齐（改名/改类型 ⇒ 编译期报错）。 */
static void      lcd_init(void);
static void      lcd_deinit(void);
static void      lcd_sync(void);
static void      lcd_set_rotation(int rotation);
static void      lcd_set_backlight(float percent);
static void      lcd_set_window(int left, int top, int width, int height);
static uint16_t *lcd_get_buffer(size_t length);
static void      lcd_send_buffer(uint16_t *buffer, size_t length);

/* ── 牙齿：逐个断言"符号存在 + 签名逐字一致" ─────────────────────────────────
 * 用 __typeof__ + __builtin_types_compatible_p 做**纯编译期**检查：
 *   缺符号（名字写错/漏实现）⇒ 未声明标识符报错；
 *   签名不符（参数类型/返回类型改了）⇒ 断言失败报错。
 * 为什么不用"取地址"（那样对未定义符号更狠）：取地址会把本来可被优化掉的函数**钉住**，
 * 让 .data/.text 长出字节 —— 本轮的硬判据是"改动前后 .obj 剥调试后逐字节相同"，
 * 取地址会破坏它，所以这里选零代码生成的写法。
 * 已知边界（诚实写清）：**声明了但从未定义、且没有任何调用点**的函数不会报错 ——
 * 这种函数本来就该删（当前 8 个都有调用点，见 rg_display.c）。 */
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_init), void (void)),
               "显示驱动契约：缺 lcd_init 或签名应为 void(void)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_deinit), void (void)),
               "显示驱动契约：缺 lcd_deinit 或签名应为 void(void)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_sync), void (void)),
               "显示驱动契约：缺 lcd_sync 或签名应为 void(void)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_set_rotation), void (int)),
               "显示驱动契约：缺 lcd_set_rotation 或签名应为 void(int)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_set_backlight), void (float)),
               "显示驱动契约：缺 lcd_set_backlight 或签名应为 void(float)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_set_window), void (int, int, int, int)),
               "显示驱动契约：缺 lcd_set_window 或签名应为 void(int,int,int,int)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_get_buffer), uint16_t *(size_t)),
               "显示驱动契约：缺 lcd_get_buffer 或签名应为 uint16_t *(size_t)");
_Static_assert(__builtin_types_compatible_p(__typeof__(lcd_send_buffer), void (uint16_t *, size_t)),
               "显示驱动契约：缺 lcd_send_buffer 或签名应为 void(uint16_t*,size_t)");
