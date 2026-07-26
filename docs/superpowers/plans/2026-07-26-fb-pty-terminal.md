# /dev/fb mmap + PTY + terminal 用户态渲染 — 实现计划 (v2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将终端渲染从内核移到用户态 terminal.elf，内核保留原始数据传输 (PTY pipe + TTY ring buffer)。

**Architecture:** PTY master 是 `FD_PTY_MASTER` 文件类型，PTY slave 是 `FD_PTY_SLAVE` 文件类型——两者都直接操作底层 pipe 获得 blocking I/O。`/dev/pts0` 通过 `devfs_ops.open` 创建 FD_PTY_SLAVE 的 file_t。/dev/fb 通过 `VM_IO` + `do_mmap` 设备分派支持 mmap。tty.c/console.c 退化到 raw 模式。`/dev/tty` 通过 `current->ctty` 指针实现每进程控制终端。

**Key design decision:** PTY slave 不通过 devfs 的 FD_DEV 路径做 I/O（FD_DEV 无 blocking 循环）。而是用独立的 `FD_PTY_SLAVE` 类型，`fd_read`/`fd_write` 直接调用 `pipe_read_internal`/`pipe_write_internal`。devfs 节点仅用于 open 时创建 file_t。

**Tech Stack:** C (kernel + userspace), x86_64, QEMU, devfs, PSF2 font

**Spec:** `docs/superpowers/specs/2026-07-26-fb-pty-terminal-design.md`

## Global Constraints

- 内核 C 代码符合现有风格
- 用户态代码放在 user/，链接现有 libc
- `make clean` 强依赖于 struct 变更后
- 每步 commit 独立可编译可运行
- `vfs_node_alloc()` 不存在——使用 `calloc(1, sizeof(vfs_node_t))`

## VM_IO 关键防护（影响 Task 4, 7）

以下 3 处必须在 Task 4 (/dev/fb) 完成时同步修复，否则 fork/exit/pf 路径会在
framebuffer MMIO 页上出错：

### A. fork_mm_copy — VM_IO 页面不 COW

`kernel/sched/task.c` 的 `fork_mm_copy` 对所有可写 Present PTE 做 COW。
framebuffer 物理地址不在 `subpage_pool` 中 → `page_cow_get` 是 no-op →
`page_cow_refs` 在 #PF 时返回 0 → 原地提升。**当前此行为偶然正确**，
但若 framebuffer 物理地址在 E820 RAM 区域内，`free_4k_page` 会把 MMIO 页
还给分配器 → 双重分配 + 显示损坏。

**修复**：在 `fork_mm_copy` 遍历 PTE 时，检查 VMA 的 `VM_IO` 标志，跳过 COW：

```c
// kernel/sched/task.c — fork_mm_copy, 在 page_cow_get 调用前:
if (vma->vm_flags & VM_IO) continue;  // MMIO pages: skip COW, share directly
```

### B. do_page_fault vm_file 路径 — VM_IO 页面不惰性填页

`kernel/arch/x86_64/trap.c:495-524` 的 `vma->vm_file` 路径在缺页时分配 RAM 页 +
`vfs_read(vma->vm_file, ...)` 填入。若 fb_mmap 的 PTE 未预先填充（或 fork 后
PTE 被清除），缺页处理会从 `/dev/fb` 读取 `fb_info` 结构体数据写入 RAM 页，
屏幕被二进制数据覆盖。

**修复 A（推荐）**：`fb_mmap` 预填充所有 PTE 后设 `vma->vm_file = NULL`，
  缺页处理器跳过 vm_file 路径。

**修复 B（额外防护）**：`do_page_fault` 在 vm_file 路径前检查 `VM_IO`：
```c
if (vma->vm_file && !(vma->vm_flags & VM_IO)) { ... }
```

两者都做——fb_mmap 清 vm_file（避免 pf 误判）+ pf 路径加 VM_IO 防护（深度防御）。

### C. vma_free_all — VM_IO 页面不释放

`kernel/memory/vma.c` 的 `vma_free_all` → `vmm_unmap_4k_page` → `free_4k_page`
对不在 subpage_pool 中的物理地址是 no-op（非 RAM 地址不匹配任何池）。
但若 framebuffer 在 E820 RAM 内 → 释放 MMIO 页到分配器 → 双重分配。

**修复**：`vma_free_all` 遍历 VMA 时跳过 `VM_IO` 页面：
```c
if (v->vm_flags & VM_IO) { vma_remove(mm, v); continue; }
```

### D. fb_mmap 统一修复

`fb_mmap` 中预填充所有 PTE 后：
```c
vfs_node_put(vma->vm_file);
vma->vm_file = NULL;  // 防止 do_page_fault 走 vfs_read 路径
```

---

## Task 重排（v3）

```
Task 1:  devfs_ops struct 重构 + 5 设备迁移 + sys_open 集成
Task 2:  pipe 函数导出 + pipe_read/write_internal 提取
Task 3:  fd_ioctl 创建 + SYS_ioctl 重构为按 fd type 分发
Task 4:  VM_IO + do_mmap 设备分派 + /dev/fb 驱动 (+ fb_ioctl)
Task 5:  tty.c 退化 (删除 canonical)
Task 6:  console.c 退化 (删除 CSI + 光标) + console_surrender_fb
Task 7:  PTY 实现 + current->ctty + /dev/tty 魔数 (master=FD_PTY_MASTER, slave=FD_PTY_SLAVE)
Task 8:  terminal.elf
Task 9:  init 编排 + busybox FEATURE_EDITING 验证
Task 10: 测试 + 文档
```

关键改动 vs v1：Task 2 新增 pipe 导出、Task 3 新增 SYS_ioctl 重构。
PTY slave 改为 `FD_PTY_SLAVE` 类型（阻塞 I/O）。

---

### Task 1: devfs_ops struct 重构

