# LVGL v9.5.0 库移植与兼容性测试设计

> **日期**: 2026-10-06
> **状态**: 待用户复审（已修订）
> **基准**: OS01 `62f9cac76ff7734d4444cf04b748dfc003554452`；LVGL v9.5.0 (`85aa60d18b3d5e5588d7b247abf90198f07c8a63`)
> **依赖评估**: `docs/assessments/2026-10-06-lvgl-libc.md`

---

## 1. 目标与范围

本任务的目标是在 OS01 用户态环境中移植开源嵌入式图形库 **LVGL v9.5.0**，将其无缝接入 OS01 的 profile 构建系统与 single-writer sysroot 架构，并编写用户态兼容性测试程序 `/bin/test_lvgl`（ELF 目标 `test_lvgl.elf`），验证基础初始化、TLSF 内存池、单调滴答推进、控件层级构建与软件光栅化渲染全流程，为下一独立任务（使用 LVGL 驱动硬件帧缓冲并重构 `desktop.c` 桌面）奠定坚实的运行库与接口基础。

### 成功标准
1. **源码受控**：`thirdpart/lvgl` 注册为 Git submodule，提供 `thirdpart/lvgl.manifest` 锁定提交 `85aa60d18b3d5e5588d7b247abf90198f07c8a63`。
2. **构建合规**：实现独立组件 `liblvgl`，遵循 OS01 single-writer sysroot 规范（`mk/components/sysroot.mk`），输出 `usr/lib/liblvgl.a` 与 `usr/include/lvgl/`，并生成 staging manifest。
3. **依赖与发布链完整**：`$(SYSROOT_STAMP)` 将 `$(STAMPS_DIR)/lvgl-install.stamp` 作为前置依赖，确保 staging 完成后发布并在输入变动时重新触发 generation 组装；测试程序注册到 `mk/components/user.mk` 的 `USER_PROGRAMS` 与 `config/rootfs.mk` 的 `ROOTFS_FILES`，以 `/bin/test_lvgl` 安装进 ext2 根文件系统。
4. **符号完整与审计**：基于当前 libc 基线配置 `lv_conf.h`。`liblvgl.a` 所有对象文件合并 (`ld.lld -r`) 后仅依赖 OS01 libc 符号；最终产物 `test_lvgl.elf` 未解析符号为 0。
5. **内存渲染兼容性测试通过**：用户态 `/bin/test_lvgl` 在内存中完成 `lv_init()`、单调滴答注册、显示驱动初始化、按钮/标签/样式构建及 `lv_timer_handler()` 渲染冲刷，精确断言 flush 区域坐标、特定区域像素颜色（背景色与按钮前景色）及 TLSF 内存池状态，在 QEMU 内以退出码 0 正常结束。

### 非目标 (Non-goals)
- 本任务不修改现有的 `user/desktop.c`（桌面改造在下一个独立任务进行）。
- 本任务不接入物理屏幕或 `/dev/gfx0`（测试为内存离屏渲染 smoke test，无需链接 `libgfx`）。
- 本任务不实现 LVGL 的多线程 OSAL 后端（`LV_USE_OS` 保持 `LV_OS_NONE`，单 UI 线程驱动）。
- 本任务不接入 POSIX 文件系统驱动（`LV_USE_FS_POSIX` 保持关闭，资源优先通过编译内置数组提供）。
- 本任务不涉及硬件 2D/3D GPU 加速（采用纯软件渲染 `LV_USE_DRAW_SW=1`）。

---

## 2. 架构设计与配置裁决

根据 `docs/assessments/2026-10-06-lvgl-libc.md` 的评估结果，针对 OS01 当前 libc 特性做出以下配置裁决：

| 配置项 | 选定值 | 裁决依据 |
|---|---|---|
| **OS 抽象 (`LV_USE_OS`)** | `LV_OS_NONE` | OS01 pthread 仅具备基础互斥锁，缺递归锁与条件变量；单 UI 线程主循环完全满足需求。保留 `lv_os.c` 与 `lv_os_none.c` 编译。 |
| **滴答计时 (`lv_tick`)** | 注册自定义回调 | 在 `lv_init()` 完成后调用 `lv_tick_set_cb()`，基于 `clock_gettime(CLOCK_MONOTONIC)` 返回单调递增毫秒。 |
| **内存分配 (`LV_USE_STDLIB_MALLOC`)** | `LV_STDLIB_BUILTIN` | 使用 LVGL 内置 TLSF 分配器（配置 4MB 独立内存池 `LV_MEM_SIZE = 4 * 1024 * 1024U`），通过 `lv_mem_monitor()` 监控内存使用。 |
| **字符串操作 (`LV_USE_STDLIB_STRING`)** | `LV_STDLIB_CLIB` | libc 已完整提供 `memcpy/memmove/memset/memcmp` 以及 `strlen/strcpy/strcmp` 等。 |
| **格式化输出 (`LV_USE_STDLIB_SPRINTF`)** | `LV_STDLIB_BUILTIN` | 采用 LVGL 内置高效格式化，避免对 libc 浮点/复杂格式扩展的依赖。 |
| **浮点与矩阵 (`LV_USE_FLOAT`, `LV_USE_MATRIX`)** | `0` | 当前 libc 无完整 libm（缺 `sinf/cosf/tanf/roundf`）；标准控件、圆角、阴影等无需浮点矩阵。 |
| **渲染流水线 (`LV_USE_DRAW_SW`)** | `1` | 纯软件渲染器，色彩格式设定为 `LV_COLOR_FORMAT_XRGB8888`（色彩深度 32bpp），与 OS01 Framebuffer 像素格式完全对齐。 |
| **文件系统 (`LV_USE_FS_*`)** | `0` | 暂不接入外部文件系统驱动，避免头文件定位差异。 |
| **默认字体与主题** | 启用 Montserrat 14/16，启用 Default Theme | 提供即开即用的高质感控件视觉外观。 |

