# P3 GUI-1 LVGL 平台后端 (libgui) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 OS01 原生图形抽象库 `libgui`，封装 LVGL v9.5.0 并隐藏底层实现对象，抽取可复用的显示后端、mmap 绘制缓冲、单调时钟、冲刷回调、显示所有权状态机与控制反转步进接口，重构 `/bin/test_lvgl` 并通过全套宿主与 QEMU 验证。

**Architecture:** 
1. 模块边界：将原有纯三方库打包升级为 `libgui` 组件，输出 `libgui.a`，仅对外发布 `gui.h` 与 `gui_platform.h`，严格隐藏 `lvgl/lvgl.h`。
2. 平台后端：`gui_platform_t` 统一管理匿名 `mmap` 分配的 partial draw buffer 和离屏 shadow canvas；冲刷回调将脏矩形搬运至 libgfx 或离屏画布；`gui_platform_step` 推进时钟并执行所有权状态机判定（支持 EAGAIN 退避 250ms、EACCES/ESTALE 回退），计算 poll 超时等待时间。
3. 测试分层：通过宿主确定性状态机 mock 测试、宿主真实 LVGL 集成测试、接口隔离编译审计以及 QEMU 端到端双模验证（`--smoke` 离屏与前台展示），全面闭环质量门禁。

**Tech Stack:** C11, Clang/LLVM, POSIX mmap/poll/clock_gettime, LVGL v9.5.0, OS01 /dev/gfx0 (libgfx) 与显示所有权协议。

**Spec:** `docs/superpowers/specs/2026-10-19-gui-1-lvgl-platform-backend-design.md`

## Global Constraints

- **接口隔离**: 公共头文件（`gui.h`、`gui_platform.h`）严禁包含 `<lvgl/lvgl.h>` 或声明任何 `lv_*` 类型；`libgui/internal/gui_lvgl_private.h` 仅供内部及过渡测试使用，严禁安装进 sysroot。
- **单实例模型**: 同一进程内仅允许同时存在一个活动 `gui_platform_t`；重复 `create` 必须返回 NULL 并置 `errno = EBUSY`。
- **坐标与尺寸限制**: 限制 `width <= 16383`、`height <= 65535`，所有宽高与行数必须通过无符号防溢出检查，超出返回 `EOVERFLOW`。
- **内存隔离**: 绘制缓冲与离屏画布必须使用匿名 `mmap` 分配，严禁占用用户态堆，销毁时严格执行 `munmap`。
- **libgfx 返回约定**: 用户态契约固定为成功返回 0，失败返回 -1 并设置 errno。失败后必须立即保存 errno，清理动作不得冲掉原始错误。
- **Fatal 断言处理**: `config/lv_conf.h` 中断言处理宏必须使用 `#define LV_ASSERT_HANDLER do { _exit(1); } while (0);`，触发时立即退出进程，不可死循环。

## Review Focus

1. **尺寸乘法与 16 位溢出**: `width * 4 > 65535` 或 `width * lines * 4 > SIZE_MAX`，期望返回 NULL 且 `errno = EOVERFLOW`，不触发非法 mmap 或畸形 header。
2. **EAGAIN 退避期新脏帧与计时**: present 返回 EAGAIN 后 250ms 内不重复触发 present，但 handler 仍可处理动画产生新 flush 并保留最新画面，到期后立即提交。
3. **前台所有权被抢占 (EACCES)**: present 遇到 EACCES 时，平台层立即安全释放并关闭 gfx 句柄，锁存 `GUI_STATUS_LOST_OWNERSHIP` 与 `errno = EACCES`，后续 step 不再重复访问已关闭设备。
4. **异常与致命退出回退**: 当应用被 kill 或触发 LVGL 内部 fatal 断言时，内核通过释放所有打开文件与显存映射，触发显示所有权自动恢复至终端 fallback。
5. **离屏查询越界与高字节掩码**: `gui_platform_get_pixel()` 查询越界坐标返回 `0xFFFFFFFFu` 并置 `EINVAL`；合法坐标返回低 24 位 RGB 值，X 字节屏蔽为 0。

---

### Task 1: LVGL 致命断言处理宏配置 (`config/lv_conf.h`)

