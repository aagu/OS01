/* BOOT-2 boot_irq_time mock runtime: hardware CPU/port replacement for
 * compiling the REAL kernel/intr/irq.c and the REAL
 * kernel/arch/x86_64/intr/pic_8259a.c on the host.  The port model is
 * the only hardware truth: OCW1 writes (0x21 master / 0xA1 slave IMR)
 * are stored, and clearing a mask bit while the mock IF is 0 latches a
 * pending edge — the spec's "IF=0 does not stop the controller from
 * latching an edge into the IRR" property. */
#include <percpu/percpu.h>
#include <arch/x86_64/gate.h>
#include <arch/irq.h>
#include <intr/interrupt.h>
#include <intr/apic.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 1;
static unsigned selected_cpu;
struct gate_struct IDT_Table[256];
void *arch_this_cpu_ptr(void) { return &percpu_data[selected_cpu]; }

int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

void *kmalloc(size_t size) { return malloc(size ? size : 1); }
void kfree(void *ptr) { free(ptr); }

__attribute__((weak)) bool ioapic_available(void) { return false; }

/* ── Mock IF flag ──────────────────────────────────────────────── */
int boot2_if_flag = 0;
unsigned long boot2_irq_enable_calls;
void arch_local_irq_enable(void) { boot2_irq_enable_calls++; boot2_if_flag = 1; }
void arch_local_irq_disable(void) { boot2_if_flag = 0; }
/* ── Port model: master IMR @0x21, slave IMR @0xA1 ─────────────── */
#define MASTER_IMR_PORT 0x21
#define SLAVE_IMR_PORT 0xA1
static uint8_t imr_model[0x10000];
static unsigned long pending_latch;

uint8_t boot2_imr_get(uint16_t port) { return imr_model[port]; }
void boot2_imr_set(uint16_t port, uint8_t value) { imr_model[port] = value; }
unsigned long boot2_pending_get(void) { return pending_latch; }

void arch_outb(uint16_t port, uint8_t value)
{
    if (port == MASTER_IMR_PORT || port == SLAVE_IMR_PORT) {
        uint8_t old = imr_model[port];
        uint8_t newly_enabled = (uint8_t)(old & ~value);  /* bits cleared */
        imr_model[port] = value;
        /* Any bit unmasked while IF==0 latches a pending edge. */
        pending_latch |= (unsigned long)newly_enabled
                         << (port == SLAVE_IMR_PORT ? 16 : 0);
    } else {
        imr_model[port] = value;
    }
}

uint8_t arch_inb(uint16_t port) { return imr_model[port]; }

void arch_this_cpu_set(unsigned id) { selected_cpu = id; }

/* APIC-surface link stubs: this suite exercises the PIC fallback path
 * only (ioapic_available() is weak-false), the IOAPIC objects merely
 * need to link. */
apic_info_t apic_info;
void lapic_eoi(void) {}
/* ACPI/MADT data query (irq_route.c init): no ACPI tables on the host. */
uint8_t irq_trigger_for_gsi(uint32_t gsi) { (void)gsi; return 0; }
uint32_t lapic_read(uint32_t offset) { (void)offset; return 0; }
void lapic_write(uint32_t offset, uint32_t value) { (void)offset; (void)value; }

/* Exception handler surface (arch dispatch.c): unreachable in this
 * suite — any call is a test-protocol violation. */
unsigned long boot2_lapic_timer_irq_calls;
void x86_handle_lapic_timer_irq(pt_regs_t *r) { (void)r; boot2_lapic_timer_irq_calls++; }
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

/* Early-console boundary (bad_event.c) — captured, never printed. */
unsigned long boot2_early_puts_calls;
void arch_early_puts(const char *s) { (void)s; boot2_early_puts_calls++; }
void arch_early_putc(char c) { (void)c; boot2_early_puts_calls++; }
