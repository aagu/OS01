/* kernel/arch/aarch64/memory/user_mm.c — AArch64 M4 mm-owned user root
 * lifecycle.  See <arch/aarch64/user_mm.h> for the public contract and
 * docs/superpowers/specs/2026-11-11-aarch64-m4-user-mm-design.md for the
 * design (§mm 生命周期和所有权).
 *
 * Ownership is a dynamically allocated record per live mm (no fixed
 * slot cap, unlike the M3 published-root registry).  Registration runs
 * under an IRQ-safe lock that covers lookup/insert/remove only — it is
 * released before any M3 page-table operation or page free, per the
 * lock-discipline rule in the spec.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <arch/mmu.h>                     /* Phy_To_Virt, ARCH_PAGE_OFFSET */
#include <arch/spinlock.h>                /* IRQ-safe registration lock */
#include <arch/aarch64/page_table.h>      /* AARCH64_TTBR_BASE_MASK */
#include <arch/aarch64/vmm_backend.h>     /* AARCH64_PT_DESC_{VALID,TABLE} */
#include <arch/aarch64/vmm_gate.h>        /* is_published, runtime gate */
#include <arch/aarch64/user_mm.h>
#include <memory/mm.h>                    /* mm_alloc */
#include <memory/memory.h>                /* PAGE_4K_SIZE */
#include <memory/pmm.h>                   /* alloc/free 4k pages */
#include <memory/slab.h>                  /* kmalloc / kfree */
#include <memory/user_vm.h>               /* policy metadata predicate */
#include <sched/task.h>                   /* mm_t layout + init_mm */
#include <arch/aarch64/user_vm_policy.h>

typedef struct user_mm_record {
    mm_t *mm;
    uint64_t root_pa;
    struct user_mm_record *next;
} user_mm_record_t;

static user_mm_record_t *user_mm_records;
static spinlock_T user_mm_records_lock = { .lock = 1UL };

/* Root PA ⇄ direct-map pointer conversion (spec: pgdir holds the PA,
 * the M3 primitives receive direct-map pointers). */
static inline uint64_t user_mm_ptr_to_pa(const uint64_t *root)
{
    return (uint64_t)(uintptr_t)root - ARCH_PAGE_OFFSET;
}

/* ── Backend-facing predicates ──────────────────────────────────── */

bool aarch64_user_mm_owns_root(const uint64_t *root)
{
    if (!root)
        return false;
    uint64_t pa = user_mm_ptr_to_pa(root);

    bool owned = false;
    uint64_t daif = spin_lock_irqsave(&user_mm_records_lock);
    for (user_mm_record_t *r = user_mm_records; r; r = r->next) {
        if (r->root_pa == pa) {
            owned = true;
            break;
        }
    }
    spin_unlock_irqrestore(&user_mm_records_lock, daif);
    return owned;
}

bool aarch64_user_mm_root_busy(const uint64_t *root)
{
    if (!root)
        return false;
    uint64_t pa = user_mm_ptr_to_pa(root);
    if (!pa)
        return false;

    if (aarch64_pt_root_is_published(&pa))
        return true;
    /* Also busy when it is the LIVE TTBR0 root (installed without the
     * probe runner's publish bookkeeping, e.g. a future installer). */
    uint64_t ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    return (ttbr0 & AARCH64_TTBR_BASE_MASK) == pa;
}

/* ── Tree walks ─────────────────────────────────────────────────── */

/* Read-only sweep of the whole tree: returns 0 only when no slot in
 * any L3 leaf is non-empty, and no L0..L2 slot holds anything but a
 * table descriptor (a block mapping or a PA-stashing invalid leaf is
 * a leaf for this purpose).  Mutates nothing. */
static int user_mm_scan_no_leaves(const uint64_t *table, unsigned level)
{
    const uint64_t k_table =
        AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
    const unsigned n = (unsigned)(PAGE_4K_SIZE / sizeof(*table));

    for (unsigned i = 0; i < n; i++) {
        uint64_t desc = table[i];
        if (!desc)
            continue;
        if (level >= 3)
            return -EBUSY;              /* any L3 leaf (valid or stash) */
        if ((desc & k_table) != k_table)
            return -EBUSY;              /* block / unexpected descriptor */
        int rc = user_mm_scan_no_leaves(
            (const uint64_t *)Phy_To_Virt(desc & AARCH64_TTBR_BASE_MASK),
            level + 1);
        if (rc)
            return rc;
    }
    return 0;
}

