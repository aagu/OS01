# /dev/fb mmap + PTY + terminal 用户态渲染 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将终端渲染从内核移到用户态 terminal.elf，内核保留原始数据传输 (PTY pipe + TTY ring buffer)。

**Architecture:** PTY master 通过 `FD_PTY_MASTER` 文件类型绕过 devfs 多实例限制，slave 走 devfs `FD_DEV` 路径。`/dev/fb` 通过 `VM_IO` + `do_mmap` 设备分派支持用户态 mmap。tty.c/console.c 退化到 raw 模式。`/dev/tty` 通过 `current->ctty` 指针实现每进程控制终端。

**Tech Stack:** C (kernel + userspace), x86_64, QEMU, devfs, PSF2 font

**Spec:** `docs/superpowers/specs/2026-07-26-fb-pty-terminal-design.md`

## Global Constraints

- 内核 C 代码符合现有风格 (kernel/tty/, kernel/fs/, kernel/driver/)
- 用户态代码放在 user/，链接现有 libc
- 所有 `kmalloc/calloc/kfree` 使用内核分配器
- `make clean` 强依赖于 struct 变更后
- 每步 commit 独立可编译

---

## 文件结构

```
Create:
  kernel/driver/pty.c              — PTY alloc/free, master/slave I/O, ptmx_open
  kernel/driver/fb.c               — fb_mmap, fb_read, fb_ioctl, FBIOSURRENDER
  kernel/include/kernel/pty.h      — pty_t struct + PTY_MAX + API
  kernel/include/kernel/fb.h       — fb_info struct
  user/terminal.c                  — VT100 terminal + PSF2 renderer

Modify:
  kernel/include/fs/devfs.h        — devfs_ops struct, devfs_register_chrdev 签名
  kernel/fs/devfs.c                — ops dispatch, devfs_open/devfs_mmap, tty_dev_open, 设备迁移
  kernel/include/fs/vfs.h          — vfs_ops.mmap 字段
  kernel/include/kernel/file.h     — FD_PTY_MASTER, file->pty
  kernel/fs/file.c                 — pipe_read/pipe_write 提取, FD_PTY_MASTER cases, file_free
  kernel/include/kernel/vma.h      — VM_IO 标志
  kernel/memory/vma.c              — do_mmap 设备分派
  kernel/include/kernel/tty.h      — 删除 canonical 字段
  kernel/tty/tty.c                 — tty_read 退化, 删除 tty_canon_process
  kernel/include/kernel/console.h  — console_surrender_fb, 移除 blink
  kernel/tty/console.c             — 删除 CSI 状态机 + 光标闪烁
  kernel/include/kernel/task.h     — ctty_type enum + ctty 字段
  kernel/sched/task.c              — fork 继承 ctty
  kernel/driver/keyboard.c         — 方向键不再检查 ICANON
  kernel/driver/pit.c              — 移除 console_blink_tick 调用
  kernel/kernel/main.c             — 注册 /dev/fb, /dev/ptmx, PTY init, console_blink_tick 移除
  user/Makefile                    — terminal.elf 构建规则
```

---

### Task 1: devfs_ops struct 重构

**Files:**
- Modify: `kernel/include/fs/devfs.h`
- Modify: `kernel/fs/devfs.c`
- Modify: `kernel/include/fs/vfs.h`
- Modify: `kernel/kernel/main.c`

**Interfaces:**
- Produces: `struct devfs_ops` (used by Task 2,3,5,6), `devfs_register_chrdev(name, priv, &ops)` (new signature), `vfs_ops.mmap` field

- [ ] **Step 1: 定义 devfs_ops struct 在 devfs.h**

在 `kernel/include/fs/devfs.h` 中替换现有函数指针参数声明：

```c
// 新增：设备操作向量
struct devfs_ops {
    int (*open)(const char *name, struct file **out_file);
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *);
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *);
    uint32_t (*poll)(void *priv, struct poll_table *pt);
    int (*mmap)(struct vfs_node *, struct vma *);
    int (*ioctl)(struct vfs_node *, int cmd, void *arg);
};

// 更新声明
int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);
```

- [ ] **Step 2: 更新 devfs_device_t 内部结构**

在 `kernel/fs/devfs.c` 中，将 `read/write/poll` 函数指针替换为 `ops` 指针：

```c
typedef struct devfs_device {
    char name[DEVFS_NAME_MAX];
    uint8_t type;
    const struct devfs_ops *ops;     // ← 替代原来的 read/write/poll 独立指针
    void *private_data;
    int registered;
} devfs_device_t;
```

- [ ] **Step 3: 更新 devfs_read/devfs_write/devfs_poll 分派**

在 `kernel/fs/devfs.c` 中，将分派改为通过 `ops->read/ops->write/ops->poll`：

```c
static int devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;

    // Block device path
    if (devices[idx].type == VFS_BLKDEV) {
        block_device_t *bdev = (block_device_t *)devices[idx].private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint32_t lba   = (uint32_t)(offset / 512);
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

    // Character device: dispatch through ops struct
    if (devices[idx].ops && devices[idx].ops->read)
        return devices[idx].ops->read(node, offset, size, buffer);
    return -1;
}
```

同理更新 `devfs_write` 和 `devfs_poll`。

- [ ] **Step 4: 新增 devfs_mmap 分派**

在 `kernel/fs/devfs.c` 中添加：

```c
static int devfs_mmap(vfs_node_t *node, vma_t *vma)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -EINVAL;
    if (devices[idx].ops && devices[idx].ops->mmap)
        return devices[idx].ops->mmap(node, vma);
    return -ENODEV;
}
```

更新 devfs_ops 表：

```c
static struct vfs_ops devfs_ops = {
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .mmap    = devfs_mmap,
};
```

- [ ] **Step 5: 新增 sys_open 的 devfs_ops.open 集成**

在 `kernel/arch/x86_64/trap.c` 的 `SYS_open` case 中（查找 `case SYS_open:`），在 `vfs_lookup_from` 之后、`file_alloc` 之前插入 open 回调检查：

找到代码中类似：
```c
vfs_node_t *node = vfs_lookup_from(path, cwd);
if (!node) { regs->rax = -ENOENT; break; }
file_t *f = file_alloc();
f->type = FD_DEV;  // or FD_VFS
f->node = node;
```

替换为：
```c
vfs_node_t *node = vfs_lookup_from(path, cwd);
if (!node) { regs->rax = -ENOENT; break; }

// 检查设备 open 回调（/dev/ptmx, /dev/tty 等）
file_t *f = NULL;
if (node->ops && node->ops->mmap) {
    // node has devfs ops → check for open callback
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx >= 0 && idx < DEVFS_MAX_DEVICES && devices[idx].registered
        && devices[idx].ops && devices[idx].ops->open) {
        int rc = devices[idx].ops->open(path, &f);
        if (rc == 0 && f) {
            // open callback returned a custom file_t
            int fd = fd_alloc(current->files, f);
            if (fd < 0) { file_free(f); regs->rax = -EMFILE; break; }
            vfs_node_put(node);
            regs->rax = fd;
            break;
        }
        if (rc != -ENOSYS) {
            vfs_node_put(node);
            regs->rax = rc;
            break;
        }
    }
}
// 回退到默认 FD_DEV 路径
if (!f) {
    f = file_alloc();
    f->type = FD_DEV;
    f->node = node;
    f->flags = flags;
    int fd = fd_alloc(current->files, f);
    if (fd < 0) { file_free(f); regs->rax = -EMFILE; break; }
    regs->rax = fd;
}
```

注意：需要添加 `#include <fs/devfs.h>` 或 extern `devices` 数组。更简洁的做法是抽取一个 `devfs_open_node()` 函数放在 `devfs.c` 中：

