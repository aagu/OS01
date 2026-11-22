/* kernel/memory/user_vm.c — AArch64 M4 anonymous-VM policy.
 *
 * This unit owns the semantic side of the first AArch64 user-VM
 * increment: VMA reservation (mmap/brk/mprotect/munmap), the sparse
 * resident-page chain, demand paging and the copy pre-instantiation
 * pass.  It never reads ESR, a page descriptor or the M3
 * AARCH64_PT_EPROT_NONE sentinel — the arch leaf adapter
 * (arch/aarch64/memory/user_vm.c) normalizes the three leaf states.
 *
 * Ownership rules (spec §地址布局和所有权 / §切离、修改和页权限):
 *   - the caller owns the mm, has detached its root from TTBR0 and
 *     unpublished it, holds no kernel lock and cannot be preempted by
 *     a scheduling point;
 *   - a resident record owns its PA, so every removal frees it once;
 *   - ELF and stack pages are never recorded here (the image manifest
 *     owns them);
 *   - an operation preflights the whole range and preallocates its
 *     split nodes before touching any leaf or VMA; a commit-time
 *     mismatch is an invariant failure, never a partial repair.
 *
 * The AArch64 build collects this TU through the arch memory wildcard
 * (kernel/Makefile ARCH_SOURCE_DIRS): the policy needs the AArch64
 * leaf adapter and has no x86 counterpart.
 */

#include <arch/mmu.h>           /* AARCH64_USER_MIN / AARCH64_USER_LIMIT */
#include <arch/aarch64/user_image.h>  /* AARCH64_USER_STACK_BASE */
#include <errno.h>
#include <fs/file.h>                    /* files_get_file (mmap fd) */
#include <kernel.h>             /* container_of */
#ifdef AARCH64_USER_RUNTIME_TEST
#include <selftest/aarch64_user_runtime.h>
#endif
#include <memory/memory.h>      /* Phy_To_Virt */
#include <memory/pmm.h>         /* alloc_4k_page / free_4k_page / PAGE_4K_* */
#include <memory/shared_backing.h>  /* inode MAP_SHARED object registry */
#include <memory/slab.h>        /* kmalloc / kfree */
#include <arch/aarch64/user_elf.h>     /* aarch64_elf_clean_code_page / sync_icache */
#include <arch/aarch64/user_vm_leaf.h>    /* leaf adapter (arch_user_vm_*) */
#include <arch/aarch64/user_vm_policy.h>  /* this TU's policy API */
#include <arch/aarch64/user_vm_internal.h> /* shared fork internals */
#include <memory/vm_area.h>
#include <memory/vma.h>         /* vma_t, VMA_*, MAP_*, PROT_* */
#include <memory/vmm.h>         /* VM_*, arch_vmm_* */
#include <sched/task.h>         /* mm_t */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <arch/aarch64/mmu.h>   /* AARCH64_USER_MIN/LIMIT */
#include <arch/aarch64/vmm_backend.h>

/* Fixed AArch64 layout (spec §地址布局和所有权). */
#define USER_VM_HEAP_LIMIT UINT64_C(0x203ff000)
#define USER_VM_MMAP_BASE  UINT64_C(0x40000000)
#define USER_VM_PROT_MASK  (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC)
#define USER_VM_MAP_KNOWN  (MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS)

/* ── Small helpers ──────────────────────────────────────────────── */

static uint64_t page_align_up(uint64_t x)
{
    return (x + (uint64_t)PAGE_4K_SIZE - 1) & ~((uint64_t)PAGE_4K_SIZE - 1);
}

/* fd-table access for the mmap entry: the production build reads the
 * syscall context's table; hosttests override this weak symbol with a
 * one-slot fake table (the mock environment keeps task_struct
 * incomplete). */
#ifdef OS01_HOST_TEST
files_t *user_vm_policy_files(void);    /* hosttests provide the fake */
#else
__attribute__((weak)) files_t *user_vm_policy_files(void)
{
    return current ? current->files : NULL;
}
#endif

/* mm->pgdir holds the root PA; the M3 entries take the direct-map
 * pointer. */
uint64_t *aarch64_user_vm_root_va(mm_t *mm)
{
    return (uint64_t *)Phy_To_Virt((uintptr_t)mm->pgdir);
}

/* A mmap-window VMA: anonymous-private, anonymous-shared or
 * file-backed (2026-11-19).  Heap and IO VMAs stay out — their VA
 * windows are outside the mmap region anyway, this is the defensive
 * shape check. */
static bool policy_vma_is_mmap(const vma_t *v)
{
    if (v->vm_flags & (VMA_HEAP | VMA_IO))
        return false;
    if (v->vm_file != NULL)
        return true;                     /* file-backed mmap */
    return (v->vm_flags & VMA_ANON) != 0;
}

static vma_t *first_overlap(mm_t *mm, uint64_t start, uint64_t end)
{
    for (list_t *p = mm->vma_list.next; p != &mm->vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_end <= start) continue;
        if (v->vm_start >= end) break;
        return v;
    }
    return NULL;
}

static vma_t *last_overlap(mm_t *mm, uint64_t start, uint64_t end)
{
    vma_t *last = NULL;
    for (list_t *p = mm->vma_list.next; p != &mm->vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_start >= end) break;
        if (v->vm_end > start) last = v;
    }
    return last;
}

static bool range_free(mm_t *mm, uint64_t start, uint64_t end)
{
    return first_overlap(mm, start, end) == NULL;
}

/* ── Sparse resident chain (NULL-terminated, sorted by VA) ───────
 *
 * The head lives in mm_t and is zero-initialised by calloc; links are
 * NULL at the ends, unlike the circular vma_list. */

static user_vm_page_t *resident_lookup(mm_t *mm, uint64_t va)
{
    for (list_t *p = mm->resident_list.next; p; p = p->next) {
        user_vm_page_t *r = container_of(p, user_vm_page_t, list);
        if (r->va == va)
            return r;
        if (r->va > va)
            break;
    }
    return NULL;
}

void aarch64_user_vm_resident_insert(mm_t *mm, user_vm_page_t *node)
{
    list_t *prev = NULL;
    list_t *p = mm->resident_list.next;
    while (p) {
        user_vm_page_t *r = container_of(p, user_vm_page_t, list);
        if (node->va < r->va)
            break;
        prev = p;
        p = p->next;
    }
    node->list.prev = prev;
    node->list.next = p;
    if (prev)
        prev->next = &node->list;
    else
        mm->resident_list.next = &node->list;
    if (p)
        p->prev = &node->list;
    else
        mm->resident_list.prev = &node->list;
}

void aarch64_user_vm_resident_unlink(mm_t *mm, user_vm_page_t *node)
{
    list_t *prev = node->list.prev;
    list_t *next = node->list.next;

    if (prev)
        prev->next = next;
    else
        mm->resident_list.next = next;
    if (next)
        next->prev = prev;
    else
        mm->resident_list.prev = prev;

    node->list.prev = NULL;
    node->list.next = NULL;
}

/* Internal corruption check: the record and the page table must
 * agree on the PA.  A query failure is as fatal as a mismatch — the
 * policy never proceeds past a broken invariant. */
/* VMA reference transaction (inode MAP_SHARED): a fragment holds one
 * node reference (vm_file metadata) and one shared-object reference.
 * Constructors and splitters acquire; every removal path releases
 * through the put helper. */
void aarch64_user_vm_vma_get_refs(vma_t *v)
{
    if (!v)
        return;
    if (v->vm_file)
        vfs_node_get(v->vm_file);
    if (v->vm_shared_backing)
        shared_backing_get(v->vm_shared_backing);
}

