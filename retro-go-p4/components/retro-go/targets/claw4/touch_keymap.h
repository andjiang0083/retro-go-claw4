#pragma once
/* ============================================================================
 * Metalio Claw4 · 触摸键位表的**选表实现**（板级文件，P3 从 rg_input.c 迁出）
 * ----------------------------------------------------------------------------
 * 为什么单独一个文件：这段逻辑 100% 是 claw4 的事（三分支表：SNES 主表 / GBA 备表 /
 * GB-GBC 专用表；判据是**当前核心机型** app->configNs，与方屏"只有一个方向"有关）。
 * 它以前写在共享文件 rg_input.c 的 `#if defined(RG_TARGET_CLAW4)` 里 ⇒ 给 claw4 改键位
 * 要动共享文件、还得证明 Tab5 二进制零位移。现在共享层只留一个**能力开关**：
 *   rg_input.c:  `#ifdef RG_TOUCH_KEYMAP_BOARD_HOOK` → `#include <本文件>`
 * 所以本文件由 rg_input.c **在同一个 TU 里 include**（静态符号、可内联，性能与之前一致）。
 * 改 claw4 键位/选表判据 ⇒ 只改这里与 targets/claw4/touch_layout.h。
 *
 * 契约（共享层在 include 之前必须已经提供）：
 *   keymap_touch_main[] / keymap_touch_alt[] / keymap_touch[] / keymap_touch_count
 *   RG_TOUCH_KEYMAP_USE_ALT()      —— 默认判据（按方向），claw4 只在"非 GB/GBC"时用
 *   RG_GAMEPAD_TOUCH_MAP_GB        —— claw4 的第三张表（config.h 定义）
 * ==========================================================================*/

#ifndef RG_TOUCH_KEYMAP_TABLE_EXTRA
#error "claw4 的三表选表需要 RG_TOUCH_KEYMAP_TABLE_EXTRA（见 targets/claw4/config.h）"
#endif

/* ⚠ 第三张表（GB/GBC 的 v2 AB）**声明在共享文件里**（rg_input.c 的 `keymap_touch_gb[]`，
 * 用通用能力宏 RG_TOUCH_KEYMAP_TABLE_EXTRA 赋值），位置在 keymap_touch[] 之前 ——
 * 那是为了让**数据布局与迁移前逐字节一致**（段名不变则对齐填充也不变）。
 * 这里只用它。 */

/* 选表判据（与叠加层铭牌/配色同源 = app->configNs）：
 *   优先 GB/GBC（第三表）→ 非 snes 的机型走备表（GBA/AB 机版）→ snes 走主表。
 * 主表 = SNES 版（13 键，贴角 L/R + 菱形 X/Y）；备表 = GBA 版（11 键，L 占 Y 位、R 占 X 位，
 * 用户 2026-10-11 定稿：GBA 没有 X/Y 但有 L/R，拇指自然位就是 L/R）。 */
static bool rg_claw4_use_gba_layout(void)
{
    rg_app_t *app = rg_system_get_app();
    const char *ns = (app && app->configNs) ? app->configNs : "";
    return strcmp(ns, "snes") != 0;
}

/* GB/GBC → v2 新 AB 表（9 键）。判据与铭牌同源：configNs == "gb" || "gbc"。 */
static bool rg_claw4_use_gb_layout(void)
{
    rg_app_t *app = rg_system_get_app();
    const char *ns = (app && app->configNs) ? app->configNs : "";
    return strcmp(ns, "gb") == 0 || strcmp(ns, "gbc") == 0;
}

/* 键位表**代际号**：每换一次表 +1。可视层（rg_touch_overlay.c）靠它判断
 * "画出来的按键要不要重建"——它那套按键精灵只在 init 建一次，而这里是每帧重算，
 * 两者错开的症状就是**画的是旧表、点的是新表**（2026-10-16 真机：GBC 里 A/B 还在老位置）。 */
static uint32_t s_touch_keymap_gen;
uint32_t rg_input_touch_keymap_gen(void) { return s_touch_keymap_gen; }

/* 选表 = **每次重算 + 变了就地换**（2026-10-16 真机实测修：GB 里仍是老键位）。
 * 为什么不能"装过就锁死"：本函数首次调用发生在 rg_input_init()（"Virtual touch gamepad
 * registered" 那段），那一刻核心的 app->configNs **还没设成 "gb"**（核心是 rg_system_init
 * 之后才赋值）⇒ 锁死 = 把 GBA 表永久带进 GB/GBC 游戏（用户反馈"GB 还是老的键位"）。
 * 正确姿势：每帧重算该用哪张，与已装那张比，变了就地 memcpy 换表；命中判定与可视层都每帧
 * 经 rg_input_get_touch_keymap() 取数 ⇒ 运行中换表安全。
 * 十字键几何在三张表里逐字节相同（中心 160,600 / 臂距 72）⇒ 换表无需重算 dpad_geom。
 * 返回"这次是否真的换了表"，调用点据此同步"设备可产键"掩码。 */
static bool touch_keymap_ensure(void)
{
    static int installed = -1;   /* -1=未装 0=主表 1=备表 2=GB/GBC */
    const bool use_gb  = rg_claw4_use_gb_layout();
    const bool use_alt = !use_gb && RG_TOUCH_KEYMAP_USE_ALT();
    const int want = use_gb ? 2 : (use_alt ? 1 : 0);
    if (want == installed)
        return false;
    const rg_keymap_touch_t *src = use_gb ? keymap_touch_gb : (use_alt ? keymap_touch_alt : keymap_touch_main);
    const size_t cnt = use_gb ? RG_COUNT(keymap_touch_gb) : (use_alt ? RG_COUNT(keymap_touch_alt) : RG_COUNT(keymap_touch_main));
    memcpy(keymap_touch, src, cnt * sizeof(rg_keymap_touch_t));
    keymap_touch_count = cnt;
    installed = want;
    s_touch_keymap_gen++;   /* 通知可视层：精灵与命中已不同代，必须重建 */
    const rg_app_t *app = rg_system_get_app();
    RG_LOGI("重选表 ⇒ %s (ns=%s, %u 键)\n",
            use_gb ? "GB/GBC 版 AB v2 对角" : (use_alt ? "GBA/AB 机版" : "SNES 版"),
            (app && app->configNs) ? app->configNs : "", (unsigned)cnt);
    return true;
}
