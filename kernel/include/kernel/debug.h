#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H

// Transition: all debug_<channel>() macros now forward to log_debug()
// while preserving their compile-time per-channel gating (the existing
// OS01_DEBUG_<ch> flags, set via DEBUG_CHANNELS= in the Makefile).
// This means:
//   make DEBUG=1                   → all debug msgs compiled in (default: off)
//   make DEBUG_CHANNELS=sched      → sched msgs visible at LOG_DEBUG
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
