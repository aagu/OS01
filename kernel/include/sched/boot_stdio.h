#ifndef _KERNEL_SCHED_BOOT_STDIO_H
#define _KERNEL_SCHED_BOOT_STDIO_H

#include <sched/task.h>

/* Prepares stdio file table with fd 0, 1, 2 on /dev/tty for idle task.
 * Returns 0 on success with idle->files attached.
 * On failure, returns -errno, sets *failed_component, and does not attach files.
 * failed_component is one of: "target", "files", "tty-node", "file-0", "file-1",
 * "file-2", "fd-0", "fd-1", "fd-2".
 */
int sched_boot_prepare_stdio(task_t *idle, const char **failed_component);

#endif /* _KERNEL_SCHED_BOOT_STDIO_H */
