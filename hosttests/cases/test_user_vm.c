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

#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_vm.h>
#include <intr/dispatch.h>
#include <memory/user_vm.h>
#include <memory/vm_area.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <uapi/syscall.h>

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
    /* Legally parsed but unsupported requests: shared-alone is
     * syntactically valid (exactly one of the pair) and must report
     * -EOPNOTSUPP, not -EINVAL. */
    assert_eq(-EOPNOTSUPP, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                   MAP_SHARED | MAP_ANONYMOUS));
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

/* ── Task 3: transaction layer + fault classification ────────── */

#define ESR_LOWER_DATA(fsc, write, extra) \
    ((UINT64_C(0x24) << AARCH64_ESR_EC_SHIFT) | \
     ((uint64_t)(fsc) & AARCH64_ESR_ISS_FSC_MASK) | \
     ((write) ? AARCH64_ESR_ISS_WNR : 0) | (extra))

static void tx_setup(mm_t *mm)
{
    user_vm_test_set_current_mm(mm);
    user_vm_test_set_frame_valid(1);
    user_vm_test_set_ttbr0((uint64_t)(uintptr_t)mm->pgdir);
    user_vm_test_set_published(1);
}

static struct intr_event make_fault_event(uint64_t esr)
{
    struct intr_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = INTR_SYNC;
    ev.origin = INTR_ORIGIN_USER;
    ev.reason = INTR_REASON_MEMORY;
    ev.native_status = esr;
    ev.fault_addr_valid = true;
    ev.fault_addr = HEAP_BASE;
    ev.native_address = HEAP_BASE;
    return ev;
}

TEST_FUNC(test_fault_classification_pure)
{
    bool write = true;
    bool cow = true;

    assert_true(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x04, false, 0), true, &write, &cow));
    assert_false(write);
    assert_false(cow);

    assert_true(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x07, true, 0), true, &write, &cow));
    assert_true(write);
    assert_false(cow);

    /* A level-3 write permission fault is the only COW shape. */
    assert_true(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x0f, true, 0), true, &write, &cow));
    assert_true(write);
    assert_true(cow);

    /* Higher-level permission and read permission faults are neither
     * demand-filled nor COW. */
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x0f, false, 0), true, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x0c, true, 0), true, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x0d, true, 0), true, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x0e, true, 0), true, NULL, NULL));

    /* Access-flag faults keep the old path. */
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x08, false, 0), true, NULL, NULL));

    /* S1PTW / CM / invalid FAR are rejected for both shapes. */
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x04, false, AARCH64_ESR_ISS_S1PTW), true, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x04, false, AARCH64_ESR_ISS_CM), true, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x04, false, 0), false, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        ESR_LOWER_DATA(0x0f, true, AARCH64_ESR_ISS_S1PTW), true, NULL, NULL));

    /* Instruction abort and a same-EL data abort are not EL0 faults. */
    assert_false(aarch64_user_vm_fault_classify(
        (UINT64_C(0x20) << AARCH64_ESR_EC_SHIFT) | 0x04, true, NULL, NULL));
    assert_false(aarch64_user_vm_fault_classify(
        (UINT64_C(0x25) << AARCH64_ESR_EC_SHIFT) | 0x04, true, NULL, NULL));
}

TEST_FUNC(test_transaction_native_success_restores_state)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + 0x1000),
              user_vm_brk(mm, HEAP_BASE + 0x1000));
    tx_setup(mm);
    user_vm_test_set_tls(0x1111, 0x2222);
    user_vm_test_set_irq(1);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    aarch64_vm_native_enter(&regs, SYS_brk);

    uint64_t args[6] = { HEAP_BASE + 0x2000, 0, 0, 0, 0, 0 };
    assert_eq((int64_t)(HEAP_BASE + 0x2000),
              arch_syscall_user_vm(SYS_brk, args));

    /* One detach and one reinstall; the original root came back and
     * both TLS registers plus the entry IRQ state were restored. */
    assert_eq(1, user_vm_test_root_switches());
    assert_eq(1, user_vm_test_tls_restores());
    assert_eq((uint64_t)(uintptr_t)mm->pgdir,
              user_vm_test_installed_root());
    assert_eq(0x1111, user_vm_test_live_tls(0));
    assert_eq(0x2222, user_vm_test_live_tls(1));
    assert_eq(1, user_vm_test_irq_masked());

    aarch64_vm_native_leave();
    destroy_mm(mm);
}

TEST_FUNC(test_transaction_refuses_unverified_contexts)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    mm_t *other = new_mm(HEAP_BASE);
    tx_setup(mm);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    uint64_t args[6] = { HEAP_BASE + 0x1000, 0, 0, 0, 0, 0 };

    /* Wrong/unverified formal frame. */
    user_vm_test_set_frame_valid(0);
    aarch64_vm_native_enter(&regs, SYS_brk);
    assert_eq(-EPERM, arch_syscall_user_vm(SYS_brk, args));

    /* Verified frame, but the current mm is no longer the authorised
     * mm. */
    user_vm_test_set_frame_valid(1);
    aarch64_vm_native_enter(&regs, SYS_brk);
    user_vm_test_set_current_mm(other);
    assert_eq(-EPERM, arch_syscall_user_vm(SYS_brk, args));

    /* TTBR0 no longer holds the mm root: no authorization at all. */
    user_vm_test_set_current_mm(mm);
    user_vm_test_set_ttbr0(0xdead000);
    aarch64_vm_native_enter(&regs, SYS_brk);
    assert_eq(-EPERM, arch_syscall_user_vm(SYS_brk, args));

    /* Published-root check failure. */
    user_vm_test_set_ttbr0((uint64_t)(uintptr_t)mm->pgdir);
    user_vm_test_set_published(0);
    aarch64_vm_native_enter(&regs, SYS_brk);
    assert_eq(-EPERM, arch_syscall_user_vm(SYS_brk, args));

    /* SYS_exit never authorizes (non-returning). */
    user_vm_test_set_published(1);
    aarch64_vm_native_enter(&regs, SYS_exit);
    assert_eq(-EPERM, arch_syscall_user_vm(SYS_brk, args));

    /* A number without a handler never authorizes. */
    aarch64_vm_native_enter(&regs, SYS_read);
    assert_eq(-EPERM, arch_syscall_user_vm(SYS_brk, args));

    assert_eq(0, user_vm_test_root_switches());
    assert_eq(0, user_vm_test_tls_restores());
    assert_eq(1, user_vm_test_irq_masked());

    aarch64_vm_native_leave();
    user_vm_destroy(other);
    destroy_mm(mm);
    free(other);
}

