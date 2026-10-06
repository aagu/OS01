# LVGL v9.5.0 库移植与兼容性测试设计

> **日期**: 2026-10-06
> **状态**: 待用户复审（v4 最终修订）
> **基准**: OS01 `62f9cac76ff7734d4444cf04b748dfc003554452`；LVGL v9.5.0 (`85aa60d18b3d5e5588d7b247abf90198f07c8a63`)
> **依赖评估**: `docs/assessments/2026-10-06-lvgl-libc.md`

---

## 1. 目标与范围

本任务的目标是在 OS01 用户态环境中移植开源嵌入式图形库 **LVGL v9.5.0**，将其无缝接入 OS01 的 profile 构建系统与 single-writer sysroot 架构，并编写用户态兼容性测试程序 `/bin/test_lvgl`（ELF 目标 `test_lvgl.elf`），验证基础初始化、TLSF 内存池、单调滴答推进、控件层级构建与软件光栅化渲染全流程，为下一独立任务（使用 LVGL 驱动硬件帧缓冲并重构 `desktop.c` 桌面）奠定坚实的运行库与接口基础。

### 成功标准
1. **源码受控**：`thirdpart/lvgl` 注册为 Git submodule，提供 `thirdpart/lvgl.manifest` 锁定提交 `85aa60d18b3d5e5588d7b247abf90198f07c8a63`。
2. **构建合规与增量安全**：
   - 实现独立组件 `liblvgl`，遵循 OS01 single-writer sysroot 规范（`mk/components/sysroot.mk`），输出 `usr/lib/liblvgl.a` 与 `usr/include/lvgl/`，并生成 staging manifest。
   - `liblvgl/Makefile` 采用 `-MMD -MP` 生成依赖，`install` 目标严格校验 `INSTALL_ROOT` 非空且匹配当前 profile 的 `staging/lvgl` 路径后执行原子清理与串行安装；配置 `config/lv_conf.h` 变动能准确触发重新编译与 sysroot republish。
3. **依赖与发布链完整**：
   - `$(SYSROOT_STAMP)` 将 `$(STAMPS_DIR)/lvgl-install.stamp` 作为前置依赖，确保 staging 完成后发布并在输入变动时重新触发 generation 组装。
   - 应用编译契约明确：在应用编译参数中增加 `-I$(TARGET_INCDIR)/lvgl -DLV_CONF_INCLUDE_SIMPLE`，保证应用与库引用完全相同的已安装配置，禁止在应用阶段穿透直接引用源码目录。
   - 测试程序注册到 `mk/components/user.mk` 的 `USER_PROGRAMS` 与 `config/rootfs.mk` 的 `ROOTFS_FILES`，以 `/bin/test_lvgl` 安装进 ext2 根文件系统。
4. **符号完整与动态比对审计**：
   - 基于当前 libc 基线配置 `lv_conf.h`。将 `liblvgl.a` 所有对象文件合并 (`ld.lld -r`) 为 `combined.o`。
   - 提取其未解析符号，比对 OS01 已发布的 `libc.a` 导出符号全集（包含 `malloc/free/realloc`、`memcpy/memmove/memset/memcmp`、`strlen/strnlen/strcpy/strncpy/strcmp/strncmp/strcat/strncat/strchr`、`vsnprintf` 及 SSP canary 符号）；严格禁止出现未实现的 `libm`（如 `cosf/sinf/tanf`）和 `pthread` 符号。
   - `combined.o` 与 `libc.a` 做可重定位链接验证，剩余未解析符号为 0；最终产物 `test_lvgl.elf` 静态检查未解析符号为 0。
5. **内存渲染兼容性测试通过**：
   - `/bin/test_lvgl` 包含健壮的时钟回调机制：回调内维护 `clock_failed` 状态标志，测试等待采用独立循环预算（50 次短延时），每次采样先断言时钟调用成功，再断言合理有界递增 `0 < (uint32_t)(t1 - t0) <= 100`。
   - 验证控件光栅化冲刷矩形有效性（`x1 <= x2 && y1 <= y2`）；
   - 设置背景采样与按钮采样标记，使用精确字节行跨度（stride 校验无异常行填充）定位像素，精确匹配背景纯色（`0x00333333`）与按钮内部纯色（`0x001A73E8`）的低 24 位 RGB 值，断言两处采样均被命中。
   - 通过 `lv_mem_monitor()` 精确断言 TLSF 内存池容量、已用比例和空闲空间。
   - 在 QEMU 内以退出码 0 正常结束，输出 `[TEST PASS]` 标记。

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
| **字符串操作 (`LV_USE_STDLIB_STRING`)** | `LV_STDLIB_CLIB` | libc 已完整提供 `memcpy/memmove/memset/memcmp` 以及 `strlen/strnlen/strcpy/strncpy/strcmp/strncmp/strcat/strncat/strchr` 等。 |
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
| `user/Makefile` | 增加 `test_lvgl.elf` 构建与链接规则（配置搜索路径与 `-llvgl -lc`）。 |
| `mk/components/user.mk` | 将 `test_lvgl` 加入 `USER_PROGRAMS`，确保 ELF 被复制到 `$(USER_ARTIFACT_DIR)`。 |
| `config/rootfs.mk` | 在 `ROOTFS_FILES` 中注册 `/bin/test_lvgl=$(USER_ARTIFACT_DIR)/test_lvgl.elf:0755`。 |

