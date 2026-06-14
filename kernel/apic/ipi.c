#include <kernel/ipi.h>
#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <kernel/printk.h>
#include <kernel/vmm.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/regs.h>

// Assembly stubs (defined below)
extern void ipi_tlb_stub(void);
extern void ipi_resched_stub(void);

// ── C handlers (non-static — called from asm stubs) ─────

void ipi_tlb_handler(pt_regs_t *regs __attribute__((unused)),
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

void ipi_resched_handler(pt_regs_t *regs __attribute__((unused)),
                         uint64_t error_code __attribute__((unused)))
{
    this_cpu()->need_resched = 1;
    lapic_eoi();
}

// ── Assembly stubs: save regs → call C → ret_from_intr ───

__asm__(
    ".text\n\t"
    ".globl ipi_tlb_stub\n\t"
    "ipi_tlb_stub:\n\t"
    "cld\n\t"
    "pushq $0\n\t"        // ERRCODE  (0x90)
    "pushq $0\n\t"        // FUNC     (0x88)
    "pushq %rax\n\t"      // RAX      (0x80)
    "movq %es, %rax\n\t"
    "pushq %rax\n\t"      // ES       (0x78)
    "movq %ds, %rax\n\t"
    "pushq %rax\n\t"      // DS       (0x70)
    "xorq %rax, %rax\n\t"
    "pushq %rbp\n\t"      // RBP      (0x68)
    "pushq %rdi\n\t"      // RDI      (0x60)
    "pushq %rsi\n\t"      // RSI      (0x58)
    "pushq %rdx\n\t"      // RDX      (0x50)
    "pushq %rcx\n\t"      // RCX      (0x48)
    "pushq %rbx\n\t"      // RBX      (0x40)
    "pushq %r8\n\t"       // R8       (0x38)
    "pushq %r9\n\t"       // R9       (0x30)
    "pushq %r10\n\t"      // R10      (0x28)
    "pushq %r11\n\t"      // R11      (0x20)
    "pushq %r12\n\t"      // R12      (0x18)
    "pushq %r13\n\t"      // R13      (0x10)
    "pushq %r14\n\t"      // R14      (0x08)
    "pushq %r15\n\t"      // R15      (0x00)
    "movq $0x10, %rdi\n\t"
    "movq %rdi, %ds\n\t"
    "movq %rdi, %es\n\t"
    "movq %rsp, %rdi\n\t"
    "xorq %rsi, %rsi\n\t"
    "call ipi_tlb_handler\n\t"
    "jmp ret_from_intr\n\t"
);

__asm__(
    ".text\n\t"
    ".globl ipi_resched_stub\n\t"
    "ipi_resched_stub:\n\t"
    "cld\n\t"
    "pushq $0\n\t"        // ERRCODE  (0x90)
    "pushq $0\n\t"        // FUNC     (0x88)
    "pushq %rax\n\t"      // RAX      (0x80)
    "movq %es, %rax\n\t"
    "pushq %rax\n\t"      // ES       (0x78)
    "movq %ds, %rax\n\t"
    "pushq %rax\n\t"      // DS       (0x70)
    "xorq %rax, %rax\n\t"
    "pushq %rbp\n\t"
    "pushq %rdi\n\t"
    "pushq %rsi\n\t"
    "pushq %rdx\n\t"
    "pushq %rcx\n\t"
    "pushq %rbx\n\t"
    "pushq %r8\n\t"
    "pushq %r9\n\t"
    "pushq %r10\n\t"
    "pushq %r11\n\t"
    "pushq %r12\n\t"
    "pushq %r13\n\t"
    "pushq %r14\n\t"
    "pushq %r15\n\t"
    "movq $0x10, %rdi\n\t"
    "movq %rdi, %ds\n\t"
    "movq %rdi, %es\n\t"
    "movq %rsp, %rdi\n\t"
    "xorq %rsi, %rsi\n\t"
    "call ipi_resched_handler\n\t"
    "jmp ret_from_intr\n\t"
);

// ── Register in IDT ───────────────────────────────────────

void ipi_init(void)
{
    set_intr_gate(IPI_VECTOR_TLB,     0, ipi_tlb_stub);
    set_intr_gate(IPI_VECTOR_RESCHED, 0, ipi_resched_stub);
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