TEST_FUNC(test_transaction_irq_depth_and_nesting_refused)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    tx_setup(mm);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    uint64_t args[6] = { HEAP_BASE + 0x1000, 0, 0, 0, 0, 0 };
    aarch64_vm_native_enter(&regs, SYS_brk);

    /* An IRQ handler on the same BSP may not borrow the SVC
     * authorization. */
    aarch64_vm_irq_enter();
    assert_eq(-EBUSY, arch_syscall_user_vm(SYS_brk, args));
    aarch64_vm_irq_leave();
    assert_eq(0, user_vm_test_root_switches());

    /* A second transaction while the first is detached is refused. */
    assert_eq(0, aarch64_vm_edit_begin(mm));
    assert_eq(-EBUSY, aarch64_vm_edit_begin(mm));
    assert_eq(-EBUSY, arch_syscall_user_vm(SYS_brk, args));
    aarch64_vm_edit_end();
    assert_eq(1, user_vm_test_root_switches());
    assert_eq(1, user_vm_test_tls_restores());

    aarch64_vm_native_leave();
    destroy_mm(mm);
}

TEST_FUNC(test_transaction_policy_error_still_restores)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + 0x1000),
              user_vm_brk(mm, HEAP_BASE + 0x1000));
    tx_setup(mm);
    user_vm_test_set_tls(0xa1a1, 0xb2b2);
    user_vm_test_set_irq(0);            /* entry IRQs enabled */

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    aarch64_vm_native_enter(&regs, SYS_clock_gettime);

    user_vm_test_fail_page_after(0);
    assert_eq(-ENOMEM, aarch64_user_vm_prepare_copy(mm, HEAP_BASE, 0x1000,
                                                    true));
    assert_eq(0, user_vm_test_leaf_count());

    /* The failed policy call still went through detach/restore. */
    assert_eq(1, user_vm_test_root_switches());
    assert_eq((uint64_t)(uintptr_t)mm->pgdir,
              user_vm_test_installed_root());
    assert_eq(0xa1a1, user_vm_test_live_tls(0));
    assert_eq(0xb2b2, user_vm_test_live_tls(1));
    assert_eq(0, user_vm_test_irq_masked());   /* entry state enabled */

    /* Not left detached: another transaction is allowed. */
    assert_eq(0, aarch64_vm_edit_begin(mm));
    aarch64_vm_edit_end();

    aarch64_vm_native_leave();
    destroy_mm(mm);
}

TEST_FUNC(test_fault_entry_fills_and_refuses)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + 0x2000),
              user_vm_brk(mm, HEAP_BASE + 0x2000));
    tx_setup(mm);
    user_vm_test_set_tls(0x3300, 0x4400);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.elr_el1 = 0x1234;

    struct intr_event ev = make_fault_event(ESR_LOWER_DATA(0x07, true, 0));
    assert_eq(0, aarch64_user_vm_fault(&regs, &ev));
    assert_eq(1, user_vm_test_leaf_state(HEAP_BASE));
    assert_eq(0x1234, regs.elr_el1);     /* same PC is retried */
    assert_eq(0x3300, user_vm_test_live_tls(0));
    assert_eq(0x4400, user_vm_test_live_tls(1));

    int switches = user_vm_test_root_switches();

    /* Access-flag/permission/invalid-FAR faults keep the old exit. */
    struct intr_event bad = make_fault_event(ESR_LOWER_DATA(0x08, false, 0));
    assert_eq(-EFAULT, aarch64_user_vm_fault(&regs, &bad));
    bad = make_fault_event(ESR_LOWER_DATA(0x0c, true, 0));
    assert_eq(-EFAULT, aarch64_user_vm_fault(&regs, &bad));
    bad = make_fault_event(ESR_LOWER_DATA(0x04, false, 0));
    bad.fault_addr_valid = false;
    assert_eq(-EFAULT, aarch64_user_vm_fault(&regs, &bad));
    assert_eq(switches, user_vm_test_root_switches());

    /* A policy OOM reports -ENOMEM, never -EFAULT. */
    user_vm_test_fail_page_after(0);
    struct intr_event oom =
        make_fault_event(ESR_LOWER_DATA(0x04, true, 0));
    oom.fault_addr = HEAP_BASE + 0x1000;
    oom.native_address = oom.fault_addr;
    assert_eq(-ENOMEM, aarch64_user_vm_fault(&regs, &oom));
    assert_eq(0x3300, user_vm_test_live_tls(0));

    destroy_mm(mm);
}

/* ── Image-page ownership (M4 Task 1) ───────────────────────────
 *
 * The ELF/stack loader hands each leaf to the mm through the resident
 * chain; the record carries its source, logical permission and the
 * exclusive/share state, and destroy releases it exactly once. */

static user_vm_page_t *resident_at(mm_t *mm, uint64_t va)
{
    for (list_t *p = mm->resident_list.next; p; p = p->next) {
        user_vm_page_t *r = (user_vm_page_t *)p;   /* list is first */
        if (r->va == va)
            return r;
    }
    return NULL;
}

TEST_FUNC(test_map_image_page_records_source_and_ownership)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    uint64_t elf_pa = alloc_4k_page();
    uint64_t stk_pa = alloc_4k_page();
    assert_true(elf_pa != 0 && stk_pa != 0);

    assert_eq(0, user_vm_map_image_page(mm, 0x400000, elf_pa,
                                        VM_USER_RO | VM_NO_EXEC));
    assert_eq(0, user_vm_map_image_page(mm, AARCH64_USER_STACK_BASE, stk_pa,
                                        VM_USER_RW | VM_NO_EXEC));

    assert_eq(2, user_vm_test_leaf_count());
    assert_eq(elf_pa, user_vm_test_leaf_pa(0x400000));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(0x400000));
    assert_eq(stk_pa, user_vm_test_leaf_pa(AARCH64_USER_STACK_BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(AARCH64_USER_STACK_BASE));

    user_vm_page_t *elf = resident_at(mm, 0x400000);
    assert_not_null(elf);
    assert_eq((uint32_t)USER_VM_PAGE_ELF, elf->source);
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC), elf->logical);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, elf->share);

    user_vm_page_t *stk = resident_at(mm, AARCH64_USER_STACK_BASE);
    assert_not_null(stk);
    assert_eq((uint32_t)USER_VM_PAGE_STACK, stk->source);
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC), stk->logical);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, stk->share);

    /* mm destroy releases every record once, in VA order. */
    user_vm_destroy(mm);
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(2, user_vm_test_free_count());
    assert_eq(elf_pa, user_vm_test_free_pa_at(0));
    assert_eq(stk_pa, user_vm_test_free_pa_at(1));
    assert_false(user_vm_has_metadata(mm));
    free(mm);
}

