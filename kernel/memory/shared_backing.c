/*
 * memory/shared_backing.c — architecture-neutral registry of shared
 * pages for inode-level MAP_SHARED (aarch64 cross-process plan).
 *
 * Identity: a file-backed object is keyed by (vfs_mount instance,
 * node->ino) — independent opens/mmaps of the same mounted inode
 * rendezvous on one object; the same inode number under a different
 * mount instance stays isolated.  Anonymous objects are created per
 * call and never enter the file table.
 *
 * Pages: each object owns a sparse two-level index (fixed bucket
 * array per object, collision chains of on-demand page nodes).  A
 * published page is fully initialized before it becomes visible —
 * a racing filler never overwrites it and never publishes a partial
 * page; the loser re-looks-up under the lock, drops its candidate
 * and borrows the winner's PA.
 *
 * Lock discipline: the registry spinlock serializes lookup, publish,
 * refcount changes and last-reference removal ONLY.  Candidate
 * allocation, page zeroing, VFS reads and all reclaim run outside
 * the lock (callers hold an object reference across any lock-drop
 * window).  The static lock uses the pmm.c unlocked initial value
 * (lock field == 1); irqsave/restore guards the shared state.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <fs/vfs.h>
#include <arch/spinlock.h>
#include <memory/memory.h>     /* Phy_To_Virt */
#include <memory/pmm.h>        /* PAGE_4K_SHIFT / PAGE_4K_SIZE */
#include <memory/slab.h>       /* kmalloc / kfree */
#include <memory/shared_backing.h>

#define SHARED_FILE_BUCKETS 64u
#define SHARED_PAGE_BUCKETS 64u

struct shared_page_node {
    uint64_t index;
    uint64_t pa;
    struct shared_page_node *next;
};

struct shared_backing {
    uint32_t refs;
    bool     anon;
    struct vfs_node *node;              /* file-backed: held reference */
    struct vfs_mount *mount;            /* file identity (NULL anon) */
    uint64_t ino;
    struct shared_page_node *pages[SHARED_PAGE_BUCKETS];
    struct shared_backing *file_next;   /* file-table collision chain */
};

/* pmm.c unlocked initial value: lock field == 1. */
static spinlock_T g_shared_backing_lock = { .lock = 1L };

static struct shared_backing
    *g_shared_files[SHARED_FILE_BUCKETS];

#ifdef OS01_HOST_TEST
/* Host race-test observability: the mock backend asserts that
 * allocations, reads and reclaims never run inside the registry
 * critical section. */
static _Thread_local uint32_t t_lock_depth;
static _Thread_local uint64_t t_irq_flags;

uint32_t shared_backing_test_lock_depth(void)
{
    return t_lock_depth;
}

#define REGISTRY_LOCK()                                              \
    (t_irq_flags = spin_lock_irqsave(&g_shared_backing_lock),        \
     t_lock_depth++, (uint64_t)0)
#define REGISTRY_UNLOCK(flags)                                       \
    do {                                                             \
        (void)(flags);                                               \
        t_lock_depth--;                                              \
        spin_unlock_irqrestore(&g_shared_backing_lock, t_irq_flags); \
    } while (0)
#else
#define REGISTRY_LOCK()  spin_lock_irqsave(&g_shared_backing_lock)
#define REGISTRY_UNLOCK(flags) \
    spin_unlock_irqrestore(&g_shared_backing_lock, (flags))
#endif

static uint32_t shared_file_bucket(struct vfs_mount *mount, uint64_t ino)
{
    uint64_t h = (uint64_t)(uintptr_t)mount;
    h ^= ino + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2);
    return (uint32_t)(h >> 4) & (SHARED_FILE_BUCKETS - 1);
}

static uint32_t shared_page_bucket(uint64_t index)
{
    uint64_t h = index * UINT64_C(0x9e3779b97f4a7c15);
    return (uint32_t)((h >> 32) ^ h) & (SHARED_PAGE_BUCKETS - 1);
}