```c
// kernel/fs/devfs.c — 新增
int devfs_open_node(vfs_node_t *node, const char *path, int flags, file_t **out)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -ENODEV;
    if (devices[idx].ops && devices[idx].ops->open) {
        int rc = devices[idx].ops->open(path, out);
        if (rc == 0 && *out) {
            (*out)->flags = flags;
            return 0;
        }
        if (rc != -ENOSYS) return rc;
    }
    // Default: allocate FD_DEV
    *out = file_alloc();
    if (!*out) return -ENOMEM;
    (*out)->type = FD_DEV;
    (*out)->node = vfs_node_get(node);
    (*out)->flags = flags;
    return 0;
}
```

在 `devfs.h` 中声明 `int devfs_open_node(...)`，然后 SYS_open 中调用它。

- [ ] **Step 6: VFS vfs_ops 添加 mmap 字段**

在 `kernel/include/fs/vfs.h` 的 `vfs_ops_t` 末尾添加：

```c
    // mmap handler for device/file-backed mappings. NULL = not supported.
    int (*mmap)(struct vfs_node *, struct vma *);
} vfs_ops_t;
```

在 `kernel/fs/vfs.c` 中，所有返回 `vfs_ops_t` 的地方确认 `.mmap = NULL`（默认不支持）。

- [ ] **Step 7: 迁移 5 个现有设备到 ops struct**

在 `kernel/fs/devfs.c` 的 `devfs_init()` 中，为每个设备创建 static ops struct：

```c
// null
static const struct devfs_ops null_ops = {
    .open  = NULL,  // 走默认 FD_DEV
    .read  = null_read,
    .write = null_write,
};

// zero
static const struct devfs_ops zero_ops = {
    .read  = zero_read,
    .write = null_write,
};

// random
static const struct devfs_ops random_ops = {
    .read  = random_read,
    .write = random_write,
};

// serial
static const struct devfs_ops serial_ops = {
    .read  = serial_read,
    .write = serial_write,
};

// tty — 暂用旧指针（Task 6 改为 kbd_tty + ops）
static const struct devfs_ops tty_ops_v1 = {
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .poll  = dev_tty_poll,
};
```

`devfs_init()` 中调用：
```c
devfs_register_chrdev("null",   NULL, &null_ops);
devfs_register_chrdev("zero",   NULL, &zero_ops);
devfs_register_chrdev("random", NULL, &random_ops);
devfs_register_chrdev("serial", NULL, &serial_ops);
devfs_register_chrdev("tty",    NULL, &tty_ops_v1);  // private_data 在 Task 6 改为 kbd_tty
```

- [ ] **Step 8: 更新 devfs_register_chrdev 实现**

```c
int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops)
{
    if (device_count >= DEVFS_MAX_DEVICES)
        return -1;

    int idx = device_count;
    size_t nlen = strlen(name);
    if (nlen >= DEVFS_NAME_MAX) nlen = DEVFS_NAME_MAX - 1;
    memcpy(devices[idx].name, name, nlen);
    devices[idx].name[nlen] = '\0';

    devices[idx].type = VFS_CHRDEV;
    devices[idx].ops = ops;
    devices[idx].private_data = private_data;
    devices[idx].registered = 1;
    device_count++;

    debug_fs("devfs: registered '%s' (chrdev)\n", name);
    return 0;
}
```

- [ ] **Step 9: 编译验证**

```bash
make clean && make
```

- [ ] **Step 10: Commit**

```bash
git add kernel/include/fs/devfs.h kernel/fs/devfs.c kernel/include/fs/vfs.h \
        kernel/fs/vfs.c kernel/kernel/main.c kernel/arch/x86_64/trap.c
git commit -m "feat: devfs_ops struct refactor — ops vector replaces individual function pointers

- struct devfs_ops with open/read/write/poll/mmap/ioctl callbacks
- vfs_ops_t gains .mmap field
- devfs_open_node() for sys_open integration
- 5 existing devices migrated to ops structs
- devfs_mmap dispatcher added"
```

---

### Task 2: FD_PTY_MASTER + file_t 扩展 + pipe 函数提取

**Files:**
- Modify: `kernel/include/kernel/file.h`
- Modify: `kernel/fs/file.c`

**Interfaces:**
- Produces: `FD_PTY_MASTER` enum value, `file->pty`, `pipe_read()`, `pipe_write()`, `fd_poll` FD_PTY_MASTER case, `fd_ioctl` FD_PTY_MASTER case, `file_free` FD_PTY_MASTER handling

- [ ] **Step 1: 添加 FD_PTY_MASTER 和 file->pty**

在 `kernel/include/kernel/file.h` 中：

```c
enum file_type {
    FD_NONE = 0,
    FD_VFS,
    FD_PIPE,
    FD_DEV,
    FD_PTY_MASTER,   // ← 新增
};

// 前向声明
struct pty_struct;
typedef struct pty_struct pty_t;

typedef struct file {
    enum file_type type;
    uint32_t       refcount;
    int            flags;
    uint64_t       offset;
    struct vfs_node *node;   // FD_VFS / FD_DEV
    pipe_t         *pipe;    // FD_PIPE
    pty_t          *pty;     // FD_PTY_MASTER ← 新增
} file_t;
```

- [ ] **Step 2: 提取 pipe_read/pipe_write 内部函数**

在 `kernel/fs/file.c` 中，从 `fd_read` 和 `fd_write` 的 `FD_PIPE` case 提取核心逻辑：

```c
// 从 fd_read 的 FD_PIPE case 提取（只保留读循环核心，去除 wait/schedule）
static int64_t pipe_read_internal(pipe_t *p, void *buf, uint64_t size)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t total = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&p->lock);

        while (total < size && !pipe_empty(p)) {
            dst[total++] = p->buf[p->tail];
            p->tail = (p->tail + 1) % PIPE_SIZE;
        }

        if (total > 0) {
            pipe_wake_writers(p);
            spin_unlock_irqrestore(&p->lock, flags);
            return (int64_t)total;
        }

        if (p->writers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;  // EOF
        }

        spin_unlock_irqrestore(&p->lock, flags);

        // 注册到 wait queue
        wait_queue_t *wq = &p->read_wait;
        int do_eof = 0;
        uint64_t wq_flags = spin_lock_irqsave(&wq->lock);
        list_add_to_before(&wq->head, &current->io_wait_node);

        {
            uint64_t p2_flags = spin_lock_irqsave(&p->lock);
            if (p->writers == 0 && pipe_empty(p)) {
                list_del_init(&current->io_wait_node);
                do_eof = 1;
            } else if (p->writers == 0 && !pipe_empty(p)) {
                list_del_init(&current->io_wait_node);
            }
            spin_unlock_irqrestore(&p->lock, p2_flags);
        }

        if (do_eof) {
            spin_unlock_irqrestore(&wq->lock, wq_flags);
            return 0;
        }

        current->state = TASK_INTERRUPTIBLE;
        int was_queued = !list_is_empty(&current->io_wait_node);
        spin_unlock_irqrestore(&wq->lock, wq_flags);

        if (!was_queued) {
            current->state = TASK_RUNNING;
        } else {
            schedule();
            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            current->state = TASK_RUNNING;
        }

        if (signal_pending_fatal())
            return -EINTR;
    }
}
```

同理提取 `pipe_write_internal(pipe_t *p, const void *buf, uint64_t size)`。

然后 `fd_read` 的 `FD_PIPE` case 简化为：
```c
case FD_PIPE:
    return pipe_read_internal(f->pipe, buf, size);
```

`fd_write` 同理。

- [ ] **Step 3: 添加 FD_PTY_MASTER dispatch**

在 `fd_read` 的 switch 中，`FD_PIPE` case 后面添加：

```c
case FD_PTY_MASTER: {
    pty_t *pty = f->pty;
    if (!pty || !pty->slave_to_master) return -1;
    return pipe_read_internal(pty->slave_to_master, buf, size);
}
```

在 `fd_write` 中：

```c
case FD_PTY_MASTER: {
    pty_t *pty = f->pty;
    if (!pty || !pty->master_to_slave) return -1;
    return pipe_write_internal(pty->master_to_slave, buf, size);
}
```

- [ ] **Step 4: 添加 FD_PTY_MASTER poll case**

