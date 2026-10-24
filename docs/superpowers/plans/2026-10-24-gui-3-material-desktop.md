# GUI-3 Material Desktop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 LVGL 重写 `user/desktop`，交付一个由单一 Material 令牌表驱动的单进程桌面外壳（状态栏 / 启动器 / 应用抽屉 / 基本设置 + 4 个内置应用），并把「应用启动 / 退出 / 全屏切换」的语义显式定义与实现，不绘制任何窗口装饰。

**Architecture:** 设计系统下沉到 `libgui`（新增公共 `gui/gui_theme.h` 令牌表 + `gui/gui_lvgl.h` 公共 LVGL 显示句柄访问），桌面只消费公共 API。外壳的界面状态机抽成**纯 C 无 LVGL 依赖**的 `user/desktop/desktop_state.c`，因此可在宿主测试里穷举 (状态 × 意图) 组合；LVGL 控件构造集中在 `desktop_shell.c`，内置应用集中在 `desktop_apps.c`。所有应用都是**壳内应用**：应用表面占满整个显示区，退出即销毁并重建主屏。

**Tech Stack:** C11、OS01 libc（`opendir/readdir`、`localtime/strftime`）、LVGL v9.5.0（`thirdpart/lvgl` submodule，v9.3.0-992-g85aa60d18）、libgui、libgfx、宿主 hosttests、QEMU/QMP。

**Spec:** [2026-10-24-gui-3-material-desktop-design.md](../specs/2026-10-24-gui-3-material-desktop-design.md)。

**Plan状态：** Revision 1（初稿，待评审）。实施基线 `d0153dc3`（`master`），分支 `feature/gui-3-material-desktop`，worktree `/home/aagu/OS01/.worktrees/gui-3-material-desktop`。

## Global Constraints

- **单进程**：桌面是唯一进程；不 fork / exec 子进程，不引入显示所有权交接与输入租约交接（spec §1.3 取舍 1）。
- **仅浅色 Material**：一套浅色色彩角色表；不做深色主题、不做主题切换项（spec §1.3 取舍 2）。
- **不画窗口装饰**：无标题栏边框、无最小化 / 最大化 / 关闭按钮、无可拖动边框、无 Z-order；旧 `user/desktop/desktop.c` 里的「欢迎窗口 + 三按钮」必须删除（spec §6.4）。
- **令牌唯一来源**：颜色 / 字号 / 间距 / 圆角 / elevation 一律取自 `gui_theme_*` API；桌面代码里不得出现裸颜色字面量（`0x......`）与裸字号数字。
- **公共 ABI 不含 LVGL 头**：`libgui/include/gui/*.h` 只用前置声明 `typedef struct _lv_display_t lv_display_t;`；sysroot **不**发布 LVGL 头。
- **输入只走 GUI-2 通道**：桌面不得 `open("/dev/keyboard")` / `open("/dev/mouse")`；鼠标坐标、焦点、重复、滚轮一律由 libgui 提供。
- **字体层级**：`config/lv_conf.h` 必须启用 `LV_FONT_MONTSERRAT_12 / 14 / 16 / 20 / 24`（当前只有 14 / 16）；改 `lv_conf.h` 后必须 `make clean`。
- **LVGL 内存池**：4 MiB（`LV_MEM_SIZE` 不变）；构造表面前检查余量，不足则诊断退出，不静默半初始化。
- **宿主对象隔离**：宿主测试只用宿主 libc / 宿主 LVGL 对象，不得混用目标库或目标 libc；sysroot 仍由单一发布器写入。
- **测试入口**：所有测试从仓库根 make 入口运行；宿主 focused 用 `make PROFILE=x86_64-clang test-gui-host CASE=<case>`。
- **不扩张到**：GUI-4 字体与资源、GUI-5 窗口服务器、GUI-6 窗口化 terminal、IME、USB HID、aarch64 用户态 GUI。

## Review Focus

以下六项是实施时最容易遗漏的故障输入；对应断言已归入任务，不以「后续人工看看」代替：

1. **令牌被绕过** —— 桌面里出现裸颜色 / 裸字号，令牌表形同虚设 → Task 5 加**静态审计**（扫描 `user/desktop/*.c` 的颜色字面量）。
2. **迁移在 LVGL 回调里发生** —— 回调期间销毁控件树是未定义行为 → Task 4 的外壳只投递意图，迁移在主循环执行。
3. **迁移后残留旧表面** —— 旧控件留在焦点组里，Tab 会跳进已销毁的应用 → Task 4 迁移前整体销毁 + Task 3 状态机断言 `APP(a) → APP(b)` 不允许。
4. **终端消费按键后焦点卡死** —— 消费掉的 DOWN 没进 LVGL，但 UP 仍须释放 LVGL 侧活动键 → Task 6 用真实 LVGL 断言「终端退出后焦点组可继续导航」。
5. **状态栏时钟不刷新或刷新泄漏** —— 每个 tick 新建定时器会导致定时器雪崩 → Task 4 断言定时器只创建一次、每秒只触发一次。
6. **旧手绘资产残留** —— `font8x16.h` / 旧的 `gfx_*` 手绘路径仍在编译 → Task 4 删除文件 + Task 5 静态审计断言桌面不再直接调用 `gfx_*` primitive。

## 工作方式、验证入口与文件分工

先读本 plan、spec 和根 `AGENTS.md`；存在 `.codegraph/` 时先用 `codegraph node/explore` 定位代码，未索引的文档 / 文件再直接读。执行前用 `superpowers:using-git-worktrees` 检查隔离状态；不覆盖别人的修改（`master` 可能仍在前进，合并前先 rebase）。

