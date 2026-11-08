/* kernel/sched/boot_stdio.c — Private standard fd transaction for idle task.
 *
 * Implements sched_boot_prepare_stdio() (BOOT-3 Task 3).
 * Installs fd 0 (O_RDWR), fd 1 (O_WRONLY), and fd 2 (O_WRONLY) on /dev/tty
 * into a newly allocated files_struct.
 *
 * Only attaches to the target task when all 3 descriptors are successfully
 * installed.  On any error, all references are dropped and the private table
 * is destroyed via files_unpin().
 */

#include <sched/boot_stdio.h>
#include <sched/resource.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <errno.h>

int sched_boot_prepare_stdio(task_t *idle, const char **failed_component)
{
    if (!idle || idle->files != NULL) {
        if (failed_component) *failed_component = "target";
        return -EINVAL;
    }

    files_t *files = files_alloc();
    if (!files) {
        if (failed_component) *failed_component = "files";
        return -ENOMEM;
    }

    vfs_node_t *tty = vfs_lookup("/dev/tty");
    if (!tty) {
        files_unpin(files);
        if (failed_component) *failed_component = "tty-node";
        return -ENOENT;
    }

    uint32_t soft = (uint32_t)task_nofile_soft(idle);

    /* fd 0: stdin (O_RDWR) */
    file_t *f0 = file_alloc();
    if (!f0) {
        vfs_node_put(tty);
        files_unpin(files);
        if (failed_component) *failed_component = "file-0";
        return -ENOMEM;
    }
    f0->type = FD_DEV;
    f0->node = vfs_node_get(tty);
    f0->flags = O_RDWR;
    int rc0 = fd_alloc_limit(files, f0, 0, soft);
    if (rc0 < 0 || rc0 != 0) {
        file_put(f0);
        vfs_node_put(tty);
        files_unpin(files);
        if (failed_component) *failed_component = "fd-0";
        return rc0 < 0 ? rc0 : -EMFILE;
    }

    /* fd 1: stdout (O_WRONLY) */
    file_t *f1 = file_alloc();
    if (!f1) {
        vfs_node_put(tty);
        files_unpin(files);
        if (failed_component) *failed_component = "file-1";
        return -ENOMEM;
    }
    f1->type = FD_DEV;
    f1->node = vfs_node_get(tty);
    f1->flags = O_WRONLY;
    int rc1 = fd_alloc_limit(files, f1, 0, soft);
    if (rc1 < 0 || rc1 != 1) {
        file_put(f1);
        vfs_node_put(tty);
        files_unpin(files);
        if (failed_component) *failed_component = "fd-1";
        return rc1 < 0 ? rc1 : -EMFILE;
    }

    /* fd 2: stderr (O_WRONLY) */
    file_t *f2 = file_alloc();
    if (!f2) {
        vfs_node_put(tty);
        files_unpin(files);
        if (failed_component) *failed_component = "file-2";
        return -ENOMEM;
    }
    f2->type = FD_DEV;
    f2->node = vfs_node_get(tty);
    f2->flags = O_WRONLY;
    int rc2 = fd_alloc_limit(files, f2, 0, soft);
    if (rc2 < 0 || rc2 != 2) {
        file_put(f2);
        vfs_node_put(tty);
        files_unpin(files);
        if (failed_component) *failed_component = "fd-2";
        return rc2 < 0 ? rc2 : -EMFILE;
    }

    /* Independent lookup reference is released; slots hold their own references */
    vfs_node_put(tty);

    /* Atomic attachment of complete table to idle task */
    idle->files = files;
    return 0;
}