void aarch64_user_vm_vma_put_refs(vma_t *v)
{
    if (!v)
        return;
    if (v->vm_shared_backing)
        shared_backing_put(v->vm_shared_backing);
    if (v->vm_file)
        vfs_node_put(v->vm_file);
}

/* VMA drain (fork rollback + destroy): unlink every VMA node, release
 * its node + object references and free it.  The caller keeps
 * ownership of any break/stack field cleanup it still needs. */
void aarch64_user_vm_free_vmas(mm_t *mm)
{
    while (mm->vma_list.next != &mm->vma_list) {
        vma_t *v = container_of(mm->vma_list.next, vma_t, list);
        aarch64_user_vm_vma_put_refs(v);
        vm_area_unlink(v);
        kfree(v);
    }
}

static void resident_verify(mm_t *mm, uint64_t *root, user_vm_page_t *r)
{
    struct user_vm_leaf leaf;
    int rc = arch_user_vm_query(root, r->va, &leaf);
    if (rc != 0 || leaf.state == USER_VM_ABSENT || leaf.pa != r->pa)
        arch_user_vm_invariant("resident leaf/PA mismatch");
}

/* Drop one reference to a shared PA and free it only when that was the
 * last one.  A private record owns the page outright. */
static void resident_drop_pa(uint64_t pa, uint32_t share)
{
    if (share == USER_VM_PAGE_SHARED) {
        bool last = false;
        if (pmm_cow_release(pa, &last) != 0)
            arch_user_vm_invariant("shared record release mismatch");
        if (!last)
            return;
    }
    free_4k_page(pa);
}

/* Borrowed-leaf removal (device window / shared-object page): unmap
 * and verify only — the PA belongs to the device or to the shared
 * backing object, not to this mm. */
static void io_remove_leaf(mm_t *mm, user_vm_page_t *r, uint64_t *root)
{
    struct user_vm_leaf leaf;
    int rc = arch_user_vm_remove(root, r->va, &leaf);
    if (rc != 0 || leaf.state == USER_VM_ABSENT || leaf.pa != r->pa)
        arch_user_vm_invariant("borrowed leaf remove mismatch");
    aarch64_user_vm_resident_unlink(mm, r);
    kfree(r);
}

/* Commit-time removal: remove the leaf, verify the PA the adapter
 * hands back, drop the record's reference (freeing on the last one),
 * then unlink and free the record.  Borrowed leaves (device window,
 * shared-object pages) unmap without freeing: their PA belongs to the
 * device or to the shared backing object. */
static void resident_remove_leaf(mm_t *mm, user_vm_page_t *r, uint64_t *root)
{
    if (r->source == USER_VM_PAGE_IO ||
        r->source == USER_VM_PAGE_BACKING) {
        io_remove_leaf(mm, r, root);
        return;
    }
    struct user_vm_leaf leaf;
    int rc = arch_user_vm_remove(root, r->va, &leaf);
    if (rc != 0 || leaf.state == USER_VM_ABSENT || leaf.pa != r->pa)
        arch_user_vm_invariant("resident remove mismatch");
    resident_drop_pa(leaf.pa, r->share);
    aarch64_user_vm_resident_unlink(mm, r);
    kfree(r);
}

/* ── COW privatization ─────────────────────────────────────────── */

/* Bring one shared resident page back to exclusive ownership.
 *
 * Preconditions (all checked here, nothing changes on failure): the
 * record is shared with logical write permission, and its leaf is
 * present at exactly r->pa with hardware RO/NX.
 *
 *   refs > 1 : allocate a private copy, copy the whole page, replace
 *              the leaf through the checked COW entry, drop the old
 *              reference (not the last one, so the PA stays alive for
 *              the other owner).
 *   refs == 1: upgrade the same PA to RW/NX and release the unique
 *              reference; the PA becomes exclusively ours and is not
 *              freed.
 *
 * Returns 0, -ENOMEM when the private copy cannot be allocated, or
 * -EFAULT for a state that is not a legal COW target.  The caller
 * must hold the detached root. */
static int cow_private_page(mm_t *mm, user_vm_page_t *r, uint64_t *root)
{
    (void)mm;
    if (!r || r->share != USER_VM_PAGE_SHARED ||
        !(r->logical & VM_WRITE))
        return -EFAULT;

    struct user_vm_leaf leaf;
    if (arch_user_vm_query(root, r->va, &leaf) != 0 ||
        leaf.state != USER_VM_PRESENT || leaf.pa != r->pa ||
        leaf.flags != (VM_USER_RO | VM_NO_EXEC))
        return -EFAULT;

    uint16_t refs = page_cow_refs(r->pa);
    if (refs == 0)
        arch_user_vm_invariant("shared record without a reference");

    if (refs == 1) {
        uint64_t old_pa = 0;
        uint32_t old_vm = 0;
        int rc = arch_vmm_update_4k(root, r->pa, r->va,
                                    VM_USER_RW | VM_NO_EXEC,
                                    &old_pa, &old_vm);
        if (rc != 0 || old_pa != r->pa ||
            old_vm != (VM_USER_RO | VM_NO_EXEC))
            arch_user_vm_invariant("cow upgrade mismatch");

        bool last = false;
        if (pmm_cow_release(r->pa, &last) != 0 || !last)
            arch_user_vm_invariant("cow unique release mismatch");
        r->share = USER_VM_PAGE_PRIVATE;
        return 0;
    }

    uint64_t new_pa = alloc_4k_page();
    if (!new_pa)
        return -ENOMEM;
    memcpy(Phy_To_Virt(new_pa), Phy_To_Virt(r->pa), PAGE_4K_SIZE);

    uint64_t old_pa = r->pa;
    int rc = arch_user_vm_replace_cow(root, r->va, old_pa, new_pa);
    if (rc != 0) {
        free_4k_page(new_pa);
        return (rc == -ENOMEM) ? -ENOMEM : -EFAULT;
    }

#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_cow_barrier(r->va, old_pa);
#endif

    /* Release the old PA only after the leaf swap and TLB invalidation
     * have completed; if a concurrent owner dropped its reference first,
     * this is the last reference and we free old_pa here. */
    bool last = false;
    if (pmm_cow_release(old_pa, &last) != 0)
        arch_user_vm_invariant("cow copy release mismatch");
    if (last) {
#ifdef AARCH64_USER_RUNTIME_TEST
        aarch64_user_runtime_test_cow_last_free(old_pa);
#endif
        free_4k_page(old_pa);
    }

    r->pa = new_pa;
    r->share = USER_VM_PAGE_PRIVATE;
    return 0;
}

int user_vm_cow_fault(mm_t *mm, uint64_t far)
{
    if (!mm)
        return -EFAULT;

    uint64_t va = far & ~((uint64_t)PAGE_4K_SIZE - 1);
    if (va < AARCH64_USER_MIN || va >= AARCH64_USER_LIMIT)
        return -EFAULT;

    user_vm_page_t *r = resident_lookup(mm, va);
    if (!r)
        return -EFAULT;

    /* Shared-backing write fault (inode MAP_SHARED): the page stays an
     * object page — upgrade ONLY this task's leaf to the logical
     * permission.  Every owner reads the same PA, so the write is
     * visible to all of them without their own write faults.  No PMM
     * COW reference exists to release, and the object — not the mm —
     * owns the PA.  A write fault on a logically read-only leaf is a
     * real access violation. */
    if (r->source == USER_VM_PAGE_BACKING) {
        if (!(r->logical & VM_USER_RW))
            return -EFAULT;
        uint64_t *root = aarch64_user_vm_root_va(mm);
        if (arch_vmm_update_4k(root, r->pa, r->va, r->logical,
                               NULL, NULL) != 0)
            return -EFAULT;
        return 0;
    }

    /* cow_private_page performs the remaining checks (shared, logical
     * RW, same-PA RO/NX leaf) and returns -EFAULT for a non-COW
     * fault, -ENOMEM for copy exhaustion. */
    return cow_private_page(mm, r, aarch64_user_vm_root_va(mm));
}

