/*
 * hosttests/cases/test_user_vm.c — AArch64 M4 anonymous-VM policy.
 *
 * Links the REAL kernel/memory/user_vm.c policy, the REAL shared
 * vm_area.c list primitives and the REAL AArch64 leaf adapter
 * (kernel/arch/aarch64/memory/user_vm.c) against a fake M3 backend
 * and heap-backed page/record allocators.  Fault injection is
 * expressed as "let N matching allocations succeed, fail the next".
 *
 * The suite pins the M4 contract that the QEMU acceptance case later
 * exercises end-to-end: metadata-only mmap/brk growth, split-node
 * preallocation, hole handling, demand-fill rollback, stash
 * ownership and the brk retained-tail zeroing.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include <memory/user_vm.h>
#include <memory/vm_area.h>
#include <memory/vma.h>

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root"
#endif

#define BASE       UINT64_C(0x40000000)
#define HEAP_BASE  UINT64_C(0x500000)
#define PROT_RW    (PROT_READ | PROT_WRITE)
#define MAP_ANON_PRIVATE (MAP_PRIVATE | MAP_ANONYMOUS)

/* ── Helpers ─────────────────────────────────────────────────── */

static mm_t *new_mm(uint64_t heap_base)
{
    mm_t *mm = calloc(1, sizeof(*mm));
    assert_not_null(mm);
    list_init(&mm->vma_list);
    mm->mmap_base = BASE;
    /* The fake M3 backend keys only on the VA; the root pointer is
     * never dereferenced. */
    mm->pgdir = (uint64_t *)(uintptr_t)0x8000;
    assert_eq(0, user_vm_init_heap(mm, heap_base));
    return mm;
}

static void destroy_mm(mm_t *mm)
{
    user_vm_destroy(mm);
    assert_false(user_vm_has_metadata(mm));
    free(mm);
}

static int vma_count(mm_t *mm)
{
    int n = 0;
    for (list_t *p = mm->vma_list.next; p != &mm->vma_list; p = p->next)
        n++;
    return n;
}

static int64_t test_mmap_call(mm_t *mm, uint64_t addr, uint64_t len, uint64_t prot,
                       uint64_t flags)
{
    uint64_t args[6] = { addr, len, prot, flags, UINT64_MAX, 0 };
    return user_vm_mmap(mm, args);
}

static void write_page(uint64_t pa, unsigned char value)
{
    memset(Phy_To_Virt(pa), value, PAGE_4K_SIZE);
}

/* ── Tests: metadata-only growth ─────────────────────────────── */

TEST_FUNC(test_mmap_and_brk_growth_allocate_no_pages)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    assert_eq((int64_t)HEAP_BASE, user_vm_brk(mm, 0));
    assert_eq((int64_t)(HEAP_BASE + 0x3000),
              user_vm_brk(mm, HEAP_BASE + 0x3000));
    assert_eq((int64_t)(HEAP_BASE + 0x3000), user_vm_brk(mm, 0));

    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x4000, PROT_RW, MAP_ANON_PRIVATE));

    /* brk grow and mmap only commit metadata. */
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_page_allocs());
    assert_eq(2, vma_count(mm));            /* heap + one mmap */
    assert_eq(0, user_vm_test_map_calls()); /* no leaf was created */

    destroy_mm(mm);
}

/* ── Tests: mprotect split preallocation ─────────────────────── */

TEST_FUNC(test_mprotect_split_node_failure_changes_nothing)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x4000, PROT_RW, MAP_ANON_PRIVATE));
    assert_eq(2, vma_count(mm));            /* heap + one mmap */

    /* The range needs a split at both ends => two nodes.  Failing the
     * first and then the second must leave the single VMA intact. */
    user_vm_test_fail_node_after(0);
    assert_eq(-ENOMEM, user_vm_mprotect(mm, BASE + 0x1000, 0x1000,
                                        PROT_READ));
    assert_eq(2, vma_count(mm));

    user_vm_test_fail_node_after(1);
    assert_eq(-ENOMEM, user_vm_mprotect(mm, BASE + 0x1000, 0x1000,
                                        PROT_READ));
    assert_eq(2, vma_count(mm));
    vma_t *whole = vm_area_lookup(mm, BASE);
    assert_not_null(whole);
    assert_eq(BASE, whole->vm_start);
    assert_eq(BASE + 0x4000, whole->vm_end);
    assert_true((whole->vm_flags & VMA_PROT_WRITE) != 0);

    /* Without injection the same request splits into three. */
    assert_eq(0, user_vm_mprotect(mm, BASE + 0x1000, 0x1000, PROT_READ));
    assert_eq(4, vma_count(mm));
    assert_true((vm_area_lookup(mm, BASE)->vm_flags & VMA_PROT_WRITE) != 0);
    assert_eq(0, vm_area_lookup(mm, BASE + 0x1000)->vm_flags & VMA_PROT_WRITE);
    assert_true((vm_area_lookup(mm, BASE + 0x2000)->vm_flags
                 & VMA_PROT_WRITE) != 0);

    destroy_mm(mm);
}