static struct shared_backing *file_find_locked(struct vfs_mount *mount,
                                               uint64_t ino)
{
    struct shared_backing *b =
        g_shared_files[shared_file_bucket(mount, ino)];
    for (; b; b = b->file_next)
        if (b->mount == mount && b->ino == ino)
            return b;
    return NULL;
}

static struct shared_page_node *page_find_locked(
    const struct shared_backing *backing, uint64_t index)
{
    struct shared_page_node *p =
        backing->pages[shared_page_bucket(index)];
    for (; p; p = p->next)
        if (p->index == index)
            return p;
    return NULL;
}

/* File admission (spec): VFS_FILE on a registered ext2/tmpfs/vfat
 * mount with a nonzero inode number. */
static int shared_admission(const struct vfs_node *node)
{
    if (!node)
        return -EINVAL;
    if (node->type == VFS_DIR)
        return -EISDIR;
    if (node->type != VFS_FILE)
        return -EOPNOTSUPP;
    if (!node->mount || !node->mount->fs_type ||
        !node->mount->fs_type->name)
        return -EOPNOTSUPP;
    const char *name = node->mount->fs_type->name;
    if (strcmp(name, "ext2") != 0 && strcmp(name, "tmpfs") != 0 &&
        strcmp(name, "vfat") != 0)
        return -EOPNOTSUPP;
    if (node->ino == 0)
        return -EOPNOTSUPP;
    return 0;
}

/* Caller receives refs == 1.  For a file object `node` is the held
 * reference transferred from the opener. */
static struct shared_backing *backing_new(struct vfs_node *node)
{
    struct shared_backing *b = kmalloc(sizeof(*b));
    if (!b)
        return NULL;
    memset(b, 0, sizeof(*b));
    b->refs = 1;
    b->anon = (node == NULL);
    b->node = node;
    if (node) {
        b->mount = node->mount;
        b->ino = node->ino;
    }
    return b;
}

/* Runs with no registry lock held: frees every published page, the
 * object metadata and the held node reference. */
static void backing_destroy(struct shared_backing *backing)
{
    for (uint32_t i = 0; i < SHARED_PAGE_BUCKETS; i++) {
        struct shared_page_node *p = backing->pages[i];
        while (p) {
            struct shared_page_node *next = p->next;
            free_4k_page(p->pa);
            kfree(p);
            p = next;
        }
    }
    struct vfs_node *node = backing->node;
    kfree(backing);
    if (node)
        vfs_node_put(node);
}

int shared_backing_open_file(struct vfs_node *node,
                             struct shared_backing **out)
{
    if (!out)
        return -EINVAL;
    int adm = shared_admission(node);
    if (adm != 0)
        return adm;

    /* Hold our node reference before the identity can be published so
     * the object keeps the inode alive across any lock-drop window. */
    struct vfs_node *held = vfs_node_get(node);

    uint64_t flags = REGISTRY_LOCK();
    struct shared_backing *found =
        file_find_locked(node->mount, node->ino);
    if (found) {
        found->refs++;
        REGISTRY_UNLOCK(flags);
        vfs_node_put(held);
        *out = found;
        return 0;
    }
    REGISTRY_UNLOCK(flags);

    struct shared_backing *cand = backing_new(node);
    if (!cand) {
        vfs_node_put(held);
        return -ENOMEM;
    }

    flags = REGISTRY_LOCK();
    found = file_find_locked(node->mount, node->ino);
    if (found) {
        found->refs++;
        REGISTRY_UNLOCK(flags);
        /* Concurrent opener won the publish: the candidate never
         * entered the table (empty page set) — return its node
         * reference and hand out the winner. */
        kfree(cand);
        vfs_node_put(held);
        *out = found;
        return 0;
    }
    uint32_t bucket = shared_file_bucket(node->mount, node->ino);
    cand->file_next = g_shared_files[bucket];
    g_shared_files[bucket] = cand;
    REGISTRY_UNLOCK(flags);

    *out = cand;
    return 0;
}

int shared_backing_create_anon(struct shared_backing **out)
{
    if (!out)
        return -EINVAL;
    struct shared_backing *b = backing_new(NULL);
    if (!b)
        return -ENOMEM;
    *out = b;
    return 0;
}