**Files:**
- Modify: `kernel/include/fs/devfs.h`
- Modify: `kernel/fs/devfs.c`
- Modify: `kernel/include/fs/vfs.h`
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/arch/x86_64/trap.c`

**Interfaces:**
- Produces: `struct devfs_ops`, `devfs_register_chrdev(name, private_data, &ops)`, `devfs_open_node()`, `vfs_ops.mmap`

- [ ] **Step 1: devfs_ops struct 定义 (devfs.h)**

```c
// kernel/include/fs/devfs.h — 替换现有函数指针声明

struct devfs_ops {
    // open: 返回自定义 file_t。返回 -ENOSYS 走默认 FD_DEV
    int (*open)(const char *name, struct file **out_file);
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *);
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *);
    uint32_t (*poll)(void *priv, struct poll_table *pt);
    int (*mmap)(struct vfs_node *, struct vma *);
    int (*ioctl)(struct vfs_node *, int cmd, void *arg);
};

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);

// sys_open 集成：检查 devfs 设备的 open 回调，返回自定义 file_t 或 NULL
// 返回 NULL + *out==NULL → 走默认 FD_DEV
int devfs_open_node(struct vfs_node *node, const char *path, int flags,
                    struct file **out);
```

- [ ] **Step 2: 更新 devfs_device_t (devfs.c)**

```c
typedef struct devfs_device {
    char name[DEVFS_NAME_MAX];
    uint8_t type;
    const struct devfs_ops *ops;     // 替代原 read/write/poll 独立指针
    void *private_data;
    int registered;
} devfs_device_t;
```

- [ ] **Step 3: devfs_read/write/poll 通过 ops 分派**

在 `devfs_read` 中将 `if (devices[idx].read)` 改为 `if (devices[idx].ops && devices[idx].ops->read)`，同理 write / poll。

- [ ] **Step 4: devfs_mmap 分派**

```c
static int devfs_mmap(vfs_node_t *node, vma_t *vma)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered) return -EINVAL;
    if (devices[idx].ops && devices[idx].ops->mmap)
        return devices[idx].ops->mmap(node, vma);
    return -ENODEV;
}
```

更新 devfs vfs_ops 表添加 `.mmap = devfs_mmap`。

- [ ] **Step 5: devfs_open_node 实现**

```c
// kernel/fs/devfs.c
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
    // Default: FD_DEV
    *out = file_alloc();
    if (!*out) return -ENOMEM;
    (*out)->type = FD_DEV;
    (*out)->node = vfs_node_get(node);
    (*out)->flags = flags;
    return 0;
}
```

- [ ] **Step 6: sys_open 调用 devfs_open_node**

在 `kernel/arch/x86_64/trap.c` 的 `SYS_open` case 中，`vfs_lookup_from` 后：

```c
vfs_node_t *node = vfs_lookup_from(path, cwd);
if (!node) { regs->rax = -ENOENT; break; }

file_t *f = NULL;
int rc = devfs_open_node(node, path, flags, &f);
vfs_node_put(node);  // node 来自 lookup，现在引用已移交或释放
if (rc < 0) { regs->rax = rc; break; }
if (!f) { regs->rax = -ENOMEM; break; }
```

`devfs_open_node` 内部：若走 open 回调返回自定义 file_t，则该 file_t 不绑定 vfs_node
（如 FD_PTY_MASTER）。若回调返回 -ENOSYS，则分配 FD_DEV + vfs_node_get(node)。
无论哪种，调用方统一 `vfs_node_put(node)` 正确——一个引用来自 lookup，另一个
（若需）来自 file_t 内部。

int fd = fd_alloc(current->files, f);
if (fd < 0) { file_free(f); regs->rax = -EMFILE; break; }
regs->rax = fd;
```

- [ ] **Step 7: vfs_ops.mmap 字段**

在 `kernel/include/fs/vfs.h` 的 `vfs_ops_t` 末尾添加：
```c
int (*mmap)(struct vfs_node *, struct vma *);
```

- [ ] **Step 8: 迁移 5 个现有设备到 ops struct**

为 null/zero/random/serial/tty 各创建 static ops struct，在 `devfs_init()` 中用新签名注册。

- [ ] **Step 9: vfs_node_alloc 不存在 → 用 calloc**

整个实现中所有 `vfs_node_alloc()` 调用替换为 `calloc(1, sizeof(vfs_node_t))`。

- [ ] **Step 10: 编译验证 + Commit**

---

### Task 2: pipe 函数导出 + pipe_read/write_internal 提取

**Files:**
- Modify: `kernel/fs/file.c`
- Modify: `kernel/include/kernel/file.h`

**Interfaces:**
- Produces: `pipe_read_internal()`, `pipe_write_internal()`, `pipe_wake_readers()` (public), `pipe_wake_writers()` (public), `FD_PTY_MASTER`, `FD_PTY_SLAVE`, `file->pty`

- [ ] **Step 1: FD_PTY_MASTER + FD_PTY_SLAVE + file->pty (file.h)**

```c
// kernel/include/kernel/file.h

// Forward declaration
struct pty_struct;
typedef struct pty_struct pty_t;

enum file_type {
    FD_NONE = 0,
    FD_VFS,
    FD_PIPE,
    FD_DEV,
    FD_PTY_MASTER,   // PTY master
    FD_PTY_SLAVE,    // PTY slave (blocking I/O via pipe directly)
};

typedef struct file {
    enum file_type type;
    uint32_t       refcount;
    int            flags;
    uint64_t       offset;
    struct vfs_node *node;   // FD_VFS / FD_DEV
    pipe_t         *pipe;    // FD_PIPE
    pty_t          *pty;     // FD_PTY_MASTER / FD_PTY_SLAVE
} file_t;

// ── Pipe API (exported for PTY) ──
int64_t pipe_read_internal(pipe_t *p, void *buf, uint64_t size);
int64_t pipe_write_internal(pipe_t *p, const void *buf, uint64_t size);
void    pipe_wake_readers(pipe_t *p);
void    pipe_wake_writers(pipe_t *p);
```