每个任务都先写断言跑出**预期失败**（RED），再实现跑绿（GREEN），最后提交该任务列出的文件。RED 可以是缺符号 / 缺声明，但不得是无关环境失败，必须记录失败原因。已有行为回归是 GREEN 门禁的一部分。可重复故障用 `superpowers:systematic-debugging`；不靠增大超时、屏蔽断言或修改验收条件「修复」。

### 宿主 focused 入口（随所属任务引入）

扩展既有 `make PROFILE=x86_64-clang test-gui-host CASE=...`，不新建根测试 bucket：

| CASE | 二进制与引入任务 |
|---|---|
| `theme` | `test_gui_theme.elf`，Task 1 |
| `desktop-state` | `test_desktop_state.elf`，Task 3 |

各 CASE 的 hosttests 子 make 目标命名 `test-gui-<case>`，由 root `os01_submake` 转发；不得从 shell 直接 `make -C hosttests`。每新增 binary 同时加入 `hosttests/Makefile::TEST_BINS`、`mk/components/run.mk::GUI_HOST_REGISTERED_CASES` 及对应的 `_test-gui-host-run-<case>` 目标。未知 CASE 在构建前非零失败。

### 文件责任图

| 单元 | 文件与责任 |
|---|---|
| 设计令牌 | 新 `libgui/include/gui/gui_theme.h` + `libgui/src/theme.c`：色彩角色表、字号 / 间距 / 圆角 / elevation 取值 API、LVGL 主题落地 |
| 公共 LVGL 访问 | 新 `libgui/include/gui/gui_lvgl.h`：前置声明 + `gui_platform_get_lv_display()`；私有头 `libgui/internal/gui_lvgl_private.h` 改为转发 |
| 界面状态机 | 新 `user/desktop/desktop_state.{c,h}`：**纯 C**，无 LVGL 依赖；状态 + 意图 + 迁移纯函数 |
| 外壳 UI | 新 `user/desktop/desktop_shell.{c,h}`：表面构建（状态栏 / 主屏 / 启动器 / 抽屉 / 设置）、令牌化样式辅助 |
| 内置应用 | 新 `user/desktop/desktop_apps.{c,h}`：应用注册表 + 文件 / 终端 / 系统 / 设置四个应用界面 |
| 主入口 | 重写 `user/desktop/desktop.c`：平台 + 输入装配、主循环、意图执行、退出语义 |
| 构建 / 发布 | `libgui/Makefile`（新增源文件 + 公共头安装）、`config/lv_conf.h`（字体）、`user/desktop/app.mk`（链接与包含路径） |
| 宿主测试 | 新 `hosttests/cases/test_gui_theme.c`、`hosttests/cases/test_desktop_state.c`；`hosttests/Makefile`、`mk/components/run.mk` 注册 |
| 端到端 | 新 `qemutests/gui_desktop_test.py` + runner 接线；`qemutests/test_gui_runner.py` 补 fake 契约 |
| 文档 | `docs/roadmap.md`、`docs/gui/gui.md`、`docs/changelog.md` |

---

### Task 1: Material 令牌表与公共 API

**Files:** Create `libgui/include/gui/gui_theme.h`、`libgui/src/theme.c`、`hosttests/cases/test_gui_theme.c`；Modify `libgui/Makefile`、`hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:** 公共头 `gui/gui_theme.h` 不包含 LVGL 头，只用 `<stdint.h>`。产出（spec §4.5 逐字一致）：

```c
typedef struct { uint32_t primary; uint32_t on_primary; uint32_t primary_container;
                 uint32_t on_primary_container; uint32_t secondary; uint32_t on_secondary;
                 uint32_t secondary_container; uint32_t on_secondary_container;
                 uint32_t tertiary; uint32_t on_tertiary; uint32_t tertiary_container;
                 uint32_t on_tertiary_container; uint32_t error; uint32_t on_error;
                 uint32_t error_container; uint32_t on_error_container; uint32_t success;
                 uint32_t on_success; uint32_t success_container; uint32_t on_success_container;
                 uint32_t surface; uint32_t on_surface; uint32_t surface_variant;
                 uint32_t on_surface_variant; uint32_t surface_container_low;
                 uint32_t surface_container; uint32_t surface_container_high;
                 uint32_t surface_container_highest; uint32_t outline; uint32_t outline_variant;
                 uint32_t inverse_surface; uint32_t inverse_on_surface; uint32_t scrim;
                 uint32_t shadow; } gui_theme_colors_t;

typedef enum { GUI_THEME_FONT_DISPLAY = 0, GUI_THEME_FONT_HEADLINE,
               GUI_THEME_FONT_TITLE, GUI_THEME_FONT_BODY,
               GUI_THEME_FONT_LABEL } gui_theme_font_t;
typedef enum { GUI_THEME_SPACE_XS = 0, GUI_THEME_SPACE_SM, GUI_THEME_SPACE_MD,
               GUI_THEME_SPACE_LG, GUI_THEME_SPACE_XL } gui_theme_space_t;
typedef enum { GUI_THEME_RADIUS_SM = 0, GUI_THEME_RADIUS_MD, GUI_THEME_RADIUS_LG,
               GUI_THEME_RADIUS_XL, GUI_THEME_RADIUS_FULL } gui_theme_radius_t;
typedef enum { GUI_THEME_ELEVATION_0 = 0, GUI_THEME_ELEVATION_1, GUI_THEME_ELEVATION_2,
               GUI_THEME_ELEVATION_3, GUI_THEME_ELEVATION_4,
               GUI_THEME_ELEVATION_5 } gui_theme_elevation_t;

