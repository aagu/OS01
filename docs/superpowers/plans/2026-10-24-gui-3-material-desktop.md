# GUI-3 Material Desktop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 LVGL 重写 `user/desktop`，交付一个由单一 Material 令牌表驱动的单进程桌面外壳（状态栏 / 启动器 / 应用抽屉 / 基本设置 + 4 个壳内应用），并把「应用启动 / 退出 / 全屏切换」的语义显式定义与实现，不绘制任何窗口装饰。

**Architecture:** 设计系统下沉到 `libgui`（公共 `gui/gui_theme.h` 纯 C 令牌表 + 内部 `theme_apply.c` 落地 LVGL 主题）。**公共 API 保持 LVGL 无关**——sysroot 的任何已发布头不得出现 LVGL 符号（`qemutests/gui_build_audit.py` 的硬约束），所以主题由 `gui_platform_create()` 自动套用，业务应用用 `lv_display_get_default()` 取 display。外壳的界面状态机抽成**纯 C 无 LVGL 依赖**的 `user/desktop/desktop_state.c`，可宿主穷举验证；LVGL 控件构造集中在 `desktop_shell.c`，应用注册表是纯 C 数据 `desktop_registry.c`，4 个应用界面在 `desktop_apps.c`。所有应用都是**壳内应用**：应用表面占满整个显示区，退出即销毁并重建主屏。

**Tech Stack:** C11、OS01 libc（`opendir/readdir`、`localtime/strftime`）、LVGL v9.5.0（`thirdpart/lvgl` submodule，v9.3.0-992-g85aa60d18）、libgui、libgfx、宿主 hosttests、QEMU/QMP。

**Spec:** [2026-10-24-gui-3-material-desktop-design.md](../specs/2026-10-24-gui-3-material-desktop-design.md)。

**Plan 状态：** Revision 2。Revision 1 经独立评审后重构了 4 处架构（公共 API 的 LVGL 隔离、主题生命周期、宿主可测的外壳、应用注册表前置），并补齐了评审指出的全部断言缺口。

**基线与分支：** 分支 `feature/gui-3-material-desktop`，worktree `/home/aagu/OS01/.worktrees/gui-3-material-desktop`，起点 `d0153dc3`（`master`）。Task 1 已完成于 `0040715a`；Task 2 已完成于本计划的 Task 2 提交。

## Global Constraints

- **单进程**：桌面是唯一进程；不 fork / exec 子进程，不引入显示所有权交接与输入租约交接（spec §1.3 取舍 1）。
- **仅浅色 Material**：一套浅色色彩角色表；不做深色主题、不做主题切换项（spec §1.3 取舍 2）。
- **不画窗口装饰**：无标题栏边框、无最小化 / 最大化 / 关闭按钮、无可拖动边框、无 Z-order；旧 `user/desktop/desktop.c` 里的「欢迎窗口 + 三按钮」必须删除（spec §6.4）。
- **公共 API 不含 LVGL**：任何**已发布**头文件不得出现 `lv_*` 符号。新增公共头必须同步 `qemutests/gui_build_audit.py` 的 `GUI_PUBLIC_HEADERS` / `required_files` / `GUI_STAGED_FILES` 三处清单，并核对 `libgui.a` 的导出符号清单。
- **令牌唯一来源**：颜色 / 字号 / 间距 / 圆角 / elevation 一律取自 `gui_theme_*` API；桌面代码里不得出现裸颜色字面量与裸字号。
- **主题与平台同生共死**：`gui_platform_destroy()` 的顺序必须是「先删 display → 再拆主题 → 最后 `lv_deinit()`」。反序会造成 use-after-free（屏幕对象上仍挂着已释放的样式）。
- **输入只走 GUI-2 通道**：桌面不得 `open("/dev/keyboard")` / `open("/dev/mouse")`；鼠标坐标、焦点、重复、滚轮一律由 libgui 提供。
- **字体层级**：`config/lv_conf.h` 必须启用 `LV_FONT_MONTSERRAT_12 / 14 / 16 / 20 / 24`（原本只有 14 / 16）；改 `lv_conf.h` 后必须 `make clean`。
- **LVGL 内存池**：4 MiB（`LV_MEM_SIZE` 不变）；构造表面前检查余量，不足则诊断退出，不静默半初始化。
- **宿主对象隔离**：宿主测试只用宿主 libc / 宿主 LVGL 对象，不得混用目标库或目标 libc；sysroot 仍由单一发布器写入。
- **颜色比较口径**：LVGL 内部颜色值是 `0xFFRRGGBB`，令牌表是 `0x00RRGGBB`（与 `gui_platform_get_pixel()` 一致）。比较时必须掩掉最高字节。
- **不扩张到**：GUI-4 字体与资源、GUI-5 窗口服务器、GUI-6 窗口化 terminal、IME、USB HID、aarch64 用户态 GUI。

## Review Focus

以下八项是实施时最容易遗漏的故障输入；对应断言已归入任务，不以「后续人工看看」代替：