- [ ] **Step 2: 提取 pipe_read_internal (file.c)**

从 `fd_read` 的 `FD_PIPE` case 提取完整 blocking read 循环（含 wait queue + schedule），命名为 `pipe_read_internal`。

- [ ] **Step 3: 提取 pipe_write_internal (file.c)**

从 `fd_write` 的 `FD_PIPE` case 提取完整 blocking write 循环，命名为 `pipe_write_internal`。

- [ ] **Step 4: pipe_wake_readers/writers 改 public**

删除 `static` 关键字，在 `file.h` 中声明。

- [ ] **Step 5: fd_read 添加 FD_PTY_MASTER + FD_PTY_SLAVE case**

```c
case FD_PTY_MASTER:
    return pipe_read_internal(f->pty->slave_to_master, buf, size);
case FD_PTY_SLAVE:
    return pipe_read_internal(f->pty->master_to_slave, buf, size);
```

同理 `fd_write`——master 写 master_to_slave，slave 写 slave_to_master。

- [ ] **Step 6: fd_poll 添加 FD_PTY_MASTER + FD_PTY_SLAVE case**

```c
case FD_PTY_MASTER: {
    pty_t *pty = f->pty;
    uint32_t mask = 0;
    if (pty->slave_to_master) {
        uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
        if (pty->slave_to_master->head != pty->slave_to_master->tail)
            mask |= POLLIN | POLLRDNORM;
        else if (pt && !pt->triggered)
            poll_wait(pt, &pty->slave_to_master->read_wait, &pty->slave_to_master->lock);
        spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
    }
    if (pty->master_to_slave) {
        uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
        if (!pipe_full(pty->master_to_slave))
            mask |= POLLOUT | POLLWRNORM;
        else if (pt && !pt->triggered)
            poll_wait(pt, &pty->master_to_slave->write_wait, &pty->master_to_slave->lock);
        spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
    }
    return mask;
}
case FD_PTY_SLAVE: {
    // symmetric: slave reads master_to_slave, writes slave_to_master
    pty_t *pty = f->pty;
    uint32_t mask = 0;
    if (pty->master_to_slave) {
        uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
        if (pty->master_to_slave->head != pty->master_to_slave->tail)
            mask |= POLLIN | POLLRDNORM;
        else if (pt && !pt->triggered)
            poll_wait(pt, &pty->master_to_slave->read_wait, &pty->master_to_slave->lock);
        spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
    }
    if (pty->slave_to_master) {
        uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
        if (!pipe_full(pty->slave_to_master))
            mask |= POLLOUT | POLLWRNORM;
        else if (pt && !pt->triggered)
            poll_wait(pt, &pty->slave_to_master->write_wait, &pty->slave_to_master->lock);
        spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
    }
    return mask;
}
```

- [ ] **Step 7: file_free 处理 FD_PTY_MASTER + FD_PTY_SLAVE (含 pipe_free)**

```c
if ((f->type == FD_PTY_MASTER || f->type == FD_PTY_SLAVE) && f->pty) {
    pty_t *pty = f->pty;
    int need_r_a = 0, need_r_b = 0;

    if (f->type == FD_PTY_MASTER) {
        // master reads slave_to_master, writes master_to_slave
        if (pty->slave_to_master) {
            uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
            pty->slave_to_master->readers--;           // master stopped reading
            need_r_b = (pty->slave_to_master->readers == 0);
            spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
        }
        if (pty->master_to_slave) {
            uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
            pty->master_to_slave->writers--;            // master stopped writing
            need_r_a = (pty->master_to_slave->writers == 0);
            spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        }
    } else { // FD_PTY_SLAVE
        // slave reads master_to_slave, writes slave_to_master
        if (pty->master_to_slave) {
            uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
            pty->master_to_slave->readers--;            // slave stopped reading
            need_r_a = (pty->master_to_slave->readers == 0);
            spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        }
        if (pty->slave_to_master) {
            uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
            pty->slave_to_master->writers--;            // slave stopped writing
            need_r_b = (pty->slave_to_master->writers == 0);
            spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
        }
        pty->pgrp = 0;  // reset on slave close
    }

    // Wake blocked readers if all writers are gone
    if (need_r_a) pipe_wake_readers(pty->master_to_slave);
    if (need_r_b) pipe_wake_readers(pty->slave_to_master);

    // Check if both pipes are fully closed → free them + reclaim slot
    int m2s_done = (!pty->master_to_slave->readers && !pty->master_to_slave->writers);
    int s2m_done = (!pty->slave_to_master->readers && !pty->slave_to_master->writers);
    if (m2s_done && s2m_done) {
        pipe_free(pty->master_to_slave);
        pipe_free(pty->slave_to_master);
        pty->master_to_slave = NULL;
        pty->slave_to_master = NULL;
        pty->allocated = false;  // slot reusable
    }

    f->pty = NULL;
}
```

- [ ] **Step 8: 编译验证 + Commit**

---

### Task 3: fd_ioctl 创建 + SYS_ioctl 重构

**Files:**
- Create: `kernel/fs/file.c` → add `fd_ioctl()`
- Modify: `kernel/include/kernel/file.h` → declare `fd_ioctl`
- Modify: `kernel/arch/x86_64/trap.c` → SYS_ioctl case

**Interfaces:**
- Produces: `int64_t fd_ioctl(file_t *f, int cmd, void *arg)` — 按文件类型分发 ioctl

- [ ] **Step 1: 添加 fd_ioctl (file.c)**

