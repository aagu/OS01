# P3 GUI-3 Material 风格桌面设计规范

> 日期：2026-10-24
> 状态：设计定稿（待实施）
> 依赖：GUI-1 ✅（libgui 平台后端）、GUI-2 ✅（输入与焦点）、现有 `user/desktop` 演示
> 后续：GUI-4 字体与资源、GUI-5 Window Server + compositor、GUI-6 terminal 窗口化

---

## 1. 目标、范围与设计选择

### 1.1 完成条件

GUI-3 用 LVGL 重写 `user/desktop`，交付一个 **Material 风格的单进程桌面外壳**，并满足以下可核验条件：

1. **设计系统落地**：颜色角色、字体层级、间距、圆角、elevation、hover/pressed/focus/disabled 六类状态由**单一令牌表**驱动，任何界面元素不得硬编码颜色 / 字号 / 圆角。
2. **外壳四件套**：状态栏、启动器、应用抽屉、基本设置全部实现且可交互。
3. **应用模型明确**：内置应用的**启动**、**退出**与**全屏切换**语义被显式定义、显式实现、显式记录（§6）。
4. **不假装有窗口系统**：GUI-3 不实现窗口系统，因此**不绘制任何窗口装饰**（标题栏边框、最小化 / 最大化 / 关闭按钮、窗口阴影边框等一律不出现）。
5. **证据可复跑**：宿主契约测试覆盖令牌表与外壳状态机；QEMU E2E 覆盖屏幕呈现与输入。

### 1.2 不包含

| 不做 | 理由 / 归属 |
|------|------------|
| 多进程窗口、Z-order、拖拽、缩放、遮挡 | GUI-5 Window Server + compositor |
| 窗口化 terminal、PTY surface 接入 | GUI-6 |
| 深色主题 / 动态取色 / 主题切换 | 本期只交付浅色 Material（§1.3 取舍 2） |
| 中文字体、动态字体、图片资源、文件资源加载 | GUI-4 字体与资源 |
| IME、剪贴板、Unicode 布局、USB HID、触摸 | 超出 GUI 阶段 |
| aarch64 用户态 GUI | 等用户态与显示设备基础就绪 |
| **外部进程应用**（fork/exec 子进程接管全屏） | 本期只交付**壳内应用**（§1.3 取舍 1） |

### 1.3 方案取舍

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 应用模型 | **仅壳内应用**：每个应用是外壳内的一块全屏 LVGL 界面，由外壳自己创建与销毁 | roadmap 明确「先交付单进程桌面外壳」。外部子进程需要显示所有权交接 + 输入租约交接（GUI-2 的租约是独占的，子进程无法同时读键盘），会引入 fork/exec 与租约暂停/恢复两套新协议，风险与工作量都远超本期目标。应用「启动 / 退出 / 全屏切换」的**语义**已在 §6 完整定义，不依赖子进程。 |
| 2 | 主题范围 | **仅浅色 Material**：一套完整的 Material 3 浅色色彩角色表 | 深色主题需要成对的色彩角色表 + 设置项切换 + 即时重绘，属于独立增量。本期把色彩**角色**结构做对（而不是把颜色写死），将来加深色只需再加一套角色表。 |
| 3 | 设计系统归属 | 令牌表与主题实现放在 **libgui**（`gui/gui_theme.h` + `src/theme.c`），桌面消费它 | 令牌表是**跨应用**的 UI 语言，不是某个应用的私有常量；放 libgui 才能被宿主契约测试覆盖（用户态程序不在宿主测试范围内），也让 GUI-5 的窗口客户端能直接复用。 |
| 4 | LVGL 访问器 | `gui_platform_get_lv_display()` 从私有头**提升为公共 API**，公共头用**前置声明** `typedef struct _lv_display_t lv_display_t;` | GUI-3 是第一个真实业务应用，私有头的「公共业务应用禁止直接使用」约束必须正面解决。前置声明让 libgui 的公共 ABI **不依赖 LVGL 头文件**，sysroot 无需发布 LVGL 头（避免引入 LVGL 头树的发布与去重风险）。已实测：与 LVGL 自身 typedef 重复定义在 `-Wall -Wextra -Werror` 下无告警。 |
| 5 | 应用集合 | 4 个内置应用：**文件 / 终端 / 系统 / 设置** | 覆盖「浏览真实 VFS」「文本输入」「只读系统信息」「可交互设置」四类形态，足以验证外壳与令牌系统在真实内容下的表现；不引入需要子进程或网络协议栈的应用。 |
| 6 | 终端实现 | 壳内命令台，**内建命令**（help / ls / cat / uname / date / clear），经**原生按键回调**消费键入字符 | 不引入 PTY、不 fork 子进程。GUI-2 的原生回调是公开 API 且先于控件路径执行，回调消费掉的键不会再进入 LVGL 控件路径（被消费的 UP 仍会强制释放 LVGL 侧活动键），因此壳内终端不会破坏控件焦点。 |
| 7 | 状态栏时钟 | 每秒刷新一次（LVGL timer），显示 `HH:MM` 或 `hh:mm AM/PM`（设置项决定） | 12/24 小时制是「基本设置」的一项真实内容；秒级刷新对帧率无意义。 |
| 8 | 壁纸 | 纯色背景（令牌 `surface` 色），不使用图片资源 | GUI-4 才引入图片资源；纯色背景已能验证 elevation 与层次关系。 |