**Files:**
- Modify: `config/lv_conf.h:58-64`
- Test: `hosttests/cases/test_gui_assert_syntax.c`

**Interfaces:**
- Consumes: `<unistd.h>` (`_exit`)
- Produces: `LV_ASSERT_HANDLER_INCLUDE` 与 `LV_ASSERT_HANDLER` 宏定义

- [ ] **Step 1: 编写宿主断言宏语法检查测试**

在 `hosttests/cases/test_gui_assert_syntax.c` 中包含 `<unistd.h>` 与 `config/lv_conf.h`，编写一个简单的宏展开校验函数，断言编译期能够正确展开宏（含末尾分号语法合规）。

```c
#include <unistd.h>
#include "../../config/lv_conf.h"

int test_assert_syntax_expand(int cond) {
    if (!cond) {
        LV_ASSERT_HANDLER
    }
    return 0;
}
```

- [ ] **Step 2: 验证编译并确认测试生效**

Run: `clang -fsyntax-only -I. hosttests/cases/test_gui_assert_syntax.c`
Expected: 检查当前是否有语法错误或编译告警。

- [ ] **Step 3: 修改 `config/lv_conf.h` 致命处理宏**

修改 `config/lv_conf.h` 中的 `LV_ASSERT_HANDLER` 定义，严格采用 spec §2.2 规范：
```c
#define LV_ASSERT_HANDLER_INCLUDE <unistd.h>
#define LV_ASSERT_HANDLER do { _exit(1); } while (0);
```

- [ ] **Step 4: 重新编译验证语法通过**

Run: `clang -fsyntax-only -I. hosttests/cases/test_gui_assert_syntax.c`
Expected: PASS，无警告。

- [ ] **Step 5: 提交配置变更**

```bash
git add config/lv_conf.h hosttests/cases/test_gui_assert_syntax.c
git commit -m "build(lvgl): configure non-looping fatal assert handler in lv_conf.h"
```

---

### Task 2: libgui 公共/私有头文件与基础骨架定义

**Files:**
- Create: `libgui/include/gui/gui.h`
- Create: `libgui/include/gui/gui_platform.h`
- Create: `libgui/internal/gui_lvgl_private.h`

**Interfaces:**
- Consumes: `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`
- Produces: `gui_mode_t`, `gui_status_t`, `gui_config_t`, `gui_platform_t`, 公共 API 声明与私有内部桥接声明

- [ ] **Step 1: 创建公共平台头文件 `libgui/include/gui/gui_platform.h`**

严格按照 Spec §3.1 定义：
- 枚举 `gui_mode_t` (`GUI_MODE_ONSCREEN`, `GUI_MODE_OFFSCREEN`)
- 枚举 `gui_status_t` (`GUI_STATUS_OK`, `GUI_STATUS_EAGAIN`, `GUI_STATUS_LOST_OWNERSHIP`, `GUI_STATUS_DISPLAY_CHANGED`, `GUI_STATUS_ERROR`)
- 配置体 `gui_config_t` (`mode`, `width`, `height`, `buffer_lines`)
- 不透明句柄 `typedef struct gui_platform gui_platform_t;`
- 函数声明：`gui_platform_create`, `gui_platform_destroy`, `gui_platform_get_width`, `gui_platform_get_height`, `gui_platform_get_mode`, `gui_platform_step`, `gui_platform_get_pixel`

- [ ] **Step 2: 创建顶层入口头文件 `libgui/include/gui/gui.h`**

严格按照 Spec §3.2，包含 `<gui/gui_platform.h>`。

- [ ] **Step 3: 创建仓库私有桥接头文件 `libgui/internal/gui_lvgl_private.h`**

严格按照 Spec §3.3，包含 `<gui/gui_platform.h>` 与 `<lvgl/lvgl.h>`，声明 `lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform);`。

- [ ] **Step 4: 编译检查头文件自洽性**

Run: `clang -fsyntax-only -Ilibgui/include libgui/include/gui/gui.h && clang -fsyntax-only -Ilibgui/include libgui/include/gui/gui_platform.h`
Expected: 编译通过，0 警告，确认无任何 lvgl 依赖。

- [ ] **Step 5: 提交头文件定义**