/* ── Permission translation ────────────────────────────────────── */

static int prot_to_vma_flags(uint64_t prot, uint64_t *out)
{
    *out = 0;
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;

    switch (prot) {
    case PROT_NONE:
        return 0;
    case PROT_READ:
        *out = VMA_PROT_READ;
        return 0;
    case PROT_READ | PROT_WRITE:
        *out = VMA_PROT_READ | VMA_PROT_WRITE;
        return 0;
    default:
        /* Write-only is impossible on AArch64; EXEC is out of scope. */
        return -EINVAL;
    }
}

/* VM_* word for a newly allocated present leaf.  The policy never
 * grants execute permission to anonymous memory. */
static uint32_t present_vm(uint64_t vma_flags)
{
    if (vma_flags & VMA_PROT_WRITE)
        return VM_USER_RW | VM_NO_EXEC;
    return VM_USER_RO | VM_NO_EXEC;
}

/* VM_* word a mprotect request installs on a resident leaf. */
static uint32_t target_vm(uint64_t prot)
{
    if (prot == PROT_NONE)
        return VM_PROTNONE;
    if (prot & PROT_WRITE)
        return VM_USER_RW | VM_NO_EXEC;
    return VM_USER_RO | VM_NO_EXEC;
}

static bool leaf_readable(uint32_t vm)
{
    return (vm & (VM_PRESENT | VM_PROTNONE)) == VM_PRESENT;
}

static bool leaf_writable(uint32_t vm)
{
    return (vm & (VM_PRESENT | VM_PROTNONE)) == VM_PRESENT &&
           (vm & VM_WRITE) != 0;
}

/* An absent page may be instantiated only when a heap or anonymous
 * mmap VMA covers it with the requested permission.  ELF/stack pages
 * are never absent-authorized here — they exist as manifested leaves
 * or they are not accessible. */
static int authorize_absent(mm_t *mm, uint64_t va, bool write,
                            uint32_t *map_vm_out)
{
    vma_t *v = vm_area_lookup(mm, va);
    if (!v)
        return -EFAULT;

    bool heap = (v->vm_flags & VMA_HEAP) != 0;
    bool anon_mmap = policy_vma_is_mmap(v) && va >= USER_VM_MMAP_BASE;
    if (!heap && !anon_mmap)
        return -EFAULT;
    if (heap && va >= USER_VM_HEAP_LIMIT)
        return -EFAULT;

    uint64_t need = write ? VMA_PROT_WRITE : VMA_PROT_READ;
    if ((v->vm_flags & need) != need)
        return -EFAULT;

    if (map_vm_out)
        *map_vm_out = present_vm(v->vm_flags);
    return 0;
}

/* ── VMA split ─────────────────────────────────────────────────── */

/* Split v at `at` using the caller's preallocated right node.  The
 * caller has already verified `v->vm_start < at < v->vm_end`. */
static void split_policy_vma(mm_t *mm, vma_t *v, uint64_t at, vma_t *right)
{
    list_init(&right->list);
    right->vm_start     = at;
    right->vm_end       = v->vm_end;
    right->vm_flags     = v->vm_flags;
    right->vm_pgoff     = v->vm_pgoff +
        ((at - v->vm_start) >> PAGE_4K_SHIFT);
    right->vm_file      = v->vm_file;
    right->vm_shared_backing = v->vm_shared_backing;
    aarch64_user_vm_vma_get_refs(right);   /* each fragment holds refs */
    v->vm_end = at;
    vm_area_insert_sorted(mm, right);
}

/* ── init / destroy ────────────────────────────────────────────── */

int user_vm_init_heap(mm_t *mm, uint64_t heap_base)
{
    if (!mm)
        return -EINVAL;
    /* The mm must be a fresh, unpublished anonymous address space. */
    if (mm->start_brk != 0 || mm->end_brk != 0 ||
        mm->resident_list.next != NULL || !list_is_empty(&mm->vma_list))
        return -EINVAL;

    uint64_t base = page_align_up(heap_base);
    if (base < AARCH64_USER_MIN || base > USER_VM_HEAP_LIMIT)
        return -EINVAL;

    vma_t *hv = kmalloc(sizeof(*hv));
    if (!hv)
        return -ENOMEM;

    list_init(&hv->list);
    hv->vm_start     = base;
    hv->vm_end       = base;            /* zero-length until brk grows */
    hv->vm_flags     = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON | VMA_HEAP;
    hv->vm_pgoff     = 0;
    hv->vm_file      = NULL;
    hv->vm_shared_backing = NULL;

    mm->start_brk = base;
    mm->end_brk   = base;
    return vm_area_insert_sorted(mm, hv);
}

/* The only permission words an image leaf may carry: RO/X, RO/NX,
 * RW/NX.  W+X is rejected by the ELF segment validator before this
 * entry is reached. */
static bool image_perm_legal(uint32_t flags)
{
    return flags == VM_USER_RO ||
           flags == (VM_USER_RO | VM_NO_EXEC) ||
           flags == (VM_USER_RW | VM_NO_EXEC);
}

int user_vm_map_io_page(mm_t *mm, uint64_t va, uint64_t pa,
                        uint32_t vm_flags)
{
    if (!mm)
        return -EINVAL;
    if (va < USER_VM_MMAP_BASE || va >= AARCH64_USER_LIMIT ||
        (va & ((uint64_t)PAGE_4K_SIZE - 1)) != 0)
        return -EINVAL;
    if (!pa || (pa & ((uint64_t)PAGE_4K_SIZE - 1)) != 0)
        return -EINVAL;
    if (resident_lookup(mm, va))
        return -EEXIST;

    user_vm_page_t *rec = kmalloc(sizeof(*rec));
    if (!rec)
        return -ENOMEM;

    if (arch_vmm_map_user_io_4k(aarch64_user_vm_root_va(mm), pa, va,
                                vm_flags) != 0) {
        kfree(rec);
        return -EINVAL;
    }

    rec->va      = va;
    rec->pa      = pa;
    rec->source  = USER_VM_PAGE_IO;
    rec->logical = vm_flags;
    rec->share   = USER_VM_PAGE_PRIVATE;
    aarch64_user_vm_resident_insert(mm, rec);
    return 0;
}

int user_vm_map_image_page(mm_t *mm, uint64_t va, uint64_t pa, uint32_t flags)
{
    if (!mm)
        return -EINVAL;
    if (va < AARCH64_USER_MIN || va >= AARCH64_USER_LIMIT ||
        (va & ((uint64_t)PAGE_4K_SIZE - 1)) != 0)
        return -EINVAL;
    if (!pa || (pa & ((uint64_t)PAGE_4K_SIZE - 1)) != 0)
        return -EINVAL;
    if (!image_perm_legal(flags))
        return -EINVAL;
    if (resident_lookup(mm, va))
        return -EEXIST;

    /* Record first: a map failure leaves the PA with the caller. */
    user_vm_page_t *rec = kmalloc(sizeof(*rec));
    if (!rec)
        return -ENOMEM;
    rec->va      = va;
    rec->pa      = pa;
    rec->source  = (va == AARCH64_USER_STACK_BASE)
                       ? USER_VM_PAGE_STACK : USER_VM_PAGE_ELF;
    rec->logical = flags;
    rec->share   = USER_VM_PAGE_PRIVATE;
    rec->list.prev = NULL;
    rec->list.next = NULL;

    int rc = arch_vmm_map_4k_new(aarch64_user_vm_root_va(mm), pa, va, flags);
    if (rc) {
        kfree(rec);             /* PA still belongs to the caller */
        return rc;
    }

    aarch64_user_vm_resident_insert(mm, rec);
    return 0;
}