---

## 2. 当前实现与必须补齐的边界

### 2.1 现状（已核实）

| 事实 | 证据 |
|------|------|
| `user/desktop/desktop.c` 是**手绘**像素桌面：自绘 Material 卡片、squircle 图标、任务栏、欢迎窗口 | 源文件 700+ 行，全部走 `gfx_*` primitive + 自带 `font8x16.h` |
| 手绘桌面**没有输入**：只轮询 `/dev/mouse` 与 `/dev/keyboard` 的原始字节，自己维护 `mouse_x/mouse_y` | `desktop.c` 的 main loop 直接 `read()` 设备文件 |
| 现有桌面**画了窗口装饰**：欢迎窗口带最小化 / 最大化 / 关闭三个「按钮」 | `draw_welcome_window()`：只是画出来的像素，不带任何窗口语义 |
| GUI-1 已提供 `libgui`：`gui_platform_create/destroy/step/get_pixel` + 离屏 / 屏幕两种模式 | `libgui/include/gui/gui_platform.h` |
| GUI-2 已提供 `libgui` 输入：独占租约、LVGL indev、光标、修饰键、重复、Tab 焦点 | `libgui/include/gui/gui_input.h` |
| LVGL 9.5 已就位，`liblvgl.a` 与 `libgui.a` 一起进 sysroot | `libgui/Makefile` 的 install 规则 |
| LVGL 字体源码齐备：`montserrat_12/14/16/20/24/28` | `thirdpart/lvgl/src/font/`；但 `config/lv_conf.h` 只启用了 14 与 16 |
| 用户态 libc 有 `opendir/readdir`、`localtime/strftime`、`fork/execv/waitpid` | `libc/include/dirent.h`、`time.h`、`unistd.h` |
| LVGL 主题 API 可用：`lv_theme_create()` + `lv_theme_set_apply_cb()` + `lv_display_set_theme()` | `thirdpart/lvgl/src/themes/lv_theme.h` |

### 2.2 必须补齐的边界

1. **公共 LVGL 访问器**：私有头 `gui_lvgl_private.h` 的注释写明「公共业务应用禁止直接使用」，而 GUI-3 就是第一个公共业务应用 → 必须提升为公共 API（§1.3 取舍 4）。
2. **字体层级**：现有只启用 Montserrat 14 / 16，无法支撑 5 级字体层级 → 必须在 `config/lv_conf.h` 启用 12 / 20 / 24。
3. **设计令牌层**：仓库内没有任何跨应用共享的 UI 语言 → 新增 `libgui/include/gui/gui_theme.h` + `libgui/src/theme.c`。
4. **外壳状态机**：现有桌面只有 `drawer_open` / `window_open` 两个布尔量，没有可验证的界面状态模型 → 需要显式状态机（§5.1）。
5. **输入接入**：新桌面必须走 `libgui` 的输入通道（GUI-2），不得自己 `read()` 设备文件，也不得再持有独立的鼠标坐标。
6. **旧资产清理**：`user/desktop/font8x16.h`（手绘位图字体）与旧 `desktop.c` 全部作废，由 LVGL 字体 + 令牌表取代。

---

## 3. 分层、文件与所有权

```
┌──────────────────────────────────────────────────────────────┐
│ user/desktop/            业务层：外壳状态机 + 4 个内置应用      │
│   desktop.c              主入口：平台/输入装配 + 主循环         │
│   desktop_shell.{c,h}    外壳：表面、状态机、状态栏/启动器/抽屉   │
│   desktop_apps.{c,h}     应用注册表 + 4 个内置应用界面          │
└──────────────────────────────────────────────────────────────┘
              │  只依赖 libgui 公共 API + LVGL 头
┌──────────────────────────────────────────────────────────────┐
│ libgui/                  平台层 + 设计系统                     │
│   include/gui/gui.h      伞头（现有）                          │
│   include/gui/gui_platform.h / gui_input.h   （现有）        │
│   include/gui/gui_lvgl.h  ★新：公共 LVGL 显示句柄访问          │
│   include/gui/gui_theme.h ★新：Material 令牌表（无 LVGL 依赖） │
│   src/theme.c             ★新：令牌表实现 + LVGL 主题落地      │
└──────────────────────────────────────────────────────────────┘
```

**所有权规则**

