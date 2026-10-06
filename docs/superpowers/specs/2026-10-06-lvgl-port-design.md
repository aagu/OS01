# LVGL v9.5.0 库移植与兼容性测试设计

> **日期**: 2026-10-06
> **状态**: 待用户复审
> **基准**: OS01 `62f9cac76ff7734d4444cf04b748dfc003554452`；LVGL v9.5.0 (`85aa60d18b3d5e5588d7b247abf90198f07c8a63`)
> **依赖评估**: `docs/assessments/2026-10-06-lvgl-libc.md`

---

## 1. 目标与范围

本任务的目标是在 OS01 用户态环境中移植开源图形库 **LVGL v9.5.0**，使其无缝接入 OS01 的 profile 构建系统与 single-writer sysroot 架构，并编写用户态兼容性测试程序 `test_lvgl.elf`，验证基础初始化、内存池、控件构建与软件光栅化渲染全流程，为下一独立任务（使用 LVGL 重构 `desktop.c` 桌面）奠定坚实的运行库与接口基础。

### 成功标准
1. **源码受控**：`thirdpart/lvgl` 注册为 Git submodule，并提供 `thirdpart/lvgl.manifest` 锁定提交 `85aa60d18b3d5e5588d7b247abf90198f07c8a63`。
2. **构建合规**：实现独立组件 `liblvgl`，遵循 OS01 single-writer sysroot 规范（`mk/components/sysroot.mk`），输出 `usr/lib/liblvgl.a` 与 `usr/include/lvgl/`，并生成 staging manifest。
3. **符号完整**：基于当前 libc 基线配置 `lv_conf.h`，`liblvgl.a` 无未解析符号，不依赖外部未实现的 libm（如 `cosf/sinf/tanf`）或未完善的 pthread 递归互斥锁。
4. **兼容性测试通过**：用户态 `test_lvgl.elf` 成功编译链接，在内存中完成 `lv_init()`、滴答回调注册、显示驱动初始化、按钮/标签/样式构建及 `lv_timer_handler()` 渲染冲刷，断言有效像素输出并正常退出。

### 非目标 (Non-goals)
- 本任务不修改现有的 `user/desktop.c`（桌面改造在下一个独立任务进行）。
- 本任务不实现 LVGL 的多线程 OSAL 后端（`LV_USE_OS` 保持 `LV_OS_NONE`，单 UI 线程驱动）。
- 本任务不接入 POSIX 文件系统驱动（`LV_USE_FS_POSIX` 保持关闭，资源优先通过编译内置数组提供）。
- 本任务不涉及硬件 2D/3D GPU 加速（采用纯软件渲染 `LV_USE_DRAW_SW=1`）。

---

## 2. 架构设计与配置裁决

根据 `docs/assessments/2026-10-06-lvgl-libc.md` 的评估结果，针对 OS01 当前 libc 特性做出以下配置裁决：

| 配置项 | 选定值 | 裁决依据 |
|---|---|---|
| **OS 抽象 (`LV_USE_OS`)** | `LV_OS_NONE` | OS01 pthread 仅具备基础互斥锁，缺递归锁与条件变量；初期采用单 UI 线程主循环即可满足需求。 |
| **滴答计时 (`lv_tick`)** | 注册自定义回调 | 使用 OS01 已支持的 `clock_gettime(CLOCK_MONOTONIC)` 提供单调毫秒滴答。 |
| **内存分配 (`LV_USE_STDLIB_MALLOC`)** | `LV_STDLIB_BUILTIN` | 使用 LVGL 内置 TLSF 分配器（配置 4MB 独立内存池），避免早期 libc `malloc` 碎片化风险，大型帧缓冲由应用单独分配。 |
| **字符串操作 (`LV_USE_STDLIB_STRING`)** | `LV_STDLIB_CLIB` | libc 已完整提供 `memcpy/memmove/memset/memcmp` 以及 `strlen/strcpy/strcmp` 等。 |
| **格式化输出 (`LV_USE_STDLIB_SPRINTF`)** | `LV_STDLIB_BUILTIN` | 采用 LVGL 内置高效格式化，避免对 libc 浮点/复杂格式扩展的依赖。 |
| **浮点与矩阵 (`LV_USE_FLOAT`, `LV_USE_MATRIX`)** | `0` | 当前 libc 无完整 libm（缺 `sinf/cosf/tanf/roundf`）；标准控件、圆角、阴影等无需浮点矩阵。 |
| **渲染流水线 (`LV_USE_DRAW_SW`)** | `1` | 纯软件渲染器，色彩深度设为 `32`（XRGB8888），与 OS01 Framebuffer 格式完全对齐。 |
| **文件系统 (`LV_USE_FS_*`)** | `0` | 暂不接入外部文件系统驱动，避免头文件定位差异。 |
| **默认字体与主题** | 启用 Montserrat 14/16，启用 Default Theme | 提供即开即用的高质感控件视觉外观。 |

---

## 3. 组件职责与文件清单

