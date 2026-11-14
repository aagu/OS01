#include <arch/spinlock.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <core/debug.h>
#include <log/log.h>
#include <random/random.h>
#include <memory/vmm.h>
#include <memory/uaccess.h>   // X86_USER_MIN_ADDR (Cat C — Task 8 random_read user/kernel split)
#include <arch/cpu.h>
#include <memory/slab.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <tty/tty.h>
#include <sched/task.h>
#include <fs/poll.h>
#include <fs/file.h>
#include <tty/pty.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <arch/x86_64/uaccess.h>
#include <arch/x86_64/vma.h>

// ── Internal device tree ────────────────────────────────────
//
// Every registered node lives in one tree rooted at devfs_root.  A
// node is either a directory (DEVFS_KIND_DIR) or a device
// (DEVFS_KIND_CHR / DEVFS_KIND_BLK).  A device registered under a
// '/'-separated path ("pci/0000:00:1f.2/vendor") gets the
// intermediate directories created implicitly.
//
// Records are never freed once published (ARCH-10 §7.3: "运行期已发布
// 对象存活至关机"), so a record pointer stays valid after the registry
// lock is dropped.

typedef enum {
    DEVFS_KIND_DIR,
    DEVFS_KIND_CHR,
    DEVFS_KIND_BLK,
} devfs_kind_t;

typedef struct devfs_node {
    char                    name[DEVFS_NAME_MAX];  // single path component
    uint64_t                id;                    // stable, non-zero, monotonic
    devfs_kind_t            kind;
    const struct devfs_ops *ops;                   // devices only
    void                   *private_data;          // devices only
    struct devfs_node      *parent;
    struct devfs_node      *first_child;
    struct devfs_node      *last_child;
    struct devfs_node      *next_sibling;
    struct devfs_node      *prev_sibling;
} devfs_node_t;

static spinlock_T devfs_lock = { 1 };
static devfs_node_t devfs_root = {
    .name = "",
    .id   = 0,
    .kind = DEVFS_KIND_DIR,
};
static uint64_t next_device_id = 1;

// Find a direct child of `dir` named `name`.  Caller holds devfs_lock.
static devfs_node_t *devfs_find_child(devfs_node_t *dir, const char *name)
{
    if (!dir || !name) return NULL;
    for (devfs_node_t *cur = dir->first_child; cur; cur = cur->next_sibling) {
        if (strcmp(cur->name, name) == 0)
            return cur;
    }
    return NULL;
}

// Append `node` as the last child of `dir`.  Caller holds devfs_lock.
static void devfs_link_child(devfs_node_t *dir, devfs_node_t *node)
{
    node->parent       = dir;
    node->prev_sibling = dir->last_child;
    node->next_sibling = NULL;
    if (dir->last_child)
        dir->last_child->next_sibling = node;
    else
        dir->first_child = node;
    dir->last_child = node;
}

// Does the subtree rooted at `dir` contain `needle`?
static bool devfs_subtree_contains(devfs_node_t *dir, const devfs_node_t *needle)
{
    if (!dir || !needle) return false;
    for (devfs_node_t *cur = dir->first_child; cur; cur = cur->next_sibling) {
        if (cur == needle) return true;
        if (devfs_subtree_contains(cur, needle)) return true;
    }
    return false;
}

// Resolve a VFS directory node to the devfs directory record behind
// it.  The mount root gets its fs_data in devfs_fs_mount(); the NULL
// fallback keeps lookups working if that root node is ever replaced.
static devfs_node_t *devfs_dir_record(const vfs_node_t *dir)
{
    if (!dir) return NULL;
    devfs_node_t *rec = (devfs_node_t *)dir->fs_data;
    if (!rec) return &devfs_root;
    if (rec->kind != DEVFS_KIND_DIR) return NULL;
    return rec;
}

// First device record whose private_data matches `priv`.
// Caller holds devfs_lock.
static devfs_node_t *devfs_find_device_by_private(devfs_node_t *dir, void *priv)
{
    if (!dir) return NULL;
    for (devfs_node_t *cur = dir->first_child; cur; cur = cur->next_sibling) {
        if (cur->kind != DEVFS_KIND_DIR) {
            if (cur->private_data == priv)
                return cur;
        } else {
            devfs_node_t *hit = devfs_find_device_by_private(cur, priv);
            if (hit) return hit;
        }
    }
    return NULL;
}

