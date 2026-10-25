// ── Isolated destructive dispatch probes (Task 5, spec §9.3) ─────
//
// Two cases, each compiled into its OWN kernel/image variant:
//
//   X86_INTR_FAULT_TEST_BP  — a kernel-mode `int3` that reaches the
//       ORIGINAL #BP fatal handler
//       (kernel/arch/x86_64/intr/trap.c::x86_handle_int3).  A resume
//       label sits IMMEDIATELY AFTER the int3, so the saved RIP of the
//       fatal diagnostic equals that label's address; the acceptance
//       runner resolves the label from the VARIANT ELF symbol table
//       and never from a hardcoded address.
//
//   X86_INTR_FAULT_TEST_NMI — the guest arms and waits; the runner
//       injects a REAL hardware NMI over QMP `inject-nmi`.  The wait
//       path holds NO lock the fatal path takes (see the lock-free
//       output helpers below): an injected NMI that landed inside the
//       ordinary logger's critical section would spin forever in
//       spin_lock_irqsave(&serial_lock) and the run would die without
//       producing a diagnostic.
//
// The file is added to the kernel source list ONLY when the
// probe-selection macro is present (kernel/Makefile), so the normal
// build contains no probe at all.

#include <stdint.h>

#include <core/printk.h>
#include <driver/serial.h>
#include <percpu/percpu.h>

#include <arch/x86_64/intr_fault_probe.h>

#ifndef OS01_STRINGIFY
#define OS01_STRINGIFY_(x) #x
#define OS01_STRINGIFY(x) OS01_STRINGIFY_(x)
#endif

// Spin budget between two heartbeats.  It only has to be "slow enough
// not to flood the UART"; the exact rate is not part of the contract.
// The delay comes BEFORE the first heartbeat on purpose: the runner
// injects the NMI within a few milliseconds of seeing the `armed`
// marker, so the injection lands in the quiet window and the fatal
// diagnostic starts on a line boundary.
#define X86_INTR_FAULT_SPINS 4000000UL

// ── Lock-free UART output (probe only) ─────────────────────────
//
// The probe's output path must hold no lock the fatal path takes: the
// original #BP / NMI fatal handler ends in log_err(), which takes the
// ordinary logger's log_lock and serial_lock.  An injected NMI that
// lands inside such a critical section spins forever in
// spin_lock_irqsave(&serial_lock) — the CPU hangs and the run dies
// without a diagnostic, which is exactly the misleading evidence the
// probe must avoid.
//
// This is not a theoretical hazard: the ordinary logger holds
// serial_lock for the WHOLE log line, and a 30-byte line takes ~8 ms
// to leave the UART at 38400 baud.  The runner injects the NMI within
// a few milliseconds of seeing the `armed` marker, so a logger-written
// marker would regularly drop the injection inside the logger's
// critical section.
//
// The probe therefore writes its markers with the raw UART writer and
// takes NO lock at all.  That is safe here and only here:
//   * the probe runs on the BSP before x86_64_boot_aps(), so there is
//     no second CPU that could write;
//   * the probe disables interrupts before its first byte, so no IRQ
//     handler can write;
//   * no task or thread exists yet.
//
// The deviation from write_serial_unlocked()'s documented contract
// ("the caller MUST hold serial_lock") is deliberate and confined to
// this file: taking the lock here is exactly what would deadlock the
// injected NMI.
static void probe_putc(char c)
{
    write_serial_unlocked(c);
}

static void probe_puts(const char *s)
{
    while (*s) {
        probe_putc(*s++);
    }
}

// Minimal unsigned decimal printer — the probe must not pull in the
// logger (and therefore its locks) for a three-digit counter.
static void probe_putu(unsigned long value)
{
    char buf[24];
    int i = 0;

    if (value == 0) {
        probe_putc('0');
        return;
    }
    while (value) {
        buf[i++] = (char)('0' + (int)(value % 10));
        value /= 10;
    }
    while (i) {
        probe_putc(buf[--i]);
    }
}

