# Log Level System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add runtime log level filtering (ERR/WARN/INFO/DEBUG) to OS01's kernel logging, replacing the all-or-nothing `serial_printk`/`color_printk` with a unified `log(level, fmt, ...)` system.

**Architecture:** A `log()` macro checks `level <= g_log_level` before formatting (zero overhead for filtered messages). The `_log_write()` function formats via `vsnprintf` into a static buffer and dispatches to serial and/or framebuffer based on build-time `LOG_TARGET`. The existing `debug_<channel>()` macros redirect to `log_debug` for runtime filterability.

**Tech Stack:** C (freestanding), x86_64 kernel, APIC spinlocks, serial UART, framebuffer.

---

## Global Constraints

- **`-static`** in kernel LDFLAGS (mandatory)
- **`make clean`** after struct changes (no header deps in Makefile)
- Use `spin_lock_irqsave`/`spin_unlock_irqrestore` for the log lock (matches existing pattern in wait queues, pmm, etc.)
- `spin_lock_irqsave` disables local IRQs during the entire UART busy-wait (~26ms per 100 chars at 38400 baud). Acceptable for a debug OS; timer ticks are lost on the local CPU but the win is no deadlock risk from `int $0x80` syscall handlers.
- `vsnprintf` from libc (`libc/stdio/vsnprintf.c`, declared in `<stdio.h>`) is available
- `LOG_TARGET` uses C preprocessor defines set via Makefile `-DLOG_TARGET_SERIAL=1` / `-DLOG_TARGET_FB=1`
- `g_log_level` default is `LOG_DEBUG` during the transition (so debug channel output remains visible); change to `LOG_INFO` after migration is complete

---

### Task 1: Create `log.h` — log levels + macros

**Files:**
- Create: `kernel/include/kernel/log.h`

**Interfaces:**
- Produces: `LOG_ERR=3`, `LOG_WARN=4`, `LOG_INFO=6`, `LOG_DEBUG=7` level constants
- Produces: `log(level, fmt, ...)` core macro
- Produces: `log_err()`, `log_warn()`, `log_info()`, `log_debug()` convenience macros
- Produces: `g_log_level` extern declaration, `log_set_level()`, `log_get_level()` prototypes
- Produces: `NDEBUG` compile-time elimination of `log_debug`

- [ ] **Step 1: Create the header**

```c
#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H

// ── Log levels ────────────────────────────────────────────
// Higher number = more verbose.  Matches Linux KERN_* convention.
#define LOG_ERR    3   // Error conditions
#define LOG_WARN   4   // Warning conditions
#define LOG_INFO   6   // Informational
#define LOG_DEBUG  7   // Debug — eliminated in NDEBUG builds

// ── Core log macro ────────────────────────────────────────
// Only evaluates the level check (integer compare) at runtime.
// If the level passes, calls _log_write() which does vsnprintf + output.
// This ensures filtered messages pay zero formatting cost.
#define log(level, fmt, ...) do {                                \
    if ((level) <= g_log_level) {                                \
        void _log_write(int, const char *, ...);                 \
        _log_write(level, fmt, ##__VA_ARGS__);                   \
    }                                                            \
} while(0)

// ── Convenience macros ────────────────────────────────────
#define log_err(fmt, ...)   log(LOG_ERR,   fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  log(LOG_INFO,  fmt, ##__VA_ARGS__)

// ── Debug level (compile-time eliminable) ─────────────────
#ifndef NDEBUG
#define log_debug(fmt, ...) log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define log_debug(fmt, ...) do {} while(0)
#endif

// ── Runtime level control ─────────────────────────────────
// g_log_level is intentionally a plain int (no atomic/volatile):
// a transient torn read in a multicore race is harmless — it only
// causes one extra/missing log message.  RELAXED ordering accepted.
extern int g_log_level;

void log_set_level(int level);
int  log_get_level(void);

#endif // _KERNEL_LOG_H
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/kernel/log.h
git commit -m "feat(log): add log level system header with LOG_ERR/WARN/INFO/DEBUG"
```

