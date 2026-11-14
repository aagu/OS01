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
#include <kernel.h>             /* container_of */
#include <memory/memory.h>      /* Phy_To_Virt */
#include <memory/pmm.h>         /* alloc_4k_page / free_4k_page / PAGE_4K_* */
#include <memory/slab.h>        /* kmalloc / kfree */
#include <memory/user_vm.h>            /* leaf adapter (arch_user_vm_*) */
#include <arch/aarch64/user_vm_policy.h>  /* this TU's policy API */
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

/* mm->pgdir holds the root PA; the M3 entries take the direct-map
 * pointer. */
static uint64_t *vm_root(mm_t *mm)
{
    return (uint64_t *)Phy_To_Virt((uintptr_t)mm->pgdir);
}

static bool policy_vma_is_mmap(const vma_t *v)
{
    return (v->vm_flags & VMA_ANON) &&
           !(v->vm_flags & (VMA_HEAP | VMA_IO)) &&
           v->vm_file == NULL;
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

static void resident_insert(mm_t *mm, user_vm_page_t *node)
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

static void resident_unlink(mm_t *mm, user_vm_page_t *node)
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

/* Commit-time removal: remove the leaf, verify the PA the adapter
 * hands back, drop the record's reference (freeing on the last one),
 * then unlink and free the record. */
static void resident_remove_leaf(mm_t *mm, user_vm_page_t *r, uint64_t *root)
{
    struct user_vm_leaf leaf;
    int rc = arch_user_vm_remove(root, r->va, &leaf);
    if (rc != 0 || leaf.state == USER_VM_ABSENT || leaf.pa != r->pa)
        arch_user_vm_invariant("resident remove mismatch");
    resident_drop_pa(leaf.pa, r->share);
    resident_unlink(mm, r);
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

    int rc = arch_user_vm_replace_cow(root, r->va, r->pa, new_pa);
    if (rc != 0) {
        free_4k_page(new_pa);
        return (rc == -ENOMEM) ? -ENOMEM : -EFAULT;
    }

    bool last = false;
    if (pmm_cow_release(r->pa, &last) != 0 || last)
        arch_user_vm_invariant("cow copy release mismatch");

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

    /* cow_private_page performs the remaining checks (shared, logical
     * RW, same-PA RO/NX leaf) and returns -EFAULT for a non-COW
     * fault, -ENOMEM for copy exhaustion. */
    return cow_private_page(mm, r, vm_root(mm));
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
    right->vm_pgoff     = v->vm_pgoff;
    right->vm_file      = NULL;
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

    int rc = arch_vmm_map_4k_new(vm_root(mm), pa, va, flags);
    if (rc) {
        kfree(rec);             /* PA still belongs to the caller */
        return rc;
    }

    resident_insert(mm, rec);
    return 0;
}

bool user_vm_has_metadata(const mm_t *mm)
{
    if (!mm)
        return false;
    return mm->resident_list.next != NULL ||
           mm->vma_list.next != &mm->vma_list;
}

/* ── fork clone transaction ─────────────────────────────────────── */

/* One staged page: the parent snapshot and the child record/mapping.
 * The flags below are the log of what this call actually completed,
 * so rollback undoes exactly that much — never more. */
struct clone_stage {
    struct clone_stage *next;
    user_vm_page_t    *parent;
    user_vm_page_t    *child;
    struct user_vm_leaf parent_leaf;   /* PRESENT/STASHED snapshot */
    uint16_t           old_refs;
    bool               first_share;
    bool               acquired;
    bool               parent_downgraded;
    bool               child_mapped;
    bool               child_inserted;
};

static void clone_stages_free(struct clone_stage *s)
{
    while (s) {
        struct clone_stage *next = s->next;
        if (s->child && !s->child_inserted)
            kfree(s->child);
        kfree(s);
        s = next;
    }
}

static void clone_child_vmas_free(mm_t *child)
{
    while (child->vma_list.next != &child->vma_list) {
        vma_t *v = container_of(child->vma_list.next, vma_t, list);
        vm_area_unlink(v);
        kfree(v);
    }
}

/* Borrowed child leaves: remove them and verify the PA.  The PA is
 * never freed here — the parent still owns it. */
static void clone_unmap_children(struct clone_stage *s, uint64_t *child_root)
{
    for (; s; s = s->next) {
        if (!s->child_mapped)
            continue;
        struct user_vm_leaf leaf;
        int rc = arch_user_vm_remove(child_root, s->child->va, &leaf);
        if (rc != 0 || leaf.state == USER_VM_ABSENT ||
            leaf.pa != s->child->pa)
            arch_user_vm_invariant("fork rollback child leaf mismatch");
        s->child_mapped = false;
    }
}

/* Undo the committed references.  A first share established BOTH
 * references, so it is released twice; an already-shared page only
 * loses the one this fork added. */
static void clone_release_refs(struct clone_stage *s)
{
    for (; s; s = s->next) {
        if (!s->acquired)
            continue;
        bool last = false;
        if (s->first_share) {
            if (pmm_cow_release(s->parent->pa, &last) != 0 || last)
                arch_user_vm_invariant("fork rollback first share");
            if (pmm_cow_release(s->parent->pa, &last) != 0 || !last)
                arch_user_vm_invariant("fork rollback first share");
            s->parent->share = USER_VM_PAGE_PRIVATE;
        } else {
            if (pmm_cow_release(s->parent->pa, &last) != 0)
                arch_user_vm_invariant("fork rollback shared");
        }
        s->acquired = false;
    }
}

/* Restore every parent leaf this fork lowered to RO/NX. */
static void clone_restore_parent_leaves(struct clone_stage *s,
                                       uint64_t *parent_root)
{
    for (; s; s = s->next) {
        if (!s->parent_downgraded)
            continue;
        uint64_t old_pa = 0;
        uint32_t old_vm = 0;
        int rc = arch_vmm_update_4k(parent_root, s->parent->pa,
                                    s->parent->va, VM_USER_RW | VM_NO_EXEC,
                                    &old_pa, &old_vm);
        if (rc != 0 || old_pa != s->parent->pa ||
            old_vm != (VM_USER_RO | VM_NO_EXEC))
            arch_user_vm_invariant("fork rollback parent restore mismatch");
        s->parent_downgraded = false;
    }
}

static void clone_remove_child_records(mm_t *child, struct clone_stage *s)
{
    for (; s; s = s->next) {
        if (!s->child_inserted)
            continue;
        resident_unlink(child, s->child);
        s->child_inserted = false;
        kfree(s->child);
        s->child = NULL;
    }
}

/* Permission word for the child's first mapping.  A stash needs a
 * temporarily valid word (map_new refuses VM_PROTNONE and the entry is
 * an update-only state); a logical-RW page starts read-only; RO and
 * RX keep their exact flags.  Non-resident pages are never filled. */
static uint32_t clone_child_vm(const struct user_vm_leaf *leaf)
{
    if (leaf->state == USER_VM_STASHED)
        return VM_USER_RO | VM_NO_EXEC;
    if (!(leaf->flags & VM_WRITE))
        return leaf->flags;
    return VM_USER_RO | VM_NO_EXEC;
}

static void clone_child_clear(mm_t *child)
{
    clone_child_vmas_free(child);
    child->resident_list.next = NULL;
    child->resident_list.prev = NULL;
    child->start_code   = child->end_code   = 0;
    child->start_data   = child->end_data   = 0;
    child->start_rodata = child->end_rodata = 0;
    child->start_brk    = child->end_brk    = 0;
    child->start_stack  = 0;
    child->mmap_base    = 0;
}

int user_vm_clone_fork(mm_t *parent, mm_t *child, int (*publish)(void *),
                       void *arg)
{
    if (!parent || !child || !publish || parent == child)
        return -EINVAL;
    if (!parent->pgdir || !child->pgdir)
        return -EINVAL;
    if (child->resident_list.next != NULL ||
        !list_is_empty(&child->vma_list))
        return -EINVAL;

    uint64_t *parent_root = vm_root(parent);
    uint64_t *child_root  = vm_root(child);
    int rc = 0;

    /* Declared before the first fallible step: every failure path below
     * rolls back through this log, including a VMA-copy failure. */
    struct clone_stage *stages = NULL;
    struct clone_stage **tail = &stages;

    /* Copy the address layout, then every VMA node. */
    child->start_code   = parent->start_code;
    child->end_code     = parent->end_code;
    child->start_data   = parent->start_data;
    child->end_data     = parent->end_data;
    child->start_rodata = parent->start_rodata;
    child->end_rodata   = parent->end_rodata;
    child->start_brk    = parent->start_brk;
    child->end_brk      = parent->end_brk;
    child->start_stack  = parent->start_stack;
    child->mmap_base    = parent->mmap_base;

    for (list_t *p = parent->vma_list.next; p != &parent->vma_list;
         p = p->next) {
        vma_t *pv = container_of(p, vma_t, list);
        vma_t *cv = kmalloc(sizeof(*cv));
        if (!cv) {
            rc = -ENOMEM;
            goto fail;
        }
        *cv = *pv;
        list_init(&cv->list);
        vm_area_insert_sorted(child, cv);
    }

    /* Borrow phase: snapshot each parent leaf and map the child leaf
     * at the same PA.  No PMM reference exists yet. */
    for (list_t *p = parent->resident_list.next; p; p = p->next) {
        user_vm_page_t *pr = container_of(p, user_vm_page_t, list);

        struct clone_stage *st = kmalloc(sizeof(*st));
        user_vm_page_t *cr = kmalloc(sizeof(*cr));
        if (!st || !cr) {
            kfree(st);
            kfree(cr);
            rc = -ENOMEM;
            goto fail;
        }
        __builtin_memset(st, 0, sizeof(*st));
        st->parent = pr;
        st->child = cr;
        cr->va        = pr->va;
        cr->pa        = pr->pa;
        cr->source    = pr->source;
        cr->logical   = pr->logical;
        cr->share     = USER_VM_PAGE_PRIVATE;   /* until commit */
        cr->list.prev = NULL;
        cr->list.next = NULL;
        *tail = st;
        tail = &st->next;

        if (arch_user_vm_query(parent_root, pr->va, &st->parent_leaf) != 0 ||
            st->parent_leaf.state == USER_VM_ABSENT ||
            st->parent_leaf.pa != pr->pa) {
            rc = -EINVAL;
            goto fail;
        }

        st->old_refs = page_cow_refs(pr->pa);
        if (pr->share == USER_VM_PAGE_PRIVATE) {
            if (st->old_refs != 0) {
                rc = -EINVAL;
                goto fail;
            }
            st->first_share = true;
        } else if (pr->share == USER_VM_PAGE_SHARED) {
            if (st->old_refs == 0) {
                rc = -EINVAL;
                goto fail;
            }
            if (st->old_refs == UINT16_MAX) {
                rc = -EOVERFLOW;
                goto fail;
            }
            /* A shared writable leaf violates the hardware contract. */
            if (st->parent_leaf.state == USER_VM_PRESENT &&
                (st->parent_leaf.flags & VM_WRITE)) {
                rc = -EINVAL;
                goto fail;
            }
        } else {
            rc = -EINVAL;
            goto fail;
        }

        rc = arch_vmm_map_4k_new(child_root, pr->pa, pr->va,
                                 clone_child_vm(&st->parent_leaf));
        if (rc)
            goto fail;
        st->child_mapped = true;

        if (st->parent_leaf.state == USER_VM_STASHED) {
            rc = arch_vmm_update_4k(child_root, pr->pa, pr->va,
                                    VM_PROTNONE, NULL, NULL);
            if (rc)
                goto fail;
        }
    }

    /* Commit: one checked reference per page, then lower each parent
     * writable leaf.  A commit-time mismatch halts the kernel. */
    for (struct clone_stage *st = stages; st; st = st->next) {
        rc = pmm_cow_acquire(st->parent->pa, st->first_share);
        if (rc)
            goto fail;
        st->acquired = true;
        st->parent->share = USER_VM_PAGE_SHARED;
        st->child->share  = USER_VM_PAGE_SHARED;

        if (st->parent_leaf.state == USER_VM_PRESENT &&
            (st->parent_leaf.flags & VM_WRITE)) {
            uint64_t old_pa = 0;
            uint32_t old_vm = 0;
            int urc = arch_vmm_update_4k(parent_root, st->parent->pa,
                                         st->parent->va,
                                         VM_USER_RO | VM_NO_EXEC,
                                         &old_pa, &old_vm);
            if (urc != 0 || old_pa != st->parent->pa ||
                old_vm != (VM_USER_RW | VM_NO_EXEC))
                arch_user_vm_invariant("fork parent downgrade mismatch");
            st->parent_downgraded = true;
        }
    }

    /* The child mm is complete; task publication is the last fallible
     * step. */
    for (struct clone_stage *st = stages; st; st = st->next) {
        resident_insert(child, st->child);
        st->child_inserted = true;
    }

    rc = publish(arg);
    if (rc)
        goto fail;

    clone_stages_free(stages);
    return 0;

fail:
    /* Undo exactly what this call completed.  The child leaves are
     * borrowed, so removing them frees no PA; the references, parent
     * permissions and child records follow; the child mm goes back
     * empty for the caller to destroy. */
    clone_unmap_children(stages, child_root);
    clone_release_refs(stages);
    clone_restore_parent_leaves(stages, parent_root);
    clone_remove_child_records(child, stages);
    clone_child_clear(child);
    clone_stages_free(stages);
    return rc;
}

void user_vm_destroy(mm_t *mm)
{
    if (!mm)
        return;

    uint64_t *root = vm_root(mm);

    list_t *p = mm->resident_list.next;
    while (p) {
        list_t *next = p->next;
        user_vm_page_t *r = container_of(p, user_vm_page_t, list);
        resident_remove_leaf(mm, r, root);
        p = next;
    }
    mm->resident_list.next = NULL;
    mm->resident_list.prev = NULL;

    while (mm->vma_list.next != &mm->vma_list) {
        vma_t *v = container_of(mm->vma_list.next, vma_t, list);
        vm_area_unlink(v);
        kfree(v);
    }

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
    uint64_t *root = vm_root(mm);

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

    /* Legally parsed but out of scope for anonymous-private v1:
     * shared, file-backed and fixed mappings all fail here, before the
     * private-anonymous fd/off validation. */
    if (map_type != MAP_PRIVATE || !(flags & MAP_ANONYMOUS) ||
        (flags & MAP_FIXED))
        return -EOPNOTSUPP;

    /* Raw 64-bit fd/off, before any mapping metadata is touched. */
    if (fd != UINT64_MAX || off != 0)
        return -EINVAL;

    if (len == 0)
        return -EINVAL;
    uint64_t alen = page_align_up(len);
    if (alen < len)                                   /* overflow */
        return -EINVAL;
    if (alen > AARCH64_USER_LIMIT - USER_VM_MMAP_BASE)
        return -ENOMEM;

    uint64_t cand = 0;

    if (hint != 0) {
        if ((hint & ((uint64_t)PAGE_4K_SIZE - 1)) ||
            hint < USER_VM_MMAP_BASE || hint >= AARCH64_USER_LIMIT ||
            alen > AARCH64_USER_LIMIT - hint)
            return -EINVAL;
        if (range_free(mm, hint, hint + alen))
            cand = hint;
        /* A valid but occupied hint falls back to the base first-fit. */
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
            alen > AARCH64_USER_LIMIT - cursor)
            return -ENOMEM;
        cand = cursor;
    }

    vma_t *v = kmalloc(sizeof(*v));
    if (!v)
        return -ENOMEM;

    list_init(&v->list);
    v->vm_start     = cand;
    v->vm_end       = cand + alen;
    v->vm_flags     = vma_flags | VMA_ANON;
    v->vm_pgoff     = 0;
    v->vm_file      = NULL;
    vm_area_insert_sorted(mm, v);
    return (int64_t)cand;
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
    uint64_t *root = vm_root(mm);
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
    for (list_t *lp = mm->resident_list.next; lp; lp = lp->next) {
        user_vm_page_t *r = container_of(lp, user_vm_page_t, list);
        if (r->va < addr) continue;
        if (r->va >= end) break;

        uint32_t hw = logical;
        if (r->share == USER_VM_PAGE_SHARED && prot != PROT_NONE &&
            (prot & PROT_WRITE))
            hw = VM_USER_RO | VM_NO_EXEC;

        if (arch_vmm_update_4k(root, r->pa, r->va, hw,
                               NULL, NULL) != 0)
            arch_user_vm_invariant("mprotect leaf update failed");
        r->logical = logical;
    }
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

    uint64_t *root = vm_root(mm);
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
            vm_area_unlink(v);
            kfree(v);
        }
        lp = next;
    }
    return 0;
}

