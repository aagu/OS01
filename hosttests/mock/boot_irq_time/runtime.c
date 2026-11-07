/* BOOT-2 boot_irq_time mock runtime: hardware CPU/port replacement for
 * compiling the REAL kernel/intr/irq.c, 8259A controller, PIT, tick
 * chain and the REAL RTC PIE calibration transaction on the host.
 *
 * Hardware truth modelled here (never the boot2 state machines):
 *   - port model: master IMR 0x21, slave IMR 0xA1; a mask-bit clear
 *     while IF==0 latches a pending edge (IRR property);
 *   - CMOS RTC: index/data register pair at 0x70/0x71 with status
 *     registers A/B/C; enabling PIE with the IRQ8 route open keeps the
 *     periodic IRQ pending — delivered by arch_cpu_pause() (the
 *     foreground spin-loop yield), so the transaction's own loop drives
 *     arrival without the mock knowing any transaction step;
 *   - LAPIC: register file for the masked-sample model;
 *   - GS poison: arch_this_cpu_ptr() traps until the base is installed.
 */
#include <percpu/percpu.h>
#include <arch/x86_64/gate.h>
#include <arch/irq.h>
#include <intr/interrupt.h>
#include <intr/apic.h>
#include <arch/x86_64/platform/boot_irq.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* percpu_data/num_cpus come from the REAL percpu.c in the link. */
static unsigned selected_cpu;
struct gate_struct IDT_Table[256];

/* ── Mock IF flag ──────────────────────────────────────────────── */
int boot2_if_flag = 0;
unsigned long boot2_irq_enable_calls;
void (*boot2_sti_hook)(void);
void arch_local_irq_enable(void)
{
    boot2_irq_enable_calls++;
    boot2_if_flag = 1;
    if (boot2_sti_hook)
        boot2_sti_hook();
}
void arch_local_irq_disable(void) { boot2_if_flag = 0; }

apic_info_t apic_info;
void lapic_eoi(void) {}
/* accounting / lapic premeasure boundaries for time.c and tick.c */
unsigned long boot2_account_tick_calls;
void sched_account_tick(void) { boot2_account_tick_calls++; }
void sched_accounting_cpu_start(uint32_t cpu, uint32_t hz, bool ok)
{ (void)cpu; (void)hz; (void)ok; }
void lapic_timer_set_premeasured(uint64_t hz) { (void)hz; }
void serial_poll(void) {}
int device_boot_result(void) { return 0; }
/* Fatal boundary: recorded, never returns in a passing test. */
unsigned long boot2_kpanic_calls;
/* glibc _exit — declared directly: <unistd.h> resolves to the OS01
 * libc shadow here, which clashes with the host types. */
void _exit(int) __attribute__((noreturn));
void kpanic(const char *msg, ...)
{
    (void)msg;
    boot2_kpanic_calls++;
    /* A BOOT-2 fatal is fatal: the forked test child records it as a
     * failure instead of running off __builtin_unreachable. */
    _exit(86);
}
/* ACPI/MADT data query: no ACPI tables on the host. */
uint8_t irq_trigger_for_gsi(uint32_t gsi) { (void)gsi; return 0; }
uint32_t isa_irq_to_gsi(uint8_t source) { return source; }
/* Test-controlled CPUID 0x15 answer (shadowed cpuid.h reads it). */
int boot2_cpu_id15_answer;

/* ── GS poison model ───────────────────────────────────────────── */
static void *gs_base_installed;
unsigned long boot2_gs_installs;
void (*boot2_gs_install_hook)(void);
void boot2_test_install_gs(void *base)
{
    gs_base_installed = base;
    boot2_gs_installs++;
    if (boot2_gs_install_hook)
        boot2_gs_install_hook();
}
void *arch_this_cpu_ptr(void)
{
    if (gs_base_installed == NULL)
        __builtin_trap();       /* pre-GS access must fail loudly */
    return gs_base_installed;
}

/* ── Port model ────────────────────────────────────────────────── */
#define MASTER_IMR_PORT 0x21
#define SLAVE_IMR_PORT  0xA1
#define CMOS_ADDR_PORT  0x70
#define CMOS_DATA_PORT  0x71

static uint8_t port_model[0x10000];
static unsigned long pending_latch;
uint8_t boot2_imr_get(uint16_t port) { return port_model[port]; }
void boot2_imr_set(uint16_t port, uint8_t value) { port_model[port] = value; }
unsigned long boot2_pending_get(void) { return pending_latch; }

/* CMOS access counter (cache-path assertions). */
unsigned long boot2_cmos_accesses;

/* CMOS register file: A (rate), B (PIE/AIE/UIE), C (flags, read-clear). */
static uint8_t cmos_index;
uint8_t boot2_cmos_regs[16];
/* When true, an open IRQ8 route + B.PIE keeps a PF pending so
 * pause-driven delivery produces a real periodic IRQ. */
int boot2_cmos_periodic;

static void rtc_pending_refresh(void)
{
    uint8_t slave = port_model[SLAVE_IMR_PORT];
    uint8_t master = port_model[MASTER_IMR_PORT];
    if (boot2_cmos_periodic && (boot2_cmos_regs[0x0B] & 0x40) &&
        !(slave & 0x01) && !(master & 0x04))
        pending_latch |= 1UL << 32;              /* RTC pending token */
}