static devfs_node_t *devfs_device_from_node(const vfs_node_t *node)
{
    if (!node) return NULL;
    if (node->type != VFS_CHRDEV && node->type != VFS_BLKDEV)
        return NULL;
    if (!node->fs_data) return NULL;
#ifndef OS01_HOST_TEST
    if ((uint64_t)node->fs_data < 0xffff800000000000ULL)
        return NULL;
#endif

    devfs_node_t *candidate = (devfs_node_t *)node->fs_data;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    bool found = devfs_subtree_contains(&devfs_root, candidate);
    spin_unlock_irqrestore(&devfs_lock, flags);
    return found ? candidate : NULL;
}

// The console TTY — obtained via get_dev_tty() from tty.c.

// ── Built-in device handlers ───────────────────────────────

// /dev/null — reads return EOF (0 bytes), writes are discarded
static int null_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;  // EOF
}

static int null_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)buffer;
    return (int)size;  // discard all data
}

// /dev/zero — reads return zero-filled buffer, writes are discarded
static int zero_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (buffer && size > 0)
        memset(buffer, 0, size);
    return (int)size;
}

// /dev/tty — console TTY (canonical mode, echo, keyboard input)
static int dev_tty_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    tty_t *tty = get_dev_tty();
    if (!tty || !buffer || size == 0) return 0;
    return tty_read(tty, (char *)buffer, (int)size, false);
}

static int dev_tty_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    tty_t *tty = get_dev_tty();
    if (!tty || !buffer || size == 0) return 0;
    return tty_write(tty, (const char *)buffer, (int)size);
}

// ── /dev/tty poll — delegates to tty_poll via global TTY singleton
static uint32_t dev_tty_poll(void *priv, uint32_t requested,
                             struct poll_table *pt)
{
    (void)priv;
    tty_t *tty = get_dev_tty();
    if (!tty)
        return POLLERR;
    return tty_poll(tty, requested, pt);
}

// ── /dev/serial — read/write the COM1 serial port ─────────
static int serial_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    // Blocking read: read one character at a time
    uint64_t total = 0;
    while (total < size) {
        ((char *)buffer)[total] = read_serial();
        total++;
        // For now, single-char reads per call to keep it simple
        break;
    }
    return (int)total;
}

static int serial_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    /*
     * The whole buffer must reach the wire CONTIGUOUSLY.
     *
     * The console has several writers: the shell's echo, the terminal's
     * periodic stats line, the GUI-2 probe's protocol records.  A console
     * write is a line or a record, and the host-side parser requires a
     * protocol line to be key=value only -- so a byte of another writer
     * landing in the middle of a record destroys it.
     *
     * write_serial() takes and releases serial_lock per BYTE, so a
     * multi-byte write could be split by any other writer in the middle
     * (observed: the terminal's "[term-probe] ..." stats line landed
     * inside a probe record, between "fat" and "al_trigger=1").
     *
     * Holding serial_lock across the whole buffer is the kernel's own
     * convention for a multi-byte console write (kernel/log/log.c,
     * kernel/core/printk.c, kernel/tty/tty.c): the same bytes are
     * emitted, they are simply no longer interleavable in the middle.
     *
     * Nothing is truncated: write_serial_unlocked() waits for the
     * transmit FIFO to drain and then writes exactly one byte, so the
     * loop delivers the whole buffer whatever its size (there is no
     * device capacity to overrun).  The serial RX path does not take
     * serial_lock, so the keyboard/serial interrupts are not blocked by
     * this lock.
     */
    uint64_t sf = spin_lock_irqsave(&serial_lock);
    for (uint64_t i = 0; i < size; i++)
        write_serial_unlocked(((char *)buffer)[i]);
    spin_unlock_irqrestore(&serial_lock, sf);

    return (int)size;
}

