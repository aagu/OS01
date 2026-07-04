#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H

#include <kernel/printk.h>

// ── Channel definitions ────────────────────────────
// Each OS01_DEBUG_<channel> is either 1 (enabled) or 0 (disabled).
// Set via -DOS01_DEBUG_<channel> in kernel/Makefile CFLAGS.
// The Makefile passes lowercase channel names, e.g.:
//   DEBUG_CHANNELS=sched,tty
//   → -DOS01_DEBUG_sched=1 -DOS01_DEBUG_tty=1

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

// ── Debug print macros ─────────────────────────────
// Usage:  debug_sched("cpu %d switching to pid %d\n", cpu, next->pid);
// Expands to nothing when channel is disabled — zero runtime cost.

#define debug_sched(fmt, ...)                                               \
    do { if (OS01_DEBUG_sched)  serial_printk("[sched] " fmt, ##__VA_ARGS__); } while(0)
#define debug_tty(fmt, ...)                                                 \
    do { if (OS01_DEBUG_tty)    serial_printk("[tty] " fmt, ##__VA_ARGS__); } while(0)
#define debug_vfs(fmt, ...)                                                 \
    do { if (OS01_DEBUG_vfs)    serial_printk("[vfs] " fmt, ##__VA_ARGS__); } while(0)
#define debug_mm(fmt, ...)                                                  \
    do { if (OS01_DEBUG_mm)     serial_printk("[mm] " fmt, ##__VA_ARGS__); } while(0)
#define debug_irq(fmt, ...)                                                 \
    do { if (OS01_DEBUG_irq)    serial_printk("[irq] " fmt, ##__VA_ARGS__); } while(0)
#define debug_syscall(fmt, ...)                                             \
    do { if (OS01_DEBUG_syscall) serial_printk("[syscall] " fmt, ##__VA_ARGS__); } while(0)
#define debug_task(fmt, ...)                                                \
    do { if (OS01_DEBUG_task)   serial_printk("[task] " fmt, ##__VA_ARGS__); } while(0)
#define debug_ipi(fmt, ...)                                                 \
    do { if (OS01_DEBUG_ipi)    serial_printk("[ipi] " fmt, ##__VA_ARGS__); } while(0)
#define debug_block(fmt, ...)                                               \
    do { if (OS01_DEBUG_block)  serial_printk("[block] " fmt, ##__VA_ARGS__); } while(0)
#define debug_fs(fmt, ...)                                                  \
    do { if (OS01_DEBUG_fs)     serial_printk("[fs] " fmt, ##__VA_ARGS__); } while(0)

#endif // _KERNEL_DEBUG_H
