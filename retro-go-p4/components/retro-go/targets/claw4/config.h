/* Configuration for Metalio Claw4 (ESP32-P4, 方屏 720×720)
 *
 * 面板：NV3051F（720×720，MIPI-DSI 2 lanes，RGB565/colmod 0x55），驱动走 RG_SCREEN_DRIVER 5
 *   = drivers/display/mipi_dsi_claw4.h（**单方向、逻辑=物理、线性 1:1、无旋转**）。
 * 显示链路：自建 DSI+DPI（不依赖 M5 BSP），仅复用厂商面板初始化序列 esp_lcd_nv3051f.c。
 * 构建：python3 rg_tool.py --target claw4 --no-networking build-img launcher gbsp
 * 阶段：P1 = 点屏（触摸手柄 RG_ENABLE_TOUCH_GAMEPAD 0；键位表已冻结在 touch_layout.h，P2 再接）。
 */
/****************************************************************************
 * Target definition                                                        *
 ****************************************************************************/
#define RG_TARGET_NAME             "Metalio Claw4"

/****************************************************************************
 * 方屏专属开关（都只在 claw4 生效）
 ****************************************************************************/
/* 首次启动的"屏幕方向"弹窗与设置项：方屏只有一个方向 ⇒ 关掉（用户 2026-10-11："去掉这个"） */
#define RG_ORIENT_SWITCH            0
/* 背光默认拉满：NV3051F 的 DCS 亮度上限低于常见屏，默认 80% 在真机上偏暗 */
#define RG_DISPLAY_DEFAULT_BACKLIGHT 100
/* Claw4 默认中文（面向国内用户；RG_LANG_ZH=2）。新机器首刷即中文；
 * 老机器由下面 FORCE 一次性迁移（claw4 暂无语言设置 UI，用户改不了）。 */
#define RG_GUI_DEFAULT_LANGUAGE     RG_LANG_ZH
/* claw4 没有语言设置 UI ⇒ 强制迁移存量 NVS 一次到中文（rg_gui.c 消费，见 LangForced）。 */
#define RG_GUI_FORCE_LANGUAGE       RG_LANG_ZH
/* 一级菜单（关于）瘦身：去 Website、屏蔽 Debug menu、语言提到最后（用户拍板 2026-10）。 */
#define RG_GUI_HIDE_WEBSITE         1
#define RG_GUI_HIDE_DEBUG_MENU      1
#define RG_GUI_LANG_IN_ABOUT_MENU   1


/****************************************************************************
 * Storage (SD 卡, SDMMC slot0 4-bit + 片上 LDO chan4)
 * ---------------------------------------------------------------------------
 * 引脚取自厂商板级 config.h（CloudZao/MetalioClaw4）。⚠ 那份文件自己写着
 * "Verify these GPIO assignments against the actual metalio-claw-4 schematic" ——
 * 即厂商也没从原理图上核对过，**SD 是 P2 的验证项**（读不到卡先怀疑这里）。
 * SD 卡槽供电受 IO 扩展器 TCA9555 控制（厂商 README：P0-3 必须拉低才使能），
 * 那段初始化在 lcd_init 里做（见 mipi_dsi_claw4.h），失败只告警不致命。
 ****************************************************************************/
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_VENDOR_RECIPE 1
#if RG_STORAGE_SDMMC_VENDOR_RECIPE
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_0
#define RG_STORAGE_SDMMC_ONCHIP_LDO 1
#define RG_STORAGE_SDMMC_WIDTH      4
#else
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_ONCHIP_LDO 0
#define RG_STORAGE_SDMMC_WIDTH      1
#endif
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_HIGHSPEED
#define RG_STORAGE_SDMMC_LDO_CHAN   4
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
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable（老 i2s.c 不支持 MCLK，用不了）
#define RG_AUDIO_USE_TAB5_CODEC     0   // ES8388 via M5 BSP: MCLK=30 BCLK=27 WS=29 DOUT=26
/* Claw4 音频：I2S→板载蓝牙音频编解码芯片（UART2 AT 控制），参数抄官方 MetalioClaw4。
 * 芯片是 I2S 从机模式的**主机**（它给 BCLK/WS）且采样率固定 16kHz ⇒ 必须强制对齐，
 * 否则核心按 GBA 原生 32768Hz 喂数据会慢放。实现见 drivers/audio/claw4_bt_audio.c。 */