1. **令牌被绕过** —— 桌面里出现裸颜色 / 裸字号，令牌表形同虚设 → Task 5 加**静态审计**（扫描 `user/desktop/*.c` 的颜色字面量）。
2. **迁移在 LVGL 回调里发生** —— 回调期间销毁控件树是未定义行为 → Task 4 的外壳只投递意图，迁移在主循环执行。
3. **迁移后残留旧表面** —— 旧控件留在焦点组里，Tab 会跳进已销毁的应用 → Task 4 迁移前整体销毁 + Task 3 状态机断言 `APP + OPEN_APP` 不允许。
4. **终端消费按键后焦点卡死** —— 消费掉的 DOWN 没进 LVGL，但 UP 仍须释放 LVGL 侧活动键 → Task 6 断言「终端退出后焦点组可继续导航」。
5. **状态栏时钟定时器雪崩** —— 每个 tick 新建定时器 → Task 4 断言定时器只创建一次。
6. **旧手绘资产残留** —— `font8x16.h` / 旧的 `gfx_*` 手绘路径仍在编译 → Task 4 删除文件 + Task 5 静态审计断言桌面不再调用 `gfx_*` primitive。
7. **发布头被污染 / 构建审计破口** —— 新增公共头却没同步审计清单，`make test-static` 直接失败 → Task 2 同步三处清单并跑 `make test-static`。
8. **主题生命周期 use-after-free** —— 主题与 display 销毁顺序颠倒，屏幕删除时读到已释放样式（偶发堆破坏） → Task 2 固定销毁顺序并用 15 次连跑验证不崩。

## 工作方式、验证入口与文件分工

先读本 plan、spec 和根 `AGENTS.md`；存在 `.codegraph/` 时先用 `codegraph node/explore` 定位代码，未索引的文档 / 文件再直接读。执行前用 `superpowers:using-git-worktrees` 检查隔离状态；不覆盖别人的修改（`master` 可能仍在前进，合并前先 rebase）。

每个任务都先写断言跑出**预期失败**（RED），再实现跑绿（GREEN），最后提交该任务列出的文件。RED 可以是缺符号 / 缺声明，但不得是无关环境失败，必须记录失败原因。已有行为回归是 GREEN 门禁的一部分。可重复故障用 `superpowers:systematic-debugging`；不靠增大超时、屏蔽断言或修改验收条件「修复」。

### 宿主 focused 入口

扩展既有 `make PROFILE=x86_64-clang test-gui-host CASE=...`，不新建根测试 bucket：

| CASE | 二进制与引入任务 |
|---|---|
| `theme` | `test_gui_theme.elf`，Task 1（已完成） |
| `theme-apply` | `test_gui_theme_apply.elf`，Task 2（已完成） |
| `desktop-state` | `test_desktop_state.elf`，Task 3 |
| `desktop-shell` | `test_desktop_shell.elf`，Task 4 |
| `desktop-audit` | `test_desktop_audit.elf`，Task 5 |

各 CASE 的 hosttests 子 make 目标命名 `test-gui-<case>`，由 root `os01_submake` 转发；不得从 shell 直接 `make -C hosttests`。每新增 binary 同时加入 `hosttests/Makefile::TEST_BINS`、`mk/components/run.mk::GUI_HOST_REGISTERED_CASES` 及对应的 `_test-gui-host-run-<case>` 目标。未知 CASE 在构建前非零失败。

### 文件责任图

| 单元 | 文件与责任 |
|---|---|
| 设计令牌 | `libgui/include/gui/gui_theme.h` + `libgui/src/theme.c`：色彩角色表、字号 / 间距 / 圆角 / elevation 取值 API（纯 C，无 LVGL） |
| 主题落地 | `libgui/src/theme_apply.c` + `libgui/internal/gui_theme_private.h`：令牌表 → LVGL 主题；主题与样式随平台生灭 |
| 公共 LVGL 访问 | `libgui/internal/gui_lvgl_private.h`（既有，保持私有）；业务应用用 `lv_display_get_default()` |
| 界面状态机 | `user/desktop/desktop_state.{c,h}`：**纯 C**，无 LVGL 依赖；状态 + 意图 + 迁移纯函数 |
| 应用注册表 | `user/desktop/desktop_registry.{c,h}`：**纯 C 数据**，应用 id / 名称 / 说明 / 图标字 / 强调色 / 是否进启动器 |
| 外壳 UI | `user/desktop/desktop_shell.{c,h}`：表面构建（状态栏 / 主屏 / 启动器 / 抽屉 / 设置）、令牌化样式辅助 |
| 内置应用 | `user/desktop/desktop_apps.{c,h}`：4 个应用界面（文件 / 终端 / 系统 / 设置） |
| 主入口 | `user/desktop/desktop.c`：平台 + 输入装配、主循环、意图执行、退出语义 |
| 构建 / 发布 | `libgui/Makefile`、`config/lv_conf.h`、`user/desktop/app.mk`、`qemutests/gui_build_audit.py` |
| 宿主测试 | `hosttests/cases/test_gui_theme.c`、`test_gui_theme_apply.c`、`test_desktop_state.c`、`test_desktop_shell.c`、`test_desktop_audit.c`；`hosttests/Makefile`、`mk/components/run.mk` |
| 端到端 | `qemutests/gui_desktop_test.py` + `qemutests/run_test.py` 分派 + `mk/components/run.mk` 的 suite 分派与 per-suite 变量 |
| 文档 | `docs/roadmap.md`、`docs/gui/gui.md`、`docs/changelog.md` |