// Busy-wait between two heartbeats.  Interrupts stay disabled, so this
// is a pure delay; an injected NMI may land anywhere inside it and
// finds no lock held.
static void probe_delay(void)
{
    volatile unsigned long spins = X86_INTR_FAULT_SPINS;

    while (spins--) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

// The suite runs single-CPU: a second CPU would be able to interleave
// output with the fatal diagnostic and would make the guest ordering
// ambiguous.  The probe runs before the per-CPU bring-up, so num_cpus
// is still 0 — which IS the single-CPU proof at this point (no AP has
// been registered and none will be).  A failed precondition halts the
// CPU with a marker the acceptance runner rejects.
static void probe_check_precondition(void)
{
    if (num_cpus == 0) {
        return;
    }
    serial_printk("[x86-intr-fault] precondition FAIL: num_cpus=%u\n",
                  (unsigned)num_cpus);
    for (;;) {
        __asm__ __volatile__("hlt" ::: "memory");
    }
}

// The isolated variant token (KERNEL_VARIANT_NAME, the same macro the
// CSPRNG boot message uses) — printed by the probe itself so the
// acceptance runner can prove the running kernel IS the fault variant
// even though the probe runs before random_init().
static void probe_print_variant(void)
{
#ifdef KERNEL_VARIANT_NAME
    probe_puts("[x86-intr-fault] variant=" OS01_STRINGIFY(KERNEL_VARIANT_NAME) "\n");
#else
    probe_puts("[x86-intr-fault] variant=default\n");
#endif
}

#if defined(X86_INTR_FAULT_TEST_BP)

__attribute__((noinline))
static void bp_probe(void)
{
    probe_check_precondition();

    // Interrupts off BEFORE the first byte: from here on nothing but
    // this probe can write to the UART, so the marker can never be
    // interleaved with an IRQ handler's output.
    __asm__ __volatile__("cli" ::: "memory");
    probe_print_variant();
    probe_puts("[x86-intr-fault] bp armed\n");

    // The single-byte `int3` and the resume label that follows it.
    //
    // The CPU pushes the address of the instruction AFTER a trap, so the
    // saved RIP of the fatal diagnostic equals the label's address —
    // never the int3's own address.  The label is a GLOBAL symbol so
    // the acceptance runner can resolve it from the variant ELF.
    //
    // The block is volatile so the compiler can neither elide the int3
    // nor move it; `noinline` on the probe function keeps the label
    // defined exactly once.
    __asm__ __volatile__(
        "int3\n\t"
        ".globl x86_intr_fault_bp_resume\n\t"
        ".type x86_intr_fault_bp_resume, @function\n\t"
        "x86_intr_fault_bp_resume:\n\t"
        ::: "memory");

    // Unreachable: the #BP fatal path halts the CPU.  The marker exists
    // so the runner can prove the probe never returned.
    probe_puts("[x86-intr-fault] bp returned\n");
    for (;;) {
        __asm__ __volatile__("hlt" ::: "memory");
    }
}

#endif /* X86_INTR_FAULT_TEST_BP */

#if defined(X86_INTR_FAULT_TEST_NMI)

__attribute__((noinline))
static void nmi_probe(void)
{
    unsigned long seq = 0;

    probe_check_precondition();

    // Interrupts off BEFORE the first byte: from here on nothing but
    // this probe can write to the UART, and no IRQ handler can take
    // the logger lock while the injected NMI may land.
    __asm__ __volatile__("cli" ::: "memory");
    probe_print_variant();
    probe_puts("[x86-intr-fault] nmi armed\n");

    // The wait path: interrupts stay disabled, no logger lock is
    // held, no allocator, no scheduler.  The NMI is not maskable and
    // wakes the CPU out of the delay loop.
    for (;;) {
        // Delay first, heartbeat second: the runner injects within a
        // few milliseconds of the `armed` marker, so the injection
        // lands before the first heartbeat and the fatal diagnostic
        // starts on a line boundary.
        probe_delay();
        probe_puts("[x86-intr-fault] nmi heartbeat ");
        probe_putu(seq++);
        probe_putc('\n');
    }
}

#endif /* X86_INTR_FAULT_TEST_NMI */

void x86_intr_fault_probe_run(void)
{
#if defined(X86_INTR_FAULT_TEST_BP)
    bp_probe();
#elif defined(X86_INTR_FAULT_TEST_NMI)
    nmi_probe();
#else
#error "fault_probe.c compiled without a probe-selection macro"
#endif
}
