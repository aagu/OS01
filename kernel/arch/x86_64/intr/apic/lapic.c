#include <intr/apic.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <core/debug.h>
#include <arch/msr.h>
#include <arch/cpuid.h>
#include <arch/x86_64/gate.h>
#include <arch/irq.h>
#include <arch/thread.h>
#include <stdint.h>

// ──────────────────────────────────────────────
//  Internal state
// ──────────────────────────────────────────────

static int lapic_ready = 0;

// Forward declarations from sibling compilation units
extern int ioapic_init(void);

// ──────────────────────────────────────────────
//  LAPIC MMIO register access
//  Assumes the LAPIC base has been mapped at Phy_To_Virt(base).
//  Registers are 32-bit, 128-bit (16-byte) aligned.
// ──────────────────────────────────────────────

uint32_t lapic_read(uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)Phy_To_Virt(apic_info.lapic_base + offset);
    return *reg;
}

void lapic_write(uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)Phy_To_Virt(apic_info.lapic_base + offset);
    *reg = value;
}

// ──────────────────────────────────────────────
//  EOI — signal end-of-interrupt to LAPIC
// ──────────────────────────────────────────────

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

// ──────────────────────────────────────────────
//  Spurious interrupt stub
//  LAPIC may deliver spurious interrupts — no
//  EOI needed, just return via iretq.  Must be
//  raw assembly because set_intr_gate puts the
//  address directly in the IDT: a bare C function
//  would return via `ret` and leak CS+RFLAGS on
//  the stack (16 bytes per occurrence).
// ──────────────────────────────────────────────

extern void lapic_spurious_stub(void);

__asm__(
    ".globl lapic_spurious_stub\n\t"
    "lapic_spurious_stub:\n\t"
    "iretq\n\t"
);

// ──────────────────────────────────────────────
//  Initialization
// ──────────────────────────────────────────────

int lapic_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    // 1. Verify LAPIC support via CPUID leaf 1, EDX bit 9
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & CPUID_FEAT_EDX_APIC)) {
        debug_irq("APIC: CPUID reports no APIC support\n");
        return 0;
    }
    debug_irq("APIC: CPUID APIC feature present\n");

    // 2. Read IA32_APIC_BASE MSR for the LAPIC base address
    uint64_t apic_base_msr = rdmsr(IA32_APIC_BASE);
    uint64_t msr_base = apic_base_msr & APIC_BASE_ADDR_MASK;

    // If MADT provided a different base, prefer it (should match in practice)
    if (apic_info.lapic_base != 0 && apic_info.lapic_base != msr_base) {
        debug_irq("APIC: MADT base (%#010lx) differs from MSR (%#010lx), using MADT\n",
                      apic_info.lapic_base, msr_base);
    }
    if (apic_info.lapic_base == 0) {
        apic_info.lapic_base = msr_base;
    }
    // Default fallback
    if (apic_info.lapic_base == 0) {
        apic_info.lapic_base = DEFAULT_LAPIC_BASE;
    }

    debug_irq("APIC: LAPIC base = %#010lx\n", apic_info.lapic_base);

    // 3. Map the LAPIC MMIO region (2MB page, uncacheable)
    uint64_t lapic_page = apic_info.lapic_base & PAGE_2M_MASK;
    vmm_map_page(kernel_map, lapic_page,
                 (uintptr_t)Phy_To_Virt(lapic_page), PAGE_KERNEL_PMD_NOCACHE);

    // 4. Ensure APIC is enabled in the MSR (set bit 11)
    if (!(apic_base_msr & APIC_BASE_ENABLE)) {
        wrmsr(IA32_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE);
        debug_irq("APIC: enabled via IA32_APIC_BASE MSR\n");
    }

    // 5. Set Spurious Interrupt Vector Register
    //    bit 8 = APIC Software Enable, bits 0-7 = spurious vector (0xFF)
    lapic_write(LAPIC_SVR, APIC_SPURIOUS_VAL);
    debug_irq("APIC: SVR set to %#x (spurious vector %#x)\n",
                  APIC_SPURIOUS_VAL, SPURIOUS_VECTOR);

    // 6. Set Task Priority Register to 0 (accept all interrupts)
    lapic_write(LAPIC_TPR, 0);

    // 7. Configure Logical Destination Register and Destination Format Register
    //    Use flat model: DFR = all 1s, LDR = logical ID = 1 for BSP
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_LDR, (1 << 24));   // logical APIC ID for BSP

    // 8. Mask all LVT entries initially
    lapic_write(LAPIC_LVT_TIMER,   LVT_MASK);
    lapic_write(LAPIC_LVT_LINT0,   LVT_MASK | LVT_DELIVERY_EXTINT);  // LINT0 = ExtINT for PIC compatibility
    lapic_write(LAPIC_LVT_LINT1,   LVT_MASK);
    lapic_write(LAPIC_LVT_ERROR,   LVT_MASK);
    lapic_write(LAPIC_LVT_PERF,    LVT_MASK);
    lapic_write(LAPIC_LVT_THERMAL, LVT_MASK);

    // 9. Clear Error Status Register
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);   // write twice — first write clears, second re-clears

    // 10. EOI any stale pending interrupt
    lapic_write(LAPIC_EOI, 0);

    // 11. Install spurious interrupt stub at the spurious vector
    set_intr_gate_raw(SPURIOUS_VECTOR, 0, lapic_spurious_stub);

    lapic_ready = 1;
    debug_irq("APIC: LAPIC initialized, id=%#x\n", lapic_read(LAPIC_ID));

    return 1;
}

int apic_available(void)
{
    return lapic_ready;
}

// ──────────────────────────────────────────────
//  APIC subsystem init: ACPI → LAPIC → IOAPIC
// ──────────────────────────────────────────────

void apic_init(uint64_t rsdp_phys)
{
    debug_irq("APIC: initializing (RSDP=%#010lx)\n", rsdp_phys);

    // 1. Parse ACPI tables to find LAPIC / IOAPIC info
    if (!acpi_parse(rsdp_phys)) {
        debug_irq("APIC: ACPI parse failed — using default LAPIC base\n");
        apic_info.lapic_base = DEFAULT_LAPIC_BASE;
    }

    // 2. Initialize the Local APIC
    if (!lapic_init()) {
        debug_irq("APIC: LAPIC init failed — staying on PIC\n");
        return;
    }

    // 3. Initialize I/O APICs
    ioapic_init();
}

#include <subsys/subsys.h>
// Register this driver into the platform's subsystem table. The
// _register function is collected by arch_register_subsys() at boot via
// the .subsys_init linker section; it calls register_subsys() to queue
// the init wrapper for subsys_init_phase() to run later. The split
// (register vs init) ensures each driver's init runs ONCE — at the
// right phase — and never during the .subsys_init pass (which would
// race init order with sibling drivers).
// This file lives under kernel/arch/x86_64/intr/ so it is x86_64-only by
// directory placement; the previous #ifdef __x86_64__ guard is now dead
// and was removed when the file was relocated (AAGU-4.5).
extern uint64_t arch_boot_rsdp;
static int _apic_init_wrapper(void)
{
    apic_init(arch_boot_rsdp);
    return 0;
}
static int _apic_register(void)
{
    register_subsys("apic", _apic_init_wrapper, SUBSYS_PHASE_3, 0);
    return 0;
}
SUBSYS_INITCALL(_apic_register);