```c
// kernel/fs/file.c
int64_t fd_ioctl(file_t *f, int cmd, void *arg)
{
    if (!f) return -EBADF;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        // 如果 devfs 设备有 ioctl handler → 调用
        if (!f->node) return -ENOTTY;
        int idx = (int)(uintptr_t)f->node->fs_data;
        // 通过 devfs API 查找 ops
        extern int devfs_ioctl_node(vfs_node_t *node, int cmd, void *arg);
        return devfs_ioctl_node(f->node, cmd, arg);
    }
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
    case FD_PTY_SLAVE: {
        pty_t *pty = f->pty;
        if (!pty) return -ENOTTY;
        return pty_slave_ioctl(pty, cmd, arg);  // defined in pty.c (Task 7)
    }
    case FD_PIPE:
    default:
        return -ENOTTY;
    }
}
```

- [ ] **Step 2: 添加 devfs_ioctl_node 辅助函数 (devfs.c)**

```c
// kernel/fs/devfs.c
int devfs_ioctl_node(vfs_node_t *node, int cmd, void *arg)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -ENODEV;
    if (devices[idx].ops && devices[idx].ops->ioctl)
        return devices[idx].ops->ioctl(node, cmd, arg);
    return -ENOTTY;
}
```

在 `devfs.h` 中声明。

- [ ] **Step 3: SYS_ioctl 重构 (trap.c)**

找到 `case SYS_ioctl:`（约 trap.c:1492），替换整个 case：

```c
case SYS_ioctl: {
    int fd = (int)regs->rdi;
    int request = (int)regs->rsi;
    void *arg = (void *)regs->rdx;

    if (fd < 0 || fd >= NOFILE || !current->files || !current->files->fd[fd]) {
        regs->rax = -EBADF;
        break;
    }
    file_t *f = current->files->fd[fd];

    // User pointer validation
    if ((uint64_t)arg >= current->addr_limit) {
        regs->rax = -EFAULT;
        break;
    }

    regs->rax = fd_ioctl(f, request, arg);
    break;
}
```

删除原来的 switch-on-ioctl-number 硬编码逻辑（TCGETS → get_dev_tty() 等）。

- [ ] **Step 4: 编译验证 + Commit**

---

### Task 4: VM_IO + do_mmap 设备分派 + /dev/fb

**Files:**
- Modify: `kernel/include/kernel/vma.h`
- Modify: `kernel/memory/vma.c`
- Create: `kernel/driver/fb.c`
- Create: `kernel/include/kernel/fb.h`
- Modify: `kernel/kernel/main.c`

- [ ] **Step 1: VM_IO = 0x80 (vma.h)**

- [ ] **Step 2: do_mmap 设备分派 (vma.c)**

在 `do_mmap` 的步 3（prot→flags）之后、步 5（VMA 分配）之前，替换现有的步 4：

```c
    // ── 4. Check for device/file mmap ──
    vfs_node_t *file_node = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        file_t *file = current->files->fd[fd];
        if (!file || !file->node) { regs setup if needed; return -EBADF; }
        file_node = vfs_node_get(file->node);

        // Device node with mmap handler → device path
        if (file_node && file_node->ops && file_node->ops->mmap) {
            if (!(flags & MAP_SHARED))
                return -EINVAL;  // device map must be SHARED

            // Pre-allocate VMA for the device handler to fill PTEs
            vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
            if (!vma) { vfs_node_put(file_node); return -ENOMEM; }
            list_init(&vma->list);
            vma->vm_start     = addr;
            vma->vm_end       = addr + length;
            vma->vm_flags     = vm_flags_base | VM_IO;
            vma->vm_page_prot = page_prot;
            vma->vm_pgoff     = offset >> PAGE_4K_SHIFT;
            vma->vm_file      = file_node;  // handler may clear this

            int rc = file_node->ops->mmap(file_node, vma);
            if (rc < 0) {
                // Handler failed — vma not inserted, clean up
                vfs_node_put(file_node);
                kfree(vma);
                return rc;
            }
            // Handler fills PTEs (e.g. fb_mmap eager-fills + flush_tlb +
            //   vfs_node_put(vma->vm_file); vma->vm_file = NULL)
            vma_insert(current->mm, vma);
            return (int64_t)vma->vm_start;
        }
        // Normal file mapping → fall through to step 5
    }

    // ── 5. Non-device: allocate VMA (existing logic unchanged) ──
    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
    if (!vma) { if (file_node) vfs_node_put(file_node); return -ENOMEM; }
    // ... existing VMA init + vma_insert
```

- [ ] **Step 3: fb.h**

```c
#ifndef _KERNEL_FB_H
#define _KERNEL_FB_H
#include <stdint.h>

struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define FBIOSURRENDER  0x00004601
#endif
```

- [ ] **Step 4: fb.c — fb_read, fb_mmap, fb_ioctl**

```c
// fb_read: offset=0 返回 fb_info, offset>0 返回 0 (EOF)
// fb_write: 返回 size (discard) + 检查 fb_surrendered 标志
//   static bool fb_surrendered = false;
//   若 fb_surrendered → 只输出到 serial (write_serial)，不写 fb
static int fb_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (fb_surrendered) {
        for (uint64_t i = 0; i < size; i++)
            write_serial(((char *)buffer)[i]);
    }
    return (int)size;
}

// fb_mmap:
//   校验 SHARED + 不越界 → 设置 VM_IO → eager PTE fill → flush_tlb
//   → vfs_node_put(vma->vm_file); vma->vm_file = NULL;  // 防 pf vm_file 路径
// fb_ioctl:
//   FBIOSURRENDER → console_surrender_fb() + fb_surrendered = true
```

`fb_mmap` 末尾 `flush_tlb()` 必不可少。`vma->vm_file = NULL` 防止
do_page_fault 的 vm_file 路径误判——fb_mmap 已将 PTE 全部预填入，
后续缺页只可能来自 fork 清除的 PTE。VM_IO 防护（见 Global Constraints）
在 fork_mm_copy 中跳过 VM_IO 页面 → PTE 不会被清除 → 不会触发缺页。

