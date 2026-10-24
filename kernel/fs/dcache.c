#include <fs/dcache.h>
#include <fs/vfs.h>
#include <arch/spinlock.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// ── Bounded Dentry Cache (Spec §5.4) ──────────────────────
typedef struct vfs_dcache_entry {
    vfs_mount_t             *parent_mount;
    uint64_t                 parent_ino;
    char                     name[VFS_NAME_MAX];
    vfs_node_t              *node;
    struct vfs_dcache_entry *hash_next;
    struct vfs_dcache_entry *lru_prev;
    struct vfs_dcache_entry *lru_next;
    bool                     in_use;
} vfs_dcache_entry_t;

static vfs_dcache_entry_t  dcache_pool[VFS_DCACHE_CAPACITY];
static vfs_dcache_entry_t *dcache_buckets[VFS_DCACHE_BUCKETS];
static vfs_dcache_entry_t *dcache_lru_head = NULL;
static vfs_dcache_entry_t *dcache_lru_tail = NULL;
static uint32_t            dcache_count = 0;
static spinlock_T          dcache_lock;
static uint64_t            dcache_mut_gen = 1;
static uint64_t            dcache_wrap_epoch = 0;
static uint32_t            dcache_mut_active = 0;
static uint64_t            dcache_hits = 0;
static uint64_t            dcache_misses = 0;
static uint64_t            dcache_evictions = 0;

static void dcache_normalize_name(const vfs_mount_t *mp, const char *name,
                                  char out[VFS_NAME_MAX])
{
    bool ci = (mp && (mp->mnt_flags & VFS_FS_CASE_INSENSITIVE)) != 0;
    size_t i = 0;
    while (name[i] && i < VFS_NAME_MAX - 1) {
        char c = name[i];
        if (ci && c >= 'a' && c <= 'z') c -= 32;
        out[i++] = c;
    }
    out[i] = '\0';
}

