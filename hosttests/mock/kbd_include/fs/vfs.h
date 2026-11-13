/* Host shadow of <fs/vfs.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * keyboard.h only references vfs_node_t by pointer; the real header
 * pulls in the whole block-device/stat tree. */
#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <stdint.h>
#include <stddef.h>

typedef struct vfs_node vfs_node_t;

/* Device mmap commit callback (mirror of the real header). */
typedef int (*vfs_mmap_commit_fn)(void *ctx, uint64_t first_pa,
                                  uint64_t pte_flags);

#endif /* _FS_VFS_H */