/* ── Tests: munmap and holes ─────────────────────────────────── */

TEST_FUNC(test_munmap_across_holes_and_fragments)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x4000, PROT_RW, MAP_ANON_PRIVATE));

    assert_eq(0, user_vm_munmap(mm, BASE + 0x1000, 0x1000));
    assert_eq(3, vma_count(mm));            /* heap + 2 fragments */
    assert_null(vm_area_lookup(mm, BASE + 0x1000));

    /* The hole is a no-op, not an error. */
    assert_eq(0, user_vm_munmap(mm, BASE + 0x1000, 0x1000));
    assert_eq(3, vma_count(mm));

    /* A range spanning fragments + hole removes both fragments. */
    assert_eq(0, user_vm_munmap(mm, BASE, 0x4000));
    assert_eq(1, vma_count(mm));            /* heap only */
    assert_null(vm_area_lookup(mm, BASE));

    destroy_mm(mm);
}

/* ── Tests: demand fault ─────────────────────────────────────── */

TEST_FUNC(test_demand_fault_allocates_zero_page_and_oom)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x2000, PROT_RW, MAP_ANON_PRIVATE));

    /* Record allocation OOM. */
    user_vm_test_fail_node_after(0);
    assert_eq(-ENOMEM, user_vm_fault(mm, BASE, true));
    assert_eq(0, user_vm_test_leaf_count());

    /* Page allocation OOM. */
    user_vm_test_fail_page_after(0);
    assert_eq(-ENOMEM, user_vm_fault(mm, BASE, true));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_double_frees());

    /* Success: present, zeroed. */
    assert_eq(0, user_vm_fault(mm, BASE + 0x10, true));
    assert_eq(1, user_vm_test_leaf_state(BASE));
    uint64_t pa = user_vm_test_leaf_pa(BASE);
    assert_true(pa != 0);
    for (size_t i = 0; i < PAGE_4K_SIZE; i++)
        assert_eq(0, ((unsigned char *)Phy_To_Virt(pa))[i]);

    /* A second fault on a present leaf is refused. */
    assert_eq(-EFAULT, user_vm_fault(mm, BASE, true));

    /* Guard/hole and PROT_NONE pages are refused. */
    assert_eq(-EFAULT, user_vm_fault(mm, BASE + 0x2000, true));
    assert_eq(0, user_vm_mprotect(mm, BASE + 0x1000, 0x1000, PROT_NONE));
    assert_eq(-EFAULT, user_vm_fault(mm, BASE + 0x1000, true));
    assert_eq(-EFAULT, user_vm_fault(mm, BASE + 0x1000, false));

    destroy_mm(mm);
}

TEST_FUNC(test_demand_fault_permission)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x1000, PROT_READ,
                                     MAP_ANON_PRIVATE));

    /* Read-fault creates the leaf; write-fault is refused. */
    assert_eq(0, user_vm_fault(mm, BASE, false));
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq(-EFAULT, user_vm_fault(mm, BASE, true));

    /* The heap is RW, so a write fault succeeds there. */
    assert_eq((int64_t)(HEAP_BASE + 0x1000),
              user_vm_brk(mm, HEAP_BASE + 0x1000));
    assert_eq(0, user_vm_fault(mm, HEAP_BASE, true));

    destroy_mm(mm);
}

/* ── Tests: copy pre-instantiation ───────────────────────────── */

TEST_FUNC(test_copy_across_hole_creates_nothing)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                     MAP_ANON_PRIVATE));

    /* Page 1 is authorised, page 2 is a hole: the whole-read-only
     * validation pass must reject before creating the first page. */
    assert_eq(-EFAULT, user_vm_prepare_copy(mm, BASE, 0x2000, true));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_page_allocs());

    destroy_mm(mm);
}

