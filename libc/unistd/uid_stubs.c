#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

int getuid(void) { return 0; }
int getgid(void) { return 0; }
int getegid(void) { return 0; }
int setgid(gid_t g) { (void)g; return 0; }
int setuid(uid_t u) { (void)u; return 0; }
int setegid(gid_t g) { (void)g; return 0; }
int seteuid(uid_t u) { (void)u; return 0; }
int setgroups(size_t s, const gid_t *l) { (void)s; (void)l; return 0; }
int initgroups(const char *u, int g) { (void)u; (void)g; return 0; }
int vfork(void) { errno = ENOSYS; return -1; }
int setsid(void) { errno = ENOSYS; return -1; }
pid_t getsid(pid_t pid) { (void)pid; return 0; }

pid_t getpgrp(void) { return 1; }
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
int tcsetpgrp(int fd, pid_t pgrp) { (void)fd; (void)pgrp; return 0; }
pid_t tcgetpgrp(int fd) { (void)fd; return 1; }
int killpg(pid_t pgrp, int sig) { (void)pgrp; (void)sig; return -1; }