在 `fd_poll` 函数中（`kernel/fs/file.c` 或 `kernel/fs/poll.c`）：

```c
case FD_PTY_MASTER: {
    pty_t *pty = f->pty;
    if (!pty) return POLLERR;
    uint32_t mask = 0;
    if (pty->slave_to_master) {
        uint64_t flags = spin_lock_irqsave(&pty->slave_to_master->lock);
        if (!pipe_empty(pty->slave_to_master)) {
            mask |= POLLIN | POLLRDNORM;
        } else if (pt && !pt->triggered) {
            poll_wait(pt, &pty->slave_to_master->read_wait,
                      &pty->slave_to_master->lock);
        }
        spin_unlock_irqrestore(&pty->slave_to_master->lock, flags);
    }
    if (pty->master_to_slave) {
        uint64_t flags = spin_lock_irqsave(&pty->master_to_slave->lock);
        if (!pipe_full(pty->master_to_slave)) {
            mask |= POLLOUT | POLLWRNORM;
        } else if (pt && !pt->triggered) {
            poll_wait(pt, &pty->master_to_slave->write_wait,
                      &pty->master_to_slave->lock);
        }
        spin_unlock_irqrestore(&pty->master_to_slave->lock, flags);
    }
    return mask;
}
```

- [ ] **Step 5: 添加 FD_PTY_MASTER ioctl case**

在 `fd_ioctl` 函数中（查找现有 ioctl dispatch，通常在 `trap.c` 的 SYS_ioctl case 或 `file.c`）：

```c
case FD_PTY_MASTER: {
    pty_t *pty = f->pty;
    if (!pty) return -ENOTTY;
    if (cmd == TCGETS) {
        if (!arg) return -EINVAL;
        memcpy(arg, &pty->term, sizeof(struct termios));
        return 0;
    }
    return -ENOTTY;
}
```

- [ ] **Step 6: file_free 的 FD_PTY_MASTER 处理**

在 `kernel/fs/file.c` 的 `file_free` 中，`FD_PIPE` 清理后添加：

```c
// FD_PTY_MASTER cleanup: static slot, no kfree, close pipe write ends
if (f->type == FD_PTY_MASTER && f->pty) {
    pty_t *pty = f->pty;
    // 关闭 master→slave 写端 → slave reader 收到 EOF
    if (pty->master_to_slave) {
        uint64_t flags = spin_lock_irqsave(&pty->master_to_slave->lock);
        pty->master_to_slave->writers--;
        int need_r = (pty->master_to_slave->writers == 0);
        spin_unlock_irqrestore(&pty->master_to_slave->lock, flags);
        if (need_r) pipe_wake_readers(pty->master_to_slave);
    }
    // 关闭 slave→master 写端 → master reader 收到 EOF
    if (pty->slave_to_master) {
        uint64_t flags = spin_lock_irqsave(&pty->slave_to_master->lock);
        pty->slave_to_master->writers--;
        int need_r = (pty->slave_to_master->writers == 0);
        spin_unlock_irqrestore(&pty->slave_to_master->lock, flags);
        if (need_r) pipe_wake_readers(pty->slave_to_master);
    }
    pty->pgrp = 0;  // 重置进程组
    f->pty = NULL;
}
```

- [ ] **Step 7: 编译验证**

```bash
make clean && make
```

- [ ] **Step 8: Commit**

```bash
git add kernel/include/kernel/file.h kernel/fs/file.c
git commit -m "feat: FD_PTY_MASTER file type + pipe_read/pipe_write extraction

- New FD_PTY_MASTER enum value + file->pty field
- pipe_read_internal/pipe_write_internal extracted from FD_PIPE cases
- FD_PTY_MASTER fd_read/fd_write delegates to PTY pipes
- FD_PTY_MASTER fd_poll with poll_wait registration
- FD_PTY_MASTER fd_ioctl(TCGETS) for termios awareness
- file_free handles FD_PTY_MASTER (static slot, decrement pipe refcounts)"
```

---

### Task 3: VM_IO + do_mmap 设备分派 + /dev/fb

**Files:**
- Modify: `kernel/include/kernel/vma.h`
- Modify: `kernel/memory/vma.c`
- Create: `kernel/driver/fb.c`
- Create: `kernel/include/kernel/fb.h`
- Modify: `kernel/kernel/main.c`

**Interfaces:**
- Produces: `VM_IO`, `fb_mmap`, `fb_read`, `fb_ioctl(FBIOSURRENDER)`, `/dev/fb` 注册

- [ ] **Step 1: 添加 VM_IO 标志**

在 `kernel/include/kernel/vma.h` 的 VM_* 定义后：

```c
#define VM_IO  0x80   // 设备 MMIO — 页面不由 pmm/slab 管理
```

- [ ] **Step 2: 重构 do_mmap 步 4 — 设备 mmap 分派**

在 `kernel/memory/vma.c` 的 `do_mmap` 中，将现有的步 4（File mapping setup）替换为包含设备分派的版本：

找到：
```c
    // ── 4. File mapping setup ──────────────────────────
    vfs_node_t *file_node = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        file_t *file = current->files->fd[fd];
        file_node = vfs_node_get(file->node);
    }

    // ── 5. Allocate VMA ────────────────────────────────
    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
```

替换为：
```c
    // ── 4. Check for device mmap (new) ─────────────────
    vfs_node_t *file_node = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        file_t *file = current->files->fd[fd];
        if (!file || !file->node) return -EBADF;
        file_node = vfs_node_get(file->node);

        // Device node with mmap handler → device path
        if (file_node && file_node->ops && file_node->ops->mmap) {
            if (!(flags & MAP_SHARED))
                return -EINVAL;  // device mappings must be SHARED

            vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
            if (!vma) { vfs_node_put(file_node); return -ENOMEM; }
            list_init(&vma->list);
            vma->vm_start     = addr;
            vma->vm_end       = addr + length;
            vma->vm_flags     = vm_flags_base | VM_IO;
            vma->vm_page_prot = page_prot;
            vma->vm_pgoff     = offset >> PAGE_4K_SHIFT;
            vma->vm_file      = file_node;

            int rc = file_node->ops->mmap(file_node, vma);
            if (rc < 0) {
                kfree(vma);
                vfs_node_put(file_node);
                return rc;
            }
            vma_insert(current->mm, vma);
            return (int64_t)vma->vm_start;
        }
    }

    // ── 5. Non-device path: Allocate VMA (existing) ───
    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
```

- [ ] **Step 3: 创建 fb.h**

```c
// kernel/include/kernel/fb.h
#ifndef _KERNEL_FB_H
#define _KERNEL_FB_H

#include <stdint.h>

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bpp;
    uint32_t format;   // 0 = XRGB8888
} __attribute__((packed));

// ioctl: give up framebuffer to userspace
#define FBIOSURRENDER  0x00004601

#endif
```

- [ ] **Step 4: 创建 kernel/driver/fb.c**