---

## 4. 构建与 Sysroot 暂存实现细节

### 4.1 源码编译策略与增量安全
- `liblvgl/Makefile` 递归扫描编译 `thirdpart/lvgl/src` 下的所有 `.c` 文件（约 463 个），源文件内自带的条件宏会自动处理未启用后端的空编译。
- 编译依赖：使用 `clang -MMD -MP` 生成精确依赖文件，包含 `config/lv_conf.h`。修改 `lv_conf.h` 时触发相关源文件重新编译。
- 编译参数：`-ffreestanding -fno-builtin -Wall -Wextra -O2 -fno-pic -fno-pie -mno-red-zone -fstack-protector-strong`。
- 头文件搜索包含：`-I$(STAGING_DIR)/kernel-headers/usr/include -isystem $(STAGING_DIR)/libc/usr/include -I$(LVGL_DIR) -I$(LV_CONF_DIR) -DLV_CONF_INCLUDE_SIMPLE`。

### 4.2 头文件暂存与安全清理
在 `liblvgl/Makefile` 的 `install` 目标中：
1. 校验 `INSTALL_ROOT` 非空且必须以 `/staging/lvgl` 结尾，防止误删其它目录：
   ```make
   install: $(ARCHIVE)
   	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install requires INSTALL_ROOT"; exit 1; }
   	@echo "$(INSTALL_ROOT)" | grep -q '/staging/lvgl$$' || { echo "ERROR: INSTALL_ROOT must point to staging/lvgl"; exit 1; }
   	rm -rf $(INSTALL_ROOT)
   	mkdir -p $(INSTALL_ROOT)/usr/include/lvgl $(INSTALL_ROOT)/usr/lib
   	cp $(ARCHIVE) $(INSTALL_ROOT)/usr/lib/liblvgl.a
   	cp $(LV_CONF_DIR)/lv_conf.h $(INSTALL_ROOT)/usr/include/lvgl/lv_conf.h
   	cp $(LVGL_DIR)/lvgl.h $(LVGL_DIR)/lv_version.h $(INSTALL_ROOT)/usr/include/lvgl/
   	cp -R --preserve=timestamps $(LVGL_DIR)/src $(INSTALL_ROOT)/usr/include/lvgl/
   	@find $(INSTALL_ROOT) -type f ! -name manifest | sed 's|^$(INSTALL_ROOT)/||' | sort > $(INSTALL_ROOT)/manifest
   ```
2. 串行安装规则由 top-level makefile 与 `sysroot.mk` 统一调度，确保无并发写冲突。

### 4.3 Sysroot 聚合与应用编译契约
1. **Sysroot 前置依赖**：
   在 `mk/components/sysroot.mk` 中，将 `$(STAMPS_DIR)/lvgl-install.stamp` 加入 `$(SYSROOT_STAMP)` 的 prerequisites 列表，并加入 publish 循环列表：
   `for comp in kernel-headers libc libgfx lvgl mbedtls compat-libs; do ...`
2. **应用侧编译契约**：
   应用必须直接从 sysroot 引用 LVGL，不得穿透引用源码目录。在 `user/Makefile` 中：
   ```make
   CFLAGS += -I$(TARGET_INCDIR)/lvgl -DLV_CONF_INCLUDE_SIMPLE
   ```
   当应用使用 `#include <lvgl/lvgl.h>` 时，LVGL 内部头文件根据 `LV_CONF_INCLUDE_SIMPLE` 直接引用 `"lv_conf.h"`，并在 `$(TARGET_INCDIR)/lvgl` 中命中发布的配置。
3. **测试程序链接契约**：
   `test_lvgl.elf` 仅链接 `$(USER_RAW_LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $< -llvgl -lc`。

---

## 5. 兼容性测试程序设计 (`user/test_lvgl.c`)

