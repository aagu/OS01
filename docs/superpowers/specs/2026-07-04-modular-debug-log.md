# Modular Debug Log System — Implementation Spec

**Date:** 2026-07-04
**Source:** roadmap analysis — ArvernOS channel-based debugging
**Scope:** Replace ad-hoc `serial_printk` debugging with compile-time channel gates
**Status:** proposed

## Problem

The AGENTS.md rule states: "never add/remove `serial_printk` debug lines ad-hoc.
Gate all debug prints behind a build-time flag."  Currently only
`-DOS01_DEBUG_SCHED`, `-DOS01_DEBUG_TTY` exist as ad-hoc flags in `kernel/time/timer.c`.
Every other subsystem relies on commented-out `serial_printk` calls.

## Solution

A unified set of `OS01_DEBUG_<SUBSYSTEM>` macros, one per subsystem, controlled
from `kernel/Makefile` via `DEBUG_CHANNELS=<subsys1>,<subsys2>`.

## Design

### 1. New header: `kernel/include/kernel/debug.h`

```c
#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H

#include <kernel/printk.h>

// ── Channel definitions ────────────────────────────
// Each OS01_DEBUG_<CHANNEL> is either 1 (enabled) or 0 (disabled).
// Set via -DOS01_DEBUG_<CHANNEL> in kernel/Makefile CFLAGS.

#ifndef OS01_DEBUG_SCHED
#define OS01_DEBUG_SCHED 0
#endif
#ifndef OS01_DEBUG_TTY
#define OS01_DEBUG_TTY 0
#endif
#ifndef OS01_DEBUG_VFS
#define OS01_DEBUG_VFS 0
#endif
#ifndef OS01_DEBUG_MM
#define OS01_DEBUG_MM 0
#endif
#ifndef OS01_DEBUG_IRQ
#define OS01_DEBUG_IRQ 0
#endif
#ifndef OS01_DEBUG_SYSCALL
#define OS01_DEBUG_SYSCALL 0
#endif
#ifndef OS01_DEBUG_TASK
#define OS01_DEBUG_TASK 0
#endif
#ifndef OS01_DEBUG_IPI
#define OS01_DEBUG_IPI 0
#endif
#ifndef OS01_DEBUG_BLOCK
#define OS01_DEBUG_BLOCK 0
#endif
#ifndef OS01_DEBUG_FS
#define OS01_DEBUG_FS 0
#endif

// ── Debug print macros ─────────────────────────────
// Usage:  debug_sched("cpu %d: switching to pid %d\n", cpu, next->pid);
// Expands to nothing when channel is disabled — zero runtime cost.

#define debug_sched(fmt, ...)   do { if (OS01_DEBUG_SCHED)  serial_printk("[sched] "  fmt, ##__VA_ARGS__); } while(0)
#define debug_tty(fmt, ...)     do { if (OS01_DEBUG_TTY)    serial_printk("[tty] "    fmt, ##__VA_ARGS__); } while(0)
#define debug_vfs(fmt, ...)     do { if (OS01_DEBUG_VFS)    serial_printk("[vfs] "    fmt, ##__VA_ARGS__); } while(0)
#define debug_mm(fmt, ...)      do { if (OS01_DEBUG_MM)     serial_printk("[mm] "     fmt, ##__VA_ARGS__); } while(0)
#define debug_irq(fmt, ...)     do { if (OS01_DEBUG_IRQ)    serial_printk("[irq] "    fmt, ##__VA_ARGS__); } while(0)
#define debug_syscall(fmt, ...) do { if (OS01_DEBUG_SYSCALL) serial_printk("[syscall] " fmt, ##__VA_ARGS__); } while(0)
#define debug_task(fmt, ...)    do { if (OS01_DEBUG_TASK)   serial_printk("[task] "   fmt, ##__VA_ARGS__); } while(0)
#define debug_ipi(fmt, ...)     do { if (OS01_DEBUG_IPI)    serial_printk("[ipi] "    fmt, ##__VA_ARGS__); } while(0)
#define debug_block(fmt, ...)   do { if (OS01_DEBUG_BLOCK)  serial_printk("[block] "  fmt, ##__VA_ARGS__); } while(0)
#define debug_fs(fmt, ...)      do { if (OS01_DEBUG_FS)     serial_printk("[fs] "     fmt, ##__VA_ARGS__); } while(0)

#endif // _KERNEL_DEBUG_H
```

