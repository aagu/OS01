#ifndef _KERNEL_FILE_H
#define _KERNEL_FILE_H

#include <stdint.h>
#include <kernel/arch/spinlock.h>
#include <list.h>
#include <kernel/wait.h>

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
struct pty_struct;
struct tty_struct;       // v2: forward decl for file_t->tty field
typedef struct pty_struct pty_t;

// ── File types ─────────────────────────────────────────────

enum file_type {
    FD_NONE = 0,
    FD_VFS,          // regular file via VFS
    FD_PIPE,         // pipe
    FD_DEV,          // device (uses vfs_node, same ops as FD_VFS)
    FD_PTY_MASTER,   // PTY master
    FD_PTY_SLAVE,    // PTY slave (blocking I/O via pipe directly)
    FD_SOCKET,       // network socket (lwIP netconn)
};

// ── Pipe ───────────────────────────────────────────────────

typedef struct pipe {
    char      *buf;           // heap-allocated ring buffer (PIPE_SIZE bytes)
    int       head;           // producer writes here (ring buffer)
    int       tail;           // consumer reads here
    int       readers;        // active reader count
    int       writers;        // active writer count
    spinlock_T lock;
    wait_queue_t  read_wait;    // task 直接阻塞 (fd_read) — 挂 task_t.io_wait_node
    wait_queue_t  write_wait;   // task 直接阻塞 (fd_write)
    list_t        read_poll;    // poll entry — 挂 poll_wait_entry_t.node
    list_t        write_poll;   // poll entry
} pipe_t;

// ── Socket ────────────────────────────────────────────────────

typedef struct socket {
    void        *conn;         // lwIP struct netconn *
    int          domain;       // AF_INET (2)
    int          type;         // SOCK_STREAM (1) or SOCK_DGRAM (2)
    int          protocol;     // IPPROTO_TCP (6) or IPPROTO_UDP (17)
    int          state;        // UNCONNECTED/CONNECTED/LISTENING/CLOSED
    int          bound;        // 1 if bind() was called
    volatile int rx_pending;   // data available (netconn callback RCVPLUS)
    spinlock_T   lock;
    list_t       poll_list;    // poll_wait_entry_t chain
    // Partial-read state: when read() consumes less than the full
    // netbuf, the remainder is cached here for the next read.  lwIP
    // netbufs are one-shot (netbuf_delete frees everything), so a
    // 1-byte fgets read would otherwise destroy the whole 370-byte
    // HTTP response.
    void        *rx_nb;        // lwIP struct netbuf * (partially consumed)
    int          rx_off;       // bytes already consumed from rx_nb
} socket_t;

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
    // FD_PTY_MASTER / FD_PTY_SLAVE
    pty_t          *pty;
    // FD_SOCKET
    socket_t       *sock;
    // spec §4.1.1: 标记此 fd 指向控制台 TTY（仅由两个控制台 TTY open 路径设置）
    struct tty_struct *tty;
} file_t;

// ── Per-process file descriptor table ──────────────────────

typedef struct files_struct {
    spinlock_T   lock;       // 护 fd[] 槽位
    int          refcount;   // 表生命周期（__sync 原子增减）
    file_t      *fd[NOFILE];
    char        *cwd;        // heap-allocated current working directory
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

// ── Reference protocol (concurrency-safe fd table access) ──
// Table lifecycle: pin/unpin.  file lifecycle: get/put.
// All take NULL as a no-op (defensive; failure paths are safe to call).
// files_unpin / file_put / files_put_file MUST NOT be called while
// holding task_list_lock, fs->lock, or an rq lock — their drop-to-zero
// path may synchronously files_free/file_free.
void    files_pin(files_t *fs);
void    files_unpin(files_t *fs);
file_t *files_get_file(files_t *fs, int fd);   // locks fs->lock internally
void    files_put_file(file_t *f);
void    file_get(file_t *f);                   // safe under any lock
void    file_put(file_t *f);
int     fd_dup(files_t *fs, int oldfd, int minfd);   // locks fs->lock internally
int     fd_dup2(files_t *fs, int oldfd, int newfd);  // locks fs->lock internally

// Read / write through fd (may sleep for pipes)
int64_t      fd_read(file_t *f, void *buf, uint64_t size);
int64_t      fd_write(file_t *f, const void *buf, uint64_t size);

// ioctl through fd — dispatches by file type
int64_t      fd_ioctl(file_t *f, int cmd, void *arg);

// Create a pipe — fills fds[0] (read end), fds[1] (write end)
int64_t       do_pipe(int *user_fds);

// ── Pipe API (exported for PTY) ──
int64_t pipe_read_internal(pipe_t *p, void *buf, uint64_t size);
int64_t pipe_write_internal(pipe_t *p, const void *buf, uint64_t size);
void    pipe_wake_readers(pipe_t *p);
void    pipe_wake_writers(pipe_t *p);

#endif // _KERNEL_FILE_H
