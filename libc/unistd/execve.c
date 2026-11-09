#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int64_t execve(const char *path, char *const argv[], char *const envp[])
{
    int64_t ret = exec(path, argv, envp);
    // exec() only returns on error; return value is negative errno
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}

int64_t execv(const char *path, char *const argv[])
{
    extern char **environ;
    return execve(path, argv, environ);
}

/* The search path used when PATH is NOT SET.  POSIX leaves that to the
 * implementation ("the default path is implementation-defined"); glibc uses
 * confstr(_CS_PATH), which OS01 does not have, so the libc keeps its own
 * default.  It is a single directory on purpose: OS01's root filesystem
 * keeps the user programs in /bin. */
#define OS01_EXECVP_DEFAULT_PATH "/bin"

/* execvp — search PATH for `file` and execute it.
 *
 * POSIX rules this implementation follows:
 *
 *   - a name that contains a '/' is NOT searched for in PATH;
 *   - a PATH that is NOT SET uses the implementation's default path
 *     (OS01_EXECVP_DEFAULT_PATH);
 *   - an EMPTY PATH names NO directory at all: nothing is searched and
 *     the answer is ENOENT.  The empty PATH and the unset PATH are two
 *     DIFFERENT states — the old implementation merged them and searched
 *     /bin for an empty PATH, which is not what glibc does;
 *   - an EMPTY segment of PATH means the CURRENT DIRECTORY.  The old
 *     implementation used strtok_r(), which SKIPS empty segments, so
 *     "PATH=:/bin:" silently lost the current directory.  The scan below
 *     is a plain walk over the string and keeps the empty segments;
 *   - if a candidate exists but is not executable, the answer is EACCES,
 *     and EACCES takes PRIORITY over the ENOENT of the other candidates;
 *   - a candidate that exists but cannot be executed as a binary
 *     (ENOEXEC) is run through /bin/sh, as POSIX and glibc do.  The shell
 *     receives the RESOLVED CANDIDATE PATH — the very path the search
 *     found — and the caller's argv[0] is dropped, exactly as glibc
 *     does.  Handing the shell the BARE name instead would make the
 *     shell resolve it relative to the CURRENT DIRECTORY: a script
 *     outside the CWD would not be found, and a DIFFERENT file with the
 *     same name IN the CWD would be run instead;
 *   - a failed allocation answers ENOMEM immediately — it does NOT
 *     continue the search and hide the failure behind the last
 *     candidate's errno.
 */
int64_t execvp(const char *file, char *const argv[])
{
    extern char **environ;
    const char *path_env;
    size_t flen;
    int saved_errno = ENOENT;
    int saw_eacces = 0;

    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }
    /* If file contains '/', don't search PATH */
    if (strchr(file, '/')) {
        return execve(file, argv, environ);
    }

    path_env = getenv("PATH");
    if (path_env == NULL) {
        /* PATH is NOT SET: search the implementation's default path. */
        path_env = OS01_EXECVP_DEFAULT_PATH;
    } else if (*path_env == '\0') {
        /* PATH is SET but EMPTY: it names no directory, so NOTHING is
         * searched and the answer is ENOENT (this is glibc's answer too). */
        errno = ENOENT;
        return -1;
    }
    flen = strlen(file);

    {
        const char *seg = path_env;
        for (;;) {
            const char *colon = strchr(seg, ':');
            size_t dlen = colon ? (size_t)(colon - seg) : strlen(seg);
            /* An EMPTY segment is the current directory. */
            const char *dir = dlen ? seg : ".";
            size_t dirlen = dlen ? dlen : 1;
            char *full = malloc(dirlen + 1 + flen + 1);
            int e;

            if (full == NULL) {
                errno = ENOMEM;
                return -1;
            }
            memcpy(full, dir, dirlen);
            full[dirlen] = '/';
            memcpy(full + dirlen + 1, file, flen + 1);

            execve(full, argv, environ);
            /* Exec only returns on error; save errno for reporting */
            e = errno;

            if (e == ENOEXEC) {
                /* The candidate exists but is not a binary executable:
                 * run it through /bin/sh IMMEDIATELY, as POSIX and glibc
                 * do — the PATH search is not continued.
                 *
                 * The shell gets the RESOLVED CANDIDATE PATH, not the bare
                 * name, and the caller's argv[0] is dropped — this is what
                 * glibc does.  `full` is therefore kept alive until the
                 * shell attempt is over. */
                {
                    size_t argc = 0;
                    char **newargv;

                    while (argv[argc] != NULL)
                        argc++;
                    /* "/bin/sh", the candidate path, the original argv[1..],
                     * and the NULL — argc + 2 entries in all. */
                    newargv = malloc((argc + 2) * sizeof(char *));
                    if (newargv == NULL) {
                        errno = ENOMEM;
                        free(full);
                        return -1;
                    }
                    newargv[0] = (char *)"/bin/sh";
                    newargv[1] = full;
                    for (size_t i = 1; i < argc; i++)
                        newargv[i + 1] = argv[i];
                    newargv[argc + 1] = NULL;

                    {
                        int64_t ret = execve("/bin/sh", newargv, environ);
                        /* The shell did not take the script: report its
                         * errno, not the allocator's or free()'s. */
                        int saved = errno;
                        free(newargv);
                        free(full);
                        errno = saved;
                        return ret;
                    }
                }
            }

            free(full);
            if (e == EACCES) {
                saw_eacces = 1;
            } else if (e != ENOENT && e != ENOTDIR) {
                saved_errno = e;
            }

            if (!colon) break;
            seg = colon + 1;
        }
    }

    if (saw_eacces) {
        /* EACCES takes priority over the ENOENT of the other candidates. */
        errno = EACCES;
        return -1;
    }

    errno = saved_errno;
    return -1;
}
