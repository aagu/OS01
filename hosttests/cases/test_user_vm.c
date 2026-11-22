/*
 * hosttests/cases/test_user_vm.c — AArch64 M4 anonymous-VM policy.
 *
 * Links the REAL kernel/arch/aarch64/memory/user_vm_policy.c policy, the REAL shared
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
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <intr/dispatch.h>
#include <arch/aarch64/user_vm_leaf.h>
#include <memory/vm_area.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <memory/shared_backing.h>
#include <uapi/syscall.h>

/* Host pthread prototypes (avoiding the libc pthread.h shadow) — same
 * pattern as test_usb_msc.c. */
typedef unsigned long host_pthread_t;
extern int pthread_create(host_pthread_t *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg);
extern int pthread_join(host_pthread_t thread, void **retval);

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root"
#endif

#define BASE       UINT64_C(0x40000000)
#define HEAP_BASE  UINT64_C(0x500000)
#define PROT_RW    (PROT_READ | PROT_WRITE)
#define MAP_ANON_PRIVATE (MAP_PRIVATE | MAP_ANONYMOUS)

/* ── Helpers ─────────────────────────────────────────────────── */

static jmp_buf g_fatal_jb;
static int g_fatal_armed;

void vmm_gate_violation(const char *reason)
{
    (void)reason;
    if (!g_fatal_armed) {
        fprintf(stderr, "vmm_gate_violation outside armed test: %s\n",
                reason ? reason : "(null)");
        abort();
    }
    g_fatal_armed = 0;
    longjmp(g_fatal_jb, 1);
}

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
    /* 2026-11-19: shared-anonymous is served now (it allocates a VMA
     * at BASE); file-backed without a valid fd (fd=-1 here) is
     * -EINVAL, not the v1 blanket -EOPNOTSUPP. */
    assert_eq((int64_t)BASE, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                     MAP_SHARED | MAP_ANONYMOUS));
    assert_eq(2, vma_count(mm));
    assert_eq(0, user_vm_munmap(mm, BASE, 0x1000));
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                   MAP_PRIVATE));
    /* 2026-11-19: MAP_FIXED is served for private-anonymous requests;
     * without a usable hint it is -EINVAL (POSIX-aligned), not the v1
     * blanket -EOPNOTSUPP. */
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_RW,
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

TEST_FUNC(edit_quiesce_error_restores_context)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    int64_t mapped = test_mmap_call(mm, BASE, 0x1000, PROT_RW, MAP_ANON_PRIVATE);
    assert_eq((int64_t)BASE, mapped);
    tx_setup(mm);

    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.elr_el1 = 0x1234;
    struct intr_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.origin = INTR_ORIGIN_USER;
    ev.native_status = ((uint64_t)AARCH64_EC_DATA_ABORT_LOWER <<
                        AARCH64_ESR_EC_SHIFT) |
                       0x07ULL | AARCH64_ESR_ISS_WNR;
    ev.fault_addr_valid = true;
    ev.fault_addr = BASE;
    assert_eq(0, aarch64_user_vm_fault(&regs, &ev));
    assert_eq(1, user_vm_test_leaf_state(BASE));
    uint64_t orig_pa = user_vm_test_leaf_pa(BASE);
    uint32_t orig_vm = user_vm_test_leaf_vm(BASE);

    struct task_struct *tsk = user_vm_test_task(0);
    struct aarch64_user_task *owner = aarch64_user_task_owner(tsk);
    assert_not_null(owner);

    user_vm_test_set_tls(0x1111, 0x2222);
    user_vm_test_set_irq(0);            /* entry IRQs enabled */
    aarch64_vm_native_enter(&regs, SYS_munmap);

    user_vm_test_set_quiesce_error(-EAGAIN);
    uint64_t args[6] = { BASE, 0x1000, 0, 0, 0, 0 };
    assert_eq(-EAGAIN, arch_syscall_user_vm(SYS_munmap, args));

    assert_eq((uint64_t)(uintptr_t)mm->pgdir,
              user_vm_test_installed_root());
    assert_eq(0x1111, user_vm_test_live_tls(0));
    assert_eq(0x2222, user_vm_test_live_tls(1));
    assert_eq(0, user_vm_test_irq_masked());
    assert_false(aarch64_vm_task_editing(tsk));
    assert_eq(0ULL, owner->vm.edit_irq);
    assert_eq(0ULL, owner->vm.edit_tpidr_el0);
    assert_eq(0ULL, owner->vm.edit_tpidrro_el0);
    assert_eq(0ULL, owner->vm.edit_root_pa);
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq(orig_pa, user_vm_test_leaf_pa(BASE));
    assert_eq(orig_vm, user_vm_test_leaf_vm(BASE));
    assert_not_null(vm_area_lookup(mm, BASE));

    /* Reset fault and retry with the same native authorization. */
    user_vm_test_set_quiesce_error(0);
    assert_eq(0, arch_syscall_user_vm(SYS_munmap, args));
    assert_eq(0, user_vm_test_leaf_state(BASE));
    assert_null(vm_area_lookup(mm, BASE));

    aarch64_vm_native_leave();
    destroy_mm(mm);
}

TEST_FUNC(edit_quiesce_busy_fails_stopped)
{
    int cases[2] = { -EBUSY, -EINVAL };
    for (int i = 0; i < 2; i++) {
        user_vm_test_reset();
        mm_t *mm = new_mm(HEAP_BASE);
        int64_t mapped = test_mmap_call(mm, BASE, 0x1000, PROT_RW,
                                        MAP_ANON_PRIVATE);
        assert_eq((int64_t)BASE, mapped);
        tx_setup(mm);

        pt_regs_t regs;
        memset(&regs, 0, sizeof(regs));
        regs.elr_el1 = 0x1234;
        struct intr_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.origin = INTR_ORIGIN_USER;
        ev.native_status = ((uint64_t)AARCH64_EC_DATA_ABORT_LOWER <<
                            AARCH64_ESR_EC_SHIFT) |
                           0x07ULL | AARCH64_ESR_ISS_WNR;
        ev.fault_addr_valid = true;
        ev.fault_addr = BASE;
        assert_eq(0, aarch64_user_vm_fault(&regs, &ev));
        int installs_before = user_vm_test_root_installs();

        user_vm_test_set_tls(0x5555, 0x6666);
        user_vm_test_set_irq(0);
        aarch64_vm_native_enter(&regs, SYS_munmap);
        user_vm_test_set_quiesce_error(cases[i]);

        uint64_t args[6] = { BASE, 0x1000, 0, 0, 0, 0 };
        g_fatal_armed = 1;
        int tripped = setjmp(g_fatal_jb);
        if (tripped == 0) {
            (void)arch_syscall_user_vm(SYS_munmap, args);
        }
        g_fatal_armed = 0;

        assert_eq(1, tripped);
        assert_eq(installs_before, user_vm_test_root_installs());
        assert_eq(1, user_vm_test_irq_masked());
        assert_eq(1, user_vm_test_leaf_state(BASE));
        assert_not_null(vm_area_lookup(mm, BASE));

        aarch64_vm_native_leave();
        destroy_mm(mm);
    }
}

TEST_FUNC(test_transaction_serial_interleaving_per_task)
{
    user_vm_test_reset();
    mm_t *mm_a = new_mm(HEAP_BASE);
    mm_t *mm_b = new_mm(HEAP_BASE);
    mm_b->pgdir = (uint64_t *)(uintptr_t)0x9000;

    struct task_struct *task_a = user_vm_test_task(0);
    struct task_struct *task_b = user_vm_test_task(1);
    assert_not_null(task_a);
    assert_not_null(task_b);

    pt_regs_t regs_a;
    pt_regs_t regs_b;
    memset(&regs_a, 0, sizeof(regs_a));
    memset(&regs_b, 0, sizeof(regs_b));

    /* Task A authorizes and detaches (edit_begin). */
    user_vm_test_set_current_task(task_a);
    tx_setup(mm_a);
    user_vm_test_set_tls(0x1111, 0x2222);
    user_vm_test_set_irq(0);
    aarch64_vm_native_enter(&regs_a, SYS_brk);
    assert_eq(0, aarch64_vm_edit_begin(mm_a));
    assert_true(aarch64_vm_task_editing(task_a));
    assert_false(aarch64_vm_task_editing(task_b));
    assert_eq(0, user_vm_test_live_tls(0));
    assert_eq(0, user_vm_test_live_tls(1));
    assert_eq(0, user_vm_test_installed_root());

    /* Switch mock current to Task B while Task A's edit is paused. */
    user_vm_test_set_current_task(task_b);
    tx_setup(mm_b);
    user_vm_test_set_tls(0x3333, 0x4444);
    user_vm_test_set_irq(1);
    aarch64_vm_native_enter(&regs_b, SYS_brk);

    /* Non-zero IRQ depth refuses edit_begin with -EBUSY. */
    aarch64_vm_irq_enter();
    assert_eq(-EBUSY, aarch64_vm_edit_begin(mm_b));
    aarch64_vm_irq_leave();

    assert_eq(0, aarch64_vm_edit_begin(mm_b));
    assert_true(aarch64_vm_task_editing(task_a));
    assert_true(aarch64_vm_task_editing(task_b));
    aarch64_vm_edit_end();
    assert_true(aarch64_vm_task_editing(task_a));
    assert_false(aarch64_vm_task_editing(task_b));
    assert_eq((uint64_t)(uintptr_t)mm_b->pgdir,
              user_vm_test_installed_root());
    assert_eq(0x3333, user_vm_test_live_tls(0));
    assert_eq(0x4444, user_vm_test_live_tls(1));
    assert_eq(1, user_vm_test_irq_masked());
    aarch64_vm_native_leave();

    /* Switch back to Task A (paused edit sees kernel root + zeroed TLS). */
    user_vm_test_set_current_task(task_a);
    user_vm_test_set_current_mm(mm_a);
    aarch64_user_restore_kernel_root();
    user_vm_test_set_tls(0, 0);
    assert_true(aarch64_vm_task_editing(task_a));
    assert_false(aarch64_vm_task_editing(task_b));

    /* Task A's edit_end restores A's root, TLS, and entry IRQ state. */
    aarch64_vm_edit_end();
    assert_false(aarch64_vm_task_editing(task_a));
    assert_eq((uint64_t)(uintptr_t)mm_a->pgdir,
              user_vm_test_installed_root());
    assert_eq(0x1111, user_vm_test_live_tls(0));
    assert_eq(0x2222, user_vm_test_live_tls(1));
    assert_eq(0, user_vm_test_irq_masked());
    aarch64_vm_native_leave();

    destroy_mm(mm_b);
    destroy_mm(mm_a);
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

static void drop_peer_cow_ref(uint64_t old_pa)
{
    bool peer_last = true;
    assert_eq(0, pmm_cow_release(old_pa, &peer_last));
    assert_false(peer_last);
}

TEST_FUNC(test_cow_write_fault_concurrent_last_ref_frees_old_page)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    uint64_t old_pa = fault_private_page(mm, HEAP_BASE);
    memset(Phy_To_Virt(old_pa), 0x5a, PAGE_4K_SIZE);
    user_vm_page_t *r = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);

    user_vm_test_on_replace_cow(drop_peer_cow_ref);
    assert_eq(0, user_vm_cow_fault(mm, HEAP_BASE + 0x40));

    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    assert_true(r->pa != old_pa);
    assert_eq(0, user_vm_test_cow_refs(old_pa));
    assert_eq(0, user_vm_test_cow_refs(r->pa));
    assert_eq(1, user_vm_test_free_count());   /* old_pa freed once */
    for (size_t i = 0; i < PAGE_4K_SIZE; i++)
        assert_eq(0x5a, ((const uint8_t *)Phy_To_Virt(r->pa))[i]);

    destroy_mm(mm);
    assert_eq(2, user_vm_test_free_count());   /* + private copy */
}