/* Second pass — reclaim every intermediate table and the root itself.
 * All intermediate descriptors were allocated by M3 for 4 KiB mappings
 * (same shape as user_root.c's free_tables); leaves belong to the
 * caller and are gone by the time this runs.  Empty tables left by a
 * failed map are reclaimed by the same walk. */
static void user_mm_free_tables(uint64_t *table, unsigned level)
{
    if (level < 3) {
        const uint64_t k_table =
            AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
        const unsigned n = (unsigned)(PAGE_4K_SIZE / sizeof(*table));
        for (unsigned i = 0; i < n; i++) {
            uint64_t desc = table[i];
            if ((desc & k_table) == k_table)
                user_mm_free_tables(
                    (uint64_t *)Phy_To_Virt(desc & AARCH64_TTBR_BASE_MASK),
                    level + 1);
        }
    }
    free_4k_page((uintptr_t)table - ARCH_PAGE_OFFSET);
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

int aarch64_user_mm_create(mm_t **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;

    int rc = aarch64_user_mm_runtime_check();
    if (rc)
        return rc;

    mm_t *mm = mm_alloc();
    if (!mm)
        return -ENOMEM;

    uint64_t root_pa = alloc_4k_page();
    if (!root_pa) {
        kfree(mm);
        return -ENOMEM;
    }
    uint64_t *root = (uint64_t *)Phy_To_Virt(root_pa);
    __builtin_memset(root, 0, PAGE_4K_SIZE);

    user_mm_record_t *rec = kmalloc(sizeof(*rec));
    if (!rec) {
        free_4k_page(root_pa);
        kfree(mm);
        return -ENOMEM;
    }
    rec->mm = mm;
    rec->root_pa = root_pa;
    rec->next = NULL;

    uint64_t daif = spin_lock_irqsave(&user_mm_records_lock);
    rec->next = user_mm_records;
    user_mm_records = rec;
    spin_unlock_irqrestore(&user_mm_records_lock, daif);

    mm->pgdir = (uint64_t *)(uintptr_t)root_pa;
    *out = mm;
    return 0;
}

int aarch64_user_mm_destroy(mm_t *mm)
{
    if (!mm || mm == &init_mm)
        return -EINVAL;

    /* Spec 运行期门控与检查顺序: destroy uses the runtime gate like
     * create — before walking records or freeing anything, so an
     * unready/unfriendly topology never gets past this point. */
    int rc = aarch64_user_mm_runtime_check();
    if (rc)
        return rc;

    /* Ownership lookup: lock held only for the walk. */
    user_mm_record_t *rec = NULL;
    uint64_t daif = spin_lock_irqsave(&user_mm_records_lock);
    for (user_mm_record_t *r = user_mm_records; r; r = r->next) {
        if (r->mm == mm) {
            rec = r;
            break;
        }
    }
    spin_unlock_irqrestore(&user_mm_records_lock, daif);
    if (!rec)
        return -EINVAL;

    /* Installed / still published → -EBUSY, everything intact. */
    uint64_t *root = (uint64_t *)Phy_To_Virt(rec->root_pa);
    if (aarch64_user_mm_root_busy((const uint64_t *)root))
        return -EBUSY;

    /* Read-only scan first: any leaf → -EBUSY with the tree,
     * registration and mm untouched (a partially-scanned empty subtree
     * in front of the leaf stays intact). */
    if (user_mm_scan_no_leaves(root, 0))
        return -EBUSY;

    /* A leaf-free root may still carry anonymous-VM metadata (a VMA
     * or a resident record).  The image destroy path releases that
     * before calling here; refusing keeps the ownership split honest
     * instead of silently dropping a page record. */
    if (user_vm_has_metadata(mm))
        return -EBUSY;

    /* Scan passed: revoke the registration. */
    daif = spin_lock_irqsave(&user_mm_records_lock);
    user_mm_record_t **p = &user_mm_records;
    while (*p && *p != rec)
        p = &(*p)->next;
    if (*p)
        *p = rec->next;
    spin_unlock_irqrestore(&user_mm_records_lock, daif);
    kfree(rec);

    /* Second pass frees every intermediate table + the root. */
    user_mm_free_tables(root, 0);

    kfree(mm);
    return 0;
}
