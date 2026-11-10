/*
 * execl / execlp / execle — the variadic forms of the exec family.
 *
 * POSIX: execl(path, arg0, arg1, …, NULL) builds the argument vector and
 * calls execve(); execlp(file, arg0, …, NULL) searches PATH and calls
 * execvp(); execle(path, arg0, …, NULL, envp) passes an explicit
 * environment.
 *
 * The argument list is terminated by a NULL pointer, so its length is not
 * known up front: it is counted with a va_copy pass and then collected with
 * a second pass.  The collected vector is a PRIVATE heap array — it is NOT a
 * global scratch buffer, and the PATH search in execvp() uses a buffer of
 * its own, so the two never share storage.
 *
 * These wrappers keep the OS01 exec return type (int64_t): execve() only
 * returns on failure, so the wrappers return execve()'s answer unchanged
 * and the argv array is released on the failure path.
 */
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Count the NULL-terminated argument list.  `ap` is consumed. */
static size_t exec_argc(const char *first, va_list ap)
{
    size_t n = 0;
    const char *a = first;

    while (a != NULL) {
        n++;
        a = va_arg(ap, const char *);
    }
    return n;
}

/* Collect the NULL-terminated argument list into a private heap array. */
static char **exec_argv(const char *first, va_list ap, size_t n)
{
    char **argv = malloc((n + 1) * sizeof(char *));
    const char *a = first;

    if (argv == NULL)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        argv[i] = (char *)a;
        a = va_arg(ap, const char *);
    }
    argv[n] = NULL;
    return argv;
}

int64_t execl(const char *path, const char *arg, ...)
{
    extern char **environ;
    va_list ap;
    size_t n;
    char **argv;
    int64_t ret;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    va_start(ap, arg);
    n = exec_argc(arg, ap);
    va_end(ap);

    va_start(ap, arg);
    argv = exec_argv(arg, ap, n);
    va_end(ap);
    if (argv == NULL) {
        errno = ENOMEM;
        return -1;
    }

    ret = execve(path, argv, environ);
    free(argv);
    return ret;
}

int64_t execlp(const char *file, const char *arg, ...)
{
    va_list ap;
    size_t n;
    char **argv;

    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }

    va_start(ap, arg);
    n = exec_argc(arg, ap);
    va_end(ap);

    va_start(ap, arg);
    argv = exec_argv(arg, ap, n);
    va_end(ap);
    if (argv == NULL) {
        errno = ENOMEM;
        return -1;
    }

    {
        int64_t ret = execvp(file, argv);
        free(argv);
        return ret;
    }
}

int64_t execle(const char *path, const char *arg, ...)
{
    va_list ap;
    size_t n;
    char **argv;
    char *const *envp;
    int64_t ret;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    va_start(ap, arg);
    n = exec_argc(arg, ap);
    va_end(ap);

    va_start(ap, arg);
    argv = exec_argv(arg, ap, n);
    if (argv != NULL)
        envp = va_arg(ap, char *const *);
    else
        envp = NULL;
    va_end(ap);

    if (argv == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (envp == NULL) {
        free(argv);
        errno = EINVAL;
        return -1;
    }

    ret = execve(path, argv, envp);
    free(argv);
    return ret;
}
