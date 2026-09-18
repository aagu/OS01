// kernel/compiler_rt/udivti3.c — __uint128_t unsigned division for freestanding builds.
//
// Why this exists:
//   Clang lowers `(__uint128_t)numerator / denominator` to a runtime call
//   to `__udivti3` on architectures without native 128-bit division
//   (aarch64 is the primary OS01 consumer; RISC-V, AVR32 etc. would
//   use the same helper if OS01 ported there). x86_64 gets the symbol
//   implicitly via the libc sysroot's compiler-rt. Freestanding builds
//   (OS01 aarch64, no libc) need an explicit definition.
//
// ABI: aarch64 passes 128-bit values in register pairs (lo in first
// register, hi in second); return value uses x0:x1. The C `__uint128_t`
// type lowers to that automatically.
//
// Implementation: binary long division using 64-bit halves. We
// deliberately avoid any internal `__uint128_t` DIVIDE/MODULO (which
// would re-call __udivti3) — only shifts and 64-bit comparisons.

#include <stdint.h>

typedef struct {
    uint64_t lo;
    uint64_t hi;
} u128_halves;

static inline __uint128_t pack(u128_halves v)
{
    return ((__uint128_t)v.hi << 64) | (__uint128_t)v.lo;
}

static inline u128_halves unpack(__uint128_t v)
{
    return (u128_halves){ (uint64_t)v, (uint64_t)(v >> 64) };
}

static inline int u128_halves_ge(u128_halves a, u128_halves b)
{
    if (a.hi != b.hi) return a.hi > b.hi;
    return a.lo >= b.lo;
}

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

    if (B.lo == 0 && B.hi == 0) {
        return ((__uint128_t)0);
    }

    u128_halves q = { 0, 0 };
    u128_halves r = { 0, 0 };

    for (int i = 127; i >= 0; i--) {
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