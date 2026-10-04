/*
 * hosttests/cases/test_vmm_semantic_header.c — pin the public vmm
 * header contract (aarch64 M3.2 Task 14 Step 1 RED test).
 *
 * Contract (plan Task 14 "Interfaces"):
 *   1. <memory/vmm.h> defines the arch-neutral VM_* semantic bits and
 *      the composite VM_KERNEL_RW / VM_KERNEL_RO / VM_USER_RW /
 *      VM_USER_RO / VM_DEVICE values.
 *   2. <memory/vmm.h> does NOT leak the x86 PAGE_* hardware PTE bits
 *      — those live in the x86-private <arch/x86_64/pte.h>.
 *   3. The arch-neutral arch_vmm_* semantic API is declared in the
 *      public header.
 *   4. <arch/x86_64/pte.h> provides the x86 PAGE_* hardware bits and
 *      the raw vmm_pt_walk prototype.
 *
 * Checks 1-3 are evaluated BEFORE the pte.h include at the bottom of
 * this TU; the preprocessor processes sequentially, so the earlier
 * tests really do observe memory/vmm.h in isolation.
 */
#include <test_framework.h>
#include <stdint.h>

#include <memory/vmm.h>

TEST_FUNC(vm_semantic_bits_defined) {
#if !defined(VM_PRESENT) || !defined(VM_WRITE) || !defined(VM_USER) || \
    !defined(VM_NO_EXEC) || !defined(VM_HUGE) || !defined(VM_NOCACHE) || \
    !defined(VM_PROTNONE) || !defined(VM_COW)
    assert_true(0);   /* semantic bit missing from memory/vmm.h */
#else
    assert_true(1);
#endif
}

TEST_FUNC(vm_composite_values_defined) {
#if !defined(VM_KERNEL_RW) || !defined(VM_KERNEL_RO) || \
    !defined(VM_USER_RW) || !defined(VM_USER_RO) || !defined(VM_DEVICE)
    assert_true(0);   /* composite value missing from memory/vmm.h */
#else
    /* Composites must be subsets of the semantic bits (spec §4.2). */
    assert_true((VM_KERNEL_RW & ~VM_PRESENT & ~VM_WRITE) == 0);
    assert_true((VM_KERNEL_RO & ~VM_PRESENT) == 0);
    assert_true((VM_USER_RW & ~VM_PRESENT & ~VM_USER & ~VM_WRITE) == 0);
    assert_true((VM_USER_RO & ~VM_PRESENT & ~VM_USER) == 0);
    assert_true((VM_DEVICE & ~VM_PRESENT & ~VM_WRITE & ~VM_NOCACHE &
                 ~VM_NO_EXEC) == 0);
#endif
}

TEST_FUNC(x86_hw_bits_not_in_semantic_header) {
#if defined(PAGE_PRESENT) || defined(PAGE_RW) || defined(PAGE_USER) || \
    defined(PAGE_HUGE) || defined(PAGE_VALID) || defined(PAGE_WRITE) || \
    defined(PAGE_USER_PTE) || defined(PAGE_KERNEL_PGD) || \
    defined(PAGE_COW) || defined(PAGE_PROTNONE) || defined(PAGE_NO_EXEC)
    assert_true(0);   /* x86 hardware bit leaked through memory/vmm.h */
#else
    assert_true(1);
#endif
}