#define RG_AUDIO_USE_CLAW4_BT       1
/* RG_AUDIO_FORCE_SAMPLE_RATE 已停用：实测 gbsp 核心按自己的原生率(32768)喂数据、
 * 不读 rg_audio_get_sample_rate() ⇒ 强制系统采样率没有作用，采样率换算必须在 sink 内做。
 * （宏本身与 rg_audio.c 里的守卫保留，供将来确认核心会读取时再启用。） */


/****************************************************************************
 * Video — MIPI DSI / ST7123, 走官方 Tab5 BSP                                *
 ****************************************************************************/
/* 【屏幕方向开关】0 = 竖屏（默认，= 现状，行为零变化） 1 = 横屏
 * ---------------------------------------------------------------------------
 * 横屏形态 = 老线（M5Burner 0.2/0.3）那套：逻辑画布 1280×720、驱动做 90° 映射，
 * 游戏窗仍是 **720×480 居中**（x[280,1000) y[120,600)），四周留白全给虚拟按键。
 *
 * 两种方向的差别**只有四处**：逻辑画布尺寸 / 驱动映射方向 / 键位表 / 视口上限；
 * 核心、缩放、滤镜、存档、封面完全共用。详见 docs/ORIENTATION-SWITCH-EVAL.md。
 *
 * 构建横屏镜像：RG_TAB5_ORIENTATION=1 tools/build-tab5-skin.sh
 * （⚠ 构建脚本每次都会显式传 0/1 —— CMake 缓存变量是"粘"的，不显式传会串味。） */

/* 【PPA 传输模式实验开关】2026-10-10，横屏驱动（mipi_dsi_tab5.h）专用。
 * 目的：回答"按块 PPA 慢 25 倍"里有多少是 blocking 的等待/提交成本、多少是硬件本体
 * （方案与判据见 docs/SPEC-PPA-NONBLOCK-EXPERIMENT.md）。
 *   0 = PPA 关（**默认，= 现状**，CPU 转置；发行/日常行为零变化）
 *   1 = 按块 PPA **非阻塞**（事件回调 semaphore + 1 深流水线；叠加层推迟到收块之后合成）
 *   2 = 按块 PPA **BLOCKING**（= 当年 25x 那个形态，用作"同配置对照"，把等待成本单独拆出来）
 *   3 = 按块 PPA **BLOCKING + 叠加层合成到源缓冲**（P0 首版，2026-10-10）
 *       ⛔ **实测失败，不要使用**：显示路径本身达标（稳态 0.91ms/块、叠加层混合只剩 44µs），
 *          但**系统级崩塌** —— 240s 里模拟器只推进约 6 秒的量（对照档 ~225 行心跳、本档 6 行），
 *          无 panic / 无 PPA 报错 / 无重启。已排除 UART 刷屏（SPAM=0 复现）、对齐、存储落点。
 *          未解释的规律："每块不再碰帧缓冲"与崩塌强相关（当年 D 组同样掉到 ~2fps）。
 *          失败注记与待验证假设见 `drivers/display/mipi_dsi_tab5.h` 里 `RG_TAB5_PPA_MODE == 3` 段。
 *       —— 2 与 3 只差"叠加层在哪合成"这一个变量：
 *          2 = 写面板帧缓冲（裁剪矩形=整屏 ⇒ 每块都混合全部 13 个按键，≈30.4ms/块，实测真身）
 *          3 = 写本块的**暂存源缓冲**（原点=本块，只混合本块内的按键；随旋转被 PPA 转过去）
 *       配套：mode 3 的暂存块换序成小端 ⇒ PPA 用 .byte_swap=false（由 src_le 自动推导）。
 * ⚠ 只影响横屏驱动。竖屏驱动（mipi_dsi_tab5_p.h）的 PPA 路径不读这个开关。 */