static uint32_t dcache_hash(const vfs_mount_t *mp, uint64_t parent_ino,
                            const char *norm_name)
{
    uint64_t h = 1469598103934665603ULL;
    uintptr_t mp_val = (uintptr_t)mp;
    h ^= (uint64_t)mp_val;
    h *= 1099511628211ULL;
    h ^= parent_ino;
    h *= 1099511628211ULL;
    for (const unsigned char *p = (const unsigned char *)norm_name; *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    return (uint32_t)(h & (VFS_DCACHE_BUCKETS - 1));
}

static void dcache_lru_remove_locked(vfs_dcache_entry_t *e)
{
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else dcache_lru_head = e->lru_next;

    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else dcache_lru_tail = e->lru_prev;

    e->lru_prev = NULL;
    e->lru_next = NULL;
}

static void dcache_lru_insert_head_locked(vfs_dcache_entry_t *e)
{
    e->lru_prev = NULL;
    e->lru_next = dcache_lru_head;
    if (dcache_lru_head) dcache_lru_head->lru_prev = e;
    else dcache_lru_tail = e;
    dcache_lru_head = e;
}

static void dcache_hash_remove_locked(uint32_t bucket, vfs_dcache_entry_t *e)
{
    vfs_dcache_entry_t **pp = &dcache_buckets[bucket];
    while (*pp) {
        if (*pp == e) {
            *pp = e->hash_next;
            e->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

// Detach single entry and extract node atomically inside dcache_lock
static void dcache_detach_entry_locked(uint32_t bucket, vfs_dcache_entry_t *e,
                                       vfs_node_t **out_node)
{
    dcache_hash_remove_locked(bucket, e);
    dcache_lru_remove_locked(e);
    if (out_node) *out_node = e->node;
    e->node = NULL;
    e->parent_mount = NULL;
    e->parent_ino = 0;
    e->name[0] = '\0';
    e->in_use = false;
    if (dcache_count > 0) dcache_count--;
}

static void dcache_flush_all(void)
{
    while (1) {
        vfs_node_t *to_put = NULL;
        spin_lock(&dcache_lock);
        if (!dcache_lru_head) {
            spin_unlock(&dcache_lock);
            break;
        }
        vfs_dcache_entry_t *victim = dcache_lru_head;
        uint32_t b = dcache_hash(victim->parent_mount, victim->parent_ino, victim->name);
        dcache_detach_entry_locked(b, victim, &to_put);
        spin_unlock(&dcache_lock);
        if (to_put) {
            vfs_node_put(to_put);
        }
    }
}

static inline uint64_t dcache_encode_token_locked(void)
{
    if (dcache_mut_active != 0) return 0;
    uint64_t token = (dcache_wrap_epoch << 48) | (dcache_mut_gen & 0x0000FFFFFFFFFFFFULL);
    if (token == 0) token = 1;
    return token;
}

static void dcache_bump_gen_locked(bool *out_wrapped)
{
    dcache_mut_gen++;
    if ((dcache_mut_gen & 0x0000FFFFFFFFFFFFULL) == 0) {
        dcache_wrap_epoch++;
        dcache_mut_gen = 1;
        if (out_wrapped) *out_wrapped = true;
    }
}

void dcache_init(void)
{
    memset(dcache_pool, 0, sizeof(dcache_pool));
    memset(dcache_buckets, 0, sizeof(dcache_buckets));
    dcache_lru_head = NULL;
    dcache_lru_tail = NULL;
    dcache_count = 0;
    spin_init(&dcache_lock);
    dcache_mut_gen = 1;
    dcache_wrap_epoch = 0;
    dcache_mut_active = 0;
    dcache_hits = 0;
    dcache_misses = 0;
    dcache_evictions = 0;
}

vfs_node_t *dcache_lookup_begin(vfs_node_t *parent, const char *name,
                                uint64_t *out_gen)
{
    if (out_gen) *out_gen = 0;
    if (!parent || !parent->mount || !name || name[0] == '\0') return NULL;
    if ((parent->node_flags & VFS_NODE_NO_DCACHE) ||
        (parent->mount->mnt_flags & VFS_FS_NO_DCACHE)) {
        return NULL;
    }

    char norm_name[VFS_NAME_MAX];
    dcache_normalize_name(parent->mount, name, norm_name);
    uint64_t parent_ino = vfs_node_stable_ino(parent);
    uint32_t bucket = dcache_hash(parent->mount, parent_ino, norm_name);

    vfs_node_t *to_put = NULL;
    vfs_node_t *hit_node = NULL;

    spin_lock(&dcache_lock);
    for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
        if (e->parent_mount == parent->mount &&
            e->parent_ino == parent_ino &&
            strcmp(e->name, norm_name) == 0) {
            if (e->node &&
                e->node->parent == parent &&
                !(e->node->node_flags & (VFS_NODE_ORPHANED | VFS_NODE_STALE))) {
                dcache_lru_remove_locked(e);
                dcache_lru_insert_head_locked(e);
                hit_node = vfs_node_get(e->node);
                dcache_hits++;
                spin_unlock(&dcache_lock);
                return hit_node;
            }
            // Stale parent chain or orphaned node: evict and treat as miss.
            dcache_detach_entry_locked(bucket, e, &to_put);
            break;
        }
    }

    dcache_misses++;
    if (out_gen) {
        *out_gen = dcache_encode_token_locked();
    }
    spin_unlock(&dcache_lock);

    if (to_put) {
        vfs_node_put(to_put);
    }
    return NULL;
}

void dcache_insert_validated(vfs_node_t *parent, const char *name,
                             vfs_node_t *child, uint64_t lookup_gen)
{
    if (!parent || !parent->mount || !name || name[0] == '\0' || !child) return;
    if (lookup_gen == 0) return;
    if ((parent->node_flags & VFS_NODE_NO_DCACHE) ||
        (parent->mount->mnt_flags & VFS_FS_NO_DCACHE) ||
        (child->node_flags & (VFS_NODE_NO_DCACHE | VFS_NODE_MOUNT_STUB |
                              VFS_NODE_ORPHANED | VFS_NODE_STALE))) {
        return;
    }
    if (child->mount != parent->mount || child->parent != parent) return;

    char norm_name[VFS_NAME_MAX];
    dcache_normalize_name(parent->mount, name, norm_name);
    uint64_t parent_ino = vfs_node_stable_ino(parent);
    uint32_t bucket = dcache_hash(parent->mount, parent_ino, norm_name);

    vfs_node_t *evicted_to_put = NULL;

    spin_lock(&dcache_lock);
    uint64_t current_token = dcache_encode_token_locked();
    if (dcache_mut_active != 0 || current_token == 0 || current_token != lookup_gen ||
        (child->node_flags & (VFS_NODE_ORPHANED | VFS_NODE_STALE))) {
        spin_unlock(&dcache_lock);
        return;
    }

    for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
        if (e->parent_mount == parent->mount &&
            e->parent_ino == parent_ino &&
            strcmp(e->name, norm_name) == 0) {
            if (e->node != child) {
                evicted_to_put = e->node;
                e->node = vfs_node_get(child);
            }
            dcache_lru_remove_locked(e);
            dcache_lru_insert_head_locked(e);
            spin_unlock(&dcache_lock);
            if (evicted_to_put) vfs_node_put(evicted_to_put);
            return;
        }
    }

    vfs_dcache_entry_t *slot = NULL;
    if (dcache_count >= VFS_DCACHE_CAPACITY && dcache_lru_tail != NULL) {
        vfs_dcache_entry_t *victim = dcache_lru_tail;
        uint32_t vb = dcache_hash(victim->parent_mount, victim->parent_ino, victim->name);
        dcache_detach_entry_locked(vb, victim, &evicted_to_put);
        dcache_evictions++;
        slot = victim;
    } else {
        for (int i = 0; i < VFS_DCACHE_CAPACITY; i++) {
            if (!dcache_pool[i].in_use) {
                slot = &dcache_pool[i];
                break;
            }
        }
    }

    if (slot) {
        slot->parent_mount = parent->mount;
        slot->parent_ino = parent_ino;
        memcpy(slot->name, norm_name, strlen(norm_name) + 1);
        slot->node = vfs_node_get(child);
        slot->in_use = true;
        slot->hash_next = dcache_buckets[bucket];
        dcache_buckets[bucket] = slot;
        dcache_lru_insert_head_locked(slot);
        dcache_count++;
    }
    spin_unlock(&dcache_lock);

    if (evicted_to_put) {
        vfs_node_put(evicted_to_put);
    }
}

void dcache_mutation_begin(void)
{
    bool wrapped = false;
    spin_lock(&dcache_lock);
    dcache_mut_active++;
    dcache_bump_gen_locked(&wrapped);
    spin_unlock(&dcache_lock);

    if (wrapped) {
        dcache_flush_all();
    }
}

void dcache_invalidate(vfs_node_t *parent, const char *name)
{
    if (!parent || !parent->mount || !name || name[0] == '\0') return;

    char norm_name[VFS_NAME_MAX];
    dcache_normalize_name(parent->mount, name, norm_name);
    uint64_t parent_ino = vfs_node_stable_ino(parent);
    uint32_t bucket = dcache_hash(parent->mount, parent_ino, norm_name);

    vfs_node_t *to_put = NULL;

    spin_lock(&dcache_lock);
    for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
        if (e->parent_mount == parent->mount &&
            e->parent_ino == parent_ino &&
            strcmp(e->name, norm_name) == 0) {
            dcache_detach_entry_locked(bucket, e, &to_put);
            break;
        }
    }
    spin_unlock(&dcache_lock);

    if (to_put) {
        vfs_node_put(to_put);
    }
}

void dcache_mutation_end_invalidate(vfs_node_t *parent, const char *name)
{
    vfs_node_t *to_put = NULL;
    bool wrapped = false;

    char norm_name[VFS_NAME_MAX];
    uint64_t parent_ino = 0;
    uint32_t bucket = 0;
    bool has_target = (parent && parent->mount && name && name[0] != '\0');
    if (has_target) {
        dcache_normalize_name(parent->mount, name, norm_name);
        parent_ino = vfs_node_stable_ino(parent);
        bucket = dcache_hash(parent->mount, parent_ino, norm_name);
    }

    spin_lock(&dcache_lock);
    if (has_target) {
        for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
            if (e->parent_mount == parent->mount &&
                e->parent_ino == parent_ino &&
                strcmp(e->name, norm_name) == 0) {
                dcache_detach_entry_locked(bucket, e, &to_put);
                break;
            }
        }
    }
    dcache_bump_gen_locked(&wrapped);
    if (!wrapped) {
        if (dcache_mut_active > 0) dcache_mut_active--;
        spin_unlock(&dcache_lock);
    } else {
        spin_unlock(&dcache_lock);
        dcache_flush_all();
        spin_lock(&dcache_lock);
        if (dcache_mut_active > 0) dcache_mut_active--;
        spin_unlock(&dcache_lock);
    }

    if (to_put) {
        vfs_node_put(to_put);
    }
}

