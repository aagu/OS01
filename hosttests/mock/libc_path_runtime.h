/*
 * hosttests/mock/libc_path_runtime.h — the control surface of the OS01
 * path/exec host-test boundary (plan T14).
 *
 * The mock replaces the FILESYSTEM and the EXEC boundary, not the code
 * under test: libc/stdlib/realpath.c and libc/unistd/execl.c +
 * libc/unistd/execve.c are the production sources.
 *
 * What is simulated and what the REAL kernel does:
 *
 *   - lstat(path)  : the kernel's SYS_lstat is NOFOLLOW — it reports the
 *                    final component WITHOUT following it (kernel/syscall/
 *                    sys_fs.c, "SYS_lstat(73): lstat(path, buf) — NOFOLLOW").
 *                    The mock does the same: a symbolic link answers
 *                    S_IFLNK and its own mode, not the target's.
 *   - stat(path)   : the kernel's SYS_stat FOLLOWS the final component.
 *                    The mock resolves the chain (up to 40 hops, then
 *                    ELOOP) and reports the final target's mode.
 *   - readlink()   : the kernel returns the target and refuses a
 *                    non-symlink with EINVAL.  The mock does the same.
 *   - getcwd()     : the kernel returns the process's working directory.
 *                    The mock returns a directory the test sets.
 *   - exec()       : the kernel NEVER returns on success.  The mock always
 *                    fails, with the errno the test arms, so that execvp()
 *                    can be observed walking the PATH.
 *
 * The fake filesystem is a flat table of whole paths.  A path is resolved
 * component by component; every component must be present in the table.
 */
#ifndef OS01_LIBC_PATH_RUNTIME_H
#define OS01_LIBC_PATH_RUNTIME_H

#include <stddef.h>

/* Reset the whole fake filesystem, the cwd and the exec recording. */
void path_rt_reset(void);

/* The working directory getcwd() reports.  The default is "/cwd". */
void path_rt_set_cwd(const char *cwd);

/* Register a path.  `mode` is the st_mode value (use S_IFDIR / S_IFREG /
 * S_IFLNK from <sys/stat.h>).  `target` is the symbolic link target, or
 * NULL for an ordinary file or directory. */
void path_rt_add(const char *path, unsigned mode, const char *target);

/* The errno every exec attempt answers (POSIX errno, positive).  The
 * default is ENOENT, i.e. "not found". */
void path_rt_set_exec_errno(int err);

/* The errno the exec attempt for THIS path answers; other paths keep the
 * global value. */
void path_rt_set_exec_errno_for(const char *path, int err);

/* How many exec attempts were made. */
int path_rt_exec_count(void);

/* The path of the n-th exec attempt, or NULL. */
const char *path_rt_exec_path(int n);

/* The n-th exec attempt's argv[0], or NULL. */
const char *path_rt_exec_argv0(int n);

/* The number of entries in the n-th exec attempt's argv (the NULL
 * terminator is not counted). */
int path_rt_exec_argc(int n);

/* The n-th exec attempt's argv[i], or NULL. */
const char *path_rt_exec_argv(int n, int i);

/* The number of entries in the n-th exec attempt's envp (0 when the
 * attempt passed no environment). */
int path_rt_exec_envc(int n);

/* The n-th exec attempt's envp[i], or NULL. */
const char *path_rt_exec_env(int n, int i);

#endif /* OS01_LIBC_PATH_RUNTIME_H */