- [ ] **Step 5: fork_mm_copy + do_page_fault + vma_free_all VM_IO 防护**

在 `kernel/sched/task.c` 的 `fork_mm_copy` 中，COW 循环里：
```c
// 在遍历 PTE 时，获取 vma 后检查:
if (vma->vm_flags & VM_IO) continue;  // skip MMIO pages
```

在 `kernel/arch/x86_64/trap.c` 的 `do_page_fault`，vm_file 路径前：
```c
if (vma->vm_file && !(vma->vm_flags & VM_IO)) {
    // 现有惰性填页逻辑 (alloc_4k_page + vfs_read + vmm_map_4k_page)
}
```

在 `kernel/memory/vma.c` 的 `vma_free_all`，循环开头：
```c
if (v->vm_flags & VM_IO) { vma_remove(mm, v); continue; }
```

- [ ] **Step 6: main.c 注册 /dev/fb**

`devfs_register_chrdev("fb", NULL, &fb_ops);`

- [ ] **Step 6: 编译验证 + Commit**

---

### Task 5: tty.c 退化

**Files:** `kernel/include/kernel/tty.h`, `kernel/tty/tty.c`, `kernel/driver/keyboard.c`

- 删除 `TTY_L_ICANON/ECHO/ISIG` 宏
- 删除 `tty->line[], line_len, read_pos, line_ready, lflag, pgrp`
- `cooked_lock` → `ring_lock`, `cooked[]` → `ring[]`
- `tty_read`: 删除 canonical 处理，直接 ring → buf 拷贝 + sleep/wake
- 删除 `tty_canon_process()`, `tty_cooked_pop()`
- `keyboard.c`: 方向键无条件发 VT100 序列

---

### Task 6: console.c 退化

**Files:** `kernel/include/kernel/console.h`, `kernel/tty/console.c`, `kernel/driver/pit.c`

- 删除 CSI 状态机全部（enum + 静态变量 + switch）
- 删除 光标闪烁 全部（`console_draw_blink`, `console_blink_tick`, PIT 回调）
- `console_putchar`: 仅处理 `\n \r \b \t` + 可打印字符 + `console_scroll`
- 新增 `console_surrender_fb()` / `console_force_enable()`
- console.h 移除 `console_blink_tick` 声明
- **注意**: `color_printk` 直接写 `Pos.FB_addr`，不检查 `term_initialized`。terminal.elf surrender 后内核 log 的 fb 输出仍会覆盖 framebuffer。修复：在 `putchar_at` 开头添加 `if (!term_initialized) return;` 或新增 `fb_surrendered` 标志。V1 可接受此行为（surrender 前所有 log 已完成，surrender 后仅 panic 写 fb）。

---

### Task 7: PTY 实现 + current->ctty + /dev/tty 魔数

**Files:**
- Create: `kernel/include/kernel/pty.h`
- Create: `kernel/driver/pty.c`
- Modify: `kernel/include/kernel/task.h`
- Modify: `kernel/sched/task.c`
- Modify: `kernel/fs/devfs.c`
- Modify: `kernel/kernel/main.c`

**Key architecture**: `FD_PTY_MASTER` + `FD_PTY_SLAVE`（均在 Task 2 定义）。
/dev/pts0 注册为 devfs 节点，其 `devfs_ops.open` 回调创建 `FD_PTY_SLAVE` file_t。
I/O 完全通过 pipe_read/write_internal，无 devfs 中间层。

- [ ] **Step 1: pty.h**

```c
#define PTY_MAX  8

typedef struct pty_struct {
    int index; bool allocated;
    pipe_t *master_to_slave, *slave_to_master;
    struct termios term;
    uint16_t ws_row, ws_col;
    pid_t pgrp;
} pty_t;

extern pty_t pty_table[PTY_MAX];
extern spinlock_T pty_lock;  // SMP-safe allocation

void pty_init(void);
pty_t *pty_alloc(void);
int pty_slave_ioctl(pty_t *pty, int cmd, void *arg);  // called by fd_ioctl
```

- [ ] **Step 2: pty.c — pty_alloc (with lock)**

```c
pty_t pty_table[PTY_MAX];
spinlock_T pty_lock = {1};

pty_t *pty_alloc(void)
{
    uint64_t fl = spin_lock_irqsave(&pty_lock);
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) {
            pty_t *pty = &pty_table[i];
            memset(pty, 0, sizeof(*pty));
            pty->index = i; pty->allocated = true;
            pty->master_to_slave = pipe_alloc();
            pty->slave_to_master = pipe_alloc();
            if (!pty->master_to_slave || !pty->slave_to_master) {
                if (pty->master_to_slave) { pipe_free(pty->master_to_slave); pty->master_to_slave = NULL; }
                if (pty->slave_to_master) { pipe_free(pty->slave_to_master); pty->slave_to_master = NULL; }
                pty->allocated = false;
                spin_unlock_irqrestore(&pty_lock, fl);
                return NULL;
            }
            pty->term.c_lflag = 0;  // raw mode
            pty->term.c_iflag = ICRNL;
            pty->term.c_oflag = OPOST | ONLCR;
            // c_cflag / c_line / c_ispeed / c_ospeed already zero from memset
            pty->term.c_cc[VMIN] = 1;
            pty->term.c_cc[VTIME] = 0;
            pty->ws_row = 25; pty->ws_col = 80; pty->pgrp = 0;
            spin_unlock_irqrestore(&pty_lock, fl);
            return pty;
        }
    }
    spin_unlock_irqrestore(&pty_lock, fl);
    return NULL;
}
```

- [ ] **Step 3: pty_slave_ioctl (public)**