/* 运行时几何表：**必须在这里 include** —— 下面这些宏（RG_SCREEN_WIDTH 等）展开成
 * rg_geom()->…，而宏是在**调用方 TU** 展开的，所以每个用到它们的翻译单元都要先看到声明。
 * （config.h 本身被所有 TU 经 rg_system.h 间接包含 ⇒ 挂在这里最省事。） */
#include "geom.h"

/* ════════════ 屏幕几何：**运行时按方向取**（0.4.9 单 app）════════════
 * 原来这里是一对 `#if RG_TAB5_ORIENTATION == 1 … #else … #endif`：一次编译只装一个方向。
 * 单 app 形态要一个镜像同时支持横竖 ⇒ 几何改成运行时值（两行表在 targets/tab5/geom.h）。
 *
 * 手法（关键）：下面把宏**定义成"读运行时几何的表达式"**（如 RG_SCREEN_WIDTH = rg_geom()->w），
 * 于是所有既有调用点**一字不改**就变成运行时取值；只有少数"必须是编译期常量"的地方
 * （数组长度 / 静态初始化）需要单独处理 —— 编译器会把它们全部报出来，不靠人找。
 * 值本身没变：横屏那套值原样搬进 geom.h 的 [1] 行、竖屏搬进 [0] 行。
 * 双 app 形态下 rg_orient_active() 返回运行分区标签 ⇒ 取到的值 = 改造前的编译期宏（零回归）。 */

/* —— 两方向**相同**的部分，保持普通宏 —— */
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_PARTIAL_UPDATES   1
/* 无 SPI 命令序列：面板初始化在驱动里走 Tab5 BSP（lcd_init 会调用本宏） */
#define RG_SCREEN_INIT()
/* 游戏缩放两方向都用 ZOOM（是否锁死在设置里由 rg_geom()->lock_scaling 决定；
 * 倍数上限两方向不同，见下）。 */
#define RG_DISPLAY_DEFAULT_SCALING     RG_DISPLAY_SCALING_ZOOM

/* —— 按方向变化的 9 项：值在 targets/tab5/geom.h —— */
/* ⚠ RG_SCREEN_DRIVER **必须保持编译期常量** —— 它被 rg_display.c 用 `#if` 选"包含哪份驱动头"。
 * 双 app：2 = 横 / 3 = 竖（与改造前逐字相同）；单 app：4 = "两份都编、运行时由分发层选"。
 * 运行时想知道"当前生效的是哪份后端"，用 rg_geom()->driver（= 2 或 3）。 */
#define RG_SCREEN_DRIVER               5   /* claw4 单方向方屏：drivers/display/mipi_dsi_claw4.h */
#define RG_SCREEN_WIDTH                (rg_geom()->w)
#define RG_SCREEN_HEIGHT               (rg_geom()->h)
#define RG_SCREEN_VISIBLE_AREA         {rg_geom()->vis.l, rg_geom()->vis.t, rg_geom()->vis.r, rg_geom()->vis.b}
#define RG_SCREEN_VISIBLE_AREA_NO_SHLD {rg_geom()->vis_ns.l, rg_geom()->vis_ns.t, rg_geom()->vis_ns.r, rg_geom()->vis_ns.b}
#define RG_DISPLAY_DEFAULT_CUSTOM_ZOOM (rg_geom()->def_custom_zoom)
#define RG_DISPLAY_MAX_CUSTOM_ZOOM     (rg_geom()->max_custom_zoom)
#define RG_DISPLAY_MAX_WINDOW_HEIGHT   (rg_geom()->max_window_h)
#define RG_DISPLAY_LOCK_SCALING        (rg_geom()->lock_scaling)