const gui_theme_colors_t *gui_theme_colors(void);
int      gui_theme_font_size(gui_theme_font_t role);
int      gui_theme_space(gui_theme_space_t role);
int      gui_theme_radius(gui_theme_radius_t role);
int      gui_theme_elevation_shadow_width(gui_theme_elevation_t role);
int      gui_theme_elevation_shadow_offset_y(gui_theme_elevation_t role);
int      gui_theme_elevation_shadow_opa(gui_theme_elevation_t role);
uint32_t gui_theme_elevation_surface(gui_theme_elevation_t role);
```

所有取值函数对非法枚举返回 `-1`（`gui_theme_elevation_surface` 返回 `0xFFFFFFFFu`）；越界**不得**读越界数组。

- [ ] **1. 写 RED fixture。** `hosttests/cases/test_gui_theme.c` 断言（spec §9.1 全部 8 条）：① 每个色彩角色低 8 位为 0 且 RGB 分量非全 0（除 `shadow` / `scrim` 允许为纯黑）；② `on_x` 与 `x` 成对存在且取值不同；③ 字号 `24 > 20 > 16 > 14 > 12`；④ 间距 `4 < 8 < 16 < 24 < 32`；⑤ 圆角 `4 < 8 < 12 < 16 < 28`；⑥ elevation 0..5 的 shadow 宽度 / 偏移 / 不透明度单调不减，且 elevation 0 的三项均为 0；⑦ elevation 表面色序列为 `surface → surface_container_low → surface_container → surface_container_high → surface_container_highest`（严格等于该序列的元素值）；⑧ 非法枚举返回 `-1`。
- [ ] **2. 接入 `CASE=theme` 后执行** `make PROFILE=x86_64-clang test-gui-host CASE=theme`，确认因缺 `gui_theme_*` 符号而 RED（记录失败信息）。
- [ ] **3. 实现令牌表。** `libgui/src/theme.c` 用 `static const gui_theme_colors_t g_colors = {...}` 定义 spec §4.1 的**逐个具体值**（`primary=0x000B57D0`、`on_primary=0x00FFFFFF`、`primary_container=0x00D3E3FD`、`on_primary_container=0x00041E49`、`secondary=0x0000639B`、`on_secondary=0x00FFFFFF`、`secondary_container=0x00CDE5FF`、`on_secondary_container=0x00001D33`、`tertiary=0x0008B5000`、`on_tertiary=0x00FFFFFF`、`tertiary_container=0x00FFDDB6`、`on_tertiary_container=0x002C1600`、`error=0x00BA1A1A`、`on_error=0x00FFFFFF`、`error_container=0x00FFDAD6`、`on_error_container=0x00410002`、`success=0x00146C2E`、`on_success=0x00FFFFFF`、`success_container=0x00C4EED0`、`on_success_container=0x0000210B`、`surface=0x00FDFCFF`、`on_surface=0x001A1C1E`、`surface_variant=0x00E0E2EC`、`on_surface_variant=0x0044474F`、`surface_container_low=0x00F7F2FA`、`surface_container=0x00F3EDF7`、`surface_container_high=0x00ECE6F0`、`surface_container_highest=0x00E6E0E9`、`outline=0x0074777F`、`outline_variant=0x00C4C6D0`、`inverse_surface=0x002F3033`、`inverse_on_surface=0x00F1F0F4`、`scrim=0x00000000`、`shadow=0x00000000`）。字号 `{24,20,16,14,12}`、间距 `{4,8,16,24,32}`、圆角 `{4,8,12,16,28}`、elevation 阴影宽度 `{0,1,2,4,6,8}`、偏移 `{0,1,2,4,6,8}`、不透明度 `{0,20,31,41,51,61}`、表面色 `{surface, surface_container_low, surface_container, surface_container_high, surface_container_highest, surface_container_highest}` 全部写死为常量数组。
- [ ] **4. 构建接线。** `libgui/Makefile` 的 `PLATFORM_C_SOURCES` 追加 `$(LIBGUI_DIR)/src/theme.c`；`hosttests/Makefile` 新增 `test_gui_theme.elf`（编译 `hosttests/cases/test_gui_theme.c` + `libgui/src/theme.c`，宿主编译，不链接目标 libc）并加入 `TEST_BINS`；`mk/components/run.mk` 的 `GUI_HOST_REGISTERED_CASES` 追加 `theme`，并新增 `_test-gui-host-run-theme` 目标。
- [ ] **5. 执行 `make PROFILE=x86_64-clang test-gui-host CASE=theme` 与 `make PROFILE=x86_64-clang test-host`。** 新断言全绿；既有 hosttests 全绿；`libgui.a` 归档含 `gui_theme_*` 符号（`llvm-nm` 抽查）。
- [ ] **6. 提交** `feat(gui): add Material design token table`。

---

### Task 2: 公共 LVGL 访问器、LVGL 主题落地与字体层级

**Files:** Create `libgui/include/gui/gui_lvgl.h`；Modify `libgui/src/theme.c`、`libgui/src/platform.c`、`libgui/internal/gui_lvgl_private.h`、`libgui/Makefile`、`config/lv_conf.h`、`hosttests/Makefile`、`hosttests/cases/test_gui_theme.c`。

**Interfaces:**

- 新公共头 `libgui/include/gui/gui_lvgl.h`：
  ```c
  #ifndef _GUI_LVGL_H
  #define _GUI_LVGL_H
  #include <gui/gui_platform.h>
  typedef struct _lv_display_t lv_display_t;
  lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform);
  void          gui_theme_apply(lv_display_t *display);
  #endif
  ```
- `libgui/internal/gui_lvgl_private.h` 改为**只** `#include <gui/gui_lvgl.h>`，不再重复声明（消除双份真源）。
- `libgui/src/platform.c` 的 `gui_platform_get_lv_display()` 保留实现，签名改成公共头版本；`gui_platform_destroy()` 里在 `lv_display_delete()` **之前**调用 `gui_theme_apply` 创建的主题随 display 释放（LVGL 主题挂在 display 上，display 删除即销毁；不额外持有）。
- `gui_theme_apply()` 用 `lv_theme_create()` + `lv_theme_set_apply_cb()` 自建主题，**不**以 LVGL 默认主题为父主题；覆盖类：`LV_THEME_OBJ / LABEL / BUTTON / SWITCH / CHECKBOX / SLIDER / BAR / LIST / SCROLLBAR / TEXTAREA`。

