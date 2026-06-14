#include <kernel/ipi.h>
#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <kernel/printk.h>
#include <kernel/vmm.h>
#include <kernel/arch/x86_64/gate.h>

// ── File-scope: generate assembly stubs ──────────────────
// MUST be outside any function so the stub code is not
// executed as part of normal control flow.

DEFINE_INTR_STUB(tlb,     0x40);
DEFINE_INTR_STUB(resched, 0x41);

// ── C handlers ───────────────────────────────────────────
// Called from generic_intr_dispatch via the stubs above.

static void ipi_tlb_handler(uint64_t nr __attribute__((unused)),
                            uint64_t param __attribute__((unused)),
                            pt_regs_t *regs __attribute__((unused)))
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

static void ipi_resched_handler(uint64_t nr __attribute__((unused)),
                                uint64_t param __attribute__((unused)),
                                pt_regs_t *regs __attribute__((unused)))
{
    this_cpu()->need_resched = 1;
    lapic_eoi();
}

// ── Initialisation ───────────────────────────────────────

void ipi_init(void)
{
    REGISTER_INTR_HANDLER(tlb,     0x40, ipi_tlb_handler);
    REGISTER_INTR_HANDLER(resched, 0x41, ipi_resched_handler);
    serial_printk("IPI: vectors TLB=%#x RESCHED=%#x registered\n",
                  (unsigned)IPI_VECTOR_TLB, (unsigned)IPI_VECTOR_RESCHED);
}

// ── Send IPI ──────────────────────────────────────────────

void ipi_send(uint32_t dest_apic_id, uint8_t vector)
{
    uint32_t timeout = 10000;
    while (lapic_read(LAPIC_ICR_LOW) & ICR_STATUS_PENDING) {
        if (--timeout == 0) {
            serial_printk("IPI: ICR stuck pending\n");
            break;
        }
        __asm__ __volatile__("pause");
    }

    uint32_t high = dest_apic_id << 24;
    lapic_write(LAPIC_ICR_HIGH, high);
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