void shared_backing_get(struct shared_backing *backing)
{
    if (!backing)
        return;
    uint64_t flags = REGISTRY_LOCK();
    backing->refs++;
    REGISTRY_UNLOCK(flags);
}

void shared_backing_put(struct shared_backing *backing)
{
    if (!backing)
        return;
    bool destroy = false;

    uint64_t flags = REGISTRY_LOCK();
    if (backing->refs > 0 && --backing->refs == 0) {
        if (!backing->anon) {
            struct shared_backing **pp =
                &g_shared_files[shared_file_bucket(backing->mount,
                                                   backing->ino)];
            while (*pp && *pp != backing)
                pp = &(*pp)->file_next;
            if (*pp)
                *pp = backing->file_next;
        }
        destroy = true;
    }
    REGISTRY_UNLOCK(flags);

    if (destroy)
        backing_destroy(backing);
}

int shared_backing_page(struct shared_backing *backing, uint64_t index,
                        uint64_t *pa_out)
{
    if (!backing || !pa_out)
        return -EINVAL;

    uint64_t flags = REGISTRY_LOCK();
    struct shared_page_node *hit = page_find_locked(backing, index);
    uint64_t pa = hit ? hit->pa : 0;
    REGISTRY_UNLOCK(flags);
    if (hit) {
        *pa_out = pa;
        return 0;
    }

    int ret = 0;
    uint64_t cand_pa = 0;
    struct shared_page_node *entry = NULL;

    if (!backing->anon &&
        index > (UINT64_MAX >> PAGE_4K_SHIFT)) {
        /* index << PAGE_4K_SHIFT would not be a lossless byte
         * offset. */
        ret = -EINVAL;
        goto recheck;
    }

    /* Candidate preparation outside the registry lock: the caller
     * holds an object reference, so the object cannot disappear. */
    cand_pa = alloc_4k_page();
    if (!cand_pa) {
        ret = -ENOMEM;
        goto recheck;
    }
    entry = kmalloc(sizeof(*entry));
    if (!entry) {
        ret = -ENOMEM;
        goto recheck_free_pa;
    }

    /* Initialize the whole page first: short reads, holes and EOF
     * leave the zeroed tail (anonymous fills stay all-zero). */
    memset((void *)Phy_To_Virt(cand_pa), 0, PAGE_4K_SIZE);

    if (!backing->anon) {
        int n = vfs_read(backing->node, index << PAGE_4K_SHIFT,
                         PAGE_4K_SIZE, Phy_To_Virt(cand_pa));
        if (n < 0) {
            ret = (n == -ENOMEM) ? -ENOMEM : -EFAULT;
            goto recheck_free_both;
        }
    }

    /* Second lookup under the lock: only one fully-prepared candidate
     * per key gets published; a concurrent winner keeps its page. */
    flags = REGISTRY_LOCK();
    hit = page_find_locked(backing, index);
    if (hit) {
        pa = hit->pa;
        REGISTRY_UNLOCK(flags);
        free_4k_page(cand_pa);
        kfree(entry);
        *pa_out = pa;
        return 0;
    }
    entry->index = index;
    entry->pa = cand_pa;
    uint32_t bucket = shared_page_bucket(index);
    entry->next = backing->pages[bucket];
    backing->pages[bucket] = entry;
    REGISTRY_UNLOCK(flags);

    *pa_out = cand_pa;
    return 0;

recheck_free_both:
    free_4k_page(cand_pa);
    kfree(entry);
    goto recheck;
recheck_free_pa:
    free_4k_page(cand_pa);
    goto recheck;
recheck:
    /* Error path: an already-published page still wins so a racing
     * caller gets a valid PA instead of an error. */
    flags = REGISTRY_LOCK();
    hit = page_find_locked(backing, index);
    pa = hit ? hit->pa : 0;
    REGISTRY_UNLOCK(flags);
    if (hit) {
        *pa_out = pa;
        return 0;
    }
    return ret;
}