TEST_FUNC(test_prepare_copy_concurrent_last_ref_frees_old_page)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    uint64_t old_pa = fault_private_page(mm, HEAP_BASE);
    memset(Phy_To_Virt(old_pa), 0x3c, PAGE_4K_SIZE);
    user_vm_page_t *r = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);

    user_vm_test_on_replace_cow(drop_peer_cow_ref);
    assert_eq(0, user_vm_prepare_copy(mm, HEAP_BASE, PAGE_4K_SIZE, true));

    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r->share);
    assert_true(r->pa != old_pa);
    assert_eq(0, user_vm_test_cow_refs(old_pa));
    assert_eq(1, user_vm_test_free_count());   /* old_pa freed once */
    for (size_t i = 0; i < PAGE_4K_SIZE; i++)
        assert_eq(0x3c, ((const uint8_t *)Phy_To_Virt(r->pa))[i]);

    destroy_mm(mm);
    assert_eq(2, user_vm_test_free_count());   /* + private copy */
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

/* Pass-2 preallocation failure on the SECOND multi-reference COW leaf:
 * the first temporary copy is returned exactly once, both original
 * leaves (PA, refs, content, records) are untouched, and no absent
 * leaf or chain edit happens. */
TEST_FUNC(test_prepare_copy_second_cow_alloc_failure)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    fault_private_page(mm, HEAP_BASE);
    fault_private_page(mm, HEAP_BASE + PAGE_4K_SIZE);
    user_vm_page_t *a = make_shared_page(mm, HEAP_BASE, 2,
                                         VM_USER_RW | VM_NO_EXEC);
    user_vm_page_t *b = make_shared_page(mm, HEAP_BASE + PAGE_4K_SIZE, 2,
                                         VM_USER_RW | VM_NO_EXEC);
    uint64_t a_pa = a->pa, b_pa = b->pa;
    write_page(a_pa, 0xA5);
    write_page(b_pa, 0x5A);

    assert_eq(2, resident_count(mm));
    assert_eq(2, user_vm_test_leaf_count());
    int frees0 = user_vm_test_free_count();
    int allocs0 = user_vm_test_page_allocs();

    /* First temporary page succeeds, second fails. */
    user_vm_test_fail_page_after(1);
    assert_eq(-ENOMEM, user_vm_prepare_copy(mm, HEAP_BASE,
                                            PAGE_4K_SIZE * 2, true));

    /* Two alloc calls (the second one failed — the counter counts
     * calls, not successes), exactly one net free: the first
     * temporary copy.  No double free. */
    assert_eq(allocs0 + 2, user_vm_test_page_allocs());
    assert_eq(frees0 + 1, user_vm_test_free_count());
    assert_eq(0, user_vm_test_double_frees());

    /* Both original pages and records retained. */
    assert_eq(2, resident_count(mm));
    assert_eq(2, user_vm_test_leaf_count());
    assert_eq(a_pa, a->pa);
    assert_eq(b_pa, b->pa);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, a->share);
    assert_eq((uint32_t)USER_VM_PAGE_SHARED, b->share);
    assert_eq(2, user_vm_test_cow_refs(a_pa));
    assert_eq(2, user_vm_test_cow_refs(b_pa));
    assert_eq((unsigned char)0xA5,
              ((unsigned char *)Phy_To_Virt(a_pa))[0]);
    assert_eq((unsigned char)0x5A,
              ((unsigned char *)Phy_To_Virt(b_pa))[0]);
    /* Hardware leaves unchanged: still RO/NX (no COW replace ran). */
    uint64_t qpa = 0;
    uint32_t qvm = 0;
    assert_eq(0, arch_vmm_query_4k(mm->pgdir, HEAP_BASE, &qpa, &qvm));
    assert_eq(a_pa, qpa);
    assert_true((qvm & VM_WRITE) == 0);

    user_vm_test_set_cow_refs(a_pa, 1);
    user_vm_test_set_cow_refs(b_pa, 1);
    user_vm_destroy(mm);
    assert_eq(0, user_vm_test_double_frees());
    free(mm);
    free_4k_page(a_pa);
    free_4k_page(b_pa);
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


/* ── Tests: MAP_FIXED (private-anonymous replace, 2026-11-19) ── */