// ── /dev/random + /dev/urandom — kernel CSPRNG pool ────────
// Same source as getrandom(2); uses the same user_write_range_begin/end
// helper so the devfs read path has the same kernel-write safety as the
// syscall (a deliberate deviation from the other devices' raw writes —
// see spec §5.2).  Cap mirrors the syscall so one huge read can't hold
// mm->lock while serializing that mm's munmap/mprotect.
//
// Task 8 (Cat C): fd_read FD_DEV bounces through a kernel buffer, so
// `buffer` here is kernel heap memory, NOT a user pointer.  Skip the
// user_write_range_begin/end pair when the address lies in kernel
// space — those helpers are designed to validate user ranges and
// would (correctly) reject a kernel address with -EFAULT.
static int random_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    if (size > RANDOM_MAX_LEN) size = RANDOM_MAX_LEN;

    bool in_user = ((uint64_t)buffer >= X86_USER_MIN_ADDR &&
                    (uint64_t)buffer < current->addr_limit);
    int rc = 0;
    if (in_user) {
        rc = user_write_range_begin((uint64_t)buffer, (size_t)size);
        if (rc < 0) return rc;                 // -EFAULT (lock released)
    }
    get_random_bytes(buffer, (size_t)size);
    if (in_user) user_write_range_end();
    return (int)size;
}

static int random_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    if (size > RANDOM_MAX_LEN) size = RANDOM_MAX_LEN;
    random_add_entropy(buffer, (size_t)size);
    return (int)size;
}

// ── Dispatch read/write via device record stored in node->fs_data ──

static int devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return -1;

    // Block device path: sector-level read via block_device_t
    if (dev->kind == DEVFS_KIND_BLK) {
        block_device_t *bdev = (block_device_t *)dev->private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint64_t lba   = offset / 512;
        uint32_t count = (uint32_t)((size + 511) / 512);
        if (count == 0) return 0;
        uint8_t *tmp = kmalloc(count * 512);
        if (!tmp) return -1;
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0)
            memcpy(buffer, tmp + (offset % 512), size);
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // Character device path
    if (
#ifndef OS01_HOST_TEST
        dev->ops && (uint64_t)dev->ops >= 0xffff800000000000ULL &&
#endif
        dev->ops && dev->ops->read)
        return dev->ops->read(node, offset, size, buffer);
    return -1;
}

// ── devfs_read_file — file-aware read dispatch (VFS bridge) ──
static int64_t devfs_read_file(file_t *f, uint64_t offset, uint64_t size,
                                void *buffer)
{
    if (!f || !f->node) return -1;
    devfs_node_t *dev = devfs_device_from_node(f->node);
    if (!dev) return -1;

    const struct devfs_ops *ops = dev->ops;
    if (ops && ops->read_file)
        return ops->read_file(f, offset, size, buffer);

    // No file-aware callback: legacy node read.
    return devfs_read(f->node, offset, size, buffer);
}

static int devfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return -1;

    // Block device path: read-modify-write for unaligned edges
    if (dev->kind == DEVFS_KIND_BLK) {
        block_device_t *bdev = (block_device_t *)dev->private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint64_t lba   = offset / 512;
        uint32_t count = (uint32_t)((size + 511) / 512);
        if (count == 0) return 0;
        uint8_t *tmp = kmalloc(count * 512);
        if (!tmp) return -1;
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0) {
            memcpy(tmp + (offset % 512), buffer, size);
            ret = block_device_write(bdev, lba, count, tmp);
        }
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // Character device path
    if (
#ifndef OS01_HOST_TEST
        dev->ops && (uint64_t)dev->ops >= 0xffff800000000000ULL &&
#endif
        dev->ops && dev->ops->write)
        return dev->ops->write(node, offset, size, buffer);
    return -1;
}

// ── devfs_mmap — dispatch mmap for device-backed mmap ────────
static int devfs_mmap(vfs_node_t *node, struct vma *vma,
                      vfs_mmap_commit_fn commit, void *ctx)
{
    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return -EINVAL;
    if (
#ifndef OS01_HOST_TEST
        dev->ops && (uint64_t)dev->ops >= 0xffff800000000000ULL &&
#endif
        dev->ops && dev->ops->mmap)
        return dev->ops->mmap(node, vma, commit, ctx);
    return -ENODEV;
}

// ── devfs_poll — dispatch poll for fd_poll(FD_DEV) ─────────
uint32_t devfs_poll(vfs_node_t *node, uint32_t requested, poll_table_t *pt)
{
    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return POLLNVAL;

    if (
#ifndef OS01_HOST_TEST
        dev->ops && (uint64_t)dev->ops >= 0xffff800000000000ULL &&
#endif
        dev->ops && dev->ops->poll)
        return dev->ops->poll(dev->private_data, requested, pt);

    // No poll callback: default to always ready
    return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
}