- [ ] **1. 写 RED fixture（追加到 `test_gui_theme.c`）。** 复用既有 `test_gui_lvgl_integration.c` 的离屏 display 装配方式，断言 `gui_theme_apply(disp)` 之后 `lv_display_get_theme(disp) != NULL`；再断言一个 `lv_button_create()` 的默认样式里 `bg_color` 等于 `gui_theme_colors()->primary`、`radius` 等于 `gui_theme_radius(GUI_THEME_RADIUS_MD)`、`text_font` 等于 24/20/16/14/12 之一的字号（`lv_font_get_line_height` 比对）。同时断言 `gui_platform_get_lv_display(NULL)` 返回 `NULL` 且 `errno == EINVAL`。
- [ ] **2. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=theme`，确认因缺 `gui_theme_apply` / 公共头而 RED。
- [ ] **3. 实现 `gui_theme_apply()`。** 主题 `apply_cb` 按控件类套用令牌：① `LV_THEME_OBJ`：`bg_color = surface`、`border_color = outline_variant`、`border_width = 1`、`radius = radius_lg`、`pad = space_md`、`text_color = on_surface`、`text_font = &lv_font_montserrat_14`；② `LV_THEME_LABEL`：只设 `text_color = on_surface`、`text_font = &lv_font_montserrat_14`（不改 bg）；③ `LV_THEME_BUTTON`：`bg_color = primary`、`text_color = on_primary`、`radius = radius_md`、`pad_h/pad_v = space_sm`，并用状态选择器追加：`LV_STATE_HOVERED → bg secondary`、`LV_STATE_PRESSED → bg primary_container + text on_primary_container`、`LV_STATE_DISABLED → bg surface_variant + text on_surface_variant`；④ `LV_THEME_SWITCH` / `CHECKBOX`：主色 `primary` / `on_primary`，`LV_STATE_CHECKED → primary_container + on_primary_container`；⑤ `LV_THEME_SLIDER` / `BAR`：`bg_color = secondary_container`、`indicator bg = primary`、`knob = primary`；⑥ `LV_THEME_LIST`：行 `bg = surface`、`radius = radius_md`、`pad = space_sm`；⑦ `LV_THEME_SCROLLBAR`：`bg = outline_variant`、`radius = radius_full`；⑧ `LV_THEME_TEXTAREA`：`bg = surface_variant`、`radius = radius_md`、`text_color = on_surface`。**焦点环**统一用 `LV_STATE_FOCUSED` 选择器追加 `border_width = 2` + `border_color = primary`。
- [ ] **4. 提升公共访问器。** 新建 `libgui/include/gui/gui_lvgl.h`；`libgui/internal/gui_lvgl_private.h` 改为转发；`libgui/src/platform.c` 的实现签名对齐公共头；`libgui/Makefile` 的 install 规则把 `gui_lvgl.h` 与 `gui_theme.h` 一并 `cp` 到 `$(INSTALL_ROOT)/usr/include/gui/`；`libgui/include/gui/gui.h` 伞头追加 `#include <gui/gui_lvgl.h>` 与 `#include <gui/gui_theme.h>`。
- [ ] **5. 启用字体层级。** `config/lv_conf.h` 把 `LV_FONT_MONTSERRAT_12 / 20 / 24` 从 `0` 改为 `1`（14 / 16 保持 `1`）。**必须 `make clean`** 后重编（LVGL 配置宏变化不触发 `.d` 依赖）。审计 `liblvgl.a` 体积增长（记录字节数变化，写进提交说明）。
- [ ] **6. 执行 `make clean && make` 与 `make PROFILE=x86_64-clang test-gui-host CASE=theme`、`make PROFILE=x86_64-clang test-host`。** 全部绿；`libgui.a` 含 `gui_theme_apply` 与 `gui_platform_get_lv_display` 强符号；sysroot 的 `usr/include/gui/` 含 5 个公共头。
- [ ] **7. 提交** `feat(gui): publish LVGL display accessor and apply Material theme`。

---

### Task 3: 外壳界面状态机（纯 C，可宿主测试）

**Files:** Create `user/desktop/desktop_state.{c,h}`、`hosttests/cases/test_desktop_state.c`；Modify `hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:** `user/desktop/desktop_state.h` **不包含任何 LVGL 头**，只含 `<stdbool.h>` / `<stdint.h>`：

```c
typedef enum { DESKTOP_STATE_HOME = 0, DESKTOP_STATE_DRAWER,
               DESKTOP_STATE_SETTINGS, DESKTOP_STATE_APP,
               DESKTOP_STATE_INVALID = -1 } desktop_state_t;

typedef enum { DESKTOP_INTENT_NONE = 0, DESKTOP_INTENT_OPEN_DRAWER,
               DESKTOP_INTENT_OPEN_SETTINGS, DESKTOP_INTENT_OPEN_APP,
               DESKTOP_INTENT_BACK, DESKTOP_INTENT_ESC } desktop_intent_t;