```c
// kernel/driver/fb.c
#include <kernel/fb.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <kernel/vma.h>
#include <kernel/printk.h>
#include <kernel/console.h>
#include <kernel/slab.h>
#include <string.h>
#include <stddef.h>

// External framebuffer globals from printk.c
extern position Pos;

// ── fb_read: return metadata struct ────
static int fb_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node;
    if (offset >= sizeof(struct fb_info)) return 0;  // EOF
    if (offset + size > sizeof(struct fb_info))
        size = sizeof(struct fb_info) - offset;

    struct fb_info info = {
        .width  = (uint32_t)Pos.XResolution,
        .height = (uint32_t)Pos.YResolution,
        .stride = (uint32_t)(Pos.XResolution * sizeof(uint32_t)),
        .bpp    = 32,
        .format = 0,  // XRGB8888
    };
    memcpy(buffer, ((uint8_t *)&info) + offset, size);
    return (int)size;
}

// ── fb_write: not supported ────
static int fb_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)buffer;
    return (int)size;  // discard (fb written via mmap)
}

// ── fb_mmap: eager PTE fill ────
static int fb_mmap(vfs_node_t *node, vma_t *vma)
{
    (void)node;
    uint64_t pitch = Pos.XResolution * sizeof(uint32_t);
    uint64_t fb_size = pitch * Pos.YResolution;

    if (vma->vm_pgoff != 0)               return -EINVAL;
    if (vma->vm_end - vma->vm_start > fb_size) return -EINVAL;
    if (!(vma->vm_flags & VM_SHARED))     return -EINVAL;

    vma->vm_flags |= VM_IO;
    vma->vm_page_prot = PAGE_USER_4K | PAGE_PWT | PAGE_PCD;

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_4K_SIZE) {
        uint64_t pa = (uint64_t)Pos.Phy_addr + (va - vma->vm_start);
        vmm_map_4k_page(user_pml4, va, pa, vma->vm_page_prot);
    }
    flush_tlb();
    return 0;
}

// ── fb_ioctl ────
static int fb_ioctl(vfs_node_t *node, int cmd, void *arg)
{
    (void)node; (void)arg;
    if (cmd == FBIOSURRENDER) {
        console_surrender_fb();
        return 0;
    }
    return -ENOTTY;
}

// ── ops ────
static const struct devfs_ops fb_ops = {
    .open  = NULL,   // default FD_DEV
    .read  = fb_read,
    .write = fb_write,
    .mmap  = fb_mmap,
    .ioctl = fb_ioctl,
};

// ── init ────
void fb_init(void)
{
    devfs_register_chrdev("fb", NULL, &fb_ops);
    log_info("fb: /dev/fb registered (%ux%u, %u bpp)\n",
             (unsigned)Pos.XResolution, (unsigned)Pos.YResolution, 32);
}
```

- [ ] **Step 5: main.c 中注册 /dev/fb**

在 `kernel/kernel/main.c` 中添加 `extern void fb_init(void);` 并在 subsys 或设备初始化阶段调用 `fb_init();`。

- [ ] **Step 6: 编译验证**

```bash
make clean && make
```

- [ ] **Step 7: Commit**

```bash
git add kernel/driver/fb.c kernel/include/kernel/fb.h \
        kernel/include/kernel/vma.h kernel/memory/vma.c kernel/kernel/main.c
git commit -m "feat: VM_IO + do_mmap device dispatch + /dev/fb mmap + FBIOSURRENDER ioctl

- VM_IO = 0x80 for device MMIO mappings
- do_mmap step 4 checks node->ops->mmap, pre-allocates VMA, calls handler
- /dev/fb: fb_read (metadata), fb_mmap (eager PTE fill + flush_tlb), fb_ioctl(FBIOSURRENDER)"
```

---

### Task 4: tty.c 退化

**Files:**
- Modify: `kernel/include/kernel/tty.h`
- Modify: `kernel/tty/tty.c`
- Modify: `kernel/driver/keyboard.c`

**Interfaces:**
- Produces: slimmed `tty_t` (no canonical fields), raw `tty_read`, no `tty_canon_process`, no TTY_L_* macros

- [ ] **Step 1: 精简 tty_t**

在 `kernel/include/kernel/tty.h` 中删除 canonical 字段：

删除：
```c
    char        line[TTY_BUF_SIZE];    // REMOVE
    int         line_len;              // REMOVE
    int         read_pos;              // REMOVE
    bool        line_ready;            // REMOVE
    uint8_t     lflag;                 // REMOVE
    int64_t     pgrp;                  // REMOVE
```

删除宏：
```c
// REMOVE:
#define TTY_L_ICANON  (1 << 0)
#define TTY_L_ECHO    (1 << 1)
#define TTY_L_ISIG    (1 << 2)
```

`cooked_lock` 改名为 `ring_lock`，`cooked[]` 改名为 `ring[]`。

- [ ] **Step 2: 简化 tty_read**

重写 `tty.c` 的 `tty_read`——删除 canonical 处理，直接从 ring buffer 拷贝：

```c
int tty_read(tty_t *tty, char *buf, int size, bool nonblock)
{
    if (!tty || !buf || size <= 0) return 0;

    for (;;) {
        // Phase 1: drain ring buffer directly
        int copied = 0;
        {
            uint64_t flags = spin_lock_irqsave(&tty->ring_lock);
            while (copied < size && tty->head != tty->tail) {
                buf[copied++] = tty->ring[tty->tail];
                tty->tail = (tty->tail + 1) % TTY_BUF_SIZE;
            }
            spin_unlock_irqrestore(&tty->ring_lock, flags);
        }

        if (copied > 0) return copied;
        if (nonblock) return 0;
        if (signal_pending_fatal()) return 0;

        // Phase 2: sleep on wait queue
        uint64_t wq_flags = spin_lock_irqsave(&tty->read_wait_lock);
        current->state = TASK_INTERRUPTIBLE;
        list_add_to_before(&tty->read_wait, &current->io_wait_node);

        // Double-check
        if (tty->head != tty->tail) {
            list_del_init(&current->io_wait_node);
            current->state = TASK_RUNNING;
            spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);
            continue;
        }
        spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);

        schedule();
        do_signal_delivery(NULL);

        // Cleanup
        wq_flags = spin_lock_irqsave(&tty->read_wait_lock);
        if (!list_is_empty(&current->io_wait_node))
            list_del_init(&current->io_wait_node);
        spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);

        if (signal_pending_fatal()) return 0;
    }
}
```

- [ ] **Step 3: 简化 tty_push_input**

将 `tty_cooked_push` 改名为 `tty_ring_push`，`tty_cooked_empty` 改名为 `tty_ring_empty`。

`tty_push_input` 维持不变——仍然从 IRQ 推入 ring buffer 并唤醒等待者。

- [ ] **Step 4: 删除 tty_canon_process 和 tty_cooked_pop**

彻底删除这两个函数。删除 `tty_ioctl` 中所有 ICANON/ECHO/ISIG 相关的 lflag 操作。

- [ ] **Step 5: keyboard.c 方向键适配**

在 `kernel/driver/keyboard.c` 的 `translate_and_push` 中，找到：
```c
if (!(kbd_tty->lflag & TTY_L_ICANON)) {
    switch (c) { ... }
}
```

替换为无条件：
```c
switch (c) {
case K_UP:    push_vt100_seq(kbd_tty, 'A'); break;
case K_DOWN:  push_vt100_seq(kbd_tty, 'B'); break;
case K_LEFT:  push_vt100_seq(kbd_tty, 'D'); break;
case K_RIGHT: push_vt100_seq(kbd_tty, 'C'); break;
case K_HOME:  push_vt100_seq(kbd_tty, 'H'); break;
case K_END:   push_vt100_seq(kbd_tty, 'F'); break;
default: break;
}
```

- [ ] **Step 6: 编译验证**

```bash
make clean && make
```

- [ ] **Step 7: Commit**

```bash
git add kernel/include/kernel/tty.h kernel/tty/tty.c kernel/driver/keyboard.c
git commit -m "refactor: degrade tty.c to raw ring buffer — remove canonical processing

- Remove tty_canon_process, TTY_L_ICANON/ECHO/ISIG macros
- Remove canonical line buffer (line[], line_len, read_pos, line_ready, lflag, pgrp)
- tty_read simplified: direct ring buffer copy, no line discipline
- keyboard.c direction keys always emit VT100 sequences (no ICANON check)"
```

---

### Task 5: console.c 退化

**Files:**
- Modify: `kernel/include/kernel/console.h`
- Modify: `kernel/tty/console.c`
- Modify: `kernel/driver/pit.c`

**Interfaces:**
- Produces: `console_surrender_fb()`, `console_force_enable()`, degraded `console_putchar()`

- [ ] **Step 1: 删除 CSI 状态机**

在 `kernel/tty/console.c` 中删除：
- `enum csi_state` 定义
- `cs`, `cs_param`, `cs_qmark` 静态变量
- `console_cursor_left`, `console_cursor_right`, `console_clear_to_eol`, `console_advance` 辅助函数
- 整个 `console_putchar` 的 CSI switch 结构