### 5.1 测试流程与严格断言契约
1. **环境初始化与时钟绑定**：
   - 首先调用 `lv_init()`，完成全局状态分配与默认配置加载。
   - 定义单调时钟回调，内建失败标志：
     ```c
     static bool clock_failed = false;
     static uint32_t my_tick_get_cb(void) {
         struct timespec ts;
         if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
             clock_failed = true;
             return 0;
         }
         return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
     }
     ```
   - 验证初始时钟调用：调用一次 `clock_gettime(CLOCK_MONOTONIC, &ts)`，若非 0 则报错并退出码 1 终止。
   - 调用 `lv_tick_set_cb(my_tick_get_cb)`。
   - **断言单调时间递增（有界预算探测）**：
     - 记录基准：`uint32_t t0 = lv_tick_get();`，断言 `!clock_failed`。
     - 采用有界探测循环（最多 50 次迭代，每次微量延时约 1~2ms），避免仅依赖待测时钟进行超时计时：
       ```c
       bool tick_progressed = false;
       for (int i = 0; i < 50; i++) {
           struct timespec pause = { .tv_sec = 0, .tv_nsec = 2000000 };
           nanosleep(&pause, NULL);
           uint32_t t1 = lv_tick_get();
           if (clock_failed) {
               fprintf(stderr, "clock_gettime failed during tick polling\n");
               exit(2);
           }
           uint32_t delta = t1 - t0;
           if (delta > 0 && delta <= 100) {
               tick_progressed = true;
               break;
           }
       }
       if (!tick_progressed) {
           fprintf(stderr, "tick failed to progress within budget\n");
           exit(2);
       }
       ```
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
   - 设置特定屏幕背景色：`#define CLR_EXPECT_BG 0x00333333u`。
     `lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_EXPECT_BG), 0);`
     `lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);`
   - 创建测试按钮 `lv_obj_t *btn = lv_button_create(scr);`：
     - 固定位置与尺寸：`lv_obj_set_pos(btn, 60, 60);`，`lv_obj_set_size(btn, 200, 60);`。
     - 设置特定按钮背景色：`#define CLR_EXPECT_BTN 0x001A73E8u`。
       `lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_EXPECT_BTN), 0);`
       `lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);`
   - 创建标签：`lv_obj_t *lbl = lv_label_create(btn);`，设置文本 `"OS01 LVGL OK"`，居中对齐。
4. **渲染冲刷与像素精确断言**：
   - 定义全局标记：`static bool bg_sampled = false; static bool btn_sampled = false;`。
   - 选定采样检测点：
     - 背景采样点：`(10, 10)`（避开按钮，确保为纯背景）。
     - 按钮内部采样点：`(100, 70)`（位于按钮内部，避开边缘圆角与中央文本）。
   - 在 `flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)` 中：
     - 递增 `flush_count`。
     - **坐标边界断言**：
       `assert(area->x1 <= area->x2 && area->y1 <= area->y2);`
       `assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < DISP_HOR_RES && area->y2 < DISP_VER_RES);`
     - **行跨度提取与像素寻址**：
       - `int32_t area_w = area->x2 - area->x1 + 1;`
       - 读取或断言 `stride_bytes = area_w * 4;`（XRGB8888 格式每像素 4 字节，断言无意外对齐填充）。
       - 若包含 `(10, 10)`：
         `const uint8_t *row = px_map + (10 - area->y1) * stride_bytes;`
         `uint32_t px = *(const uint32_t *)(row + (10 - area->x1) * 4);`
         断言 `(px & 0x00FFFFFFu) == CLR_EXPECT_BG`，并标记 `bg_sampled = true;`。
       - 若包含 `(100, 70)`：
         `const uint8_t *row = px_map + (70 - area->y1) * stride_bytes;`
         `uint32_t px = *(const uint32_t *)(row + (100 - area->x1) * 4);`
         断言 `(px & 0x00FFFFFFu) == CLR_EXPECT_BTN`，并标记 `btn_sampled = true;`。
     - 调用 `lv_display_flush_ready(disp)`。
   - 调用 `lv_timer_handler()` 触发单次完整渲染流程。
   - **像素命中与冲刷断言**：
     - 断言 `flush_count > 0`。
     - 断言 `bg_sampled && btn_sampled`（两处目标采样点必须都被覆盖并验证）。
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
   - 执行 `llvm-nm --undefined-only combined.o`，验证未解析符号全集严格受限于 OS01 `libc.a` 导出的定义符号集合；确认无任何 `cosf/sinf/tanf/powf/sqrtf` 等数学库符号及 `pthread_*` 符号。
   - 将 `combined.o` 与 `libc.a` 执行 `ld.lld -r`，断言剩余未解析符号为 0。
   - 检查最终用户态可执行文件 `test_lvgl.elf`，执行 `llvm-nm --undefined-only test_lvgl.elf` 必须得到 0 个未解析符号。
2. **构建一致性与增量测试**：
   - 执行 `make PROFILE=x86_64-clang sysroot`，确保 staging 阶段无 manifest 重复路径冲突。
   - 测试修改 `config/lv_conf.h`，确保触发增量重新编译与 sysroot 重新发布。
   - 执行 `make PROFILE=x86_64-clang disk.img`，确保 `test_lvgl.elf` 编译成功并正确打包入 ext2 根分区的 `/bin/test_lvgl`。
3. **QEMU 自动化运行测试**：
   - 自动化无头运行脚本：通过串口管道向 QEMU 注入执行命令 `/bin/test_lvgl`，并捕获退出码标记（如 `echo EXIT_CODE:$?`）。
   - 超时设定：整机启动超时 25s，测试执行超时 10s。
   - 验收断言：标准输出中出现 `[TEST PASS] LVGL compatibility smoke test succeeded.` 且 `EXIT_CODE:0`；失败时自动保留完整串口日志输出。