```bash
git add libgui/include/gui/gui.h libgui/include/gui/gui_platform.h libgui/internal/gui_lvgl_private.h
git commit -m "feat(gui): define public and internal headers for libgui"
```

---

### Task 3: 平台单调时钟与核心实现 (`src/tick.c`, `src/platform.c`)

**Files:**
- Create: `libgui/src/tick.c`
- Create: `libgui/src/platform.c`

**Interfaces:**
- Consumes: `gui_platform.h`, `gui_lvgl_private.h`, `libgfx` (`gfx_open`, `gfx_acquire`, `gfx_present`, `gfx_release`, `gfx_close`, `gfx_sprite_blit`), `gfx_client_policy`
- Produces: `gui_platform_t` 结构体、生命周期、mmap 缓冲分配、冲刷流水线、所有权状态机与单步步进逻辑

- [ ] **Step 1: 实现单调时钟源 `libgui/src/tick.c`**

实现 LVGL tick 回调函数：
- 调用 `clock_gettime(CLOCK_MONOTONIC, &ts)`；
- 成功返回 `(uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000)`；
- 失败时记录内部错误标志，并返回上次成功毫秒数。
- 提供独立 uint64 毫秒读取函数供平台退避计时。

- [ ] **Step 2: 实现核心平台逻辑 `libgui/src/platform.c`**

按照 Spec §4 编写：
1. 单实例互斥标志 `static bool g_gui_platform_active = false;`，重复创建返回 NULL/`EBUSY`；
2. 配置校验：
   - 模式校验、离屏宽高非零检查、屏幕宽高必须为 0；
   - 宽高上限校验：`width <= 16383`，`height <= 65535`，乘法溢出检查；
   - 行数限制：`lines = min(buffer_lines ? buffer_lines : 40, height)`；
3. 内存映射：
   - 使用匿名 `mmap` 分配 `draw_buf`（`width * lines * 4` 字节）；
   - 离屏模式使用匿名 `mmap` 分配 `shadow_canvas`（`width * height * 4` 字节）；
4. LVGL 绑定与初始化：
   - 调用 `lv_init()`，注册 tick 回调；
   - 创建 display 并设置 `LV_COLOR_FORMAT_XRGB8888`，绑定 partial buffer，设置 user_data 与 flush 回调；
5. 冲刷回调 `platform_flush_cb`：
   - 校验矩形边界与 `stride == area_w * 4`；
   - 屏幕模式执行 `gfx_sprite_blit` 并标记 `pending_present = true`；
   - 离屏模式逐行拷贝至 `shadow_canvas`；
   - 无论成功与否，恰好调用一次 `lv_display_flush_ready(disp)`；
6. 状态机推进与步进 `gui_platform_step`：
   - 优先检查终止态与时钟有效性；
   - 执行 `lv_timer_handler()`；
   - 屏幕模式下按 Spec §4.4 判定 EAGAIN 250ms 退避、前台 acquire、present 结果及 EACCES/ESTALE 回退；
   - 计算并向 `out_next_delay_ms` 输出饱和至 `[0, 100]` ms 的等待时间；
7. 像素采样 `gui_platform_get_pixel`：
   - 离屏模式下读取 `(y * width + x)` 像素，返回低 24 位 RGB，X 字节屏蔽为 0；越界返回 `0xFFFFFFFFu` 并置 `EINVAL`；
8. 销毁 `gui_platform_destroy`：
   - 依次执行 `lv_display_delete`、`lv_deinit`、`gfx_release`、`gfx_close`、`munmap`，重置单实例标志。

- [ ] **Step 3: 语法检查与本地对象编译**

编译 `libgui/src/tick.c` 和 `libgui/src/platform.c`，确保无未解析标识符与语法警告。

- [ ] **Step 4: 提交平台源码**

```bash
git add libgui/src/tick.c libgui/src/platform.c
git commit -m "feat(gui): implement platform lifecycle, mmap buffer, flush and step"
```

---

### Task 4: 宿主确定性状态机测试 (`hosttests/cases/test_gui_platform.c`)

**Files:**
- Create: `hosttests/cases/test_gui_platform.c`
- Modify: `hosttests/Makefile`

