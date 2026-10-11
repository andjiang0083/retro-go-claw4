#pragma once
/* Metalio Claw4 方屏几何 —— **恒定值，没有方向分支**。
 *
 * 与 Tab5 的 targets/tab5/geom.h 的差别（照抄前先读这段）：
 *   · Tab5 那份是"一个 app 装横竖两个方向 ⇒ 几何必须运行时取（rg_geom_rows[2] + rg_orient_active()）"；
 *   · Claw4 只有一个方向（720×720 方屏，逻辑 = 物理、**线性 1:1、无旋转**）⇒ 直接给常量，
 *     rg_geom() 返回同一份。少一层运行时状态 = 少一类"几何与驱动错配"的 bug。
 *
 * 布局（用户 2026-10-11 定稿；键位表与门禁见 targets/claw4/touch_layout.h）：
 *   游戏区 240×160 @3x = **720×480 贴顶**（y[0,480)），下方 y[480,720) 240px 是虚拟手柄控制带。
 *   ⇒ vis = {0,0,0,240}：底部 240px 不给画面用（Tab5 竖屏是 800px，本机只有 240px，
 *     所以键位尺寸从 84/±85 压到 72/±72、系统键面积砍半 —— 见 touch_layout.h）。
 *   GB/GBC 的 160×144 @3x = 480×432 落在同一 480 带内水平居中，控制带不变。
 */
typedef struct
{
    int l, t, r, b;
} rg_geom_rect_t;

typedef struct
{
    int             driver;          /* 5 = claw4 单方向方屏驱动（drivers/display/mipi_dsi_claw4.h） */
    int             w, h;            /* 逻辑画面尺寸 = 720x720 */
    rg_geom_rect_t  vis;             /* RG_SCREEN_VISIBLE_AREA（含肩键行机型） */
    rg_geom_rect_t  vis_ns;          /* RG_SCREEN_VISIBLE_AREA_NO_SHLD（无肩键行机型）—— 方屏两者相同 */
    int             lcd_rows;        /* 每块行数：32 ⇒ LCD_BUFFER_LENGTH = 720*32 = 23040 像素（46KB） */
    int             max_window_h;    /* RG_DISPLAY_MAX_WINDOW_HEIGHT = 480（= 游戏带高 ⇒ 最大 3x） */
    int             lock_scaling;    /* 0 = 允许在设置里改缩放 */
    float           def_custom_zoom; /* 默认 ZOOM = 3x（240→720 正好满宽、160→480 正好满带） */
    float           max_custom_zoom; /* 上限同 3x：再大就被 max_window_h 挡回去 */
} rg_geom_t;

/* 只有一行 —— 方屏无第二方向。字段顺序与 targets/tab5/geom.h 一致，便于两边对照。 */
static const rg_geom_t rg_geom_fixed = {
    /*driver*/ 5,
    /*w, h*/   720, 720,
    /*vis*/    {0, 0, 0, 240},
    /*vis_ns*/ {0, 0, 0, 240},
    /*lcd_rows*/ 32,
    /*max_window_h*/ 480,
    /*lock_scaling*/ 0,
    /*def_custom_zoom, max_custom_zoom*/ 3.0f, 3.0f,
};

#define RG_GEOM_MAX_W 720
#define RG_GEOM_MAX_H 720
/* 驱动里两个 LCD 缓冲是静态数组（长度必须编译期已知）⇒ 按 720*32 分配。 */
#define LCD_BUFFER_LENGTH_MAX (720 * 32)

static inline const rg_geom_t *rg_geom(void)
{
    return &rg_geom_fixed;
}
