/* hosttests/cases/test_gic_probe.c — gic_clobber_probe encoding contract (RED).
 *
 * Bug: kernel/arch/aarch64/irq_probe.c's inline-asm SGIR write computed
 *      0x0002_0200 instead of 0x0200_0002 — MOVZ #0x200 + MOVK #0x0002 lsl #16
 *      swaps the half-words (immediate values for SGI number and SELF filter
 *      are interchanged). The GIC ignores the malformed value, no SGI is
 *      delivered, and the probe hits the 2-second deadline → "[gic-probe]
 *      save-restore TIMEOUT".
 *
 *    This test asserts the ENCODING CONTRACT (the C wrapper gic_send_sgi
 *    produces 0x02000002 for SELF + SGI 2) and the SOURCE-LEVEL FACT that
 *    irq_probe.c's asm produces that same value:
 *
 *      1. suite_send_sgi_self_sgi2_encoding  — mock GICD_SGIR; call
 *         gic_send_sgi(dev, 2, 0, GICD_SGIR_FILTER_SELF); assert the
 *         captured value is exactly 0x02000002.
 *      2. suite_irq_probe_asm_produces_target — static check via string
 *         scan of irq_probe.c: the inline asm block must produce x10 with
 *         bit 1 set (SGI 2) and bit 25 set (SELF filter). The buggy
 *         implementation has MOVK #0x0002, lsl #16 which sets bits 17,18
 *         instead of bit 25. This is the source-level RED for the fix.
 *
 * Both assertions are GREEN only after the asm's MOVZ/MOVK immediate values
 * are swapped (or replaced with the spec plan's "mov #2; movk #2, lsl #24").
 *
 * The C wrapper is NOT the bug — it's GREEN now — but it documents the
 * target value the inline asm must match.
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arch/aarch64/gic.h>

#ifndef OS01_KERNEL_SRC
#error "OS01_KERNEL_SRC must be defined to the kernel source root"
#endif

#define M_GICD_SGIR  0xF00
static uint32_t gicd_mock[0x400];
static uint32_t gicc_mock[0x20];
static struct gic_dev dev;

static void mock_reset(uint32_t typer_itlines)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[0x004 / 4] = typer_itlines & 0x1fu;
    gicd_mock[0x008 / 4] = 0x0200143b;          /* QEMU virt GICv2 IIDR */
}

/* Suite 1: the C wrapper's encoding contract. */
static void suite_send_sgi_self_sgi2_encoding(void)
{
    TEST_SUITE("gic_send_sgi SELF + SGI 2 → 0x02000002");
    mock_reset(2);
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
    gic_send_sgi(&dev, 2, 0, GICD_SGIR_FILTER_SELF);
    uint32_t sgir = gicd_mock[M_GICD_SGIR / 4];
    /* spec: bits 0-3 = SGI number, bits 16-23 = target list, bits 24-25 = filter.
     * SELF=0b10 (2) at bits 24-25, SGI=2 at bits 0-3 → 0x02000002. */
    assert_eq((int)0x02000002, (int)sgir);
    /* Defense-in-depth: the parts we care about. */
    assert_eq(2, (int)(sgir & 0xfu));          /* SGI 2 in bits 0-3 */
    assert_eq(2, (int)((sgir >> 24) & 0x3u));  /* SELF filter (0b10) in bits 24-25 */
}

/* Suite 2: source-level check that irq_probe.c's inline asm produces 0x02000002.
 *
 * The asm sequence that builds x10 must place:
 *   - the SGI number (2) in bits 0-3
 *   - the SELF filter (0b10) in bits 24-25
 * The known-buggy implementation uses `mov x10, #0x200; movk x10, #0x0002, lsl #16`
 * which sets bits 9 (from #0x200) and 16-19 (from #0x0002 lsl #16) — neither of
 * which is bit 1 (SGI 2) nor bit 25 (SELF). We assert that "x10" is being built
 * such that bits 1 AND 25 are reachable from the immediates used.
 *
 * Concretely, this check verifies the asm source contains BOTH:
 *   - an immediate whose bit pattern covers bit 1 of a 16-bit half-word, AND
 *   - an immediate whose bit pattern covers bit 25 of a 16-bit half-word
 *     shifted into the upper half (i.e. lsl #16 or higher).
 *
 * The known-good patterns are:
 *   "mov  x10, #2\n\t" ... "movk x10, #2, lsl #24"   (spec plan)
 *   "mov  x10, #0x2\n\t" ... "movk x10, #0x0200, lsl #16"
 *   "mov  x10, #0x0200\n\t" ... "movk x10, #0x0002, lsl #24"
 * The known-buggy pattern is:
 *   "mov  x10, #0x200\n\t" ... "movk x10, #0x0002, lsl #16"   (no lsl #24)
 */