void arch_outb(uint16_t port, uint8_t value)
{
    if (port == MASTER_IMR_PORT || port == SLAVE_IMR_PORT) {
        uint8_t old = port_model[port];
        uint8_t newly_enabled = (uint8_t)(old & ~value);
        port_model[port] = value;
        pending_latch |= (unsigned long)newly_enabled
                         << (port == SLAVE_IMR_PORT ? 16 : 0);
        rtc_pending_refresh();
        return;
    }
    if (port == CMOS_ADDR_PORT) {
        cmos_index = value & 0x7f;
        return;
    }
    port_model[port] = value;
    if (port == CMOS_DATA_PORT) {
        boot2_cmos_accesses++;
        boot2_cmos_regs[cmos_index] = value;
        /* A 1024 Hz periodic source keeps firing on its own: any B or
         * route change re-evaluates the pending state (hardware
         * semantics, not a transaction step). */
        rtc_pending_refresh();
    }
}

uint8_t arch_inb(uint16_t port)
{
    if (port == CMOS_DATA_PORT) {
        boot2_cmos_accesses++;
        uint8_t v = boot2_cmos_regs[cmos_index];
        if (cmos_index == 0x0C)
            boot2_cmos_regs[0x0C] = 0;   /* C is read-clear */
        return v;
    }
    return port_model[port];
}

/* ── LAPIC register file (masked-sample model) + TSC model ─────── */
static uint32_t lapic_regs[0x40];
static uint64_t tsc_model;
uint32_t lapic_read(uint32_t offset) { return lapic_regs[offset >> 2]; }
void lapic_write(uint32_t offset, uint32_t value)
{
    lapic_regs[offset >> 2] = value;
    /* An INIT-count write (re)loads the current decrementer. */
    if (offset == LAPIC_TIMER_INIT)
        lapic_regs[LAPIC_TIMER_CUR >> 2] = value;
}
void boot2_lapic_reset(void) { memset(lapic_regs, 0, sizeof(lapic_regs)); }
uint32_t boot2_lapic_get(uint32_t offset) { return lapic_regs[offset >> 2]; }

/* ── pause-driven IRQ delivery ─────────────────────────────────── */
unsigned long boot2_pause_deliveries;
int boot2_tsc_frozen;   /* test knob: TSC does not advance on delivery */
/* The foreground calibration loop yields through arch_cpu_pause(); a
 * pending RTC periodic IRQ is delivered here through the REAL registry
 * dispatch — the mock never calls a handler directly. */
void arch_cpu_pause(void)
{
    if (pending_latch & (1UL << 32)) {
        pending_latch &= ~(1UL << 32);
        boot2_cmos_regs[0x0C] = 0x40;    /* PF: this IRQ's reason */
        pt_regs_t regs;
        memset(&regs, 0, sizeof(regs));
        boot2_pause_deliveries++;
        arch_irq_dispatch(&regs, 0x28);
        /* Time passes during a delivered tick: TSC advances, the LAPIC
         * decrementer counts down.  Fixed steps keep the sample math
         * deterministic. */
        if (!boot2_tsc_frozen)
            tsc_model += 100000;
        lapic_regs[LAPIC_TIMER_CUR >> 2] -= 50000;
        rtc_pending_refresh();           /* next periodic tick re-pends */
    }
}
void arch_cpu_halt(void) {}

/* ── Fake TSC ──────────────────────────────────────────────────── */
uint64_t arch_cycle_counter(void) { return tsc_model; }
void boot2_tsc_set(uint64_t v) { tsc_model = v; }
void boot2_tsc_advance(uint64_t d) { tsc_model += d; }

/* ── Link-surface stubs (machine boundaries; see ledger) ───────── */
void *kmalloc(size_t size) { return malloc(size ? size : 1); }
void kfree(void *ptr) { free(ptr); }
__attribute__((weak)) bool ioapic_available(void) { return false; }
int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }
unsigned long boot2_early_puts_calls;
void arch_early_puts(const char *s) { (void)s; boot2_early_puts_calls++; }
void arch_early_putc(char c) { (void)c; boot2_early_puts_calls++; }
unsigned long boot2_lapic_timer_irq_calls;
void x86_handle_lapic_timer_irq(pt_regs_t *r) { (void)r; boot2_lapic_timer_irq_calls++; }

/* Exception handler surface (arch dispatch.c): unreachable in this
 * suite — any call is a test-protocol violation. */
void x86_handle_debug(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_nmi(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_int3(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_overflow(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_bounds(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_dev_not_available(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_double_fault(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_coprocessor_segment_overrun(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_invalid_TSS(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_segment_not_present(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_stack_segment_fault(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_x87_FPU_error(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_alignment_check(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_machine_check(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_SIMD_exception(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_virtualization_exception(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_divide_error(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_undefined_opcode(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_general_protection(pt_regs_t *r, uint64_t e) { __builtin_trap(); }
void x86_handle_page_fault(pt_regs_t *r, uint64_t e, uint64_t a) { __builtin_trap(); }

void arch_this_cpu_set(unsigned id) { selected_cpu = id; }

/* percpu/tick-field accessors for the context test TU (which cannot
 * include the kernel percpu.h against the host libc). */
uint64_t boot2_watchdog(unsigned cpu) { return percpu_data[cpu].watchdog_counter; }
uint64_t boot2_need_resched(unsigned cpu) { return percpu_data[cpu].need_resched; }
void *boot2_read_this_cpu(void) { return this_cpu(); }
unsigned boot2_num_cpus(void) { return num_cpus; }
void boot2_percpu_reset(void)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    num_cpus = 0;
}

/* Establish the BOOT-2 CONTEXT state for the rtc group (real percpu.c
 * in the link; the transaction precondition needs context + IF=0). */
void boot2_rtc_ctx_prepare(void)
{
    percpu_data[0].self = (uint64_t)&percpu_data[0];
    percpu_data[0].cpu_id = 0;
    __atomic_store_n(&percpu_data[0].online, 1, __ATOMIC_RELEASE);
    num_cpus = 1;
    arch_this_cpu_set(0);
    extern void percpu_install_gs(uint32_t cpu);
    percpu_install_gs(0);
    x86_boot_irq_context_prepare();
}