TEST_FUNC(arch_vmm_protos_declared) {
    /* Taking these addresses fails to compile if the prototypes are
     * not declared in the public header, and pins the signatures the
     * Task 15/16 backends must implement. */
    int                (*init)(void) = arch_vmm_init;
    int                (*map_4k_new)(uint64_t *, uint64_t, uint64_t,
                                     uint32_t) = arch_vmm_map_4k_new;
    int                (*update_4k)(uint64_t *, uint64_t, uint64_t,
                                    uint32_t, uint64_t *, uint32_t *) =
        arch_vmm_update_4k;
    int                (*unmap_4k)(uint64_t *, uint64_t, uint64_t *,
                                   uint32_t *) = arch_vmm_unmap_4k;
    int                (*query_4k)(uint64_t *, uint64_t, uint64_t *,
                                  uint32_t *) = arch_vmm_query_4k;
    int                (*map_2m)(uint64_t *, uint64_t, uint64_t,
                                 uint32_t) = arch_vmm_map_2m;
    int                (*unmap_2m)(uint64_t *, uint64_t, uint64_t *) =
        arch_vmm_unmap_2m;
    int                (*split_2m_to_4k)(uint64_t *, uint64_t) =
        arch_vmm_split_2m_to_4k;
    assert_true(init && map_4k_new && update_4k && unmap_4k &&
                query_4k && map_2m && unmap_2m && split_2m_to_4k);
}

/* ── Check 4: the x86-private PTE header, evaluated AFTER the public
 * header checks above (sequential preprocessing keeps them isolated).
 * ─────────────────────────────────────────────────────────────── */
#include <arch/x86_64/pte.h>

TEST_FUNC(x86_pte_header_provides_hw_bits_and_pt_walk) {
#if !defined(PAGE_PRESENT) || !defined(PAGE_RW) || !defined(PAGE_USER) || \
    !defined(PAGE_HUGE) || !defined(PAGE_VALID) || !defined(PAGE_WRITE)
    assert_true(0);   /* pte.h missing the x86 hardware bits */
#else
    uint64_t *(*walk)(uint64_t *, uint64_t, uint64_t, int) = vmm_pt_walk;
    assert_true(walk != 0);
    /* x86 PTE format: Present/Writable are bits 0/1. */
    assert_true((PAGE_PRESENT & 1UL) == 1UL);
    assert_true((PAGE_RW & 2UL) == 2UL);
#endif
}

TEST_LIST_BEGIN
    TEST_ENTRY(vm_semantic_bits_defined),
    TEST_ENTRY(vm_composite_values_defined),
    TEST_ENTRY(x86_hw_bits_not_in_semantic_header),
    TEST_ENTRY(arch_vmm_protos_declared),
    TEST_ENTRY(x86_pte_header_provides_hw_bits_and_pt_walk),
TEST_LIST_END

/* Link stubs: this is a header-contract TU.  The arch_vmm_* backends
 * land in Tasks 15/16 (aarch64 / x86); vmm_pt_walk lives in the x86
 * backend kernel/memory/vmm.c, which this TU deliberately does not
 * link.  Never called — only their addresses are taken. */
int arch_vmm_init(void) { return 0; }
int arch_vmm_map_4k_new(uint64_t *p, uint64_t ph, uint64_t v, uint32_t f) { (void)p;(void)ph;(void)v;(void)f; return 0; }
int arch_vmm_update_4k(uint64_t *p, uint64_t ph, uint64_t v, uint32_t f, uint64_t *op, uint32_t *ov) { (void)p;(void)ph;(void)v;(void)f;(void)op;(void)ov; return 0; }
int arch_vmm_unmap_4k(uint64_t *p, uint64_t v, uint64_t *po, uint32_t *ov) { (void)p;(void)v;(void)po;(void)ov; return 0; }
int arch_vmm_query_4k(uint64_t *p, uint64_t v, uint64_t *po, uint32_t *vo) { (void)p;(void)v;(void)po;(void)vo; return 0; }
int arch_vmm_map_2m(uint64_t *p, uint64_t ph, uint64_t v, uint32_t f) { (void)p;(void)ph;(void)v;(void)f; return 0; }
int arch_vmm_unmap_2m(uint64_t *p, uint64_t v, uint64_t *po) { (void)p;(void)v;(void)po; return 0; }
int arch_vmm_split_2m_to_4k(uint64_t *p, uint64_t v) { (void)p;(void)v; return 0; }
uint64_t *vmm_pt_walk(uint64_t *p, uint64_t v, uint64_t f, int a) { (void)p;(void)v;(void)f;(void)a; return 0; }

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