| 对象 | 所有者 | 规则 |
|------|--------|------|
| `gui_platform_t` | `user/desktop` 主入口 | `main()` 结束前 `gui_platform_destroy()`；输入对象随平台销毁，桌面不得单独 `gui_input_destroy()` |
| `gui_input_t` | 平台（挂接后） | 桌面只持有指针用于注册回调，不得销毁 |
| `lv_display_t` | 平台 | 桌面通过 `gui_platform_get_lv_display()` 借用，不得 `lv_display_delete()` |
| `lv_obj_t` 树 | 外壳 | 切换表面时**整体销毁**旧表面并重建新表面（§5.1），不得跨表面保留控件指针 |
| 令牌表 | `libgui` | 只读全局常量，桌面不得修改 |

**文件清单**

| 文件 | 状态 | 职责 |
|------|------|------|
| `libgui/include/gui/gui_lvgl.h` | 新增 | 公共 LVGL 显示句柄访问（前置声明，无 LVGL 头依赖） |
| `libgui/src/gui_lvgl.c` 或并入 `platform.c` | 新增/修改 | 实现公共访问器 |
| `libgui/include/gui/gui_theme.h` | 新增 | 令牌表公共 API（色彩角色 / 字体层级 / 间距 / 圆角 / elevation） |
| `libgui/src/theme.c` | 新增 | 令牌表定义 + `gui_theme_apply()` 落地 LVGL 主题 |
| `libgui/internal/gui_lvgl_private.h` | 修改 | 私有声明改为「转发到公共头」，保留兼容 |
| `libgui/Makefile` | 修改 | 编译 `src/theme.c`；install 规则追加 2 个公共头 |
| `config/lv_conf.h` | 修改 | 启用 `LV_FONT_MONTSERRAT_12 / 20 / 24` |
| `user/desktop/desktop.c` | 重写 | 主入口 |
| `user/desktop/desktop_shell.{c,h}` | 新增 | 外壳 |
| `user/desktop/desktop_apps.{c,h}` | 新增 | 应用 |
| `user/desktop/font8x16.h` | 删除 | 手绘位图字体作废 |
| `hosttests/cases/test_gui_theme.c` | 新增 | 令牌表与主题契约测试 |
| `qemutests/gui_desktop_test.py` | 新增 | 桌面 E2E 门禁 |

---

## 4. Material 设计令牌系统

### 4.1 色彩角色（浅色）

角色命名沿用 Material 3，值取自 M3 基线浅色调色板；`success` / `tertiary` 是 OS01 扩展角色（Material 无对应项，用于「正常 / 提示」类语义）。

| 角色 | 值 | 用途 |
|------|----|------|
| `primary` | `#0B57D0` | 主要动作、主按钮填充、选中指示 |
| `on_primary` | `#FFFFFF` | `primary` 之上的文字/图标 |
| `primary_container` | `#D3E3FD` | 选中容器、当前页底色 |
| `on_primary_container` | `#041E49` | `primary_container` 之上的文字 |
| `secondary` | `#00639B` | 次要强调 |
| `on_secondary` | `#FFFFFF` | `secondary` 之上的文字 |
| `secondary_container` | `#CDE5FF` | 次要容器 |
| `on_secondary_container` | `#001D33` | `secondary_container` 之上的文字 |
| `tertiary` | `#8B5000` | 提示（警告类）强调 |
| `on_tertiary` | `#FFFFFF` | `tertiary` 之上的文字 |
| `tertiary_container` | `#FFDDB6` | 提示容器 |
| `on_tertiary_container` | `#2C1600` | `tertiary_container` 之上的文字 |
| `error` | `#BA1A1A` | 错误 |
| `on_error` | `#FFFFFF` | `error` 之上的文字 |
| `error_container` | `#FFDAD6` | 错误容器 |
| `on_error_container` | `#410002` | `error_container` 之上的文字 |
| `success` | `#146C2E` | 正常状态 |
| `on_success` | `#FFFFFF` | `success` 之上的文字 |
| `success_container` | `#C4EED0` | 正常容器 |
| `on_success_container` | `#00210B` | `success_container` 之上的文字 |
| `surface` | `#FDFCFF` | 页面底色（壁纸） |
| `on_surface` | `#1A1C1E` | `surface` 之上的主要文字 |
| `surface_variant` | `#E0E2EC` | 芯片 / 次级填充 |
| `on_surface_variant` | `#44474F` | 次要文字 |
| `surface_container_low` | `#F7F2FA` | 卡片（低） |
| `surface_container` | `#F3EDF7` | 卡片（默认） |
| `surface_container_high` | `#ECE6F0` | 卡片（高） |
| `surface_container_highest` | `#E6E0E9` | 卡片（最高） |
| `outline` | `#74777F` | 轮廓 |
| `outline_variant` | `#C4C6D0` | 细分割线 |
| `inverse_surface` | `#2F3033` | 反色表面（深色徽标底） |
| `inverse_on_surface` | `#F1F0F4` | `inverse_surface` 之上的文字 |
| `scrim` | `#000000` | 遮罩 |
| `shadow` | `#000000` | 阴影基色 |