TEST_FUNC(test_map_image_page_failure_keeps_pa_with_caller)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    uint64_t pa = alloc_4k_page();
    assert_true(pa != 0);

    /* Record allocation failure: nothing mapped, PA untouched. */
    user_vm_test_fail_node_after(0);
    assert_eq(-ENOMEM, user_vm_map_image_page(mm, 0x400000, pa,
                                              VM_USER_RW | VM_NO_EXEC));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_free_count());

    /* Leaf-map failure: the record is released, PA still the caller's. */
    user_vm_test_fail_map_after(0);
    assert_eq(-ENOMEM, user_vm_map_image_page(mm, 0x400000, pa,
                                              VM_USER_RW | VM_NO_EXEC));
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_free_count());

    /* Arguments are checked before anything is allocated or mapped. */
    assert_eq(-EINVAL, user_vm_map_image_page(mm, 0x400000, pa, 0));
    assert_eq(-EINVAL, user_vm_map_image_page(mm, 0x400001, pa,
                                              VM_USER_RW | VM_NO_EXEC));
    assert_eq(-EINVAL, user_vm_map_image_page(mm, 0x400000, 0,
                                              VM_USER_RW | VM_NO_EXEC));
    assert_eq(-EINVAL, user_vm_map_image_page(mm, 0x400000, pa + 1,
                                              VM_USER_RW | VM_NO_EXEC));

    assert_eq(0, user_vm_map_image_page(mm, 0x400000, pa,
                                        VM_USER_RW | VM_NO_EXEC));
    assert_eq(1, user_vm_test_leaf_count());

    destroy_mm(mm);
    assert_eq(1, user_vm_test_free_count());
    assert_eq(pa, user_vm_test_free_pa_at(0));
}

/* ── Checked COW replacement forwarding (M4 Task 1) ─────────────
 *
 * The policy calls arch_user_vm_replace_cow(); the real adapter
 * forwards to the M3 backend.  The fake backend is keyed by (root,
 * VA), so the parent and child trees are independent — exactly what
 * fork needs. */

TEST_FUNC(test_replace_cow_forwarding_parent_child_roots)
{
    user_vm_test_reset();

    uint64_t *parent = (uint64_t *)(uintptr_t)0x8000;
    uint64_t *child  = (uint64_t *)(uintptr_t)0x9000;
    uint64_t old_pa = alloc_4k_page();
    uint64_t new_pa = alloc_4k_page();
    assert_true(old_pa != 0 && new_pa != 0);

    /* The parent leaf is the post-fork RO/NX shared copy. */
    assert_eq(0, arch_vmm_map_4k_new(parent, old_pa, BASE,
                                     VM_USER_RO | VM_NO_EXEC));

    /* The child tree has no leaf at the same VA. */
    assert_eq(-ENOENT, arch_user_vm_replace_cow(child, BASE, old_pa, new_pa));
    /* Wrong old PA, same PA and a misaligned VA are rejected. */
    assert_eq(-EINVAL, arch_user_vm_replace_cow(parent, BASE, new_pa, old_pa));
    assert_eq(-EINVAL, arch_user_vm_replace_cow(parent, BASE, old_pa, old_pa));
    assert_eq(-EINVAL, arch_user_vm_replace_cow(parent, BASE + 1,
                                                old_pa, new_pa));
    /* A writable leaf and a stash are not valid COW sources. */
    assert_eq(0, arch_vmm_map_4k_new(parent, old_pa, BASE + 0x1000,
                                     VM_USER_RW | VM_NO_EXEC));
    assert_eq(-EINVAL, arch_user_vm_replace_cow(parent, BASE + 0x1000,
                                                old_pa, new_pa));
    assert_eq(0, arch_vmm_map_4k_new(parent, old_pa, BASE + 0x2000,
                                     VM_USER_RO | VM_NO_EXEC));
    assert_eq(0, arch_vmm_update_4k(parent, old_pa, BASE + 0x2000,
                                    VM_PROTNONE, NULL, NULL));
    assert_eq(-EINVAL, arch_user_vm_replace_cow(parent, BASE + 0x2000,
                                                old_pa, new_pa));

    /* Success: only the parent tree changes, to the new PA and RW/NX. */
    assert_eq(0, arch_user_vm_replace_cow(parent, BASE, old_pa, new_pa));
    assert_eq(new_pa, user_vm_test_leaf_pa_in(parent, BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(parent, BASE));
    assert_eq(0, user_vm_test_leaf_state_in(child, BASE));

    /* The old PA is still owned by the caller — replacement never
     * frees it. */
    assert_eq(1, user_vm_test_leaf_state_in(parent, BASE));
    free_4k_page(old_pa);
    free_4k_page(new_pa);
}

/* ── fork cloning (M4 Task 2) ───────────────────────────────────
 *
 * user_vm_clone_fork() runs the whole borrow → acquire → publish
 * transaction.  The fake M3 store is keyed by (root, VA), so the
 * parent and child trees are inspected independently. */

static mm_t *new_child_mm(uint64_t root)
{
    mm_t *mm = calloc(1, sizeof(*mm));
    assert_not_null(mm);
    list_init(&mm->vma_list);
    mm->pgdir = (uint64_t *)(uintptr_t)root;
    return mm;
}

struct clone_publish_probe {
    int calls;
    int pid;
};

static int clone_publish_ok(void *arg)
{
    struct clone_publish_probe *p = arg;
    p->calls++;
    p->pid = 1234;
    return 0;
}

static int clone_publish_fail(void *arg)
{
    struct clone_publish_probe *p = arg;
    p->calls++;
    return -ENOMEM;
}

static uint64_t fault_private_page(mm_t *mm, uint64_t va)
{
    if (mm->end_brk < va + PAGE_4K_SIZE)
        assert_eq((int64_t)(va + PAGE_4K_SIZE),
                  user_vm_brk(mm, va + PAGE_4K_SIZE));
    assert_eq(0, user_vm_fault(mm, va, true));
    user_vm_page_t *r = resident_at(mm, va);
    assert_not_null(r);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    return r->pa;
}

TEST_FUNC(test_clone_fork_shares_and_downgrades)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child  = new_child_mm(0x9000);

    uint64_t rw_pa = fault_private_page(parent, HEAP_BASE);
    uint64_t ro_pa = alloc_4k_page();
    assert_true(ro_pa != 0);
    assert_eq(0, user_vm_map_image_page(parent, 0x400000, ro_pa,
                                        VM_USER_RO | VM_NO_EXEC));

    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(parent, child, clone_publish_ok, &probe));
    assert_eq(1, probe.calls);
    assert_eq(1234, probe.pid);

    /* Parent: the writable leaf was lowered, both records are shared,
     * and both pages hold two references. */
    user_vm_page_t *pr = resident_at(parent, HEAP_BASE);
    assert_not_null(pr);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, pr->share);
    assert_eq(2, user_vm_test_cow_refs(rw_pa));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));

    user_vm_page_t *pro = resident_at(parent, 0x400000);
    assert_not_null(pro);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, pro->share);
    assert_eq(2, user_vm_test_cow_refs(ro_pa));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(0x400000));

    /* Child: independent root, same PAs, read-only leaves, shared
     * records and copied metadata. */
    assert_eq(rw_pa, user_vm_test_leaf_pa_in(child->pgdir, HEAP_BASE));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(child->pgdir, HEAP_BASE));
    user_vm_page_t *cr = resident_at(child, HEAP_BASE);
    assert_not_null(cr);
    assert_eq(rw_pa, cr->pa);
    assert_eq((uint32_t)USER_VM_PAGE_ANON, cr->source);
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC), cr->logical);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, cr->share);
    assert_eq(ro_pa, user_vm_test_leaf_pa_in(child->pgdir, 0x400000));

    assert_eq(vma_count(parent), vma_count(child));
    assert_eq(parent->start_brk, child->start_brk);
    assert_eq(parent->end_brk, child->end_brk);
    assert_eq(parent->mmap_base, child->mmap_base);
    assert_eq(parent->start_stack, child->start_stack);

    /* Destroying the child drops its references but frees no page. */
    user_vm_destroy(child);
    assert_eq(1, user_vm_test_cow_refs(rw_pa));
    assert_eq(1, user_vm_test_cow_refs(ro_pa));
    assert_eq(2, user_vm_test_leaf_count());   /* only the parent's */
    assert_eq(0, user_vm_test_free_count());
    assert_false(user_vm_has_metadata(child));
    free(child);

    /* The parent's destroy drops the last reference and frees both. */
    destroy_mm(parent);
    assert_eq(0, user_vm_test_cow_refs(rw_pa));
    assert_eq(0, user_vm_test_cow_refs(ro_pa));
    assert_eq(2, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());
}