```c
int pty_slave_ioctl(pty_t *pty, int cmd, void *arg)
{
    if (!pty) return -ENODEV;
    switch (cmd) {
    case TCGETS: memcpy(arg, &pty->term, sizeof(struct termios)); return 0;
    case TCSETS: case TCSETSW: memcpy(&pty->term, arg, sizeof(struct termios)); return 0;
    case TIOCGWINSZ: ((struct winsize *)arg)->ws_row = pty->ws_row; ((struct winsize *)arg)->ws_col = pty->ws_col; return 0;
    case TIOCSWINSZ: pty->ws_row = ((struct winsize *)arg)->ws_row; pty->ws_col = ((struct winsize *)arg)->ws_col; return 0;
    case TIOCGPGRP: *(pid_t *)arg = pty->pgrp; return 0;
    case TIOCSPGRP: pty->pgrp = *(pid_t *)arg; return 0;
    case TIOCSCTTY:  // stub: set ctty (no-op, ctty set by ptsN_open)
        return 0;
    // FIONREAD — busybox ash needs this for get_more_input()
    case FIONREAD: {
        if (!pty->master_to_slave) return -ENODEV;
        uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
        int avail = (pty->master_to_slave->head - pty->master_to_slave->tail + PIPE_SIZE) % PIPE_SIZE;
        spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        *(int *)arg = avail;
        return 0;
    }
    default: return -ENOTTY;
    }
}
```

- [ ] **Step 4: ptmx_open + ptsN_open (devfs open callbacks)**

```c
// ── ptmx_open: 分配 PTY，返回 FD_PTY_MASTER file_t ──
static int ptmx_open(const char *name, file_t **out_file)
{
    (void)name;
    pty_t *pty = pty_alloc();
    if (!pty) return -ENOMEM;

    char slave_name[16];
    snprintf(slave_name, sizeof(slave_name), "pts%d", pty->index);
    devfs_register_chrdev(slave_name, pty, &pty_slave_ops);

    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type = FD_PTY_MASTER;
    f->pty = pty;
    f->flags = O_RDWR;
    *out_file = f;
    return 0;
}

// ── ptsN_open: 返回 FD_PTY_SLAVE file_t，设置 ctty ──
static int ptsN_open(const char *name, file_t **out_file)
{
    // 找到匹配的 PTY slot
    pty_t *pty = NULL;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_table[i].allocated) continue;
        char expected[16];
        snprintf(expected, sizeof(expected), "pts%d", pty_table[i].index);
        if (strcmp(name + strlen(name) - strlen(expected), expected) == 0
            || strstr(name, expected)) {
            pty = &pty_table[i];
            break;
        }
    }
    if (!pty) return -ENOENT;

    // 设置控制终端
    current->ctty_type = CTTY_PTY;
    current->ctty = pty;

    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type = FD_PTY_SLAVE;
    f->pty = pty;
    f->flags = O_RDWR;
    *out_file = f;
    return 0;
}

static const struct devfs_ops ptmx_ops = { .open = ptmx_open };
```

注意 `pty_slave_ops` 的 `.open = ptsN_open`，不需要 `.read/.write/.poll`——I/O 通过 FD_PTY_SLAVE 的 fd_read/fd_write。

- [ ] **Step 5: task_struct ctty (task.h)**

```c
// kernel/include/kernel/task.h — task_struct 内添加:
    enum ctty_type { CTTY_NONE = 0, CTTY_PHYS, CTTY_PTY } ctty_type;
    void *ctty;  // → tty_t (CTTY_PHYS) or pty_t (CTTY_PTY)
```

更新 `INIT_TASK` 宏：`.ctty_type = CTTY_NONE, .ctty = NULL`。

- [ ] **Step 6: fork 继承 ctty (task.c)**

```c
child->ctty_type = current->ctty_type;
child->ctty      = current->ctty;
```

- [ ] **Step 7: /dev/tty 魔数 open handler (devfs.c)**

物理 TTY 注册改为 `private_data = kbd_tty`（不是 NULL）。

```c
extern tty_t *kbd_tty;
devfs_register_chrdev("tty", kbd_tty, &tty_magic_ops);   // /dev/tty magic
devfs_register_chrdev("tty0", kbd_tty, &tty_phys_ops);   // physical alias
```

`tty_magic_ops.open`:
```c
static int tty_magic_open(const char *name, file_t **out_file)
{
    if (strcmp(name, "/dev/tty") != 0) return -ENOSYS;

    void *target = NULL;
    if (current->ctty_type == CTTY_PTY) target = current->ctty;
    else target = kbd_tty;

    for (int i = 0; i < device_count; i++) {
        if (devices[i].private_data == target && devices[i].type == VFS_CHRDEV
            && devices[i].registered) {
            vfs_node_t *node = calloc(1, sizeof(vfs_node_t));  // NOT vfs_node_alloc()
            if (!node) return -ENOMEM;
            node->type = VFS_CHRDEV;
            node->fs_data = (void *)(uintptr_t)i;
            node->ops = &devfs_ops;
            node->refcount = 1;
            *out_file = file_alloc();
            if (!*out_file) { free(node); return -ENOMEM; }
            (*out_file)->type = FD_DEV;
            (*out_file)->node = node;
            return 0;
        }
    }
    return -ENXIO;
}
```

- [ ] **Step 8: pty_init + 注册 /dev/ptmx**

在 main.c 中调用 `pty_init()`。`pty_init` 注册 `/dev/ptmx`：

```c
void pty_init(void) {
    memset(pty_table, 0, sizeof(pty_table));
    devfs_register_chrdev("ptmx", NULL, &ptmx_ops);
}
```

- [ ] **Step 9: 编译验证**

确认 `snprintf` 可用。若不可用，替换为手动构造 `"pts0"` 等字符串：
```c
// In pty_alloc, after returning pty:
// caller (ptmx_open) is responsible for registering slave
// Use simple number-to-string for index < 10:
slave_name[3] = '0' + pty->index; slave_name[4] = '\0';
```