**Interfaces:**
- Consumes: `gui_platform.h`, mock libgfx, mock time/mmap
- Produces: 纳入 `make test-host` 的确定性测试二进制

- [ ] **Step 1: 编写宿主状态机测试用例**

在 `hosttests/cases/test_gui_platform.c` 中：
1. **参数与边界检验**：
   - `gui_platform_create(NULL)` 自动读取屏幕配置（在宿主 mock fb 下验证）；
   - 离屏模式宽高为 0 返回 NULL 且 `errno = EINVAL`；
   - 屏幕模式显式传宽/高返回 NULL 且 `errno = EINVAL`；
   - 宽度 16384、高度 65536 或乘法溢出测试，验证返回 NULL 且 `errno = EOVERFLOW`；
2. **单实例生命周期与回滚**：
   - 第一次 `create` 成功后，第二次 `create` 返回 NULL 且 `errno = EBUSY`；
   - 销毁后可再次成功 `create`；
   - `gui_platform_destroy(NULL)` 安全无崩溃；
   - 模拟时钟/mmap/open 失败，验证资源按序回滚，无残留映射且保留原 errno；
3. **显示所有权与状态机测试**：
   - 模拟 acquire 返回 EBUSY，验证 step 返回 ERROR 且锁存 EBUSY；
   - 模拟 present 返回 EAGAIN，验证返回 `GUI_STATUS_EAGAIN`，并在 250ms 内不重新发起 present；
   - 模拟 present 返回 EACCES，验证返回 `GUI_STATUS_LOST_OWNERSHIP`，且 gfx 自动释放并关闭；
   - 模拟 present 返回 ESTALE，验证返回 `GUI_STATUS_DISPLAY_CHANGED`；
4. **离屏像素采样与步进计时**：
   - 离屏模式 step 推进后，验证 `gui_platform_get_pixel()` 对越界坐标返回 `0xFFFFFFFFu` 且 `errno = EINVAL`；
   - 验证 `out_next_delay_ms` 正确计算，无活动定时器时返回 100ms。

- [ ] **Step 2: 在 `hosttests/Makefile` 中注册该测试**

将 `cases/test_gui_platform.c` 加入 `hosttests/Makefile` 的测试用例列表。

- [ ] **Step 3: 运行宿主测试验证全部通过**

Run: `make test-host CASE=test_gui_platform`
Expected: ALL PASS，退出码 0。

- [ ] **Step 4: 提交宿主测试**

```bash
git add hosttests/cases/test_gui_platform.c hosttests/Makefile
git commit -m "test(gui): add host deterministic state machine tests for libgui"
```

---

### Task 5: 宿主真实 LVGL 集成、生命周期与 Fatal 退出测试

**Files:**
- Create: `hosttests/cases/test_gui_lvgl_integration.c`
- Modify: `hosttests/Makefile`

**Interfaces:**
- Consumes: `gui_platform.h`, `gui_lvgl_private.h`, 真实 LVGL submodule 静态库
- Produces: 真实渲染断言、50 次生命周期循环测试、子进程 fatal assert 退出测试

- [ ] **Step 1: 编写宿主真实 LVGL 集成测试**

在 `hosttests/cases/test_gui_lvgl_integration.c` 中：
1. **真实离屏渲染与像素采样**：
   - 创建离屏平台 `320x240`；
   - 通过 `gui_platform_get_lv_display()` 获取 display，在 active screen 创建背景（颜色 `0x181825`）和一个按钮（颜色 `0x1A73E8`）；
   - 执行 `gui_platform_step()` 触发渲染；
   - 调用 `gui_platform_get_pixel()` 断言采样背景与按钮坐标处色彩严格匹配；
2. **真实生命周期循环**：
   - 连续执行 50 次完整循环：`create` -> 构造简单控件 -> `step` -> `destroy`；
   - 验证 50 轮全部成功且无内存泄漏或残留 timer；
3. **Fatal 断言子进程测试**：
   - `fork()` 隔离子进程；
   - 在子进程中耗尽内存池或触发 LVGL 内部断言；
   - 父进程 `waitpid()` 验证子进程以状态码 1 干净退出（验证 Task 1 配置的 `LV_ASSERT_HANDLER do { _exit(1); } while (0);` 生效）。

