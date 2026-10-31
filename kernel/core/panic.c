#include <core/panic.h>
#include <core/printk.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <percpu/percpu.h>
#include <kernel.h>
#include <time/timer.h>
#include <driver/fb_state.h>
#include <tty/console.h>

char buf[4096] = {'[','k','e','r','n','e','l',' ','p','a','n','i','c',']',' ',0};

void panic_enable_fb_if_possible(void)
{
    fb_snapshot_t snap;
    if (fb_snapshot_read(&snap) == 0 && snap.addr != NULL && snap.mapped_size > 0) {
        if (spin_trylock(&Pos.lock)) {
            console_force_enable_locked();
            spin_unlock(&Pos.lock);
        }
    }
}

void kpanic(const char * msg,...)
{
    va_list args;
    va_start(args,msg);
    vsprintf(buf+15, msg, args);
    va_end(args);
    serial_printk(buf);

    panic_enable_fb_if_possible();

    while (1) {
        arch_local_irq_disable();
        arch_cpu_halt();
    }
}

// hang_dump_all() moved to kernel/sched/diag.c (Phase 2, Task 2): the
// dump is arch-neutral scheduler state, while this TU drags the x86-only
// framebuffer/console headers.  Declaration: <sched/diag.h>.