- [ ] **Step 10: Commit**

---

### Task 8: terminal.elf

**Files:**
- Create: `user/terminal.c`
- Modify: `user/Makefile`

**Keyboard input flow (critical):** terminal.elf 在设置 ctty **之前** 打开 `/dev/tty`（此时 ctty==CTTY_NONE → 魔数回调 → 物理 TTY），所以 tty_fd 指向物理 TTY（键盘+串口）。键盘 IRQ → kbd_tty ring buffer → tty_fd 的 read → terminal.elf。路径正确。

- [ ] **Step 1: 创建 user/terminal.c**

```c
/* terminal.elf — OS01 userspace VT100 terminal emulator
 *
 * Keyboard path: open /dev/tty BEFORE open /dev/pts0 → ctty==NULL → phys TTY
 *   kbd IRQ → kbd_tty ring buffer → /dev/tty fd → terminal.elf → PTY master → ash
 *
 * Ash output path: ash → PTY slave → pipe → PTY master fd → terminal.elf → fb
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/syscall.h>
#include <errno.h>
#include <poll.h>         // ← NOT <sys/poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <termios.h>

struct fb_info { uint32_t width, height, stride, bpp, format; } __attribute__((packed));
#define FBIOSURRENDER  0x00004601

// Must match kernel font.h: 8 uint32 fields + glyphs byte + packed
typedef struct {
    uint32_t magic, version, headersize, flags, numglyph, bytesperglyph, height, width;
    uint8_t glyphs;  // make sizeof = 33 (same as kernel font.h)
} __attribute__((packed)) psf2_t;

extern char _binary_user_terminal_font_psf_start[];

static uint32_t *fb;
static struct fb_info fb_info;
static psf2_t *font;
static int term_col, term_row, term_cols, term_rows;
static uint32_t fg = 0xFFFFFFFF, bg = 0x00000000;
static bool cursor_visible = true;
static char line_buf[256];
static int line_len;

// VT100 state
enum { S_NORMAL, S_ESC, S_CSI_PARAM };
static int cs = S_NORMAL, cs_param;
static bool cs_qmark;

static void put_glyph(int col, int row, uint32_t fgc, uint32_t bgc, char c)
{
    if (col < 0 || col >= term_cols || row < 0 || row >= term_rows) return;
    unsigned char *glyph = (unsigned char *)font + font->headersize
        + ((c > 0 && c < (int)font->numglyph) ? c : 0) * font->bytesperglyph;
    for (uint32_t y = 0; y < font->height; y++) {
        uint32_t *line = fb + (row * font->height + y) * (fb_info.stride / 4) + col * font->width;
        uint32_t test = 0x100;
        for (uint32_t x = 0; x < font->width; x++) {
            test >>= 1;
            *line++ = (*glyph & test) ? fgc : bgc;
        }
        glyph++;
    }
}

static void fb_scroll(void) {
    uint8_t *fb_bytes = (uint8_t *)fb;
    uint32_t row_bytes = font->height * fb_info.stride;
    uint32_t total = row_bytes * term_rows;
    memmove(fb_bytes, fb_bytes + row_bytes, total - row_bytes);
    memset(fb_bytes + total - row_bytes, 0, row_bytes);
    term_row = term_rows - 1;
}

static void output_char(char c)
{
    // VT100 CSI state machine: S_NORMAL → S_ESC → S_CSI_PARAM
    // Supported: ESC[nA B C D (cursor), ESC[nK (clear line),
    //   ESC[2J (clear screen), ESC[H (home), ESC[?25h/l (cursor show/hide)
    // Normal chars: \n \r \b \t + printable → put_glyph + advance
    // Full implementation per spec section 6 (VT100 parser)
}

static void handle_output(char *buf, int n) {
    for (int i = 0; i < n; i++) output_char(buf[i]);
}

static void handle_input(char *buf, int n, int pty_fd, int ash_pid)
{
    struct termios term;
    bool is_cooked = (ioctl(pty_fd, TCGETS, &term) == 0) && (term.c_lflag & ICANON);

    for (int i = 0; i < n; i++) {
        char c = buf[i];

        if (!is_cooked) {
            write(pty_fd, &c, 1);  // raw: passthrough
            continue;
        }

        // Cooked mode with line editing
        if (c == '\r' || c == '\n') {
            line_buf[line_len++] = '\n';
            write(pty_fd, line_buf, line_len);
            line_len = 0;
            output_char('\r'); output_char('\n');
        } else if (c == '\x7f' || c == '\b') {
            if (line_len > 0) { line_len--; if (term_col > 0) term_col--; put_glyph(term_col, term_row, fg, bg, ' '); }
        } else if (c == '\x03') {
            if (term.c_lflag & ISIG) kill(ash_pid, SIGINT);
        } else if (c >= ' ') {
            line_buf[line_len++] = c;
            output_char(c);
        }
    }
}

int main(void)
{
    // 1. Open phys TTY BEFORE ctty is set (CTTY_NONE → phys TTY)
    int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) { write(2, "terminal: /dev/tty\n", 19); return 1; }

    // 2. Open fb
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) { write(2, "terminal: /dev/fb\n", 18); return 1; }
    read(fb_fd, &fb_info, sizeof(fb_info));
    fb = mmap(NULL, fb_info.height * fb_info.stride,
              PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((int64_t)fb < 0) { write(2, "terminal: mmap fb\n", 18); return 1; }
    ioctl(fb_fd, FBIOSURRENDER, NULL);

    font = (psf2_t *)_binary_user_terminal_font_psf_start;
    term_cols = fb_info.width / font->width;
    term_rows = fb_info.height / font->height;

    // 3. Open PTY master
    int pty_fd = open("/dev/ptmx", O_RDWR);
    if (pty_fd < 0) { write(2, "terminal: /dev/ptmx\n", 19); return 1; }

    // 4. Open slave → sets ctty=PTY for this process + fork children
    int slave = open("/dev/pts0", O_RDWR);
    if (slave < 0) { write(2, "terminal: /dev/pts0\n", 19); return 1; }

    // 5. Fork ash (child inherits ctty=PTY)
    int ash_pid = fork();
    if (ash_pid == 0) {
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
        char *argv[] = { "/bin/ash", NULL };
        exec("/bin/ash", argv, NULL);
        exit(1);
    }
    close(slave);

    // 6. Main loop
    struct pollfd fds[2] = {{.fd = tty_fd, .events = POLLIN}, {.fd = pty_fd, .events = POLLIN}};
    char buf[256];
    int exit_code = 0;
    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;  // signal arrived, retry
            break;
        }
        if (fds[0].revents & POLLIN) { int n = read(tty_fd, buf, sizeof(buf)); if (n > 0) handle_input(buf, n, pty_fd, ash_pid); }
        if (fds[1].revents & POLLIN) { int n = read(pty_fd, buf, sizeof(buf)); if (n > 0) handle_output(buf, n); else if (n == 0) break; }
    }

    // Wait for ash to exit (reap zombie)
    waitpid(ash_pid, &exit_code, 0);

    // Cleanup
    munmap(fb, fb_info.height * fb_info.stride);
    close(pty_fd); close(tty_fd); close(fb_fd);
    return exit_code;
}
```