/****************************************************************************
 * Input — Tab5 只有 Reset/Boot，没有用户按键；用触摸屏当虚拟手柄            *
 *                                                                          *
 * 坐标是**逻辑空间 720x1280 竖屏**（= 用户实际看到的方向），命中区 = 中心+尺寸 *
 *                                                                          *
 * 硬约束：命中区**不得压到游戏画面**。按 3x 整数缩放（240x160 -> 720x480）   *
 * 计算，游戏区占满宽度、位于**顶部** y[0,480)，控制区是下方 y[480,1280) ——    *
 * 800px 高，比横屏版两侧各 280px 的 margin 宽裕得多。                        *
 * 本表所有命中区都在 y>=500，与游戏区之间留有大片空白。                      *
 *                                                                          *
 * ⚠ 因此游戏缩放模式必须是 ZOOM(custom_zoom=3)：FULL 会把画面拉满屏，        *
 *   按键就会压到画面上。（横屏版的"上下零 margin"问题在竖屏下不存在。）      *
 * （launcher 菜单是全屏 UI，那时按键压到菜单是正常的、也是必需的。）          *
 ****************************************************************************/
/* P2 触摸虚拟手柄总开关：P1 显示验证阶段曾置 0（避免触摸初始化问题干扰显示判定）；
 * P2 打开 —— 触摸初始化见 rg_input.c 的 claw4 分支（GT911 @0x5D，INT GPIO33）。 */
#define RG_ENABLE_TOUCH_GAMEPAD 1
/* 测量用：合成 A 键（2s 一次）。用户定义的负载路径：launcher 里按 A 进游戏 ⇒ 游戏内 A=跳。
 * ★ 测量构建专用；测完必须回 0（"测量开关不许过夜"，见 CLAW4-P3-FPS.md）。默认 0。 */
#define RG_TEST_AUTOPRESS_A_MS 0 
#define RG_TEST_AUTOPRESS_MENU2_AT_MS 0 
#define RG_TEST_AUTOPRESS_MENU_AT_MS 0 


/* ── 开机 recovery 组合键：**关掉**（0 = 永不触发）─────────────────────────
 * components/retro-go/config.h:110 的默认值是 **RG_KEY_ANY**（任意一个键）：开机早期那 5 次
 * 采样里只要有一个键被判成"按住"，就直接进 recovery 对话框；而那个对话框的 default 分支是
 * rg_system_exit() ⇒ 重启 ⇒ 又进 recovery ⇒ **点一下就重启一轮的死循环**（真机 2026-10-11 反馈：
 * "recovery mode 不断重启"）。
 * 本机没有"上电时按住某物理键"这种常规入口（BOOT 键 + 触摸虚拟键），而**触摸在开机时可能上报
 * 幽灵点** —— 用 ANY 等于把开机稳定性押在触摸驱动上。recovery 改从设置菜单进。
 * ⚠ 别照 Tab5/其它 target 抄 RG_KEY_MENU：那和 BOOT 键同源，仍会被幽灵输入撞上。 */
#define RG_RECOVERY_BTN 0

/* ── 物理按键：BOOT 键（GPIO35）当系统键（菜单/退出）。按键区见 touch_layout.h 的 MENU。
 * 低有效（按下接地）。
 * ⚠ 2026-10-11 真机教训：**必须打开内部上拉**。第一版照"开发板 BOOT 键一般有外部上拉"
 *   的想当然写成 pullup=0（= GPIO_FLOATING），实测引脚悬空读 0 ⇒ MENU 被判成"一直按住"
 *   ⇒ launcher 收到 MENU 就 exit → switch_app → 重启 → 再 exit，**无限重启循环**
 *   （日志：rg_system_exit / rg_system_switch_app / Shutdown: keys still held）。
 *   悬空读 0 这一实测事实本身就否掉了"有外部上拉"⇒ 打开内部上拉即可。
 *   教训：引脚外部有没有上拉/下拉，属于**必须实测**的板级事实，不能照开发板惯例推。 */
#define RG_GAMEPAD_GPIO_MAP { \
    {RG_KEY_MENU, .num = GPIO_NUM_35, .pullup = 1, .level = 0}, \
}