---

### Task 1: Material 令牌表与公共 API ✅ 已完成（`0040715a`）

**产出：** `libgui/include/gui/gui_theme.h`、`libgui/src/theme.c`、`hosttests/cases/test_gui_theme.c`；`hosttests/Makefile` 与 `mk/components/run.mk` 注册 `CASE=theme`。

**保留为回归证据：** `make PROFILE=x86_64-clang test-gui-host CASE=theme` 必须持续绿。令牌表的 8 组断言见 spec §9.1。

---

### Task 2: LVGL 主题落地、字体层级与发布审计同步

**Files:** Create `libgui/src/theme_apply.c`、`libgui/internal/gui_theme_private.h`、`hosttests/cases/test_gui_theme_apply.c`；Modify `libgui/src/platform.c`、`libgui/internal/gui_platform_private.h`、`libgui/Makefile`、`config/lv_conf.h`、`hosttests/Makefile`、`mk/components/run.mk`、`qemutests/gui_build_audit.py`。

**Interfaces:**

- 公共（发布）：`libgui/include/gui/gui_theme.h` —— **纯 C**，只有令牌表；**不含** `gui_theme_apply`、**不含**任何 `lv_*`。
- 内部（不发布）：`libgui/internal/gui_theme_private.h`
  ```c
  lv_theme_t *gui_theme_create(lv_display_t *display);
  void        gui_theme_destroy(lv_theme_t *theme);
  ```
- `gui_platform_create()`：建好 display 后 `platform->theme = gui_theme_create(platform->disp);`，失败回滚（`ENOMEM`）。
- `gui_platform_destroy()` 顺序固定为：`gui_input_destroy` → `lv_display_delete` → `gui_theme_destroy` → `lv_deinit()`。

- [x] **1. RED fixture。** `hosttests/cases/test_gui_theme_apply.c`：链接真实 `theme.c` + `theme_apply.c` + `platform.c` + `$(GUI_LVGL_ARCHIVE)`（复用 `test_gui_lvgl_integration.elf` 的链接形状：`$(GUI_INTEGRATION_DIR)/gui_test_runtime.o` / `platform.o` / `tick.o` / `gfx_client_policy.o` / `$(GUI_INPUT_UNIT_OBJS)` / `$(GUI_LVGL_ARCHIVE)`）。断言见 spec §9.1 的第 9–16 条（含 hover / pressed / disabled / focus / checked 五类状态）。
- [x] **2. 跑出 RED。** 链接缺 `gui_theme_create` / `gui_theme_destroy` 符号而失败（记录失败信息）。
- [x] **3. 实现 `theme_apply.c`。** 自建 `lv_theme_t`（`lv_theme_create()` + `lv_theme_set_apply_cb()`），**不**以 LVGL 默认主题为父主题。按控件类套用令牌：`OBJ`（elevation 2 表面 + radius_lg + 1px outline_variant 边框 + pad md）、`LABEL`（on_surface + body 字号）、`BUTTON`（primary + on_primary + radius_md + title 字号；hover → secondary，pressed → primary_container/on_primary_container，disabled → surface_variant/on_surface_variant）、`SWITCH` / `CHECKBOX` / `SLIDER` / `BAR` / `LIST` / `SCROLLBAR` / `TEXTAREA`。焦点环用 `LV_STATE_FOCUSED` 与 `LV_STATE_FOCUS_KEY` 两个选择器追加 2px `primary` 轮廓。**屏幕必须显式套用主题**（LVGL 的 `lv_display_set_theme()` 只在 4 屏 display 上自动重套）。
- [x] **4. 主题与样式随平台生灭。** 样式块用 `lv_calloc` 分配、挂在 `theme->user_data` 上；`gui_theme_destroy()` 先 `lv_style_reset()` 每个样式（释放 LVGL 池里的值缓冲）再 `lv_free()` 样式块，最后 `lv_theme_delete()`。样式**不得**放文件静态区：`lv_deinit()` 会释放整个 LVGL 池，静态样式会变成悬垂指针（症状是第二次建平台时颜色变成别的数据）。
- [x] **5. 接入平台生命周期。** `gui_platform_private.h` 的 `gui_platform` 增加 `lv_theme_t *theme;`；`platform.c` 创建时套用、销毁时按固定顺序拆除。
- [x] **6. 启用字体层级。** `config/lv_conf.h` 启用 `LV_FONT_MONTSERRAT_12 / 20 / 24`（14 / 16 保持 1）。**必须 `make clean`** 后重编。审计 `liblvgl.a` 体积增长并写进提交说明。
- [x] **7. 同步发布审计。** `qemutests/gui_build_audit.py`：`GUI_PUBLIC_HEADERS` 加 `gui_theme.h`；`required_files` 加 `usr/include/gui/gui_theme.h`；`GUI_STAGED_FILES` 加同项；`required_symbols` 加 8 个 `gui_theme_*` 符号。
- [x] **8. 接线宿主测试。** `hosttests/Makefile`：新增 `test_gui_theme_apply.elf`（含 `theme.o` / `theme_apply.o`）并加入 `TEST_BINS`；**所有**链接真实 `platform.o` 的宿主测试（deterministic / integration / input / input_lvgl 四组）都要加 `$(GUI_THEME_OBJS)`，否则链接缺符号。`mk/components/run.mk` 注册 `theme-apply`。
- [x] **9. 验证。** `make clean && make` 绿；`CASE=theme` / `CASE=theme-apply` 绿；`make test-host` 绿；`make test-static` 绿（发布头清单 / manifest / 导出符号三项都过）。
- [x] **10. 偶发崩溃回归。** 销毁顺序写反会造成 use-after-free（屏幕对象上仍挂着已释放的样式，症状是偶发 SIGSEGV）。用 `for i in $(seq 1 15); do ./build/x86_64-clang/host-test/test_gui_input_lvgl.elf; done` 验证 15 次连跑零崩溃。
- [x] **11. 提交** `feat(gui): apply Material theme to LVGL and publish token table`。