bool user_vm_has_metadata(const mm_t *mm)
{
    if (!mm)
        return false;
    return mm->resident_list.next != NULL ||
           mm->vma_list.next != &mm->vma_list;
}

void user_vm_destroy(mm_t *mm)
{
    if (!mm)
        return;

    uint64_t *root = aarch64_user_vm_root_va(mm);

    list_t *p = mm->resident_list.next;
    while (p) {
        list_t *next = p->next;
        user_vm_page_t *r = container_of(p, user_vm_page_t, list);
        resident_remove_leaf(mm, r, root);
        p = next;
    }
    mm->resident_list.next = NULL;
    mm->resident_list.prev = NULL;

    aarch64_user_vm_free_vmas(mm);

    mm->start_brk = 0;
    mm->end_brk   = 0;
}

/* ── brk ───────────────────────────────────────────────────────── */

int64_t user_vm_brk(mm_t *mm, uint64_t addr)
{
    if (!mm || mm->start_brk == 0)
        return -ENOMEM;

    uint64_t old_end = mm->end_brk;
    if (addr == 0)
        return (int64_t)old_end;
    if (addr < mm->start_brk)
        return -EINVAL;
    if (addr > USER_VM_HEAP_LIMIT)
        return -ENOMEM;
    if (addr == old_end)
        return (int64_t)addr;

    vma_t *heap = vm_area_find_heap(mm);
    if (!heap)
        return -ENOMEM;

    if (addr > old_end) {
        /* Growth commits metadata only; pages appear on demand. */
        mm->end_brk  = addr;
        heap->vm_end = page_align_up(addr);
        return (int64_t)addr;
    }

    /* Shrink.  Pages wholly outside the new aligned boundary are
     * released; the page containing an unaligned break keeps its PA
     * and has its tail zeroed through the direct-map alias. */
    uint64_t aligned_new = page_align_up(addr);
    uint64_t aligned_old = page_align_up(old_end);
    uint64_t *root = aarch64_user_vm_root_va(mm);

    /* The retained tail must be exclusively ours before it is zeroed:
     * writing a shared page would corrupt the peer.  This is the first
     * fallible step and it runs BEFORE the break, any released page or
     * the tail bytes change; failure leaves the whole shrink undone. */
    user_vm_page_t *tail = NULL;
    if (addr != aligned_new) {
        tail = resident_lookup(mm, aligned_new - (uint64_t)PAGE_4K_SIZE);
        if (tail && tail->share == USER_VM_PAGE_SHARED) {
            int prc = cow_private_page(mm, tail, root);
            if (prc)
                return prc;
        }
    }

    list_t *p = mm->resident_list.next;
    while (p) {
        list_t *next = p->next;
        user_vm_page_t *r = container_of(p, user_vm_page_t, list);
        if (r->va >= aligned_new && r->va < aligned_old)
            resident_remove_leaf(mm, r, root);
        p = next;
    }

    if (tail) {
        resident_verify(mm, root, tail);
        size_t off = (size_t)(addr -
                              (aligned_new - (uint64_t)PAGE_4K_SIZE));
        memset((unsigned char *)Phy_To_Virt(tail->pa) + off, 0,
               (size_t)((uint64_t)PAGE_4K_SIZE - off));
    }

    mm->end_brk  = addr;
    heap->vm_end = aligned_new;
    return (int64_t)addr;
}

/* ── mmap ──────────────────────────────────────────────────────── */

/* Release the mmap entry's transient references (node + object):
 * the VMA never took ownership. */
static void mmap_release_transient(vfs_node_t *file_node,
                                   struct shared_backing *backing)
{
    if (backing)
        shared_backing_put(backing);
    if (file_node)
        vfs_node_put(file_node);
}