/* ── 振动马达（GPIO22，高有效）：虚拟键按下时给一次短脉冲（实现在 rg_input.c，
 * 只在定义了这个宏的 target 上编译）。30ms 是"点得到"的下限，再短感觉得不到。 */
#define RG_VIBRATE_GPIO   GPIO_NUM_22
#define RG_VIBRATE_MS     30

/* ── 电量圆灯：**暂时关闭**（驱动里的宏，默认 1 = Tab5 原行为）────────────────
 * 官方板子读电量走 BQ27220 电量计（bq27220_gauge.cc），本 target 还没接 ⇒ BATT 恒为 0，
 * "0%" 被当成真的没电 ⇒ 画红闪灯 + **把整条灯带擦成背景色** ⇒ 真机表现为按键下方一条
 * 黑色遮挡（2026-10-11 反馈）。接上电量计后把这里改成 1 即可。 */
#define RG_BATT_LED_ENABLE 0

/* ── 虚拟手柄皮肤：用 E 套「素面板」──────────────────────────────────────────
 * 用户 2026-10-11 真机反馈：Tab5 那套面板装饰（四个分区框 + 机型铭牌）压进本机 240px 高的
 * 控制带后，读成一堆割裂的细线，"皮肤影响了观感"（也和一贯的"否决常驻条纹"一致）。
 * E 套 = 底色 + 按键，面板上零装饰；按键配色/形状与 D 套逐字段相同 ⇒ 只改观感。
 * 想换别的风格不用改代码：设备菜单 →「虚拟手柄」→ 皮肤。 */
#define RG_TOUCH_SKIN_DEFAULT 4

/* ── 无人值守测量：启动器扫 ROM 目录直接进 gbsp（2026-10-11 P3 帧率压榨）───────
 * 为什么需要：真机上没人点屏，而"选 app → 选 ROM"要两次输入；每轮刷机后要能自己复现负载。
 * 与 RG_GBA_DIAG 分开：后者连带脚本按键/诊断刷屏，会换掉测量负载。交付前改回 0。 */
#ifndef RG_TEST_BOOTROM
#define RG_TEST_BOOTROM 0
#endif

/* 虚拟按键可视层：把命中区画到屏幕上（0 = 完全不编，回到"盲区"模式）。
 * 实现在 rg_touch_overlay.c（目标无关的共享模块），由显示驱动在"推给面板前"合成
 * ——放在显示层是为了不被 GUI 立即模式的重绘覆盖（实机表现为按键闪烁，27b44b4 修过）。
 * 标签/透明度/按下反馈都在那个模块里；运行时开关与透明度档位存 NVS（NS_GLOBAL），
 * 入口在设置菜单 →「虚拟手柄」。 */
#define RG_TOUCH_OVERLAY 1

/* 每帧"算一块推一块"的块大小（像素）。默认 RG_SCREEN_WIDTH*4=4 行 => 一帧 180 次推送，
 * 播游戏时面板 DMA 会持续忙、触发 tab5_draw 的重试风暴。这里放到 16 行（一帧 45 次）。
 * 内存代价：驱动里两个 LCD_BUFFER_LENGTH 大小的缓冲 = 2 × 46,080B（720×32×2B ≈ 90KB），
 * 再加上 48KB 的片内 SRAM 暂存（驱动里的 tab5_stage），合计约 138KB 静态片内 SRAM。 */
/* 2026-09-28：16→32 行/块。真机实测每 16 行推一次 → 一帧 30 次推送，
 * DMA2D 大量丢弃(ESP_ERR_INVALID_STATE)、重试烧掉 224ms/秒。
 * 32 行一块 = 46KB，正好装进 48KB 的片内 SRAM 暂存(tab5_stage)，推送次数减半。
 * ⚠ 横屏不能用 32：块大小 = 宽×行数×2B，1280×32×2B = 80KB **超过 48KB 暂存**；
 *   横屏取 16 行 = 40KB（老线就是这个值）。 */