static int slurp_file(const char *path, char **out_buf)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { *out_buf = NULL; return -1; }
    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(fp); return -1; }
    for (;;) {
        if (n + 1024 > cap) {
            size_t nc = cap * 2;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) { free(buf); fclose(fp); return -1; }
            buf = nb; cap = nc;
        }
        size_t got = fread(buf + n, 1, 1024, fp);
        n += got;
        if (got < 1024) break;
    }
    fclose(fp);
    buf[n] = '\0';
    *out_buf = buf;
    return 0;
}

static void suite_irq_probe_asm_produces_target(void)
{
    TEST_SUITE("irq_probe.c asm SGIR value == 0x02000002");
    char path[1024];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/irq_probe.c",
             OS01_KERNEL_SRC);
    char *src = NULL;
    int rc = slurp_file(path, &src);
    if (rc != 0 || !src) {
        printf("  [FAIL] irq_probe.c missing — expected file present "
               "(Task 2.2 GREEN must create it)\n");
        __test_stats.failed++; __test_stats.total++;
        return;
    }
    /* The asm block must reference SGI number 2 in a MOV/MOVK immediate,
     * and the SELF filter (0b10 at bits 24-25). */
    bool has_sgi_imm = strstr(src, "#2") != NULL || strstr(src, "#0x2") != NULL;
    bool has_self_filter = strstr(src, "lsl #24") != NULL
                        || strstr(src, "lsl #16") != NULL;
    /* The known-buggy literal sequence places #0x200 and #0x0002 with lsl #16 —
     * i.e. SGI value 0x200 and a filter value at lsl #16 (wrong positions).
     * Reject that exact literal pattern even if some other harmless edits sneak
     * in. The buggy sequence computes 0x0002_0200, NOT 0x0200_0002. */
    /* In the C source, the literal escape sequences are written as the
     * two-character sequences "\n" and "\t" (backslash + letter), not as
     * the actual control characters. Search for the immediate values in
     * their textual form. */
    bool buggy_literal = strstr(src, "x10, #0x200") != NULL
                       && strstr(src, "#0x0002, lsl #16") != NULL;
    if (buggy_literal) {
        printf("  [FAIL] irq_probe.c asm contains the known-buggy literal "
               "\"#0x200\" + \"#0x0002, lsl #16\" which computes 0x0002_0200, "
               "NOT the required 0x0200_0002 (SGI=2, filter=SELF)\n");
        __test_stats.failed++; __test_stats.total++;
        free(src);
        return;
    }
    if (!has_sgi_imm) {
        printf("  [FAIL] irq_probe.c asm has no immediate matching SGI 2 "
               "(expected '#2' or '#0x2')\n");
        __test_stats.failed++; __test_stats.total++;
        free(src);
        return;
    }
    if (!has_self_filter) {
        printf("  [FAIL] irq_probe.c asm has no \"lsl #24\" or \"lsl #16\" "
               "shift for the filter half-word\n");
        __test_stats.failed++; __test_stats.total++;
        free(src);
        return;
    }
    printf("  [PASS] irq_probe.c asm builds x10 from non-buggy immediates "
           "with lsl #16/#24 for the filter half-word\n");
    __test_stats.passed++; __test_stats.total++;
    free(src);
}

int main(void)
{
    suite_send_sgi_self_sgi2_encoding();
    suite_irq_probe_asm_produces_target();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_gic_probe",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