TEST_FUNC(test_mmap_fixed_replaces_overlap_and_keeps_neighbors)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    /* One 3-page mapping; the fixed request replaces the middle page. */
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x3000, PROT_RW, MAP_ANON_PRIVATE));
    assert_eq(2, vma_count(mm));

    assert_eq((int64_t)(BASE + 0x1000),
              test_mmap_call(mm, BASE + 0x1000, 0x1000, PROT_READ,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    /* The 3-page VMA split into three: head RW, fixed middle RO, tail RW. */
    assert_eq(4, vma_count(mm));
    vma_t *head = vm_area_lookup(mm, BASE);
    vma_t *mid  = vm_area_lookup(mm, BASE + 0x1000);
    vma_t *tail = vm_area_lookup(mm, BASE + 0x2000);
    assert_not_null(head); assert_not_null(mid); assert_not_null(tail);
    assert_eq(BASE, head->vm_start);
    assert_eq(BASE + 0x1000, head->vm_end);
    assert_true((head->vm_flags & VMA_PROT_WRITE) != 0);
    assert_eq(BASE + 0x1000, mid->vm_start);
    assert_eq(BASE + 0x2000, mid->vm_end);
    assert_true((mid->vm_flags & VMA_PROT_WRITE) == 0);
    assert_eq(BASE + 0x2000, tail->vm_start);
    assert_eq(BASE + 0x3000, tail->vm_end);
    assert_true((tail->vm_flags & VMA_PROT_WRITE) != 0);

    /* A fixed request matching a whole existing VMA replaces it
     * without leaving fragments. */
    assert_eq((int64_t)(BASE + 0x1000),
              test_mmap_call(mm, BASE + 0x1000, 0x1000, PROT_RW,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(4, vma_count(mm));
    mid = vm_area_lookup(mm, BASE + 0x1000);
    assert_not_null(mid);
    assert_true((mid->vm_flags & VMA_PROT_WRITE) != 0);

    destroy_mm(mm);
}

TEST_FUNC(test_mmap_fixed_no_hint_is_einval)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq(-EINVAL, test_mmap_call(mm, 0, 0x1000, PROT_RW,
                                      MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(1, vma_count(mm));            /* heap only */
    destroy_mm(mm);
}

TEST_FUNC(test_mmap_fixed_rejects_non_mmap_overlap)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    /* The heap VMA is not an mmap VMA: a fixed request covering it
     * must refuse, before any state changes. */
    assert_eq(-EINVAL,
              test_mmap_call(mm, HEAP_BASE, 0x1000, PROT_RW,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    /* MMAP base window boundary: a fixed request below it is refused. */
    assert_eq(-EINVAL,
              test_mmap_call(mm, HEAP_BASE + 0x100000, 0x1000, PROT_RW,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(1, vma_count(mm));
    destroy_mm(mm);
}

TEST_FUNC(test_mmap_fixed_split_node_failure_changes_nothing)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x3000, PROT_RW, MAP_ANON_PRIVATE));

    /* The middle-page replace needs two split nodes + one new VMA
     * node; failing any preallocation must leave the old mapping
     * intact (no half-released window). */
    user_vm_test_fail_node_after(0);
    assert_eq(-ENOMEM,
              test_mmap_call(mm, BASE + 0x1000, 0x1000, PROT_READ,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(2, vma_count(mm));
    vma_t *whole = vm_area_lookup(mm, BASE);
    assert_not_null(whole);
    assert_eq(BASE + 0x3000, whole->vm_end);
    assert_true((whole->vm_flags & VMA_PROT_WRITE) != 0);

    user_vm_test_fail_node_after(2);
    assert_eq(-ENOMEM,
              test_mmap_call(mm, BASE + 0x1000, 0x1000, PROT_READ,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(2, vma_count(mm));
    whole = vm_area_lookup(mm, BASE);
    assert_not_null(whole);
    assert_eq(BASE + 0x3000, whole->vm_end);

    /* Without injection the replace succeeds. */
    assert_eq((int64_t)(BASE + 0x1000),
              test_mmap_call(mm, BASE + 0x1000, 0x1000, PROT_READ,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(4, vma_count(mm));
    destroy_mm(mm);
}


/* ── Tests: file-backed + shared mappings (2026-11-19) ────────── */

TEST_FUNC(test_mmap_file_backed_demand_fill)
{
    user_vm_test_reset();
    static const unsigned char bytes[8192] =
        { [0] = 0x41, [4096] = 0x42 };
    struct vfs_node *node =
        user_vm_test_make_file_node(bytes, sizeof(bytes));
    file_t f = { .type = FD_VFS, .node = node };
    user_vm_test_set_files(&f);
    user_vm_test_set_file_bytes(bytes, sizeof(bytes));

    mm_t *mm = new_mm(HEAP_BASE);
    /* fd 0, offset 4096: one page filled with byte 0x42. */
    uint64_t args[6] = { 0, 0x1000, PROT_RW,
                         MAP_PRIVATE, 0, 4096 };
    assert_eq((int64_t)BASE, user_vm_mmap(mm, args));
    assert_eq(2, vma_count(mm));
    /* Metadata only: the leaf store stays absent (0) until the first
     * touch. */
    assert_eq(0, user_vm_test_leaf_state(BASE));

    assert_eq(0, user_vm_fault(mm, BASE, false));
    user_vm_page_t *r = resident_at(mm, BASE);
    assert_not_null(r);
    unsigned char *pg = (unsigned char *)Phy_To_Virt(r->pa);
    assert_eq(0x42, pg[0]);
    assert_eq(0x00, pg[1]);      /* short fill keeps the zero page */
    assert_eq((int64_t)BASE, (int64_t)r->va);

    /* The VMA owns the node reference; destroy drops it. */
    destroy_mm(mm);
    user_vm_test_set_files(NULL);
}

TEST_FUNC(test_mmap_shared_anon_fork_write_through)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(parent, 0, 0x1000, PROT_RW,
                             MAP_SHARED | MAP_ANONYMOUS));
    assert_eq(0, user_vm_fault(parent, BASE, true));
    user_vm_page_t *pr = resident_at(parent, BASE);
    assert_not_null(pr);
    assert_eq((uint32_t)USER_VM_PAGE_BACKING, pr->source);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, pr->share);
    uint64_t pa = pr->pa;
    memset(Phy_To_Virt(pa), 0x5A, PAGE_4K_SIZE);

    mm_t *child  = new_child_mm(0x9000);
    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(parent, child,
                                    clone_publish_ok, &probe));

    /* Both sides hold the object page directly at RW/NX and the same
     * PA: fork neither downgrades the parent leaf nor touches the PMM
     * COW map (object pages are borrowed, never COW-counted). */
    assert_eq(pa, user_vm_test_leaf_pa_in(parent->pgdir, BASE));
    assert_eq(pa, user_vm_test_leaf_pa_in(child->pgdir, BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(parent->pgdir, BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(child->pgdir, BASE));
    user_vm_page_t *cr = resident_at(child, BASE);
    assert_not_null(cr);
    assert_eq((uint32_t)USER_VM_PAGE_BACKING, cr->source);
    assert_eq(0, user_vm_test_cow_refs(pa));

    /* The shared backing write-fault path keeps the leaf in place —
     * and an ordinary child store needs no fault at all. */
    assert_eq(0, user_vm_cow_fault(child, BASE));
    assert_eq(pa, user_vm_test_leaf_pa_in(child->pgdir, BASE));
    memset(Phy_To_Virt(pa), 0x7C, PAGE_4K_SIZE);
    assert_eq(0x7C, *(unsigned char *)Phy_To_Virt(pa));

    /* Contrast: a PRIVATE mapping's write fault privatizes. */
    mm_t *p2 = new_mm(HEAP_BASE);
    assert_eq((int64_t)(BASE + 0x2000),
              test_mmap_call(p2, BASE + 0x2000, 0x1000, PROT_RW,
                             MAP_ANON_PRIVATE));
    assert_eq(0, user_vm_fault(p2, BASE + 0x2000, true));
    mm_t *c2 = new_child_mm(0xa000);
    struct clone_publish_probe probe2 = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(p2, c2, clone_publish_ok, &probe2));
    assert_eq(0, user_vm_cow_fault(c2, BASE + 0x2000));
    user_vm_page_t *c2r = resident_at(c2, BASE + 0x2000);
    assert_not_null(c2r);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, c2r->share);
    assert_true(c2r->pa != user_vm_test_leaf_pa_in(p2->pgdir,
                                                   BASE + 0x2000));

    destroy_mm(parent);
    destroy_mm(child);
    destroy_mm(p2);
    destroy_mm(c2);
}


/* ── Tests: device mmap (/dev/mem, 2026-11-19) ───────────────── */

TEST_FUNC(test_mmap_device_map_and_teardown_never_frees)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    /* A fake /dev/mem fd: FD_DEV with a node named "mem". */
    static struct vfs_node mem_node;
    static char mem_name[] = "mem";
    mem_node.name = mem_name;
    file_t f = { .type = FD_DEV, .node = &mem_node };
    user_vm_test_set_files(&f);

    /* Map two device pages from the DTB device window. */
    uint64_t args[6] = { BASE, 0x2000, PROT_RW, MAP_SHARED,
                         0, 0x08000000 };
    assert_eq((int64_t)BASE, user_vm_mmap(mm, args));
    assert_eq(2, vma_count(mm));            /* heap + the VMA_IO node */
    vma_t *v = vm_area_lookup(mm, BASE);
    assert_not_null(v);
    assert_true((v->vm_flags & VMA_IO) != 0);
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq(0x08000000, user_vm_test_leaf_pa(BASE));
    assert_eq(0x08001000, user_vm_test_leaf_pa(BASE + 0x1000));

    int frees_before = user_vm_test_free_count();
    destroy_mm(mm);
    /* Teardown unmaps but never frees a device PA. */
    assert_eq(frees_before, user_vm_test_free_count());
    assert_eq(0, user_vm_test_leaf_count());
    user_vm_test_set_files(NULL);
}

TEST_FUNC(test_mmap_device_rejects_out_of_window_and_rolls_back)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    static struct vfs_node mem_node;
    static char mem_name[] = "mem";
    mem_node.name = mem_name;
    file_t f = { .type = FD_DEV, .node = &mem_node };
    user_vm_test_set_files(&f);

    /* PA outside the device window: refused before anything maps. */
    uint64_t args[6] = { BASE, 0x1000, PROT_RW, MAP_SHARED,
                         0, 0x40000000 };
    assert_eq(-EINVAL, user_vm_mmap(mm, args));
    assert_eq(1, vma_count(mm));
    assert_eq(0, user_vm_test_leaf_count());

    /* MAP_PRIVATE on a device is refused. */
    uint64_t args2[6] = { BASE, 0x1000, PROT_RW, MAP_PRIVATE,
                          0, 0x08000000 };
    assert_eq(-EINVAL, user_vm_mmap(mm, args2));
    assert_eq(1, vma_count(mm));

    /* Non-device fds never take the device path (fd=-1 anonymous). */
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x1000, PROT_RW, MAP_ANON_PRIVATE));
    assert_eq(2, vma_count(mm));
    destroy_mm(mm);
    user_vm_test_set_files(NULL);
}


/* ── Review-fix tests (final review 2026-11-19) ──────────────── */

TEST_FUNC(test_mmap_device_occupied_range_rejected)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    static struct vfs_node mem_node;
    static char mem_name[] = "mem";
    mem_node.name = mem_name;
    file_t f = { .type = FD_DEV, .node = &mem_node };
    user_vm_test_set_files(&f);

    /* An anon mapping occupies [BASE, BASE+0x1000).  A device mapping
     * over the same range must be refused whole — not silently
     * inserted as an overlapping VMA node. */
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x1000, PROT_RW, MAP_ANON_PRIVATE));
    uint64_t args[6] = { BASE, 0x2000, PROT_RW, MAP_SHARED,
                         0, 0x08000000 };
    assert_eq(-EINVAL, user_vm_mmap(mm, args));
    assert_eq(2, vma_count(mm));            /* heap + anon, unchanged */
    assert_eq(0, user_vm_test_leaf_count());

    /* A free range still maps. */
    uint64_t args2[6] = { BASE + 0x4000, 0x1000, PROT_RW, MAP_SHARED,
                          0, 0x08000000 };
    assert_eq((int64_t)(BASE + 0x4000), user_vm_mmap(mm, args2));
    assert_eq(3, vma_count(mm));
    assert_eq(0x08000000, user_vm_test_leaf_pa(BASE + 0x4000));

    destroy_mm(mm);
    user_vm_test_set_files(NULL);
}

TEST_FUNC(test_mmap_fixed_guard_across_hole_rejects_device_overlap)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    static struct vfs_node mem_node;
    static char mem_name[] = "mem";
    mem_node.name = mem_name;
    file_t f = { .type = FD_DEV, .node = &mem_node };
    user_vm_test_set_files(&f);

    /* anon at [BASE, +0x1000), hole at +0x1000, device at [+0x2000,
     * +0x3000).  A fixed anon replace spanning all three must hit the
     * policy guard (device VMA is not an mmap VMA) instead of silently
     * unmapping the device leaves. */
    assert_eq((int64_t)BASE,
              test_mmap_call(mm, 0, 0x1000, PROT_RW, MAP_ANON_PRIVATE));
    /* Fault the anon leaf in so the assert can prove the rejected
     * fixed replace leaves resident state untouched. */
    assert_eq(0, user_vm_fault(mm, BASE, false));
    uint64_t dargs[6] = { BASE + 0x2000, 0x1000, PROT_RW, MAP_SHARED,
                          0, 0x08000000 };
    assert_eq((int64_t)(BASE + 0x2000), user_vm_mmap(mm, dargs));
    assert_eq(0x08000000, user_vm_test_leaf_pa(BASE + 0x2000));

    uint64_t fargs[6] = { BASE, 0x3000, PROT_RW,
                          MAP_ANON_PRIVATE | MAP_FIXED };
    assert_eq(-EINVAL, user_vm_mmap(mm, fargs));

    /* Both original mappings intact. */
    assert_eq(3, vma_count(mm));
    assert_eq(1, user_vm_test_leaf_state(BASE));
    assert_eq(0x08000000, user_vm_test_leaf_pa(BASE + 0x2000));

    destroy_mm(mm);
    user_vm_test_set_files(NULL);
}

TEST_FUNC(test_mmap_device_read_only_leaf_is_ro)
{
    user_vm_test_reset();
    mm_t *mm = new_mm(HEAP_BASE);

    static struct vfs_node mem_node;
    static char mem_name[] = "mem";
    mem_node.name = mem_name;
    file_t f = { .type = FD_DEV, .node = &mem_node };
    user_vm_test_set_files(&f);

    /* PROT_READ must not silently produce a writable device leaf. */
    uint64_t args[6] = { BASE, 0x1000, PROT_READ, MAP_SHARED,
                         0, 0x08000000 };
    assert_eq((int64_t)BASE, user_vm_mmap(mm, args));
    uint32_t vm = user_vm_test_leaf_vm(BASE);
    assert_true((vm & VM_WRITE) == 0);

    destroy_mm(mm);
    user_vm_test_set_files(NULL);
}

/* ══════════════════════════════════════════════════════════════
 * Shared backing registry (inode MAP_SHARED plan, Task 2)
 * ══════════════════════════════════════════════════════════════ */

static const unsigned char sb_bytes3[8] = { 0xD1, 0xD2, 0xD3 };