### 4.2 字体层级

| 层级 | 字号 | LVGL 字体 | 用途 |
|------|------|-----------|------|
| `display` | 24 | `lv_font_montserrat_24` | 应用大标题、数值 |
| `headline` | 20 | `lv_font_montserrat_20` | 表面标题（抽屉 / 设置） |
| `title` | 16 | `lv_font_montserrat_16` | 卡片标题、应用列表项标题 |
| `body` | 14 | `lv_font_montserrat_14` | 正文、命令行、目录项 |
| `label` | 12 | `lv_font_montserrat_12` | 状态栏、辅助说明、图标下标签 |

字重只用 LVGL 内置的 normal（Montserrat 无粗体变体）；层级靠**字号 + 颜色角色**区分，不靠字重。

### 4.3 间距 / 圆角 / elevation

| 间距令牌 | 值 | 用途 |
|------|------|------|
| `space_xs` | 4 | 图标内边距、紧凑行距 |
| `space_sm` | 8 | 控件内边距、列表项间距 |
| `space_md` | 16 | 卡片内边距、表面内边距 |
| `space_lg` | 24 | 卡片间距、分区间距 |
| `space_xl` | 32 | 主区域留白 |

| 圆角令牌 | 值 | 用途 |
|------|------|------|
| `radius_sm` | 4 | 芯片、小徽标 |
| `radius_md` | 8 | 按钮、列表项 |
| `radius_lg` | 12 | 卡片 |
| `radius_xl` | 16 | 抽屉、对话框 |
| `radius_full` | 28 | 应用图标（圆形）、FAB |

| elevation | 表面色 | 阴影偏移 | 阴影透明度 | 用途 |
|----------|--------|----------|------------|------|
| 0 | `surface` | 0 | 0 | 页面底 |
| 1 | `surface_container_low` | 1 | 0x14 (8%) | 启动器条 |
| 2 | `surface_container` | 2 | 0x1F (12%) | 卡片 |
| 3 | `surface_container_high` | 4 | 0x29 (16%) | 抽屉、浮层 |
| 4 | `surface_container_highest` | 6 | 0x33 (20%) | 对话框 |
| 5 | `surface_container_highest` | 8 | 0x3D (24%) | 最高层浮层 |

阴影以 LVGL 的 `shadow_width` / `shadow_offset_y` / `shadow_opa` 落地，颜色恒为 `shadow` 角色。

### 4.4 交互状态

六类状态由 LVGL 状态选择器承载，样式一律从令牌表取值：

| 状态 | LVGL 选择器 | 视觉规则 |
|------|-------------|----------|
| 默认 | `LV_STATE_DEFAULT` | 卡片 `surface_container`；按钮 `primary` |
| hover | `LV_STATE_HOVERED` | 表面提亮一级（`surface_container` → `surface_container_high`）；主按钮提亮至 `secondary` |
| pressed | `LV_STATE_PRESSED` | 表面压暗一级（→ `surface_container_highest`）；主按钮压暗至 `primary_container` + 文字转 `on_primary_container` |
| focus | `LV_STATE_FOCUSED` | 2px `primary` 焦点环（`outline_width` + `outline_color`） |
| disabled | `LV_STATE_DISABLED` | 文本转 `on_surface_variant` 且整体不透明度 38%（`LV_OPA_50` 量级），填充转 `surface_variant` |
| checked | `LV_STATE_CHECKED` | 选中态填充 `primary_container`，文字 `on_primary_container`（用于设置项开关与当前页指示） |

规则：**焦点环与 hover / pressed 叠加时焦点环必须仍然可见**（焦点环画在状态选择器 `LV_STATE_FOCUSED` 上，LVGL 的状态优先级保证它不被覆盖）。

### 4.5 令牌表公共 API

公共头 **不包含 LVGL 头文件**（避免 sysroot 发布 LVGL 头）：

