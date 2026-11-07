// kernel/arch/x86_64/platform/boot_irq.c
//
// BOOT-2: x86-private boot IRQ/time coordination (spec §6).  One-shot
// state machine over the BSP's early-IRQ stages; every illegal
// transition fails closed before any sti or GS read.  Production logs
// are stage markers only — never per-tick.

#include <arch/x86_64/platform/boot_irq.h>

#include <arch/irq.h>
#include <percpu/percpu.h>
#include <intr/softirq.h>
#include <driver/pit.h>
#include <subsys/subsys.h>
#include <log/log.h>
#include <core/panic.h>
#include <kernel.h>
#include <stddef.h>

enum boot2_state {
    BOOT2_COLD = 0,
    BOOT2_CONTEXT,
    BOOT2_CALIBRATION,
    BOOT2_RUNTIME,
};
static enum boot2_state boot2_state = BOOT2_COLD;

static void boot2_fatal(const char *stage) __attribute__((noreturn));
static void boot2_fatal(const char *stage)
{
    log_err("[boot-2] FATAL stage=%s\n", stage);
    kpanic("BOOT-2: boot IRQ/time protocol violation, stage=%s", stage);
    __builtin_unreachable();
}

void x86_boot_irq_context_prepare(void)
{
    if (boot2_state != BOOT2_COLD)
        boot2_fatal("context");
    /* The prepare itself runs with IRQs off — a sti here would open the
     * window before the state exists.  (irq_save returns whole RFLAGS
     * on x86 — test the IF bit, not the word.) */
    arch_irq_state_t flags = arch_local_irq_save();
    arch_local_irq_restore(flags);
    if (flags & (1UL << 9))
        boot2_fatal("context");

    /* BOOT-1 prerequisites: the BSP percpu is constructed, GS installed
     * and CPU0 online (acquire — the BSP's own release publication). */
    if (__atomic_load_n(&percpu_data[0].online, __ATOMIC_ACQUIRE) != 1)
        boot2_fatal("context");
    if (percpu_data[0].self != (uint64_t)&percpu_data[0])
        boot2_fatal("context");
    if (this_cpu() != &percpu_data[0])
        boot2_fatal("context");

    /* The BSP stack is the init_task_union frame — reachable by
     * construction (we are running on it); the softirq base table must
     * be initialized by the early chain. */
    if (num_cpus == 0)
        boot2_fatal("context");

    boot2_state = BOOT2_CONTEXT;
    log_info("[boot-2] context-ready\n");
    arch_local_irq_restore(flags);
}

bool x86_boot_irq_calibration_allowed(void)
{
    if (boot2_state != BOOT2_CONTEXT)
        return false;
    /* A window may only open with entry IF=0 (the caller must not be
     * inside an already-enabled region).  IF bit only — the saved word
     * is whole RFLAGS on x86. */
    arch_irq_state_t flags = arch_local_irq_save();
    arch_local_irq_restore(flags);
    return (flags & (1UL << 9)) == 0;
}

void x86_boot_irq_calibration_begin(void)
{
    if (boot2_state != BOOT2_CONTEXT)
        boot2_fatal("context");
    boot2_state = BOOT2_CALIBRATION;
}

void x86_boot_irq_calibration_end(void)
{
    if (boot2_state != BOOT2_CALIBRATION)
        boot2_fatal("context");
    boot2_state = BOOT2_CONTEXT;
}

void x86_boot_irq_runtime_open(void)
{
    if (boot2_state != BOOT2_CONTEXT)
        boot2_fatal("runtime");
    /* Regular IRQs need the phase-4 time facilities: the framework
     * timer, the PIT (prepared, not yet started) and the LAPIC timer
     * stub.  clocksource may legitimately be inactive (fallback
     * chain) — active=0 is allowed here. */
    if (subsys_status("timer") != 1 || subsys_status("pit") != 1 ||
        subsys_status("lapic-timer") != 1)
        boot2_fatal("runtime");

    if (pit_start() != 0)
        boot2_fatal("runtime");

    boot2_state = BOOT2_RUNTIME;
    log_info("[boot-2] runtime-open source=pit\n");
    arch_local_irq_enable();
}

bool x86_boot_irq_runtime_opened(void)
{
    return boot2_state == BOOT2_RUNTIME;
}