/* ── Demand paging ─────────────────────────────────────────────── */

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

    uint64_t *root = vm_root(mm);

    /* Only a truly absent leaf may be instantiated: a present leaf
     * means the hardware abort was not a demand fault, and a stash is
     * PROT_NONE (the old termination path owns both). */
    struct user_vm_leaf leaf;
    if (arch_user_vm_query(root, va, &leaf) != 0 ||
        leaf.state != USER_VM_ABSENT)
        return -EFAULT;

    user_vm_page_t *rec = kmalloc(sizeof(*rec));
    if (!rec)
        return -ENOMEM;

    uint64_t pa = alloc_4k_page();
    if (!pa) {
        kfree(rec);
        return -ENOMEM;
    }
    memset(Phy_To_Virt(pa), 0, PAGE_4K_SIZE);

    if (arch_vmm_map_4k_new(root, pa, va, map_vm) != 0) {
        free_4k_page(pa);
        kfree(rec);
        return -ENOMEM;
    }

    rec->va = va;
    rec->pa = pa;
    rec->source  = USER_VM_PAGE_ANON;
    rec->logical = map_vm;
    rec->share   = USER_VM_PAGE_PRIVATE;
    rec->list.prev = NULL;
    rec->list.next = NULL;
    resident_insert(mm, rec);
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
 * records, so each PA is removed, verified and freed once. */
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
        free_4k_page(leaf.pa);
        kfree(r);
    }
}