TEST_FUNC(test_clone_fork_first_leaf_failure_leaves_parent)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child  = new_child_mm(0x9000);

    uint64_t pa = fault_private_page(parent, HEAP_BASE);
    struct clone_publish_probe probe = { 0, 0 };

    user_vm_test_fail_map_after(0);
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child, clone_publish_ok,
                                          &probe));
    assert_eq(0, probe.calls);

    /* Parent untouched: still private, RW, unreferenced. */
    user_vm_page_t *pr = resident_at(parent, HEAP_BASE);
    assert_not_null(pr);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, pr->share);
    assert_eq(0, user_vm_test_cow_refs(pa));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));

    /* Child empty and handed back to the caller. */
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, HEAP_BASE));
    assert_null(child->resident_list.next);
    assert_eq(0, vma_count(child));
    assert_eq((uint64_t)0, child->start_brk);
    assert_eq(0, user_vm_test_free_count());

    user_vm_destroy(child);
    free(child);
    destroy_mm(parent);
}

TEST_FUNC(test_clone_fork_second_leaf_failure_removes_first)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child  = new_child_mm(0x9000);

    uint64_t pa1 = fault_private_page(parent, HEAP_BASE);
    uint64_t pa2 = fault_private_page(parent, HEAP_BASE + PAGE_4K_SIZE);
    struct clone_publish_probe probe = { 0, 0 };

    /* The first child leaf maps, the second fails. */
    user_vm_test_fail_map_after(1);
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child, clone_publish_ok,
                                          &probe));
    assert_eq(0, probe.calls);

    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, HEAP_BASE));
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir,
                                            HEAP_BASE + PAGE_4K_SIZE));
    assert_eq(0, user_vm_test_cow_refs(pa1));
    assert_eq(0, user_vm_test_cow_refs(pa2));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_null(child->resident_list.next);

    user_vm_destroy(child);
    free(child);
    destroy_mm(parent);
}

TEST_FUNC(test_clone_fork_publish_failure_mixed_refs)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child  = new_child_mm(0x9000);

    /* One private page (first share) and one already-shared page. */
    uint64_t private_pa = fault_private_page(parent, HEAP_BASE);
    uint64_t shared_pa = alloc_4k_page();
    assert_true(shared_pa != 0);
    assert_eq(0, user_vm_map_image_page(parent, 0x400000, shared_pa,
                                        VM_USER_RO | VM_NO_EXEC));
    user_vm_page_t *shared = resident_at(parent, 0x400000);
    assert_not_null(shared);
    shared->share = USER_VM_PAGE_SHARED;
    user_vm_test_set_cow_refs(shared_pa, 2);

    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child, clone_publish_fail,
                                          &probe));
    assert_eq(1, probe.calls);

    /* First-share page: 0→2→0, exclusive again, PA not freed. */
    user_vm_page_t *priv = resident_at(parent, HEAP_BASE);
    assert_not_null(priv);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, priv->share);
    assert_eq(0, user_vm_test_cow_refs(private_pa));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));

    /* Already-shared page: 2→3→2 and its RO/NX leaf is untouched. */
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, shared->share);
    assert_eq(2, user_vm_test_cow_refs(shared_pa));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(0x400000));

    /* No child leaf, record, VMA or freed page survives. */
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, HEAP_BASE));
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, 0x400000));
    assert_null(child->resident_list.next);
    assert_eq(0, vma_count(child));
    assert_eq(0, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());

    user_vm_destroy(child);
    free(child);
    user_vm_destroy(parent);
    free(parent);
}