desktop_state_t desktop_state_next(desktop_state_t cur, desktop_intent_t intent,
                                   const char *app_id);
bool            desktop_state_is_valid(desktop_state_t st);
const char     *desktop_state_name(desktop_state_t st);
```

迁移规则（spec §5.1，唯一允许集合）：

| 当前 | 意图 | 结果 |
|------|------|------|
| HOME | `OPEN_DRAWER` | DRAWER |
| HOME | `OPEN_SETTINGS` | SETTINGS |
| HOME | `OPEN_APP(id)` | APP（`app_id` 非空且非 `DESKTOP_STATE_INVALID` 时） |
| HOME | `BACK` / `ESC` | HOME |
| DRAWER | `BACK` / `ESC` | HOME |
| SETTINGS | `BACK` / `ESC` | HOME |
| APP | `BACK` / `ESC` | HOME |
| APP | `OPEN_APP(id)` | **INVALID**（禁止叠加） |
| 其他组合 | — | INVALID |

- [ ] **1. 写 RED fixture。** `hosttests/cases/test_desktop_state.c` 断言（spec §9.2 全部 5 条）：① 表驱动穷举 (状态 × 意图) 全组合，结果**只**可能是期望集合内的状态或 `DESKTOP_STATE_INVALID`；② `HOME + OPEN_APP("files")` → `DESKTOP_STATE_APP`；③ `APP("a") + OPEN_APP("b")` → `DESKTOP_STATE_INVALID`；④ `HOME + ESC` → `HOME`（不改变）；⑤ `DRAWER / SETTINGS / APP + BACK` → 一律 `HOME`；⑥ `HOME + OPEN_APP(NULL)` → `DESKTOP_STATE_INVALID`；⑦ `desktop_state_name()` 对每个合法状态返回非空且互不相同。
- [ ] **2. 接入 `CASE=desktop-state` 后执行** `make PROFILE=x86_64-clang test-gui-host CASE=desktop-state`，确认因缺 `desktop_state_*` 符号而 RED。
- [ ] **3. 实现状态机。** `desktop_state.c` 是**纯 C**：不含 LVGL 头、不含 libc 调用（`desktop_state_name()` 用静态字符串表）。`desktop_state_next()` 是**纯函数**：不访问全局状态，不读 `app_id` 之外的东西。
- [ ] **4. 构建接线。** `hosttests/Makefile` 编译**真实的** `user/desktop/desktop_state.c`（沿用既有 `user/tetris/tetris_logic.c` / `user/terminal/terminal_core.c` 的接线范式：`-I$(TESTS_DIR)/user/desktop`），新增 `test_desktop_state.elf` 并加入 `TEST_BINS`；`mk/components/run.mk` 的 `GUI_HOST_REGISTERED_CASES` 追加 `desktop-state`，并新增 `_test-gui-host-run-desktop-state`。
- [ ] **5. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=desktop-state` 与 `make PROFILE=x86_64-clang test-host`，全绿。
- [ ] **6. 提交** `feat(desktop): add pure surface state machine`。

---

### Task 4: 外壳表面构建与桌面主入口

**Files:** Create `user/desktop/desktop_shell.{c,h}`；Rewrite `user/desktop/desktop.c`；Delete `user/desktop/font8x16.h`；Modify `user/desktop/app.mk`。

**Interfaces:**

- `user/desktop/desktop_shell.h`（内部使用，可含 LVGL 头）：
  ```c
  typedef struct desktop_shell desktop_shell_t;

  desktop_shell_t *desktop_shell_create(lv_display_t *disp, const gui_config_t *cfg);
  void             desktop_shell_destroy(desktop_shell_t *shell);
  desktop_state_t  desktop_shell_state(const desktop_shell_t *shell);
  void             desktop_shell_request(desktop_shell_t *shell,
                                        desktop_intent_t intent, const char *app_id);
  void             desktop_shell_apply_pending(desktop_shell_t *shell); /* 执行迁移 */
  void             desktop_shell_tick(desktop_shell_t *shell);          /* 时钟刷新 */
  bool             desktop_shell_handle_key(desktop_shell_t *shell,
                                            const gui_key_event_t *ev);
  ```
- `user/desktop/desktop.c` 的 `main()`：创建 platform（`GUI_MODE_ONSCREEN`）→ 创建 input（`require_mouse = false`，允许无鼠标降级）→ 取 `lv_display_t` → `gui_theme_apply()` → 创建 shell → 进入主循环：`poll(gui_input_get_pollfds(...))` → `gui_input_dispatch()` → `gui_platform_step()` → `desktop_shell_apply_pending()` → `desktop_shell_tick()`。
- 退出语义（spec §8.1）：`GUI_STATUS_EAGAIN` 继续；`LOST_OWNERSHIP / DISPLAY_CHANGED / ERROR` 打印诊断后销毁平台并 `return 1`；正常路径 `return 0`。