---

### Task 3: 外壳界面状态机（纯 C，可宿主测试）

**Files:** Create `user/desktop/desktop_state.{c,h}`、`hosttests/cases/test_desktop_state.c`；Modify `hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:** `user/desktop/desktop_state.h` **不包含任何 LVGL 头**，只含 `<stdbool.h>`：

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

允许的迁移（**唯一**集合，其余一律 `DESKTOP_STATE_INVALID`）：

| 当前 | 意图 | 结果 |
|------|------|------|
| HOME | `OPEN_DRAWER` | DRAWER |
| HOME | `OPEN_SETTINGS` | SETTINGS |
| HOME | `OPEN_APP(id)` | APP（`app_id` 非空且非空串） |
| HOME | `BACK` / `ESC` | HOME |
| DRAWER / SETTINGS / APP | `BACK` / `ESC` | HOME |
| APP | `OPEN_APP(id)` | **INVALID**（应用不允许叠加） |

**当前应用标识不由状态机携带**：`desktop_state_t` 只有四个表面状态，*哪个* 应用处于 `APP` 由外壳记在 `shell->current_app`。

- [ ] **1. RED fixture。** `hosttests/cases/test_desktop_state.c`：表驱动穷举 (状态 × 意图) 全组合；断言 spec §9.2 的第 1–7 条。宿主测试**编译真实的** `user/desktop/desktop_state.c`（沿用 `user/tetris/tetris_logic.c` / `user/terminal/terminal_core.c` 的接线范式：`-I$(TESTS_DIR)/user/desktop`）。
- [ ] **2. 跑出 RED。** 链接缺 `desktop_state_*` 符号而失败（记录失败信息）。
- [ ] **3. 实现状态机。** 纯 C：不读全局状态、不调 libc；`desktop_state_name()` 用静态字符串表；非法枚举返回 `DESKTOP_STATE_INVALID`。
- [ ] **4. 接线。** `hosttests/Makefile` 新增 `test_desktop_state.elf`（编译 `hosttests/cases/test_desktop_state.c` + `user/desktop/desktop_state.c`）并加入 `TEST_BINS`；`mk/components/run.mk` 注册 `desktop-state` 与 `_test-gui-host-run-desktop-state`。
- [ ] **5. 验证。** `make PROFILE=x86_64-clang test-gui-host CASE=desktop-state` 与 `make PROFILE=x86_64-clang test-host` 全绿。
- [ ] **6. 提交** `feat(desktop): add pure surface state machine`。

---

### Task 4: 应用注册表、外壳表面与桌面主入口

**Files:** Create `user/desktop/desktop_registry.{c,h}`、`user/desktop/desktop_shell.{c,h}`、`hosttests/cases/test_desktop_shell.c`；Rewrite `user/desktop/desktop.c`；Delete `user/desktop/font8x16.h`；Modify `user/desktop/app.mk`、`hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:**

- `user/desktop/desktop_registry.h`（**纯 C**，无 LVGL）：
  ```c
  typedef enum { DESKTOP_APP_FILES = 0, DESKTOP_APP_TERMINAL,
                 DESKTOP_APP_SYSTEM, DESKTOP_APP_SETTINGS,
                 DESKTOP_APP_COUNT } desktop_app_id_t;

  typedef struct {
      desktop_app_id_t id;
      const char      *name;     /* 显示名 */
      const char      *desc;     /* 一行说明 */
      const char      *glyph;    /* 图标首字 */
      uint32_t         accent;   /* 图标底色，必须取自 gui_theme_colors() */
      bool             in_launcher;
  } desktop_app_info_t;

  const desktop_app_info_t *desktop_app_info(desktop_app_id_t id);
  uint32_t                   desktop_app_count(void);
  ```
  注册表 4 项（**accent 全部取自令牌表**）：`files`（文件 / 只读浏览根文件系统 / 文 / primary / 进启动器）、`terminal`（终端 / 壳内命令台 / `>` / secondary / 进启动器）、`system`（系统 / 内核与运行时信息 / 系 / tertiary / **不进**启动器）、`settings`（设置 / 时钟、壁纸与密度 / 设 / success / 进启动器）。启动器 = `in_launcher` 的 3 个，抽屉 = 全集 4 个。

