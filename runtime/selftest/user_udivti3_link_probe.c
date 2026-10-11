/* User runtime consumer link probe (monitor plan Task 5).
 *
 * Linked as a real user program with the profile's user toolchain, this
 * probe is the build-system proof that the user builtins consumer exists:
 * the 128-bit divisions below emit undefined references to __udivti3 that
 * only the consumer=user archive (build/<profile>/runtime/user/<provider>-<digest>/)
 * may resolve.  The kernel consumer archive is never linked here — its
 * objects are compiled with -mcmodel=kernel and are unusable in user ELFs.
 *
 * The operands go through volatile locals so no division can be constant
 * folded: the probe object must carry a real U __udivti3 reference before
 * the link and a defined T __udivti3 after it (asserted by
 * qemutests/monitor_runtime_test.py with llvm-nm).
 *
 * The quotient assertions reuse the fixed vectors from the existing kernel
 * runtime selftest so both consumers are held to the same arithmetic
 * contract.  The probe is runnable under QEMU (exit 0 = all vectors pass),
 * but the monitor runtime gate itself is a static link check.
 */

#include "udivti3_vectors.h"

#include <stddef.h>
#include <stdint.h>

extern unsigned __int128 __udivti3(unsigned __int128, unsigned __int128);

typedef union {
    unsigned __int128 value;
    struct {
        uint64_t lo, hi;
    } limb;
} u128_bits;

int main(void)
{
    volatile unsigned __int128 dividend;
    volatile unsigned __int128 divisor;
    unsigned failures = 0;

    for (size_t i = 0; i < OS01_UDIVTI3_VECTOR_COUNT; ++i) {
        const struct udivti3_vector *v = &udivti3_vectors[i];
        u128_bits n, d, q;

        n.limb.lo = v->n_lo;
        n.limb.hi = v->n_hi;
        d.limb.lo = v->d_lo;
        d.limb.hi = v->d_hi;

        /* The volatile round-trip keeps __udivti3 calls real. */
        dividend = n.value;
        divisor = d.value;
        q.value = __udivti3(dividend, divisor);

        if (q.limb.lo != v->q_lo || q.limb.hi != v->q_hi)
            ++failures;
    }

    return failures == 0 ? 0 : 1;
}