---

### Task 2: Add `LOG_TARGET` build option to `kernel/Makefile`

> ⚠️ Done BEFORE log.c so the -DLOG_TARGET_* defines exist when log.c is compiled.

**Files:**
- Modify: `kernel/Makefile`

**Interfaces:**
- Produces: `LOG_TARGET=serial` (default), `LOG_TARGET=fb`, `LOG_TARGET=both`
- Produces: `-DLOG_TARGET_SERIAL=1` / `-DLOG_TARGET_FB=1` CFLAGS

- [ ] **Step 1: Edit `kernel/Makefile` — add the bool helper and LOG_TARGET section**

Insert after line 86 (the `KERNEL_SELFTEST` block):

```makefile
# ── Log output target (serial | fb | both) ────────────────
# Helper: non-empty → 1, empty → 0
bool = $(if $1,1,0)

LOG_TARGET ?= serial
ALL_CFLAGS += -DLOG_TARGET_SERIAL=$(call bool,$(filter serial both,$(LOG_TARGET)))
ALL_CFLAGS += -DLOG_TARGET_FB=$(call bool,$(filter fb both,$(LOG_TARGET)))
```

- [ ] **Step 2: Verify the Makefile still parses**

```bash
make -C kernel clean   # clean any stale build
make -C kernel         # build with default LOG_TARGET=serial
```

Expected output: clean compile, `kernel.bin` produced.

- [ ] **Step 3: Commit**

```bash
git add kernel/Makefile
git commit -m "build(kernel): add LOG_TARGET build option (serial/fb/both)"
```

---

### Task 3: Create `log.c` — `_log_write()` + level control

**Files:**
- Create: `kernel/kernel/log.c`
- Modify: (implicit — `kernel/log.c` is already covered by `$(wildcard kernel/*.c)` in Makefile)

**Interfaces:**
- Consumes: `LOG_ERR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG` from `log.h`
- Consumes: `LOG_TARGET_SERIAL`, `LOG_TARGET_FB` as preprocessor defines (set by Task 2)
- Consumes: `write_serial(char)` from `driver/serial.h`
- Consumes: `color_printk(int, int, const char*, ...)` from `kernel/printk.h`
- Consumes: `LIGHT_GRAY` from `kernel/printk.h` (added in this task)
- Consumes: `spin_lock_irqsave` / `spin_unlock_irqrestore` / `spinlock_T` from `kernel/arch/x86_64/spinlock.h`
- Consumes: `vsnprintf` from `<stdio.h>`
- Produces: `_log_write()` — the output dispatcher
- Produces: `g_log_level`, `log_set_level()`, `log_get_level()`

- [ ] **Step 1: Add `LIGHT_GRAY` constant to `printk.h`**

Edit `kernel/include/kernel/printk.h`, add `#define LIGHT_GRAY 0x00c0c0c0` after the existing color constants.

- [ ] **Step 2: Create `log.c`**