| 文件 | 职责 |
|---|---|
| `thirdpart/lvgl/` | LVGL v9.5.0 源码目录（Git submodule）。 |
| `thirdpart/lvgl.manifest` | 源码清单文件，记录源路径与固定的 Git commit hash。 |
| `config/lv_conf.h` | OS01 定制的 LVGL 功能与平台配置文件。 |
| `liblvgl/Makefile` | 编译 `thirdpart/lvgl/src` 源码，构建 `liblvgl.a`，并向 staging 树暂存库文件及公开头文件。 |
| `mk/components/sysroot.mk` | 接入 `$(STAMPS_DIR)/lvgl-install.stamp`，管理 manifest 与 receipt，发布至 sysroot。 |
| `user/test_lvgl.c` | 兼容性测试程序，覆盖初始化、绘制缓冲绑定、控件创建与单次渲染循环。 |
| `user/Makefile` | 增加 `test_lvgl.elf` 的构建与链接规则（`-llvgl -lgfx -lc`）。 |

---

## 4. 构建与 Sysroot 暂存实现细节

### 4.1 源码发现与编译过滤
`thirdpart/lvgl/src` 包含约 460 个 C 源文件。为保证纯粹与可靠，构建脚本将自动递归扫描 `thirdpart/lvgl/src` 下的所有 `.c` 文件，并排除以下不适用的模块：
- `src/osal/` 中除 `lv_os_none.c` 之外的其他操作系统抽象（如 `lv_pthread.c`, `lv_freertos.c` 等）。
- `src/draw/` 中特定硬件加速器（如 `nema_gfx`, `dave2d`, `renesas`, `nxp`, `sdl`, `opengles` 等）。
- `src/libs/` 中需要额外第三方依赖的驱动（如 `freetype`, `thorvg`, `ffmpeg`, `libpng`, `libjpeg_turbo` 等）。
- `src/drivers/` 中的非通用外设驱动。

### 4.2 头文件树暂存结构
为了让应用能以 `#include <lvgl/lvgl.h>` 或 `#include <lvgl/src/...>` 方式标准引用，暂存目录结构规划为：
```
staging/lvgl/
  usr/
    include/
      lvgl/
        lvgl.h
        lv_version.h
        lv_conf.h
        src/
          core/
          draw/
          ...
    lib/
      liblvgl.a
  manifest
```

### 4.3 Sysroot 聚合集成
在 `mk/components/sysroot.mk` 中：
1. 添加 `$(STAMPS_DIR)/lvgl-install.stamp`，依赖 `$(STAMPS_DIR)/libc-install.stamp`。
2. 在 sysroot 的 publish 循环列表补充 `lvgl`：
   `for comp in kernel-headers libc libgfx lvgl mbedtls compat-libs; do ...`

---

## 5. 兼容性测试程序设计 (`user/test_lvgl.c`)

### 5.1 测试流程
1. **系统环境初始化**：
   - 打印测试启动信息。
   - 配置基于 `clock_gettime(CLOCK_MONOTONIC)` 的滴答回调 `uint32_t my_tick_get_cb(void)`。
   - 调用 `lv_tick_set_cb(my_tick_get_cb)`。
   - 调用 `lv_init()`，断言内部状态正常。
2. **显示与虚拟缓冲区配置**：
   - 定义虚拟屏幕尺寸（例如 640x480）。
   - 在 BSS 或堆上分配行级绘制缓冲区（例如 640x40 像素的 `lv_color32_t` 缓冲区）。
   - 调用 `lv_display_create(640, 480)`。
   - 设置绘制缓冲 `lv_display_set_buffers(...)`，设置模式为 `LV_DISPLAY_RENDER_MODE_PARTIAL`。
   - 注册冲刷回调 `flush_cb`，在回调中统计冲刷区域及像素统计，并调用 `lv_display_flush_ready(disp)`。
3. **UI 场景构建**：
   - 获取当前活动屏幕 `lv_screen_active()`。
   - 创建容器面板，设置背景色与圆角。
   - 创建测试按钮 `lv_button_create(parent)`，居中显示。
   - 创建测试文本 `lv_label_create(btn)`，设置文本为 `"OS01 LVGL v9.5.0 OK"`。
4. **渲染执行与断言**：
   - 调用 `lv_timer_handler()` 触发一次完整的布局更新与光栅化渲染。
   - 断言：
     - `flush_cb` 至少被触发 1 次；
     - 冲刷缓冲区中存在非全零的有效像素颜色值；
     - 内存池统计 `lv_mem_get_pct_used()` 属于合理有效范围。
5. **正常收尾**：
   - 打印 `[TEST PASS] LVGL compatibility smoke test succeeded.` 并以返回码 0 退出。

---

## 6. 验证与审计方案

1. **静态符号审计**：
   - 使用 `llvm-nm --undefined-only build/.../liblvgl.a` 确认仅引用了 OS01 libc 提供的符号。
   - 检查 `build/.../test_lvgl.elf` 的未解析符号为 0。
2. **构建一致性测试**：
   - 执行 `make PROFILE=x86_64-clang sysroot`，确保 staging 阶段无 manifest 重复路径冲突。
   - 执行 `make PROFILE=x86_64-clang`，确保 `test_lvgl.elf` 顺利编译并安装到 `artifacts/user`。
3. **QEMU 自动化或无头运行测试**：
   - 在 QEMU 中执行 `/test_lvgl.elf`，确认终端输出期望的测试通过日志且返回值无崩溃。
