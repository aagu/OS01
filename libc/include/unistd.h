#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int vfork(void);
int setsid(void);
pid_t getsid(pid_t pid);

// ── Standard file descriptors ────────────────────────────────
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

// ── File descriptor API ────────────────────────────────────

// Open a file by path.  Returns fd, or -1 on error.
int open(const char *path, int flags, ...);

// Close a file descriptor.  Returns 0, or -1 on error.
int close(int fd);

// Read from fd into buffer.  Returns bytes read, 0 (EOF), or -1.
int64_t read(int fd, void *buf, uint64_t size);

// Write to fd from buffer.  Returns bytes written, or -1.
int64_t write(int fd, const void *buf, uint64_t size);

// Duplicate a file descriptor (lowest available new fd).
int dup(int oldfd);

// Duplicate oldfd to a specific newfd.
int dup2(int oldfd, int newfd);

// ── Process API ────────────────────────────────────────────

// Create a child process.  Returns 0 in child, child PID in parent, -1 on error.
int64_t fork(void);

// Wait for a child process.  Returns child PID or -1.
int64_t waitpid(int64_t pid, int *status, int options);

#define WNOHANG 1

// ── File system ────────────────────────────────────────────

int chdir(const char *path);
char *getcwd(char *buf, size_t size);

// ── Pipes ──────────────────────────────────────────────────

int pipe(int fds[2]);

// ── Exec ───────────────────────────────────────────────────
int64_t execve(const char *path, char *const argv[], char *const envp[]);
int64_t execv(const char *path, char *const argv[]);
int64_t execvp(const char *file, char *const argv[]);
int64_t execl(const char *path, const char *arg, ...);
int64_t execlp(const char *file, const char *arg, ...);
int64_t execle(const char *path, const char *arg, ...);

// ── Filesystem operations ─────────────────────────────────────

int unlink(const char *path);
int link(const char *oldpath, const char *newpath);
int symlink(const char *target, const char *linkpath);
int readlink(const char *path, char *buf, size_t buf_size);
int mkdir(const char *path, int mode);
int rmdir(const char *path);
int rename(const char *oldpath, const char *newpath);
int truncate(const char *path, int64_t length);
int ftruncate(int fd, int64_t length);

// ── Process ───────────────────────────────────────────────────
int64_t getppid(void);
int64_t getpid(void);
__attribute__((__noreturn__)) void _exit(int status);

// ── Time ──────────────────────────────────────────────────────
time_t time(time_t *tloc);
int nanosleep(const struct timespec *req, struct timespec *rem);

// ── Permissions ───────────────────────────────────────────────
int chmod(const char *path, int mode);
int fchmod(int fd, int mode);
int umask(int mode);
int chown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int mknod(const char *path, mode_t mode, dev_t dev);

// ── Extra stubs ───────────────────────────────────────────────
unsigned int alarm(unsigned int s);
unsigned int sleep(unsigned int s);
int isatty(int fd);
int getuid(void);
int getgid(void);
int geteuid(void);
int getgroups(int size, unsigned int *list);
int initgroups(const char *user, int group);
void endgrent(void);
int tcflush(int fd, int q);
#define TCIFLUSH 0

// ── getopt ────────────────────────────────────────────────────
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif
int vfork(void);
int setsid(void);
pid_t getsid(pid_t pid);

#endif /* _UNISTD_H */
int vfork(void);
int setsid(void);
pid_t getsid(pid_t pid);

/* sysconf() */
#define _SC_CLK_TCK 2
#define _SC_ARG_MAX 4
#define _SC_PAGESIZE 30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
long sysconf(int name);
int getegid(void);
int setgid(gid_t g);
int setuid(uid_t u);
int setegid(gid_t g);
int seteuid(uid_t u);
int fchdir(int fd);
int chroot(const char *path);
int mkstemp(char *tmpl);
int ttyname_r(int fd, char *buf, size_t buflen);
int settimeofday(const struct timeval *tv, const struct timezone *tz);


/* Process groups */
pid_t getpgrp(void);
int setpgid(pid_t pid, pid_t pgid);
int tcsetpgrp(int fd, pid_t pgrp);
pid_t tcgetpgrp(int fd);
int killpg(pid_t pgrp, int sig);
