#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <kernel/printk.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/regs.h>
#include <device/timer.h>

// Assembly stub created below
extern void lapic_timer_stub(void);

// ──────────────────────────────────────────────
//   LAPIC Timer — per-CPU scheduling tick
//
//   The PIT fires IRQ0→BSP only.  Every other CPU
//   needs its own timer to get scheduling ticks.
//   The LAPIC contains a per-core 32-bit countdown
//   timer that can be placed in periodic mode.
//
//   This file:
//     1. Calibrates the LAPIC timer against the PIT
//        (one-shot, measure ticks per 10ms).
//     2. Starts the timer in periodic mode at 100 Hz
//        for whichever CPU calls lapic_timer_start().
//     3. Provides a per-CPU tick handler that mirrors
//        the PIT handler (need_resched + softirq).
// ──────────────────────────────────────────────

// LAPIC timer interrupt vector — above the IRQ range (0x20–0x37).
#define LAPIC_TIMER_VECTOR  0x38

// Effective ticks per second after the divisor chosen at calibration.
static uint64_t lapic_timer_hz = 0;

// Divisor value programmed during calibration (kept across start).
static uint32_t lapic_timer_divisor = 0;

// ── Calibration ───────────────────────────────────
// Uses the PIT (already running at 100 Hz) as a
// reference to measure how many LAPIC timer ticks
// elapse in one jiffy (10 ms).

void lapic_timer_calibrate(void)
{
    // 1. Mask and stop the timer
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);

    // 2. Use divisor 0 (divide-by-1) for best precision.
    //    If the raw count overflows 32 bits at 100 Hz start,
    //    we fall back to divide-by-16.
    lapic_timer_divisor = 0;   // divide by 1
    lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor);

    // 3. Load maximum count and wait one PIT tick
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    uint64_t start = jiffies;
    while (jiffies == start)
        __asm__ __volatile__("pause");

    uint32_t cur_count = lapic_read(LAPIC_TIMER_CUR);
    uint32_t elapsed = 0xFFFFFFFF - cur_count;

    // 4. If elapsed is suspiciously near zero, the timer
    //    overflowed 32 bits in 10 ms — retry with divide-by-16.
    if (elapsed < 1000) {
        lapic_timer_divisor = 3;   // divide by 16
        lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor);

        lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

        start = jiffies;
        while (jiffies == start)
            __asm__ __volatile__("pause");

        cur_count = lapic_read(LAPIC_TIMER_CUR);
        elapsed = 0xFFFFFFFF - cur_count;
    }

    // 5. Compute effective frequency
    lapic_timer_hz = (uint64_t)elapsed * 100;

    serial_printk("LAPIC timer: %u ticks/10ms → %lu Hz (div=%u)\n",
                  elapsed, (unsigned long)lapic_timer_hz,
                  (unsigned)lapic_timer_divisor);
}

// ── Start periodic timer ──────────────────────────

void lapic_timer_start(uint32_t freq_hz)
{
    if (!lapic_timer_hz) {
        serial_printk("LAPIC timer: not calibrated, skipping start\n");
        return;
    }

    // Compute initial count for the target frequency.
    // The divisor was already written during calibration.
    uint32_t init_count = (uint32_t)(lapic_timer_hz / freq_hz);
    if (init_count == 0)
        init_count = 1;

    // LVT Timer: vector in bits 0–7, periodic mode in bit 17.
    // Delivery mode fixed (0), unmasked (bit 16 = 0).
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1 << 17));

    // Load initial count — the timer starts counting on write.
    lapic_write(LAPIC_TIMER_INIT, init_count);

    serial_printk("LAPIC timer: started at %u Hz (init_count=%u) on CPU %u\n",
                  (unsigned)freq_hz, init_count, (unsigned)cpu_id());
}

// ── Per-CPU tick handler (called from assembly stub) ──────
// rdi = pt_regs *, rsi = 0 (no error code for interrupts).
// Sets need_resched and sends EOI.

void lapic_timer_handler(pt_regs_t *regs __attribute__((unused)),
                         uint64_t error_code __attribute__((unused)))
{
    this_cpu()->need_resched = 1;
    lapic_eoi();
}

// ── Assembly stub — bridges the CPU interrupt entry to C ──
// The CPU vectors through an interrupt gate.  On entry the
// stack has RFLAGS:CS:RIP.  We build a full pt_regs frame
// matching entry.S layout so ret_from_intr / RESTORE_ALL work.
//
// Critical: RESTORE_ALL does `addq $0x10, %rsp` before iretq,
// skipping the FUNC(0x88) and ERRCODE(0x90) slots.  Exception
// entries push those; for IRQ stubs we push zeros as placeholders.
__asm__(
    ".text\n\t"
    ".globl lapic_timer_stub\n\t"
    "lapic_timer_stub:\n\t"
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
    "movq %rsp, %rdi\n\t" // 1st arg → pt_regs *
    "xorq %rsi, %rsi\n\t" // 2nd arg → error_code = 0
    "call lapic_timer_handler\n\t"
    "jmp ret_from_intr\n\t"
);

// ── One-time init for the BSP ─────────────────────
// Registers the IDT gate and calibrates against PIT.
// Does NOT start the timer — call lapic_timer_start()
// after percpu_init() + percpu_install_gs() so that
// this_cpu() works in the interrupt handler.

void lapic_timer_init(void)
{
    // Register the assembly stub (NOT the C function directly —
    // the CPU interrupt gate entry doesn't set up pt_regs).
    set_intr_gate(LAPIC_TIMER_VECTOR, 0, lapic_timer_stub);

    // Calibrate using PIT as reference (PIT must already run).
    lapic_timer_calibrate();

    // Start is deferred to caller — GS base must be set first.
}
