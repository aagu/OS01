#ifndef _KERNEL_FILE_H
#define _KERNEL_FILE_H

#include <stdint.h>
#include <arch/spinlock.h>
#include <list.h>
#include <sync/wait.h>

// ── Constants ──────────────────────────────────────────────

#include <config/limits.h>

#define NOFILE    OS01_NOFILE_CAPACITY // max open files per process
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
    // ── Cat C read_busy (Task 8) ──
    // Kernel-enforced single-reader serialization for the
    // peek→copy→commit window in pipe_read_internal.  fork()/dup()
    // can share a read end across two tasks; without this, the second
    // reader racing in the peek→commit window would corrupt tail.
    // Acquired in pipe_read_reserve(); released in pipe_read_release()
    // (lock-protected IDEMPOTENT — also safe to call from the _ft_res
    // fault-cleanup path, since out_release checks the `released` flag).
    int          read_busy;
    spinlock_T   read_busy_lock;
    wait_queue_t read_busy_wq;
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
    // 设备打开时由 devfs_open_node 或 device open 回调分配；release_file
    // 通过此指针回收 gfx_view / 其他私有状态。节点 ref 通过 file_free
    // 末尾的 vfs_node_put 单独释放，顺序固定为：devfs_release_file →
    // vfs_node_put。NULL 表示设备 open 不需要 per-file 状态。
    void           *dev_private;
    // spec §6（monitor design 2026-10-09）：文件系统私有 per-open 快照
    // （如 procfs 统计文本）。不复用 dev_private——设备文件走 devfs
    // 自有 release 协议，快照文件走 vfs_ops 的通用 release_file hook，
    // 两条协议互不重叠。file_alloc 置 NULL；仅文件系统的 open_file
    // hook 可写入，仅 release_file hook 可释放。dup/fork 共享同一
    // file_t，因此也共享同一份快照。
    void           *fs_private;
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

// ── Descriptor installation (ARCH-10 Task 6) ─────────────────
//
// Every descriptor INSTALLATION goes through fd_alloc_limit() — the
// paths that copy an already-open descriptor into a fresh table
// (files_dup(), the fork copy) write the slots directly, because
// they install nothing new.
//
// Limit: the install range is [min_fd, exclusive_limit).  The upper
// bound is EXCLUSIVE — a descriptor number equal to exclusive_limit
// is NOT installable, and exclusive_limit == 0 forbids every
// installation.
//
// Lock order: the task's RLIMIT_NOFILE soft limit is snapshotted
// under the task's resource lock (task_nofile_soft_current()) and
// that lock is RELEASED before files_t.lock is taken.  The two locks
// are never held at the same time.
//
// Ownership: on success the caller's file_t reference becomes the
// slot's reference — the caller must NOT release it.  On failure
// the reference is NOT taken: the caller keeps it and must release
// it.
//
// Errors: a negative errno — -EMFILE when the range holds no free
// slot (or when there is no descriptor table at all), -EINVAL for a
// malformed request.
int          fd_alloc_limit(files_t *fs, file_t *f, uint32_t min_fd,
                            uint32_t exclusive_limit);

// Assign a file to the lowest free slot below the current task's
// RLIMIT_NOFILE soft limit.
//   -EMFILE no free slot below the limit, or no descriptor table
//   -EINVAL a malformed request (NULL file)
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

// dup(): install a second descriptor for oldfd at the lowest free
// slot below the task's RLIMIT_NOFILE soft limit.
//   -EBADF  oldfd is not an open descriptor
//   -EMFILE no free slot below the soft limit
int     fd_dup(files_t *fs, int oldfd);   // locks fs->lock internally

// F_DUPFD / F_DUPFD_CLOEXEC: install a second descriptor for oldfd at
// the lowest free slot >= min_fd, bounded by the soft limit.
//   -EINVAL min_fd is not below the soft limit (checked BEFORE the
//           source descriptor, as POSIX/Linux do)
//   -EBADF  oldfd is not an open descriptor
//   -EMFILE no free slot in [min_fd, soft)
int     fd_dupfd(files_t *fs, int oldfd, int min_fd);   // locks fs->lock internally

// dup2(oldfd, newfd): install oldfd's file at newfd, closing the
// descriptor previously installed there.
//   -EBADF  oldfd is not an open descriptor, or newfd is outside
//           [0, soft) — an already-open descriptor stays valid even
//           when its number is above the current soft limit, but a
//           NEW descriptor may not be installed there.
// oldfd == newfd returns newfd without installing anything (and
// without a limit check) when that descriptor is already open.
int     fd_dup2(files_t *fs, int oldfd, int newfd);  // locks fs->lock internally

// Read / write through fd (may sleep for pipes)
int64_t      fd_read(file_t *f, void *buf, uint64_t size);
int64_t      fd_write(file_t *f, const void *buf, uint64_t size);

// ── Per-file snapshot lifecycle (spec §6) ──────────────────
// Thin wrappers over the optional vfs_ops open_file/read_file/
// release_file hooks.  Without a hook: open/release are no-ops and
// read falls back to the legacy node read, so existing filesystems
// and devices are unaffected.
int          vfs_file_open(file_t *f);
int64_t      vfs_file_read(file_t *f, uint64_t offset, uint64_t size,
                           void *buf);
void         vfs_file_release(file_t *f);

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