int64_t user_vm_mmap(mm_t *mm, const uint64_t args[6])
{
    if (!mm || !args)
        return -EINVAL;

    uint64_t hint  = args[0];
    uint64_t len   = args[1];
    uint64_t prot  = args[2];
    uint64_t flags = args[3];
    uint64_t fd    = args[4];
    uint64_t off   = args[5];

    /* Flag syntax first: unknown bits and the private/shared pair.
     * EXACTLY ONE of the pair is legal syntax; shared-alone is a
     * syntactically valid request for an unsupported mapping (below),
     * while both-set or neither-set is a syntax error. */
    if (flags & ~(uint64_t)USER_VM_MAP_KNOWN)
        return -EINVAL;
    uint64_t map_type = flags & (MAP_SHARED | MAP_PRIVATE);
    if (map_type != MAP_PRIVATE && map_type != MAP_SHARED)
        return -EINVAL;

    uint64_t vma_flags = 0;
    int rc = prot_to_vma_flags(prot, &vma_flags);
    if (rc)
        return rc;

    /* File/anonymous shared mappings (inode MAP_SHARED) rendezvous on
     * an architecture-neutral shared object keyed by the mount
     * instance and inode; anonymous shared regions create a fresh
     * zero-filled object that fork inherits.  File MAP_PRIVATE keeps
     * the 2026-11-19 demand-fill path. */
    bool fixed = (flags & MAP_FIXED) != 0;
    bool file_backed = (flags & MAP_ANONYMOUS) == 0;
    bool shared_map = (map_type == MAP_SHARED);

    /* fd/off: an anonymous mapping requires fd=-1/off=0; a file
     * mapping resolves the fd through the caller's fd table (the
     * syscall context owns the detached root for the whole entry, so
     * the table is stable).  At most ONE temporary fd reference is
     * taken for the whole entry — device probing and file admission
     * share it — and every exit below releases it.  The VMA holds its
     * own node reference for its whole life (fork dups it,
     * munmap/split/destroy drop it). */
    file_t *f = NULL;
    if (fd != UINT64_MAX && fd < NOFILE)
        f = files_get_file(user_vm_policy_files(), (int)fd);

#define MMAP_RETURN(value)                                              \
    do {                                                                \
        int64_t __mrc = (value);                                        \
        files_put_file(f);                                              \
        return __mrc;                                                   \
    } while (0)

    vfs_node_t *file_node = NULL;
    bool device_map = false;
    if (f && f->type == FD_DEV && f->node && f->node->name &&
        strcmp(f->node->name, "mem") == 0)
        device_map = true;              /* /dev/mem: offset = phys base */

    if (len == 0)
        MMAP_RETURN(-EINVAL);
    uint64_t alen = page_align_up(len);
    if (alen < len)                                   /* overflow */
        MMAP_RETURN(-EINVAL);
    if (alen > AARCH64_USER_LIMIT - USER_VM_MMAP_BASE)
        MMAP_RETURN(-ENOMEM);
    if (device_map) {
        /* Device mmap (M4-remaining): /dev/mem maps physical pages
         * from the DTB device window with the VM_DEVICE attribute.
         * MAP_SHARED semantics only (device memory is inherently
         * shared), eager leaves, no demand paging, no COW, and the
         * teardown never frees the PA.  v1 maps only into a FREE
         * range — an occupied range is refused whole (final review
         * 2026-11-19: a silent overlapping insert would corrupt the
         * lookup/split/munmap invariants); replace-style device
         * remapping waits for a real user. */
        if (map_type != MAP_SHARED)
            MMAP_RETURN(-EINVAL);
        if (hint == 0 || (hint & ((uint64_t)PAGE_4K_SIZE - 1)) ||
            hint < USER_VM_MMAP_BASE || hint >= AARCH64_USER_LIMIT ||
            alen > AARCH64_USER_LIMIT - hint)
            MMAP_RETURN(-EINVAL);
        if (off & ((uint64_t)PAGE_4K_SIZE - 1))
            MMAP_RETURN(-EINVAL);
        if (!range_free(mm, hint, hint + alen))
            MMAP_RETURN(-EINVAL);

        uint32_t dev_vm = (vma_flags & VMA_PROT_WRITE)
                              ? (VM_USER_RW | VM_NO_EXEC)
                              : (VM_USER_RO | VM_NO_EXEC);

        vma_t *dv = kmalloc(sizeof(*dv));
        if (!dv)
            MMAP_RETURN(-ENOMEM);
        list_init(&dv->list);
        dv->vm_start     = hint;
        dv->vm_end       = hint + alen;
        dv->vm_flags     = vma_flags | VMA_IO;
        dv->vm_pgoff     = 0;
        dv->vm_file      = NULL;
        dv->vm_shared_backing = NULL;

        for (uint64_t p = 0; p < alen; p += PAGE_4K_SIZE) {
            int rc2 = user_vm_map_io_page(mm, hint + p, off + p, dev_vm);
            if (rc2 != 0) {
                /* Rollback: the leaves mapped so far go through the
                 * same remove path (unmap, no free). */
                for (uint64_t q = 0; q < p; q += PAGE_4K_SIZE) {
                    user_vm_page_t *rr = resident_lookup(mm, hint + q);
                    if (rr)
                        io_remove_leaf(mm, rr,
                                       aarch64_user_vm_root_va(mm));
                }
                kfree(dv);
                MMAP_RETURN(rc2);
            }
        }
        vm_area_insert_sorted(mm, dv);
        MMAP_RETURN((int64_t)hint);
    }

    /* ── fd disposition ───────────────────────────────────────────────
     * Anonymous fd/offset rules, then file admission.  The VMA keeps
     * one node reference; a shared mapping additionally keeps one
     * object reference.  Both are acquired HERE — before any MAP_FIXED
     * old-leaf removal — so a fixed replace that overlaps the same
     * object can never drop its cached pages. */
    if (!device_map && !file_backed) {
        if (fd != UINT64_MAX || off != 0)
            MMAP_RETURN(-EINVAL);
    } else if (!device_map) {
        if (fd == UINT64_MAX || fd >= NOFILE)
            MMAP_RETURN(-EINVAL);
        if (!f || f->type != FD_VFS || !f->node)
            MMAP_RETURN(-EBADF);
        file_node = vfs_node_get(f->node);
        if (!file_node)
            MMAP_RETURN(-ENOMEM);
    }

    struct shared_backing *backing = NULL;
    bool maywrite = false;
    if (shared_map && file_backed && !device_map) {
        /* File shared ceiling: the fd's access mode is the maximum
         * write permission for the whole mapping lifetime.  A
         * non-readable fd or a write request without O_RDWR is
         * refused whole. */
        uint32_t mode = (uint32_t)f->flags & 3u;
        if (mode == O_WRONLY) {
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EACCES);
        }
        if ((vma_flags & VMA_PROT_WRITE) && mode != O_RDWR) {
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EACCES);
        }
        /* Offset window: a negative off_t, a non-page-aligned offset,
         * or a mapping whose last byte would exceed INT64_MAX is
         * refused before anything is published. */
        if ((int64_t)off < 0) {
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EINVAL);
        }
        if (off & ((uint64_t)PAGE_4K_SIZE - 1)) {
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EINVAL);
        }
        if (alen - PAGE_4K_SIZE > (uint64_t)INT64_MAX - off) {
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EINVAL);
        }
        int orc = shared_backing_open_file(f->node, &backing);
        if (orc != 0) {
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(orc);
        }
        maywrite = (mode == O_RDWR);
    } else if (shared_map) {
        /* Anonymous shared: a fresh zero-filled object per mmap; fork
         * inherits the reference, independent mmaps stay separate. */
        if (shared_backing_create_anon(&backing) != 0)
            MMAP_RETURN(-ENOMEM);
        maywrite = true;
    }

    uint64_t cand = 0;

    if (hint != 0) {
        if ((hint & ((uint64_t)PAGE_4K_SIZE - 1)) ||
            hint < USER_VM_MMAP_BASE || hint >= AARCH64_USER_LIMIT ||
            alen > AARCH64_USER_LIMIT - hint) {
        mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EINVAL);
        }
        if (fixed) {
            /* MAP_FIXED takes the hint as-is; overlapping mmap-window
             * VMAs are replaced below (the window check already
             * excludes the ELF / stack / heap VMAs, which all live
             * below USER_VM_MMAP_BASE). */
            cand = hint;
        } else if (range_free(mm, hint, hint + alen)) {
            cand = hint;
        }
        /* A valid but occupied hint falls back to the base first-fit. */
    }

    if (cand == 0 && fixed) {
        mmap_release_transient(file_node, backing);
        MMAP_RETURN(-EINVAL);      /* MAP_FIXED without a usable hint */
    }

    if (cand == 0) {
        uint64_t cursor = USER_VM_MMAP_BASE;
        for (list_t *p = mm->vma_list.next; p != &mm->vma_list;
             p = p->next) {
            vma_t *v = container_of(p, vma_t, list);
            if (v->vm_end <= cursor)
                continue;
            if (v->vm_start >= cursor &&
                v->vm_start - cursor >= alen)
                break;
            if (v->vm_end > cursor)
                cursor = v->vm_end;
        }
        if (cursor >= AARCH64_USER_LIMIT ||
            alen > AARCH64_USER_LIMIT - cursor) {
        mmap_release_transient(file_node, backing);
            MMAP_RETURN(-ENOMEM);
        }
        cand = cursor;
    }

    vma_t *v = kmalloc(sizeof(*v));
    if (!v) {
        mmap_release_transient(file_node, backing);
        MMAP_RETURN(-ENOMEM);
    }
    list_init(&v->list);
    v->vm_start     = cand;
    v->vm_end       = cand + alen;
    v->vm_flags     = vma_flags |
                      (file_backed ? 0 : VMA_ANON) |
                      (shared_map ? VMA_SHARED : 0) |
                      (shared_map && maywrite ? VMA_SHARED_MAYWRITE : 0);
    v->vm_pgoff     = file_backed ? (off >> PAGE_4K_SHIFT) : 0;
    v->vm_file      = file_node;            /* the VMA owns the reference */
    v->vm_shared_backing = backing;         /* and the object reference */

    if (!fixed) {
        vm_area_insert_sorted(mm, v);
        MMAP_RETURN((int64_t)cand);
    }

    /* ── MAP_FIXED replace transaction ────────────────────────────
     * Mirrors user_vm_munmap: preallocate the two split nodes
     * together with the new VMA node, then commit — after the
     * preallocation nothing below can fail with -ENOMEM, so the
     * caller never observes a half-released window.  The new mapping's
     * node and object references were taken above, so a replace that
     * overlaps the SAME object never loses its cached pages. */
    uint64_t end = cand + alen;
    vma_t *first = first_overlap(mm, cand, end);
    vma_t *last  = last_overlap(mm, cand, end);
    bool need_start = first && first->vm_start < cand;
    bool need_end   = last && last->vm_end > end;
    int need = (need_start ? 1 : 0) + (need_end ? 1 : 0);

    vma_t *split[2] = { NULL, NULL };
    for (int i = 0; i < need; i++) {
        split[i] = kmalloc(sizeof(vma_t));
        if (!split[i]) {
            for (int j = 0; j < i; j++)
                kfree(split[j]);
            kfree(v);
            mmap_release_transient(file_node, backing);
            MMAP_RETURN(-ENOMEM);
        }
    }

    /* Policy guard: only mmap-window VMAs may be replaced (heap, IO
     * and image metadata live outside the window; the guard is
     * defense against a future VMA kind entering it).  Holes inside
     * the fixed range are legal; the walk covers the WHOLE overlap
     * span — a lookup-based walk would stop at the first hole and let
     * the commit below destroy a VMA the guard never saw (final
     * review 2026-11-19). */
    for (list_t *gp = mm->vma_list.next; gp != &mm->vma_list;
         gp = gp->next) {
        vma_t *o = container_of(gp, vma_t, list);
        if (o->vm_end <= cand)
            continue;
        if (o->vm_start >= end)
            break;
        if (!policy_vma_is_mmap(o)) {
            for (int j = 0; j < need; j++)
                kfree(split[j]);
            kfree(v);
        mmap_release_transient(file_node, backing);
            MMAP_RETURN(-EINVAL);
        }
    }

    uint64_t *root = aarch64_user_vm_root_va(mm);
    for (list_t *lp = mm->resident_list.next; lp; lp = lp->next) {
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va < cand) continue;
        if (r->va >= end) break;
        resident_verify(mm, root, r);
    }

    /* Commit: leaves first (mirrors munmap), then the VMA surgery. */
    list_t *lp = mm->resident_list.next;
    while (lp) {
        list_t *next = lp->next;
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va >= cand && r->va < end)
            resident_remove_leaf(mm, r, root);
        lp = next;
    }

    int si = 0;
    first = first_overlap(mm, cand, end);
    if (first && first->vm_start < cand)
        split_policy_vma(mm, first, cand, split[si++]);
    last = last_overlap(mm, cand, end);
    if (last && last->vm_end > end)
        split_policy_vma(mm, last, end, split[si++]);

    lp = mm->vma_list.next;
    while (lp != &mm->vma_list) {
        list_t *next = lp->next;
        vma_t *o = container_of(lp, vma_t, list);
        if (o->vm_end > cand && o->vm_start < end) {
            aarch64_user_vm_vma_put_refs(o);
            vm_area_unlink(o);
            kfree(o);
        }
        lp = next;
    }

    vm_area_insert_sorted(mm, v);
    MMAP_RETURN((int64_t)cand);