TEST_FUNC(test_clone_fork_overflow_fails_preflight)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child  = new_child_mm(0x9000);

    uint64_t pa = fault_private_page(parent, HEAP_BASE);
    user_vm_page_t *pr = resident_at(parent, HEAP_BASE);
    assert_not_null(pr);
    pr->share = USER_VM_PAGE_SHARED;
    assert_eq(0, arch_vmm_update_4k(parent->pgdir, pa, HEAP_BASE,
                                    VM_USER_RO | VM_NO_EXEC, NULL, NULL));
    user_vm_test_set_cow_refs(pa, UINT16_MAX);

    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(-EOVERFLOW, user_vm_clone_fork(parent, child, clone_publish_ok,
                                             &probe));
    assert_eq(0, probe.calls);
    assert_eq((uint16_t)UINT16_MAX, user_vm_test_cow_refs(pa));
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, pr->share);
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, HEAP_BASE));
    assert_null(child->resident_list.next);

    user_vm_destroy(child);
    free(child);
    user_vm_destroy(parent);
    free(parent);
}

TEST_FUNC(test_clone_fork_twice_2_3_2)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child1 = new_child_mm(0x9000);
    mm_t *child2 = new_child_mm(0xa000);

    uint64_t pa = fault_private_page(parent, HEAP_BASE);
    struct clone_publish_probe probe = { 0, 0 };

    assert_eq(0, user_vm_clone_fork(parent, child1, clone_publish_ok, &probe));
    assert_eq(2, user_vm_test_cow_refs(pa));

    /* Second fork shares the already-shared page: 2→3→2 on failure. */
    probe = (struct clone_publish_probe){ 0, 0 };
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child2, clone_publish_fail,
                                          &probe));
    assert_eq(1, probe.calls);
    assert_eq(2, user_vm_test_cow_refs(pa));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_null(child2->resident_list.next);

    user_vm_destroy(child2);
    free(child2);

    /* The first child still holds its reference. */
    assert_eq(2, user_vm_test_cow_refs(pa));
    user_vm_destroy(child1);
    free(child1);
    assert_eq(1, user_vm_test_cow_refs(pa));

    destroy_mm(parent);
    assert_eq(0, user_vm_test_cow_refs(pa));
    assert_eq(1, user_vm_test_free_count());
}

/* ── COW write fault, mprotect/brk sharing, copy staging (Task 3) ─ */

static uint64_t fault_mmap_page(mm_t *mm, uint64_t va)
{
    assert_eq(0, user_vm_fault(mm, va, true));
    user_vm_page_t *r = resident_at(mm, va);
    assert_not_null(r);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    return r->pa;
}

/* Put a resident page into the post-fork shared state: RO/NX hardware
 * for a logical-RW page, the requested hardware word otherwise, and
 * `refs` PMM COW references. */
static user_vm_page_t *make_shared_page(mm_t *mm, uint64_t va, uint16_t refs,
                                        uint32_t logical)
{
    user_vm_page_t *r = resident_at(mm, va);
    assert_not_null(r);
    r->share = USER_VM_PAGE_SHARED;
    r->logical = logical;

    uint32_t hw = logical;
    if (logical == VM_PROTNONE)
        hw = VM_PROTNONE;
    else if (logical & VM_WRITE)
        hw = VM_USER_RO | VM_NO_EXEC;
    assert_eq(0, arch_vmm_update_4k(mm->pgdir, r->pa, va, hw, NULL, NULL));
    user_vm_test_set_cow_refs(r->pa, refs);
    return r;
}

static int resident_count(mm_t *mm)
{
    int n = 0;
    for (list_t *p = mm->resident_list.next; p; p = p->next)
        n++;
    return n;
}

TEST_FUNC(test_cow_write_fault_copies_multi_ref_page)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    uint64_t old_pa = fault_private_page(mm, HEAP_BASE);
    memset(Phy_To_Virt(old_pa), 0x5a, PAGE_4K_SIZE);
    user_vm_page_t *r = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);

    assert_eq(0, user_vm_cow_fault(mm, HEAP_BASE + 0x40));

    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    assert_true(r->pa != old_pa);
    assert_eq(1, user_vm_test_cow_refs(old_pa));
    assert_eq(0, user_vm_test_cow_refs(r->pa));
    assert_eq(r->pa, user_vm_test_leaf_pa(HEAP_BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(0, user_vm_test_free_count());   /* old page still owned */
    assert_mem_eq(Phy_To_Virt(old_pa), Phy_To_Virt(r->pa), PAGE_4K_SIZE);

    destroy_mm(mm);
    assert_eq(1, user_vm_test_free_count());   /* only the private copy */
    free_4k_page(old_pa);
}

TEST_FUNC(test_cow_write_fault_upgrades_unique_ref)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    uint64_t pa = fault_private_page(mm, HEAP_BASE);
    user_vm_page_t *r = make_shared_page(mm, HEAP_BASE, 1,
                                         VM_USER_RW | VM_NO_EXEC);

    assert_eq(0, user_vm_cow_fault(mm, HEAP_BASE));

    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    assert_eq(pa, r->pa);
    assert_eq(pa, user_vm_test_leaf_pa(HEAP_BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(0, user_vm_test_cow_refs(pa));
    assert_eq(0, user_vm_test_free_count());

    destroy_mm(mm);
    assert_eq(1, user_vm_test_free_count());
}

TEST_FUNC(test_cow_write_fault_rejections)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    uint64_t pa = fault_private_page(mm, HEAP_BASE);
    user_vm_page_t *r = resident_at(mm, HEAP_BASE);
    assert_not_null(r);

    /* A private page is not a COW target. */
    assert_eq(-EFAULT, user_vm_cow_fault(mm, HEAP_BASE));
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);

    /* A shared page without logical write permission is refused. */
    r = make_shared_page(mm, HEAP_BASE, 2, VM_USER_RO | VM_NO_EXEC);
    assert_eq(-EFAULT, user_vm_cow_fault(mm, HEAP_BASE));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(2, user_vm_test_cow_refs(pa));

    /* A shared writable leaf (hardware not RO/NX) is refused. */
    r->logical = VM_USER_RW | VM_NO_EXEC;
    assert_eq(0, arch_vmm_update_4k(mm->pgdir, pa, HEAP_BASE,
                                    VM_USER_RW | VM_NO_EXEC, NULL, NULL));
    assert_eq(-EFAULT, user_vm_cow_fault(mm, HEAP_BASE));

    /* No record at the address. */
    assert_eq(-EFAULT, user_vm_cow_fault(mm, HEAP_BASE + PAGE_4K_SIZE));

    /* A multi-reference OOM preserves the whole shared state. */
    assert_eq(0, arch_vmm_update_4k(mm->pgdir, pa, HEAP_BASE,
                                    VM_USER_RO | VM_NO_EXEC, NULL, NULL));
    user_vm_test_fail_page_after(0);
    assert_eq(-ENOMEM, user_vm_cow_fault(mm, HEAP_BASE));
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, r->share);
    assert_eq(pa, user_vm_test_leaf_pa(HEAP_BASE));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(2, user_vm_test_cow_refs(pa));

    /* Leave the mock reference count at zero so destroy frees the page. */
    user_vm_test_set_cow_refs(pa, 1);
    destroy_mm(mm);
}