注意包含完整的 VT100 状态机实现。上述代码中省略了 `output_char` 的完整 CSI 状态机（与 spec 一致的 A/B/C/D/K/J/H/h/l 序列），实现时应完整写入。

- [ ] **Step 2: 更新 user/Makefile**

```makefile
# Copy font -- avoid symbol collision with kernel_psf_start
user/terminal_font.psf: kernel/font.psf
	cp kernel/font.psf user/terminal_font.psf

user/terminal_font.psf.o: user/terminal_font.psf
	$(OBJCOPY) -B i386 -I binary -O elf64-x86-64 user/terminal_font.psf user/terminal_font.psf.o

user/terminal.elf: user/terminal.o user/terminal_font.psf.o
	$(LD) $(LDFLAGS) -o $@ $^ $(LDLIBS)
```

符号名：`extern char _binary_user_terminal_font_psf_start[];`（objcopy 自动生成）。

- [ ] **Step 3: 编译验证 + Commit**

---

### Task 9: init 编排 + busybox FEATURE_EDITING

**Files:**
- Verify: `thirdpart/busybox-1.36.1/.config` (FEATURE_EDITING)
- Modify: init 启动逻辑

- [ ] **Step 1: 确认 busybox FEATURE_EDITING=y**

```bash
grep CONFIG_FEATURE_EDITING thirdpart/busybox-1.36.1/.config
# 应为 CONFIG_FEATURE_EDITING=y，如果不是则手动设置
```

- [ ] **Step 2: init 启动 terminal.elf 而非 ash**

如果 init 在 `kernel/kernel/main.c`：将 `exec("/bin/ash", ...)` 改为 `exec("/terminal.elf", ...)`。

如果是用户态 `/bin/init`：改为启动 `/terminal.elf`。

- [ ] **Step 3: 集成测试**

`make run` → 验证图形终端渲染、键盘输入、命令执行、Ctrl-C、退出。

- [ ] **Step 4: Commit**

---

### Task 10: 测试 + 文档

- [ ] **Step 1: 手动测试清单**
  1. 启动 → 终端渲染 (white-on-black)
  2. 键盘 → echo
  3. `ls` → 列出目录
  4. `echo hello` → 显示
  5. Ctrl-C → SIGINT
  6. Backspace → 删除
  7. 大量输出正确滚动
  8. `exit` → ash 退出 → terminal.elf 退出

- [ ] **Step 2: 串口验证** — kernel log 正常

- [ ] **Step 3: Commit**

---

## 相关文件总览

```
Create:
  kernel/driver/pty.c           — pty_alloc, pty_slave_ioctl, ptmx_open, ptsN_open
  kernel/driver/fb.c            — fb_mmap, fb_read, fb_ioctl, fb_init
  kernel/include/kernel/pty.h   — pty_t, PTY_MAX, API
  kernel/include/kernel/fb.h    — fb_info, FBIOSURRENDER
  user/terminal.c               — VT100 terminal.elf

Modify:
  kernel/include/fs/devfs.h     — devfs_ops, devfs_open_node, devfs_ioctl_node
  kernel/fs/devfs.c             — ops dispatch, 5 device migration, magic open
  kernel/include/fs/vfs.h       — vfs_ops.mmap
  kernel/include/kernel/file.h  — FD_PTY_MASTER/SLAVE, pipe API export, file->pty, fd_ioctl
  kernel/fs/file.c              — pipe_read/write_internal, pipe_wake export, FD_PTY_* cases, fd_ioctl, file_free
  kernel/include/kernel/vma.h   — VM_IO
  kernel/memory/vma.c           — do_mmap device dispatch
  kernel/include/kernel/tty.h   — remove canonical fields
  kernel/tty/tty.c              — tty_read degradation
  kernel/include/kernel/console.h — surrender/force_enable, no blink
  kernel/tty/console.c          — remove CSI + cursor blink
  kernel/include/kernel/task.h  — ctty_type + ctty, INIT_TASK update
  kernel/sched/task.c           — fork ctty inherit
  kernel/driver/keyboard.c      — always emit VT100 arrows
  kernel/driver/pit.c           — remove console_blink_tick
  kernel/arch/x86_64/trap.c     — sys_open devfs_open_node, SYS_ioctl → fd_ioctl
  kernel/kernel/main.c          — register fb, pty_init, init → terminal.elf
  user/Makefile                 — terminal.elf build
```
