#include <kernel/ipi.h>
#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <kernel/printk.h>
#include <kernel/vmm.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/regs.h>

// ── IPI handlers ──────────────────────────────────────────
// Called from IDT via entry.S error_code path.
// IST=0 → no stack switch, this_cpu() works.

// TLB shootdown handler — invalidate TLB and acknowledge.
static void ipi_tlb_handler(pt_regs_t *regs __attribute__((unused)),
                            uint64_t error_code __attribute__((unused)))
{
    percpu_t *cpu = this_cpu();
    if (cpu->tlb_wanted) {
        flush_tlb();
        __sync_synchronize();
        cpu->tlb_ack++;
        cpu->tlb_wanted = 0;
    }
    lapic_eoi();
}

// Reschedule handler — set the per-CPU flag so the target
// CPU picks up scheduling on the next interrupt return.
static void ipi_resched_handler(pt_regs_t *regs __attribute__((unused)),
                                uint64_t error_code __attribute__((unused)))
{
    this_cpu()->need_resched = 1;
    lapic_eoi();
}

// ── Register handlers in the IDT ──────────────────────────

void ipi_init(void)
{
    set_intr_gate(IPI_VECTOR_TLB,     0, ipi_tlb_handler);
    set_intr_gate(IPI_VECTOR_RESCHED, 0, ipi_resched_handler);
    serial_printk("IPI: vectors TLB=%#x RESCHED=%#x registered\n",
                  (unsigned)IPI_VECTOR_TLB, (unsigned)IPI_VECTOR_RESCHED);
}

// ── Send IPI ──────────────────────────────────────────────
// ICR is 64-bit split across two 32-bit MMIO registers:
//   HIGH (0x310): destination APIC ID in bits 24-31
//   LOW  (0x300): vector, delivery mode, trigger → WRITE TRIGGERS SEND

void ipi_send(uint32_t dest_apic_id, uint8_t vector)
{
    // Wait until ICR is idle (Delivery Status bit 12 = 0)
    uint32_t timeout = 10000;
    while (lapic_read(LAPIC_ICR_LOW) & ICR_STATUS_PENDING) {
        if (--timeout == 0) {
            serial_printk("IPI: ICR stuck pending\n");
            break;
        }
        __asm__ __volatile__("pause");
    }

    // Write high dword first (destination)
    uint32_t high = dest_apic_id << 24;
    lapic_write(LAPIC_ICR_HIGH, high);

    // Write low dword — THIS TRIGGERS THE SEND
    lapic_write(LAPIC_ICR_LOW, (uint32_t)vector | ICR_DEST_PHYSICAL);
}

void ipi_broadcast(uint8_t vector, int exclude_self)
{
    uint32_t me = cpu_id();
    for (uint32_t i = 0; i < num_cpus; i++) {
        if (!percpu_data[i].online)
            continue;
        if (exclude_self && i == me)
            continue;
        ipi_send(percpu_data[i].apic_id, vector);
    }
}