TEST_FUNC(test_mprotect_shared_keeps_hardware_readonly)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x1000, PROT_RW, MAP_ANON_PRIVATE));
    fault_mmap_page(mm, BASE);
    user_vm_page_t *r = make_shared_page(mm, BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);

    /* RW mprotect keeps the shared leaf hardware RO/NX. */
    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_RW));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC), r->logical);
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(BASE));
    assert_eq(2, user_vm_test_cow_refs(r->pa));

    /* NONE still stashes, and the reference survives. */
    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_NONE));
    assert_eq(2, user_vm_test_leaf_state(BASE));
    assert_eq((uint32_t)VM_PROTNONE, r->logical);
    assert_eq(2, user_vm_test_cow_refs(r->pa));

    /* Restoring RW re-establishes logical write but stays RO/NX. */
    assert_eq(0, user_vm_mprotect(mm, BASE, 0x1000, PROT_RW));
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC), r->logical);
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(BASE));
    assert_eq(2, user_vm_test_cow_refs(r->pa));

    /* The first write then makes it private. */
    uint64_t old_pa = r->pa;
    assert_eq(0, user_vm_cow_fault(mm, BASE));
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    assert_true(r->pa != old_pa);
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(BASE));

    user_vm_test_set_cow_refs(old_pa, 1);
    destroy_mm(mm);
}

TEST_FUNC(test_brk_shrink_privatizes_shared_tail)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    fault_private_page(mm, HEAP_BASE);
    fault_private_page(mm, HEAP_BASE + PAGE_4K_SIZE);

    user_vm_page_t *tail = resident_at(mm, HEAP_BASE);
    assert_not_null(tail);
    uint64_t old_pa = make_shared_page(mm, HEAP_BASE, 2,
                                       VM_USER_RW | VM_NO_EXEC)->pa;
    memset(Phy_To_Virt(old_pa), 0xA5, PAGE_4K_SIZE);

    assert_eq((int64_t)(HEAP_BASE + 0x800),
              user_vm_brk(mm, HEAP_BASE + 0x800));

    /* The retained tail is a private copy with the head preserved and
     * the tail beyond the new break zeroed. */
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, tail->share);
    assert_true(tail->pa != old_pa);
    assert_eq(1, user_vm_test_cow_refs(old_pa));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));

    unsigned char *head = Phy_To_Virt(tail->pa);
    for (size_t i = 0; i < 0x800; i++)
        if (head[i] != 0xA5) { assert_true(0); break; }
    for (size_t i = 0x800; i < PAGE_4K_SIZE; i++)
        if (head[i] != 0x00) { assert_true(0); break; }

    /* The peer's page was never modified. */
    unsigned char *peer = Phy_To_Virt(old_pa);
    for (size_t i = 0; i < PAGE_4K_SIZE; i++)
        if (peer[i] != 0xA5) { assert_true(0); break; }

    /* The fully-outside page was released. */
    assert_eq(0, user_vm_test_leaf_state(HEAP_BASE + PAGE_4K_SIZE));
    assert_eq(1, user_vm_test_free_count());

    user_vm_test_set_cow_refs(old_pa, 1);
    destroy_mm(mm);
}

TEST_FUNC(test_brk_shrink_shared_tail_oom_preserves_state)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    fault_private_page(mm, HEAP_BASE);
    fault_private_page(mm, HEAP_BASE + PAGE_4K_SIZE);

    user_vm_page_t *tail = resident_at(mm, HEAP_BASE);
    assert_not_null(tail);
    uint64_t old_pa = make_shared_page(mm, HEAP_BASE, 2,
                                       VM_USER_RW | VM_NO_EXEC)->pa;
    memset(Phy_To_Virt(old_pa), 0xA5, PAGE_4K_SIZE);
    uint64_t old_end = mm->end_brk;

    user_vm_test_fail_page_after(0);
    assert_eq(-ENOMEM, user_vm_brk(mm, HEAP_BASE + 0x800));

    assert_eq(old_end, mm->end_brk);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, tail->share);
    assert_eq(old_pa, tail->pa);
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(2, user_vm_test_cow_refs(old_pa));
    assert_eq(1, user_vm_test_leaf_state(HEAP_BASE + PAGE_4K_SIZE));
    unsigned char *peer = Phy_To_Virt(old_pa);
    for (size_t i = 0; i < PAGE_4K_SIZE; i++)
        if (peer[i] != 0xA5) { assert_true(0); break; }
    assert_eq(0, user_vm_test_free_count());

    user_vm_test_set_cow_refs(old_pa, 1);
    destroy_mm(mm);
}