---

## 3. 组件职责与文件清单

| 文件 | 职责 |
|---|---|
| `thirdpart/lvgl/` | LVGL v9.5.0 源码目录（Git submodule）。 |
| `thirdpart/lvgl.manifest` | 源码清单文件，记录源路径与固定的 Git commit hash。 |
| `config/lv_conf.h` | OS01 定制的 LVGL 功能与平台配置文件。 |
| `liblvgl/Makefile` | 编译 `thirdpart/lvgl/src` 源码，构建 `liblvgl.a`，向 staging 树暂存库文件及公开头文件并生成 manifest。 |
| `mk/components/sysroot.mk` | 接入 `$(STAMPS_DIR)/lvgl-install.stamp`，纳入 `$(SYSROOT_STAMP)` 前置依赖与 publish 循环。 |
| `user/test_lvgl.c` | 兼容性测试程序，覆盖初始化、绘制缓冲绑定、控件创建与单次渲染循环。 |
| `user/Makefile` | 增加 `test_lvgl.elf` 构建与链接规则（仅需 `-llvgl -lc`）。 |
| `mk/components/user.mk` | 将 `test_lvgl` 加入 `USER_PROGRAMS`，确保 ELF 被复制到 `$(USER_ARTIFACT_DIR)`。 |
| `config/rootfs.mk` | 在 `ROOTFS_FILES` 中注册 `/bin/test_lvgl=$(USER_ARTIFACT_DIR)/test_lvgl.elf:0755`。 |

---

## 4. 构建与 Sysroot 暂存实现细节

### 4.1 源码编译策略
LVGL 源码结构设计高度遵循配置驱动，源文件内部包含宏条件防护（例如 `lv_pthread.c` 内有 `#if LV_USE_OS == LV_OS_PTHREAD`，禁用时自动为空编译单元）。
- `liblvgl/Makefile` 采用全量安全扫描：编译 `thirdpart/lvgl/src` 下的所有 `.c` 文件（约 463 个）。
- 这彻底规避了误删 `src/osal/lv_os.c`（包含 `lv_os_init` / `lv_lock` 等通用接口）导致的符号缺失风险。
- 编译参数：`-ffreestanding -fno-builtin -Wall -Wextra -O2 -fno-pic -fno-pie -mno-red-zone -fstack-protector-strong`，头文件包含 `-I$(STAGING_DIR)/kernel-headers/usr/include -isystem $(STAGING_DIR)/libc/usr/include -I$(LVGL_DIR) -I$(LV_CONF_DIR) -DLV_CONF_INCLUDE_SIMPLE`。

### 4.2 头文件树暂存结构
暂存目录结构规划为：
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

### 4.3 Sysroot 聚合集成与完整依赖链
在 `mk/components/sysroot.mk` 中：
1. 定义安装规则：
   ```make
   $(STAMPS_DIR)/lvgl-install.stamp: $(STAMPS_DIR)/kernel-headers-install.stamp \
                                     $(STAMPS_DIR)/libc-install.stamp FORCE
   	@mkdir -p $(dir $@)
   	$(call os01_submake,liblvgl,install INSTALL_ROOT=$(STAGING_DIR)/lvgl $(OS01_SUBMAKE_ARGS))
   	$(call stamp_check,$(STAGING_DIR)/lvgl)
   ```
2. 将 `$(STAMPS_DIR)/lvgl-install.stamp` 加入 `$(SYSROOT_STAMP)` 的 prerequisites 列表，确保 sysroot 发布时 LVGL staging 已完成，并在源码变化时触发 generation 重新组装。
3. 在 generation 发布循环列表补充 `lvgl`：
   `for comp in kernel-headers libc libgfx lvgl mbedtls compat-libs; do ...`

---

## 5. 兼容性测试程序设计 (`user/test_lvgl.c`)

