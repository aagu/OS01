/* aarch64 phase 1: EL1 IRQ dispatch (Task 2.2) + EL1 sync diagnostics
 * (spec 2026-09-30-aarch64-el1-sync-diagnostics-design.md, Task 1).
 *
 * Interrupt dispatch phase 1 (spec 2026-10-23-intr-dispatch-phase1
 * §6.2): both EL1h C landing points are now entry adapters.  Each
 * builds a fully initialized `struct intr_event` ON ITS STACK and hands
 * it to the common stateless dispatcher `arch_intr_dispatch()`; the
 * event's lifetime covers only that call.
 *
 *   entry.S `el1_irq_entry` (offset 0x280) → `el1_irq(regs)`: decode
 *   the IRQ event, dispatch, return normally — the assembly restore and
 *   `eret` are unchanged.  IAR/EOI stay with the GIC backend reached
 *   through the aarch64 IRQ backend (arch/aarch64/intr/dispatch.c).
 *
 *   entry.S `el1_sync_entry` (offset 0x200) → `aarch64_el1_sync(regs,
 *   esr, far)`: decode the sync event from the raw ESR/FAR snapshot,
 *   dispatch, and NEVER return — the sync backend ends in the original
 *   fatal diagnostic or bad-event, and an unexpected return of the
 *   common entry itself goes straight to arch_intr_bad_event().
 *
 * `aarch64_el1_sync_fatal` keeps its frozen output contract (spec §8):
 * it masks IRQ then the full DAIF, claims the BSS `first_fault` flag via
 * arch_atomic_cas, prints one fixed-width `[aarch64-sync] FATAL …` line
 * (`far=n/a` when FAR is invalid), and parks the CPU in `wfi`.
 *
 * `arch_install_exception_vectors()` stays no-op: VBAR_EL1 is installed
 * by main.c (msr vbar_el1, ...) immediately after pl011_init, which is
 * why this file does not touch the system register.
 */

#include <stdint.h>
#include <stdbool.h>
#include <arch/regs.h>
#include <arch/intr.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/intr_dispatch.h>
#include <arch/aarch64/gic.h>
#include <arch/mmu.h>
#include <arch/atomic.h>
#include <arch/cpu.h>
#include <arch/early_print.h>
#include <arch/irq.h>
#include <percpu/percpu.h>

/* entry.S el1_irq_entry 的 C 落点（IRQ 适配器）：构造 IRQ 事件 → 共同
 * 分派 → 正常返回，汇编恢复/eret 不变。 */
void el1_irq(struct pt_regs *regs)
{
    struct intr_event ev;

    aarch64_intr_decode_irq(&ev, regs);
    arch_intr_dispatch(&ev, regs);
}

/* entry.S el1_sync_entry 的 C 落点（sync 适配器，offset 0x200）。
 *
 * noreturn：后端终点是原 fatal 诊断或 bad-event；共同入口按整体定义
 * 可返回，若它意外返回则立即执行 bad-event 停机（spec §6.2），汇编的
 * DAIF+WFI 兜底保持不可达但保留。
 *
 * 早期可达链：no_stack_protector（canary prologue 依赖的 guard reload
 * 在 per-CPU 初始化前不可用）。 */
__attribute__((noreturn, no_stack_protector))
void aarch64_el1_sync(pt_regs_t *regs, uint64_t esr, uint64_t far)
{
    struct intr_event ev;

    aarch64_intr_decode_sync(&ev, regs, esr, far);
    arch_intr_dispatch(&ev, regs);

    /* Unreachable in practice: the sync backend never returns, and a
     * contract-broken chain already parked in bad-event.  Kept as the
     * mandated fail-stop if the common entry ever came back. */
    arch_intr_bad_event(&ev, regs);
}

/* ──────────────────────────────────────────────────────────────
 *  M3 (Task 10): SGI 3 = IPI_VECTOR_TLB shootdown handler.
 *  Registered once by gic_init() (the handler table is global, so the
 *  registration covers BSP and APs alike); gic_cpu_init() enables the
 *  banked SGI 3 line on every CPU.
 *
 *  3-arg signature (kernel/include/arch/aarch64/gic.h:15); the handler
 *  must NOT EOI — gic_dev_dispatch (kernel/arch/aarch64/intr/gic_driver.c)
 *  writes back the full IAR to EOIR after the handler returns.
 * ────────────────────────────────────────────────────────────── */
void aarch64_tlb_sgi_handler(uint32_t intid, uint64_t param,
                             struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    /* vmalle1 + dsb sy + isb */
    arch_flush_tlb_all();
    /* ACK: bump this CPU's generation counter so the initiator's
     * tlb_ack_gen wait observes completion. Handler context is
     * serialized per-CPU and IRQ-masked; cpu_id() is valid here
     * (IRQs are only unmasked after percpu_install_gs/percpu_init). */
    __atomic_fetch_add(&percpu_data[cpu_id()].tlb_ack_gen, 1,
                       __ATOMIC_RELEASE);
}