- [ ] **Step 2: 运行真实集成测试**

Run: `make test-host CASE=test_gui_lvgl_integration`
Expected: ALL PASS，退出码 0。

- [ ] **Step 3: 提交集成测试**

```bash
git add hosttests/cases/test_gui_lvgl_integration.c hosttests/Makefile
git commit -m "test(gui): add host integration, 50x lifecycle and fatal assert tests"
```

---

### Task 6: 构建系统接入与 Sysroot Staging 迁移

**Files:**
- Create: `libgui/Makefile`
- Modify: `mk/components/sysroot.mk:72-78,97-105`
- Modify: `mk/profiles/x86_64-clang.mk:40-45`
- Delete: `liblvgl/Makefile` (或由新 Makefile 接管)

**Interfaces:**
- Consumes: `thirdpart/lvgl.manifest`, `user/common/gfx_client_policy.c`, `libgui/src/*`
- Produces: `libgui.a`, sysroot 发布产物 (`usr/lib/libgui.a`, `usr/include/gui/*`)

- [ ] **Step 1: 编写 `libgui/Makefile`**

参考原 `liblvgl/Makefile` 与 Spec §2.3：
1. 编译 `thirdpart/lvgl/src` 下所有 C 文件；
2. 编译 `libgui/src/platform.c`、`libgui/src/tick.c`；
3. 编译 `user/common/gfx_client_policy.c`（私有归档入库）；
4. 打包为 `$(BUILD_DIR)/libgui/lib/libgui.a`；
5. 实现 `install` 目标：
   - 校验 `INSTALL_ROOT` 必须指向 `staging/gui`；
   - 仅安装 `libgui.a`、`gui.h`、`gui_platform.h` 及 `usr/share/licenses/libgui/LVGL-LICENCE.txt`；
   - 严禁安装任何 LVGL 原生头文件；生成 manifest。

- [ ] **Step 2: 更新 `mk/components/sysroot.mk` 与 profile 配置**

1. 将 `$(STAMPS_DIR)/lvgl-install.stamp` 替换为 `$(STAMPS_DIR)/gui-install.stamp`；
2. 依赖链变更为：依赖 `kernel-headers`、`libc`、`libgfx` 的 install stamp；
3. 更新 `sysroot.mk` generation 发布逻辑中的组件名为 `gui`；
4. 在 `mk/profiles/x86_64-clang.mk` 中配置 `LIBGUI_BUILD_DIR := $(BUILD_DIR)/libgui`。