```c
/* libgui/include/gui/gui_theme.h */
typedef struct {
    uint32_t primary;                 /* 0x00RRGGBB */
    uint32_t on_primary;
    uint32_t primary_container;
    uint32_t on_primary_container;
    uint32_t secondary;
    uint32_t on_secondary;
    uint32_t secondary_container;
    uint32_t on_secondary_container;
    uint32_t tertiary;
    uint32_t on_tertiary;
    uint32_t tertiary_container;
    uint32_t on_tertiary_container;
    uint32_t error;
    uint32_t on_error;
    uint32_t error_container;
    uint32_t on_error_container;
    uint32_t success;
    uint32_t on_success;
    uint32_t success_container;
    uint32_t on_success_container;
    uint32_t surface;
    uint32_t on_surface;
    uint32_t surface_variant;
    uint32_t on_surface_variant;
    uint32_t surface_container_low;
    uint32_t surface_container;
    uint32_t surface_container_high;
    uint32_t surface_container_highest;
    uint32_t outline;
    uint32_t outline_variant;
    uint32_t inverse_surface;
    uint32_t inverse_on_surface;
    uint32_t scrim;
    uint32_t shadow;
} gui_theme_colors_t;

typedef enum {
    GUI_THEME_FONT_DISPLAY = 0,   /* 24 */
    GUI_THEME_FONT_HEADLINE,      /* 20 */
    GUI_THEME_FONT_TITLE,         /* 16 */
    GUI_THEME_FONT_BODY,          /* 14 */
    GUI_THEME_FONT_LABEL          /* 12 */
} gui_theme_font_t;

typedef enum {
    GUI_THEME_SPACE_XS = 0,       /* 4 */
    GUI_THEME_SPACE_SM,           /* 8 */
    GUI_THEME_SPACE_MD,           /* 16 */
    GUI_THEME_SPACE_LG,           /* 24 */
    GUI_THEME_SPACE_XL            /* 32 */
} gui_theme_space_t;

typedef enum {
    GUI_THEME_RADIUS_SM = 0,      /* 4 */
    GUI_THEME_RADIUS_MD,          /* 8 */
    GUI_THEME_RADIUS_LG,          /* 12 */
    GUI_THEME_RADIUS_XL,          /* 16 */
    GUI_THEME_RADIUS_FULL         /* 28 */
} gui_theme_radius_t;

typedef enum {
    GUI_THEME_ELEVATION_0 = 0,
    GUI_THEME_ELEVATION_1,
    GUI_THEME_ELEVATION_2,
    GUI_THEME_ELEVATION_3,
    GUI_THEME_ELEVATION_4,
    GUI_THEME_ELEVATION_5
} gui_theme_elevation_t;

const gui_theme_colors_t *gui_theme_colors(void);
int      gui_theme_font_size(gui_theme_font_t role);        /* 像素 */
int      gui_theme_space(gui_theme_space_t role);
int      gui_theme_radius(gui_theme_radius_t role);
int      gui_theme_elevation_shadow_width(gui_theme_elevation_t role);
int      gui_theme_elevation_shadow_offset_y(gui_theme_elevation_t role);
int      gui_theme_elevation_shadow_opa(gui_theme_elevation_t role);   /* 0..255 */
uint32_t gui_theme_elevation_surface(gui_theme_elevation_t role);
```

`gui_theme_apply()` 把令牌表落地为 LVGL 主题，其签名只用到前置声明的 `lv_display_t`：

```c
/* libgui/include/gui/gui_lvgl.h */
typedef struct _lv_display_t lv_display_t;
lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform);
void          gui_theme_apply(lv_display_t *display);
```

**主题落地方式**：自建 `lv_theme_t`（`lv_theme_create()` + `lv_theme_set_apply_cb()`），`apply_cb` 按控件类套用令牌样式；**不**以 LVGL 默认主题为父主题（默认主题的圆角 / 间距 / 状态色与 Material 令牌不一致，继承会让「统一」落空）。需覆盖的控件类限定为：

`LV_THEME_OBJ` / `LV_THEME_LABEL` / `LV_THEME_BUTTON` / `LV_THEME_SWITCH` / `LV_THEME_CHECKBOX` / `LV_THEME_SLIDER` / `LV_THEME_BAR` / `LV_THEME_LIST` / `LV_THEME_SCROLLBAR` / `LV_THEME_TEXTAREA`

---

## 5. 桌面外壳架构

### 5.1 表面与状态机

外壳同时**只有一个**可见表面。所有表面都占满整个显示区（全屏），彼此互斥。

| 状态 | 表面 | 内容 |
|------|------|------|
| `HOME` | 主屏 | 状态栏 + 欢迎卡片 + 启动器（dock） |
| `DRAWER` | 应用抽屉 | 状态栏 + 搜索提示 + 全部应用列表（滚动） |
| `SETTINGS` | 设置 | 状态栏 + 设置项列表 |
| `APP` | 应用 | 应用专属顶栏（标题 + 返回）+ 应用内容（全屏） |

状态迁移（**唯一**允许的迁移集合）：

```
HOME ──"应用"按钮 / Tab 到启动器后回车──▶ DRAWER
HOME ──点击启动器图标 / 抽屉中的条目──▶ APP(id)
HOME ──点击"设置"入口────────────────▶ SETTINGS
DRAWER ──ESC / 返回 / 点击背景────────▶ HOME
SETTINGS ──ESC / 返回────────────────▶ HOME
APP(id) ──ESC / 应用内"返回"─────────▶ HOME
```