### 2. Makefile integration

In `kernel/Makefile`, add:

```makefile
# ── Debug channels (comma-separated, e.g. DEBUG_CHANNELS=sched,tty) ──
ifdef DEBUG_CHANNELS
  CHANNELS := $(subst $(comma), ,$(DEBUG_CHANNELS))
  ALL_CFLAGS += $(foreach ch,$(CHANNELS),-DOS01_DEBUG_$(shell echo $(ch) | tr 'a-z' 'A-Z')=1)
endif
comma := ,
```

Usage:
```bash
make run DEBUG_CHANNELS=sched,tty         # enable sched + tty channels
make run DEBUG_CHANNELS=sched,vfs,irq     # enable sched + vfs + irq
```

### 3. Migration plan

Replace existing ad-hoc `serial_printk` debug lines with `debug_<channel>()` macros:

| File | Old pattern | New macro |
|------|------------|-----------|
| `kernel/time/timer.c` | `#ifdef OS01_DEBUG_SCHED` + `serial_printk` | `debug_sched(...)` |
| `kernel/sched/task.c` | commented-out `serial_printk` | `debug_sched(...)` / `debug_task(...)` |
| `kernel/tty/tty.c` | commented-out `serial_printk` | `debug_tty(...)` |
| `kernel/fs/vfs.c` | commented-out `serial_printk` | `debug_vfs(...)` |
| `kernel/memory/*.c` | commented-out `serial_printk` | `debug_mm(...)` |
| `kernel/intr/apic/ipi.c` | commented-out `serial_printk` | `debug_ipi(...)` |
| `kernel/intr/irq.c` | commented-out `serial_printk` | `debug_irq(...)` |
| `kernel/fs/fat.c` | commented-out `serial_printk` | `debug_fs(...)` |
| `kernel/block/ahci.c` | commented-out `serial_printk` | `debug_block(...)` |

### 4. Production build

Without `DEBUG_CHANNELS`, all macros expand to nothing — zero overhead.
`make` with no `DEBUG_CHANNELS` = clean production build.

### Files Changed

| File | Action |
|------|--------|
| `kernel/include/kernel/debug.h` | **NEW** — channel macros |
| `kernel/Makefile` | ~5 lines — `DEBUG_CHANNELS` support |
| `kernel/time/timer.c` | migrate existing `#ifdef OS01_DEBUG_SCHED` blocks |
| `kernel/sched/task.c` | uncomment + convert `serial_printk` → `debug_*` |
| `kernel/tty/tty.c` | uncomment + convert |
| `kernel/fs/vfs.c` | uncomment + convert |
| `kernel/memory/*.c` | uncomment + convert |
| `kernel/intr/apic/ipi.c` | uncomment + convert |
| `kernel/intr/irq.c` | uncomment + convert |
| `kernel/fs/fat.c` | uncomment + convert |
| `kernel/block/blockdev.c` | uncomment + convert |
| `AGENTS.md` | update debug rules to reference `debug_*` macros |

### Verification

1. `make run` — boot without `DEBUG_CHANNELS`, confirm no debug output
2. `make run DEBUG_CHANNELS=sched` — boot, confirm `[sched]` prefix messages appear
3. `make run DEBUG_CHANNELS=sched,tty,vfs,mm` — confirm all 4 channels active
4. `make clean && make` — confirm zero debug code in binary (`strings kernel.bin | grep debug_`)