```c
#include <kernel/log.h>
#include <kernel/printk.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <driver/serial.h>
#include <stdio.h>

// ── Lock ──────────────────────────────────────────────────
// spin_lock_irqsave protects both the vsnprintf into the static
// buffer AND the output loop.  This means local IRQs are disabled
// during the entire UART write (up to ~26ms for 100 chars at
// 38400 baud).  This is acceptable for a debug OS: timer ticks
// are lost on the local CPU but the deadline is short and the
// alternative (spin_lock without irqsave) deadlocks if an
// int $0x80 syscall handler calls log() while task context
// holds log_lock.
static spinlock_T log_lock = {1};

// ── Global level ──────────────────────────────────────────
// Default to LOG_DEBUG during the transition so that existing
// debug_<channel>() messages remain visible.  After the
// migration is complete, change this to LOG_INFO.
int g_log_level = LOG_DEBUG;

void log_set_level(int level)
{
    g_log_level = level;
}

int log_get_level(void)
{
    return g_log_level;
}

// ── Level → FB color mapping ─────────────────────────────
#if LOG_TARGET_FB
static int level_to_color(int level)
{
    switch (level) {
    case LOG_ERR:   return RED;
    case LOG_WARN:  return ORANGE;
    case LOG_INFO:  return WHITE;
    case LOG_DEBUG: return LIGHT_GRAY;
    default:        return WHITE;
    }
}
#endif

// ── Batch serial write ────────────────────────────────────
// Loops over buf calling write_serial().  Could be optimized
// to fill the UART FIFO, but one-char-at-a-time is sufficient
// for a debug OS.
#if LOG_TARGET_SERIAL
static void write_serial_buf(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        write_serial((unsigned char)buf[i]);
}
#endif

// ── Output dispatcher ─────────────────────────────────────
// Lock is acquired BEFORE vsnprintf to protect the shared static
// buffer from concurrent access (TOCTOU race on SMP).
void _log_write(int level, const char *fmt, ...)
{
    static char log_buf[1024];
    va_list args;
    int len;

    uint64_t flags = spin_lock_irqsave(&log_lock);

    va_start(args, fmt);
    len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);
    if (len < 0) { spin_unlock_irqrestore(&log_lock, flags); return; }
    if (len >= (int)sizeof(log_buf))
        len = (int)sizeof(log_buf) - 1;

#if LOG_TARGET_SERIAL
    write_serial_buf(log_buf, len);
#endif
#if LOG_TARGET_FB
    color_printk(level_to_color(level), BLACK, "%s", log_buf);
#endif

    spin_unlock_irqrestore(&log_lock, flags);
}
```

- [ ] **Step 3: Build and verify**

```bash
make -C kernel clean && make -C kernel
```

Expected: clean compile. LOG_TARGET_SERIAL and LOG_TARGET_FB are now 1 and 0 respectively (default LOG_TARGET=serial).

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel/log.c kernel/include/kernel/printk.h
git commit -m "feat(log): add _log_write() dispatcher with serial/FB output"
```

---

---

### Task 4: Transition `debug.h` — redirect to `log_debug`

**Files:**
- Create: `kernel/sched/task.c` — one-line migration
- Modify: `kernel/include/kernel/debug.h`

**Interfaces:**
- Consumes: `log_debug()` from `kernel/log.h`
- Consumes: `log_info()` from `kernel/log.h`
- Produces: `debug_sched()`, `debug_tty()`, `debug_vfs()`, `debug_mm()`, `debug_irq()`, `debug_syscall()`, `debug_task()`, `debug_ipi()`, `debug_block()`, `debug_fs()` all forward to `log_debug`, preserving per-channel compile-time gating

> ⚠️ **task.c dependency:** `kernel/sched/task.c:224` calls `serial_printk("[hang] ...")` which previously got its declaration through `debug.h` → `printk.h`. After debug.h drops the `#include <printk.h>`, this will fail to compile. Fix it in the same commit by migrating that call to `log_info`.

- [ ] **Step 1: Migrate task.c:224 from serial_printk to log_info**

In `kernel/sched/task.c`, find:
```c
serial_printk("[hang] CPU %u recovered (watchdog=%lu ticks)\n",
              (unsigned long)cpu_id(),
              (unsigned long)this_cpu()->watchdog_counter);
```

Replace with:
```c
log_info("[hang] CPU %u recovered (watchdog=%lu ticks)\n",
         (unsigned long)cpu_id(),
         (unsigned long)this_cpu()->watchdog_counter);
```

Add `#include <kernel/log.h>` to the includes in task.c. Remove `#include <kernel/printk.h>` if it was only used transitively (check for other uses first).