- `user/desktop/desktop_shell.h`（含 LVGL 头）：
  ```c
  typedef struct desktop_shell desktop_shell_t;

  desktop_shell_t *desktop_shell_create(lv_display_t *disp);
  void             desktop_shell_destroy(desktop_shell_t *shell);
  desktop_state_t  desktop_shell_state(const desktop_shell_t *shell);
  void             desktop_shell_request(desktop_shell_t *shell,
                                        desktop_intent_t intent, const char *app_id);
  void             desktop_shell_apply_pending(desktop_shell_t *shell);
  void             desktop_shell_tick(desktop_shell_t *shell);
  bool             desktop_shell_handle_key(desktop_shell_t *shell,
                                            const gui_key_event_t *ev);
  ```
  `desktop_shell_create()` **不接收** `gui_config_t`：显示尺寸用 `lv_display_get_width/height` 取。

- `user/desktop/desktop.c` 的 `main()`：`gui_platform_create()`（`GUI_MODE_ONSCREEN`）→ `gui_input_create(..., require_mouse = false, ...)`（允许无鼠标降级）→ `lv_display_get_default()` 取 display → `desktop_shell_create(disp)` → 主循环：`poll(gui_input_get_pollfds(...))` → `gui_input_dispatch()` → `gui_platform_step()` → `desktop_shell_apply_pending()` → `desktop_shell_tick()`。

- [ ] **1. RED fixture（宿主像素契约）。** `hosttests/cases/test_desktop_shell.c`：链接真实 `desktop_shell.c` + `desktop_registry.c` + `desktop_state.c` + 真实 LVGL + 真实离屏 platform（`gui_platform_get_pixel()` 读回像素）。断言 spec §9.2 的第 8–11 条，采样点必须**钉死坐标**：
  - 壁纸区取 `(2, 4)`（状态栏之下、启动器之上）= `surface`；
  - 状态栏区取 `(2, 12)` = `elevation 1` 表面；
  - 启动器卡片取**图标之间**的空隙（例如第 1、2 个图标的中点）= `elevation 2` 表面——不得落在图标色块上；
  - 抽屉表面取 `(2, 4)` = `elevation 3` 表面；设置表面取 `(2, 4)` = `elevation 2` 表面；
  - 应用表面根对象宽高 = display 宽高。
- [ ] **2. 跑出 RED。** 链接缺 `desktop_shell_*` 符号而失败（记录失败信息）。
- [ ] **3. 实现注册表。** `desktop_registry.c` 只放静态数据与两个取值函数，**不含 LVGL**、不含裸颜色字面量（accent 一律 `gui_theme_colors()->primary` 等）。
- [ ] **4. 实现外壳表面构建。** `desktop_shell.c`：状态栏（高 32px，left 时间 / 中系统名 / 右指示区）、主屏（欢迎卡片 + 启动器）、抽屉（可滚动应用列表）、设置（四个设置项）。全部样式取自 `gui_theme_*`；**不得**出现裸颜色字面量或裸字号。启动器图标 = 圆形色块（accent 令牌）+ 首字 + `label` 层级文字标签。
- [ ] **5. 实现迁移执行。** `desktop_shell_apply_pending()`：取挂起意图 → `desktop_state_next()` → `INVALID` 则忽略并记诊断 → 否则**销毁当前表面的整棵 `lv_obj_t` 树**并重建目标表面 → 更新 `shell->state`。迁移**只**在这里发生；LVGL 控件回调只调用 `desktop_shell_request()` 投递意图。
- [ ] **6. 实现时钟刷新。** 用**一个** LVGL timer（周期 1000ms）在 `desktop_shell_tick()` 里刷新时间 label；`shell->clock_timer` 非空则不重建（防定时器雪崩）。格式由 `shell->settings.clock_24h` 决定（`strftime` 的 `%H:%M` 或 `%I:%M %p`）。
- [ ] **7. 实现键盘契约。** 注册原生按键回调 `gui_input_set_key_handler()`：`ESC` → `DESKTOP_INTENT_ESC`；`Tab` 交给 LVGL（**不**消费）；其余键在终端应用激活时由应用消费（Task 6）。
- [ ] **8. 删除旧资产并重写主入口。** 删 `user/desktop/font8x16.h`；`desktop.c` 全面重写；`user/desktop/app.mk` 的 `APP_CFLAGS` 追加 `-I$(OS01_ROOT)/thirdpart -I$(OS01_ROOT)/thirdpart/lvgl -I$(OS01_ROOT)/config -DLV_CONF_INCLUDE_SIMPLE`（**不**加 `-I$(OS01_ROOT)/libgui/internal`：公共 API 已不需要私有头），`APP_LDLIBS` 改为 `-lgui -lgfx`。
- [ ] **9. 接线宿主测试。** `hosttests/Makefile` 新增 `test_desktop_shell.elf`（编译 `hosttests/cases/test_desktop_shell.c` + `user/desktop/desktop_shell.c` + `user/desktop/desktop_registry.c` + `user/desktop/desktop_state.c` + `$(GUI_INTEGRATION_DIR)/platform.o` + `$(GUI_THEME_OBJS)` + `$(GUI_LVGL_ARCHIVE)`）并加入 `TEST_BINS`；`mk/components/run.mk` 注册 `desktop-shell`。
- [ ] **10. 验证。** `make PROFILE=x86_64-clang user` 绿；`CASE=desktop-shell` 绿；`make test-host` 绿；`llvm-nm` 抽查新 `desktop.elf` 不再引用 `gfx_fill_rect` / `gfx_pixel` 等手绘 primitive。
- [ ] **11. 提交** `feat(desktop): add app registry and LVGL shell surfaces`。

