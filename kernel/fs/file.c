#include <kernel/file.h>
#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/task.h>
#include <kernel.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <kernel/poll.h>

// ── Allocate a file_t ──────────────────────────────────────
file_t *file_alloc(void)
{
    file_t *f = (file_t *)calloc(1, sizeof(file_t));
    if (!f) return NULL;
    f->refcount = 1;
    return f;
}

// ── Release a file_t ────────────────────────────────────────
void file_free(file_t *f)
{
    if (!f) return;
    if (f->node)
        vfs_node_put(f->node);
    // pipe is freed separately via pipe_free
    free(f);
}

// ── Allocate a pipe ─────────────────────────────────────────
pipe_t *pipe_alloc(void)
{
    pipe_t *p = (pipe_t *)calloc(1, sizeof(pipe_t));
    if (!p) return NULL;
    p->readers = 1;
    p->writers = 1;
    spin_init(&p->lock);
    wait_queue_init(&p->read_wait);
    wait_queue_init(&p->write_wait);
    list_init(&p->read_poll);
    list_init(&p->write_poll);
    return p;
}

// ── Release a pipe ──────────────────────────────────────────
void pipe_free(pipe_t *p)
{
    if (!p) return;
    free(p);
}

// ── Allocate a files_struct ─────────────────────────────────
files_t *files_alloc(void)
{
    files_t *fs = (files_t *)calloc(1, sizeof(files_t));
    if (!fs) return NULL;
    // Default cwd is root
    fs->cwd[0] = '/';
    fs->cwd[1] = '\0';
    return fs;
}

// ── Release entire fd table ─────────────────────────────────
void files_free(files_t *fs)
{
    if (!fs) return;
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i]) {
            file_t *f = fs->fd[i];
            f->refcount--;
            if (f->refcount == 0)
                file_free(f);
            fs->fd[i] = NULL;
        }
    }
    free(fs);
}

// ── Deep-copy fd table (for fork) ───────────────────────────
// Each file->refcount is incremented so the child shares the
// same open file descriptions with the parent.
files_t *files_dup(files_t *fs)
{
    if (!fs) return NULL;

    files_t *new_fs = (files_t *)calloc(1, sizeof(files_t));
    if (!new_fs) return NULL;

    memcpy(new_fs->cwd, fs->cwd, sizeof(fs->cwd));

    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i]) {
            fs->fd[i]->refcount++;
            new_fs->fd[i] = fs->fd[i];
        }
    }
    return new_fs;
}

// ── Find lowest free fd slot ────────────────────────────────
int fd_alloc(files_t *fs, file_t *f)
{
    if (!fs || !f) return -1;
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i] == NULL) {
            fs->fd[i] = f;
            return i;
        }
    }
    return -1;  // table full
}

// ── Forward declarations for pipe wake helpers ──────────────
static void pipe_wake_readers(pipe_t *p);
static void pipe_wake_writers(pipe_t *p);

// ── Close a single fd ───────────────────────────────────────
void fd_close(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return;

    file_t *f = fs->fd[fd];
    if (!f) return;

    fs->fd[fd] = NULL;

    // For pipes: decrement reader/writer counts and wake poll waiters
    if (f->type == FD_PIPE && f->pipe) {
        uint64_t flags = spin_lock_irqsave(&f->pipe->lock);

        if (f->flags == O_RDONLY) {
            f->pipe->readers--;
            if (f->pipe->readers == 0)
                pipe_wake_writers(f->pipe);
        } else {
            f->pipe->writers--;
            if (f->pipe->writers == 0)
                pipe_wake_readers(f->pipe);
        }

        spin_unlock_irqrestore(&f->pipe->lock, flags);
    }

    f->refcount--;
    if (f->refcount == 0) {
        if (f->type == FD_PIPE && f->pipe)
            pipe_free(f->pipe);
        file_free(f);
    }
}

// ── Pipe helpers ──────────────────────────────────────────

static inline int pipe_empty(pipe_t *p)
{
    return p->head == p->tail;
}

static inline int pipe_full(pipe_t *p)
{
    return ((p->head + 1) % PIPE_SIZE) == p->tail;
}

// ── Pipe wake helpers ──────────────────────────────────────
// Caller must hold p->lock.  Wake one direct waiter + all poll
// entries.  Direct waiters use wait_queue_t (task_t.io_wait_node).
// Poll entries use a plain list_t (poll_wait_entry_t.node).
// Each poll entry cascades to wait_queue_wake_all(entry->poll_wq).