### 5.1 测试流程与严格断言契约
1. **环境初始化与时钟绑定**：
   - 首先调用 `lv_init()`，完成全局状态分配与默认配置加载。
   - 定义单调时钟回调：
     ```c
     static uint32_t my_tick_get_cb(void) {
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
     }
     ```
   - 调用 `lv_tick_set_cb(my_tick_get_cb)`。
   - 验证滴答计时推进：读取当前 tick，稍微空转或休眠 2ms，再次读取断言 `tick2 >= tick1`。
2. **显示驱动与字节尺寸精确计算**：
   - 设定虚拟视口尺寸：`#define DISP_HOR_RES 320`，`#define DISP_VER_RES 240`。
   - 设定部分刷新缓冲行数：`#define BUF_LINES 40`。
   - 显式计算缓冲区字节数：`uint32_t buf_bytes = DISP_HOR_RES * BUF_LINES * sizeof(uint32_t);`（每像素 4 字节）。
   - 分配行级静态缓冲区 `static uint32_t draw_buf[DISP_HOR_RES * BUF_LINES];`。
   - 创建显示对象：`lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);`。
   - 设置颜色格式：`lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);`。
   - 设置渲染缓冲区：`lv_display_set_buffers(disp, draw_buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);`。
   - 注册冲刷回调 `flush_cb`。
3. **UI 场景构建与目标颜色设定**：
   - 获取活动屏幕：`lv_obj_t *scr = lv_screen_active();`。
   - 明确设置屏幕背景颜色（例如特定灰色 `0x00333333`）：
     `lv_obj_set_style_bg_color(scr, lv_color_hex(0x333333), 0);`
     `lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);`
   - 创建测试按钮 `lv_obj_t *btn = lv_button_create(scr);`：
     - 固定位置与尺寸：`lv_obj_set_pos(btn, 60, 60);`，`lv_obj_set_size(btn, 200, 60);`。
     - 设置按钮背景颜色（例如特定高亮蓝色 `0x001A73E8`）：
       `lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A73E8), 0);`
       `lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);`
   - 创建标签：`lv_obj_t *lbl = lv_label_create(btn);`，设置文本 `"OS01 LVGL OK"`，居中对齐。
4. **渲染冲刷与像素断言**：
   - 在 `flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)` 中：
     - 递增 `flush_count`。
     - 断言 `area->x1 >= 0 && area->y1 >= 0 && area->x2 < DISP_HOR_RES && area->y2 < DISP_VER_RES`。
     - 若冲刷区域覆盖按钮内部坐标（如点 `(100, 70)`），在 `px_map` 中取样对应的像素并断言包含按钮的蓝色分量（非全零、非背景纯灰）。
     - 调用 `lv_display_flush_ready(disp)`。
   - 调用 `lv_timer_handler()` 触发单次完整渲染流程。
   - 断言 `flush_count > 0`。
5. **TLSF 内存池状态严格校验**：
   - 使用 v9.5.0 API 结构体：
     ```c
     lv_mem_monitor_t mon;
     lv_mem_monitor(&mon);
     ```
   - 断言：
     - `mon.total_size >= (4 * 1024 * 1024 - 65536)`（与配置的 4MB 池容量吻合）。
     - `mon.used_pct > 0 && mon.used_pct < 50`（构建按钮与标签后占用了少量堆，未耗尽）。
     - `mon.free_size > 0 && mon.free_size < mon.total_size`。
6. **收尾与退出**：
   - 打印 `[TEST PASS] LVGL compatibility smoke test succeeded.`。
   - `exit(0)`。

---

## 6. 验证与审计方案

1. **静态符号审计**：
   - 将 `liblvgl` 编译的所有 `.o` 文件执行 `ld.lld -r` 合并为 `combined.o`。
   - 执行 `llvm-nm --undefined-only combined.o`，验证未解析符号集严格受限于 OS01 libc.a 提供的符号列表（`malloc/free/realloc`, `memcpy/memmove/memset/memcmp`, `strlen/strcpy/strcmp`, `vsnprintf` 及 SSP canary 符号）。
   - 检查最终用户态可执行文件 `test_lvgl.elf`，执行 `llvm-nm --undefined-only test_lvgl.elf` 必须得到 0 个未解析符号。
2. **构建一致性测试**：
   - 执行 `make PROFILE=x86_64-clang sysroot`，确保 staging 阶段无 manifest 重复路径冲突。
   - 执行 `make PROFILE=x86_64-clang disk.img`，确保 `test_lvgl.elf` 编译成功并正确打包入 ext2 根分区的 `/bin/test_lvgl`。
3. **QEMU 自动化运行测试**：
   - 使用 QEMU 无头运行，在 init/shell 启动后执行 `/bin/test_lvgl`。
   - 设置 30s 超时时间。
   - 断言标准输出中出现 `[TEST PASS] LVGL compatibility smoke test succeeded.` 且进程退出码为 0。