#undef MMAP_RETURN
}

/* ── mprotect ──────────────────────────────────────────────────── */

int64_t user_vm_mprotect(mm_t *mm, uint64_t addr, uint64_t len,
                         uint64_t prot)
{
    if (!mm || len == 0)
        return -EINVAL;
    if (addr & ((uint64_t)PAGE_4K_SIZE - 1))
        return -EINVAL;

    uint64_t alen = page_align_up(len);
    if (alen < len)
        return -EINVAL;
    if (addr < USER_VM_MMAP_BASE || addr >= AARCH64_USER_LIMIT ||
        alen > AARCH64_USER_LIMIT - addr)
        return -EINVAL;
    uint64_t end = addr + alen;

    uint64_t new_flags = 0;
    int rc = prot_to_vma_flags(prot, &new_flags);
    if (rc)
        return rc;

    /* Shared-file ceiling (inode MAP_SHARED): a request that adds
     * write permission is validated against EVERY covered VMA before
     * any split allocation or leaf change — one segment without the
     * recorded maximum-write permission refuses the whole range
     * atomically. */
    if (new_flags & VMA_PROT_WRITE) {
        uint64_t cp = addr;
        while (cp < end) {
            vma_t *v = vm_area_lookup(mm, cp);
            if (!v || !policy_vma_is_mmap(v) || v->vm_end <= cp)
                return -ENOMEM;
            if ((v->vm_flags & VMA_SHARED) && v->vm_shared_backing &&
                !(v->vm_flags & VMA_SHARED_MAYWRITE))
                return -EACCES;
            cp = v->vm_end;
        }
    }

    /* Preflight: the whole range must be covered contiguously by
     * anonymous mmap VMAs (holes are -ENOMEM). */
    uint64_t p = addr;
    while (p < end) {
        vma_t *v = vm_area_lookup(mm, p);
        if (!v || !policy_vma_is_mmap(v) || v->vm_end <= p)
            return -ENOMEM;
        p = v->vm_end;
    }

    vma_t *first = first_overlap(mm, addr, end);
    vma_t *last  = last_overlap(mm, addr, end);
    if (!first || !last)
        return -ENOMEM;

    bool need_start = first->vm_start < addr;
    bool need_end   = last->vm_end > end;
    int need = (need_start ? 1 : 0) + (need_end ? 1 : 0);

    vma_t *split[2] = { NULL, NULL };
    for (int i = 0; i < need; i++) {
        split[i] = kmalloc(sizeof(vma_t));
        if (!split[i]) {
            for (int j = 0; j < i; j++)
                kfree(split[j]);
            return -ENOMEM;
        }
    }

    /* Every resident record inside the range must already agree with
     * its leaf before the first mutation. */
    uint64_t *root = aarch64_user_vm_root_va(mm);
    for (list_t *lp = mm->resident_list.next; lp; lp = lp->next) {
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va < addr) continue;
        if (r->va >= end) break;
        resident_verify(mm, root, r);
    }

    /* Commit. */
    int si = 0;
    if (need_start)
        split_policy_vma(mm, first, addr, split[si++]);
    last = last_overlap(mm, addr, end);
    if (need_end)
        split_policy_vma(mm, last, end, split[si++]);

    for (list_t *lp = mm->vma_list.next; lp != &mm->vma_list;
         lp = lp->next) {
        vma_t *v = container_of(lp, vma_t, list);
        if (v->vm_end <= addr) continue;
        if (v->vm_start >= end) break;
        v->vm_flags &= ~(uint64_t)USER_VM_PROT_MASK;
        v->vm_flags |= new_flags;
    }

    /* The logical permission is committed with the VMA: a PROT_NONE
     * request keeps the PA in a stash, a read-only request is RO/NX,
     * and a restored write permission on a still-shared page keeps the
     * hardware RO/NX — the first real write does the COW.  A private
     * page takes the requested hardware word directly. */
    uint32_t logical = target_vm(prot);
    bool need_icache_sync = false;
    for (list_t *lp = mm->resident_list.next; lp; lp = lp->next) {
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va < addr) continue;
        if (r->va >= end) break;

        uint32_t hw = logical;
        if (r->share == USER_VM_PAGE_SHARED && prot != PROT_NONE &&
            (prot & PROT_WRITE))
            hw = VM_USER_RO | VM_NO_EXEC;

        uint32_t old_vm = 0;
        if (arch_vmm_update_4k(root, r->pa, r->va, hw,
                               NULL, &old_vm) != 0)
            arch_user_vm_invariant("mprotect leaf update failed");
        if (!(hw & VM_NO_EXEC) && (old_vm & VM_NO_EXEC)) {
            aarch64_elf_clean_code_page(
                (const unsigned char *)Phy_To_Virt(r->pa));
            need_icache_sync = true;
        }
        r->logical = logical;
    }
    if (need_icache_sync)
        aarch64_elf_sync_icache();
    return 0;
}

/* ── munmap ────────────────────────────────────────────────────── */

