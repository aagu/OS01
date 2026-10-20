/* Host shadow of <fs/file.h> for compiling the input route next to the
 * PS/2 drivers (kernel/driver/keyboard.c, kernel/driver/mouse.c).
 *
 * The real header pulls in <sync/wait.h>, whose wait_queue_t conflicts
 * with the <fs/poll.h> shadow in this directory, so this shadow carries
 * just the file fields the route touches: the file type and the
 * per-open private state (dev_private). */
#ifndef _KERNEL_FILE_H
#define _KERNEL_FILE_H

#include <stdint.h>

struct vfs_node;

enum file_type {
    FD_NONE = 0,
    FD_VFS,
    FD_PIPE,
    FD_DEV,
    FD_PTY_MASTER,
    FD_PTY_SLAVE,
    FD_SOCKET,
};

typedef struct file {
    enum file_type   type;
    uint32_t         refcount;
    int              flags;
    uint64_t         offset;
    struct vfs_node *node;
    void            *dev_private;
} file_t;

file_t *file_alloc(void);
void    file_free(file_t *f);
void    file_get(file_t *f);
void    file_put(file_t *f);

#endif /* _KERNEL_FILE_H */