// ── devfs_poll_file — file-aware poll dispatch ─────────────
uint32_t devfs_poll_file(file_t *f, uint32_t requested, poll_table_t *pt)
{
    if (!f || !f->node) return POLLNVAL;
    if (f->type != FD_DEV) return POLLNVAL;

    devfs_node_t *dev = devfs_device_from_node(f->node);
    if (!dev)
        return POLLNVAL;

    const struct devfs_ops *ops = dev->ops;
    if (ops && ops->poll_file)
        return ops->poll_file(f, requested, pt);

    // No file-aware callback: legacy node poll
    return devfs_poll(f->node, requested, pt);
}

// ── devfs_ioctl_node — dispatch ioctl to a devfs device ─────
int devfs_ioctl_node(vfs_node_t *node, int cmd, void *arg)
{
    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return -ENODEV;
    if (
#ifndef OS01_HOST_TEST
        dev->ops && (uint64_t)dev->ops >= 0xffff800000000000ULL &&
#endif
        dev->ops && dev->ops->ioctl)
        return dev->ops->ioctl(node, cmd, arg);
    return -ENOTTY;
}

// ── devfs_ioctl_file — file-aware ioctl dispatch ────────────
int devfs_ioctl_file(file_t *f, int cmd, void *arg)
{
    if (!f || !f->node) return -ENOTTY;
    if (f->type != FD_DEV) return -ENOTTY;

    devfs_node_t *dev = devfs_device_from_node(f->node);
    if (!dev)
        return -ENODEV;

    const struct devfs_ops *ops = dev->ops;
    if (ops && ops->ioctl_file) {
        int rc = ops->ioctl_file(f, cmd, arg);
        if (rc != -ENOTTY) return rc;
    }
    if (ops && ops->ioctl) return ops->ioctl(f->node, cmd, arg);
    return -ENOTTY;
}

// ── devfs_release_file — per-file final-close hook ───────────
void devfs_release_file(file_t *f)
{
    if (!f || !f->node) return;
    if (f->type != FD_DEV) return;

    devfs_node_t *dev = devfs_device_from_node(f->node);
    if (!dev)
        return;
    const struct devfs_ops *ops = dev->ops;
    if (ops && ops->release_file) ops->release_file(f);
}

// ── readdir: enumerate registered devices ───────────────────

static int devfs_readdir(vfs_node_t *node, uint64_t index, vfs_dirent_t *entry)
{
    if (!node || node->type != VFS_DIR || !entry) return -1;

    devfs_node_t *rec = devfs_dir_record(node);
    if (!rec) return -1;

    uint64_t cur_idx = 0;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *cur = rec->first_child;
    while (cur) {
        if (cur_idx == index) {
            size_t nlen = strlen(cur->name);
            if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
            memcpy(entry->name, cur->name, nlen);
            entry->name[nlen] = '\0';
            entry->type = (cur->kind == DEVFS_KIND_DIR) ? VFS_DIR :
                          (cur->kind == DEVFS_KIND_BLK) ? VFS_BLKDEV :
                                                           VFS_CHRDEV;
            entry->size = 0;
            entry->ino = cur->id;
            spin_unlock_irqrestore(&devfs_lock, flags);
            return 0;
        }
        cur = cur->next_sibling;
        cur_idx++;
    }
    spin_unlock_irqrestore(&devfs_lock, flags);

    entry->name[0] = '\0';  // end of directory
    return 0;
}

// ── Single devfs operations vector ──────────────────────────

static const vfs_inode_ops_t devfs_iops;
static const vfs_file_ops_t devfs_fops;

static vfs_node_t *devfs_create_node_for_record(devfs_node_t *rec, vfs_node_t *dir)
{
    vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!child) return NULL;
    child->name = strndup(rec->name, VFS_NAME_MAX - 1);
    if (!child->name) {
        kfree(child);
        return NULL;
    }
    child->type = (rec->kind == DEVFS_KIND_BLK) ? VFS_BLKDEV : VFS_CHRDEV;
    child->ino = rec->id;
    child->rdev = rec->id;
    child->mode = (child->type == VFS_CHRDEV ? S_IFCHR : S_IFBLK) | 0666;
    child->nlink = 1;
    child->size = 0;
    if (dir) {
        child->mount = dir->mount;
        child->parent = vfs_node_get(dir);
    }
    child->fs_data = rec;
    child->i_ops = &devfs_iops;
    child->f_ops = &devfs_fops;
    child->refcount = 1;
    return child;
}

