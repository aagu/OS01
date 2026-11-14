/*
 * hosttests/cases/test_vmm_semantic_header.c — pin the public VMM/MMU
 * header boundary (mm-architecture-boundaries Task 2).
 *
 * Contract:
 *   1. Including ONLY the public <memory/vmm.h>, <memory/memory.h> and
 *      <memory/vma.h> must NOT provide:
 *        - the ARM semantic VM_* bits (they moved to the ARM backend
 *          header),
 *        - any x86 PAGE_* hardware bit,
 *        - PAGE_PGD_SHIFT, get_cr3 or the flush_tlb alias,
 *        - any private header include guard (no transitive private
 *          header is pulled in).
 *      The public <memory/vmm.h> still declares tlb_shootdown(void).
 *   2. Symbol-level "no declaration" probes live in the cross-profile
 *      static case `test-static CASE=mm-header-boundary`
 *      (qemutests/mm_header_boundary.py): it compiles each public
 *      header as an independent TU with the real target configuration
 *      and proves that referencing arch_vmm_query_4k / vmm_pt_walk /
 *      arch_get_page_table FAILS to compile after only the public
 *      headers.  A host TU cannot assert a compile failure, so this
 *      file pins the preprocessor-level half of the same contract.
 *   3. After EXPLICITLY including the private headers, the ARM
 *      semantic bits + arch_vmm_* prototypes and the x86 raw API are
 *      available again (positive contract for the private layer).
 *
 * Checks 1-2 are evaluated BEFORE the private includes at the bottom
 * of this TU; the preprocessor processes sequentially, so the earlier
 * tests really do observe the public headers in isolation.
 */
#include <test_framework.h>
#include <stdint.h>

#include <memory/vmm.h>
#include <memory/memory.h>
#include <memory/vma.h>

TEST_FUNC(public_headers_hide_semantic_and_hw_bits) {
    /* ARM semantic bits: ARM-only, behind <arch/aarch64/vmm_backend.h>. */
#if defined(VM_PRESENT) || defined(VM_WRITE) || defined(VM_USER) || \
    defined(VM_NO_EXEC) || defined(VM_HUGE) || defined(VM_NOCACHE) || \
    defined(VM_PROTNONE) || defined(VM_COW) || defined(VM_DEVICE)
    assert_true(0);   /* VM_* bit leaked through a public header */
#else
    assert_true(1);
#endif
}

TEST_FUNC(public_headers_hide_x86_hw_bits) {
#if defined(PAGE_PRESENT) || defined(PAGE_RW) || defined(PAGE_USER) || \
    defined(PAGE_HUGE) || defined(PAGE_VALID) || defined(PAGE_WRITE) || \
    defined(PAGE_USER_PTE) || defined(PAGE_KERNEL_PGD) || \
    defined(PAGE_COW) || defined(PAGE_PROTNONE) || defined(PAGE_NO_EXEC)
    assert_true(0);   /* x86 hardware bit leaked through a public header */
#else
    assert_true(1);
#endif
}

TEST_FUNC(public_headers_hide_pgd_shift_and_aliases) {
#if defined(PAGE_PGD_SHIFT) || defined(get_cr3) || defined(flush_tlb) || \
    defined(switch_tlb)
    assert_true(0);   /* PGD shift / root alias leaked through a public header */
#else
    assert_true(1);
#endif
}

TEST_FUNC(public_headers_do_not_pull_private_guards) {
    /* _ARCH_MMU_H is the narrowed arch FACADE (memory.h includes it for
     * ARCH_PAGE_OFFSET) — allowed.  The per-arch private headers must
     * NOT be pulled in transitively. */
#if defined(_ARCH_X86_64_MMU_H) || \
    defined(_ARCH_X86_64_VMM_H) || defined(_ARCH_X86_64_PTE_H) || \
    defined(_ARCH_AARCH64_MMU_H) || defined(_ARCH_AARCH64_VMM_BACKEND_H)
    assert_true(0);   /* a private header was pulled in transitively */
#else
    assert_true(1);
#endif
}

TEST_FUNC(public_tlb_shootdown_still_declared) {
    /* The one thing the public VMM header still owns: the shared TLB
     * broadcast.  Taking its address fails to compile if the
     * declaration vanished. */
    void (*shootdown)(void) = tlb_shootdown;
    assert_true(shootdown != 0);
}

