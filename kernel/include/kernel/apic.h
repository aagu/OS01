#ifndef _KERNEL_APIC_H
#define _KERNEL_APIC_H

#include <stdint.h>
#include <kernel/interrupt.h>
#include <kernel/arch/x86_64/cpu.h>

// ──────────────────────────────────────────────
//  LAPIC Register Offsets (128-bit aligned, 32-bit access)
// ──────────────────────────────────────────────

#define LAPIC_ID            0x020   // Local APIC ID Register (R/W)
#define LAPIC_VERSION       0x030   // Local APIC Version Register (R/O)
#define LAPIC_TPR           0x080   // Task Priority Register (R/W)
#define LAPIC_APR           0x090   // Arbitration Priority Register (R/O)
#define LAPIC_PPR           0x0A0   // Processor Priority Register (R/O)
#define LAPIC_EOI           0x0B0   // End-of-Interrupt Register (W/O)
#define LAPIC_RRD           0x0C0   // Remote Read Register (R/O)
#define LAPIC_LDR           0x0D0   // Logical Destination Register (R/W)
#define LAPIC_DFR           0x0E0   // Destination Format Register (R/W)
#define LAPIC_SVR           0x0F0   // Spurious Interrupt Vector Register (R/W)
#define LAPIC_ISR_BASE      0x100   // In-Service Register base (8 x 32-bit)
#define LAPIC_TMR_BASE      0x180   // Trigger Mode Register base (8 x 32-bit)
#define LAPIC_IRR_BASE      0x200   // Interrupt Request Register base (8 x 32-bit)
#define LAPIC_ESR           0x280   // Error Status Register (R/W)
#define LAPIC_LVT_CMCI      0x2F0   // LVT Corrected Machine Check Interrupt
#define LAPIC_ICR_LOW       0x300   // Interrupt Command Register low
#define LAPIC_ICR_HIGH      0x310   // Interrupt Command Register high
#define LAPIC_LVT_TIMER     0x320   // LVT Timer Register
#define LAPIC_LVT_THERMAL   0x330   // LVT Thermal Sensor Register
#define LAPIC_LVT_PERF      0x340   // LVT Performance Monitor Counters Register
#define LAPIC_LVT_LINT0     0x350   // LVT LINT0 Register
#define LAPIC_LVT_LINT1     0x360   // LVT LINT1 Register
#define LAPIC_LVT_ERROR     0x370   // LVT Error Register
#define LAPIC_TIMER_INIT    0x380   // Timer Initial Count Register
#define LAPIC_TIMER_CUR     0x390   // Timer Current Count Register (R/O)
#define LAPIC_TIMER_DIV     0x3E0   // Timer Divide Configuration Register

// ──────────────────────────────────────────────
//  Spurious Interrupt Vector Register (SVR) bits
// ──────────────────────────────────────────────

#define APIC_SVR_ENABLE      (1 << 8)   // APIC Software Enable

// ──────────────────────────────────────────────
//  LVT entry bit definitions
// ──────────────────────────────────────────────

#define LVT_MASK             (1 << 16)  // Mask this interrupt source
#define LVT_DELIVERY_FIXED   (0 << 8)
#define LVT_DELIVERY_SMI     (2 << 8)
#define LVT_DELIVERY_NMI     (4 << 8)
#define LVT_DELIVERY_EXTINT  (7 << 8)
#define LVT_DELIVERY_INIT    (5 << 8)

// ──────────────────────────────────────────────
//  I/O APIC Register Indexes
// ──────────────────────────────────────────────

#define IOAPIC_REG_ID        0x00   // I/O APIC ID
#define IOAPIC_REG_VER       0x01   // I/O APIC Version
#define IOAPIC_REG_ARB       0x02   // Arbitration ID
#define IOAPIC_REG_REDTBL(n) (0x10 + 2 * (n))  // Redirection Table entry n (low)

// ──────────────────────────────────────────────
//  I/O APIC Version register
// ──────────────────────────────────────────────

#define IOAPIC_VER_MAX_REDIR(ver) (((ver) >> 16) & 0xFF)  // Max Redirection Entry

// ──────────────────────────────────────────────
//  I/O APIC Redirection Entry (low 32 bits)
// ──────────────────────────────────────────────

#define IOAPIC_RED_MASK       (1 << 16)  // Mask (1 = disabled)
#define IOAPIC_RED_TRIG_LEVEL (1 << 15)  // 0 = edge, 1 = level
#define IOAPIC_RED_POL_LOW    (1 << 13)  // 0 = active high, 1 = active low
#define IOAPIC_RED_DEST_LOG   (1 << 11)  // 0 = physical, 1 = logical
#define IOAPIC_RED_DEL_FIXED  (0 << 8)   // Fixed delivery mode
#define IOAPIC_RED_DEL_LOWPRI (1 << 8)   // Lowest priority

// ──────────────────────────────────────────────
//  ICR (Interrupt Command Register) helpers
// ──────────────────────────────────────────────