// Synthetic directory node for a devfs sub-directory.  Its fs_data
// carries the directory record so that the next lookup() call knows
// which directory it is descending into.
static vfs_node_t *devfs_create_dir_node(devfs_node_t *rec, vfs_node_t *dir)
{
    vfs_node_t *node = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!node) return NULL;
    node->name = strndup(rec->name, VFS_NAME_MAX - 1);
    if (!node->name) {
        kfree(node);
        return NULL;
    }
    node->type = VFS_DIR;
    node->ino = rec->id;
    node->rdev = 0;
    node->mode = S_IFDIR | 0755;
    node->nlink = 2;
    node->size = 0;
    if (dir) {
        node->mount = dir->mount;
        node->parent = vfs_node_get(dir);
    }
    node->fs_data = rec;
    node->i_ops = &devfs_iops;
    node->f_ops = &devfs_fops;
    node->refcount = 1;
    return node;
}

static int devfs_iop_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !name) return -EINVAL;
    if (dir->type != VFS_DIR) return -ENOTDIR;

    devfs_node_t *rec = devfs_dir_record(dir);
    if (!rec) return -ENOTDIR;

    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *target = devfs_find_child(rec, name);
    spin_unlock_irqrestore(&devfs_lock, flags);

    if (!target) return -ENOENT;

    vfs_node_t *child = (target->kind == DEVFS_KIND_DIR)
                        ? devfs_create_dir_node(target, dir)
                        : devfs_create_node_for_record(target, dir);
    if (!child) return -ENOMEM;
    *out = child;
    return 0;
}

// Forward declaration — the implementation lives in the public API
// section at the end of this file.
static int devfs_split_path(const char *path,
                             char comps[DEVFS_MAX_DEPTH][DEVFS_NAME_MAX],
                             int *out_count);

int devfs_lookup_node(const char *name, struct vfs_node **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!name) return -EINVAL;

    char (*comps)[DEVFS_NAME_MAX] =
        (char (*)[DEVFS_NAME_MAX])calloc(DEVFS_MAX_DEPTH, DEVFS_NAME_MAX);
    if (!comps) return -ENOMEM;

    int count = 0;
    int rc = devfs_split_path(name, comps, &count);
    if (rc != 0) {
        kfree(comps);
        return rc;
    }

    devfs_node_t *found = NULL;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *cur = &devfs_root;
    for (int i = 0; i < count; i++) {
        devfs_node_t *child = devfs_find_child(cur, comps[i]);
        if (!child) { found = NULL; break; }
        found = child;
        if (i + 1 < count) {
            // Only a directory can be descended into.
            if (child->kind != DEVFS_KIND_DIR) { found = NULL; break; }
            cur = child;
        }
    }
    spin_unlock_irqrestore(&devfs_lock, flags);

    kfree(comps);

    if (!found) return -ENOENT;

    vfs_node_t *child = (found->kind == DEVFS_KIND_DIR)
                        ? devfs_create_dir_node(found, NULL)
                        : devfs_create_node_for_record(found, NULL);
    if (!child) return -ENOMEM;
    *out = child;
    return 0;
}

static const vfs_inode_ops_t devfs_iops = {
    .lookup = devfs_iop_lookup,
};

static const vfs_file_ops_t devfs_fops = {
    .read      = devfs_read,
    .write     = devfs_write,
    .readdir   = devfs_readdir,
    .mmap      = devfs_mmap,
    // File-aware read bridge: fd_read() reaches the device through it.
    .read_file = devfs_read_file,
};