- [ ] **1. 写 RED fixture（离屏像素契约）。** 新增 `user/desktop/desktop.c` 的 `--smoke` 模式（`GUI_MODE_OFFSCREEN`，640×480）：断言① `gui_platform_get_pixel(platform, 2, 4)` 等于 `gui_theme_colors()->surface`（状态栏之上是壁纸）；② 状态栏区域（y = 16）采样等于 `gui_theme_elevation_surface(GUI_THEME_ELEVATION_1)`；③ 启动器卡片中心采样等于 `gui_theme_elevation_surface(GUI_THEME_ELEVATION_2)`。断言失败写 stderr 并 `return 1`。此时 `desktop.c` 仍是旧的手绘实现 → 必然 RED。
- [ ] **2. 执行** `make PROFILE=x86_64-clang user` 后在 QEMU 离屏 smoke 下观察断言失败（记录失败信息）。**RED 必须是像素断言失败**，不得是编译错误。
- [ ] **3. 实现外壳表面构建。** `desktop_shell.c` 提供：`build_home()`（状态栏 + 欢迎卡片 + 启动器）、`build_drawer()`、`build_settings()`、`build_app()`。全部样式从 `gui_theme_*` 取值；**不得**出现裸颜色字面量。控件构造要点：状态栏 = 高 32px 的 `LV_THEME_OBJ` 子对象，左侧时间 label（`GUI_THEME_FONT_LABEL`）、中间系统名（`GUI_THEME_FONT_BODY`）、右侧指示区（`GUI_THEME_FONT_LABEL`）；欢迎卡片 = `elevation 2` 圆角卡片，内含 `display` 层级标题与 `body` 层级说明；启动器 = 底部水平容器（`elevation 2`），内含 4 个图标按钮（圆形色块 + 首字 label），每个图标下方带 `label` 层级文字。
- [ ] **4. 实现迁移执行。** `desktop_shell_apply_pending()`：取挂起的意图 → `desktop_state_next()` → 若 `INVALID` 则忽略并记录诊断；若目标状态 ≠ 当前状态 → **销毁当前表面的整棵 `lv_obj_t` 树**（`lv_obj_delete`）→ 重建目标表面 → 更新 `shell->state`。迁移**只**在这里发生；LVGL 控件回调只调用 `desktop_shell_request()` 投递意图。
- [ ] **5. 实现时钟刷新。** 用**一个** LVGL timer（周期 1000ms）在 `desktop_shell_tick()` 里刷新时间 label；断言定时器**只创建一次**（`shell->clock_timer` 非空则不重建）。时间格式由 `shell->settings.clock_24h` 决定（`strftime` 的 `%H:%M` 或 `%I:%M %p`）。
- [ ] **6. 实现键盘契约。** 注册原生按键回调 `gui_input_set_key_handler()`：处理 `ESC` → `DESKTOP_INTENT_ESC`；`Tab` 交给 LVGL（**不**消费）；其余键在终端应用激活时由应用消费（Task 6）。指针事件不注册回调（LVGL 自己处理）。
- [ ] **7. 删除旧资产并重写主入口。** 删除 `user/desktop/font8x16.h`；`user/desktop/desktop.c` 全面重写为「平台 + 输入 + 外壳」装配；`user/desktop/app.mk` 的 `APP_CFLAGS` 追加 `-I$(OS01_ROOT)/libgui/internal -I$(OS01_ROOT)/thirdpart -I$(OS01_ROOT)/thirdpart/lvgl -I$(OS01_ROOT)/config -DLV_CONF_INCLUDE_SIMPLE`，`APP_LDLIBS` 改为 `-lgui -lgfx`。
- [ ] **8. 执行** `make clean && make PROFILE=x86_64-clang` 与 QEMU 离屏 smoke；断言全绿；确认新 `desktop.elf` **不**再引用 `gfx_fill_rect` / `gfx_pixel` 等手绘 primitive（`llvm-nm` 抽查 `desktop.elf` 的未定义符号）。
- [ ] **9. 提交** `feat(desktop): rebuild desktop shell on LVGL with Material surfaces`。

---

### Task 5: 应用抽屉与基本设置表面

**Files:** Modify `user/desktop/desktop_shell.{c,h}`；Create `hosttests/cases/test_desktop_audit.c`（静态审计）。

**Interfaces:** `desktop_shell_t` 新增设置状态（在外壳自己的 C 变量里，不依赖控件存活）：`clock_24h`（bool，默认 true）、`wallpaper`（枚举：surface / container_low / container_high / container_highest，默认 surface）、`density`（枚举：comfortable / compact，默认 comfortable）。

- [ ] **1. 写 RED fixture（抽屉 / 设置的表面契约）。** 扩展 `--smoke`：注入 `DESKTOP_INTENT_OPEN_DRAWER` 后断言抽屉标题区域采样等于 `elevation 3` 表面色；注入 `DESKTOP_INTENT_OPEN_SETTINGS` 后断言设置表面采样等于 `elevation 2` 表面色；`DESKTOP_INTENT_BACK` 后断言回到 `HOME` 采样。
- [ ] **2. 执行** smoke，确认因抽屉 / 设置尚未实现而 RED（采样不匹配）。
- [ ] **3. 实现应用抽屉。** 全屏列表：标题（`headline` 层级，「应用」+ 总数）+ 可滚动容器 + 每项一条（图标色块 + `title` 层级名称 + `label` 层级一行说明 + 右侧 `label` 层级类型标签）。抽屉项与启动器共用**同一份应用注册表**（Task 6 引入），抽屉是全集、启动器是固定子集。
- [ ] **4. 实现基本设置。** 四个设置项（时钟格式 / 壁纸颜色 / 卡片密度 / 关于）用 `switch` 与 `list` 表达；值变化通过 `desktop_shell_request()` 投递的意图回到外壳执行，设置值写入 `shell->settings`，**立即**重建当前表面以生效。「关于」是只读展示（内核架构、LVGL 版本、构建 profile、显示模式与分辨率）。
- [ ] **5. 写静态审计 fixture。** `hosttests/cases/test_desktop_audit.c`：读取 `user/desktop/desktop_shell.c` 与 `user/desktop/desktop_apps.c` 源文件文本，断言 ① 不出现 `0x[0-9A-Fa-f]{6,8}` 形式的**颜色**字面量（允许 `0x` 开头的非颜色常量需逐一列白名单，白名单为空）；② 不出现 `gfx_fill_rect` / `gfx_pixel` / `gfx_rect` / `gfx_hline` / `gfx_vline` / `gfx_line`；③ 不出现 `open("/dev/keyboard"` / `open("/dev/mouse"`；④ 字号只经由 `gui_theme_font_size(...)` / `gui_theme_font_*` 获得（断言不出现裸的 `lv_obj_set_style_text_font(..., 12)` 这类字面量字号）。
- [ ] **6. 接入 focused 入口并执行。** `test_desktop_audit.elf` 加入 `hosttests/Makefile::TEST_BINS` 与 `mk/components/run.mk::GUI_HOST_REGISTERED_CASES`（CASE 名 `desktop-audit`）。执行 `make PROFILE=x86_64-clang test-gui-host CASE=desktop-audit`、`CASE=theme`、`CASE=desktop-state` 与 `make PROFILE=x86_64-clang test-host`，全绿。
- [ ] **7. 提交** `feat(desktop): add app drawer and basic settings surfaces`。