- [ ] **Step 3: 验证增量与全量 Sysroot 发布**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang sysroot`
Expected:
1. `build/x86_64-clang/sysroot/usr/include/gui/` 包含 `gui.h` 与 `gui_platform.h`；
2. 检查 `build/x86_64-clang/sysroot/usr/include/lvgl/` 不复存在；
3. 检查 `build/x86_64-clang/sysroot/usr/lib/libgui.a` 存在。

- [ ] **Step 4: 编译隔离验证 (Consumer Isolation Test)**

编写一段微型 C 程序，仅以 `-I$(SYSROOT)/usr/include` 编译，只包含 `<gui/gui.h>`，验证无需任何 LVGL 私有头即可正常编译通过。

- [ ] **Step 5: 提交构建系统与 sysroot 迁移**

```bash
git add libgui/Makefile mk/components/sysroot.mk mk/profiles/x86_64-clang.mk
git commit -m "build(gui): introduce libgui staging component and migrate sysroot"
```

---

### Task 7: 重构测试程序 `/bin/test_lvgl` 并消除底层样板代码

**Files:**
- Modify: `user/test_lvgl/app.mk`
- Modify: `user/test_lvgl/test_lvgl.c`
- Modify: `user/Makefile`

**Interfaces:**
- Consumes: `<gui/gui.h>`, `<gui/gui_platform.h>`, `gui_lvgl_private.h`, `-lgui`, `-lgfx`, `-lc`
- Produces: 升级重构后的 `test_lvgl.elf` 应用程序

- [ ] **Step 1: 更新 `user/test_lvgl/app.mk` 与应用编译参数**

1. 将链接参数改为 `APP_LDLIBS := -lgui -lgfx`；
2. 删除 `APP_SHARED_SOURCES := common/gfx_client_policy.c`（已内聚进 libgui）；
3. 仅在 `test_lvgl/app.mk` 中设置私有包含路径 `-I$(OS01_ROOT)/libgui/internal -I$(OS01_ROOT)/thirdpart -I$(OS01_ROOT)/thirdpart/lvgl -I$(OS01_ROOT)/config -DLV_CONF_INCLUDE_SIMPLE`；
4. 移除 `user/Makefile` 中的全局 lvgl 头文件路径。

- [ ] **Step 2: 重构 `user/test_lvgl/test_lvgl.c`**

1. 移除应用内重复的 `my_tick_get_cb`、`open("/dev/fb")`、`gfx_open`、静态 `draw_buf` 数组、`test_flush_cb` 及 `gfx_present_ok`；
2. 初始化阶段：
   - 若处于 `--smoke` 模式，构造 `gui_config_t cfg = { .mode = GUI_MODE_OFFSCREEN, .width = 320, .height = 240 }`；
   - 否则构造屏幕模式配置 `gui_config_t cfg = { .mode = GUI_MODE_ONSCREEN }`；
   - 调用 `gui = gui_platform_create(&cfg);`
3. 界面构建：
   - 通过 `gui_platform_get_lv_display(gui)` 获取内部 display 并调用 `lv_display_get_screen_active()`；
   - 保留原有的丰富卡片 UI 构造函数 `build_rich_ui`；
4. 运行循环：
   - 使用 Spec §4.5 的标准 `gui_platform_step()` + `poll()` 控制反转主循环；
5. 离屏 Smoke 模式验证：
   - 循环调用 `gui_platform_step()` 并在 1 秒内完成；
   - 使用 `gui_platform_get_pixel(gui, 10, 10)` 和按钮坐标直接断言背景色（`0x00181825u`）与按钮色（`0x001A73E8u`）；
   - 保留原 TLSF 内存池断言并输出 `[TEST PASS] LVGL compatibility smoke test succeeded.`，退出码 0；
6. 统一退出：
   - 无论是正常退出还是异常，均调用 `gui_platform_destroy(gui)`。

- [ ] **Step 3: 编译验证用户态程序**

Run: `make PROFILE=x86_64-clang user`
Expected: 成功生成 `test_lvgl.elf`，无链接与符号未解析错误。

- [ ] **Step 4: 提交 test_lvgl 重构**

```bash
git add user/test_lvgl/app.mk user/test_lvgl/test_lvgl.c user/Makefile
git commit -m "refactor(test_lvgl): migrate test_lvgl to libgui platform backend"
```

---

### Task 8: QEMU 端到端自动化套件接入与全线回归

**Files:**
- Modify: `qemutests/` (相关测试入口，如 `qemutests/test_resolution_switcher.py` 或新增 `qemutests/test_gui_runner.py`)
- Modify: `mk/components/run.mk` 或 top-level Makefile

**Interfaces:**
- Consumes: QEMU x86_64 虚拟机、`/bin/test_lvgl`、QMP screendump / 串口日志输出
- Produces: 自动化 E2E 验证结果

- [ ] **Step 1: 离屏 Smoke QEMU 自动化验证**

执行 QEMU 测试，启动 `/bin/test_lvgl --smoke`：
- 断言标准输出匹配 `[TEST PASS] LVGL compatibility smoke test succeeded.`；
- 断言进程退出码为 0，运行耗时小于 2 秒。

- [ ] **Step 2: 屏幕模式与终端回退验证**

执行 QEMU 测试，前台运行 `/bin/test_lvgl --wait`：
- 注入按键 `q`，断言应用平稳退出；
- 断言前台所有权自动回退至终端 fallback（`terminal` 恢复活动）。

- [ ] **Step 3: 运行静态审计与全套测试**

Run:
1. `make test-static`
2. `make test-host`
3. `make test-qemu SUITE=phase-0`

Expected: 全部通过，无回归。

- [ ] **Step 4: 提交 E2E 测试并更新文档状态**

```bash
git add qemutests/
git commit -m "test(gui): integrate libgui smoke and onscreen E2E tests into qemutests"
```