/* 每帧"算一块推一块"的块大小（像素）= 宽 × 行数，行数按方向取（竖 32 / 横 16；值在 geom.h）。
 * 为什么横屏不能 32：块 = 宽×行数×2B，1280×32×2B = 80KB **超过 48KB 片内暂存**（竖屏 720×32×2B
 * = 46KB 才正好装下）；横屏取 16 行 = 40KB。历史上 16 行块导致重试风暴（DMA2D 丢包、224ms/秒），
 * 所以竖屏保持 32、横屏保持 16 —— 与改造前逐字相同。
 * ⚠ 驱动里两个缓冲是**静态数组**（长度必须编译期已知）⇒ 按两方向最大值分配
 *   （LCD_BUFFER_LENGTH_MAX，见 geom.h），实际使用长度由这里按方向给。 */
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * rg_geom()->lcd_rows)

/* GUI 字体放大倍数：默认字体 VeraBold11 渲染高度 13px，在 1280x720 上太小（实机反馈）。
 * 3 倍 = 39px。改这里即可调整（2 = 26px，3 = 39px）。 */
#define RG_GUI_FONT_SCALE 3

#if RG_ENABLE_TOUCH_GAMEPAD
/* 键位表抽到 touch_layout.h —— 命中判定 / 可视层绘制 / PC 预览三方共用一份，
 * 抄成两份必然漂移（表现：画的和点的不是一回事）。改键位改那个文件。 */
#include "touch_layout.h"
/* 触摸坐标换算：改成**运行时按方向分支**（0.4.9 单 app 一个镜像要装两个方向）。
 * 两个分支的算式分别与原实现逐字相同：
 *   横屏 = rg_input.c 的默认 90° 逆映射（lx=py, ly=phys_w-1-px，与驱动写入方向同源，
 *          老线 0.2/0.3 真机验证过）；
 *   竖屏 = 恒等映射（显示驱动就是线性的）—— 不同向的话手指会被换算到旋转后的逻辑点、
 *          落在命中区之外，表现就是「按键画出来了但按不动」。
 * 调用点（rg_input.c 的 4 处）一字不改。 */
/* 坐标换算：方屏逻辑 = 物理、线性 1:1（显示驱动就是线性的）⇒ 恒等映射。
 * ⚠ 别照抄 Tab5 的"按方向分支" —— 本机没有第二方向，写成分支只会多一个可能错的路径。 */
#define RG_TOUCH_LOGICAL_FROM_PHYS(px, py, lx, ly)                                  \
    do {                                                                            \
        (lx) = (px);                                                                \
        (ly) = (py);                                                                \
    } while (0)

/* 键位表（两张，几何在 touch_layout.h，已冻结）：
 *   GBA 版 = L 占 Y 位 / R 占 X 位、无贴角键（GBA/GB/GBC/NES 用）
 *   SNES 版 = 贴角 L/R + 菱形 X/Y/A/B（SNES 用）
 * ⚠ 选表要**按当前机型**（app->configNs）在运行时选，不是按方向 —— 接法见 P2 待办。
 *   在接上之前：主表 = SNES 版（键最全，当兜底），备表 = GBA 版。 */
#define RG_GAMEPAD_TOUCH_MAP     RG_CLAW4_TOUCH_MAP_SNES
#define RG_GAMEPAD_TOUCH_MAP_ALT RG_CLAW4_TOUCH_MAP_GBA
/* 两张表**条数不等**（SNES 13 / GBA 11）—— 它们是两种机型布局，不是两个方向；
 * 共享代码里那条"两张表条数必须一致"的静态断言对本机不成立，显式放行。 */
#define RG_TOUCH_KEYMAP_ALLOW_DIFFERENT_COUNTS 1
/* 选表判据改成**按当前机型**（方屏只有一个方向）：实现在 rg_input.c 的
 * rg_claw4_use_gba_layout()（判据与叠加层铭牌同源 = app->configNs）。 */
#define RG_TOUCH_KEYMAP_USE_ALT() rg_claw4_use_gba_layout()
/* 物理面板尺寸（方屏两个方向相同） */
#define RG_TOUCH_PHYS_W 720
#define RG_TOUCH_PHYS_H 720
#endif /* RG_ENABLE_TOUCH_GAMEPAD */


