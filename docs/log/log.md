# Log System

## Design

Four log levels defined in `kernel/include/log/log.h`:

| Level     | Value | Description                              |
|-----------|-------|------------------------------------------|
| `LOG_ERR` | 3     | Fatal errors, always visible             |
| `LOG_WARN`| 4     | Warnings                                 |
| `LOG_INFO`| 6     | Normal information                       |
| `LOG_DEBUG`| 7   | Debug messages (compile-time eliminated with `NDEBUG`) |

Values follow the Linux `KERN_*` convention (lower = more urgent).

---

## API

```c
log_err("disk write failed: %d\n", err);
log_warn("low memory: %lu bytes free\n", free);
log_info("CPU %d online\n", cpu);
log_debug("mapped va %p -> pa %p\n", va, pa);
```

Each is a thin wrapper around the `log(level, fmt, ...)` core macro, which checks `g_log_level` at runtime before calling `_log_write()` — filtered messages pay zero formatting cost.

---

## Target Selection (compile time)

Controlled by `LOG_TARGET` in `kernel/Makefile` (default: `serial`):

| `LOG_TARGET=` | Serial output | Framebuffer output |
|---------------|---------------|-------------------|
| `serial`      | yes           | no                |
| `fb`          | no            | yes               |
| `both`        | yes           | yes               |

Set via `make kernel.bin LOG_TARGET=both` or the root `Makefile`'s `LOG_TARGET ?= serial`.

The Makefile converts the target string to compile-time defines:

```makefile
LOG_TARGET ?= serial
ALL_CFLAGS += -DLOG_TARGET_SERIAL=$(call bool,$(filter serial both,$(LOG_TARGET)))
ALL_CFLAGS += -DLOG_TARGET_FB=$(call bool,$(filter fb both,$(LOG_TARGET)))
```

---

## Output Format

Each log line: `[LEVEL] message\n` with level prefix and color coding on framebuffer.

Framebuffer colors per level:
- `LOG_ERR`  → RED
- `LOG_WARN` → ORANGE
- `LOG_INFO` → WHITE
- `LOG_DEBUG`→ LIGHT_GRAY

---

## Compile-Time Elimination

- `NDEBUG=1` eliminates all `log_debug()` calls — expands to `do {} while(0)`.
- Production build (no `DEBUG_CHANNELS`, no `NDEBUG`): only `ERR`/`WARN`/`INFO` remain available at runtime.
- `g_log_level` defaults to `LOG_DEBUG` during the transition period; planned default is `LOG_INFO` post-migration.

---

## Runtime Filtering

- `log_set_level(LOG_INFO)` — suppresses `LOG_DEBUG` at runtime via the `g_log_level` guard in the `log()` macro.
- Default level: `LOG_DEBUG` (everything visible).
- `int g_log_level` is a plain `int` (no atomic/volatile) — a transient torn read on SMP is harmless (one extra/missing message).
- `log_get_level()` returns the current level.

---

## Debug Channel Macros

Defined in `kernel/include/core/debug.h`. Bridge between `DEBUG_CHANNELS` and `log_debug`:

```c
debug_sched("cpu %d switching to pid %d\n", cpu, pid);
debug_vfs("VFS: mounted '%s'\n", path);
debug_mm("alloc_pages: zone=%d count=%d\n", zone, n);
```

Available channels: `sched`, `tty`, `vfs`, `mm`, `irq`, `syscall`, `task`, `ipi`, `block`, `fs`.

Usage: `make kernel.bin DEBUG_CHANNELS=sched,vfs,mm`

Each macro:
1. Checks the compile-time `OS01_DEBUG_<ch>` flag (set by `DEBUG_CHANNELS=` in Makefile)
2. Forwards to `log_debug()` which adds runtime level filtering and `NDEBUG` elimination
3. Prepends a channel tag, e.g. `[sched]`, `[vfs]`

---

## Implementation

| File | Purpose |
|------|---------|
| `kernel/log/log.c` | `_log_write()` dispatcher, `log_set_level()`, `log_get_level()`, level-to-color mapping |
| `kernel/include/log/log.h` | Macros (`log`, `log_err`, `log_warn`, `log_info`, `log_debug`) and level definitions |
| `kernel/include/core/debug.h` | Per-channel debug macros forwarding to `log_debug` |
| `kernel/Makefile` | `LOG_TARGET=`, `NDEBUG=`, `DEBUG_CHANNELS=` build flags |

Key details in `kernel/log/log.c`:
- `spin_lock_irqsave` protects both the shared static buffer and the UART write loop (prevents deadlock when `int $0x80` syscall handler calls `log()` while task context holds `log_lock`).
- `vsnprintf` renders into a static 1024-byte buffer, then dispatched to serial and/or framebuffer based on compile-time flags.
- Framebuffer output uses `color_printk()` with level-dependent colors.

