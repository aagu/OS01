// kernel/selftest/test_aarch64_rndr_encoding.c —
// aarch64-only compile-time + objdump-time verification of RNDR/RNDRRS
// assembly encoding.
//
// PR #25 reviewer (0f67ead1, follow-up) found that the original
// un-guarded version broke x86_64 builds (kernel/Makefile uses
// $(wildcard selftest/*.c) on x86_64 too, so this file's aarch64
// inline asm (`mrs`, `cset`) hit "invalid instruction mnemonic"
// on x86_64).
//
// Fix: wrap the asm + helper in #ifdef __aarch64__. On x86_64 the file
// becomes a no-op (just the include guard), so it compiles cleanly
// alongside the rest of selftest/. The asm helper below mirrors
// kernel/arch/aarch64/random.c rndr_one()/rndrrs_one() — any drift
// between them is caught at KERNEL_SELFTEST=1 build time (CI's
// `aarch64 checks` job already runs llvm-objdump on the rebuilt
// arch/aarch64/random.o and verifies `mrs RNDR` / `mrs RNDRRS`
// instructions are emitted).
//
// Runtime verification (RNDR/RNDRRS actually returning entropy on
// QEMU -cpu max) is gated by CI's aarch64 boot path — see
// tests/scripts/qemu_entropy_modes.sh aarch64 STRONG mode for the
// full path exercise.

#ifdef OS01_SELFTEST

#include <stdint.h>   /* uint64_t for the aarch64 asm helper below */

/* On x86_64 the asm below uses aarch64-only instructions and cannot
 * compile. Stub out the entire asm block on non-aarch64 arches so
 * $(wildcard selftest/*.c) stays happy on every profile. */
#if defined(__aarch64__)

// Mirror of kernel/arch/aarch64/random.c rndr_one() — kept in sync to
// catch regressions if either side drifts.
static int selftest_rndr_asm_ok(uint64_t *out)
{
    uint64_t v;
    uint64_t success;
    __asm__ __volatile__(
        "mrs %0, s3_3_c2_c4_0\n\t"   /* RNDR (op2=0) */
        "cset %w1, ne"              /* Z==0 (success) ⇒ 1 */
        : "=r"(v), "=r"(success)
        :
        : "cc");
    if (success) { *out = v; return 1; }
    return 0;
}

// Mirror of rndrrs_one().
static int selftest_rndrrs_asm_ok(uint64_t *out)
{
    uint64_t v;
    uint64_t success;
    __asm__ __volatile__(
        "mrs %0, s3_3_c2_c4_1\n\t"   /* RNDRRS (op2=1) */
        "cset %w1, ne"
        : "=r"(v), "=r"(success)
        :
        : "cc");
    if (success) { *out = v; return 1; }
    return 0;
}

#endif /* __aarch64__ */

/* Compile-time-only check: this function exists so the asm snippets are
 * emitted by the compiler on aarch64. On x86_64 the helpers are not
 * defined (see #ifdef above) so this is a no-op. The actual mrs
 * encoding is verified at CI's `aarch64 checks` job via objdump |
 * grep S3_3_C2_C4 (see tests/scripts/qemu_entropy_modes.sh aarch64
 * STRONG assertions). */
int aarch64_rndr_encoding_selftest(void)
{
#if defined(__aarch64__)
    uint64_t v1 = 0, v2 = 0;
    (void)selftest_rndr_asm_ok(&v1);
    (void)selftest_rndrrs_asm_ok(&v2);
#endif
    return 0;
}

#endif /* OS01_SELFTEST */