/****************************************************************************
 * Miscellaneous                                                            *
 ****************************************************************************/
/* 没有 RG_RECOVERY_BTN：Tab5 无用户按键，进不了 recovery（P2 再决定用触摸长按替代） */
#define RG_CUSTOM_PLATFORM_INIT() \
    /* Arbitrary code executed very early during retro-go init */

// See components/retro-go/config.h for more things you can define here!

/****************************************************************************
 * 电池电量读取（2026-09-29 新增）
 * ---------------------------------------------------------------------------
 * 此前本 target 完全没有电量配置 → rg_input_read_battery_raw() 直接 return
 * false → 状态栏 BATT 恒为 0。
 * 硬件参数取自 gywan94/tab5-vgbanext 的 odroid_input.c：
 *   GPIO 53 = ADC2_CH4，分压 68K(电池侧) + 100K(GND侧) → 比例 1.68
 * 百分比按锂电 3.3V(0%) ~ 4.2V(100%) 线性换算（够用；偏了再校准）。
 ***************************************************************************/
#ifndef RG_CLAW4_BATTERY_CONFIGURED
#define RG_CLAW4_BATTERY_CONFIGURED
/* Tab5 没有可用的电池 ADC 脚（GPIO53 在官方 BSP 里是 I2C SDA），
 * 电量走 INA226 电源监测芯片：I2C 0x41，寄存器 0x02 = Bus Voltage（1.25mV/LSB）。
 * 它挂在 BSP 主 I2C 总线上（SDA=31/SCL=32），读法见 rg_input.c 的 RG_BATTERY_DRIVER==3。 */
#define RG_BATTERY_DRIVER           4
/* 【换算口径】INA226 监测的是 **2S 锂电包**（NP-F550 7.4V, 6.0~8.4V），不是单节，
 * 所以 raw（电包 mV）不能直接套单节的 3300~4200。
 * 官方口径（M5Unified Power_Class::getBatteryLevel，杜撰不得）：
 *     mv  = busVoltage(V) * 500        // = 电包 mV / 2 = 单节平均 mV
 *     lvl = (mv - 3300) * 100 / (4150 - 3350)
 * ⚠ 注意上面官方式子里分母 3350 与分子 3300 并不自洽（原文如此）。
 * 我们采用**端点明确、单调**的映射：单节 3300mV = 0%、4150mV = 100%，即
 *     lvl = (mv - 3300) / (4150 - 3300) * 100 = (mv - 3300) / 850 * 100
 * 分母 850 与官方写法的 800 不同，但端点一致，且满电（4150mV）恰好 100% 不会溢出。
 * 要重新标定就改这两个常数（3300 = 0% 点，850 = 量程）。
 * 结果在 rg_input.c 里还会被 clamp 到 0..100（低电压/无电池时会出现负值）。 */
#define RG_BATTERY_CALC_PERCENT(raw) ((float)(raw))   /* BQ27220 直接给 SOC% */
/* 显示用电包电压（7.4V 那种，与 M5Unified getBatteryVoltage 一致），不折半 */
/* 诊断开关（2026-10-03）：=1 时把每次触点的 raw/logical 坐标与矢量判定结果打到串口。
 * 用于分辨"点没命中"与"命中了但出的键不对"。查完问题记得改回 0（会刷日志）。 */
/* 2026-10-06 验证完成：关闭触摸追踪日志。
 * 注意：rg_input.c 用 #ifdef 判定，设 0 无效，必须不定义。 */
#undef RG_TOUCH_TRACE

/* 诊断开关：记录传入 GBA 核心的按键掩码与 KEYINPUT 值；仅用于一次性真机捕获。 */
/* 2026-10-06 临时：显示层 A/B —— 关掉行级部分刷新（每帧全刷），用于判定
 * "静态菜单按键没反应"是否由行校验和过滤/丢块造成。验证后删除。 */
/* #define RG_DISPLAY_FULL_REFRESH_AB 1 */