// ── devfs_open_node — sys_open integration ──────────────────
int devfs_open_node(vfs_node_t *node, const char *path, int flags, file_t **out)
{
    if (!out) return -EINVAL;

    // Guard: only process devfs device nodes.
    if (!node || (node->type != VFS_CHRDEV && node->type != VFS_BLKDEV))
        return -ENOSYS;

    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return -ENODEV;

    if (
#ifndef OS01_HOST_TEST
        dev->ops && (uint64_t)dev->ops >= 0xffff800000000000ULL &&
#endif
        dev->ops && dev->ops->open) {
        int rc = dev->ops->open(path, out);
        if (rc == 0 && *out) {
            if ((*out)->type == FD_NONE) (*out)->type = FD_DEV;
            if (!(*out)->node) (*out)->node = vfs_node_get(node);
            (*out)->flags = flags;
            return 0;
        }
        if (rc != -ENOSYS) return rc;
    }

    // Default: FD_DEV
    *out = file_alloc();
    if (!*out) return -ENOMEM;
    (*out)->type = FD_DEV;
    (*out)->node = vfs_node_get(node);
    (*out)->flags = flags;
    // Check if device is console TTY
    if (dev->private_data && dev->private_data == keyboard_get_tty()) {
        (*out)->tty = get_dev_tty();
        if (current->pgrp != 0) {
            tty_t *tty = get_dev_tty();
            if (tty) {
                uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
                if (tty->fg_pgrp == 0) tty->fg_pgrp = current->pgrp;
                spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            }
        }
    }
    return 0;
}

// ── Initialise devfs and register built-in devices ──────────

static const struct devfs_ops null_ops = {
    .read  = null_read,
    .write = null_write,
};

static const struct devfs_ops zero_ops = {
    .read  = zero_read,
    .write = null_write,
};

static const struct devfs_ops random_ops = {
    .read  = random_read,
    .write = random_write,
};

static const struct devfs_ops serial_ops = {
    .read  = serial_read,
    .write = serial_write,
};

const struct devfs_ops tty_phys_ops = {
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .poll  = dev_tty_poll,
    .ioctl = tty_phys_ioctl,
};

// ── /dev/tty magic: opens the controlling terminal ────────────
static int tty_magic_open(const char *name, file_t **out_file)
{
    if (strcmp(name, "/dev/tty") != 0) return -ENOSYS;

    void *target = NULL;
    if (current->ctty_type == CTTY_PTY) {
        pty_t *pty = (pty_t *)current->ctty;
        if (!pty) return -ENXIO;
        file_t *f = file_alloc();
        if (!f) return -ENOMEM;

        uint64_t pty_flags = spin_lock_irqsave(&pty_lock);
        if (!pty->allocated) {
            spin_unlock_irqrestore(&pty_lock, pty_flags);
            free(f);
            return -ENXIO;
        }
        if (pty->master_to_slave) {
            uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
            pty->master_to_slave->readers++;
            spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        }
        if (pty->slave_to_master) {
            uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
            pty->slave_to_master->writers++;
            spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
        }
        spin_unlock_irqrestore(&pty_lock, pty_flags);

        f->type = FD_PTY_SLAVE;
        f->pty = pty;
        f->flags = O_RDWR;
        *out_file = f;
        return 0;
    }

    target = keyboard_get_tty();

    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *matched_dev = devfs_find_device_by_private(&devfs_root, target);
    if (matched_dev && matched_dev->kind != DEVFS_KIND_CHR)
        matched_dev = NULL;
    spin_unlock_irqrestore(&devfs_lock, flags);

    if (matched_dev) {
        vfs_node_t *node = devfs_create_node_for_record(matched_dev, NULL);
        if (!node) return -ENOMEM;
        *out_file = file_alloc();
        if (!*out_file) {
            vfs_node_put(node);
            return -ENOMEM;
        }
        (*out_file)->type = FD_DEV;
        (*out_file)->node = node;
        (*out_file)->tty = get_dev_tty();
        if (current->pgrp != 0) {
            tty_t *tty = get_dev_tty();
            if (tty) {
                uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
                if (tty->fg_pgrp == 0) tty->fg_pgrp = current->pgrp;
                spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            }
        }
        return 0;
    }
    return -ENXIO;
}

const struct devfs_ops tty_magic_ops = {
    .open  = tty_magic_open,
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .poll  = dev_tty_poll,
    .ioctl = tty_phys_ioctl,
};

static int devfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev;
    (void)data;
    mp->i_ops = &devfs_iops;
    mp->f_ops = &devfs_fops;
    mp->fs_data = NULL;
    mp->root->i_ops = &devfs_iops;
    mp->root->f_ops = &devfs_fops;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0755;
    mp->root->nlink = 2;
    // Bind the mount root to the devfs tree root so that lookups
    // starting at /dev resolve against the registered records.
    mp->root->fs_data = &devfs_root;
    return 0;
}

