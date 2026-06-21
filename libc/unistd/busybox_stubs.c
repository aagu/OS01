#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <termios.h>
#include <sys/socket.h>

extern char **environ;

/* ── Missing stdio ── */
int ferror_unlocked(void *f) { (void)f; return 0; }
int clearerr(void *f) { (void)f; }
int fileno_unlocked(FILE *f) { (void)f; return 0; }
void *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(void *f) { (void)f; return 0; }

/* ── Poll ── */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)fds; (void)nfds; (void)timeout;
    errno = ENOSYS; return -1;
}

/* ── Rlimit ── */
int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource;
    if (rlim) { rlim->rlim_cur = 65536; rlim->rlim_max = 65536; }
    return 0;
}
int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource; (void)rlim; return 0;
}

/* ── MMAP ── */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    errno = ENOSYS; return MAP_FAILED;
}
int munmap(void *addr, size_t length) { (void)addr; (void)length; return 0; }

/* ── Time ── */
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    (void)s; (void)max; (void)fmt; (void)tm;
    return 0;
}

/* ── Unsetenv ── */
int unsetenv(const char *name) { (void)name; return 0; }

/* ── getpwnam stubs ── */
struct passwd { 
    char *pw_name; uid_t pw_uid; gid_t pw_gid; char *pw_dir; char *pw_shell; 
};
struct group {  
    char *gr_name; gid_t gr_gid; char **gr_mem; 
};

struct passwd *getpwnam(const char *name) { (void)name; return NULL; }
struct passwd *getpwuid(uid_t uid) { (void)uid; return NULL; }
struct group *getgrnam(const char *name) { (void)name; return NULL; }
struct group *getgrgid(gid_t gid) { (void)gid; return NULL; }

/* ── Termios stubs needed by ash ── */
int tcgetattr(int fd, struct termios *tio) { (void)fd; (void)tio; return 0; }
int tcsetattr(int fd, int a, const struct termios *tio) { (void)fd; (void)a; (void)tio; return 0; }
int tcflow(int fd, int action) { (void)fd; (void)action; return 0; }
int tcflush(int fd, int q) { (void)fd; (void)q; return 0; }
speed_t cfgetispeed(const struct termios *tio) { (void)tio; return B9600; }
speed_t cfgetospeed(const struct termios *tio) { (void)tio; return B9600; }
int cfsetispeed(struct termios *tio, speed_t s) { (void)tio; (void)s; return 0; }
int cfsetospeed(struct termios *tio, speed_t s) { (void)tio; (void)s; return 0; }

/* ── Signal ── */

/* ── Socket stubs ── */
int socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    errno = ENOSYS; return -1;
}
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    errno = ENOSYS; return -1;
}

/* ── dirname/basename ── */
char *dirname(char *path) { (void)path; return "/"; }
char *basename(char *path) { (void)path; return ""; }

/* ── putenv ── */
int putenv(char *string)
{
    static char **env_table = NULL;
    static int env_capacity = 0;
    static int env_count = 0;
    char *eq;
    int i;

    if (!string || !(eq = strchr(string, '=')))
        return -1;

    size_t key_len = (size_t)(eq - string);

    /* 查找已存在的 key → 替换 */
    for (i = 0; i < env_count; i++) {
        if (strncmp(env_table[i], string, key_len) == 0
            && env_table[i][key_len] == '=') {
            env_table[i] = string;
            return 0;
        }
    }

    /* 新条目：需要 env_count + 2 个槽（新条目 + NULL 结尾） */
    if (env_count + 2 > env_capacity) {
        int new_cap = env_capacity ? env_capacity * 2 : 16;
        char **new_table = realloc(env_table, new_cap * sizeof(char *));
        if (!new_table)
            return -1;
        env_table = new_table;
        env_capacity = new_cap;
    }

    env_table[env_count++] = string;
    env_table[env_count] = NULL;
    environ = env_table;

    return 0;
}

/* ── __libc_start_main ── */
int __libc_start_main(int (*main)(int, char **, char **),
                       int argc, char **argv,
                       void (*init)(void), void (*fini)(void),
                       void (*rtld_fini)(void), void *stack_end) {
    (void)init; (void)fini; (void)rtld_fini; (void)stack_end;
    return main(argc, argv, environ);
}

int listen(int sockfd, int backlog) { (void)sockfd; (void)backlog; errno = ENOSYS; return -1; }
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t addrlen) {
    (void)sockfd; (void)buf; (void)len; (void)flags; (void)dest; (void)addrlen;
    errno = ENOSYS; return -1;
}
/* ── sched stubs for busybox ── */
#include <sched.h>

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
    (void)pid; (void)cpusetsize; (void)mask;
    errno = ENOSYS; return -1;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    (void)pid; (void)cpusetsize; (void)mask;
    errno = ENOSYS; return -1;
}

int sched_yield(void) {
    return 0;
}

int sched_get_priority_max(int policy) { (void)policy; return 0; }
int sched_get_priority_min(int policy) { (void)policy; return 0; }
int sched_setscheduler(pid_t pid, int policy, const void *param) {
    (void)pid; (void)policy; (void)param; return 0;
}
int sched_getscheduler(pid_t pid) { (void)pid; return SCHED_NORMAL; }

/* ── xattr stubs (busybox getfattr/setfattr) ── */
ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    errno = ENOSYS; return -1;
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    errno = ENOSYS; return -1;
}
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size) {
    (void)fd; (void)name; (void)value; (void)size;
    errno = ENOSYS; return -1;
}
ssize_t listxattr(const char *path, char *list, size_t size) {
    (void)path; (void)list; (void)size;
    errno = ENOSYS; return -1;
}
ssize_t llistxattr(const char *path, char *list, size_t size) {
    (void)path; (void)list; (void)size;
    errno = ENOSYS; return -1;
}
ssize_t flistxattr(int fd, char *list, size_t size) {
    (void)fd; (void)list; (void)size;
    errno = ENOSYS; return -1;
}
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOSYS; return -1;
}
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOSYS; return -1;
}
int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    (void)fd; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOSYS; return -1;
}
int removexattr(const char *path, const char *name) {
    (void)path; (void)name;
    errno = ENOSYS; return -1;
}
int lremovexattr(const char *path, const char *name) {
    (void)path; (void)name;
    errno = ENOSYS; return -1;
}
int fremovexattr(int fd, const char *name) {
    (void)fd; (void)name;
    errno = ENOSYS; return -1;
}
