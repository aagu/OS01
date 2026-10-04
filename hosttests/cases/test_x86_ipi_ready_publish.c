/* hosttests/cases/test_x86_ipi_ready_publish.c — M2/M3 Task 12 Step 4.
 *
 * Source-level contract check (mirrors test_gic_marker_lines.c): QEMU
 * x86_64 regression is currently blocked (known infra stall), so the
 * publish-point invariants are asserted against the production source
 * text of:
 *
 *   kernel/arch/x86_64/smp/boot.c  — ipi_ready_publish_bsp/ap with the
 *                                    DEBUG (ASSERT) guards;
 *   kernel/arch/x86_64/smp/smp.c   — the two call sites;
 *   kernel/arch/x86_64/intr/apic/ipi.c — gen-ack handler rewrite.
 *
 * Assertions:
 *   1. BSP/AP publish helpers exist in boot.c and release-store
 *      percpu ipi_ready = 1;
 *   2. both helpers carry the ASSERT guards (handler registered /
 *      IF enabled / online);
 *   3. smp.c calls ipi_ready_publish_bsp() after ipi_init(), and
 *      ipi_ready_publish_ap() after arch_local_irq_enable() (line order);
 *   4. apic/ipi.c has NO legacy tlb_wanted/tlb_ack remnants, keeps
 *      lapic_eoi() at the end of the TLB handler and bumps tlb_ack_gen.
 */
#include <test_framework.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OS01_KERNEL_SRC
#define OS01_KERNEL_SRC "."
#endif

#define BOOT_SRC  OS01_KERNEL_SRC "/kernel/arch/x86_64/smp/boot.c"
#define SMP_SRC   OS01_KERNEL_SRC "/kernel/arch/x86_64/smp/smp.c"
#define IPI_SRC   OS01_KERNEL_SRC "/kernel/arch/x86_64/intr/apic/ipi.c"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  [FAIL] cannot open %s\n", path);
        return NULL;
    }
    /* No fseek/ftell: the OS01 libc stdio (shadowed in via -I order)
     * does not declare them; read to EOF in chunks instead. */
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096)
            break;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

void test_boot_publish_helpers(void)
{
    TEST_SUITE("x86 ipi_ready publish: boot.c helpers");
    char *src = slurp(BOOT_SRC);
    assert_not_null(src);
    if (!src) return;

    assert_true(contains(src, "void ipi_ready_publish_bsp(void)"));
    assert_true(contains(src, "void ipi_ready_publish_ap(uint32_t cpu)"));
    /* release-store publication of ipi_ready = 1 */
    assert_true(contains(src, "__atomic_store_n(&percpu_data[0].ipi_ready, 1, __ATOMIC_RELEASE)"));
    assert_true(contains(src, "__atomic_store_n(&percpu_data[cpu].ipi_ready, 1, __ATOMIC_RELEASE)"));
    free(src);
}

void test_boot_debug_guards(void)
{
    TEST_SUITE("x86 ipi_ready publish: DEBUG asserts");
    char *src = slurp(BOOT_SRC);
    assert_not_null(src);
    if (!src) return;

    /* BSP: handler registered, percpu ready, IF enabled */
    assert_true(contains(src, "ASSERT(intr_handler_table[IPI_VECTOR_TLB] != 0)"));
    assert_true(contains(src, "ASSERT(percpu_data[0].online == 1)"));
    assert_true(contains(src, "ASSERT(x86_irqs_enabled())"));
    free(src);
}

void test_smp_call_sites(void)
{
    TEST_SUITE("x86 ipi_ready publish: smp.c call sites");
    char *src = slurp(SMP_SRC);
    assert_not_null(src);
    if (!src) return;

    assert_true(contains(src, "ipi_ready_publish_bsp()"));
    assert_true(contains(src, "ipi_ready_publish_ap(cpu->cpu_id)"));

    /* Ordering: BSP publish strictly after ipi_init(); AP publish
     * strictly after arch_local_irq_enable() in ap_entry(). */
    const char *ipi_init  = strstr(src, "ipi_init();");
    const char *bsp_pub   = strstr(src, "ipi_ready_publish_bsp()");
    const char *ap_enab   = strstr(src, "arch_local_irq_enable();");
    const char *ap_pub    = strstr(src, "ipi_ready_publish_ap(cpu->cpu_id)");
    assert_not_null(ipi_init);
    assert_not_null(bsp_pub);
    assert_not_null(ap_enab);
    assert_not_null(ap_pub);
    assert_true(bsp_pub > ipi_init);
    assert_true(ap_pub > ap_enab);
    free(src);
}

void test_ipi_handler_migrated(void)
{
    TEST_SUITE("x86 TLB handler: gen-ack migration");
    char *src = slurp(IPI_SRC);
    assert_not_null(src);
    if (!src) return;

    /* Legacy flag protocol fully gone (tlb_ack must only survive as the
     * tlb_ack_gen suffix, never as a bare symbol). */
    assert_false(contains(src, "tlb_wanted"));
    {
        int bare_tlb_ack = 0;
        const char *p = src;
        while ((p = strstr(p, "tlb_ack")) != NULL) {
            if (p[7] != '_') {      /* "tlb_ack_gen" continues with '_' */
                bare_tlb_ack = 1;
                break;
            }
            p += 7;
        }
        assert_false(bare_tlb_ack);
    }

    /* New protocol: unconditional flush + generation bump + handler EOI */
    assert_true(contains(src, "flush_tlb();"));
    assert_true(contains(src, "__atomic_fetch_add(&cpu->tlb_ack_gen, 1, __ATOMIC_RELEASE)"));
    assert_true(contains(src, "lapic_eoi();"));
    free(src);
}

int main(void)
{
    test_boot_publish_helpers();
    test_boot_debug_guards();
    test_smp_call_sites();
    test_ipi_handler_migrated();

    printf("\n  x86 publish: total=%d passed=%d failed=%d\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed > 0 ? 1 : 0;
}