TEST_FUNC(test_shared_backing_identity)
{
    user_vm_test_reset();
    struct vfs_mount *m1 = user_vm_test_make_mount("tmpfs");
    struct vfs_mount *m2 = user_vm_test_make_mount("ext2");
    struct vfs_mount *m3 = user_vm_test_make_mount("procfs");

    struct vfs_node *a = user_vm_test_make_mount_node(m1, 42, sb_bytes3, 3);
    struct vfs_node *b = user_vm_test_make_mount_node(m1, 42, sb_bytes3, 3);
    struct vfs_node *c = user_vm_test_make_mount_node(m1, 43, sb_bytes3, 3);
    struct vfs_node *d = user_vm_test_make_mount_node(m2, 42, sb_bytes3, 3);
    struct vfs_node *dir = user_vm_test_make_mount_dir(m1, 50);
    struct vfs_node *other_fs = user_vm_test_make_mount_node(m3, 60, sb_bytes3, 3);
    struct vfs_node *zero_ino = user_vm_test_make_mount_node(m1, 0, sb_bytes3, 3);

    /* Admission: directories, unregistered filesystems and zero inode
     * numbers are not eligible. */
    struct shared_backing *bad = NULL;
    assert_eq(-EISDIR, shared_backing_open_file(dir, &bad));
    assert_eq(-EOPNOTSUPP, shared_backing_open_file(other_fs, &bad));
    assert_eq(-EOPNOTSUPP, shared_backing_open_file(zero_ino, &bad));
    assert_null(bad);

    /* Two independent nodes with the same (mount, ino) rendezvous on
     * ONE object and see the same page PA. */
    struct shared_backing *o1 = NULL, *o2 = NULL;
    assert_eq(0, shared_backing_open_file(a, &o1));
    assert_eq(0, shared_backing_open_file(b, &o2));
    assert_not_null(o1);
    assert_true(o1 == o2);

    uint64_t pa1 = 0, pa2 = 0;
    assert_eq(0, shared_backing_page(o1, 0, &pa1));
    assert_eq(0, shared_backing_page(o2, 0, &pa2));
    assert_true(pa1 != 0);
    assert_true(pa1 == pa2);

    /* Backend content flows in, short read leaves the tail zero. */
    const unsigned char *pg = (const unsigned char *)Phy_To_Virt(pa1);
    assert_eq(0xD1, pg[0]);
    assert_eq(0xD3, pg[2]);
    assert_eq(0x00, pg[3]);

    /* Different ino → different object and page (same index 0). */
    struct shared_backing *o3 = NULL;
    assert_eq(0, shared_backing_open_file(c, &o3));
    uint64_t pa3 = 0;
    assert_eq(0, shared_backing_page(o3, 0, &pa3));
    assert_true(o3 != o1);
    assert_true(pa3 != pa1);

    /* Different mount instance with the same ino → isolated. */
    struct shared_backing *o4 = NULL;
    assert_eq(0, shared_backing_open_file(d, &o4));
    uint64_t pa4 = 0;
    assert_eq(0, shared_backing_page(o4, 0, &pa4));
    assert_true(o4 != o1 && o4 != o3);
    assert_true(pa4 != pa1 && pa4 != pa3);

    /* Different page index inside one object → different page. */
    uint64_t pa1b = 0;
    assert_eq(0, shared_backing_page(o1, 6, &pa1b));
    assert_true(pa1b != pa1);

    /* Anonymous objects are always separate, even at index 0. */
    struct shared_backing *an1 = NULL, *an2 = NULL;
    assert_eq(0, shared_backing_create_anon(&an1));
    assert_eq(0, shared_backing_create_anon(&an2));
    assert_true(an1 != an2);
    uint64_t paA = 0, paB = 0;
    assert_eq(0, shared_backing_page(an1, 0, &paA));
    assert_eq(0, shared_backing_page(an2, 0, &paB));
    assert_true(paA != paB && paA != 0 && paB != 0);
    assert_eq(0x00, ((const unsigned char *)Phy_To_Virt(paA))[0]);

    shared_backing_put(an2);
    shared_backing_put(an1);
    shared_backing_put(o4);
    shared_backing_put(o3);
    shared_backing_put(o2);
    shared_backing_put(o1);

    /* Baselines: pages reclaimed, node refs balanced, allocations
     * balanced, registry lock discipline respected. */
    assert_eq(user_vm_test_page_allocs(), user_vm_test_free_count());
    assert_eq(user_vm_test_kmallocs(), user_vm_test_kfrees());
    assert_eq(user_vm_test_node_gets(a), user_vm_test_node_puts(a));
    assert_eq(user_vm_test_node_gets(b), user_vm_test_node_puts(b));
    assert_eq(user_vm_test_node_gets(c), user_vm_test_node_puts(c));
    assert_eq(user_vm_test_node_gets(d), user_vm_test_node_puts(d));
    assert_eq(user_vm_test_lock_violations(), 0);
    assert_eq(user_vm_test_double_frees(), 0);
}

/* ── Race fixtures: two threads, one key, deterministic winner ── */

static volatile int g_race_in_read;
static volatile int g_race_go_a;
static volatile int g_race_go_b;
static volatile int g_race_timeout;
static _Thread_local int g_race_role;   /* 1 = winner, 2 = loser */
static uint64_t g_race_pa_a, g_race_pa_b;
static int g_race_rc_a, g_race_rc_b;

#define RACE_SPIN_LIMIT 200000000ULL

static int race_wait(volatile int *go)
{
    for (uint64_t i = 0; i < RACE_SPIN_LIMIT; i++) {
        if (__atomic_load_n(go, __ATOMIC_ACQUIRE))
            return 0;
    }
    __atomic_store_n(&g_race_timeout, 1, __ATOMIC_RELAXED);
    return -1;
}

static int race_read(struct vfs_node *node, uint64_t offset,
                     uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    __atomic_add_fetch(&g_race_in_read, 1, __ATOMIC_SEQ_CST);
    volatile int *go = (g_race_role == 1) ? &g_race_go_a : &g_race_go_b;
    if (race_wait(go) != 0)
        return -EIO;
    uint64_t n = (size < 3) ? size : 3;
    memcpy(buffer, sb_bytes3, (size_t)n);
    return (int)n;
}

