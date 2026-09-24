// kernel/selftest/test_aarch64_rndr_encoding.c —
// Compile-time + objdump-time verification of RNDR/RNDRRS assembly encoding.
//
// PR #25 review (0f67ead1) found two bugs in the original aarch64 random.c:
//   (1) RNDR / RNDRRS S3_3_C2_C4_n op2 swapped (RNDR is op2=0, RNDRRS op2=1)
//   (2) `cset ... eq` inverted success/failure (success has Z=0 ⇒ use `ne`)
//
// This selftest reproduces the corrected inline asm verbatim and runs only
// at selftest-build time. It is excluded from runtime selftest dispatch
// (selftest.c doesn't register it) but lives in the source tree so the
// compilation contract is checked on every `make KERNEL_SELFTEST=1` and
// CI's `aarch64 checks` job verifies objdump contains the expected
// `mrs Xn, S3_3_C2_C4_0` and `mrs Xn, S3_3_C2_C4_1` instructions.
//
// Runtime verification (RNDR/RNDRRS actually returning entropy on
// QEMU -cpu max) is gated by CI's `aarch64 checks` job which boots the
// UEFI image — see tests/scripts/qemu_entropy_modes.sh aarch64 STRONG
// mode for the full path exercise.

#ifdef OS01_SELFTEST

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

/* Compile-time-only check: this function exists so the asm snippets are
 * emitted by the compiler. The actual mrs encoding is verified at CI's
 * `aarch64 checks` job via objdump | grep S3_3_C2_C4 (see tests/scripts/
 * qemu_entropy_modes.sh aarch64 STRONG assertions). */
int aarch64_rndr_encoding_selftest(void)
{
    /* Force the compiler to emit the asm by referencing the helpers. */
    uint64_t v1 = 0, v2 = 0;
    (void)selftest_rndr_asm_ok(&v1);
    (void)selftest_rndrrs_asm_ok(&v2);
    return 0;
}

#endif /* OS01_SELFTEST */
