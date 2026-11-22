#ifndef _MEMORY_SHARED_BACKING_H
#define _MEMORY_SHARED_BACKING_H

/*
 * memory/shared_backing.h — architecture-neutral shared objects for
 * inode-level MAP_SHARED (aarch64 cross-process sharing plan).
 *
 * A shared_backing owns the sparse set of 4 KiB pages published for
 * one sharing identity.  File-backed objects are keyed by
 * (vfs_mount instance, node->ino): independently opened/mmap'ed
 * nodes of the same mounted inode rendezvous on the same object and
 * therefore the same physical pages.  Anonymous objects are created
 * per mmap and inherited by fork (the caller keeps the reference).
 *
 * Reference contract:
 *   - open_file / create_anon return ONE object reference on success;
 *   - get/put each move one reference; the object (and its pages) is
 *     reclaimed when the last reference drops;
 *   - shared_backing_page returns a borrowed PA that stays valid
 *     while the caller holds an object reference.
 *
 * Output parameters are written only on success.  Registry lock
 * discipline: the lock serializes lookup/publish/refcount/removal
 * only — candidate allocation, page zeroing, VFS reads and reclaim
 * all happen outside it (callers hold an object reference across the
 * whole page-fill sequence, so any lock-drop window is protected).
 */

#include <stdint.h>

struct shared_backing;
struct vfs_node;

/* Open (or rendezvous with) the shared object for a file-backed
 * node.  Admission: VFS_FILE on a registered ext2/tmpfs/vfat mount
 * with a nonzero ino; directories answer -EISDIR, everything else
 * unsupported answers -EOPNOTSUPP.  The object holds one node
 * reference for its lifetime; a concurrently-losing opener returns
 * that reference. */
int shared_backing_open_file(struct vfs_node *node,
                             struct shared_backing **out);

/* Create an anonymous zero-filled object; never enters the file
 * table.  Returns one reference. */
int shared_backing_create_anon(struct shared_backing **out);

void shared_backing_get(struct shared_backing *backing);
void shared_backing_put(struct shared_backing *backing);

/* Look up (or fill) the page at `index` pages into the object.
 * File-backed fills read one 4 KiB page from the backend at byte
 * offset index << PAGE_4K_SHIFT and zero-fill any short read or hole;
 * anonymous fills are all-zero.  Registry allocation failures answer
 * -ENOMEM; a backend -ENOMEM is preserved, any other backend error
 * maps to -EFAULT; an index that cannot be losslessly converted to a
 * byte offset answers -EINVAL (file-backed only).  On fill failure an
 * already-published page for the same index wins. */
int shared_backing_page(struct shared_backing *backing, uint64_t index,
                        uint64_t *pa_out);

#ifdef OS01_HOST_TEST
/* Host race-test observability: how many critical sections the
 * current thread is inside.  The mock backend asserts this stays 0
 * while allocating, reading or reclaiming. */
uint32_t shared_backing_test_lock_depth(void);
#endif

#endif /* _MEMORY_SHARED_BACKING_H */