---

### Task 6: 四个内置应用

**Files:** Create `user/desktop/desktop_apps.{c,h}`；Modify `user/desktop/desktop_shell.{c,h}`、`user/desktop/desktop.c`。

**Interfaces:**

- `user/desktop/desktop_apps.h`：
  ```c
  typedef enum { DESKTOP_APP_FILES = 0, DESKTOP_APP_TERMINAL,
                 DESKTOP_APP_SYSTEM, DESKTOP_APP_SETTINGS,
                 DESKTOP_APP_COUNT } desktop_app_id_t;

  typedef struct {
      desktop_app_id_t id;
      const char      *name;        /* 显示名（中文） */
      const char      *desc;        /* 一行说明 */
      const char      *glyph;       /* 图标首字 */
      uint32_t         accent;      /* 图标底色（必须取自 gui_theme_colors()） */
      bool             in_launcher; /* 是否出现在启动器 */
  } desktop_app_info_t;

  const desktop_app_info_t *desktop_app_info(desktop_app_id_t id);
  uint32_t                   desktop_app_count(void);
  /* 构造应用表面；返回根对象（失败返回 NULL）。 */
  lv_obj_t *desktop_app_build(lv_display_t *disp, desktop_app_id_t id,
                              desktop_shell_t *shell);
  /* 应用内部请求返回主屏（终端等应用用）。 */
  void desktop_app_request_back(desktop_shell_t *shell);
  /* 终端消费键入字符；返回 true 表示已消费。 */
  bool desktop_app_handle_key(desktop_shell_t *shell, const gui_app_key_t *ev);
  ```
  `desktop_app_handle_key()` 的参数用 libgui 公共 `gui_key_event_t`（`libgui/include/gui/gui_input.h`），不引入新类型。

- 应用注册表（4 项，`accent` 全部取自 `gui_theme_colors()`）：`files`（名称「文件」，说明「只读浏览根文件系统」，glyph「文」，accent `primary`，进启动器）、`terminal`（名称「终端」，说明「壳内命令台」，glyph「>」，accent `secondary`，进启动器）、`system`（名称「系统」，说明「内核与运行时信息」，glyph「系」，accent `tertiary`，**不进**启动器）、`settings`（名称「设置」，说明「时钟、壁纸与密度」，glyph「设」，accent `success`，进启动器）。

- [ ] **1. 写 RED fixture。** 扩展 `--smoke`：遍历 4 个应用，对每个应用执行 `DESKTOP_INTENT_OPEN_APP(id)`，断言① 状态变为 `APP`；② 应用表面根对象非空；③ 应用表面根对象覆盖整个显示区（`lv_obj_get_width/height` 等于显示宽高）；④ `DESKTOP_INTENT_BACK` 后状态回到 `HOME`。同时断言终端应用能处理一次 `codepoint = 'a'`（行内容增加 1 个字符）。
- [ ] **2. 执行** smoke，确认因应用未实现而 RED。
- [ ] **3. 实现注册表与文件应用。** `desktop_apps.c` 定义 `static const desktop_app_info_t g_apps[DESKTOP_APP_COUNT]`（上表逐字取值）。文件应用：`opendir` / `readdir` / `closedir` 浏览，初始 `/`，目录在前、文件在后、各自按名称排序；每行显示名称（`body` 层级）+ 类型标签（`label` 层级）；回车 / 双击进入子目录；顶栏返回键先回父目录，父目录为 `/` 时才回主屏；顶栏副标题注明「只读」。**不得**提供写入 / 删除 / 新建入口。
- [ ] **4. 实现终端应用。** 多行输出区（`body` 层级）+ 提示符 `OS01 $ ` + 当前输入行；可打印 ASCII（0x20–0x7E）由原生按键回调消费并回显；`Enter` 执行；`Backspace` 退格；`↑` / `↓` 翻历史（最近 32 条）。内建命令：`help`、`ls [path]`、`cat <file>`、`uname`、`date`、`clear`；未知命令回 `command not found`。**不 fork 子进程**。
- [ ] **5. 实现系统应用。** 只读展示：内核架构（构建期宏）、构建 profile、显示模式与分辨率、像素格式、键盘 / 鼠标是否存在、LVGL 版本、LVGL 内存池总量与占用率（`lv_mem_monitor`）。
- [ ] **6. 实现设置应用。** 与 Task 5 的设置表面**共用**同一份 `shell->settings`（设置应用是设置表面的**同一份实现**，不是两套）；「关于」项展示与系统应用一致的信息。
- [ ] **7. 修复终端消费键后的焦点状态。** 终端激活时，原生按键回调消费可打印字符并 `return true`；被消费的 `UP` 由 libgui 强制释放 LVGL 侧活动键（GUI-2 既有行为）。断言：终端退出后，焦点组仍可 `Tab` 导航到启动器第一个图标（不卡死、不残留已销毁对象）。
- [ ] **8. 执行** `make PROFILE=x86_64-clang user` + smoke 全绿；`make PROFILE=x86_64-clang test-host` 全绿。
- [ ] **9. 提交** `feat(desktop): add files terminal system and settings apps`。

