#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <sys/syscall.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── File descriptor API ────────────────────────────────────

// Open a file by path.  Returns fd, or -1 on error.
int open(const char *path, int flags);

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
int getcwd(char *buf, uint64_t size);

// ── Pipes ──────────────────────────────────────────────────

int pipe(int fds[2]);

#ifdef __cplusplus
}
#endif

#endif