TEST_FUNC(test_copy_second_leaf_failure_rolls_back_first)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x2000, PROT_RW,
                                     MAP_ANON_PRIVATE));

    /* First map succeeds, second fails => the first leaf and its page
     * must be rolled back. */
    user_vm_test_fail_map_after(1);
    assert_eq(-ENOMEM, user_vm_prepare_copy(mm, BASE, 0x2000, true));
    assert_eq(0, user_vm_test_leaf_count());
    /* The failing page was allocated and freed, then the first leaf
     * was rolled back and freed: two frees, no double free. */
    assert_eq(2, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());

    /* Retry without injection: both pages exist and are owned. */
    assert_eq(0, user_vm_prepare_copy(mm, BASE, 0x2000, true));
    assert_eq(2, user_vm_test_leaf_count());
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq(1, user_vm_test_leaf_state(BASE + 0x1000));

    destroy_mm(mm);
    assert_eq(4, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());
}

TEST_FUNC(test_copy_existing_leaf_permissions)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    /* Simulate an image-owned (ELF/stack) leaf outside every VMA. */
    uint64_t elf_pa = alloc_4k_page();
    assert_true(elf_pa != 0);
    assert_eq(0, arch_vmm_map_4k_new((uint64_t *)(uintptr_t)0x8000,
                                     elf_pa, 0x600000,
                                     VM_USER_RO | VM_NO_EXEC));

    assert_eq(0, user_vm_prepare_copy(mm, 0x600000, 0x1000, false));
    assert_eq(-EFAULT, user_vm_prepare_copy(mm, 0x600000, 0x1000, true));

    /* Tear the fake image leaf down by hand: it is not policy-owned. */
    assert_eq(0, arch_vmm_unmap_4k((uint64_t *)(uintptr_t)0x8000,
                                   0x600000, NULL, NULL));
    free_4k_page(elf_pa);

    destroy_mm(mm);
}

/* ── Tests: stash and ownership ──────────────────────────────── */

TEST_FUNC(test_prot_none_stash_roundtrip_and_free_once)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                     MAP_ANON_PRIVATE));
    assert_eq(0, user_vm_fault(mm, BASE, true));
    uint64_t pa = user_vm_test_leaf_pa(BASE);
    write_page(pa, 0x5a);

    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_NONE));
    assert_eq(2, user_vm_test_leaf_state(BASE));
    assert_eq(pa, user_vm_test_leaf_pa(BASE));
    assert_eq((unsigned char)0x5a,
              ((unsigned char *)Phy_To_Virt(pa))[0]);

    /* NONE restores the original data with the new permission. */
    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_READ));
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq(pa, user_vm_test_leaf_pa(BASE));
    assert_eq((unsigned char)0x5a,
              ((unsigned char *)Phy_To_Virt(pa))[0]);

    /* munmap of a stashed leaf frees the PA exactly once. */
    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_NONE));
    assert_eq(0, user_vm_munmap(mm, BASE, 0x1000));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(1, user_vm_test_free_count());
    assert_eq(pa, user_vm_test_free_pa_at(0));

    destroy_mm(mm);
    assert_eq(1, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());
}

TEST_FUNC(test_destroy_frees_stashed_leaf)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                     MAP_ANON_PRIVATE));
    assert_eq(0, user_vm_fault(mm, BASE, true));
    uint64_t pa = user_vm_test_leaf_pa(BASE);
    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_NONE));

    destroy_mm(mm);
    assert_eq(1, user_vm_test_free_count());
    assert_eq(pa, user_vm_test_free_pa_at(0));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_double_frees());
}

/* ── Tests: brk shrink ───────────────────────────────────────── */

TEST_FUNC(test_brk_shrink_zeroes_tail_and_releases_pages)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + 0x2000),
              user_vm_brk(mm, HEAP_BASE + 0x2000));
    assert_eq(0, user_vm_fault(mm, HEAP_BASE, true));
    assert_eq(0, user_vm_fault(mm, HEAP_BASE + 0x1000, true));

    uint64_t first_pa = user_vm_test_leaf_pa(HEAP_BASE);
    write_page(first_pa, 0xa5);

    /* Same-page shrink: bytes [new_break, page_end) are zeroed, the
     * prefix survives, and the page beyond the new aligned boundary
     * is released while the retained page is not. */
    assert_eq((int64_t)(HEAP_BASE + 0x100),
              user_vm_brk(mm, HEAP_BASE + 0x100));
    for (size_t i = 0; i < 0x100; i++)
        assert_eq(0xa5, ((unsigned char *)Phy_To_Virt(first_pa))[i]);
    for (size_t i = 0x100; i < PAGE_4K_SIZE; i++)
        assert_eq(0, ((unsigned char *)Phy_To_Virt(first_pa))[i]);
    assert_eq(1, user_vm_test_leaf_state(HEAP_BASE));
    assert_eq(0, user_vm_test_leaf_state(HEAP_BASE + 0x1000));
    assert_eq(1, user_vm_test_free_count());

    /* Growing the metadata back does not allocate a leaf. */
    assert_eq((int64_t)(HEAP_BASE + 0x2000),
              user_vm_brk(mm, HEAP_BASE + 0x2000));
    assert_eq(1, user_vm_test_leaf_count());

    /* Page-aligned shrink below the first page frees both leaves. */
    assert_eq((int64_t)HEAP_BASE, user_vm_brk(mm, HEAP_BASE));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(2, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());

    destroy_mm(mm);
}