---

### Task 7: QEMU 端到端门禁与文档收口

**Files:** Create `qemutests/gui_desktop_test.py`；Modify `qemutests/test_gui_runner.py`、`mk/components/run.mk`、`docs/roadmap.md`、`docs/gui/gui.md`、`docs/changelog.md`。

**Interfaces:** 新 QEMU suite `gui-desktop`，沿用既有 `gui_test.py` / `gui_input_test.py` 的 QMP 驱动范式（screendump 取像素、QMP 键鼠注入）。

- [ ] **1. 写 RED fixture（runner 契约）。** 在 `qemutests/test_gui_runner.py` 追加新 suite 的**假 runner** 用例：未知 suite 名必须非零失败；`gui-desktop` suite 必须被登记。
- [ ] **2. 执行** `python3 -m unittest qemutests.test_gui_runner`（或仓库既有 runner 测试入口），确认因 suite 未登记而 RED。
- [ ] **3. 实现 `qemutests/gui_desktop_test.py`。** 五项检查（spec §9.3）：① 桌面启动后 screendump 有有效像素（主屏壁纸色命中）、进程退出码 0；② 离屏 `--smoke` 像素断言（状态栏 / 启动器表面色命中）；③ 注入点击启动器图标 → 画面变化（应用表面色命中）；④ 注入 `ESC` → 回到主屏（初始采样命中）；⑤ 桌面退出后终端 fallback 恢复（显示所有权协议回归）。
- [ ] **4. 接线 suite。** `mk/components/run.mk` 的 `SUITE` 分派新增 `gui-desktop`；`qemutests/test_gui_runner.py` 登记新 suite。
- [ ] **5. 执行端到端。** `make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop` 通过；`make PROFILE=x86_64-clang test-qemu SUITE=gui`（既有 GUI 套件）回归通过；`make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 回归通过；`make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` 通过；`make PROFILE=x86_64-clang test-static` 通过。
- [ ] **6. 文档收口。** `docs/roadmap.md` 的 GUI-3 行改为已完成并写入交付摘要（日期、提交区间、验证入口）；`docs/gui/gui.md` 新增「GUI-3 Material 风格桌面」章节（设计令牌表、表面与状态机、启动 / 退出 / 全屏切换语义、**明确不做窗口系统**的声明）；`docs/changelog.md` 追加当日条目。
- [ ] **7. 提交** `test(gui): add desktop QEMU gate and close GUI-3 docs`。

---

## Spec 覆盖自检

| Spec 范围 | 实施与验证任务 |
|---|---|
| §1.1 完成条件 1（设计系统落地） | Tasks 1–2（令牌表 + 主题落地）+ Task 5（静态审计钉死「不得绕过令牌」） |
| §1.1 完成条件 2（外壳四件套） | Task 4（状态栏 / 启动器）+ Task 5（抽屉 / 设置） |
| §1.1 完成条件 3（应用语义明确） | §6 全节；Task 3（状态机断言）+ Task 6（应用启动 / 退出） |
| §1.1 完成条件 4（不假装窗口系统） | Task 4（删除手绘窗口装饰）+ §6.4 明确不做 |
| §1.1 完成条件 5（证据可复跑） | Tasks 1/3/5（宿主）+ Task 7（QEMU） |
| §1.2 不包含 | 全程不实现（深色主题 / 外部进程 / 窗口系统 / 资源加载） |
| §1.3 取舍 1–8 | Tasks 1–6 的具体实现选择 |
| §2.2 必须补齐的边界 1–6 | Tasks 2（1、2）、1（3）、3（4）、4（5）、4（6） |
| §3 分层 / 所有权 / 文件清单 | Task 1–6 的 Files 块；Task 4 实现所有权规则 |
| §4.1–4.5 令牌与主题 | Task 1（4.1–4.4 取值）+ Task 2（4.5 API 与主题落地） |
| §5.1–5.5 外壳 | Task 3（5.1 状态机）+ Task 4（5.2 状态栏 / 5.3 启动器）+ Task 5（5.4 抽屉 / 5.5 设置） |
| §6.1–6.5 应用语义 | Task 3（状态机）+ Task 6（应用实现）+ Task 4（键鼠契约） |
| §7.1–7.4 四个内置应用 | Task 6 |
| §8.1 错误语义 | Task 4（主入口退出语义）+ Task 6（命令返回错误信息） |
| §8.2 资源预算 | Task 2（字体体积审计）+ Task 4（内存余量检查） |
| §8.3 构建约束 | Task 1（libgui 打包）+ Task 2（公共头安装 / 字体启用 / `make clean`）+ Task 4（app.mk 包含路径） |
| §9.1 宿主令牌测试 | Task 1–2 |
| §9.2 宿主状态机测试 | Task 3 |
| §9.3 QEMU E2E | Task 7 |
| §9.4 回归入口 | Task 7 |
| §10 交付分期 | P1 = Tasks 1–2，P2 = Tasks 3–4，P3 = Task 6，P4 = Task 5 + Task 7 |

## Plan 子 agent 评审修订记录

（待评审后填写。）