---

### Task 5: 应用抽屉、基本设置与静态审计

**Files:** Modify `user/desktop/desktop_shell.{c,h}`；Create `hosttests/cases/test_desktop_audit.c`。

**Interfaces:** `desktop_shell_t` 新增设置状态（存外壳自己的 C 变量，不依赖控件存活）：`clock_24h`（bool，默认 true）、`wallpaper`（枚举 surface / container_low / container_high / container_highest，默认 surface）、`density`（枚举 comfortable / compact，默认 comfortable）。

- [ ] **1. RED fixture（表面契约）。** 扩展 `test_desktop_shell.c`：注入 `OPEN_DRAWER` 后断言抽屉标题区采样 = `elevation 3` 表面；注入 `OPEN_SETTINGS` 后断言设置表面采样 = `elevation 2` 表面；`BACK` 后回到 `HOME` 采样。
- [ ] **2. 跑出 RED。** 采样不匹配（抽屉 / 设置尚未实现）。
- [ ] **3. 实现应用抽屉。** 全屏列表：标题（`headline` 层级，「应用」+ 总数）+ 可滚动容器 + 每项一条（图标色块 + `title` 层级名称 + `label` 层级说明 + 右侧 `label` 层级类型标签）。抽屉项与启动器**共用同一份注册表**；抽屉是全集、启动器是 `in_launcher` 子集。
- [ ] **4. 实现基本设置。** 四个设置项（时钟格式 / 壁纸颜色 / 卡片密度 / 关于）用 `switch` 与 `list` 表达；值变化通过 `desktop_shell_request()` 投递的意图回到外壳执行，写入 `shell->settings` 后**重建当前表面**以立即生效。「关于」是只读展示（内核架构、LVGL 版本、构建 profile、显示模式与分辨率）。
- [ ] **5. 写静态审计 fixture。** `hosttests/cases/test_desktop_audit.c`：读取 `user/desktop/desktop_shell.c`、`user/desktop/desktop_apps.c`、`user/desktop/desktop_registry.c` 源文件文本并断言：
  ① 不出现 `0x[0-9A-Fa-f]{6,8}` 形式的颜色字面量；
  ② 不出现 `gfx_fill_rect` / `gfx_pixel` / `gfx_rect` / `gfx_hline` / `gfx_vline` / `gfx_line`；
  ③ 不出现 `open("/dev/keyboard"` / `open("/dev/mouse"`；
  ④ 不出现 `gui_lvgl_private.h`（业务应用不得包含私有头）；
  ⑤ **不出现窗口装饰标识**：`minimize` / `maximize` / `window_decoration` / `title_bar`（大小写不敏感）——这是「不以绘制窗口装饰代替窗口系统」的可核验形式；
  ⑥ 字号只经由 `gui_theme_font_size(...)` 获得（不得出现 `lv_obj_set_style_text_font(..., 12)` 这类字面量字号）。
- [ ] **6. 接线并验证。** `test_desktop_audit.elf` 加入 `hosttests/Makefile::TEST_BINS` 与 `mk/components/run.mk::GUI_HOST_REGISTERED_CASES`（CASE 名 `desktop-audit`）。执行 `CASE=desktop-audit` / `CASE=desktop-shell` / `CASE=desktop-state` / `CASE=theme` / `CASE=theme-apply` 与 `make test-host`，全绿。
- [ ] **7. 提交** `feat(desktop): add app drawer and basic settings surfaces`。

---

### Task 6: 四个壳内应用

**Files:** Create `user/desktop/desktop_apps.{c,h}`；Modify `user/desktop/desktop_shell.{c,h}`、`user/desktop/desktop.c`。

**Interfaces:**

- `user/desktop/desktop_apps.h`：
  ```c
  /* 构造应用表面；返回根对象（失败返回 NULL）。 */
  lv_obj_t *desktop_app_build(lv_display_t *disp, desktop_app_id_t id,
                              desktop_shell_t *shell);
  /* 应用内部请求返回主屏。 */
  void desktop_app_request_back(desktop_shell_t *shell);
  /* 终端消费键入字符；返回 true 表示已消费。 */
  bool desktop_app_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev);
  ```
  键事件类型用 libgui 公共的 `gui_key_event_t`（`libgui/include/gui/gui_input.h`），**不**新造类型。