TEST_FUNC(test_prepare_copy_rejects_hole_without_allocating)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + PAGE_4K_SIZE),
              user_vm_brk(mm, HEAP_BASE + PAGE_4K_SIZE));

    /* The second page lies outside the heap VMA: the read-only
     * validation rejects the whole range before any allocation. */
    assert_eq(-EFAULT, user_vm_prepare_copy(mm, HEAP_BASE,
                                            PAGE_4K_SIZE * 2, true));
    assert_eq(0, user_vm_test_page_allocs());
    assert_eq(0, user_vm_test_leaf_count());
    assert_eq(0, resident_count(mm));

    destroy_mm(mm);
}

TEST_FUNC(test_prepare_copy_map_failure_preserves_three_sources)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + PAGE_4K_SIZE * 4),
              user_vm_brk(mm, HEAP_BASE + PAGE_4K_SIZE * 4));

    /* A: multi-reference COW; B: unique-reference COW; C/D: absent. */
    fault_private_page(mm, HEAP_BASE);
    fault_private_page(mm, HEAP_BASE + PAGE_4K_SIZE);
    user_vm_page_t *a = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);
    user_vm_page_t *b = make_shared_page(mm, HEAP_BASE + PAGE_4K_SIZE, 1,
                                         VM_USER_RW | VM_NO_EXEC);
    uint64_t a_pa = a->pa;
    uint64_t b_pa = b->pa;

    /* The first absent leaf maps, the second map fails. */
    user_vm_test_fail_map_after(1);
    assert_eq(-ENOMEM, user_vm_prepare_copy(mm, HEAP_BASE,
                                            PAGE_4K_SIZE * 4, true));

    /* The multi-reference COW source is untouched. */
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, a->share);
    assert_eq(a_pa, a->pa);
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(2, user_vm_test_cow_refs(a_pa));

    /* The unique-reference COW source is untouched. */
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, b->share);
    assert_eq(b_pa, b->pa);
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE + PAGE_4K_SIZE));
    assert_eq(1, user_vm_test_cow_refs(b_pa));

    /* Every temporary absent leaf/record is gone. */
    assert_eq(0, user_vm_test_leaf_state(HEAP_BASE + PAGE_4K_SIZE * 2));
    assert_eq(0, user_vm_test_leaf_state(HEAP_BASE + PAGE_4K_SIZE * 3));
    assert_eq(2, resident_count(mm));
    assert_eq(2, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_double_frees());
    /* Two setup pages + COW copy + first absent page + failed map page. */
    assert_eq(5, user_vm_test_page_allocs());
    assert_eq(3, user_vm_test_free_count());

    user_vm_test_set_cow_refs(a_pa, 1);
    user_vm_destroy(mm);
    free(mm);
}

TEST_FUNC(test_prepare_copy_success_commits_cow_and_absent)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)(HEAP_BASE + PAGE_4K_SIZE * 4),
              user_vm_brk(mm, HEAP_BASE + PAGE_4K_SIZE * 4));

    fault_private_page(mm, HEAP_BASE);
    fault_private_page(mm, HEAP_BASE + PAGE_4K_SIZE);
    user_vm_page_t *a = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);
    user_vm_page_t *b = make_shared_page(mm, HEAP_BASE + PAGE_4K_SIZE, 1,
                                         VM_USER_RW | VM_NO_EXEC);
    uint64_t a_old = a->pa;
    uint64_t b_old = b->pa;

    assert_eq(0, user_vm_prepare_copy(mm, HEAP_BASE, PAGE_4K_SIZE * 4,
                                      true));

    /* Multi-reference page: a private copy, old ref dropped. */
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, a->share);
    assert_true(a->pa != a_old);
    assert_eq(1, user_vm_test_cow_refs(a_old));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));

    /* Unique-reference page: same PA, upgraded in place. */
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, b->share);
    assert_eq(b_old, b->pa);
    assert_eq(0, user_vm_test_cow_refs(b_old));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE + PAGE_4K_SIZE));

    /* The two absent pages now have private leaves and records. */
    for (int i = 2; i < 4; i++) {
        uint64_t va = HEAP_BASE + (uint64_t)i * PAGE_4K_SIZE;
        assert_eq(1, user_vm_test_leaf_state(va));
        assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
                  user_vm_test_leaf_vm(va));
        user_vm_page_t *r = resident_at(mm, va);
        assert_not_null(r);
        assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    }
    assert_eq(4, resident_count(mm));
    assert_eq(4, user_vm_test_leaf_count());

    user_vm_test_set_cow_refs(a_old, 1);
    user_vm_destroy(mm);
    assert_eq(4, user_vm_test_free_count());
    free(mm);
    free_4k_page(a_old);
}

TEST_FUNC(test_fault_entry_cow_write)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    uint64_t old_pa = fault_private_page(mm, HEAP_BASE);
    user_vm_page_t *r = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);
    tx_setup(mm);
    user_vm_test_set_irq(0);              /* entry IRQs enabled */
    user_vm_test_set_tls(0x5500, 0x6600);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.elr_el1 = 0x2000;

    struct intr_event ev = make_fault_event(ESR_LOWER_DATA(0x0f, true, 0));
    assert_eq(0, aarch64_user_vm_fault(&regs, &ev));
    assert_eq(0x2000, regs.elr_el1);          /* same PC retried */
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    assert_true(r->pa != old_pa);
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));
    assert_eq(0x5500, user_vm_test_live_tls(0));
    assert_eq(0x6600, user_vm_test_live_tls(1));
    assert_eq(0, user_vm_test_irq_masked());

    user_vm_test_set_cow_refs(old_pa, 1);
    destroy_mm(mm);
}

/* ── Clone with a stashed page (review: mixed present/stash/shared) ─ */