保留：
- `term_cursor_row`, `term_cursor_col` 静态变量
- `term_fg`, `term_bg`, `term_initialized`
- `console_scroll()`, `putchar_at()` (在 printk.c 中)

- [ ] **Step 2: 删除光标闪烁**

删除：
- `term_cursor_visible`, `term_blink_on`, `term_blink_counter` 静态变量
- `console_draw_blink()` 函数
- `console_blink_tick()` 函数

- [ ] **Step 3: 重写 console_putchar**

```c
void console_putchar(char c)
{
    if (!term_initialized) return;

    switch (c) {
    case '\n':
        term_cursor_col = 0;
        term_cursor_row++;
        break;
    case '\r':
        term_cursor_col = 0;
        break;
    case '\b': case 0x7F:
        if (term_cursor_col > 0) term_cursor_col--;
        break;
    case '\t':
        term_cursor_col = (term_cursor_col + 8) & ~7;
        break;
    default:
        if ((unsigned char)c >= ' ') {
            putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, c);
            term_cursor_col++;
        }
        break;
    }

    int max_cols = (int)(Pos.XResolution / font->width);
    if (term_cursor_col >= max_cols) {
        term_cursor_col = 0;
        term_cursor_row++;
    }
    int max_rows = (int)(Pos.YResolution / font->height);
    if (term_cursor_row >= max_rows)
        console_scroll();
}
```

- [ ] **Step 4: 添加生存期管理函数**

```c
void console_surrender_fb(void)
{
    term_initialized = false;
}

void console_force_enable(void)
{
    term_initialized = true;
}
```

- [ ] **Step 5: 更新 console.h**

```c
#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

void console_init(void);
void console_putchar(char c);
void console_surrender_fb(void);
void console_force_enable(void);

#endif
```

移除 `console_blink_tick` 声明。

- [ ] **Step 6: 移除 PIT 中的 console_blink_tick 调用**

在 `kernel/driver/pit.c` 中删除 `console_blink_tick();` 调用行。

- [ ] **Step 7: main.c 中移除 console_blink_tick import**

如果 `main.c` 有 `extern void console_blink_tick(void);` 声明则删除。

- [ ] **Step 8: 编译验证**

```bash
make clean && make
```

- [ ] **Step 9: Commit**

```bash
git add kernel/include/kernel/console.h kernel/tty/console.c \
        kernel/driver/pit.c kernel/kernel/main.c
git commit -m "refactor: degrade console.c to emergency mode — remove CSI parser, cursor blink

- Delete VT100 CSI state machine (CSI_NORMAL/ESC/BRACKET/PARAM/QMARK)
- Delete cursor blink (console_draw_blink, console_blink_tick, PIT callback)
- console_putchar: only \\n \\r \\b \\t + printable chars + scroll
- Add console_surrender_fb() / console_force_enable() for terminal.elf handoff"
```

---

### Task 6: PTY 实现 + current->ctty + /dev/tty 魔数

**Files:**
- Create: `kernel/include/kernel/pty.h`
- Create: `kernel/driver/pty.c`
- Modify: `kernel/include/kernel/task.h`
- Modify: `kernel/sched/task.c`
- Modify: `kernel/fs/devfs.c`
- Modify: `kernel/kernel/main.c`

**Interfaces:**
- Produces: `pty_t`, `pty_alloc`, `ptmx_open`, slave ops, `ctty_type` enum, `current->ctty`, `tty_dev_open`

- [ ] **Step 1: 创建 pty.h**

```c
// kernel/include/kernel/pty.h
#ifndef _KERNEL_PTY_H
#define _KERNEL_PTY_H

#include <stdint.h>
#include <stdbool.h>
#include <termios.h>
#include <kernel/file.h>    // for pipe_t

#define PTY_MAX  8

typedef struct pty_struct {
    int         index;
    bool        allocated;

    pipe_t     *master_to_slave;
    pipe_t     *slave_to_master;

    struct termios  term;
    uint16_t    ws_row;
    uint16_t    ws_col;
    pid_t       pgrp;
} pty_t;

// Global PTY table
extern pty_t pty_table[PTY_MAX];

// Initialize PTY subsystem
void pty_init(void);

// Allocate an idle PTY slot, create two pipes. Returns NULL if none available.
pty_t *pty_alloc(void);

#endif
```

- [ ] **Step 2: 创建 pty.c — pty_alloc + slave ops**

```c
// kernel/driver/pty.c
#include <kernel/pty.h>
#include <fs/devfs.h>
#include <kernel/slab.h>
#include <kernel/tty.h>
#include <kernel/poll.h>
#include <kernel/task.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

pty_t pty_table[PTY_MAX];

// ── pty_alloc: 找空闲 slot，创建两个 pipe ──
pty_t *pty_alloc(void)
{
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) {
            pty_t *pty = &pty_table[i];
            memset(pty, 0, sizeof(*pty));
            pty->index = i;
            pty->allocated = true;

            // Create pipes
            pty->master_to_slave = pipe_alloc();
            pty->slave_to_master = pipe_alloc();
            if (!pty->master_to_slave || !pty->slave_to_master) {
                if (pty->master_to_slave) pipe_free(pty->master_to_slave);
                if (pty->slave_to_master) pipe_free(pty->slave_to_master);
                pty->allocated = false;
                return NULL;
            }

            // Initialize termios: raw mode
            pty->term.c_lflag = 0;  // !ICANON, !ECHO, !ISIG
            pty->term.c_iflag = ICRNL;
            pty->term.c_oflag = OPOST | ONLCR;
            // c_cflag: B38400 | CS8 | CREAD — default terminal speed
            pty->ws_row = 25;
            pty->ws_col = 80;
            pty->pgrp = 0;

            return pty;
        }
    }
    return NULL;
}
```

- [ ] **Step 3: slave 端 ops 实现**

**前置：devfs_get_private API**。Slave handler 通过 `node->fs_data`（device index）定位 `devices[idx].private_data`。在 `kernel/fs/devfs.c` 中新增：

```c
void *devfs_get_private(vfs_node_t *node)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return NULL;
    return devices[idx].private_data;
}
```

在 `devfs.h` 中声明 `void *devfs_get_private(struct vfs_node *node);`。

**pty_slave_read / pty_slave_write**（直接使用 devfs_get_private）：