- [ ] **Step 2: Rewrite `debug.h`**

Replace the entire file content:

```c
#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H

// Transition: all debug_<channel>() macros now forward to log_debug()
// while preserving their compile-time per-channel gating (the existing
// OS01_DEBUG_<ch> flags, set via DEBUG_CHANNELS= in the Makefile).
// This means:
//   make DEBUG_CHANNELS=sched        → sched msgs visible at LOG_DEBUG
//   make NDEBUG=1                    → all debug msgs vanish
//   log_set_level(LOG_INFO) at boot  → debug msgs filtered at runtime

#include <kernel/log.h>

// ── Channel definitions (unchanged) ───────────────────────
#ifndef OS01_DEBUG_sched
#define OS01_DEBUG_sched 0
#endif
#ifndef OS01_DEBUG_tty
#define OS01_DEBUG_tty 0
#endif
#ifndef OS01_DEBUG_vfs
#define OS01_DEBUG_vfs 0
#endif
#ifndef OS01_DEBUG_mm
#define OS01_DEBUG_mm 0
#endif
#ifndef OS01_DEBUG_irq
#define OS01_DEBUG_irq 0
#endif
#ifndef OS01_DEBUG_syscall
#define OS01_DEBUG_syscall 0
#endif
#ifndef OS01_DEBUG_task
#define OS01_DEBUG_task 0
#endif
#ifndef OS01_DEBUG_ipi
#define OS01_DEBUG_ipi 0
#endif
#ifndef OS01_DEBUG_block
#define OS01_DEBUG_block 0
#endif
#ifndef OS01_DEBUG_fs
#define OS01_DEBUG_fs 0
#endif

// ── Debug print macros ────────────────────────────────────
// The OS01_DEBUG_<ch> gate preserves compile-time channel
// filtering.  log_debug() adds runtime level filtering and
// NDEBUG elimination.

#define debug_sched(fmt, ...)                                               \
    do { if (OS01_DEBUG_sched)  log_debug("[sched] " fmt, ##__VA_ARGS__); } while(0)
#define debug_tty(fmt, ...)                                                 \
    do { if (OS01_DEBUG_tty)    log_debug("[tty] " fmt, ##__VA_ARGS__); } while(0)
#define debug_vfs(fmt, ...)                                                 \
    do { if (OS01_DEBUG_vfs)    log_debug("[vfs] " fmt, ##__VA_ARGS__); } while(0)
#define debug_mm(fmt, ...)                                                  \
    do { if (OS01_DEBUG_mm)     log_debug("[mm] " fmt, ##__VA_ARGS__); } while(0)
#define debug_irq(fmt, ...)                                                 \
    do { if (OS01_DEBUG_irq)    log_debug("[irq] " fmt, ##__VA_ARGS__); } while(0)
#define debug_syscall(fmt, ...)                                             \
    do { if (OS01_DEBUG_syscall) log_debug("[syscall] " fmt, ##__VA_ARGS__); } while(0)
#define debug_task(fmt, ...)                                                \
    do { if (OS01_DEBUG_task)   log_debug("[task] " fmt, ##__VA_ARGS__); } while(0)
#define debug_ipi(fmt, ...)                                                 \
    do { if (OS01_DEBUG_ipi)    log_debug("[ipi] " fmt, ##__VA_ARGS__); } while(0)
#define debug_block(fmt, ...)                                               \
    do { if (OS01_DEBUG_block)  log_debug("[block] " fmt, ##__VA_ARGS__); } while(0)
#define debug_fs(fmt, ...)                                                  \
    do { if (OS01_DEBUG_fs)     log_debug("[fs] " fmt, ##__VA_ARGS__); } while(0)

#endif // _KERNEL_DEBUG_H
```

- [ ] **Step 2: Clean build**

```bash
make -C kernel clean && make -C kernel
```