/* 2026-10-06 临时：真机无人值守按键时间轴（时间基准 = 开机秒数，跨 launcher→gbsp 连续）。
 * 8.0s 在启动器里按 A 启动高亮 ROM；之后是游戏内的按键序列。验证完删除。 */
/* 2026-10-06 无人值守复现用：真机按键时间轴。验证完毕，默认关闭（要复现去掉注释）。 */
/* 2026-10-06 验证完成：关闭自动按键脚本（空字符串 = 关闭），恢复真机手动输入 */
#define RG_TEST_KEYS_DEVICE ""
#define RG_TEST_NO_AUTOSAVE 0   /* 恢复常规存档行为 */
/* 2026-10-10 PPA 非阻塞实验用过的静音钩子：**默认 0**（实验期曾置 1）。
 * 保留定义而非删除，是因为 rg_audio.c 里有 `#if RG_TEST_MUTE` 引用 —— 宏不存在会变成
 * 隐式 0，读代码的人分不清"故意的"还是"漏了的"。 */
#ifndef RG_TEST_MUTE
#define RG_TEST_MUTE 0
#endif
#define RG_TEST_FLUSH_CACHE 0

/* 2026-10-06 临时：真机"输入 vs 显示"分辨探针（DIAG_CURSOR/DIR/FB）。验证后删除。 */
/* 2026-10-06 无人值守测试用的诊断探针，验证完毕，默认关闭（要复现再打开）。 */
/* 2026-10-10 PPA 非阻塞实验：临时打开过（实验已结束，改回 0） */
#ifndef RG_GBA_DIAG
#define RG_GBA_DIAG 0   /* 验证完成：关闭诊断探针与脚本按键，出干净发行版。
                         * （2026-10-10 晚二轮 P0 测量期临时开过 1，测完已改回 0。） */
#endif

#ifndef RG_GBA_INPUT_TRACE
#define RG_GBA_INPUT_TRACE 0
#endif

/* 2026-10-06 临时测试钩子：强制"启动器带了存档位"那条路（真机上没人点启动器）。
 * 与启动器语义等价：加 RG_BOOT_RESUME + 指定 slot。验证完必须改回 0。 */
#ifndef RG_TEST_BOOT_RESUME
#define RG_TEST_BOOT_RESUME 0
#endif
#ifndef RG_TEST_BOOT_RESUME_SLOT
#define RG_TEST_BOOT_RESUME_SLOT 0
#endif
/* 2026-10-06 临时 A/B：=1 时关掉"第二块画面缓冲 + 轮换"，量改前的横条率。验证完删除。 */
#ifndef RG_TEST_NO_SURFACE_ROTATION
#define RG_TEST_NO_SURFACE_ROTATION 0
#endif
/* 2026-10-06 临时：诊断探针里的"每帧刷屏"部分（ANCH / DIAG_CURSOR / DIAG_DIR / DIAG_FB）。
 * 量横条时必须关掉：每帧几十行 UART 会把核心拖慢、污染显示侧测量。
 * 默认 1（行为不变），横条 A/B 时设 0 ⇒ 只留每秒一行的 DIAG_TEAR。 */
#ifndef RG_GBA_DIAG_SPAM
#define RG_GBA_DIAG_SPAM 1   /* 默认 1（行为不变）。2026-10-10 P0 测量期临时设 0：每帧几十行 UART
                              * 会拖慢核心、污染显示侧测量；两轮测量都已跑完，已改回 1。
                              * （附：SPAM=1 下 E2 那轮曾出现"f=15 后日志断流"；SPAM=0 复测
                              *   **仍卡死** ⇒ 卡死不是 UART 造成的。） */
#endif

#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 0.001f)/* 充电判定阈值（mA，取分流电流绝对值）：小于这个充电电流不算"在充电"，
 * 免得不插充电器时被噪声抖成闪烁。官方口径：分流电流**为负**表示在充电。 */
#define RG_CLAW4_CHARGE_CURRENT_MA  40   /* 小于这个充电电流不算"在充电"（mA） */
#endif