struct copy_cow_stage {
    user_vm_page_t *rec;
    uint64_t        old_pa;
    uint64_t        new_pa;   /* 0: unique-reference same-PA upgrade */
};

int user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len, bool write)
{
    if (!mm)
        return -EFAULT;
    if (len == 0)
        return 0;
    if (addr < AARCH64_USER_MIN || addr >= AARCH64_USER_LIMIT)
        return -EFAULT;
    if ((uint64_t)len > AARCH64_USER_LIMIT - addr)
        return -EFAULT;

    uint64_t start = addr & ~((uint64_t)PAGE_4K_SIZE - 1);
    uint64_t end   = page_align_up(addr + (uint64_t)len);
    uint64_t *root = vm_root(mm);
    int rc;

    /* Pass 1 — complete read-only validation.  A write range accepts
     * a writable leaf, a legal absent page, or a shared logically-RW
     * page whose hardware leaf is RO/NX (the COW target).  Logical RO
     * and every non-leaf-level shape are rejected before anything is
     * allocated or changed. */
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

    if (cow_count > SIZE_MAX / sizeof(struct copy_cow_stage))
        return -ENOMEM;
    struct copy_cow_stage *cow = NULL;
    if (cow_count) {
        cow = kmalloc(cow_count * sizeof(*cow));
        if (!cow)
            return -ENOMEM;
    }

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

    /* Pass 3 — create the absent pages on an independent rollback
     * chain.  A map failure removes every temporary leaf and page but
     * leaves intermediate tables for mm destroy to reclaim. */
    list_t staged = { .prev = NULL, .next = NULL };

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

        user_vm_page_t *rec = kmalloc(sizeof(*rec));
        if (!rec) {
            rc = -ENOMEM;
            goto fail;
        }

        uint64_t pa = alloc_4k_page();
        if (!pa) {
            kfree(rec);
            rc = -ENOMEM;
            goto fail;
        }
        memset(Phy_To_Virt(pa), 0, PAGE_4K_SIZE);

        if (arch_vmm_map_4k_new(root, pa, va, map_vm) != 0) {
            free_4k_page(pa);
            kfree(rec);
            rc = -ENOMEM;
            goto fail;
        }

        rec->va = va;
        rec->pa = pa;
        rec->source  = USER_VM_PAGE_ANON;
        rec->logical = map_vm;
        rec->share   = USER_VM_PAGE_PRIVATE;
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
            if (pmm_cow_release(cow[i].old_pa, &last) != 0 || last)
                arch_user_vm_invariant("copy replace release mismatch");
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
        resident_insert(mm, r);
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