void dcache_flush_mount(vfs_mount_t *mp)
{
    if (!mp) return;

    while (1) {
        vfs_node_t *to_put = NULL;
        spin_lock(&dcache_lock);
        vfs_dcache_entry_t *victim = NULL;
        for (int i = 0; i < VFS_DCACHE_CAPACITY; i++) {
            if (!dcache_pool[i].in_use) continue;
            if (dcache_pool[i].parent_mount == mp ||
                (dcache_pool[i].node && dcache_pool[i].node->mount == mp)) {
                victim = &dcache_pool[i];
                break;
            }
        }
        if (!victim) {
            spin_unlock(&dcache_lock);
            break;
        }
        uint32_t b = dcache_hash(victim->parent_mount, victim->parent_ino, victim->name);
        dcache_detach_entry_locked(b, victim, &to_put);
        spin_unlock(&dcache_lock);
        if (to_put) {
            vfs_node_put(to_put);
        }
    }
}

void dcache_mutation_end_rename(vfs_node_t *olddir, const char *oldname,
                                vfs_node_t *newdir, const char *newname)
{
    (void)oldname;
    (void)newname;
    vfs_mount_t *m1 = olddir ? olddir->mount : NULL;
    vfs_mount_t *m2 = newdir ? newdir->mount : NULL;

    dcache_flush_mount(m1);
    if (m2 && m2 != m1) {
        dcache_flush_mount(m2);
    }

    bool wrapped = false;
    spin_lock(&dcache_lock);
    dcache_bump_gen_locked(&wrapped);
    if (!wrapped) {
        if (dcache_mut_active > 0) dcache_mut_active--;
        spin_unlock(&dcache_lock);
    } else {
        spin_unlock(&dcache_lock);
        dcache_flush_all();
        spin_lock(&dcache_lock);
        if (dcache_mut_active > 0) dcache_mut_active--;
        spin_unlock(&dcache_lock);
    }
}

void dcache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions)
{
    spin_lock(&dcache_lock);
    if (hits) *hits = dcache_hits;
    if (misses) *misses = dcache_misses;
    if (evictions) *evictions = dcache_evictions;
    spin_unlock(&dcache_lock);
}

#ifdef OS01_HOST_TEST
void dcache_test_set_gen(uint64_t gen)
{
    spin_lock(&dcache_lock);
    dcache_mut_gen = gen;
    spin_unlock(&dcache_lock);
}

uint64_t dcache_test_get_gen(void)
{
    spin_lock(&dcache_lock);
    uint64_t g = dcache_mut_gen;
    spin_unlock(&dcache_lock);
    return g;
}

uint64_t dcache_test_get_wrap_epoch(void)
{
    spin_lock(&dcache_lock);
    uint64_t e = dcache_wrap_epoch;
    spin_unlock(&dcache_lock);
    return e;
}

uint32_t dcache_test_get_count(void)
{
    spin_lock(&dcache_lock);
    uint32_t c = dcache_count;
    spin_unlock(&dcache_lock);
    return c;
}
#endif
