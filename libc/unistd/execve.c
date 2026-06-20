#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>

int64_t execve(const char *path, char *const argv[], char *const envp[])
{
    return exec(path, argv, envp);
}