- [ ] **1. RED fixture。** 扩展 `test_desktop_shell.c`：遍历 4 个应用，对每个执行 `OPEN_APP(id)`，断言① 状态变为 `APP`；② 应用表面根对象非空；③ 根对象覆盖整个显示区（宽高 = display 宽高）；④ `BACK` 后状态回到 `HOME` 且主屏采样回到初始值。终端应用额外断言一次 `codepoint = 'a'` 会让输入行增加 1 个字符。
- [ ] **2. 跑出 RED。** 应用未实现 → 断言失败（记录失败信息）。
- [ ] **3. 实现文件应用。** `opendir` / `readdir` / `closedir` 浏览，初始 `/`，目录在前、文件在后、各自按名称排序；每行显示名称（`body` 层级）+ 类型标签（`label` 层级）；回车 / 双击进入子目录；顶栏返回键先回父目录，父目录为 `/` 时才回主屏；顶栏副标题注明「只读」。**不得**提供写入 / 删除 / 新建入口。
- [ ] **4. 实现终端应用。** 多行输出区（`body` 层级）+ 提示符 `OS01 $ ` + 当前输入行；可打印 ASCII（0x20–0x7E）由原生按键回调消费并回显；`Enter` 执行；`Backspace` 退格；`↑` / `↓` 翻历史（最近 32 条）。内建命令：`help`、`ls [path]`、`cat <file>`、`uname`、`date`、`clear`；未知命令回 `command not found`。**不 fork 子进程**。
- [ ] **5. 实现系统应用。** 只读展示：内核架构（构建期宏）、构建 profile、显示模式与分辨率、像素格式、键盘 / 鼠标是否存在、LVGL 版本、LVGL 内存池总量与占用率（`lv_mem_monitor`）。
- [ ] **6. 实现设置应用。** 与 Task 5 的设置表面**共用**同一份 `shell->settings`（设置应用是设置表面的**同一份实现**，不是两套）；「关于」项展示与系统应用一致的信息。
- [ ] **7. 验证终端消费键后的焦点状态。** 终端激活时原生按键回调消费可打印字符并 `return true`；被消费的 `UP` 由 libgui 强制释放 LVGL 侧活动键（GUI-2 既有行为）。断言：终端退出后焦点组仍可 `Tab` 导航到启动器第一个图标（不卡死、不残留已销毁对象）。
- [ ] **8. 验证。** `make PROFILE=x86_64-clang user` 绿；`CASE=desktop-shell` 绿；`make test-host` 绿；`make test-static` 绿。
- [ ] **9. 提交** `feat(desktop): add files terminal system and settings apps`。

---

### Task 7: QEMU 端到端门禁与文档收口

**Files:** Create `qemutests/gui_desktop_test.py`；Modify `qemutests/run_test.py`、`qemutests/test_gui_runner.py`、`mk/components/run.mk`、`docs/roadmap.md`、`docs/gui/gui.md`、`docs/changelog.md`。

**Interfaces:** 新 QEMU suite `gui-desktop`，沿用既有 `qemutests/gui_test.py` / `gui_input_test.py` 的 QMP 驱动范式（screendump 取像素、QMP 键鼠注入）。

**套件接线（三处缺一不可）：**
① `qemutests/run_test.py` 的 `args.test_name` 分派链加 `gui-desktop` 分支（否则落到「未知测试」分支直接失败）；
② `mk/components/run.mk` 的 `SUITE` 分派加 `gui-desktop`；
③ 同文件的 `TEST_QEMU_FLAVOR_gui-desktop` / `TEST_QEMU_IMG_gui-desktop` 两个按 suite 命名的变量。

- [ ] **1. RED fixture（runner 契约）。** 在 `qemutests/test_gui_runner.py` 追加新 suite 的**假 runner** 用例：未知 suite 名必须非零失败；`gui-desktop` suite 必须被登记。
- [ ] **2. 跑出 RED。** suite 未登记而失败（记录失败信息）。
- [ ] **3. 实现 `qemutests/gui_desktop_test.py`。** 五项检查（spec §9.3）：① 桌面启动后 screendump 有有效像素（主屏壁纸色命中）、进程退出码 0；② 离屏像素断言（状态栏 / 启动器表面色命中）；③ 注入点击启动器图标 → 画面变化（应用表面色命中）；④ 注入 `ESC` → 回到主屏（初始采样命中）；⑤ 桌面退出后终端 fallback 恢复（显示所有权协议回归）。
- [ ] **4. 接线 suite。** 按上面的三处清单登记。
- [ ] **5. 执行端到端。** `make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop` 通过；`SUITE=gui` 回归通过；`SUITE=phase-0` 回归通过；`OS01_SYSTEST=1 SUITE=systest` 通过；`make test-static` 通过；`make test-host` 通过。
- [ ] **6. 文档收口。** `docs/roadmap.md` 的 GUI-3 行改为已完成并写入交付摘要（日期、提交区间、验证入口）；`docs/gui/gui.md` 新增「GUI-3 Material 风格桌面」章节（设计令牌表、表面与状态机、启动 / 退出 / 全屏切换语义、**明确不做窗口系统**的声明、公共 API 的 LVGL 隔离约束与主题生命周期约束）；`docs/changelog.md` 追加当日条目。
- [ ] **7. 提交** `test(gui): add desktop QEMU gate and close GUI-3 docs`。

---

## Spec 覆盖自检