static void *race_thread_winner(void *arg)
{
    struct shared_backing *obj = arg;
    g_race_role = 1;
    uint64_t pa = 0;
    int rc = shared_backing_page(obj, 0, &pa);
    __atomic_store_n(&g_race_rc_a, rc, __ATOMIC_RELAXED);
    if (rc == 0) {
        g_race_pa_a = pa;
        /* Winner publishes, then modifies the published page before
         * the loser is released. */
        ((unsigned char *)Phy_To_Virt(pa))[3] = 0x5A;
        __atomic_store_n(&g_race_go_b, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void *race_thread_loser(void *arg)
{
    struct shared_backing *obj = arg;
    g_race_role = 2;
    uint64_t pa = 0;
    int rc = shared_backing_page(obj, 0, &pa);
    __atomic_store_n(&g_race_rc_b, rc, __ATOMIC_RELAXED);
    g_race_pa_b = pa;
    return NULL;
}

TEST_FUNC(test_shared_backing_fill_race)
{
    user_vm_test_reset();
    struct vfs_mount *m = user_vm_test_make_mount("tmpfs");
    struct vfs_node *na = user_vm_test_make_mount_node(m, 77, sb_bytes3, 3);
    struct vfs_node *nb = user_vm_test_make_mount_node(m, 77, sb_bytes3, 3);

    struct shared_backing *obj = NULL;
    assert_eq(0, shared_backing_open_file(na, &obj));
    assert_eq(0, shared_backing_open_file(nb, &obj));
    assert_not_null(obj);
    user_vm_test_set_vfs_read(race_read);

    host_pthread_t ta, tb;
    assert_eq(0, pthread_create(&ta, NULL, race_thread_winner, obj));
    assert_eq(0, pthread_create(&tb, NULL, race_thread_loser, obj));

    /* Both candidates must be inside the backend read before either
     * proceeds. */
    for (uint64_t i = 0; i < RACE_SPIN_LIMIT; i++) {
        if (__atomic_load_n(&g_race_in_read, __ATOMIC_SEQ_CST) == 2)
            break;
    }
    assert_eq(2, __atomic_load_n(&g_race_in_read, __ATOMIC_SEQ_CST));
    __atomic_store_n(&g_race_go_a, 1, __ATOMIC_RELEASE);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    user_vm_test_set_vfs_read(NULL);

    assert_eq(0, g_race_timeout);
    assert_eq(0, g_race_rc_a);
    assert_eq(0, g_race_rc_b);
    assert_true(g_race_pa_a != 0);
    assert_true(g_race_pa_a == g_race_pa_b);

    /* Fully initialized page, backend content and the winner's
     * post-publish modification are both visible to the loser. */
    const unsigned char *pg = (const unsigned char *)Phy_To_Virt(g_race_pa_a);
    assert_eq(0xD1, pg[0]);
    assert_eq(0xD2, pg[1]);
    assert_eq(0xD3, pg[2]);
    assert_eq(0x5A, pg[3]);
    assert_eq(0x00, pg[4]);

    /* The loser's candidate page was released; the winner's stays with
     * the object. */
    assert_eq(1, user_vm_test_free_count());
    assert_true(user_vm_test_free_pa_at(0) != g_race_pa_a);

    shared_backing_put(obj);
    shared_backing_put(obj);

    /* Final baseline: every page freed, node refs balanced. */
    assert_eq(user_vm_test_page_allocs(), user_vm_test_free_count());
    assert_eq(user_vm_test_kmallocs(), user_vm_test_kfrees());
    assert_eq(user_vm_test_node_gets(na), user_vm_test_node_puts(na));
    assert_eq(user_vm_test_node_gets(nb), user_vm_test_node_puts(nb));
    assert_eq(user_vm_test_lock_violations(), 0);
    assert_eq(user_vm_test_double_frees(), 0);
}

/* ── Failure/retry fixtures ──────────────────────────────────── */

static struct shared_backing *g_retry_obj;
static uint64_t g_retry_inner_pa;
static int g_retry_depth;

static int retry_read_publish_then_fail(struct vfs_node *node,
                                        uint64_t offset, uint64_t size,
                                        void *buffer)
{
    (void)node; (void)offset; (void)size;
    if (g_retry_depth == 0) {
        g_retry_depth++;
        int rc = shared_backing_page(g_retry_obj, 0, &g_retry_inner_pa);
        g_retry_depth--;
        if (rc == 0) {
            /* The concurrent winner published while this backend read
             * is in flight — the outer read now fails. */
            return -EIO;
        }
    }
    memcpy(buffer, sb_bytes3, 3);
    return 3;
}

TEST_FUNC(test_shared_backing_fill_retry)
{
    user_vm_test_reset();
    struct vfs_mount *m = user_vm_test_make_mount("tmpfs");

    /* Object metadata allocation failure → -ENOMEM, node reference
     * returned, nothing published. */
    struct vfs_node *n1 = user_vm_test_make_mount_node(m, 11, sb_bytes3, 3);
    user_vm_test_fail_node_after(0);
    struct shared_backing *o1 = (struct shared_backing *)(uintptr_t)0x1;
    assert_eq(-ENOMEM, shared_backing_open_file(n1, &o1));
    assert_true(o1 == (struct shared_backing *)(uintptr_t)0x1);
    assert_eq(user_vm_test_node_gets(n1), user_vm_test_node_puts(n1));
    user_vm_test_fail_node_after(-1);

    /* PA allocation failure → -ENOMEM; retry after disarming
     * succeeds and fills from the backend. */
    struct vfs_node *n2 = user_vm_test_make_mount_node(m, 12, sb_bytes3, 3);
    struct shared_backing *o2 = NULL;
    assert_eq(0, shared_backing_open_file(n2, &o2));
    user_vm_test_fail_page_after(0);
    uint64_t pa = 0x1234;
    assert_eq(-ENOMEM, shared_backing_page(o2, 0, &pa));
    assert_eq((uint64_t)0x1234, pa);
    user_vm_test_fail_page_after(-1);
    pa = 0;
    assert_eq(0, shared_backing_page(o2, 0, &pa));
    assert_eq(0xD1, ((const unsigned char *)Phy_To_Virt(pa))[0]);
    shared_backing_put(o2);

    /* Page-node allocation failure (PA already taken) → -ENOMEM, the
     * PA is released, retry succeeds. */
    struct vfs_node *n3 = user_vm_test_make_mount_node(m, 13, sb_bytes3, 3);
    struct shared_backing *o3 = NULL;
    assert_eq(0, shared_backing_open_file(n3, &o3));
    user_vm_test_fail_node_after(0);
    pa = 0x5678;
    assert_eq(-ENOMEM, shared_backing_page(o3, 0, &pa));
    assert_eq((uint64_t)0x5678, pa);
    user_vm_test_fail_node_after(-1);
    pa = 0;
    assert_eq(0, shared_backing_page(o3, 0, &pa));
    shared_backing_put(o3);

    /* Backend -ENOMEM is preserved, other backend errors map to
     * -EFAULT, and both recover once the backend succeeds again. */
    struct vfs_node *n4 = user_vm_test_make_mount_node(m, 14, sb_bytes3, 3);
    struct shared_backing *o4 = NULL;
    assert_eq(0, shared_backing_open_file(n4, &o4));
    user_vm_test_set_node_read_err(n4, -ENOMEM);
    pa = 0;
    assert_eq(-ENOMEM, shared_backing_page(o4, 0, &pa));
    assert_eq(0, pa);
    user_vm_test_set_node_read_err(n4, -EIO);
    assert_eq(-EFAULT, shared_backing_page(o4, 0, &pa));
    user_vm_test_set_node_read_err(n4, 0);
    assert_eq(0, shared_backing_page(o4, 0, &pa));
    assert_eq(0xD2, ((const unsigned char *)Phy_To_Virt(pa))[1]);
    shared_backing_put(o4);

    /* Backend failure with a page published concurrently: the second
     * lookup takes the already-published page and reports success. */
    struct vfs_node *n6 = user_vm_test_make_mount_node(m, 16, sb_bytes3, 3);
    struct shared_backing *o6 = NULL;
    assert_eq(0, shared_backing_open_file(n6, &o6));
    g_retry_obj = o6;
    g_retry_inner_pa = 0;
    g_retry_depth = 0;
    user_vm_test_set_vfs_read(retry_read_publish_then_fail);
    pa = 0;
    assert_eq(0, shared_backing_page(o6, 0, &pa));
    user_vm_test_set_vfs_read(NULL);
    assert_true(g_retry_inner_pa != 0);
    assert_true(pa == g_retry_inner_pa);
    assert_eq(0xD3, ((const unsigned char *)Phy_To_Virt(pa))[2]);
    shared_backing_put(o6);

    /* Tail zero-fill: a 3-byte backend leaves [3, 4096) zero; an index
     * past EOF fills the whole page with zeros. */
    struct vfs_node *n7 = user_vm_test_make_mount_node(m, 17, sb_bytes3, 3);
    struct shared_backing *o7 = NULL;
    assert_eq(0, shared_backing_open_file(n7, &o7));
    pa = 0;
    assert_eq(0, shared_backing_page(o7, 0, &pa));
    const unsigned char *pg = (const unsigned char *)Phy_To_Virt(pa);
    assert_eq(0xD1, pg[0]);
    assert_eq(0xD3, pg[2]);
    assert_eq(0x00, pg[3]);
    assert_eq(0x00, pg[4095]);
    uint64_t pa_eof = 0;
    assert_eq(0, shared_backing_page(o7, 4, &pa_eof));
    assert_true(pa_eof != pa);
    for (int i = 0; i < 4096; i += 512)
        assert_eq(0x00, ((const unsigned char *)Phy_To_Virt(pa_eof))[i]);
    shared_backing_put(o7);

    /* Global baselines across every phase: the alloc counters count
     * calls, so subtract the injected failures before comparing to
     * the reclaim records. */
    assert_eq(user_vm_test_page_allocs() - user_vm_test_page_alloc_fails(),
              user_vm_test_free_count());
    assert_eq(user_vm_test_kmallocs() - user_vm_test_node_alloc_fails(),
              user_vm_test_kfrees());
    assert_eq(user_vm_test_node_gets(n1), user_vm_test_node_puts(n1));
    assert_eq(user_vm_test_node_gets(n2), user_vm_test_node_puts(n2));
    assert_eq(user_vm_test_node_gets(n3), user_vm_test_node_puts(n3));
    assert_eq(user_vm_test_node_gets(n4), user_vm_test_node_puts(n4));
    assert_eq(user_vm_test_node_gets(n6), user_vm_test_node_puts(n6));
    assert_eq(user_vm_test_node_gets(n7), user_vm_test_node_puts(n7));
    assert_eq(user_vm_test_lock_violations(), 0);
    assert_eq(user_vm_test_double_frees(), 0);
}

/* ══════════════════════════════════════════════════════════════
 * aarch64 VM paths over the shared backing (Task 3)
 * ══════════════════════════════════════════════════════════════ */

static file_t sb_fd(struct vfs_node *node, int flags)
{
    file_t f;
    memset(&f, 0, sizeof(f));
    f.type = FD_VFS;
    f.flags = flags;
    f.node = node;
    return f;
}

/* mmap args with an explicit fd and offset. */
static int64_t test_mmap_file_call(mm_t *mm, uint64_t addr, uint64_t len,
                                   uint64_t prot, uint64_t flags,
                                   uint64_t fd, uint64_t off)
{
    uint64_t args[6] = { addr, len, prot, flags, fd, off };
    return user_vm_mmap(mm, args);
}

TEST_FUNC(test_inode_shared_fault_and_uaccess)
{
    user_vm_test_reset();
    struct vfs_mount *m = user_vm_test_make_mount("tmpfs");
    static const unsigned char backend[24576] = {
        [0] = 0xB0, [4096] = 0xB1, [8192] = 0xB2,
    };
    struct vfs_node *na = user_vm_test_make_mount_node(m, 91, backend,
                                                       sizeof(backend));
    struct vfs_node *nb = user_vm_test_make_mount_node(m, 91, backend,
                                                       sizeof(backend));
    struct vfs_node *nc = user_vm_test_make_mount_node(m, 91, backend,
                                                       sizeof(backend));
    struct vfs_node *nd = user_vm_test_make_mount_node(m, 91, backend,
                                                       sizeof(backend));

    file_t fa = sb_fd(na, O_RDWR);
    file_t fb = sb_fd(nb, O_RDWR);
    file_t fc = sb_fd(nc, O_RDWR);
    file_t fpriv = sb_fd(nd, O_RDWR);

    uint64_t B1 = BASE;
    uint64_t B2 = BASE + 0x4000;
    uint64_t B3 = BASE + 0x8000;
    uint64_t B4 = BASE + 0xc000;
    uint64_t B5 = BASE + 0x10000;

    mm_t *mm1 = new_mm(HEAP_BASE);
    user_vm_test_set_files(&fa);
    assert_eq((int64_t)B1, test_mmap_file_call(mm1, B1, 3 * PAGE_4K_SIZE,
                                               PROT_RW, MAP_SHARED, 0, 0));
    mm_t *mm2 = new_mm(HEAP_BASE);
    user_vm_test_set_files(&fb);
    assert_eq((int64_t)B2, test_mmap_file_call(mm2, B2, 3 * PAGE_4K_SIZE,
                                               PROT_RW, MAP_SHARED, 0, 0));

    /* CPU fault through mm1 fills from the backend as an object page. */
    assert_eq(0, user_vm_fault(mm1, B1, false));
    user_vm_page_t *r1 = resident_at(mm1, B1);
    assert_not_null(r1);
    assert_eq((uint32_t)USER_VM_PAGE_BACKING, r1->source);
    assert_eq((uint32_t)USER_VM_PAGE_PRIVATE, r1->share);
    assert_eq(0xB0, *(const unsigned char *)Phy_To_Virt(r1->pa));

    /* uaccess (copy pass) through mm2 at the same inode/index lands on
     * the SAME PA with a zero PMM COW count. */
    assert_eq(0, user_vm_prepare_copy(mm2, B2, 4, false));
    user_vm_page_t *r2 = resident_at(mm2, B2);
    assert_not_null(r2);
    assert_eq(r1->pa, r2->pa);
    assert_eq(0, user_vm_test_cow_refs(r1->pa));

    /* Private-file uaccess: the FIRST read of a private mapping's page
     * comes from the backend, never a zero page. */
    mm_t *mm3 = new_mm(HEAP_BASE);
    user_vm_test_set_files(&fpriv);
    assert_eq((int64_t)B3, test_mmap_file_call(mm3, B3, PAGE_4K_SIZE,
                                               PROT_RW, MAP_PRIVATE, 0, 0));
    assert_eq(0, user_vm_prepare_copy(mm3, B3, 4, false));
    user_vm_page_t *r3 = resident_at(mm3, B3);
    assert_not_null(r3);
    assert_eq((uint32_t)USER_VM_PAGE_ANON, r3->source);
    assert_eq(0xB0, *(const unsigned char *)Phy_To_Virt(r3->pa));
    destroy_mm(mm3);

    /* Cross-two-page copy: the second page's backend read fails — the
     * first staged leaf is withdrawn, the other mappings keep their
     * PA and data, and the retry reuses the published cache.  The
     * fill reads through the OBJECT's node (the first opener's), so
     * the injection targets na. */
    user_vm_test_set_node_read_err_from(na, -EIO, 2 * PAGE_4K_SIZE);
    assert_eq(-EFAULT, user_vm_prepare_copy(mm2, B2 + PAGE_4K_SIZE,
                                            2 * PAGE_4K_SIZE, false));
    assert_eq(0, user_vm_test_leaf_state_in(mm2->pgdir,
                                            B2 + PAGE_4K_SIZE));
    assert_eq(0, user_vm_test_leaf_state_in(mm2->pgdir,
                                            B2 + 2 * PAGE_4K_SIZE));
    assert_eq(r1->pa, user_vm_test_leaf_pa_in(mm2->pgdir, B2));
    assert_eq(0xB0, *(const unsigned char *)Phy_To_Virt(r1->pa));
    assert_eq(0xB0, *(const unsigned char *)Phy_To_Virt(
                        user_vm_test_leaf_pa_in(mm1->pgdir, B1)));
    user_vm_test_set_node_read_err(na, 0);

    assert_eq(0, user_vm_prepare_copy(mm2, B2, 3 * PAGE_4K_SIZE, false));
    user_vm_page_t *r2p1 = resident_at(mm2, B2 + PAGE_4K_SIZE);
    user_vm_page_t *r2p2 = resident_at(mm2, B2 + 2 * PAGE_4K_SIZE);
    assert_not_null(r2p1);
    assert_not_null(r2p2);
    assert_eq((uint32_t)USER_VM_PAGE_BACKING, r2p1->source);
    assert_eq(0xB1, *(const unsigned char *)Phy_To_Virt(r2p1->pa));
    assert_eq(0xB2, *(const unsigned char *)Phy_To_Virt(r2p2->pa));

    /* The retry's first page reused the object cache: mm1 faults the
     * same index and sees the same PA. */
    assert_eq(0, user_vm_fault(mm1, B1 + PAGE_4K_SIZE, false));
    assert_eq(r2p1->pa,
              user_vm_test_leaf_pa_in(mm1->pgdir, B1 + PAGE_4K_SIZE));

    /* Leaf-install failure variant: the first staged page is withdrawn
     * while its object page keeps serving other mappings; the retry
     * reuses both cached pages. */
    mm_t *mm4 = new_mm(HEAP_BASE);
    user_vm_test_set_files(&fc);
    assert_eq((int64_t)B4, test_mmap_file_call(mm4, B4, 2 * PAGE_4K_SIZE,
                                               PROT_RW, MAP_SHARED, 0, 0));
    mm_t *mm5 = new_mm(HEAP_BASE);
    user_vm_test_set_files(&fc);
    assert_eq((int64_t)B5, test_mmap_file_call(mm5, B5, 2 * PAGE_4K_SIZE,
                                               PROT_RW, MAP_SHARED, 0, 0));

    user_vm_test_fail_map_after(1);
    assert_eq(-ENOMEM, user_vm_prepare_copy(mm4, B4, 2 * PAGE_4K_SIZE,
                                            false));
    assert_eq(0, user_vm_test_leaf_state_in(mm4->pgdir, B4));
    assert_eq(0, user_vm_test_leaf_state_in(mm4->pgdir,
                                            B4 + PAGE_4K_SIZE));
    user_vm_test_fail_map_after(-1);

    /* mm5 proves the object page published by the failed attempt is
     * alive and shared; mm4's retry reuses it. */
    assert_eq(0, user_vm_fault(mm5, B5 + PAGE_4K_SIZE, false));
    assert_eq(0, user_vm_prepare_copy(mm4, B4, 2 * PAGE_4K_SIZE, false));
    user_vm_page_t *r4 = resident_at(mm4, B4);
    user_vm_page_t *r4p1 = resident_at(mm4, B4 + PAGE_4K_SIZE);
    assert_not_null(r4);
    assert_not_null(r4p1);
    assert_eq(0xB0, *(const unsigned char *)Phy_To_Virt(r4->pa));
    assert_eq(0xB1, *(const unsigned char *)Phy_To_Virt(r4p1->pa));
    assert_eq(r4p1->pa,
              user_vm_test_leaf_pa_in(mm5->pgdir, B5 + PAGE_4K_SIZE));

    destroy_mm(mm1);
    destroy_mm(mm2);
    destroy_mm(mm4);
    destroy_mm(mm5);
    user_vm_test_set_files(NULL);

    assert_eq(user_vm_test_page_allocs() - user_vm_test_page_alloc_fails(),
              user_vm_test_free_count());
    assert_eq(user_vm_test_node_gets(na), user_vm_test_node_puts(na));
    assert_eq(user_vm_test_node_gets(nb), user_vm_test_node_puts(nb));
    assert_eq(user_vm_test_node_gets(nc), user_vm_test_node_puts(nc));
    assert_eq(user_vm_test_node_gets(nd), user_vm_test_node_puts(nd));
    assert_eq(user_vm_test_fd_gets(), user_vm_test_fd_puts());
    assert_eq(user_vm_test_lock_violations(), 0);
    assert_eq(user_vm_test_double_frees(), 0);
}

TEST_FUNC(test_shared_anon_fork_backing)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(parent, 0, 3 * PAGE_4K_SIZE, PROT_RW,
                             MAP_SHARED | MAP_ANONYMOUS));

    /* Page 0: RW leaf with content.  Page 1: RW with content, then
     * stashed (PROT_NONE).  Page 2: never touched. */
    assert_eq(0, user_vm_fault(parent, BASE, true));
    user_vm_page_t *p0 = resident_at(parent, BASE);
    assert_not_null(p0);
    write_page(p0->pa, 0xA1);
    assert_eq(0, user_vm_fault(parent, BASE + PAGE_4K_SIZE, true));
    uint64_t pa1 = user_vm_test_leaf_pa_in(parent->pgdir,
                                           BASE + PAGE_4K_SIZE);
    write_page(pa1, 0xA2);
    assert_eq(0, user_vm_mprotect(parent, BASE + PAGE_4K_SIZE,
                                  PAGE_4K_SIZE, PROT_NONE));

    mm_t *child = new_child_mm(0x9000);
    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(parent, child, clone_publish_ok,
                                    &probe));

    /* The parent's RW leaf is NOT downgraded; the child inherits the
     * same PA at its logical permission; the stash is reproduced. */
    assert_eq(p0->pa, user_vm_test_leaf_pa_in(parent->pgdir, BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(parent->pgdir, BASE));
    assert_eq(p0->pa, user_vm_test_leaf_pa_in(child->pgdir, BASE));
    assert_eq((uint32_t)(VM_USER_RW | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(child->pgdir, BASE));
    assert_eq(0, user_vm_test_cow_refs(p0->pa));
    assert_eq(2, user_vm_test_leaf_state_in(child->pgdir,
                                            BASE + PAGE_4K_SIZE));
    assert_eq(0, user_vm_test_leaf_state_in(child->pgdir,
                                            BASE + 2 * PAGE_4K_SIZE));

    /* Page 2: first touch on both sides rendezvous on one PA. */
    assert_eq(0, user_vm_fault(child, BASE + 2 * PAGE_4K_SIZE, true));
    uint64_t pa2 = user_vm_test_leaf_pa_in(child->pgdir,
                                           BASE + 2 * PAGE_4K_SIZE);
    assert_eq(0, user_vm_fault(parent, BASE + 2 * PAGE_4K_SIZE, true));
    assert_eq(pa2,
              user_vm_test_leaf_pa_in(parent->pgdir,
                                      BASE + 2 * PAGE_4K_SIZE));

    /* Restoring the stash reveals the shared content. */
    assert_eq(0, user_vm_mprotect(child, BASE + PAGE_4K_SIZE,
                                  PAGE_4K_SIZE, PROT_RW));
    assert_eq(pa1,
              user_vm_test_leaf_pa_in(child->pgdir, BASE + PAGE_4K_SIZE));
    assert_eq(0xA2,
              *(const unsigned char *)Phy_To_Virt(
                  user_vm_test_leaf_pa_in(child->pgdir,
                                          BASE + PAGE_4K_SIZE)));
    assert_eq(0, user_vm_test_cow_refs(p0->pa));

    destroy_mm(parent);
    destroy_mm(child);
}

TEST_FUNC(test_shared_anon_fork_backing_publish_fail)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    assert_eq((int64_t)BASE,
              test_mmap_call(parent, 0, 3 * PAGE_4K_SIZE, PROT_RW,
                             MAP_SHARED | MAP_ANONYMOUS));
    assert_eq(0, user_vm_fault(parent, BASE, true));
    user_vm_page_t *p0 = resident_at(parent, BASE);
    assert_not_null(p0);
    write_page(p0->pa, 0xB2);
    assert_eq(0, user_vm_fault(parent, BASE + PAGE_4K_SIZE, true));
    assert_eq(0, user_vm_mprotect(parent, BASE + PAGE_4K_SIZE,
                                  PAGE_4K_SIZE, PROT_NONE));

    uint64_t pa0 = p0->pa;
    uint32_t vm0 = user_vm_test_leaf_vm_in(parent->pgdir, BASE);
    uint32_t vm1 = user_vm_test_leaf_vm_in(parent->pgdir,
                                           BASE + PAGE_4K_SIZE);
    int frees0 = user_vm_test_free_count();
    int allocs0 = user_vm_test_page_allocs();

    mm_t *child = new_child_mm(0x9000);
    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(-ENOMEM, user_vm_clone_fork(parent, child,
                                          clone_publish_fail, &probe));
    assert_eq(1, probe.calls);

    /* No child leaf, no child VMA; the parent's permissions, PAs and
     * the PMM baseline are exactly as before. */
    assert_eq(0, user_vm_test_leaf_count_in(child->pgdir));
    assert_eq(0, vma_count(child));
    assert_eq(pa0, user_vm_test_leaf_pa_in(parent->pgdir, BASE));
    assert_eq(vm0, user_vm_test_leaf_vm_in(parent->pgdir, BASE));
    assert_eq(vm1, user_vm_test_leaf_vm_in(parent->pgdir,
                                           BASE + PAGE_4K_SIZE));
    assert_eq(0, user_vm_test_cow_refs(pa0));
    assert_eq(allocs0, user_vm_test_page_allocs());
    assert_eq(frees0, user_vm_test_free_count());

    destroy_mm(parent);
    destroy_mm(child);
}

TEST_FUNC(test_shared_vma_retire)
{
    user_vm_test_reset();
    struct vfs_mount *m = user_vm_test_make_mount("tmpfs");
    static const unsigned char backend[16384] = { [0] = 0xC0 };
    struct vfs_node *na = user_vm_test_make_mount_node(m, 95, backend,
                                                       sizeof(backend));
    struct vfs_node *nb = user_vm_test_make_mount_node(m, 95, backend,
                                                       sizeof(backend));

    uint64_t B = BASE;
    file_t fa = sb_fd(na, O_RDWR);
    user_vm_test_set_files(&fa);
    mm_t *mm1 = new_mm(HEAP_BASE);
    assert_eq((int64_t)B, test_mmap_file_call(mm1, B, 4 * PAGE_4K_SIZE,
                                              PROT_RW, MAP_SHARED, 0,
                                              0x2000));
    assert_eq(2, vma_count(mm1));   /* heap + mapping */

    assert_eq(0, user_vm_fault(mm1, B, true));
    write_page(user_vm_test_leaf_pa_in(mm1->pgdir, B), 0xD0);
    assert_eq(0, user_vm_fault(mm1, B + PAGE_4K_SIZE, true));
    write_page(user_vm_test_leaf_pa_in(mm1->pgdir, B + PAGE_4K_SIZE),
               0xD1);
    assert_eq(0, user_vm_fault(mm1, B + 2 * PAGE_4K_SIZE, true));
    uint64_t pa1 = user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + PAGE_4K_SIZE);
    uint64_t pa2 = user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + 2 * PAGE_4K_SIZE);

    /* Partial munmap: the right fragment advances the page offset. */
    assert_eq(0, user_vm_munmap(mm1, B + PAGE_4K_SIZE, 2 * PAGE_4K_SIZE));
    assert_eq(0, user_vm_test_leaf_state_in(mm1->pgdir,
                                            B + PAGE_4K_SIZE));
    assert_eq(0, user_vm_test_leaf_state_in(mm1->pgdir,
                                            B + 2 * PAGE_4K_SIZE));
    vma_t *right = NULL;
    for (list_t *p = mm1->vma_list.next; p != &mm1->vma_list;
         p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_start == B + 3 * PAGE_4K_SIZE)
            right = v;
    }
    assert_not_null(right);
    assert_eq(B + 4 * PAGE_4K_SIZE, right->vm_end);
    assert_eq(UINT64_C(5), right->vm_pgoff);   /* 2 + 3 pages */
    assert_eq((uint32_t)VMA_SHARED, right->vm_flags & VMA_SHARED);
    assert_not_null(right->vm_shared_backing);

    /* A same-object MAP_FIXED remap re-publishes the cached pages. */
    file_t fb = sb_fd(nb, O_RDWR);
    user_vm_test_set_files(&fb);
    assert_eq((int64_t)(B + PAGE_4K_SIZE),
              test_mmap_file_call(mm1, B + PAGE_4K_SIZE, 2 * PAGE_4K_SIZE,
                                  PROT_RW, MAP_SHARED | MAP_FIXED, 0,
                                  0x3000));
    assert_eq(0, user_vm_fault(mm1, B + PAGE_4K_SIZE, true));
    assert_eq(pa1, user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + PAGE_4K_SIZE));
    assert_eq(0xD1, *(const unsigned char *)Phy_To_Virt(pa1));
    assert_eq(0, user_vm_fault(mm1, B + 2 * PAGE_4K_SIZE, true));
    assert_eq(pa2, user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + 2 * PAGE_4K_SIZE));
    assert_eq(0x00, *(const unsigned char *)Phy_To_Virt(pa2));

    /* Offset-window failures keep every leaf and datum intact. */
    int64_t neg_rc = test_mmap_file_call(mm1, B + PAGE_4K_SIZE,
                                         2 * PAGE_4K_SIZE, PROT_RW,
                                         MAP_SHARED | MAP_FIXED, 0,
                                         (uint64_t)(int64_t)-1);
    int64_t una_rc = test_mmap_file_call(mm1, B + PAGE_4K_SIZE,
                                         2 * PAGE_4K_SIZE, PROT_RW,
                                         MAP_SHARED | MAP_FIXED, 0,
                                         0x1001);
    int64_t oob_rc = test_mmap_file_call(mm1, B + PAGE_4K_SIZE,
                                         2 * PAGE_4K_SIZE, PROT_RW,
                                         MAP_SHARED | MAP_FIXED, 0,
                                         0x7ffffffffffff000ULL);
    assert_eq(-EINVAL, neg_rc);
    assert_eq(-EINVAL, una_rc);
    assert_eq(-EINVAL, oob_rc);
    if (neg_rc != -EINVAL || una_rc != -EINVAL || oob_rc != -EINVAL) {
        /* Legacy divergence: invalid offsets were accepted, so the mm
         * is not in the asserted shape.  RED stops here. */
        return;
    }
    assert_eq(4, vma_count(mm1));   /* heap + left + remap + right */
    assert_eq(pa1, user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + PAGE_4K_SIZE));
    assert_eq(0xD1, *(const unsigned char *)Phy_To_Virt(pa1));

    /* Split-preparation failure: the new VMA is allocated, the split
     * node is not — the fixed replace aborts with the old VA, PA and
     * data untouched. */
    user_vm_test_fail_node_after(1);
    assert_eq(-ENOMEM,
              test_mmap_file_call(mm1, B + PAGE_4K_SIZE, PAGE_4K_SIZE,
                                  PROT_RW, MAP_SHARED | MAP_FIXED, 0,
                                  0x3000));
    user_vm_test_fail_node_after(-1);
    assert_eq(4, vma_count(mm1));
    assert_eq(pa1, user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + PAGE_4K_SIZE));
    assert_eq(0xD1, *(const unsigned char *)Phy_To_Virt(pa1));

    /* Successful replace: left fragment keeps the cache, the right
     * fragment advances its page offset. */
    assert_eq((int64_t)(B + PAGE_4K_SIZE),
              test_mmap_file_call(mm1, B + PAGE_4K_SIZE, PAGE_4K_SIZE,
                                  PROT_RW, MAP_SHARED | MAP_FIXED, 0,
                                  0x3000));
    assert_eq(5, vma_count(mm1));
    assert_eq(0, user_vm_test_leaf_state_in(mm1->pgdir,
                                            B + PAGE_4K_SIZE));
    assert_eq(0, user_vm_fault(mm1, B + PAGE_4K_SIZE, true));
    assert_eq(pa1, user_vm_test_leaf_pa_in(mm1->pgdir,
                                           B + PAGE_4K_SIZE));
    assert_eq(0xD1, *(const unsigned char *)Phy_To_Virt(pa1));

    /* A second independent mapping holds the object across mm1's
     * teardown. */
    mm_t *mm2 = new_mm(HEAP_BASE);
    assert_eq((int64_t)(BASE + 0x8000),
              test_mmap_file_call(mm2, BASE + 0x8000, PAGE_4K_SIZE,
                                  PROT_RW, MAP_SHARED, 0, 0x3000));
    assert_eq(0, user_vm_fault(mm2, BASE + 0x8000, true));
    assert_eq(pa1, user_vm_test_leaf_pa_in(mm2->pgdir, BASE + 0x8000));

    /* Teardown withdraws present and stashed leaves first, then the
     * last reference releases the object pages. */
    assert_eq(0, user_vm_mprotect(mm1, B, PAGE_4K_SIZE,
                                  PROT_NONE));
    assert_eq(2, user_vm_test_leaf_state_in(mm1->pgdir, B));
    destroy_mm(mm1);
    assert_eq(0, user_vm_test_leaf_state_in(mm1->pgdir, B));
    assert_eq(0, user_vm_test_leaf_state_in(mm1->pgdir,
                                            B + PAGE_4K_SIZE));
    destroy_mm(mm2);
    assert_eq(0, user_vm_test_leaf_state_in(mm2->pgdir,
                                            BASE + 0x8000));

    user_vm_test_set_files(NULL);
    assert_eq(user_vm_test_page_allocs() - user_vm_test_page_alloc_fails(),
              user_vm_test_free_count());
    assert_eq(user_vm_test_kmallocs() - user_vm_test_node_alloc_fails(),
              user_vm_test_kfrees());
    assert_eq(user_vm_test_node_gets(na), user_vm_test_node_puts(na));
    assert_eq(user_vm_test_node_gets(nb), user_vm_test_node_puts(nb));
    assert_eq(user_vm_test_fd_gets(), user_vm_test_fd_puts());
    assert_eq(user_vm_test_lock_violations(), 0);
    assert_eq(user_vm_test_double_frees(), 0);
}

