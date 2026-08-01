#include <sys/xattr.h>
#include <errno.h>

ssize_t getxattr(const char *p, const char *n, void *v, size_t s)
    { (void)p; (void)n; (void)v; (void)s; errno = ENOSYS; return -1; }
ssize_t lgetxattr(const char *p, const char *n, void *v, size_t s)
    { (void)p; (void)n; (void)v; (void)s; errno = ENOSYS; return -1; }
ssize_t fgetxattr(int fd, const char *n, void *v, size_t s)
    { (void)fd; (void)n; (void)v; (void)s; errno = ENOSYS; return -1; }
ssize_t listxattr(const char *p, char *l, size_t s)
    { (void)p; (void)l; (void)s; errno = ENOSYS; return -1; }
ssize_t llistxattr(const char *p, char *l, size_t s)
    { (void)p; (void)l; (void)s; errno = ENOSYS; return -1; }
ssize_t flistxattr(int fd, char *l, size_t s)
    { (void)fd; (void)l; (void)s; errno = ENOSYS; return -1; }
int setxattr(const char *p, const char *n, const void *v, size_t s, int f)
    { (void)p; (void)n; (void)v; (void)s; (void)f; errno = ENOSYS; return -1; }
int lsetxattr(const char *p, const char *n, const void *v, size_t s, int f)
    { (void)p; (void)n; (void)v; (void)s; (void)f; errno = ENOSYS; return -1; }
int fsetxattr(int fd, const char *n, const void *v, size_t s, int f)
    { (void)fd; (void)n; (void)v; (void)s; (void)f; errno = ENOSYS; return -1; }
int removexattr(const char *p, const char *n)
    { (void)p; (void)n; errno = ENOSYS; return -1; }
int lremovexattr(const char *p, const char *n)
    { (void)p; (void)n; errno = ENOSYS; return -1; }
int fremovexattr(int fd, const char *n)
    { (void)fd; (void)n; errno = ENOSYS; return -1; }