static void devfs_fs_unmount(struct vfs_mount *mp)
{
    (void)mp;
}

static vfs_fs_type_t devfs_fs_type = {
    .name     = "devfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = devfs_fs_mount,
    .unmount  = devfs_fs_unmount,
};

int devfs_register_fs(void)
{
    int ret = vfs_register_filesystem(&devfs_fs_type);
    if (ret == -EEXIST) return 0;
    return ret;
}

int devfs_init(void)
{
    int ret = devfs_register_fs();
    if (ret != 0 && ret != -EEXIST) {
        log_err("devfs: register fs failed (%d)\n", ret);
        return ret;
    }

    // Mount devfs at /dev
    ret = vfs_mount_fs("devfs", "/dev", NULL, NULL);
    if (ret != 0) {
        log_err("devfs: mount at /dev failed (%d)\n", ret);
        return ret;
    }
    debug_fs("devfs: mounted at /dev\n");

    // Register built-in character devices (essential devices: failure aborts boot)
    static const struct {
        const char *name;
        const struct devfs_ops *ops;
    } builtins[] = {
        { "null",    &null_ops },
        { "zero",    &zero_ops },
        { "random",  &random_ops },
        { "urandom", &random_ops },
        { "serial",  &serial_ops },
    };

    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        ret = devfs_register_chrdev(builtins[i].name, NULL, builtins[i].ops);
        if (ret != 0 && ret != -EEXIST) {
            log_err("devfs: failed to register essential device '%s' (%d)\n",
                    builtins[i].name, ret);
            return ret;
        }
    }

    // NOTE: /dev/tty and /dev/tty0 are registered in tty_boot_init()
    // after keyboard_set_tty() so keyboard_get_tty() returns
    // the correct pointer.
    return 0;
}

// ── Public API ──────────────────────────────────────────────

// Split a relative '/'-separated path into components.
//
// Returns 0 and fills `comps` / `*out_count` on success, or:
//   -EINVAL        NULL / empty / absolute / empty component / "." / ".."
//                  / component >= DEVFS_NAME_MAX bytes / too many components
//   -ENAMETOOLONG  the absolute user path would not fit in VFS_NAME_MAX
//
// `comps` must have room for DEVFS_MAX_DEPTH entries of DEVFS_NAME_MAX bytes.
static int devfs_split_path(const char *path,
                             char comps[DEVFS_MAX_DEPTH][DEVFS_NAME_MAX],
                             int *out_count)
{
    if (!path || path[0] == '\0') return -EINVAL;

    // devfs paths are relative to the mount point (/dev).
    if (path[0] == '/') return -EINVAL;

    // The absolute user path is "/dev/" + path and must fit in VFS_NAME_MAX.
    size_t plen = strlen(path);
    if (plen + strlen("/dev/") >= VFS_NAME_MAX) return -ENAMETOOLONG;

    int count = 0;
    const char *p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0) return -EINVAL;                // "", "a//b", "trailing/"
        if (len >= DEVFS_NAME_MAX) return -EINVAL;   // too long — never truncate
        if (count >= DEVFS_MAX_DEPTH) return -EINVAL;

        // No traversal semantics in devfs.
        if ((len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.'))
            return -EINVAL;

        memcpy(comps[count], p, len);
        comps[count][len] = '\0';
        count++;

        if (!slash) break;
        p = slash + 1;
    }

    *out_count = count;
    return 0;
}

