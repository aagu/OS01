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

int64_t execvp(const char *file, char *const argv[])
{
    extern char **environ;
    const char *path_env = getenv("PATH");
    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }
    /* If file contains '/', don't search PATH */
    if (strchr(file, '/')) {
        return execve(file, argv, environ);
    }
    if (!path_env) {
        path_env = "/bin";
    }
    char *path_copy = strdup(path_env);
    if (!path_copy) { errno = ENOMEM; return -1; }

    int saved_errno = ENOENT;
    char *save = NULL;
    char *dir = strtok_r(path_copy, ":", &save);
    while (dir) {
        size_t dlen = strlen(dir);
        size_t flen = strlen(file);
        char *full = malloc(dlen + 1 + flen + 1);
        if (full) {
            memcpy(full, dir, dlen);
            full[dlen] = '/';
            memcpy(full + dlen + 1, file, flen + 1);
            execve(full, argv, environ);
            // Exec only returns on error; save errno for reporting
            saved_errno = errno;
            free(full);
        }
        dir = strtok_r(NULL, ":", &save);
    }
    free(path_copy);
    errno = saved_errno;
    return -1;
}
