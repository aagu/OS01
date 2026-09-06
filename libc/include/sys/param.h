#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H 1

#include <sys/cdefs.h>
#include <limits.h>

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/* MAXSYMLINKS matches the OS01 kernel's follow-loop cap (kernel/include/fs/vfs.h).
 * Programs hitting this limit indicate a symlink loop; the kernel returns -ELOOP
 * at depth 8 either way, so capping libc at the same value gives consistent
 * behavior between user-space resolution and syscall-based resolution. */
#define MAXSYMLINKS 8
#define NGROUPS_MAX 32
#define NOFILE      256
#define NZERO       20

#endif