void arch_install_exception_vectors(void)
{
    /* no-op; VBAR set in main.c via entry.S's exception_vectors */
}

/* ──────────────────────────────────────────────────────────────
 *  EL1h sync diagnostics — Task 1 (spec §3, §4)
 *
 *  The diagnostic must remain usable from the earliest point after
 *  high-half VBAR is installed: no malloc, no spinlocks, no scheduler,
 *  no GIC, no ordinary logger.  Only the lock-free PL011-backed
 *  arch_early_put{cs} facade and arch_atomic_cas may be touched.
 *
 *  IRQs are masked twice on purpose: `arch_local_irq_disable` covers
 *  the IRQ bit via the documented daifset #2 immediate, then a full
 *  `DAIFSet #0xf` brings D/A/F up alongside IRQ.  Spec §4 requires
 *  the wider mask so a nested SError or FIQ cannot reorder the
 *  diagnostic against the caller.
 *
 *  First-fault claim: BSS `first_fault` starts at 0 (zero-init).  The
 *  first reporter wins via arch_atomic_cas and prints the spec line.
 *  Losers halt silently to avoid racing the UART.  SMP-safe by virtue
 *  of LDAXR/STLXR (acquire-release) inside arch_atomic_cas.
 * ────────────────────────────────────────────────────────────── */

/* BSS (zero-initialised).  Volatile because the writer and reader live
 * in the same TU but the diagnostic runs with IRQs off and the CAS
 * must not be elided. */
volatile uint64_t first_fault;

/* Local output helpers — no_stack_protector so the canary prologue is
 * not emitted (this path is noreturn-safe and stack canaries would
 * require a guard reload we deliberately cannot depend on). */
static __attribute__((no_stack_protector))
void put_hex_nibble(uint64_t v)
{
    unsigned n = (unsigned)(v & 0xfu);
    arch_early_putc((char)(n < 10 ? '0' + n : 'a' + (n - 10)));
}

static __attribute__((no_stack_protector))
void put_hex_u64(uint64_t v, unsigned digits)
{
    for (unsigned i = digits; i > 0; --i) {
        unsigned shift = (i - 1) * 4;
        put_hex_nibble(v >> shift);
    }
}

static __attribute__((no_stack_protector))
void put_hex_u8(unsigned v)
{
    put_hex_nibble(v >> 4);
    put_hex_nibble(v);
}

static __attribute__((no_stack_protector))
void put_mpidr(uint64_t v)
{
    put_hex_u64(v, 16);
}

static __attribute__((no_stack_protector))
void put_reg64(uint64_t v)
{
    put_hex_u64(v, 16);
}

__attribute__((noreturn, no_stack_protector, cold))
void aarch64_el1_sync_fatal(struct pt_regs *regs, uint64_t esr, uint64_t far)
{
    /* 1. Mask IRQ, then the wider DAIF (D/A/F/I) — spec §4. */
    arch_local_irq_disable();
    __asm__ __volatile__("msr daifset, #0xf" ::: "memory");

    /* 2. Read MPIDR_EL1 BEFORE any other ESR-dependent work.  Reading
     *    system registers is fine here (DAIF is masked) and gives a
     *    stable hardware CPU identity without depending on TPIDR_EL1. */
    uint64_t mpidr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));

    /* 3. Compute EC and FAR-validity once, then atomic-claim.  Losers
     *    bypass the UART to avoid tearing the line. */
    unsigned ec = (unsigned)((esr >> 26) & 0x3fu);
    bool far_valid = aarch64_sync_far_valid(esr);
    int winner = arch_atomic_cas(&first_fault, 0, 1);
    if (!winner) {
        for (;;) arch_cpu_halt();
    }

    /* 4. First reporter: print one line.  Fixed-width lowercase hex,
     *    fields in spec order, CR/LF once at end.  The format is
     *    consumed by qemutests/aarch64_sync_fault.py::sync_fault_evidence
     *    (Task 3), so width/order/separators are part of the ABI. */
    arch_early_puts("[aarch64-sync] FATAL mpidr=0x");
    put_mpidr(mpidr);
    arch_early_puts(" ec=0x");
    put_hex_u8(ec);
    arch_early_puts(" esr=0x");
    put_reg64(esr);
    arch_early_puts(" elr=0x");
    put_reg64(regs->elr_el1);
    arch_early_puts(" spsr=0x");
    put_reg64(regs->spsr_el1);
    arch_early_puts(" far=");
    if (far_valid) {
        arch_early_puts("0x");
        put_reg64(far);
    } else {
        arch_early_puts("n/a");
    }
    arch_early_putc('\n');

    /* 5. Park.  Diagnostic is delivered; never eret from this path. */
    for (;;) arch_cpu_halt();
    __builtin_unreachable();
}