```c
static int pty_slave_read(vfs_node_t *node, uint64_t offset,
                          uint64_t size, void *buffer)
{
    (void)offset;
    pty_t *pty = devfs_get_private(node);
    if (!pty || !pty->master_to_slave) return -1;

    uint64_t flags = spin_lock_irqsave(&pty->master_to_slave->lock);
    uint8_t *dst = (uint8_t *)buffer;
    int total = 0;
    while (total < (int)size && pty->master_to_slave->head != pty->master_to_slave->tail) {
        dst[total++] = pty->master_to_slave->buf[pty->master_to_slave->tail];
        pty->master_to_slave->tail = (pty->master_to_slave->tail + 1) % PIPE_SIZE;
    }
    spin_unlock_irqrestore(&pty->master_to_slave->lock, flags);
    return total;
}

static int pty_slave_write(vfs_node_t *node, uint64_t offset,
                           uint64_t size, void *buffer)
{
    (void)offset;
    pty_t *pty = devfs_get_private(node);
    if (!pty || !pty->slave_to_master) return -1;

    uint8_t *src = (uint8_t *)buffer;
    uint64_t flags = spin_lock_irqsave(&pty->slave_to_master->lock);
    int total = 0;
    while (total < (int)size) {
        int next = (pty->slave_to_master->head + 1) % PIPE_SIZE;
        if (next == pty->slave_to_master->tail) break;
        pty->slave_to_master->buf[pty->slave_to_master->head] = src[total++];
        pty->slave_to_master->head = next;
    }
    if (total > 0)
        pipe_wake_readers(pty->slave_to_master);
    spin_unlock_irqrestore(&pty->slave_to_master->lock, flags);
    return total;
}

- [ ] **Step 4: slave ioctl 实现**

```c
static int pty_slave_ioctl(vfs_node_t *node, int cmd, void *arg)
{
    pty_t *pty = devfs_get_private(node);
    if (!pty) return -ENODEV;

    switch (cmd) {
    case TCGETS:
        if (!arg) return -EINVAL;
        memcpy(arg, &pty->term, sizeof(struct termios));
        return 0;
    case TCSETS:
    case TCSETSW:
        if (!arg) return -EINVAL;
        memcpy(&pty->term, arg, sizeof(struct termios));
        return 0;
    case TIOCGWINSZ:
        if (!arg) return -EINVAL;
        ((struct winsize *)arg)->ws_row = pty->ws_row;
        ((struct winsize *)arg)->ws_col = pty->ws_col;
        return 0;
    case TIOCSWINSZ:
        if (!arg) return -EINVAL;
        pty->ws_row = ((struct winsize *)arg)->ws_row;
        pty->ws_col = ((struct winsize *)arg)->ws_col;
        // V1 defer: no SIGWINCH (no kill_pgrp)
        return 0;
    case TIOCGPGRP:
        if (!arg) return -EINVAL;
        *(pid_t *)arg = pty->pgrp;
        return 0;
    case TIOCSPGRP:
        if (!arg) return -EINVAL;
        pty->pgrp = *(pid_t *)arg;
        return 0;
    default:
        return -ENOTTY;
    }
}
```

- [ ] **Step 5: slave poll**

```c
static uint32_t pty_slave_poll(void *priv, struct poll_table *pt)
{
    pty_t *pty = (pty_t *)priv;
    uint32_t mask = 0;
    if (pty->master_to_slave) {
        uint64_t flags = spin_lock_irqsave(&pty->master_to_slave->lock);
        if (pty->master_to_slave->head != pty->master_to_slave->tail)
            mask |= POLLIN | POLLRDNORM;
        else if (pt && !pt->triggered)
            poll_wait(pt, &pty->master_to_slave->read_wait,
                      &pty->master_to_slave->lock);
        spin_unlock_irqrestore(&pty->master_to_slave->lock, flags);
    }
    if (pty->slave_to_master) {
        uint64_t flags = spin_lock_irqsave(&pty->slave_to_master->lock);
        int next = (pty->slave_to_master->head + 1) % PIPE_SIZE;
        if (next != pty->slave_to_master->tail)
            mask |= POLLOUT | POLLWRNORM;
        else if (pt && !pt->triggered)
            poll_wait(pt, &pty->slave_to_master->write_wait,
                      &pty->slave_to_master->lock);
        spin_unlock_irqrestore(&pty->slave_to_master->lock, flags);
    }
    return mask;
}
```

- [ ] **Step 6: ptmx_open + slave 注册**

```c
// ── Slave ops ──
static const struct devfs_ops pty_slave_ops = {
    .open  = NULL,
    .read  = pty_slave_read,
    .write = pty_slave_write,
    .poll  = pty_slave_poll,
    .ioctl = pty_slave_ioctl,
};

// ── ptmx_open: devfs open 回调 ──
static int ptmx_open(const char *name, file_t **out_file)
{
    (void)name;
    pty_t *pty = pty_alloc();
    if (!pty) return -ENOMEM;

    // Register slave in devfs
    char slave_name[16];
    snprintf(slave_name, sizeof(slave_name), "pts%d", pty->index);
    devfs_register_chrdev(slave_name, pty, &pty_slave_ops);

    // Create master file_t
    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type  = FD_PTY_MASTER;
    f->pty   = pty;
    f->flags = O_RDWR;
    *out_file = f;
    return 0;
}

// ── ptmx ops ──
static const struct devfs_ops ptmx_ops = {
    .open  = ptmx_open,
    .read  = NULL,
    .write = NULL,
};

void pty_init(void)
{
    memset(pty_table, 0, sizeof(pty_table));
    devfs_register_chrdev("ptmx", NULL, &ptmx_ops);
    log_info("pty: /dev/ptmx registered (max %d pairs)\n", PTY_MAX);
}
```

- [ ] **Step 7: task_struct 添加 ctty**

在 `kernel/include/kernel/task.h` 中找到 `task_struct` 定义，添加：

```c
// 在 task_struct 结构体中，exit_code 或 signal 字段附近添加：
    // ── Controlling terminal ──
    enum ctty_type { CTTY_NONE = 0, CTTY_PHYS, CTTY_PTY } ctty_type;
    void *ctty;                    // points to tty_t (CTTY_PHYS) or pty_t (CTTY_PTY)
```

（如果 `enum ctty_type` 定义在 struct 内部不合适，可以在文件顶部定义。）

- [ ] **Step 8: fork 继承 ctty**

在 `kernel/sched/task.c` 的 `fork()` 函数中，子进程初始化后添加：

```c
    child->ctty_type = current->ctty_type;
    child->ctty      = current->ctty;