static void pipe_wake_readers(pipe_t *p)
{
    wait_queue_wake_one(&p->read_wait);

    while (!list_is_empty(&p->read_poll)) {
        list_t *node = p->read_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
}

static void pipe_wake_writers(pipe_t *p)
{
    wait_queue_wake_one(&p->write_wait);

    while (!list_is_empty(&p->write_poll)) {
        list_t *node = p->write_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
}

// ── Read through a file descriptor ──────────────────────────
// Returns bytes read, 0 for EOF (pipe with no writers), or
// negative on error.
int64_t fd_read(file_t *f, void *buf, uint64_t size)
{
    if (!f || !buf || size == 0) return -1;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node || !f->node->ops || !f->node->ops->read)
            return -1;
        if (!(f->flags == O_RDONLY || f->flags == O_RDWR))
            return -1;
        int64_t n = vfs_read(f->node, f->offset, size, buf);
        if (n > 0)
            f->offset += (uint64_t)n;
        return n;
    }
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return -1;

        uint8_t *dst = (uint8_t *)buf;
        uint64_t total = 0;

        for (;;) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_empty(p)) {
                // Read one byte at a time from the ring buffer
                dst[total++] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
            }

            if (total > 0) {
                // Data consumed — wake any blocked writers
                pipe_wake_writers(p);
                spin_unlock_irqrestore(&p->lock, flags);
                return (int64_t)total;
            }

            // Pipe empty — check if any writer still exists
            if (p->writers == 0) {
                spin_unlock_irqrestore(&p->lock, flags);
                return 0;  // EOF
            }

            spin_unlock_irqrestore(&p->lock, flags);

            // Block on pipe's read_wait (not busy-loop schedule)
            wait_queue_sleep(&p->read_wait);

            // Check for fatal signals after wake
            if (signal_pending_fatal())
                return -EINTR;
        }
    }
    default:
        return -1;
    }
}

// ── Write through a file descriptor ─────────────────────────
// Returns bytes written or negative on error.
int64_t fd_write(file_t *f, const void *buf, uint64_t size)
{
    if (!f || !buf || size == 0) return -1;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node || !f->node->ops || !f->node->ops->write)
            return -1;
        if (!(f->flags == O_WRONLY || f->flags == O_RDWR))
            return -1;
        int64_t n = vfs_write(f->node, f->offset, size, (void *)buf);
        if (n > 0)
            f->offset += (uint64_t)n;
        return n;
    }
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return -1;

        const uint8_t *src = (const uint8_t *)buf;
        uint64_t total = 0;

        for (;;) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_full(p)) {
                p->buf[p->head] = src[total++];
                p->head = (p->head + 1) % PIPE_SIZE;
            }

            if (total > 0) {
                // Wrote some data — wake blocked readers, return
                pipe_wake_readers(p);
                spin_unlock_irqrestore(&p->lock, flags);
                return (int64_t)total;
            }

            // Pipe is full (total == 0)
            // Check if any reader still exists
            if (p->readers == 0) {
                spin_unlock_irqrestore(&p->lock, flags);
                return -EPIPE;
            }

            spin_unlock_irqrestore(&p->lock, flags);

            // Block on pipe's write_wait (not busy-loop schedule)
            wait_queue_sleep(&p->write_wait);

            if (signal_pending_fatal())
                return -EINTR;
        }
    }
    default:
        return -1;
    }
}

// ── Create a pipe ──────────────────────────────────────────
// Returns 0 on success, fills fds[0] = read end, fds[1] = write end.
int64_t do_pipe(int *user_fds)
{
    if (!current->files) return -ENFILE;

    pipe_t *p = pipe_alloc();
    if (!p) return -ENOMEM;

    // Reader file
    file_t *rf = file_alloc();
    if (!rf) { pipe_free(p); return -ENOMEM; }
    rf->type = FD_PIPE;
    rf->pipe = p;
    rf->flags = O_RDONLY;
    rf->refcount = 1;

    // Writer file
    file_t *wf = file_alloc();
    if (!wf) { file_free(rf); pipe_free(p); return -ENOMEM; }
    wf->type = FD_PIPE;
    wf->pipe = p;
    wf->flags = O_WRONLY;
    wf->refcount = 1;

    int rfd = fd_alloc(current->files, rf);
    int wfd = fd_alloc(current->files, wf);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(current->files, rfd);
        if (wfd >= 0) fd_close(current->files, wfd);
        return -ENFILE;
    }

    // Write fd numbers to user space
    int fds[2] = { rfd, wfd };
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;
    memcpy((void *)user_fds, fds, sizeof(fds));

    debug_fs("pipe: pid=%d fds=[%d,%d]\n",
                  (int)current->pid, rfd, wfd);
    return 0;
}
