/* kernel/arch/aarch64/memory/user_vm_fork.c
 *
 * AArch64 anonymous-VM fork transaction (M4 Task 2 body, split from
 * user_vm_policy.c).
 *
 * Owns user_vm_clone_fork and its clone_* stage/rollback helpers:
 * the borrow phase, the checked COW commit, child publication and the
 * exact-undo rollback.  The resident chain, VMA metadata drain and
 * root accessor are shared with the policy through
 * <arch/aarch64/user_vm_internal.h>; the public contract stays in
 * <arch/aarch64/user_vm_policy.h> (no rollback detail crosses it).
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel.h>                          /* container_of */
#include <list.h>
#include <sched/task.h>                      /* complete mm_t */
#include <memory/pmm.h>                      /* pmm_cow_acquire/release,
                                              * page_cow_refs */
#include <memory/slab.h>                     /* kmalloc / kfree */
#include <memory/vma.h>                      /* complete vma_t */
#include <memory/vm_area.h>                  /* vm_area_insert_sorted,
                                              * list_is_empty */
#include <arch/aarch64/user_vm_policy.h>     /* user_vm_page_t, public fork API */
#include <arch/aarch64/user_vm_internal.h>   /* root/resident/free_vmas */
#include <arch/aarch64/user_vm_leaf.h>       /* leaf adapter tri-state */
#include <arch/aarch64/vmm_backend.h>        /* arch_vmm_map_4k_new/update_4k */

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
        aarch64_user_vm_resident_unlink(child, s->child);
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
    aarch64_user_vm_free_vmas(child);
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

    uint64_t *parent_root = aarch64_user_vm_root_va(parent);
    uint64_t *child_root  = aarch64_user_vm_root_va(child);
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
        aarch64_user_vm_resident_insert(child, st->child);
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
