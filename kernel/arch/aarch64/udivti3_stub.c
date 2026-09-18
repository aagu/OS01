/* kernel/arch/aarch64/udivti3_stub.c — aarch64 freestanding runtime helper.
 *
 * Why this exists (aarch64 Generic Timer Task 2.2 GREEN):
 *   kernel/time/clocksource.c uses `(__uint128_t)1000000000ULL << s) / freq_hz`
 *   to compute (mult, shift) for the timer-frame conversion. aarch64 has no
 *   native 128-bit division instruction, so clang lowers that expression to
 *   a call to the runtime helper `__udivti3`. The x86_64 kernel gets that
 *   helper implicitly via the libc sysroot's compiler-rt symbols; the
 *   aarch64 kernel profile has no libc (no userspace at all in phase 1)
 *   and no compiler-rt link, so the symbol is undefined at link time.
 *
 *   This file provides a minimal, correct implementation. We deliberately
 *   avoid any internal `__uint128_t` DIVIDE/MODULO (which would re-call
 *   `__udivti3`) — only shifts and 64-bit comparisons/arithmetic.
 *
 *   ABI: aarch64 passes 128-bit values in register pairs (lo in first
 *   register, hi in second); return value uses x0:x1. The C `__uint128_t`
 *   type lowers to that automatically.
 *
 *   Scope: aarch64 ONLY. The brief forbids any x86_64-side change; this
 *   file is added to the aarch64 whitelist only.
 */
#include <stdint.h>

/* Treat the dividend as two 64-bit halves to avoid recursive __udivti3
 * calls when we do `(dividend >> i) & 1` (right-shifts by constants are
 * natively lowered; only DIVISION needs the runtime helper). */
typedef struct {
    uint64_t lo;
    uint64_t hi;
} u128_halves;

/* Pack two halves into a 128-bit value. No division involved. */
static inline __uint128_t pack(u128_halves v)
{
    return ((__uint128_t)v.hi << 64) | (__uint128_t)v.lo;
}

/* Unpack a 128-bit value into two halves. No division involved. */
static inline u128_halves unpack(__uint128_t v)
{
    return (u128_halves){ (uint64_t)v, (uint64_t)(v >> 64) };
}

/* Compare two 128-bit values as unsigned. */
static inline int u128_halves_ge(u128_halves a, u128_halves b)
{
    if (a.hi != b.hi) return a.hi > b.hi;
    return a.lo >= b.lo;
}

/* Subtract b from a (caller guarantees a >= b). */
static inline u128_halves u128_halves_sub(u128_halves a, u128_halves b)
{
    uint64_t new_lo = a.lo - b.lo;
    uint64_t borrow = (new_lo > a.lo) ? 1 : 0;
    return (u128_halves){ new_lo, a.hi - b.hi - borrow };
}

__uint128_t __udivti3(__uint128_t a, __uint128_t b)
{
    u128_halves A = unpack(a);
    u128_halves B = unpack(b);

    /* Division by zero: per the C standard / LLVM convention, the result
     * is undefined; we return 0 so the caller doesn't trap. (clocksource.c
     * never divides by zero — freq_hz is non-zero by guard in
     * clocksource_init — but be defensive.) */
    if (B.lo == 0 && B.hi == 0) {
        return ((__uint128_t)0);
    }

    u128_halves q = { 0, 0 };
    u128_halves r = { 0, 0 };

    /* 128-bit binary long division: shift dividend bits MSB→LSB into the
     * running remainder; whenever remainder >= divisor, subtract and set
     * the corresponding quotient bit. Each iteration only uses native
     * 64-bit shifts (no 128-bit divide). */
    for (int i = 127; i >= 0; i--) {
        /* r = (r << 1) | bit_i(a) */
        uint64_t new_rlo = (r.lo << 1) | (r.hi >> 63);
        uint64_t new_rhi = r.hi << 1;
        if (i >= 64) {
            new_rlo |= (A.hi >> (i - 64)) & 1ULL;
        } else {
            new_rlo |= (A.lo >> i) & 1ULL;
        }
        r.lo = new_rlo;
        r.hi = new_rhi;

        if (u128_halves_ge(r, B)) {
            r = u128_halves_sub(r, B);
            if (i >= 64) {
                q.hi |= (1ULL << (i - 64));
            } else {
                q.lo |= (1ULL << i);
            }
        }
    }
    return pack(q);
}