**迁移的实现契约**：每次迁移都**销毁**当前表面的全部 `lv_obj_t` 并**重建**目标表面；外壳状态（设置项取值、当前目录、终端历史）保存在**外壳自己的 C 变量**里，不依赖控件存活。禁止「隐藏旧表面再显示新表面」——那会让隐藏的控件继续参与焦点组与命中测试。

**状态机由外壳自己持有**，UI 回调只**投递意图**（`desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, id)`），由外壳在主循环里串行执行。UI 回调**不得**直接触发迁移（LVGL 回调期间销毁对象是未定义行为）。

### 5.2 状态栏

| 元素 | 位置 | 说明 |
|------|------|------|
| 时间 | 左 | `HH:MM` 或 `hh:mm AM/PM`，由设置项 `clock_24h` 决定；每秒刷新 |
| 系统名 | 中 | `OS01` + 内核架构（`x86_64` / `aarch64`） |
| 指示区 | 右 | 显示器分辨率、输入设备（有鼠标 / 无鼠标）、内存池占用 |

状态栏固定高度 32px，`elevation 1` 表面色。

### 5.3 启动器（dock）

主屏底部的水平应用条，`elevation 2`，圆角 `radius_lg`，内含**固定顺序**的应用图标（图标 = 圆形色块 + 首字，色值取自应用注册表的 `accent` 角色）。

- 启动器是**应用集合的一个子集**（固定的「常用」列表），应用抽屉是**全集**。
- 启动器图标必须有文本标签（`label` 层级），保证不依赖颜色即可辨识。
- 点击图标 → `APP(id)`。

### 5.4 应用抽屉

全屏列表：状态栏 + 标题（`headline`）+ 应用列表（`title` 层级条目：图标 + 名称 + 一行说明）+ 底部提示（`label` 层级，说明 ESC 返回）。

- 列表可滚动（LVGL 滚动容器），滚轮与拖动均可用（GUI-2 已提供滚轮 → 滚动区域）。
- 顶部显示应用总数（`label` 层级）。

### 5.5 基本设置

| 设置项 | 类型 | 取值 | 生效方式 |
|--------|------|------|----------|
| 时钟格式 | 二选一 | 24 小时 / 12 小时 | 立即生效（状态栏下一次刷新即反映） |
| 壁纸颜色 | 颜色选择 | `surface` / `surface_container_low` / `surface_container_high` / `surface_container_highest` | 立即生效（重建主屏背景） |
| 卡片密度 | 二选一 | 舒适 / 紧凑（`space_md` ↔ `space_sm`） | 立即生效（重建当前表面） |
| 关于 | 只读 | 内核版本、LVGL 版本、构建 profile、显示模式 | 只读展示 |

设置项用 `switch` / `list` 控件表达，值变化通过**意图**回到外壳执行。

---

## 6. 应用启动 / 退出 / 全屏切换语义（明确约定）

这一节是 roadmap「应用启动/退出及全屏切换明确，不以绘制窗口装饰代替窗口系统」的正式落地。

### 6.1 术语

| 术语 | 定义 |
|------|------|
| **表面 (surface)** | 外壳在同一时刻可见的一棵 `lv_obj_t` 树，占满整个显示区 |
| **应用 (app)** | 注册表中的一条记录，拥有自己的应用界面构造器 |
| **应用表面** | `APP` 状态下、承载某个应用界面的表面 |
| **全屏** | 表面占满显示区（`0,0,W,H`），无标题栏、无边框、无阴影框 |

### 6.2 启动

- **触发**：点击启动器图标 / 点击抽屉条目 / 抽屉中用键盘选中后回车。
- **动作**：外壳执行 `HOME → APP(id)`：销毁主屏控件树 → 构造应用表面 → 应用表面**占满整个显示区**。
- **应用表面顶栏**：高度 40px，左侧是**返回按钮**（`←` 形状 + 「返回」文本），右侧是应用标题。顶栏是**应用界面的一部分**，**不是窗口装饰**：它没有最小化 / 最大化 / 关闭按钮，不画边框阴影，不表示任何窗口管理能力。
- **并发约束**：同一时刻只有一个应用表面存在；启动新应用必须先退出当前应用（`APP(a) → HOME → APP(b)`），不允许叠加。

### 6.3 退出

- **触发**：应用顶栏的返回按钮 / ESC 键 / 应用自己的「退出」动作。
- **动作**：销毁应用表面的全部控件 → 外壳状态回到 `HOME` → 重建主屏。
- **状态保留**：设置项、终端命令历史、文件浏览器当前目录等**外壳状态**在退出应用后**保留**（它们存在外壳变量里，与控件树无关）。
- **禁止**：退出应用时不得残留焦点对象、不得残留滚动位置、不得残留 LVGL 定时器（外壳在重建前必须 `lv_obj_delete` 整棵旧树，定时器随对象销毁）。

### 6.4 全屏切换