/* ── Tests: mprotect resident updates ────────────────────────── */

TEST_FUNC(test_mprotect_updates_resident_and_untouched_pages)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x2000, PROT_RW,
                                     MAP_ANON_PRIVATE));
    assert_eq(0, user_vm_fault(mm, BASE, true));
    assert_eq(0, user_vm_fault(mm, BASE + 0x1000, true));

    assert_eq(0, user_vm_mprotect(mm, BASE + 0x1000, 0x1000, PROT_READ));
    assert_eq(VM_USER_RW | VM_NO_EXEC, user_vm_test_leaf_vm(BASE));
    assert_eq(VM_USER_RO | VM_NO_EXEC,
              user_vm_test_leaf_vm(BASE + 0x1000));

    /* The VMA description of the still-absent page is updated too. */
    assert_eq(0, user_vm_mprotect(mm, BASE + 0x1000, 0x1000, PROT_RW));
    assert_eq(0, user_vm_munmap(mm, BASE + 0x1000, 0x1000));
    vma_t *v = vm_area_lookup(mm, BASE);
    assert_not_null(v);
    assert_eq(BASE + 0x1000, v->vm_end);

    destroy_mm(mm);
}

/* ── Tests: mmap argument contract ───────────────────────────── */

TEST_FUNC(test_mmap_argument_contract)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    /* Requires MAP_PRIVATE | MAP_ANONYMOUS. */
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_RW, 0));
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                               MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS));
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                               MAP_ANONYMOUS | 0x8000));
    /* Legally parsed but unsupported requests. */
    assert_eq(-EOPNOTSUPP, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                   MAP_PRIVATE));
    assert_eq(-EOPNOTSUPP, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                   MAP_ANON_PRIVATE | MAP_FIXED));

    /* Raw 64-bit fd/off validation. */
    uint64_t args[6] = { 0, 0x1000, PROT_RW, MAP_ANON_PRIVATE, 0, 0 };
    assert_eq(-EINVAL, user_vm_mmap(mm, args));
    args[4] = 0xffffffffULL;
    assert_eq(-EINVAL, user_vm_mmap(mm, args));
    args[4] = UINT64_MAX;
    args[5] = 1;
    assert_eq(-EINVAL, user_vm_mmap(mm, args));

    /* Permission combinations. */
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_WRITE,
                               MAP_ANON_PRIVATE));
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_EXEC,
                               MAP_ANON_PRIVATE));
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0, PROT_RW, MAP_ANON_PRIVATE));

    /* Hints: unaligned is illegal, a busy hint falls back to base. */
    assert_eq(-EINVAL, test_mmap_call(mm, BASE + 1, 0x1000, PROT_RW,
                               MAP_ANON_PRIVATE));
    assert_eq((int64_t)BASE, test_mmap_call(mm, BASE, 0x1000, PROT_RW,
                                     MAP_ANON_PRIVATE));
    assert_eq((int64_t)(BASE + 0x1000),
              test_mmap_call(mm, BASE, 0x1000, PROT_RW, MAP_ANON_PRIVATE));

    destroy_mm(mm);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_mmap_and_brk_growth_allocate_no_pages),
    TEST_ENTRY(test_mprotect_split_node_failure_changes_nothing),
    TEST_ENTRY(test_munmap_across_holes_and_fragments),
    TEST_ENTRY(test_demand_fault_allocates_zero_page_and_oom),
    TEST_ENTRY(test_demand_fault_permission),
    TEST_ENTRY(test_copy_across_hole_creates_nothing),
    TEST_ENTRY(test_copy_second_leaf_failure_rolls_back_first),
    TEST_ENTRY(test_copy_existing_leaf_permissions),
    TEST_ENTRY(test_prot_none_stash_roundtrip_and_free_once),
    TEST_ENTRY(test_destroy_frees_stashed_leaf),
    TEST_ENTRY(test_brk_shrink_zeroes_tail_and_releases_pages),
    TEST_ENTRY(test_mprotect_updates_resident_and_untouched_pages),
    TEST_ENTRY(test_mmap_argument_contract),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