#define ICR_DEST_SHIFT        24         // Destination field shift (high 32 bits)
#define ICR_VECTOR_NMI        0x400      // Delivery mode = NMI in shorthand
#define ICR_DELIVERY_FIXED    (0 << 8)
#define ICR_DELIVERY_INIT     (5 << 8)
#define ICR_DELIVERY_STARTUP  (6 << 8)
#define ICR_DEST_PHYSICAL     (0 << 11)
#define ICR_LEVEL_ASSERT      (1 << 14)
#define ICR_TRIG_LEVEL        (1 << 15)
#define ICR_STATUS_PENDING    (1 << 12)

// ──────────────────────────────────────────────
//  ISO (Interrupt Source Override) flags
// ──────────────────────────────────────────────

#define ISO_POLARITY_MASK         0x3
#define ISO_POLARITY_CONF_BUS     0x0        // Conforms to bus (ISA = active high)
#define ISO_POLARITY_ACTIVE_HIGH  0x1        // Active high
#define ISO_POLARITY_RESERVED     0x2
#define ISO_POLARITY_ACTIVE_LOW   0x3        // Active low

#define ISO_TRIGGER_MASK          0xC
#define ISO_TRIGGER_CONF_BUS      0x0        // Conforms to bus (ISA = edge)
#define ISO_TRIGGER_EDGE          0x4
#define ISO_TRIGGER_RESERVED      0x8
#define ISO_TRIGGER_LEVEL         0xC

// ──────────────────────────────────────────────
//  Default values
// ──────────────────────────────────────────────

#define DEFAULT_LAPIC_BASE    0xFEE00000ULL  // Default LAPIC physical base
#define SPURIOUS_VECTOR       0xFF           // Spurious interrupt vector
#define APIC_SPURIOUS_VAL     (SPURIOUS_VECTOR | APIC_SVR_ENABLE)  // 0x1FF

// ──────────────────────────────────────────────
//  MADT entry types
// ──────────────────────────────────────────────

#define MADT_TYPE_LAPIC       0x00   // Processor Local APIC
#define MADT_TYPE_IOAPIC      0x01   // I/O APIC
#define MADT_TYPE_ISO         0x02   // Interrupt Source Override
#define MADT_TYPE_NMI         0x04   // Local APIC NMI
#define MADT_TYPE_X2APIC      0x09   // Processor Local x2APIC (ACPI 5.0+)

// ──────────────────────────────────────────────
//  Data structures
// ──────────────────────────────────────────────

// Buffer sizes for MADT parsing.
// MAX_LAPICS must be >= NR_CPUS so we never miss an enabled CPU.
#define MAX_LAPICS        NR_CPUS
#define MAX_IOAPICS       4
#define MAX_ISO_OVERRIDES 16

typedef struct {
    uint32_t apic_id;          // APIC ID (8-bit for LAPIC, 32-bit for x2APIC)
    uint32_t acpi_id;          // ACPI processor UID (8-bit or 32-bit depending on type)
    uint32_t flags;            // bit 0 = enabled
} lapic_entry_t;

typedef struct {
    uint8_t  apic_id;
    uint32_t mmio_base;    // physical address
    uint32_t gsi_base;     // starting GSI number
    uint32_t max_redir;    // max redirection entries (populated by ioapic_init)
} ioapic_entry_t;

typedef struct {
    uint8_t  bus_source;   // ISA IRQ number
    uint8_t  irq_source;
    uint32_t gsi;
    uint16_t flags;        // polarity (bits 0-1), trigger mode (bits 2-3)
} iso_override_t;

typedef struct {
    uint64_t lapic_base;          // local APIC MMIO physical base (from MADT or default)
    uint32_t lapic_count;
    lapic_entry_t lapics[MAX_LAPICS];
    uint32_t ioapic_count;
    ioapic_entry_t ioapics[MAX_IOAPICS];
    uint32_t iso_count;
    iso_override_t isos[MAX_ISO_OVERRIDES];
} apic_info_t;

extern apic_info_t apic_info;

// ──────────────────────────────────────────────
//  Public API
// ──────────────────────────────────────────────

// Initialize the APIC subsystem: ACPI parse → LAPIC init → I/O APIC init
void apic_init(uint64_t rsdp_phys);

// Returns 1 if APIC is available and initialized
int apic_available(void);

// Parse ACPI tables from RSDP (fills apic_info)
int acpi_parse(uint64_t rsdp_phys);

// LAPIC: init / EOI
int  lapic_init(void);
void lapic_eoi(void);

// LAPIC: read/write a 32-bit MMIO register
uint32_t lapic_read(uint32_t offset);
void lapic_write(uint32_t offset, uint32_t value);

// Get the I/O APIC hw_int_controller_t for use with register_irq()
hw_int_controller_t * get_ioapic_controller(void);

// Diagnostics: print all IOAPIC redirection entries
void ioapic_dump_entries(void);

// Look up the GSI for a given ISA IRQ (applying ISO overrides)
uint32_t isa_irq_to_gsi(uint8_t isa_irq);

// ── LAPIC per-CPU timer ────────────────────────
// Calibrate against PIT (must run once on BSP after PIT is active).
void lapic_timer_calibrate(void);
// Start periodic timer at the given frequency on the executing CPU.
void lapic_timer_start(uint32_t freq_hz);
// One-shot BSP setup: register IDT gate, calibrate, start at 100 Hz.
void lapic_timer_init(void);

#endif // _KERNEL_APIC_H
