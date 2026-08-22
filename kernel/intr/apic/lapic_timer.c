#include <kernel/apic.h>
#include <kernel/percpu.h>
#include <kernel/debug.h>
#include <kernel/arch/irq.h>
#include <kernel/arch/thread.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/x86_64/gate.h>
#include <device/timer.h>
#include <kernel/softirq.h>
#include <kernel/clockevent.h>   // tick_handler()
#include <kernel/clocksource.h>  // clocksource_freq_hz()
#include <stdbool.h>

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
//     1. Calibrates the LAPIC timer (RTC PIE joint result or TSC
//        window; measures decrement rate per 10ms).
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

// RTC PIE 联合校准测得的 LAPIC 频率，供 lapic_timer_calibrate 优先复用。
static uint64_t lapic_premeasured_hz = 0;

void lapic_timer_set_premeasured(uint64_t hz)
{
    lapic_premeasured_hz = hz;
}

// ── Calibration ───────────────────────────────────
// §5.1：优先复用 RTC PIE 联合结果（更稳）；否则 TSC 窗口（~10ms）。

void lapic_timer_calibrate(void)
{
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);

    uint64_t tsc_hz = clocksource_freq_hz();

    // 1. 优先复用 RTC PIE 联合结果（250ms 更稳）。
    if (lapic_premeasured_hz != 0) {
        lapic_timer_hz = lapic_premeasured_hz;
        lapic_timer_divisor = 0;   // 联合校准用 ÷2（DIV=0）
        debug_sched("LAPIC timer: %lu Hz (from RTC PIE)\n",
                    (unsigned long)lapic_timer_hz);
        return;
    }

    // 2. 回落 TSC 窗口（~10ms）。
    if (tsc_hz == 0) {
        lapic_timer_hz = 0;   // 无法校准 → 退 PIT
        debug_sched("LAPIC timer: no TSC freq, fallback PIT\n");
        return;
    }

    // divisor=0（÷2）。SDM 000b；⚠️ 原代码注释 "divide by 1" 是错的，
    // 实现时一并改正为 ÷2。
    lapic_timer_divisor = 0;   // ÷2
    lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    uint64_t tsc0 = rdtscp_serialized();
    while (rdtscp_serialized() - tsc0 < tsc_hz / 100)   // 10ms TSC 窗口
        arch_cpu_pause();

    uint32_t cur = lapic_read(LAPIC_TIMER_CUR);
    uint64_t elapsed = 0xFFFFFFFFULL - cur;

    // lapic_timer_hz 语义 = 递减率（÷2 已折算），不是真实频率。
    // elapsed 是 ÷2 后 10ms 递减量，除以 10ms 得递减率 → elapsed * 100（不 ×2）。
    // 若误写 *200 得真实频率，lapic_timer_start 的 init_count=hz/freq 会被 divisor
    // 再 ÷2 → 50Hz（正确是保持 ×100）。
    lapic_timer_hz = elapsed * 100;

    debug_sched("LAPIC timer: %u ticks/10ms → %lu Hz (div=%u, TSC)\n",
                (unsigned)elapsed, (unsigned long)lapic_timer_hz,
                (unsigned)lapic_timer_divisor);
}

// ── Start periodic timer ──────────────────────────
// Returns true if the timer was started, false if not calibrated
// (caller falls back to the PIT).  DIV is a per-LAPIC hardware register:
// the calibration-time value is re-written here for the current CPU
// (each AP's LAPIC resets to count_shift=0/÷1, so omitting it would run
// the AP at ÷1 with a ÷2-folded lapic_timer_hz → 2× frequency).

bool lapic_timer_start(uint32_t freq_hz)
{
    if (!lapic_timer_hz) {
        debug_sched("LAPIC timer: not calibrated, skipping start\n");
        return false;
    }

    uint32_t init_count = (uint32_t)(lapic_timer_hz / freq_hz);
    if (init_count == 0)
        init_count = 1;

    // 写 divisor 到当前 CPU 的 LAPIC 硬件寄存器（DIV 是 per-LAPIC 的，不是
    // 全局变量）。值仍是校准产物（不变），但必须每 CPU 各写一次：AP 的 LAPIC
    // 复位后 count_shift=0(÷1)，若不写则 AP 复用 ÷2 折算的 lapic_timer_hz
    // 却用 ÷1 硬件 → AP 频率 ×2。见 spec §7.2。
    // 必须先写 DIV 再写 INIT（先定 divisor，再装 count，保证 count 按正确 divisor 递减）。
    lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor);

    // LVT Timer: vector in bits 0–7, periodic mode in bit 17.
    // Delivery mode fixed (0), unmasked (bit 16 = 0).
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1 << 17));

    // Load initial count — the timer starts counting on write.
    lapic_write(LAPIC_TIMER_INIT, init_count);

    debug_sched("LAPIC timer: started at %u Hz (init_count=%u) on CPU %u\n",
                  (unsigned)freq_hz, init_count, (unsigned)cpu_id());
    return true;
}

// ── Per-CPU tick handler (called from assembly stub) ──────
// rdi = pt_regs *, rsi = 0 (no error code for interrupts).
// Sets need_resched and sends EOI.

void lapic_timer_handler(pt_regs_t *regs __attribute__((unused)),
                         uint64_t error_code __attribute__((unused)))
{
    if (cpu_id() == 0) {
        // BSP 驱动 jiffies + poll 扫描（tick_handler 内含 watchdog++/softirq）。
        tick_handler();
    } else {
        // AP 只做 need_resched + watchdog（BSP 的 tick_handler 已覆盖全局 jiffies）。
        this_cpu()->need_resched = 1;
        this_cpu()->watchdog_counter++;
        set_softirq_status(TIMER_SIRQ);
    }
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
    set_intr_gate_raw(LAPIC_TIMER_VECTOR, 0, lapic_timer_stub);

    // 校准（RTC PIE 联合结果优先，否则 TSC 窗口）。
    lapic_timer_calibrate();

    // Start is deferred to caller — GS base must be set first.
}