```

- [ ] **Step 9: PTY slave open 设置 ctty**

在 `pty_slave_ops` 中。但是 `devfs_ops.open` 回调是为 clone 设备设计的（如 ptmx）。对于普通 slave open，需要在 `pty_slave_read` 被首次调用之前设置 ctty。更简单：在 slave 的 `open` 回调中设置：

```c
// Replace pty_slave_ops.open from NULL to:
static int pty_slave_open(const char *name, file_t **out_file)
{
    // Default FD_DEV behavior — but first set ctty
    // Find pty from device table by name
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) continue;
        char expected[16];
        snprintf(expected, sizeof(expected), "pts%d", pty_table[i].index);
        if (strstr(name, expected)) {
            current->ctty_type = CTTY_PTY;
            current->ctty = &pty_table[i];
            return -ENOSYS;  // fall through to default FD_DEV
        }
    }
    return -ENOSYS;  // not found, default FD_DEV
}
```

更新 `pty_slave_ops.open = pty_slave_open`。

- [ ] **Step 10: /dev/tty 魔数 open handler**

在 `kernel/fs/devfs.c` 中注册 `/dev/tty` 设备（与物理 tty 使用不同名称 `/dev/tty0` 注册物理 TTY），添加魔数 open handler：

首先——将物理 TTY 注册改为 `kbd_tty` 作为 private_data：

在 `devfs_init` 中：
```c
extern tty_t *kbd_tty;  // from keyboard.c
devfs_register_chrdev("tty", kbd_tty, &tty_ops_v1);     // magic /dev/tty
devfs_register_chrdev("tty0", kbd_tty, &tty_ops_v1);    // physical tty alias
```

然后为物理 TTY 的 ops 添加 open 回调（用于 /dev/tty 魔数）：

```c
// /dev/tty 魔数 open
static int tty_magic_open(const char *name, file_t **out_file)
{
    if (strcmp(name, "/dev/tty") != 0)
        return -ENOSYS;

    void *target = NULL;
    if (current->ctty_type == CTTY_PTY) {
        target = current->ctty;
    } else {
        target = kbd_tty;  // CTTY_NONE or CTTY_PHYS → physical TTY
    }

    // Find the devfs device whose private_data matches
    for (int i = 0; i < device_count; i++) {
        if (devices[i].private_data == target
            && devices[i].type == VFS_CHRDEV
            && devices[i].registered) {
            vfs_node_t *node = vfs_node_alloc();
            node->type = VFS_CHRDEV;
            node->fs_data = (void *)(uintptr_t)i;
            node->ops = &devfs_ops;
            node->refcount = 1;

            *out_file = file_alloc();
            if (!*out_file) {
                vfs_node_put(node);
                return -ENOMEM;
            }
            (*out_file)->type = FD_DEV;
            (*out_file)->node = node;
            // flags set by caller
            return 0;
        }
    }
    return -ENXIO;
}
```

更新 `tty_ops_v1.open = tty_magic_open`。

- [ ] **Step 11: main.c 中初始化 PTY**

```c
extern void pty_init(void);
// 在设备初始化阶段调用
pty_init();
```

- [ ] **Step 12: 编译验证**

```bash
make clean && make
```

- [ ] **Step 13: Commit**

（Commit 将在 Task 8 集成测试后一起提交，或此处独立 commit。）

---

### Task 7: terminal.elf

**Files:**
- Create: `user/terminal.c`
- Modify: `user/Makefile`

**Interfaces:**
- Consumes: `/dev/tty` (raw), `/dev/fb` (mmap), `/dev/ptmx` (FD_PTY_MASTER), `/dev/pts0` (slave)
- Produces: `terminal.elf` binary

- [ ] **Step 1: 创建 user/terminal.c — 完整实现**

由于 terminal.elf 代码量约 400 行，以下给出完整结构。每个函数带完整实现。

```c
/* terminal.elf — OS01 userspace VT100 terminal emulator
 *
 * Architecture:
 *   open /dev/tty (phys kb/serial) → read input
 *   open /dev/fb → mmap framebuffer → render output
 *   open /dev/ptmx → FD_PTY_MASTER → ash I/O
 *   open /dev/pts0 → set ctty → fork ash
 *
 * Input:  /dev/tty → handle_input() → write(ptmx)
 * Output: read(ptmx) → handle_output() → render to fb
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/poll.h>    // struct pollfd, POLLIN
#include <fcntl.h>       // O_RDONLY, O_RDWR
#include <sys/mman.h>    // mmap, MAP_SHARED, PROT_WRITE
#include <sys/ioctl.h>   // ioctl
#include <termios.h>     // struct termios, TCGETS, ICANON, ECHO, ISIG

// ── fb_info (must match kernel definition) ──
struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bpp;
    uint32_t format;
} __attribute__((packed));

#define FBIOSURRENDER  0x00004601

// ── PSF2 font header ──
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t height;
    uint32_t width;
} psf2_t;

// Embedded font data (from objcopy)
extern char _binary_kernel_font_psf_start[];
extern char _binary_kernel_font_psf_end[];

// ── Terminal state ──
static uint32_t *fb;
static struct fb_info fb_info;
static psf2_t *font;
static int term_col, term_row;
static int term_rows, term_cols;
static uint32_t fg_color = 0xFFFFFFFF;  // white
static uint32_t bg_color = 0x00000000;  // black
static bool cursor_visible = true;

// ── Input line buffer ──
static char line_buf[256];
static int  line_len;
static bool echo_on = true;

// ── VT100 output parser state ──
enum { S_NORMAL, S_ESC, S_CSI_PARAM };
static int csi_state = S_NORMAL;
static int csi_param = 0;
static bool csi_qmark = false;

// ── Renderer ──────────────────────────────────────

static void put_glyph(int col, int row, uint32_t fg, uint32_t bg, char c)
{
    if (col < 0 || col >= term_cols || row < 0 || row >= term_rows) return;

    unsigned char *glyph = (unsigned char *)font + font->headersize
        + ((c > 0 && c < (int)font->numglyph) ? c : 0) * font->bytesperglyph;

    for (uint32_t y = 0; y < font->height; y++) {
        uint32_t *line = fb + (row * font->height + y) * (fb_info.stride / 4)
                         + col * font->width;
        uint32_t test = 0x100;
        for (uint32_t x = 0; x < font->width; x++) {
            test >>= 1;
            *line++ = (*glyph & test) ? fg : bg;
        }
        glyph++;
    }
}

static void fb_scroll(void)
{
    uint32_t line_bytes = font->height * fb_info.stride;
    uint32_t total = line_bytes * term_rows;
    memmove((void *)fb, (void *)(fb + line_bytes / 4), total - line_bytes);
    memset((void *)(fb + (total - line_bytes) / 4), 0, line_bytes);
    term_row = term_rows - 1;
}

// ── Output handler (VT100 parser) ─────────────────

static void output_char(char c)
{
    if (csi_state == S_NORMAL && c == '\x1b') {
        csi_state = S_ESC;
        return;
    }
    if (csi_state == S_ESC) {
        if (c == '[') { csi_state = S_CSI_PARAM; csi_param = 0; csi_qmark = false; return; }
        csi_state = S_NORMAL;
        return;
    }
    if (csi_state == S_CSI_PARAM) {
        if (c == '?') { csi_qmark = true; return; }
        if (c >= '0' && c <= '9') { csi_param = csi_param * 10 + (c - '0'); return; }
        // Terminal character
        switch (c) {
        case 'A': term_row -= (csi_param ? csi_param : 1); if (term_row < 0) term_row = 0; break;
        case 'B': term_row += (csi_param ? csi_param : 1); if (term_row >= term_rows) term_row = term_rows - 1; break;
        case 'C': term_col += (csi_param ? csi_param : 1); if (term_col >= term_cols) term_col = term_cols - 1; break;
        case 'D': term_col -= (csi_param ? csi_param : 1); if (term_col < 0) term_col = 0; break;
        case 'K':
            if (csi_param == 0) for (int x = term_col; x < term_cols; x++) put_glyph(x, term_row, fg_color, bg_color, ' ');
            else if (csi_param == 1) for (int x = 0; x <= term_col; x++) put_glyph(x, term_row, fg_color, bg_color, ' ');
            else if (csi_param == 2) for (int x = 0; x < term_cols; x++) put_glyph(x, term_row, fg_color, bg_color, ' ');
            break;
        case 'J': if (csi_param == 2) { for (int r = 0; r < term_rows; r++) for (int x = 0; x < term_cols; x++) put_glyph(x, r, fg_color, bg_color, ' '); term_row = 0; term_col = 0; } break;
        case 'H': term_row = 0; term_col = 0; break;
        case 'h': if (csi_qmark && csi_param == 25) cursor_visible = true; break;
        case 'l': if (csi_qmark && csi_param == 25) cursor_visible = false; break;
        }
        csi_state = S_NORMAL;
        return;
    }

    // Normal character
    switch (c) {
    case '\n': term_col = 0; term_row++; break;
    case '\r': term_col = 0; break;
    case '\b': case 0x7F: if (term_col > 0) term_col--; break;
    case '\t': term_col = (term_col + 8) & ~7; break;
    default:
        if ((unsigned char)c >= ' ') {
            put_glyph(term_col, term_row, fg_color, bg_color, c);
            term_col++;
        }
        break;
    }

    if (term_col >= term_cols) { term_col = 0; term_row++; }
    if (term_row >= term_rows) fb_scroll();
}

static void handle_output(char *buf, int n)
{
    for (int i = 0; i < n; i++)
        output_char(buf[i]);
}

// ── Input handler ─────────────────────────────────

static void render_backspace(void)
{
    if (term_col > 0) term_col--;
    put_glyph(term_col, term_row, fg_color, bg_color, ' ');
}

static void handle_input(char *buf, int n, int pty_fd, int ash_pid)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];

        // Raw mode check: poll termios every read cycle
        struct termios term;
        bool is_cooked = true;
        if (ioctl(pty_fd, TCGETS, &term) == 0) {
            if (!(term.c_lflag & ICANON)) {
                // Raw mode: passthrough
                write(pty_fd, &c, 1);
                continue;
            }
        }

        // Cooked mode
        if (c == '\r' || c == '\n') {
            line_buf[line_len++] = '\n';
            write(pty_fd, line_buf, line_len);
            line_len = 0;
            if (echo_on) { output_char('\r'); output_char('\n'); }
        } else if (c == '\x7f' || c == '\b') {
            if (line_len > 0) { line_len--; if (echo_on) render_backspace(); }
        } else if (c == '\x03') {
            if (term.c_lflag & ISIG) kill(ash_pid, SIGINT);
        } else if (c == '\x04' && line_len == 0) {
            // ^D: close write end → send EOF
            // V1: just write nothing (ash detects EOF via pipe close)
        } else if (c == '\x1b') {
            // ESC sequence (arrow keys): read next 2 bytes and passthrough
            char seq[3] = { '\x1b', 0, 0 };
            // Non-blocking read of 2 more bytes
            // V1 simplified: passthrough ESC alone if immediate read fails
            if (i + 2 < n) {
                seq[1] = buf[++i];
                seq[2] = buf[++i];
            }
            write(pty_fd, seq, (seq[1] ? (seq[2] ? 3 : 2) : 1));
        } else if (c >= ' ') {
            line_buf[line_len++] = c;
            if (echo_on) output_char(c);
        }
    }
}

// ── Main ──────────────────────────────────────────

int main(void)
{
    // 1. Open physical TTY (before ctty is set)
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) { write(2, "terminal: cannot open /dev/tty\n", 32); return 1; }

    // 2. Open framebuffer
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) { write(2, "terminal: cannot open /dev/fb\n", 31); return 1; }

    read(fb_fd, &fb_info, sizeof(fb_info));
    fb = mmap(NULL, fb_info.height * fb_info.stride,
              PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((int64_t)fb < 0) { write(2, "terminal: mmap fb failed\n", 25); return 1; }

    ioctl(fb_fd, FBIOSURRENDER, NULL);

    // Setup PSF2 font
    font = (psf2_t *)_binary_kernel_font_psf_start;
    term_cols = fb_info.width / font->width;
    term_rows = fb_info.height / font->height;
    term_col = 0; term_row = 0;

    // 3. Open PTY master
    int pty_fd = open("/dev/ptmx", O_RDWR);
    if (pty_fd < 0) { write(2, "terminal: cannot open /dev/ptmx\n", 32); return 1; }

    // 4. Open slave (becomes ctty)
    int slave = open("/dev/pts0", O_RDWR);
    if (slave < 0) { write(2, "terminal: cannot open /dev/pts0\n", 32); return 1; }

    // 5. Fork ash
    int ash_pid = fork();
    if (ash_pid == 0) {
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
        char *argv[] = { "/bin/ash", NULL };
        exec("/bin/ash", argv, NULL);
        exit(1);
    }

    close(slave);

    // Clear our ctty
    // (userspace cannot directly set current->ctty_type; fork inherits then we
    //  closed slave — future /dev/tty opens by terminal.elf will go to physical tty)

    // 6. Main loop
    struct pollfd fds[2] = {
        { .fd = tty_fd, .events = POLLIN },
        { .fd = pty_fd, .events = POLLIN },
    };
    char buf[256];

    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) break;

        if (fds[0].revents & POLLIN) {
            int n = read(tty_fd, buf, sizeof(buf));
            if (n > 0) handle_input(buf, n, pty_fd, ash_pid);
        }
        if (fds[1].revents & POLLIN) {
            int n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) handle_output(buf, n);
            else if (n == 0) break;  // EOF: ash exited
        }
    }

    return 0;
}
```

- [ ] **Step 2: 更新 user/Makefile**

添加 terminal.elf 构建规则：

```makefile
# Font binary object
user/font.psf.o: kernel/font.psf
	$(OBJCOPY) -B i386 -I binary -O elf64-x86-64 kernel/font.psf user/font.psf.o

# terminal.elf
user/terminal.elf: user/terminal.o user/font.psf.o
	$(LD) $(LDFLAGS) -o $@ $^ $(LDLIBS)

user/terminal.o: user/terminal.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
```

- [ ] **Step 3: 编译验证**

```bash
make user/terminal.elf
```

- [ ] **Step 4: Commit**

```bash
git add user/terminal.c user/Makefile
git commit -m "feat: terminal.elf — userspace VT100 terminal with PSF2 rendering

- PSF2 glyph rendering via mmap'd /dev/fb
- VT100 CSI subset (cursor movement, clear line/screen, cursor visibility)
- Dual-mode input handler (cooked with echo + line editing, raw with passthrough)
- PTY master/slave setup, fork ash, main poll loop"
```

---

### Task 8: busybox ash 集成 + init 编排

**Files:**
- Verify: busybox `.config` (FEATURE_EDITING)
- Modify: init 启动逻辑（或 terminal.elf 自启动）

**Note**: 当前 init 直接启动 ash。改造为启动 terminal.elf（terminal.elf 内部 fork ash）。

检查 init 实现位于何处（可能在 `kernel/kernel/main.c` 的最后一个阶段，或用户态的 `/bin/init`）。

如果 init 是内核阶段（`kernel/kernel/main.c` 中 fork 出第一个用户进程）：

- [ ] **Step 1: 检查 busybox FEATURE_EDITING 是否启用**

```bash
grep CONFIG_FEATURE_EDITING thirdpart/busybox-1.36.1/.config
```

如果未启用，在 busybox 配置中启用：
```bash
cd thirdpart/busybox-1.36.1 && make menuconfig  # or
# 手动在 .config 中设置 CONFIG_FEATURE_EDITING=y
```

如果 `CONFIG_ASH=y` 但 `CONFIG_FEATURE_EDITING` 为 n，改为 `=y`。

- [ ] **Step 2: 修改 init 逻辑**

如果 init 在 `kernel/kernel/main.c`：

查找类似 `exec("/bin/ash", ...)` 的行，改为 `exec("/terminal.elf", ...)`。

如果 init 是用户空间 `/bin/init` 脚本——在脚本中改为启动 `/terminal.elf`。

- [ ] **Step 3: 集成测试**

```bash
make run
```

验证：
1. 系统启动后出现图形终端（PSF2 字体渲染的 ash 提示符）
2. 键盘输入可见（echo 正常）
3. 输入命令可执行（`ls`, `echo hello`）
4. Ctrl-C 中断正常运行
5. Serial 输出仍有内核 log

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel/main.c  # or wherever init changes
git commit -m "feat: init starts terminal.elf instead of ash directly

- terminal.elf spawns ash internally via PTY pair
- Verified FEATURE_EDITING=y in busybox config
- Integration test: graphics terminal + keyboard input + command execution"
```

---

### Task 9: 测试 + 文档

- [ ] **Step 1: 手动功能测试清单**

1. 启动 → 可见终端渲染 (PSF2 white-on-black)
2. 键盘输入 → 字符在终端上 echo
3. `ls` → 列出 `/bin/` 目录
4. `echo hello world` → 显示 "hello world"
5. Ctrl-C → 发送 SIGINT，当前输入行被清除
6. Backspace → 删除最后一个字符
7. `cat /dev/zero | head -c 1000` → 大量输出正确滚动
8. `exit` → ash 退出 → terminal.elf 结束 → init 可重启 terminal.elf

- [ ] **Step 2: 串口验证**

`make run` 后检查 serial 输出：kernel log 正常，无 panic。

- [ ] **Step 3: 更新相关文档**

在 `README.md` 或 `AGENTS.md` 中添加 terminal.elf 说明：
```
- terminal.elf: 用户态 VT100 终端，渲染到 framebuffer，通过 PTY 连接 ash
```

- [ ] **Step 4: Final commit**

```bash
git add README.md  # or docs/
git commit -m "docs: add terminal.elf integration test checklist and usage notes"
```

---

## 实现顺序

1. Task 1: devfs_ops struct 重构
2. Task 2: FD_PTY_MASTER + file_t 扩展
3. Task 3: VM_IO + do_mmap 设备分派 + /dev/fb
4. Task 4: tty.c 退化
5. Task 5: console.c 退化
6. Task 6: PTY 实现 + current->ctty + /dev/tty 魔数
7. Task 7: terminal.elf
8. Task 8: busybox ash 集成 + init 编排
9. Task 9: 测试 + 文档

## Deferred

See spec `docs/superpowers/specs/2026-07-26-fb-pty-terminal-design.md` section "Deferred Items":
ptsname/grantpt/unlockpt, TIOCPKT, TIOCSCTTY, kill_pgrp/SIGWINCH, /dev/tty strcmp, devfs subdirs, keyboard extended keys, terminal.elf echo+readline, fb 2MB page, fb stride padding, raw mode testing.
