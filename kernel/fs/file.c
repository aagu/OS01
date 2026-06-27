#include <kernel/file.h>
#include <fs/vfs.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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

// ── Close a single fd ───────────────────────────────────────
void fd_close(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return;

    file_t *f = fs->fd[fd];
    if (!f) return;

    fs->fd[fd] = NULL;

    // For pipes: decrement reader/writer counts
    if (f->type == FD_PIPE && f->pipe) {
        if (f->flags == O_RDONLY)
            f->pipe->readers--;
        else
            f->pipe->writers--;
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

        while (total < size) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_empty(p)) {
                // Read one byte at a time from the ring buffer
                dst[total++] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
            }
            spin_unlock_irqrestore(&p->lock, flags);

            if (total > 0) break;

            // Pipe empty — check if any writer still exists
            if (p->writers == 0)
                return 0;  // EOF

            // Yield CPU so the writer can produce data
            schedule();
        }

        return (int64_t)total;
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

        while (total < size) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_full(p)) {
                p->buf[p->head] = src[total++];
                p->head = (p->head + 1) % PIPE_SIZE;
            }
            spin_unlock_irqrestore(&p->lock, flags);

            if (total == size) break;

            // Pipe full — check if any reader still exists
            if (p->readers == 0)
                return -EPIPE;

            // Yield CPU so the reader can consume data
            schedule();
        }

        return (int64_t)total;
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

    serial_printk("pipe: pid=%d fds=[%d,%d]\n",
                  (int)current->pid, rfd, wfd);
    return 0;
}