int64_t user_vm_munmap(mm_t *mm, uint64_t addr, uint64_t len)
{
    if (!mm || len == 0)
        return -EINVAL;
    if (addr & ((uint64_t)PAGE_4K_SIZE - 1))
        return -EINVAL;

    uint64_t alen = page_align_up(len);
    if (alen < len)
        return -EINVAL;
    if (addr < USER_VM_MMAP_BASE || addr >= AARCH64_USER_LIMIT ||
        alen > AARCH64_USER_LIMIT - addr)
        return -EINVAL;
    uint64_t end = addr + alen;

    vma_t *first = first_overlap(mm, addr, end);
    vma_t *last  = last_overlap(mm, addr, end);

    bool need_start = first && first->vm_start < addr;
    bool need_end   = last && last->vm_end > end;
    int need = (need_start ? 1 : 0) + (need_end ? 1 : 0);

    vma_t *split[2] = { NULL, NULL };
    for (int i = 0; i < need; i++) {
        split[i] = kmalloc(sizeof(vma_t));
        if (!split[i]) {
            for (int j = 0; j < i; j++)
                kfree(split[j]);
            return -ENOMEM;
        }
    }

    uint64_t *root = aarch64_user_vm_root_va(mm);
    for (list_t *lp = mm->resident_list.next; lp; lp = lp->next) {
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va < addr) continue;
        if (r->va >= end) break;
        resident_verify(mm, root, r);
    }

    /* Commit leaves first: the VMA split preallocation is already
     * done, so nothing below can fail with -ENOMEM. */
    list_t *lp = mm->resident_list.next;
    while (lp) {
        list_t *next = lp->next;
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va >= addr && r->va < end)
            resident_remove_leaf(mm, r, root);
        lp = next;
    }

    int si = 0;
    first = first_overlap(mm, addr, end);
    if (first && first->vm_start < addr)
        split_policy_vma(mm, first, addr, split[si++]);
    last = last_overlap(mm, addr, end);
    if (last && last->vm_end > end)
        split_policy_vma(mm, last, end, split[si++]);

    lp = mm->vma_list.next;
    while (lp != &mm->vma_list) {
        list_t *next = lp->next;
        vma_t *v = container_of(lp, vma_t, list);
        if (v->vm_end > addr && v->vm_start < end) {
            aarch64_user_vm_vma_put_refs(v);
            vm_area_unlink(v);
            kfree(v);
        }
        lp = next;
    }
    return 0;
}

/* ── Demand paging ─────────────────────────────────────────────── */

/* Unified absent-page instantiation for the CPU fault and the uaccess
 * copy pass.  The page is PREPARED before the leaf is installed:
 *
 *   - a VMA holding a shared object borrows the object page for
 *     vm_pgoff + ((va - vm_start) >> PAGE_4K_SHIFT) — the object owns
 *     the PA, the record merely borrows it and stays alive as long as
 *     the object reference does;
 *   - otherwise a private page is allocated, zero-filled and (for a
 *     file mapping) filled from the VFS at the mapping's file offset
 *     under the original rules (short read keeps the zero fill).
 *
 * Errors before the leaf install reclaim exactly this call's
 * resources; a MAP failure releases only the temporary record — an
 * object page already published keeps serving the other mappings.
 * *out is written only on success and the record is NOT on any chain
 * yet (the fault path inserts it into the resident chain, copy
 * pre-instantiation hangs it on the rollback chain). */
static int instantiate_absent(mm_t *mm, uint64_t va, uint32_t map_vm,
                              user_vm_page_t **out)
{
    uint64_t *root = aarch64_user_vm_root_va(mm);
    vma_t *v = vm_area_lookup(mm, va);

    user_vm_page_t *rec = kmalloc(sizeof(*rec));
    if (!rec)
        return -ENOMEM;
    rec->va      = va;
    rec->logical = map_vm;
    rec->share   = USER_VM_PAGE_PRIVATE;
    rec->list.prev = NULL;
    rec->list.next = NULL;

    if (v && v->vm_shared_backing) {
        uint64_t index = v->vm_pgoff +
                         ((va - v->vm_start) >> PAGE_4K_SHIFT);
        uint64_t pa = 0;
        int rc = shared_backing_page(v->vm_shared_backing, index, &pa);
        if (rc != 0) {
            kfree(rec);
            return rc;
        }
        rec->source = USER_VM_PAGE_BACKING;
        rec->pa = pa;
    } else {
        uint64_t pa = alloc_4k_page();
        if (!pa) {
            kfree(rec);
            return -ENOMEM;
        }
        memset(Phy_To_Virt(pa), 0, PAGE_4K_SIZE);
        if (v && v->vm_file) {
            uint64_t file_off = (v->vm_pgoff << PAGE_4K_SHIFT) +
                                (va - v->vm_start);
            int n = vfs_read(v->vm_file, file_off, PAGE_4K_SIZE,
                             Phy_To_Virt(pa));
            if (n < 0) {
                free_4k_page(pa);
                kfree(rec);
                return -EFAULT;
            }
        }
        rec->source = USER_VM_PAGE_ANON;
        rec->pa = pa;
    }

    if (arch_vmm_map_4k_new(root, rec->pa, va, map_vm) != 0) {
        /* The published object page keeps serving other mappings —
         * only this temporary record is released. */
        if (rec->source == USER_VM_PAGE_ANON)
            free_4k_page(rec->pa);
        kfree(rec);
        return -ENOMEM;
    }

    /* First wiring of an executable leaf (kept for parity with the
     * original CPU-fault fill; the current policy never grants X). */
    if (!(map_vm & VM_NO_EXEC)) {
        aarch64_elf_clean_code_page(
            (const unsigned char *)Phy_To_Virt(rec->pa));
        aarch64_elf_sync_icache();
    }
    *out = rec;
    return 0;
}

int user_vm_fault(mm_t *mm, uint64_t far, bool write)
{
    if (!mm)
        return -EFAULT;

    uint64_t va = far & ~((uint64_t)PAGE_4K_SIZE - 1);
    if (va < AARCH64_USER_MIN || va >= AARCH64_USER_LIMIT)
        return -EFAULT;

    uint32_t map_vm = 0;
    if (authorize_absent(mm, va, write, &map_vm) != 0)
        return -EFAULT;

    uint64_t *root = aarch64_user_vm_root_va(mm);

    /* Only a truly absent leaf may be instantiated: a present leaf
     * means the hardware abort was not a demand fault, and a stash is
     * PROT_NONE (the old termination path owns both). */
    struct user_vm_leaf leaf;
    if (arch_user_vm_query(root, va, &leaf) != 0 ||
        leaf.state != USER_VM_ABSENT)
        return -EFAULT;

    user_vm_page_t *rec = NULL;
    int rc = instantiate_absent(mm, va, map_vm, &rec);
    if (rc != 0)
        return rc;

    aarch64_user_vm_resident_insert(mm, rec);
    return 0;
}

/* ── Copy pre-instantiation ────────────────────────────────────── */

static void staged_append(list_t *head, user_vm_page_t *r)
{
    r->list.prev = head->prev;
    r->list.next = NULL;
    if (head->prev)
        head->prev->next = &r->list;
    else
        head->next = &r->list;
    head->prev = &r->list;
}

/* Undo exactly the leaves this call created: the chain holds their
 * records, so each PA is removed, verified and freed once — except
 * object pages, whose PA stays with the shared backing. */
static void staged_rollback(uint64_t *root, list_t *head)
{
    while (head->next) {
        list_t *n = head->next;
        user_vm_page_t *r = container_of(n, user_vm_page_t, list);
        head->next = n->next;
        if (head->next)
            head->next->prev = NULL;
        else
            head->prev = NULL;

        struct user_vm_leaf leaf;
        int rc = arch_user_vm_remove(root, r->va, &leaf);
        if (rc != 0 || leaf.state == USER_VM_ABSENT || leaf.pa != r->pa)
            arch_user_vm_invariant("copy rollback mismatch");
        if (r->source != USER_VM_PAGE_BACKING)
            free_4k_page(leaf.pa);
        kfree(r);
    }
}

struct copy_cow_stage {
    user_vm_page_t *rec;
    uint64_t        old_pa;
    uint64_t        new_pa;   /* 0: unique-reference same-PA upgrade */
};