- **定义**：GUI-3 的所有表面**恒为全屏**。外壳**不存在**窗口化中间态：没有窗口位置 / 尺寸 / 边框 / 阴影 / Z-order / 焦点窗口栈。
- **切换语义**：`HOME ⇄ APP(id)` 是**整屏替换**，不是「开一个窗口盖在上面」。切换时旧表面的控件树被销毁，新表面从零构建；不存在两个表面同时可见的时刻（因此也不存在「哪个在上面」的问题）。
- **明确不做**：GUI-3 **不绘制**任何窗口装饰（标题栏边框、最小化 / 最大化 / 关闭按钮、可拖动边框）。旧 `desktop.c` 里的「欢迎窗口 + 三个窗口按钮」属于典型反例，本期**删除**。
- **归属**：多窗口、Z-order、遮挡、移动 / 缩放由 GUI-5（Window Server + compositor）负责；窗口化 terminal 由 GUI-6 负责。

### 6.5 键鼠契约

| 输入 | 行为 |
|------|------|
| 鼠标左键点击 | 触发控件动作（启动应用、切换设置项、返回…） |
| 鼠标滚轮 | 在抽屉 / 设置 / 文件列表的滚动区域内滚动 |
| `Tab` / `Shift+Tab` | 在当前表面的焦点组内前进 / 后退（GUI-2 已提供） |
| `Enter` | 激活当前焦点控件 |
| `ESC` | 返回上一层（`APP → HOME`，`DRAWER → HOME`，`SETTINGS → HOME`）；在 `HOME` 上无副作用 |
| 终端应用内的可打印字符 | 由终端应用消费（原生按键回调），不进入控件路径 |

---

## 7. 内置应用

4 个应用全部在壳内渲染，共享同一套令牌与控件构造辅助函数。

### 7.1 文件（Files）

- 只读目录浏览：`opendir` / `readdir` / `closedir`。
- 初始目录 `/`；条目分「目录」与「文件」两类（目录在前，各自按名称排序）。
- 每行：`目录` / `文件` 名称（`body` 层级）+ 类型标签（`label` 层级，右对齐）。
- 进入子目录：双击 / 回车；返回：顶栏返回键 → 父目录（不是回主屏）。因此文件应用需要**两级返回**：先回父目录，父目录为 `/` 时再回主屏。
- 不可写、不可删除、不可新建——只读浏览器的边界必须在界面上说清楚（顶栏副标题注明「只读」）。

### 7.2 终端（Terminal）

- 壳内命令台：一个 `body` 字体的多行文本区 + 一行提示符 `OS01 $ ` + 当前输入行。
- 输入：原生按键回调消费 `codepoint`（ASCII 0x20–0x7E），`Enter` 执行，`Backspace` 退格。
- 内建命令：`help`、`ls [path]`、`cat <file>`、`uname`、`date`、`clear`。未知命令返回 `command not found`。
- 历史：`↑` / `↓` 在命令历史中前后翻（保留最近 32 条）。
- 终端**不 fork 子进程**：所有命令都是壳内实现。

### 7.3 系统（System）

只读展示运行时信息：

- 内核：架构、编译profile（来自构建期宏）、启动时间
- 显示：模式（屏幕 / 离屏）、分辨率、像素格式
- 输入：键盘 / 鼠标是否存在、焦点组容量
- LVGL：版本号、内存池总量与占用率

### 7.4 设置（Settings）

§5.5 的四项设置。

---

## 8. 错误、资源与构建约束

### 8.1 错误语义

| 情况 | 行为 |
|------|------|
| `gui_platform_create` 失败 | 向 stderr 打印 `errno` 与原因，退出码 1；`EBUSY` 明确提示「已有 GUI 占用显示」 |
| `gui_input_create` 失败（无鼠标） | **允许降级**：`require_mouse = false`，桌面继续运行，状态栏指示区显示「无鼠标」 |
| `gui_input_create` 失败（其他错误） | 退出码 1，打印 errno |
| `gui_platform_step` 返回 `EAGAIN` | 按返回的建议延迟继续推进（不退出） |
| `gui_platform_step` 返回 `LOST_OWNERSHIP` / `DISPLAY_CHANGED` / `ERROR` | 打印诊断，销毁平台，退出码 1 |
| 内存池耗尽（LVGL 分配失败） | LVGL 断言会中止进程；桌面在构造表面前检查 `lv_mem_monitor` 余量，低于阈值时打印诊断并退出（不静默半初始化） |

### 8.2 资源预算

| 资源 | 预算 | 说明 |
|------|------|------|
| LVGL 内存池 | 4 MiB（现有 `LV_MEM_SIZE`） | 桌面构造 4 个表面（每次只存活 1 个）后必须有余量；构造失败即退出 |
| 字体 | 5 个字号（12/14/16/20/24） | 相对现有 2 个字号新增 3 个，需审计 `liblvgl.a` 体积增长 |
| 图形 | 无图片资源 | 壁纸纯色、图标为纯色圆块 + 文字 |
| 每次显示提交 | 1 次 `gfx_present` | 沿用 libgui 的单帧一次提交语义 |

