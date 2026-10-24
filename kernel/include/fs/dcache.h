#ifndef _FS_DCACHE_H
#define _FS_DCACHE_H

#include <stdint.h>
#include <stdbool.h>

struct vfs_node;
struct vfs_mount;

// ── Bounded Dentry Cache (Spec §4.5, §5.4) ────────────────
#define VFS_DCACHE_BUCKETS   64
#define VFS_DCACHE_CAPACITY  256

void             dcache_init(void);
struct vfs_node *dcache_lookup_begin(struct vfs_node *parent, const char *name,
                                     uint64_t *out_gen);
void             dcache_insert_validated(struct vfs_node *parent, const char *name,
                                         struct vfs_node *child, uint64_t lookup_gen);
void             dcache_mutation_begin(void);
void             dcache_invalidate(struct vfs_node *parent, const char *name);
void             dcache_mutation_end_invalidate(struct vfs_node *parent, const char *name);
void             dcache_mutation_end_rename(struct vfs_node *olddir, const char *oldname,
                                            struct vfs_node *newdir, const char *newname);
void             dcache_flush_mount(struct vfs_mount *mp);
void             dcache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions);

#ifdef OS01_HOST_TEST
void             dcache_test_set_gen(uint64_t gen);
uint64_t         dcache_test_get_gen(void);
uint64_t         dcache_test_get_wrap_epoch(void);
uint32_t         dcache_test_get_count(void);
#endif

#endif /* _FS_DCACHE_H */