TEST_FUNC(test_clone_fork_stash_mixed_success_and_second_fork)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(parent, 0, PAGE_4K_SIZE * 2, PROT_RW,
                             MAP_ANON_PRIVATE));

    /* Page A: stashed (PROT_NONE) before the fork. */
    fault_mmap_page(parent, BASE);
    assert_eq(0, user_vm_mprotect(parent, BASE, PAGE_4K_SIZE, PROT_NONE));
    user_vm_page_t *stash = resident_at(parent, BASE);
    assert_not_null(stash);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, stash->share);
    assert_eq((uint32_t)VM_PROTNONE, stash->logical);
    assert_eq(2, user_vm_test_leaf_state(BASE));

    /* Page B: already shared present.  Page C: private present. */
    fault_mmap_page(parent, BASE + PAGE_4K_SIZE);
    /* One reference: the parent's own shared state, so the fork takes
     * it 1→2 and a second fork 2→3→2. */
    user_vm_page_t *shared = make_shared_page(parent, BASE + PAGE_4K_SIZE, 1,
                                              VM_USER_RW | VM_NO_EXEC);
    uint64_t private_pa = fault_private_page(parent, HEAP_BASE);

    mm_t *child1 = new_child_mm(0x9000);
    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(parent, child1, clone_publish_ok, &probe));

    /* The stashed page is copied as a stash at the same PA. */
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, stash->share);
    assert_eq(2, user_vm_test_cow_refs(stash->pa));
    assert_eq(2, user_vm_test_leaf_state(BASE));
    assert_eq(2, user_vm_test_leaf_state_in(child1->pgdir, BASE));
    assert_eq(stash->pa, user_vm_test_leaf_pa_in(child1->pgdir, BASE));
    user_vm_page_t *cstash = resident_at(child1, BASE);
    assert_not_null(cstash);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, cstash->share);
    assert_eq((uint32_t)VM_PROTNONE, cstash->logical);

    assert_eq(2, user_vm_test_cow_refs(shared->pa));
    assert_eq(2, user_vm_test_cow_refs(private_pa));
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm(HEAP_BASE));

    /* A second fork with a publish failure: every existing shared
     * entry, present and stashed alike, goes 2→3→2. */
    mm_t *child2 = new_child_mm(0xa000);
    probe = (struct clone_publish_probe){ 0, 0 };
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child2, clone_publish_fail,
                                          &probe));
    assert_eq(1, probe.calls);
    assert_eq(2, user_vm_test_cow_refs(stash->pa));
    assert_eq(2, user_vm_test_cow_refs(shared->pa));
    assert_eq(2, user_vm_test_cow_refs(private_pa));
    assert_eq(2, user_vm_test_leaf_state(BASE));
    assert_eq(0, user_vm_test_leaf_state_in(child2->pgdir, BASE));
    assert_eq(0, user_vm_test_leaf_state_in(child2->pgdir, HEAP_BASE));
    assert_null(child2->resident_list.next);

    user_vm_destroy(child2);
    free(child2);

    /* The first child releases one reference per page on destruction;
     * the stashed leaf is removed and its PA is not freed yet. */
    user_vm_destroy(child1);
    assert_eq(1, user_vm_test_cow_refs(stash->pa));
    assert_eq(1, user_vm_test_cow_refs(shared->pa));
    assert_eq(1, user_vm_test_cow_refs(private_pa));
    assert_eq(3, user_vm_test_leaf_count());
    assert_eq(0, user_vm_test_free_count());
    assert_false(user_vm_has_metadata(child1));
    free(child1);

    destroy_mm(parent);
    assert_eq(0, user_vm_test_cow_refs(stash->pa));
    assert_eq(3, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());
}

TEST_FUNC(test_clone_fork_stash_rollback_unmaps_stashed_child)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(parent, 0, PAGE_4K_SIZE, PROT_RW,
                             MAP_ANON_PRIVATE));

    /* The stashed page is first in VA order, so it maps before the
     * injected failure and the rollback must remove a STASHED child
     * leaf. */
    fault_mmap_page(parent, BASE);
    assert_eq(0, user_vm_mprotect(parent, BASE, PAGE_4K_SIZE, PROT_NONE));
    user_vm_page_t *stash = resident_at(parent, BASE);
    assert_not_null(stash);

    uint64_t private_pa = fault_private_page(parent, HEAP_BASE);
    mm_t *child = new_child_mm(0x9000);
    struct clone_publish_probe probe = { 0, 0 };

    user_vm_test_fail_map_after(1);
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child, clone_publish_ok,
                                          &probe));
    assert_eq(0, probe.calls);

    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, BASE));
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir, HEAP_BASE));
    assert_null(child->resident_list.next);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, stash->share);
    assert_eq(0, user_vm_test_cow_refs(stash->pa));
    assert_eq(2, user_vm_test_leaf_state(BASE));
    assert_eq(0, user_vm_test_cow_refs(private_pa));
    assert_eq(0, user_vm_test_free_count());

    user_vm_destroy(child);
    free(child);
    destroy_mm(parent);
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
    TEST_ENTRY(test_fault_classification_pure),
    TEST_ENTRY(test_transaction_native_success_restores_state),
    TEST_ENTRY(test_transaction_refuses_unverified_contexts),
    TEST_ENTRY(test_transaction_irq_depth_and_nesting_refused),
    TEST_ENTRY(test_transaction_policy_error_still_restores),
    TEST_ENTRY(test_fault_entry_fills_and_refuses),
    TEST_ENTRY(test_map_image_page_records_source_and_ownership),
    TEST_ENTRY(test_map_image_page_failure_keeps_pa_with_caller),
    TEST_ENTRY(test_replace_cow_forwarding_parent_child_roots),
    TEST_ENTRY(test_clone_fork_shares_and_downgrades),
    TEST_ENTRY(test_clone_fork_first_leaf_failure_leaves_parent),
    TEST_ENTRY(test_clone_fork_second_leaf_failure_removes_first),
    TEST_ENTRY(test_clone_fork_publish_failure_mixed_refs),
    TEST_ENTRY(test_clone_fork_overflow_fails_preflight),
    TEST_ENTRY(test_clone_fork_twice_2_3_2),
    TEST_ENTRY(test_clone_fork_stash_mixed_success_and_second_fork),
    TEST_ENTRY(test_clone_fork_stash_rollback_unmaps_stashed_child),
    TEST_ENTRY(test_cow_write_fault_copies_multi_ref_page),
    TEST_ENTRY(test_cow_write_fault_upgrades_unique_ref),
    TEST_ENTRY(test_cow_write_fault_rejections),
    TEST_ENTRY(test_mprotect_shared_keeps_hardware_readonly),
    TEST_ENTRY(test_brk_shrink_privatizes_shared_tail),
    TEST_ENTRY(test_brk_shrink_shared_tail_oom_preserves_state),
    TEST_ENTRY(test_prepare_copy_rejects_hole_without_allocating),
    TEST_ENTRY(test_prepare_copy_map_failure_preserves_three_sources),
    TEST_ENTRY(test_prepare_copy_success_commits_cow_and_absent),
    TEST_ENTRY(test_fault_entry_cow_write),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