| Spec 范围 | 实施与验证任务 |
|---|---|
| §1.1 完成条件 1（设计系统落地） | Tasks 1–2（令牌表 + 主题落地）+ Task 5（静态审计钉死「不得绕过令牌」） |
| §1.1 完成条件 2（外壳四件套） | Task 4（状态栏 / 启动器）+ Task 5（抽屉 / 设置） |
| §1.1 完成条件 3（应用语义明确） | §6 全节；Task 3（状态机断言）+ Task 6（应用启动 / 退出） |
| §1.1 完成条件 4（不假装窗口系统） | Task 4（删除手绘窗口装饰）+ Task 5 第 ⑤ 条静态审计 + §6.4 |
| §1.1 完成条件 5（证据可复跑） | Tasks 1–5（宿主）+ Task 7（QEMU） |
| §1.2 不包含 | 全程不实现（深色主题 / 外部进程 / 窗口系统 / 资源加载） |
| §1.3 取舍 1–9 | Tasks 1–6 的具体实现选择 |
| §2.2 边界 1–8 | Task 2（1、2、7、8）、Task 1（3）、Task 3（4）、Task 4（5、6） |
| §3 分层 / 所有权 / 文件清单 | Task 1–6 的 Files 块；Task 4 实现所有权规则 |
| §4.1–4.5 令牌与主题 | Task 1（4.1–4.4 取值）+ Task 2（4.5 API 与主题落地） |
| §5.1–5.5 外壳 | Task 3（5.1 状态机）+ Task 4（5.2 状态栏 / 5.3 启动器）+ Task 5（5.4 抽屉 / 5.5 设置） |
| §6.1–6.5 应用语义 | Task 3（状态机）+ Task 6（应用实现）+ Task 4（键鼠契约） |
| §7.1–7.4 四个内置应用 | Task 6 |
| §8.1 错误语义 | Task 4（主入口退出语义）+ Task 6（命令返回错误信息） |
| §8.2 资源预算 | Task 2（字体体积审计）+ Task 4（内存余量检查） |
| §8.3 构建约束 | Task 1（libgui 打包）+ Task 2（公共头安装 / 字体启用 / `make clean` / 审计同步）+ Task 4（app.mk 包含路径） |
| §9.1 宿主令牌与主题测试 | Tasks 1–2 |
| §9.2 宿主状态机与外壳测试 | Tasks 3–4 |
| §9.3 QEMU E2E | Task 7 |
| §9.4 回归入口 | Task 7 |
| §10 交付分期 | P1 = Tasks 1–2，P2 = Tasks 3–4，P3 = Task 6，P4 = Task 5 + Task 7 |

## Plan 评审修订记录

**Revision 1 → Revision 2**（依据独立评审的 2 个 BLOCKER + 8 个 MAJOR + 6 个 MINOR）：

| 评审发现 | 处置 |
|---|---|
| BLOCKER 1：plan 基线落后一个提交，Task 1 已完成 | Task 1 标为 ✅ 已完成，基线改写为 `0040715a` |
| BLOCKER 2：发布 GUI 头清单钉死，新增 `gui_theme.h` 使 `make test-static` 失败 | Task 2 第 7 步同步审计三处清单 + 导出符号，并把 `make test-static` 纳入验收 |
| MAJOR 3：主题落地混进 `theme.c` 会破坏宿主链接 | 拆成独立的 `theme_apply.c`（Task 2 第 3 步） |
| MAJOR 4：主题落地测试缺链接对象 | Task 2 第 1 步写明复用 `test_gui_lvgl_integration.elf` 的链接形状；第 8 步要求所有链接 `platform.o` 的宿主测试都加 `$(GUI_THEME_OBJS)` |
| MAJOR 5：Task 5 依赖 Task 6 的应用注册表 | 注册表提前到 Task 4（`desktop_registry.c`，纯 C） |
| MAJOR 6：Tasks 4–6 的 RED 步骤依赖尚不存在的 QEMU smoke | 像素断言改到**宿主**离屏 platform（Task 4 第 1 步），QEMU 只留 Task 7 的端到端 |
| MAJOR 7：Task 7 没接 `qemutests/run_test.py` 分派 | Task 7 第 4 步写明三处接线清单 |
| MAJOR 8：hover / pressed / focus / disabled / checked 无断言 | Task 2 第 1 步纳入 spec §9.1 第 9–16 条 |
| MAJOR 9：壁纸无法由 `gui_theme_apply()` 画出 | Task 2 第 3 步要求显式 `lv_theme_apply(screen)` |
| MINOR 10：`gui_app_key_t` 不存在 | Task 6 接口统一为 `gui_key_event_t` |
| MINOR 11：spec 与 plan 的状态机命名不一致 | spec §5.1 / §9.2 改用 `desktop_state_next` 与 `desktop_state.{c,h}`；并写明当前 app id 由外壳携带 |
| MINOR 12：启动器图标数与注册表矛盾 | 注册表 `in_launcher` 明确为 3 个，启动器按标记取子集 |
| MINOR 13：桌面 app.mk 保留私有头路径 | Task 4 第 8 步去掉 `-I libgui/internal`；Task 5 第 ④ 条静态审计封死 |
| MINOR 14：像素采样点没钉死 | Task 4 第 1 步逐条给出坐标，并要求避开图标色块 |
| MINOR 15：`desktop_shell_create()` 误收 `gui_config_t` | Task 4 接口去掉 `cfg`，改用 `lv_display_get_width/height` |
| MINOR：窗口装饰条款只靠构造保证 | Task 5 第 ⑤ 条静态审计显式禁止装饰标识 |
