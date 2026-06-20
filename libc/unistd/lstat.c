#include <sys/syscall.h>
#include <sys/stat.h>
#include <errno.h>

/* lstat is like stat but doesn't follow symlinks.
   Since we don't have symlinks in our FS, it's identical to stat. */
int lstat(const char *path, struct stat *buf)
{
    /* Fallback to stat - our FS doesn't have symlinks */
    return stat(path, buf);
}