// Register a node at `path`, creating intermediate directories.
//
// Nodes are allocated BEFORE the registry lock is taken (allocation must
// not happen under a spinlock).  One candidate node is prepared per path
// component; a component that already exists in the tree reuses the
// existing node and its candidate is discarded.
//
// Error codes:
//   -EINVAL        malformed path (see devfs_split_path)
//   -ENAMETOOLONG  the absolute path would not fit in VFS_NAME_MAX
//   -EEXIST        the leaf name is already taken in its directory
//   -ENOTDIR       an intermediate component is a device, not a directory
//   -ENOMEM        allocation failed
//   -ENOSPC        the id space is exhausted
//
// Intermediate directories that were already created stay in the tree when
// a later step fails — they carry no externally visible state and rolling
// them back would need extra reference counting.
static int devfs_register_node(const char *path, devfs_kind_t kind,
                                void *private_data,
                                const struct devfs_ops *ops)
{
    char (*comps)[DEVFS_NAME_MAX] =
        (char (*)[DEVFS_NAME_MAX])calloc(DEVFS_MAX_DEPTH, DEVFS_NAME_MAX);
    if (!comps) return -ENOMEM;

    int count = 0;
    int rc = devfs_split_path(path, comps, &count);
    if (rc != 0) {
        kfree(comps);
        return rc;
    }

    // One candidate node per component.  slots[i] is used only when
    // component i does not already exist in the tree.
    devfs_node_t *slots[DEVFS_MAX_DEPTH];
    uint32_t used = 0;
    for (int i = 0; i < count; i++) {
        slots[i] = (devfs_node_t *)calloc(1, sizeof(devfs_node_t));
        if (!slots[i]) {
            for (int j = 0; j < i; j++)
                kfree(slots[j]);
            kfree(comps);
            return -ENOMEM;
        }
        strncpy(slots[i]->name, comps[i], DEVFS_NAME_MAX - 1);
        slots[i]->name[DEVFS_NAME_MAX - 1] = '\0';
        slots[i]->kind = (i == count - 1) ? kind : DEVFS_KIND_DIR;
    }

    int result = 0;
    devfs_node_t *leaf = NULL;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);

    devfs_node_t *cur_dir = &devfs_root;
    for (int i = 0; i < count - 1; i++) {
        devfs_node_t *child = devfs_find_child(cur_dir, comps[i]);
        if (!child) {
            // Implicit directory.
            if (next_device_id == UINT64_MAX) { result = -ENOSPC; goto out; }
            slots[i]->id = next_device_id++;
            devfs_link_child(cur_dir, slots[i]);
            used |= (1u << i);
            child = slots[i];
        } else if (child->kind != DEVFS_KIND_DIR) {
            // A device occupies this name — it cannot be a directory.
            result = -ENOTDIR;
            goto out;
        }
        cur_dir = child;
    }

    // Leaf: the name must be free in its directory.
    if (devfs_find_child(cur_dir, comps[count - 1])) {
        result = -EEXIST;
        goto out;
    }
    if (next_device_id == UINT64_MAX) { result = -ENOSPC; goto out; }

    leaf = slots[count - 1];
    leaf->id = next_device_id++;
    leaf->kind = kind;
    leaf->ops = ops;
    leaf->private_data = private_data;
    devfs_link_child(cur_dir, leaf);
    used |= (1u << (count - 1));

out:
    spin_unlock_irqrestore(&devfs_lock, flags);

    // Release the candidates that were not linked into the tree.
    for (int i = 0; i < count; i++) {
        if (!(used & (1u << i)))
            kfree(slots[i]);
    }
    kfree(comps);

    if (result == 0) {
        debug_fs("devfs: registered '%s' (id %lu)\n", path,
                 (unsigned long)leaf->id);
    }
    // A failed registration is reported by the caller: it owns the
    // severity (a boot-critical device is an error, an optional one is
    // a warning).  devfs only hands back the errno.
    return result;
}

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops)
{
    return devfs_register_node(name, DEVFS_KIND_CHR, private_data, ops);
}

int devfs_register_path(const char *path, void *private_data,
                         const struct devfs_ops *ops)
{
    return devfs_register_node(path, DEVFS_KIND_CHR, private_data, ops);
}

int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    return devfs_register_node(name, DEVFS_KIND_BLK, dev, NULL);
}

int devfs_register_blkdev_path(const char *path, block_device_t *dev)
{
    return devfs_register_node(path, DEVFS_KIND_BLK, dev, NULL);
}

// ── devfs_get_private — retrieve private_data from a devfs node ──
void *devfs_get_private(struct vfs_node *node)
{
    devfs_node_t *dev = devfs_device_from_node(node);
    if (!dev)
        return NULL;
    return dev->private_data;
}

// ── devfs_get_file_ops — retrieve devfs_ops from an open device file ──
const struct devfs_ops *devfs_get_file_ops(file_t *f)
{
    if (!f || !f->node || f->type != FD_DEV) return NULL;
    devfs_node_t *dev = devfs_device_from_node(f->node);
    if (!dev)
        return NULL;
    return dev->ops;
}