TEST_FUNC(test_shared_file_protect_ceiling)
{
    user_vm_test_reset();
    struct vfs_mount *m = user_vm_test_make_mount("tmpfs");
    static const unsigned char backend[16384] = { [0] = 0xE0 };
    struct vfs_node *na = user_vm_test_make_mount_node(m, 96, backend,
                                                       sizeof(backend));

    uint64_t B = BASE;
    file_t ro = sb_fd(na, O_RDONLY);
    file_t wr = sb_fd(na, O_RDWR);

    /* O_RDONLY fd: readable mapping ok, writable mapping refused. */
    mm_t *mm = new_mm(HEAP_BASE);
    user_vm_test_set_files(&ro);
    assert_eq((int64_t)B, test_mmap_file_call(mm, B, 2 * PAGE_4K_SIZE,
                                              PROT_READ, MAP_SHARED, 0,
                                              0));
    mm_t *mmbad = new_mm(HEAP_BASE);
    int64_t ro_rw = test_mmap_file_call(mmbad, BASE + 0x8000,
                                        2 * PAGE_4K_SIZE, PROT_RW,
                                        MAP_SHARED, 0, 0);
    assert_eq(-EACCES, ro_rw);

    /* O_WRONLY fd is not readable. */
    file_t wo = sb_fd(na, O_WRONLY);
    user_vm_test_set_files(&wo);
    mm_t *mmwo = new_mm(HEAP_BASE);
    int64_t wo_r = test_mmap_file_call(mmwo, BASE + 0x8000, PAGE_4K_SIZE,
                                       PROT_READ, MAP_SHARED, 0, 0);
    assert_eq(-EACCES, wo_r);
    if (ro_rw != -EACCES || wo_r != -EACCES) {
        /* Legacy divergence: the access-mode ceiling was not enforced,
         * so the mm layout below is not the asserted shape.  RED stops
         * here. */
        return;
    }
    destroy_mm(mmbad);
    destroy_mm(mmwo);

    /* Adjacent segments: one RW ceiling, one RO ceiling, same object. */
    user_vm_test_set_files(&wr);
    assert_eq((int64_t)(B + 2 * PAGE_4K_SIZE),
              test_mmap_file_call(mm, B + 2 * PAGE_4K_SIZE,
                                  2 * PAGE_4K_SIZE, PROT_RW, MAP_SHARED,
                                  0, 0x2000));
    assert_eq(0, user_vm_fault(mm, B, false));
    write_page(user_vm_test_leaf_pa_in(mm->pgdir, B), 0xE1);
    assert_eq(0, user_vm_fault(mm, B + 2 * PAGE_4K_SIZE, true));

    /* Cross-segment write expansion fails atomically: neither segment
     * is upgraded or split. */
    assert_eq(-EACCES,
              user_vm_mprotect(mm, B, 3 * PAGE_4K_SIZE, PROT_RW));
    assert_eq(3, vma_count(mm));
    for (list_t *p = mm->vma_list.next; p != &mm->vma_list;
         p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_start == B)
            assert_true((v->vm_flags & VMA_PROT_WRITE) == 0);
    }
    assert_eq((uint32_t)(VM_USER_RO | VM_NO_EXEC),
              user_vm_test_leaf_vm_in(mm->pgdir, B));

    /* Split the read-only segment, fork, and re-expand every
     * fragment in the child. */
    assert_eq(0, user_vm_mprotect(mm, B, PAGE_4K_SIZE, PROT_NONE));
    assert_eq(0, user_vm_mprotect(mm, B, PAGE_4K_SIZE, PROT_READ));
    assert_eq(4, vma_count(mm));

    mm_t *child = new_child_mm(0x9000);
    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(mm, child, clone_publish_ok,
                                    &probe));
    assert_eq(4, vma_count(child));
    assert_eq(-EACCES,
              user_vm_mprotect(child, B, PAGE_4K_SIZE, PROT_RW));
    assert_eq(-EACCES,
              user_vm_mprotect(child, B + PAGE_4K_SIZE, PAGE_4K_SIZE,
                               PROT_RW));

    /* Positive ceiling: the O_RDWR segment may be downgraded, split,
     * forked, and re-expanded after the fd is long gone. */
    assert_eq(0, user_vm_mprotect(mm, B + 2 * PAGE_4K_SIZE, PAGE_4K_SIZE,
                                  PROT_READ));
    assert_eq(0, user_vm_mprotect(mm, B + 3 * PAGE_4K_SIZE, PAGE_4K_SIZE,
                                  PROT_NONE));
    assert_eq(0, user_vm_mprotect(mm, B + 3 * PAGE_4K_SIZE, PAGE_4K_SIZE,
                                  PROT_READ));
    assert_eq(5, vma_count(mm));

    mm_t *child2 = new_child_mm(0xa000);
    struct clone_publish_probe probe2 = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(mm, child2, clone_publish_ok,
                                    &probe2));
    user_vm_test_set_files(NULL);   /* the fd is closed */
    assert_eq(0,
              user_vm_mprotect(child2, B + 2 * PAGE_4K_SIZE,
                               PAGE_4K_SIZE, PROT_RW));
    assert_eq(0,
              user_vm_mprotect(child2, B + 3 * PAGE_4K_SIZE,
                               PAGE_4K_SIZE, PROT_RW));
    assert_eq(0,
              user_vm_mprotect(mm, B + 2 * PAGE_4K_SIZE, PAGE_4K_SIZE,
                               PROT_RW));

    destroy_mm(mm);
    destroy_mm(child);
    destroy_mm(child2);

    assert_eq(user_vm_test_page_allocs() - user_vm_test_page_alloc_fails(),
              user_vm_test_free_count());
    assert_eq(user_vm_test_node_gets(na), user_vm_test_node_puts(na));
    assert_eq(user_vm_test_fd_gets(), user_vm_test_fd_puts());
    assert_eq(user_vm_test_lock_violations(), 0);
    assert_eq(user_vm_test_double_frees(), 0);
}