### 8.3 构建约束

- `config/lv_conf.h` 改动 → LVGL 全部重编（`make clean` 语义由 `.d` 依赖保证，但 `LV_*` 宏变化**必须** `make clean`）。
- `libgui/Makefile` 的 install 规则必须追加 `gui_lvgl.h`、`gui_theme.h` 两个公共头到 `usr/include/gui/`；manifest 生成规则已经覆盖（`find` 扫描）。
- 用户态程序链接 `libgui.a` 时，LVGL 头来自源码树（沿用 `test_lvgl` 的 `-I thirdpart -I thirdpart/lvgl -I config -DLV_CONF_INCLUDE_SIMPLE` 既有做法），sysroot **不**发布 LVGL 头（§1.3 取舍 4 的约束）。
- 用户态程序不得绕过 libgui 直接 `open("/dev/keyboard")` 或 `open("/dev/mouse")`——桌面必须走 GUI-2 的输入通道。

---

## 9. 验证与交付门禁

### 9.1 宿主契约测试（`hosttests/cases/test_gui_theme.c`）

| # | 断言 |
|---|-------|
| 1 | 每个色彩角色的值都是合法 `0x00RRGGBB`（低 8 位为 0） |
| 2 | 色彩角色两两不同（`on_*` 与其基底除外，成对关系必须成立：`on_x` 在 `x` 上可读） |
| 3 | 字体层级字号严格递减：display > headline > title > body > label |
| 4 | 间距令牌严格递增：xs < sm < md < lg < xl |
| 5 | 圆角令牌严格递增：sm < md < lg < xl < full |
| 6 | elevation 0..5 的阴影宽度 / 偏移 / 不透明度单调不减 |
| 7 | elevation 表面色随层级单调变深（`surface` → `container_low` → `container` → `container_high` → `container_highest`） |
| 8 | `gui_theme_apply()` 在离屏 display 上成功，且主题非空 |

### 9.2 桌面外壳状态机契约测试（`hosttests/cases/test_desktop_shell.c`）

外壳状态机必须能在**不依赖真实显示**的情况下被验证：把外壳的迁移逻辑抽成纯函数 `desktop_shell_next_state(current, intent) -> next`，宿主测试穷举 (状态 × 意图) 组合：

| # | 断言 |
|---|----|
| 1 | 只允许 §5.1 列出的迁移；其余组合返回 `DESKTOP_STATE_INVALID` |
| 2 | `HOME + OPEN_APP(id)` → `APP(id)` |
| 3 | `APP(a) + OPEN_APP(b)` → **不允许**（必须先回 HOME） |
| 4 | `ESC` 在 `HOME` 上不改变状态 |
| 5 | `BACK` 在 `DRAWER` / `SETTINGS` / `APP` 上一律 → `HOME` |

### 9.3 QEMU E2E（`qemutests/gui_desktop_test.py`）

| # | 检查 |
|---|----|
| 1 | 桌面启动后画面非空（screendump 有有效像素），退出码 0 |
| 2 | 离屏模式（`--smoke`）下像素断言：状态栏区域为 `surface_container_low` 系、启动器卡片为 `surface_container` 系 |
| 3 | 注入点击启动器图标 → 画面切换到应用表面（像素变化） |
| 4 | 注入 ESC → 回到主屏（像素回到初始采样） |
| 5 | 桌面退出后终端 fallback 恢复（显示所有权协议回归） |

### 9.4 回归入口

```bash
make PROFILE=x86_64-clang test-host                      # 含新的 theme / shell 契约测试
make PROFILE=x86_64-clang test-qemu SUITE=gui            # 既有 GUI 套件
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop    # 新增桌面 E2E
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest
make PROFILE=x86_64-clang test-static                    # 静态审计
```

---

## 10. 交付分期

| 阶段 | 内容 | 完成判据 |
|------|------|----------|
| **P1 令牌层** | `gui_theme.h` + `theme.c` + 公共 LVGL 访问器 + `lv_conf.h` 字体启用 + libgui 打包 | 宿主令牌测试全绿；`libgui.a` 含 theme 符号；公共头进入 sysroot |
| **P2 外壳骨架** | 状态机 + 主屏 + 状态栏 + 启动器 + 抽屉 + 设置（空壳） | 状态机契约测试全绿；QEMU 可截到主屏 |
| **P3 内置应用** | 文件 / 终端 / 系统 / 设置四项 | 四个应用均可进入、可用、可用 ESC 返回 |
| **P4 收口** | 文档（roadmap / docs/gui/gui.md / changelog）+ 全量回归 | 全量回归绿；文档同步 |

每一阶段独立提交、独立可回滚。