static int user_vm_validate_copy_range(mm_t *mm, uint64_t addr, size_t len,
                                       bool write, size_t *cow_count_out)
{
    if (!mm)
        return -EFAULT;
    if (len == 0) {
        if (cow_count_out)
            *cow_count_out = 0;
        return 0;
    }
    if (addr < AARCH64_USER_MIN || addr >= AARCH64_USER_LIMIT)
        return -EFAULT;
    if ((uint64_t)len > AARCH64_USER_LIMIT - addr)
        return -EFAULT;

    uint64_t start = addr & ~((uint64_t)PAGE_4K_SIZE - 1);
    uint64_t end   = page_align_up(addr + (uint64_t)len);
    uint64_t *root = aarch64_user_vm_root_va(mm);

    /* Complete read-only validation.  A write range accepts a writable
     * leaf, a legal absent page, or a shared logically-RW page whose
     * hardware leaf is RO/NX (the COW target).  Logical RO and every
     * non-leaf-level shape are rejected before anything is allocated
     * or changed. */
    size_t cow_count = 0;
    for (uint64_t va = start; va < end; va += PAGE_4K_SIZE) {
        struct user_vm_leaf leaf;
        if (arch_user_vm_query(root, va, &leaf) != 0)
            return -EFAULT;

        if (leaf.state == USER_VM_PRESENT) {
            if (!write) {
                if (!leaf_readable(leaf.flags))
                    return -EFAULT;
                continue;
            }
            if (leaf_writable(leaf.flags))
                continue;
            if (leaf.flags != (VM_USER_RO | VM_NO_EXEC))
                return -EFAULT;
            user_vm_page_t *r = resident_lookup(mm, va);
            if (!r || r->share != USER_VM_PAGE_SHARED ||
                !(r->logical & VM_WRITE) || r->pa != leaf.pa)
                return -EFAULT;
            cow_count++;
        } else if (leaf.state == USER_VM_STASHED) {
            return -EFAULT;
        } else if (authorize_absent(mm, va, write, NULL) != 0) {
            return -EFAULT;
        }
    }

    if (cow_count_out)
        *cow_count_out = cow_count;
    return 0;
}

bool user_vm_range_accessible(mm_t *mm, uint64_t addr, size_t len, bool write)
{
    return user_vm_validate_copy_range(mm, addr, len, write, NULL) == 0;
}

int user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len, bool write)
{
    if (!mm)
        return -EFAULT;
    if (len == 0)
        return 0;

    /* Pass 1 — complete read-only validation. */
    size_t cow_count = 0;
    int rc = user_vm_validate_copy_range(mm, addr, len, write, &cow_count);
    if (rc != 0)
        return rc;

    uint64_t start = addr & ~((uint64_t)PAGE_4K_SIZE - 1);
    uint64_t end   = page_align_up(addr + (uint64_t)len);
    uint64_t *root = aarch64_user_vm_root_va(mm);

    if (cow_count > SIZE_MAX / sizeof(struct copy_cow_stage))
        return -ENOMEM;
    struct copy_cow_stage *cow = NULL;
    if (cow_count) {
        cow = kmalloc(cow_count * sizeof(*cow));
        if (!cow)
            return -ENOMEM;
    }

    /* The rollback chain's NULL-terminated head exists from before
     * the first `goto fail` below: pass 2's allocation failures must
     * reach the fail label with an EMPTY chain, never an
     * uninitialized one. */
    list_t staged = { .prev = NULL, .next = NULL };

    /* Pass 2 — preallocate every multi-reference private copy and log
     * the unique-reference upgrades.  The COW leaves and their counts
     * are untouched until the whole range is ready to commit. */
    size_t ncow = 0;
    for (uint64_t va = start; write && va < end; va += PAGE_4K_SIZE) {
        struct user_vm_leaf leaf;
        if (arch_user_vm_query(root, va, &leaf) != 0) {
            rc = -EFAULT;
            goto fail;
        }
        if (leaf.state != USER_VM_PRESENT || leaf_writable(leaf.flags))
            continue;

        user_vm_page_t *r = resident_lookup(mm, va);
        if (!r || r->share != USER_VM_PAGE_SHARED ||
            !(r->logical & VM_WRITE) || r->pa != leaf.pa ||
            leaf.flags != (VM_USER_RO | VM_NO_EXEC)) {
            rc = -EFAULT;
            goto fail;
        }

        uint16_t refs = page_cow_refs(r->pa);
        if (refs == 0)
            arch_user_vm_invariant("copy COW without a reference");

        uint64_t new_pa = 0;
        if (refs > 1) {
            new_pa = alloc_4k_page();
            if (!new_pa) {
                rc = -ENOMEM;
                goto fail;
            }
            memcpy(Phy_To_Virt(new_pa), Phy_To_Virt(r->pa), PAGE_4K_SIZE);
        }
        cow[ncow++] = (struct copy_cow_stage){ r, r->pa, new_pa };
    }

    /* Pass 3 — create the absent pages on the rollback chain
     * initialized before pass 2.  A map failure removes every
     * temporary leaf and page but leaves intermediate tables for mm
     * destroy to reclaim. */
    for (uint64_t va = start; va < end; va += PAGE_4K_SIZE) {
        struct user_vm_leaf leaf;
        if (arch_user_vm_query(root, va, &leaf) != 0) {
            rc = -EFAULT;
            goto fail;
        }
        if (leaf.state != USER_VM_ABSENT)
            continue;

        uint32_t map_vm = 0;
        if (authorize_absent(mm, va, write, &map_vm) != 0) {
            rc = -EFAULT;
            goto fail;
        }

        user_vm_page_t *rec = NULL;
        int irc = instantiate_absent(mm, va, map_vm, &rec);
        if (irc != 0) {
            rc = irc;
            goto fail;
        }
        staged_append(&staged, rec);
    }

    /* Pass 4 — commit the COW leaves and the reference accounting.  A
     * commit-time mismatch is an invariant failure, never a repair. */
    for (size_t i = 0; i < ncow; i++) {
        user_vm_page_t *r = cow[i].rec;

        if (cow[i].new_pa) {
            if (arch_user_vm_replace_cow(root, r->va, cow[i].old_pa,
                                         cow[i].new_pa) != 0)
                arch_user_vm_invariant("copy replace mismatch");
            bool last = false;
            if (pmm_cow_release(cow[i].old_pa, &last) != 0)
                arch_user_vm_invariant("copy replace release mismatch");
            if (last)
                free_4k_page(cow[i].old_pa);
            r->pa = cow[i].new_pa;
            r->share = USER_VM_PAGE_PRIVATE;
        } else {
            uint64_t old_pa = 0;
            uint32_t old_vm = 0;
            if (arch_vmm_update_4k(root, r->pa, r->va,
                                   VM_USER_RW | VM_NO_EXEC,
                                   &old_pa, &old_vm) != 0 ||
                old_pa != r->pa ||
                old_vm != (VM_USER_RO | VM_NO_EXEC))
                arch_user_vm_invariant("copy upgrade mismatch");
            bool last = false;
            if (pmm_cow_release(r->pa, &last) != 0 || !last)
                arch_user_vm_invariant("copy unique release mismatch");
            r->share = USER_VM_PAGE_PRIVATE;
        }
    }

    /* Success: attach the staged records to the mm chain. */
    while (staged.next) {
        list_t *n = staged.next;
        user_vm_page_t *r = container_of(n, user_vm_page_t, list);
        staged.next = n->next;
        if (staged.next)
            staged.next->prev = NULL;
        else
            staged.prev = NULL;
        aarch64_user_vm_resident_insert(mm, r);
    }
    kfree(cow);
    return 0;

fail:
    /* Free only this call's preallocated copies and temporary leaves;
     * the original COW leaves and counts are exactly as they were. */
    for (size_t i = 0; i < ncow; i++)
        if (cow[i].new_pa)
            free_4k_page(cow[i].new_pa);
    staged_rollback(root, &staged);
    kfree(cow);
    return rc;
}