/* ── Check 3: the private layers, evaluated AFTER the public header
 * checks above (sequential preprocessing keeps them isolated).
 * ─────────────────────────────────────────────────────────────── */
#include <arch/aarch64/vmm_backend.h>
#include <arch/x86_64/mmu.h>
#include <arch/x86_64/vmm.h>
#include <arch/x86_64/pte.h>

TEST_FUNC(arm_semantic_layer_behind_private_header) {
#if !defined(VM_PRESENT) || !defined(VM_WRITE) || !defined(VM_USER) || \
    !defined(VM_NO_EXEC) || !defined(VM_HUGE) || !defined(VM_NOCACHE) || \
    !defined(VM_PROTNONE) || !defined(VM_COW)
    assert_true(0);   /* ARM backend header missing the semantic bits */
#else
    /* Composites must be subsets of the semantic bits (spec §4.2). */
    assert_true((VM_KERNEL_RW & ~VM_PRESENT & ~VM_WRITE) == 0);
    assert_true((VM_KERNEL_RO & ~VM_PRESENT) == 0);
    assert_true((VM_USER_RW & ~VM_PRESENT & ~VM_USER & ~VM_WRITE) == 0);
    assert_true((VM_USER_RO & ~VM_PRESENT & ~VM_USER) == 0);
    assert_true((VM_DEVICE & ~VM_PRESENT & ~VM_WRITE & ~VM_NOCACHE &
                 ~VM_NO_EXEC) == 0);
#endif
    /* The ARM semantic prototypes pin the signatures the backend
     * implements. */
    int                (*init)(void) = arch_vmm_init;
    int                (*query_4k)(uint64_t *, uint64_t, uint64_t *,
                                  uint32_t *) = arch_vmm_query_4k;
    int                (*map_2m)(uint64_t *, uint64_t, uint64_t,
                                 uint32_t) = arch_vmm_map_2m;
    assert_true(init && query_4k && map_2m);
}

TEST_FUNC(x86_private_layer_raw_api) {
    /* x86 raw prototypes: the walker and the checked boot helper. */
    uint64_t *(*walk)(uint64_t *, uint64_t, uint64_t, int) = vmm_pt_walk;
    int (*checked)(uint64_t *, size_t, uint64_t, uint64_t *) =
        vmm_get_next_level_checked;
    assert_true(walk != 0);
    assert_true(checked != 0);
    /* x86 PTE format: Present/Writable are bits 0/1. */
    assert_true((PAGE_PRESENT & 1UL) == 1UL);
    assert_true((PAGE_RW & 2UL) == 2UL);
    /* The root primitives come from the x86 private MMU header. */
    uint64_t *(*get_root)(void) = arch_get_page_table;
    assert_true(get_root != 0);
    /* The top-level shift moved into the x86 VMM header. */
    assert_true(PAGE_PGD_SHIFT == 39);
}

TEST_LIST_BEGIN
    TEST_ENTRY(public_headers_hide_semantic_and_hw_bits),
    TEST_ENTRY(public_headers_hide_x86_hw_bits),
    TEST_ENTRY(public_headers_hide_pgd_shift_and_aliases),
    TEST_ENTRY(public_headers_do_not_pull_private_guards),
    TEST_ENTRY(public_tlb_shootdown_still_declared),
    TEST_ENTRY(arm_semantic_layer_behind_private_header),
    TEST_ENTRY(x86_private_layer_raw_api),
TEST_LIST_END

/* Link stubs: this is a header-contract TU.  It deliberately does not
 * link the x86 VMM TU or the ARM backend; only addresses are taken. */
void tlb_shootdown(void) { }
int arch_vmm_init(void) { return 0; }
int arch_vmm_query_4k(uint64_t *p, uint64_t v, uint64_t *po, uint32_t *vo) { (void)p;(void)v;(void)po;(void)vo; return 0; }
int arch_vmm_map_2m(uint64_t *p, uint64_t ph, uint64_t v, uint32_t f) { (void)p;(void)ph;(void)v;(void)f; return 0; }
uint64_t *vmm_pt_walk(uint64_t *p, uint64_t v, uint64_t f, int a) { (void)p;(void)v;(void)f;(void)a; return 0; }
int vmm_get_next_level_checked(uint64_t *cur, size_t e, uint64_t fl, uint64_t *out) { (void)cur;(void)e;(void)fl;(void)out; return 0; }

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