/* Pure read-only range check (used by aarch64 syscall_check_user_range):
 * accepts eligible COW-downgraded stack/anon leaves and authorised absent
 * VMA pages without allocating or mutating leaves/refs, and rejects
 * logical-RO, PROT_NONE stashed, holes, and VMA_IO. */
TEST_FUNC(test_range_accessible_cow_and_absent)
{
    user_vm_test_reset();
    mm_t *parent = new_mm(HEAP_BASE);
    mm_t *child  = new_child_mm(0x9000);

    /* Stack page (RW/NX) and RO ELF page, plus an untouched RW anon VMA
     * and a PROT_NONE stashed page. */
    uint64_t stack_va = AARCH64_USER_STACK_BASE;
    uint64_t stack_pa = alloc_4k_page();
    assert_true(stack_pa != 0);
    assert_eq(0, user_vm_map_image_page(parent, stack_va, stack_pa,
                                        VM_USER_RW | VM_NO_EXEC));

    uint64_t ro_va = BASE;
    assert_eq((int64_t)ro_va,
              test_mmap_call(parent, ro_va, 0x1000, PROT_READ,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(0, user_vm_fault(parent, ro_va, false));

    uint64_t absent_va = BASE + 0x2000;
    assert_eq((int64_t)absent_va,
              test_mmap_call(parent, absent_va, 0x1000, PROT_RW,
                             MAP_ANON_PRIVATE | MAP_FIXED));

    uint64_t none_va = BASE + 0x4000;
    assert_eq((int64_t)none_va,
              test_mmap_call(parent, none_va, 0x1000, PROT_RW,
                             MAP_ANON_PRIVATE | MAP_FIXED));
    assert_eq(0, user_vm_fault(parent, none_va, true));
    assert_eq(0, user_vm_mprotect(parent, none_va, 0x1000, PROT_NONE));

    /* Fork downgrades the stack page to hardware RO/NX (COW). */
    struct clone_publish_probe probe = { 0, 0 };
    assert_eq(0, user_vm_clone_fork(parent, child, clone_publish_ok, &probe));
    assert_eq(VM_USER_RO | VM_NO_EXEC, user_vm_test_leaf_vm(stack_va));
    assert_eq(2, page_cow_refs(stack_pa));

    /* Pure check accepts the COW stack page for write without breaking COW
     * or changing refcounts. */
    assert_true(user_vm_range_accessible(parent, stack_va + 0x100, 8, true));
    assert_true(user_vm_range_accessible(parent, stack_va + 0x100, 8, false));
    assert_eq(VM_USER_RO | VM_NO_EXEC, user_vm_test_leaf_vm(stack_va));
    assert_eq(2, page_cow_refs(stack_pa));

    /* Absent RW VMA is accepted for both read and write without faulting
     * it in; unmapped hole is rejected. */
    assert_true(user_vm_range_accessible(parent, absent_va, 0x1000, true));
    assert_true(user_vm_range_accessible(parent, absent_va, 0x1000, false));
    assert_eq(0, user_vm_test_leaf_state(absent_va));
    assert_false(user_vm_range_accessible(parent, BASE + 0x1000, 8, false));
    assert_false(user_vm_range_accessible(parent, BASE + 0x1000, 8, true));

    /* Logical-RO is accepted for read, rejected for write. */
    assert_true(user_vm_range_accessible(parent, ro_va, 8, false));
    assert_false(user_vm_range_accessible(parent, ro_va, 8, true));

    /* Stashed PROT_NONE is rejected for both read and write. */
    assert_false(user_vm_range_accessible(parent, none_va, 8, false));
    assert_false(user_vm_range_accessible(parent, none_va, 8, true));

    destroy_mm(child);
    destroy_mm(parent);
    assert_eq(user_vm_test_page_allocs() - user_vm_test_page_alloc_fails(),
              user_vm_test_free_count());
    assert_eq(user_vm_test_double_frees(), 0);
}

TEST_FUNC(test_shared_backing_writeback_and_overlays)
{
    user_vm_test_reset();
    struct vfs_mount *m = user_vm_test_make_mount("ext2");
    static unsigned char disk_buf[8192];
    memset(disk_buf, 0x11, sizeof(disk_buf));
    /* Node size = 5000 bytes (page 0 full 4096, page 1 partial 904 bytes). */
    struct vfs_node *n = user_vm_test_make_mount_node(m, 777, disk_buf, 5000);
    user_vm_test_set_node_writable(n, disk_buf);

    struct shared_backing *obj = NULL;
    assert_eq(0, shared_backing_open_file(n, &obj));
    assert_not_null(obj);

    uint64_t pa0 = 0, pa1 = 0;
    assert_eq(0, shared_backing_page(obj, 0, &pa0));
    assert_eq(0, shared_backing_page(obj, 1, &pa1));
    unsigned char *p0 = (unsigned char *)Phy_To_Virt(pa0);
    unsigned char *p1 = (unsigned char *)Phy_To_Virt(pa1);

    /* Modify page 0 and page 1 (including past EOF at offset 5000). */
    p0[10] = 0xAA;
    p1[10] = 0xBB;
    p1[2000] = 0xCC; /* beyond node->size (5000): must NOT overwrite disk_buf[6096] */
    shared_backing_mark_dirty(obj, 0);
    shared_backing_mark_dirty(obj, 1);

    /* Read overlay sees uncommitted dirty bytes before writeback. */
    unsigned char rbuf[16];
    memset(rbuf, 0, sizeof(rbuf));
    shared_backing_read_overlay(n, 8, 8, rbuf);
    assert_eq(0xAA, rbuf[2]);

    /* Writeback flushes dirty bytes clamped to node->size. */
    assert_eq(0, shared_backing_sync_file(n));
    assert_eq(0xAA, disk_buf[10]);
    assert_eq(0xBB, disk_buf[4096 + 10]);
    assert_eq(0x11, disk_buf[4096 + 2000]);

    /* Write overlay updates already-cached shared page. */
    unsigned char wbuf[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    shared_backing_write_overlay(n, 20, 4, wbuf);
    assert_eq(0xDE, p0[20]);
    assert_eq(0xEF, p0[23]);

    /* Truncate to 100 bytes: zeroes [100, 4096) in page 0 and reclaims page 1. */
    int frees_before = user_vm_test_free_count();
    n->size = 100;
    shared_backing_truncate(n, 100);
    assert_eq(0xAA, p0[10]);
    assert_eq(0x00, p0[100]);
    assert_eq(0x00, p0[4095]);
    assert_true(user_vm_test_free_count() > frees_before);

    /* Re-fetching page 1 allocates a fresh zeroed page (no stale 0xBB). */
    uint64_t pa1_new = 0;
    assert_eq(0, shared_backing_page(obj, 1, &pa1_new));
    unsigned char *p1_new = (unsigned char *)Phy_To_Virt(pa1_new);
    assert_eq(0x00, p1_new[10]);

    shared_backing_put(obj);
    assert_eq(0, user_vm_test_lock_violations());
    assert_eq(0, user_vm_test_double_frees());
    assert_eq(user_vm_test_kmallocs(), user_vm_test_kfrees());
    assert_eq(user_vm_test_page_allocs(), user_vm_test_free_count());
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
    TEST_ENTRY(edit_quiesce_error_restores_context),
    TEST_ENTRY(edit_quiesce_busy_fails_stopped),
    TEST_ENTRY(test_transaction_serial_interleaving_per_task),
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
    TEST_ENTRY(test_cow_write_fault_concurrent_last_ref_frees_old_page),
    TEST_ENTRY(test_prepare_copy_concurrent_last_ref_frees_old_page),
    TEST_ENTRY(test_cow_write_fault_upgrades_unique_ref),
    TEST_ENTRY(test_cow_write_fault_rejections),
    TEST_ENTRY(test_mprotect_shared_keeps_hardware_readonly),
    TEST_ENTRY(test_brk_shrink_privatizes_shared_tail),
    TEST_ENTRY(test_brk_shrink_shared_tail_oom_preserves_state),
    TEST_ENTRY(test_prepare_copy_rejects_hole_without_allocating),
    TEST_ENTRY(test_prepare_copy_map_failure_preserves_three_sources),
    TEST_ENTRY(test_prepare_copy_second_cow_alloc_failure),
    TEST_ENTRY(test_prepare_copy_success_commits_cow_and_absent),
    TEST_ENTRY(test_fault_entry_cow_write),
    TEST_ENTRY(test_mmap_fixed_replaces_overlap_and_keeps_neighbors),
    TEST_ENTRY(test_mmap_fixed_no_hint_is_einval),
    TEST_ENTRY(test_mmap_fixed_rejects_non_mmap_overlap),
    TEST_ENTRY(test_mmap_fixed_split_node_failure_changes_nothing),
    TEST_ENTRY(test_mmap_file_backed_demand_fill),
    TEST_ENTRY(test_mmap_shared_anon_fork_write_through),
    TEST_ENTRY(test_mmap_device_map_and_teardown_never_frees),
    TEST_ENTRY(test_mmap_device_rejects_out_of_window_and_rolls_back),
    TEST_ENTRY(test_mmap_device_occupied_range_rejected),
    TEST_ENTRY(test_mmap_fixed_guard_across_hole_rejects_device_overlap),
    TEST_ENTRY(test_mmap_device_read_only_leaf_is_ro),
    TEST_ENTRY(test_shared_backing_identity),
    TEST_ENTRY(test_shared_backing_fill_race),
    TEST_ENTRY(test_shared_backing_fill_retry),
    TEST_ENTRY(test_inode_shared_fault_and_uaccess),
    TEST_ENTRY(test_shared_anon_fork_backing),
    TEST_ENTRY(test_shared_anon_fork_backing_publish_fail),
    TEST_ENTRY(test_shared_vma_retire),
    TEST_ENTRY(test_shared_file_protect_ceiling),
    TEST_ENTRY(test_range_accessible_cow_and_absent),
    TEST_ENTRY(test_shared_backing_writeback_and_overlays),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