Expected: clean compile. Debug channels preserve compile-time gating AND gain runtime level filtering.

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/debug.h
git commit -m "refactor(debug): redirect debug_<channel> macros to log_debug()"
```

---

### Task 5: Migrate `trap.c` — all exception handlers to `log_err`

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`

**Interfaces:**
- Consumes: `log_err()`, `log_warn()`, `log_info()`, `log_debug()` from `kernel/log.h`
- Removes direct usage of: `serial_printk()`, `color_printk()`

- [ ] **Step 1: Add `#include <kernel/log.h>` to trap.c**

Find the existing includes and add `#include <kernel/log.h>`.

- [ ] **Step 2: Replace all `serial_printk(...)` calls in trap.c**

All exception handlers (`do_divide_error`, `do_debug`, `do_nmi`, `do_int3`, `do_overflow`, `do_bounds`, `do_undefined_opcode`, `do_dev_not_available`, `do_double_fault`, `do_coprocessor_segment_overrun`, `do_invalid_TSS`, `do_segment_not_present`, `do_stack_segment_fault`, `do_general_protection`, `do_page_fault`, `do_x87_FPU_error`, `do_alignment_check`, `do_machine_check`, `do_SIMD_exception`, `do_virtualization_exception`, fault handlers, PF handler, signal delivery paths) — all are **errors** or **warnings**. Use:

```c
// Fatal exceptions → log_err
log_err("do_general_protection(13),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",
        error_code, regs->rsp, regs->rip);

// Informative diagnostics → log_info
log_info("Segment Selector Index:%#010x\n", error_code & 0xfff8);

// Debug detail → log_debug (preserving the existing PF detail lines)
log_debug("PF: pid=%d cr2=%p no vma\n", t->pid, cr2);
```

Replace each `color_printk(RED, BLACK, ...)` in exception handlers with `log_err(...)`. The framebuffer output will get the RED color automatically from the level→color mapping in `_log_write`.

- [ ] **Step 3: Replace `serial_printk("fork: pid=%d", ...)` etc. in trap.c**

The fork/chdir/reboot/reaper messages near the end of trap.c are `log_info` level.

- [ ] **Step 4: Remove unused includes**

If `#include <kernel/printk.h>` was only used for `serial_printk`/`color_printk` in this file, it can be replaced with `<kernel/log.h>`. Keep it if `color_printk` is still used directly for any remaining FB-only output.

- [ ] **Step 5: Build and verify**

```bash
make -C kernel clean && make -C kernel          # compile check first
timeout 15 make run 2>&1 | head -80             # run QEMU with 15s timeout
```

Watch for: exception handler messages appearing in serial output at startup (expected: "do_page_fault" etc. still visible since default level is LOG_INFO). Remove `#include <kernel/printk.h>` if no longer needed.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "refactor(trap): migrate exception handlers from serial_printk to log_err/log_info"
```

---

### Task 6: Build, smoke-test, and update roadmap

**Files:**
- Modify: `docs/roadmap.md` — mark #5 as done

- [ ] **Step 1: Full clean build + run**

```bash
make clean && make kernel.bin
timeout 15 make run 2>&1 | head -80
```

Verify: kernel boots, selftest passes, exception messages appear correctly.

- [ ] **Step 2: Test compile-time DEBUG elimination**

```bash
make clean && make kernel.bin NDEBUG=1 LOG_TARGET=serial
```

Verify: builds clean, no DEBUG messages in output.

- [ ] **Step 3: Test LOG_TARGET=both**

```bash
make clean && make kernel.bin LOG_TARGET=both
```

Verify: builds clean. (Framebuffer output visible in QEMU gtk window.)

- [ ] **Step 4: Mark roadmap item #5 as complete**

Edit `docs/roadmap.md`: change "5. 日志级别系统 🟡 P1" to ✅.

- [ ] **Step 5: Commit**

```bash
git add docs/roadmap.md
git commit -m "docs: mark log level system as completed"
```