---

## Agent & Developer Guidelines (内核日志规范与编码守则)

所有后续编码 Agent 与开发者在新增或修改内核代码时，**必须严格遵守**以下规则：

### 1. 强制规范 (Mandatory Policy)

- **禁止直接调用裸输出函数**：严禁在内核子系统、驱动程序、文件系统、内存管理、架构初始化等业务代码中直接调用 `serial_printk(...)` 或 `color_printk(...)` 输出运行日志、错误诊断或调试信息。
- **必须使用结构化日志宏**：统一通过 `#include <log/log.h>` 并调用 `log_err()` / `log_warn()` / `log_info()` / `log_debug()`（或通过 `#include <core/debug.h>` 调用对应通道宏 `debug_<channel>()`）。
- **禁止硬编码颜色**：不要使用 `color_printk(RED, ...)` 等手工指定颜色输出日志。当启用 `LOG_TARGET=fb` 或 `LOG_TARGET=both` 时，日志子系统会根据日志级别自动映射 Framebuffer 渲染颜色（ERR 为红、WARN 为橙、INFO 为白、DEBUG 为浅灰）。

### 2. 日志级别选用指南 (Level Selection)

| 宏名称 | 适用场景 | 典型示例 |
|--------|---------|---------|
| `log_err(fmt, ...)` | 致命错误、不可恢复的故障、数据结构损坏、越界非法参数、硬件严重报错 | 内存越界/double-free、VFS 链表损坏、致命缺页崩溃、非法栈指针切换 |
| `log_warn(fmt, ...)` | 可容错的异常、降级运行状态、可选子系统跳过、重试警告、资源告警 | 非必要分区挂载失败、可选设备驱动初始化失败、APIC ID 超出上限被丢弃 |
| `log_info(fmt, ...)` | 内核启动里程碑、重要子系统/设备初始化成功、CPU 核心上线、硬件注册就绪 | `serial port init succeed`、`BSP online`、`tty: console TTY created` |
| `log_debug(fmt, ...)` 或 `debug_<channel>()` | 高频追踪日志、吞吐/延迟性能指标统计、协议交互明细（生产构建中由 `NDEBUG=1` 编译期消除） | `debug_sched(...)` 调度切换追踪、`fb_write_row` 渲染帧耗时统计 |

### 3. 白名单特例 (Whitelisted Exceptions)

仅在以下 4 类特殊底层基础设施场景下允许直接使用 `serial_printk` 或 `color_printk`：

1. **Panic 与极脆弱调度器热路径**：
   - 包含：`kernel/core/panic.c`（`kpanic`、`hang_dump_all`）和 `kernel/sched/core.c`（`schedule()` 中 `SCHED-PREV-BAD`）。
   - **原因**：此类路径处于死锁高危区或上下文切换中断嵌套深处，调用 `log_*` 会尝试获取 `log_lock` 自旋锁，极易引发锁重入或死锁，因此必须使用直写物理串口的裸打印。
2. **终端硬件直接渲染**：
   - 包含：`kernel/tty/tty.c`（`tty_def_output`）以及 `kernel/arch/x86_64/intr/syscall_frame.c`（`arch_syscall_putchar`）。
   - **原因**：属于 VT100 控制台显存裸字符流渲染或单字节测试通道，不属于格式化系统日志。
3. **日志与 Printk 基础设施实现自身**：
   - 包含：`kernel/log/log.c`、`kernel/core/printk.c`。
   - **原因**：避免实现层循环递归依赖。
4. **内核 Selftest 专用测试协议传输**：
   - 包含：`kernel/selftest/*.c` 以及在 `kernel/core/main.c` / `kernel/sched/core.c` 中由宿主测试脚本直接扫描断言的标记字符串（如 `[TEST]`、`[selftest]`）。
   - **原因**：宿主测试框架（如 `qemutests/test_kernel_selftest_result.py`）对裸字符串协议做行首精确正则匹配，添加日志时间戳或级别前缀会导致测试断言失败。

---

## Migration Path

| Current call | Replace with |
|-------------|--------------|
| `serial_printk("error: ...")` | `log_err("...")` |
| `serial_printk("warning: ...")` | `log_warn("...")` |
| `serial_printk("info: ...")` | `log_info("...")` |
| `serial_printk("debug: ...")` | `debug_sched(...)` / `debug_vfs(...)` etc. |
| `color_printk(...)` | `log_info(...)` (framebuffer output is automatic when `LOG_TARGET` includes `fb`) |
