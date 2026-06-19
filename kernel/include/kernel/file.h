#ifndef _KERNEL_FILE_H
#define _KERNEL_FILE_H

#include <stdint.h>
#include <kernel/arch/x86_64/spinlock.h>

// ── Constants ──────────────────────────────────────────────

#define NOFILE    16       // max open files per process
#define PIPE_SIZE  512     // pipe ring buffer size (bytes)

// open() flags
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

// ── Forward declarations ───────────────────────────────────

struct vfs_node;

// ── File types ─────────────────────────────────────────────

enum file_type {
    FD_NONE = 0,
    FD_VFS,          // regular file via VFS
    FD_PIPE,         // pipe
    FD_DEV,          // device (uses vfs_node, same ops as FD_VFS)
};

// ── Pipe ───────────────────────────────────────────────────

typedef struct pipe {
    char      buf[PIPE_SIZE];
    int       head;           // producer writes here (ring buffer)
    int       tail;           // consumer reads here
    int       readers;        // active reader count
    int       writers;        // active writer count
    spinlock_T lock;
} pipe_t;

// ── Open file ──────────────────────────────────────────────

typedef struct file {
    enum file_type type;
    uint32_t       refcount;   // incremented by fork / dup
    int            flags;       // O_RDONLY / O_WRONLY / O_RDWR
    uint64_t       offset;      // current read/write position
    // FD_VFS / FD_DEV
    struct vfs_node *node;
    // FD_PIPE
    pipe_t         *pipe;
} file_t;

// ── Per-process file descriptor table ──────────────────────

typedef struct files_struct {
    file_t *fd[NOFILE];
    char    cwd[256];          // current working directory
} files_t;

// ── API ────────────────────────────────────────────────────

// Allocate / free file structures
file_t      *file_alloc(void);
void         file_free(file_t *f);
pipe_t      *pipe_alloc(void);
void         pipe_free(pipe_t *p);

// Allocate / free fd table
files_t     *files_alloc(void);
void         files_free(files_t *fs);

// Deep-copy fd table (for fork) — all file->refcount++
files_t     *files_dup(files_t *fs);

// Assign a file to the lowest free slot
int          fd_alloc(files_t *fs, file_t *f);

// Close one fd (release reference)
void         fd_close(files_t *fs, int fd);

// Read / write through fd (may sleep for pipes)
int64_t      fd_read(file_t *f, void *buf, uint64_t size);
int64_t      fd_write(file_t *f, const void *buf, uint64_t size);

// Create a pipe — fills fds[0] (read end), fds[1] (write end)
int64_t       do_pipe(int *user_fds);

#endif // _KERNEL_FILE_H
