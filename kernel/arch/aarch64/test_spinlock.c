/* aarch64 Task 2a single-core spinlock benchmark.
 *
 * Per controller ruling R7: the benchmark lives entirely in this file
 * and uses a STATIC LOCAL lock + counter.  We do NOT touch
 * boot_percpu.c, and there are no cross-CPU shared states — Task 4
 * (SMP + 4-core benchmark) owns that work.  Task 2a only proves
 * single-core correctness of the aarch64 spinlock primitives.
 *
 * Test:
 *   1. spin_lock + spin_unlock a static counter 1,000,000 times.
 *   2. Verify the counter equals 1,000,000 — no torn writes, no
 *      double-counting from a buggy unlock, no deadlock.
 *   3. Also exercise spin_trylock (success path) and spin_lock_irqsave
 *      (IRQ-safe variant) at least once each, so the test covers
 *      more than just the hot path.
 *   4. Emit "[spinlock] single-core 1M PASS" or FAIL with the actual
 *      counter value to the PL011.
 *
 * The function takes no arguments and returns void.  aarch64_main
 * calls it after printing the boot banner.
 */

#include <stdint.h>
#include <kernel/arch/aarch64/spinlock.h>

/* Forward from pl011.c. */
void kputs(const char *s);

/* Pre-baked output strings.  We can't use printf (no libc, no
 * format engine) so the PASS line is a literal and the FAIL line is
 * built manually by kputs()ing a header + converting the counter to
 * decimal digit-by-digit. */
static const char PASS_LINE[] = "[spinlock] single-core 1M PASS\n";
static const char FAIL_HDR[]   = "[spinlock] single-core FAIL got=";
static const char FAIL_TLR[]   = " want=1000000\n";

/* Convert a uint64_t to decimal in-place and write to PL011.  No
 * divide-by-10 library needed — we do the classic repeated-divmod.
 * Buffer is 21 chars (max 20 digits + NUL) on the stack; for our
 * "want=1000000" / counter ~1M use, 11 chars is plenty. */
static void kputu(uint64_t v)
{
    char buf[24];
    int i = 0;
    if (v == 0) {
        kputs("0");
        return;
    }
    while (v > 0) {
        buf[i++] = '0' + (char)(v % 10);
        v /= 10;
    }
    /* Output in reverse so the most-significant digit goes first. */
    while (i > 0) {
        char s[2];
        s[0] = buf[--i];
        s[1] = '\0';
        kputs(s);
    }
}

void test_spinlock(void)
{
    /* Single-core: a static local is enough; nothing else runs. */
    static spinlock_T lk;
    static uint64_t   counter;

    spin_init(&lk);
    counter = 0;

    /* ── Hot path: 1,000,000 acquire/release pairs ── */
    for (uint64_t i = 0; i < 1000000UL; i++) {
        spin_lock(&lk);
        counter++;
        spin_unlock(&lk);
    }

    /* ── trylock: one success path ── */
    /* After the loop above the lock is unlocked (1M even, last op
     * was an unlock).  spin_trylock should return non-zero (1). */
    long got = spin_trylock(&lk);
    if (got == 0) {
        /* trylock failed unexpectedly — bail out before counter++. */
        kputs("[spinlock] single-core FAIL trylock=0\n");
        return;
    }
    counter++;
    spin_unlock(&lk);

    /* ── irqsave path: one full round-trip ── */
    uint64_t flags = spin_lock_irqsave(&lk);
    counter++;
    spin_unlock_irqrestore(&lk, flags);

    /* ── Verify ── */
    if (counter == 1000002UL) {
        kputs(PASS_LINE);
    } else {
        kputs(FAIL_HDR);
        kputu(counter);
        kputs(FAIL_TLR);
    }
}
