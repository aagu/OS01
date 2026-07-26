# /dev/fb mmap + PTY + terminal 用户态渲染 — 设计规格

**日期**: 2026-07-26
**状态**: 已确认
**分支**: 待创建

## 概述

将终端渲染从内核移入用户态程序 `terminal.elf`。内核保留原始数据传输
（TTY ring buffer、PTY pipe），VT100 解析、PSF2 字型渲染、行编辑全部
在用户态实现。`/dev/fb` 新增 mmap 支持，允许用户态直接写入 framebuffer。

### 目标

1. 实现 Unix98 风格 PTY（`/dev/ptmx` + `/dev/pts0`...），复用现有 pipe。devfs 是扁平目录，PTY slave 使用 `/dev/pts0`、`/dev/pts1` 命名（不支持子目录 `/dev/pts/N`）
2. `/dev/fb` 新增 mmap（`VM_IO` 映射）+ read（元数据）
3. tty.c 退化为 raw ring buffer + 阻塞读写（删除 canonical/line discipline）
4. console.c 退化为应急 panic 输出（删除 VT100 CSI 状态机、光标闪烁）
5. `/dev/tty` 实现 Linux 风格 `current->ctty` 指针（每进程控制终端）
6. 新建 `user/terminal.c`：VT100 终端 + PSF2 渲染 + 行编辑
7. busybox ash 无需改动（PTY slave 兼容 isatty/tcgetattr/tcgetpgrp）

---

## 架构

```
┌───────────────────────────────────────────────────────┐
│  Userspace                                             │
│                                                        │
│  ┌─────────────┐   PTY pair    ┌──────────────────┐   │
│  │terminal.elf  │◄════════════►│  ash (busybox)    │   │
│  │              │ master slave │                   │   │
│  │ • VT100 解析 │              │ stdin/stdout/stderr│   │
│  │ • PSF2 渲染  │              │                   │   │
│  │ • 行编辑     │              └──────────────────┘   │
│  │ • echo/^C   │                                      │
│  └──┬──────┬───┘                                      │
│     │      │                                           │
│  mmap│   read(fd=/dev/tty)                             │
│ ┌───▼──┐ ┌─▼──────┐                                   │
│ │/dev/fb│ │/dev/tty│  ← open 时查 current->ctty       │
│ └──────┘ └────────┘                                   │
├───────────────────────────────────────────────────────┤
│  Kernel                                               │
│                                                        │
│  ┌────────────┐ ┌───────────┐ ┌──────────────────┐   │
│  │ fb mmap    │ │ tty (raw) │ │ pty (双 pipe)    │   │
│  │ handler    │ │ ring+wait │ │ + termios stub   │   │
│  │ +read meta │ │ ~100 行   │ │ ~250 行          │   │
│  └────────────┘ └─────▲─────┘ └──────────────────┘   │
│                       │                                │
│            ┌──────────┴─────────┐                     │
│            │ 键盘 IRQ            │ 串口 IRQ          │
│            │ translate_and_push │                     │
│            └────────────────────┘                     │
│                                                        │
│  /dev/tty open → 查 current->ctty → 返回 PTY slave    │
│  (物理 TTY 通过其他路径访问: 应急 console + IRQ)       │
│                                                        │
│  应急 console: putchar_at + 简单滚动 (无 VT100 解析)   │
│  仅 panic 和 terminal.elf 启动前短暂使用              │
└───────────────────────────────────────────────────────┘
```

### 内核保留 vs. 移出

| 保留在内核 | 移到用户态 |
|---|---|
| 键盘 scancode → ASCII 转换 | VT100/CSI 转义序列解析 |
| 原始 ring buffer + 阻塞读写 | 行编辑 (readline) |
| 等待队列 + poll | echo（回显） |
| 应急 panic 输出到 fb | ^C → SIGINT 生成 |
| PTY 数据传输管道 | PSF2 字型渲染 |
| /dev/fb mmap | 终端颜色管理 |
| termios 属性存储 (stub) | 物理光标渲染 |

### 设备节点

| 设备 | 现况 | 改造后 |
|---|---|---|
| `/dev/tty` | 有 (canonical mode + console 渲染) | 有 (current->ctty 魔数设备) |
| `/dev/fb` | **无** | **新增** (mmap + read metadata) |
| `/dev/ptmx` | **无** | **新增** (Unix98 PTY master 克隆设备) |
| `/dev/pts0..N` | **无** | **新增** (动态注册 slave，扁平命名) |
| `/dev/keyboard` | 有 | 不变 |

### 启动流程

```
init
 ├─ fork → terminal.elf   (负责 fb 渲染 + 键盘/串口输入分发)
 │    ├─ open /dev/tty → 物理 TTY fd (ctty==NULL → 回退键盘/串口)
 │    ├─ open /dev/fb, read metadata, mmap
 │    ├─ open /dev/ptmx → master fd (/dev/pts0 自动创建)
 │    ├─ open /dev/pts0 → slave fd（使 slave 成为当前 session ctty）
 │    ├─ fork → ash
 │    │    └─ dup2(slave, 0/1/2) → close(slave) → exec("/bin/ash")
 │    │         ash 的 current->ctty 已继承自 fork = slave
 │    │         ash 内 open("/dev/tty") → ctty!=NULL → 返回 slave
 │    ├─ close(slave) → 释放 terminal 持有的 slave fd
 │    └─ loop: poll(tty_fd + pty_master) → render → write to pty
 │
 └─ 应急 console 保持活跃，仅 panic/early boot 使用
```

---

## 模块设计

---

### 1. PTY (Unix98 风格)

#### `/dev/ptmx` — Clone 设备的分派机制

**关键设计问题**：`/dev/ptmx` 是 clone 设备——每次 `open()` 创建独立的 PTY 对。
devfs 的单节点→单 `private_data` 模型无法支持多实例。

**解决方案**：PTY master 不通过 devfs 分派。改用 `FD_PTY_MASTER` 文件类型 +
`file_t` 中的 `pty` 指针，与 `FD_PIPE` 模式一致。

```c
// kernel/include/kernel/file.h

enum file_type {
    FD_NONE = 0,
    FD_VFS,          // regular file via VFS
    FD_PIPE,         // pipe
    FD_DEV,          // device (uses vfs_node)
    FD_PTY_MASTER,   // ← 新增：PTY master fd
};

typedef struct file {
    enum file_type type;
    uint32_t       refcount;
    int            flags;
    uint64_t       offset;
    // FD_VFS / FD_DEV
    struct vfs_node *node;
    // FD_PIPE
    pipe_t         *pipe;
    // FD_PTY_MASTER
    pty_t          *pty;               // ← 新增
} file_t;
```

**devfs open 回调**：`devfs_ops` 新增 `open` 回调。当设备需要在 `open` 时创建
自定义 `file_t`（而非默认 `FD_DEV` 节点），handler 返回 `file_t *`。

```c
struct devfs_ops {
    int (*open)(const char *name, file_t **out_file);  // ← 新增。返回 0 使用 out_file，
                                                        // 返回 -ENOSYS 走默认 FD_DEV
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    uint32_t (*poll)(void *priv, struct poll_table *pt);
    int (*mmap)(vfs_node_t *, vma_t *);
    int (*ioctl)(vfs_node_t *, int cmd, void *arg);
};
```

**ptmx_open 流程**：

```c
static int ptmx_open(const char *name, file_t **out_file)
{
    pty_t *pty = pty_alloc();          // 找空闲 slot，创建两个 pipe
    if (!pty) return -ENOMEM;

    // 动态注册 slave 到 devfs
    char slave_name[16];
    snprintf(slave_name, sizeof(slave_name), "pts%d", pty->index);
    devfs_register_chrdev(slave_name, pty, &pty_slave_ops);

    // 创建 master file_t
    file_t *f = file_alloc();
    f->type  = FD_PTY_MASTER;
    f->pty   = pty;
    f->flags = O_RDWR;
    *out_file = f;
    return 0;
}
```

**fd_read / fd_write / fd_poll 分派**：新增 `FD_PTY_MASTER` case——委托给底层 pipe：

```c
// fd_read 新增:
case FD_PTY_MASTER: {
    // master read = 读 slave → master pipe（ash 的输出）
    pty_t *pty = f->pty;
    if (!pty || !pty->slave_to_master) return -1;
    // 直接借用 FD_PIPE 逻辑但 pipe 来自 pty
    // V1 简化：创建临时 file_t 或抽取共享函数
    ...
}
```

**V1 简化实现**：PTY master 的 `fd_read`/`fd_write` 复用 `FD_PIPE` 的代码。
在 `FD_PTY_MASTER` case 中直接操作 pipe 字段（PTY 的两个 pipe 本质就是 pipe_t）。
实际实现可以提取 `pipe_read(pipe_t *p, void *buf, uint64_t size)` 和
`pipe_write(pipe_t *p, const void *buf, uint64_t size)` 两个内部函数，
然后 `FD_PIPE` 和 `FD_PTY_MASTER` case 都调用它们。

PTY slave 走现有 devfs 路径（`FD_DEV` → `devfs_read` → `pty_slave_read`），
`private_data = &ptys[idx]`。

#### 数据结构

```c
#define PTY_MAX  8

typedef struct pty_struct {
    int         index;              // 0..7
    bool        allocated;

    pipe_t     *master_to_slave;    // terminal.elf → ash
    pipe_t     *slave_to_master;    // ash → terminal.elf

    // termios stub — 存储终端属性供 tcgetattr/tcsetattr
    struct termios  term;

    // 窗口大小
    uint16_t    ws_row;
    uint16_t    ws_col;

    // 前台进程组
    pid_t       pgrp;
} pty_t;
```

读写和 poll 完全委托给底层 pipe——`pty_t` 不需要自己的等待队列。

#### 数据流

Master fd 操作：
- `read(master)` → 从 `slave_to_master` pipe 读（ash 的输出）
- `write(master)` → 写到 `master_to_slave` pipe（ash 的输入）
- `poll(master)` → POLLIN = slave 端有数据可读，POLLOUT = pipe 有空间
- `ioctl(master, TCGETS)` → 读取 slave 的 termios（让 terminal.elf 感知 termios 变化）
- 其他 ioctl → `-ENOTTY`

Slave fd 操作：
- `read(slave)` → 从 `master_to_slave` pipe 读（terminal.elf 的输入）
- `write(slave)` → 写到 `slave_to_master` pipe（ash 的输出）
- `poll(slave)` → 对应 pipe 的 POLLIN/POLLOUT
- `ioctl(slave, TCGETS)` → 返回 `pty->term`
- `ioctl(slave, TCSETS/TCSETSW)` → 存储到 `pty->term`（不改变实际行为）
- `ioctl(slave, TIOCGWINSZ)` → 返回 `pty->ws_row/ws_col`
- `ioctl(slave, TIOCSWINSZ)` → 设置后发 SIGWINCH 到 `pty->pgrp`（**V1 defer**: `task_send_signal` 仅支持单 PID，无 `kill_pgrp` 函数。窗口大小变化暂时存储但不产生信号，terminal.elf 通过 VT100 转义序列控制显示尺寸）
- `ioctl(slave, TIOCGPGRP)` → 返回 `pty->pgrp`
- `ioctl(slave, TIOCSPGRP)` → 设置 `pty->pgrp`

#### termios 初始化

PTY slave **初始化为 raw mode**（`!ICANON, !ECHO, ISIG=0`）。terminal.elf 完全负责终端语义（行编辑、echo、信号生成）。这符合 Linux 终端模拟器的做法——PTY slave 始终 raw，模拟器在上面实现 cooked 行为。

Slave 存储 `tcsetattr` 写入的值供 `tcgetattr` 查询，但 ly 不执行——实际行为始终 raw（无内核端行编辑、无 echo、无信号生成）。

`struct termios` 定义在 `libc/include/termios.h`，由 `kernel/include/kernel/tty.h`
通过 `#include <termios.h>` 引用。PTY 的 `pty->term` 使用同一份定义，
内核与用户空间的布局一致——`tty_ioctl` 已在此假设下运行，PTY 沿用即可。

#### pipe 生命周期与 PTY 释放

PTY 内部的两个 pipe 由 VFS pipe 的引用计数管理，PTY 层不持有额外引用。

**释放时序**：

```
terminal.elf exit → close(master fd) → iput(master node) → 递减 inode 引用
ash exit          → close(slave fd)  → iput(slave node)  → 递减 inode 引用

当 master + slave 的 inode 引用都归零：
  → pty_free(pty)
    1. 遍历所有 task: ctty == pty → ctty_type = CTTY_NONE, ctty = NULL
    2. 关闭 master_to_slave 和 slave_to_master pipe 的写端
       → pipe 引用计数归零 → pipe_free 释放缓冲区
    3. devfs_unregister("/dev/pts/<n>")
    4. pty->allocated = false
```

**关键**: `pty_free` 在 iput 回调中触发（当 master 和 slave 的最后一个引用被释放时）。
但 master 和 slave 节点各自清理时都可能触发——需要引用计数（`refcount` 从 2 开始，
master 和 slave iput 各减 1，归零时释放）。

简化 V1 实现：PTY 不主动释放。所有 8 个 PTY slot 在内核生命周期内存在。
`pty->allocated` 仅在 `ptmx_open` 时设为 true。terminal.elf 退出后 slot 可被
下一个 `ptmx_open` 复用。pipe 在两端 fd 关闭后由 VFS fd 层自动 `iput → pipe_free`。

这种做法避免了引用计数协调的复杂度——PTY 索引是静态数组，不需要动态分配。

**pty->pgrp 生命周期**：
- `ptmx_open` → `pty->pgrp = 0`（初始无前台进程组）
- ash 调用 `tcsetpgrp(slave, pgrp)` → `pty->pgrp = pgrp`
- slave fd 关闭 → `pty->pgrp = 0`（重置，避免指向已退出进程组）
- 下一个 `ptmx_open` 复用 slot 时重新初始化为 0

#### 涉及文件

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/driver/pty.c` | 新建：PTY alloc/free、master/slave 读写、ioctl | ~250 |
| `kernel/include/kernel/pty.h` | 新建：pty_t + PTY_MAX + API 声明 | ~30 |
| `kernel/fs/devfs.c` | ptmx open 时注册 /dev/ptsN | ~20 |
| `kernel/kernel/main.c` | 调用 pty_init() | ~2 |

---

### 2. /dev/fb mmap

#### 设备注册

`/dev/fb` 按字符设备注册到 devfs，提供 `read`（元数据）+ `mmap`（像素缓冲区）。

#### VM_IO 映射类型

新增 `VM_IO` 标志（`vma.h`）：

```c
#define VM_IO  0x80   // 设备 MMIO — 页面不由 pmm/slab 管理
```

`do_mmap` 中校验：`VM_IO | MAP_PRIVATE` 的组合拒绝（设备映射必须 SHARED）。

#### fb_mmap handler

mmap 时一次性填入所有 PTE——因为 fb 物理地址已知且连续，不需要惰性映射：

```c
int fb_mmap(vfs_node_t *node, vma_t *vma)
{
    uint64_t length = vma->vm_end - vma->vm_start;
    // 使用实际 GOP 返回的像素间距（可能含 padding），而非 width*4 的推算值
    uint64_t pitch = Pos.XResolution * sizeof(uint32_t);  // 当前 UEFI GOP 无 padding
    uint64_t fb_size = pitch * Pos.YResolution;

    if (vma->vm_pgoff != 0)               return -EINVAL;
    if (length > fb_size)                  return -EINVAL;
    if (!(vma->vm_flags & VM_SHARED))      return -EINVAL;

    vma->vm_flags |= VM_IO;
    vma->vm_page_prot = PAGE_USER_4K | PAGE_PWT | PAGE_PCD;

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_4K_SIZE) {
        uint64_t pa = Pos.Phy_addr + (va - vma->vm_start);
        vmm_map_4k_page(user_pml4, va, pa, vma->vm_page_prot);
    }
    flush_tlb();  // Eager 填入 PTE 后必须刷新 TLB
    return 0;
}
```

#### read 接口（元数据）

```c
struct fb_info {
    uint32_t width;          // 像素
    uint32_t height;
    uint32_t stride;         // 每行字节数 (= Pos.XResolution * 4)
    uint32_t bpp;            // 32
    uint32_t format;         // 0 = XRGB8888
} __attribute__((packed));
```

`fb_read(node, offset, size, buf)`：`offset=0` 返回 `sizeof(fb_info)` 字节，`offset>0` 返回 0（EOF）。

#### #PF handler

无需特殊处理——PTE 已在 mmap 时填入，不会因惰性映射触发 #PF。越界访问正常 SIGSEGV。

#### devfs_ops 重构

`devfs_register_chrdev` 改为接收 ops struct：

```c
struct devfs_ops {
    int (*read)(vfs_node_t *, uint64_t offset, uint64_t size, void *buf);
    int (*write)(vfs_node_t *, uint64_t offset, uint64_t size, void *buf);
    uint32_t (*poll)(void *priv, struct poll_table *pt);
    int (*mmap)(vfs_node_t *, vma_t *);
    int (*ioctl)(vfs_node_t *, int cmd, void *arg);
};

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);
```

#### do_mmap 的设备 mmap 分派

当前 `do_mmap` 创建 VMA、存储 `vm_file`，但**从未调用文件/设备的 mmap handler**。
需要新增分派路径。重构 `do_mmap` 的步骤 3-5：

```c
// kernel/memory/vma.c — do_mmap 重构后的步骤 3-5

// 3. prot → page/flags (不变)
// 4. 判断映射类型并处理（新增）
// 5. 分配 VMA + 插入（统一）

vfs_node_t *file_node = NULL;
int is_device_mmap = 0;

// ── 第 4 步: 检查并处理设备/文件映射 ──
if (!(flags & MAP_ANONYMOUS)) {
    file_t *file = current->files->fd[fd];
    file_node = vfs_node_get(file->node);

    // 设备节点有 mmap handler → 走设备路径
    if (file_node && file_node->ops->mmap) {
        is_device_mmap = 1;
        
        // 设备映射必须 SHARED（VM_IO 不可写时复制）
        if (!(flags & MAP_SHARED))
            return -EINVAL;
        
        // 先分配 VMA（统一在第 5 步）
        // 但 mmap handler 需要先运行来填入 PTE
        // → 重排：设备路径提前分配 VMA 并调用 handler
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
        // mmap handler 已填入 PTE（如 fb_mmap）
        vma_insert(current->mm, vma);
        return (int64_t)vma->vm_start;
    }
    // 普通文件映射走现有逻辑（vfs_read 惰性填页）
}

// ── 第 5 步: 非设备路径 — 分配 VMA (现有逻辑) ──
vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
if (!vma) { ... }
// ... 现有 VMA 初始化 + vma_insert
```

设备路径在第 4 步提前分配 VMA 并调用 `node->ops->mmap`（由 handler 填入 PTE），
然后直接 return。非设备路径（ANON、普通文件）保持现有的步 5 逻辑不变。
两个路径各自独立，不重复不交叉。

#### VFS 扩展

`vfs_ops` struct 新增 `mmap` 回调：

```c
struct vfs_ops {
    int (*read)(...);
    int (*write)(...);
    int (*readdir)(...);
    int (*mmap)(vfs_node_t *, vma_t *);   // 新增，可为 NULL
};
```

`vfs_node_t` 的 `ops` 字段已指向文件系统的 `vfs_ops`（如 devfs_ops）。
设备 mmap handler 通过 `node->ops->mmap` 被调用。

#### 涉及文件

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/driver/fb.c` | 新建：fb_mmap + fb_read + 注册 | ~60 |
| `kernel/fs/devfs.c` | devfs_mmap 分派 + ops struct 重构 | ~30 |
| `kernel/include/fs/devfs.h` | devfs_ops struct | ~15 |
| `kernel/include/kernel/vma.h` | VM_IO 定义 | +2 |
| `kernel/memory/vma.c` | do_mmap 设备分派 + VM_IO 校验 | ~20 |
| `kernel/include/fs/vfs.h` | vfs_ops 加 mmap | +2 |
| `kernel/fs/vfs.c` | 默认 mmap = NULL（无实现） | +1 |
| `kernel/kernel/main.c` | 注册 /dev/fb | ~3 |

---

### 3. tty.c 退化

#### 现状 (~350 行)
1. cooked ring buffer — `tty_cooked_push/pop`
2. canonical processing — `tty_canon_process`（行缓冲、退格、^C、echo）
3. blocking read — `tty_read`（cooked ring → canonical → 用户缓冲区）

#### 目标 (~100 行)
1. raw ring buffer — 不变（cooked 改名 raw）
2. canonical processing — **完全移除**
3. blocking read — **简化**为直接从 ring 拷贝到用户缓冲区

#### 退化后的 tty_t

```c
typedef struct tty_struct {
    // Raw ring buffer
    spinlock_T   ring_lock;
    char         ring[TTY_BUF_SIZE];
    volatile int head;
    volatile int tail;

    // Read wait queue
    spinlock_T   read_wait_lock;
    list_t       read_wait;

    // Poll
    list_t       read_poll;

    // Output callbacks（应急 console 用）
    void (*output_char)(char c);
    void (*echo_char)(char c);

    // ── 移除的字段 ──
    // char  line[TTY_BUF_SIZE];
    // int   line_len, read_pos;
    // bool  line_ready;
    // uint8_t lflag;
    // int64_t pgrp;
} tty_t;
```

#### 退化后的 tty_read

简化为从 ring buffer 直接拷贝，不做 canonical：

```
tty_read(tty, buf, size, nonblock):
  loop:
    // Phase 1: ring → buf（直接拷）
    while ring 非空 && copied < size:
      *buf++ = ring[tail]; tail++; copied++;
    if copied > 0: return copied

    if nonblock: return 0
    if signal_pending_fatal(): return 0

    // Phase 2: sleep
    enqueue read_wait
    double-check ring 非空
    schedule()
    dequeue read_wait
    check signals
```

#### 移除的功能

| 函数/字段 | 去向 |
|---|---|
| `tty_canon_process()` | terminal.elf 行编辑 |
| `tty->line[]`, `line_len`, `read_pos`, `line_ready` | terminal.elf 行缓冲 |
| `tty->lflag` (ICANON/ECHO/ISIG) | 概念上移到 terminal.elf |
| `tty->pgrp` | PTY slave 的 `pty->pgrp` |
| `TTY_L_ICANON/ECHO/ISIG` 宏 | 删除 |

#### 键盘模块适配

`keyboard.c` 中 `translate_and_push()` 的方向键分支不再检查 `kbd_tty->lflag & TTY_L_ICANON`——始终推送 VT100 转义序列到 TTY：

```c
if (c >= 0x100) {
    // 始终发 VT100 序列，不再检查 ICANON
    switch (c) {
    case K_UP:    push_vt100_seq(kbd_tty, 'A'); break;
    // ...
    }
    return;
}
```

#### 涉及文件

| 文件 | 改动 |
|---|---|
| `kernel/include/kernel/tty.h` | 删除 canonical 字段 + lflag + pgrp |
| `kernel/tty/tty.c` | tty_read 简化，删除 tty_canon_process/cooked pop/push |
| `kernel/driver/keyboard.c` | 方向键不再检查 ICANON |

---

### 4. console.c 退化

#### 移除
- VT100 CSI 状态机全部（`CSI_NORMAL/ESC/BRACKET/PARAM/QMARK`）
- 光标闪烁（`console_blink_tick`、`console_draw_blink`）
- PIT tick 回调

#### 保留
- `putchar_at()` — 字符渲染（在 `printk.c` 中，不动）
- `console_scroll()` — 简单滚动
- `console_putchar()` — 退化版本

#### 退化后的 console_putchar

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

    int max_cols = Pos.XResolution / font->width;
    if (term_cursor_col >= max_cols) {
        term_cursor_col = 0;
        term_cursor_row++;
    }
    int max_rows = Pos.YResolution / font->height;
    if (term_cursor_row >= max_rows)
        console_scroll();
}
```

#### 生存期管理

```c
void console_surrender_fb(void)    // terminal.elf 启动后调用
{
    term_initialized = false;       // console_putchar → no-op
}

void console_force_enable(void)    // panic 路径调用
{
    term_initialized = true;        // 恢复 fb 输出
}
```

**行为约定**：
- terminal.elf 启动前 → `term_initialized = true`，`printk`/`log_err`/`log_debug` 输出到 fb
- terminal.elf 启动后 → `console_surrender_fb()`，`term_initialized = false`
- 之后所有内核 log（`log_debug`、`log_err`、`log_info`）仅 serial 输出
- fb 由 terminal.elf 独占写入
- `console_force_enable()` 仅在 panic 路径调用——绕过 terminal.elf，直写 fb 显示崩溃信息
- **Panic fb 竞态**：panic 时 terminal.elf 可能仍在运行并同时写 fb。两者通过
  不同虚拟地址（kernel direct map vs. user mmap）写入同一物理 framebuffer，
  内容可能互相覆盖。panic 是终局路径，此竞态可接受——自旋失败和崩溃信息可见性
  是合理的权衡。
- `LOG_TARGET=both` 配置下，终端启动后等价于 `LOG_TARGET=serial`；需在文档中记录

#### 涉及文件

| 文件 | 改动 |
|---|---|
| `kernel/tty/console.c` | 删除 CSI 状态机 + 光标闪烁，console_putchar 退化，新增生存期函数 |
| `kernel/include/kernel/console.h` | 更新声明 |
| `kernel/driver/pit.c` | 移除 console_blink_tick 调用 |

---

### 5. /dev/tty 魔数设备（current->ctty）

#### 实现

```c
// kernel/include/kernel/sched.h
enum ctty_type { CTTY_NONE = 0, CTTY_PHYS, CTTY_PTY };

struct task_struct {
    // ...
    enum ctty_type  ctty_type;   // CTTY_NONE / CTTY_PHYS / CTTY_PTY
    void           *ctty;        // 指向 tty_t（CTTY_PHYS）或 pty_t（CTTY_PTY）
};
```

- `fork()` 时子进程继承父进程的 `ctty_type` 和 `ctty`
- `/dev/tty` open 时根据 `ctty_type` 分派：
  - `CTTY_PTY` → 遍历 device 表找 PTY slave（`private_data == ctty`）
  - `CTTY_PHYS` → 直接查找物理 TTY 的 devfs 节点（特殊路径，见下方）
  - `CTTY_NONE` → 回退物理 TTY
- session leader 首次打开 PTY slave 时：`current->ctty_type = CTTY_PTY; current->ctty = pty`
- PTY 释放时：遍历所有 task，`ctty == this → ctty_type = CTTY_NONE, ctty = NULL`
- `/dev/tty` fd 在 open 时解析 ctty，之后无论 ctty 如何变化，fd 指向的设备不变

#### /dev/tty open handler 实现草图

```c
// kernel/fs/devfs.c
static vfs_node_t *tty_dev_open(const char *name)
{
    if (strcmp(name, "/dev/tty") != 0) return NULL;

    void *target = NULL;

    if (current->ctty_type == CTTY_PTY) {
        target = current->ctty;  // 指向 pty_t
    } else {
        // CTTY_NONE 或 CTTY_PHYS → 物理 TTY
        target = kbd_tty;
    }

    // 遍历 device 表匹配 private_data
    for (int i = 0; i < device_count; i++) {
        if (devices[i].private_data == target && devices[i].type == VFS_CHRDEV) {
            vfs_node_t *node = vfs_node_alloc();
            node->type = VFS_CHRDEV;
            node->fs_data = (void *)(uintptr_t)i;
            node->ops = &devfs_ops;
            return node;
        }
    }
    return NULL;  // → vfs_open 返回 -ENXIO
}
```

**前置依赖**: 物理 TTY 注册时须传 `kbd_tty` 为 `private_data`（当前传 `NULL`）：

```c
// kernel/kernel/main.c 或 devfs_init
devfs_register_chrdev("tty", kbd_tty, &tty_devfs_ops);
```

**物理 TTY 的额外路径**：`/dev/tty` 成为魔数设备后，物理 TTY 仍需一个固定路径名
供 terminal.elf 打开。保留现有 `/dev/tty` 命名的同时，物理 TTY 注册为 `/dev/tty0`
作为别名（或直接用 `/dev/tty` + CTTY_NONE 回退）。terminal.elf 启动时
`ctty_type == CTTY_NONE`，打开 `/dev/tty` 即获得物理 TTY fd。

`/dev/tty` 不通过常规 `vfs_lookup + vfs_open` 路径——`vfs_open` 在路径为
`"/dev/tty"` 时调用 `tty_dev_open`，不走常规 devfs directory 查找。

> **已知设计瑕疵**: `strcmp(path, "/dev/tty")` 硬编码路径字符串。后续可改为
> 在 devfs 目录中注册一个 `VFS_REDIRECT` 节点类型——`open` 时检查节点类型
> 标志走特殊分派逻辑。V1 保留 strcmp。

#### 时序与行为

terminal.elf 的启动顺序是关键——读键盘的 `/dev/tty` fd 必须在设置 ctty 之前打开：

```
terminal.elf:
  1. open("/dev/tty")        → ctty_type==CTTY_NONE → 回退到物理 TTY (键盘/串口)
  2. open("/dev/ptmx")       → master fd，创建 pty[0]
  3. open("/dev/pts0")      → slave fd, current->ctty_type=CTTY_PTY, current->ctty = &ptys[0]
  4. fork()                  → ash 继承 ctty_type=CTTY_PTY, ctty=ptys[0]
  5. close(slave)            // terminal 不需要 slave fd（只用 master）
  6. current->ctty_type = CTTY_NONE;  // terminal.elf 清空自己的 ctty

  // terminal.elf 的 tty_fd 仍指向物理 TTY（在步骤 1 已解析完毕）
  // ash 的 /dev/tty → ctty_type==CTTY_PTY → PTY slave

ash (fork 后):
  open("/dev/tty")           → ctty_type==CTTY_PTY → 遍历 device 表匹配 ptys[0] → slave fd
  tcgetpgrp/tcsetpgrp        → 操作 PTY slave（兼容）
```
  open("/dev/tty")           → ctty==&ptys[0] → PTY slave fd
  tcgetpgrp/tcsetpgrp        → 操作 PTY slave（兼容）
```

#### 涉及文件

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/include/kernel/sched.h` | task 结构体加 `void *ctty` | +2 |
| `kernel/sched/task.c` | fork 继承 ctty | +2 |
| `kernel/driver/pty.c` | slave open 设 ctty，free 清 ctty | ~10 |
| `kernel/fs/devfs.c` | /dev/tty 魔数设备注册 + open handler | ~20 |

---

### 6. terminal.elf

#### 职责

1. 从 `/dev/tty` 读键盘/串口输入（统一字符流）
2. 从 PTY master 读 ash 输出
3. 解析 ash 输出中的 VT100 转义序列
4. PSF2 字型渲染到 framebuffer
5. 行编辑（echo、backspace、^C、方向键透传）
6. 处理后输入写入 PTY master → ash

#### 主循环

```c
int tty_fd  = open("/dev/tty", O_RDONLY);   // ctty==NULL → 回退物理 TTY
int fb_fd   = open("/dev/fb", O_RDWR);
int pty_fd  = open("/dev/ptmx", O_RDWR);    // 返回 master
// /dev/pts0 被自动创建

// 读取 fb 元数据
struct fb_info info;
read(fb_fd, &info, sizeof(info));

// mmap framebuffer
uint32_t *fb = mmap(NULL, info.height * info.stride,
                    PROT_WRITE, MAP_SHARED, fb_fd, 0);

// 使 PTY slave 成为控制终端
int slave = open("/dev/pts0", O_RDWR);    // current->ctty = slave

// Fork ash
int ash_pid = fork();
if (ash_pid == 0) {
    dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
    close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
    exec("/bin/ash", NULL);
}

close(slave);  // terminal.elf 不需要在 master 端持有 slave fd

// 主循环
struct pollfd fds[2] = {
    {.fd = tty_fd, .events = POLLIN},
    {.fd = pty_fd, .events = POLLIN},
};
char buf[256];

while (1) {
    poll(fds, 2, -1);

    if (fds[0].revents & POLLIN) {    // /dev/tty: 键盘输入
        int n = read(tty_fd, buf, sizeof(buf));
        handle_input(buf, n, pty_fd, ash_pid, fb, &info);
    }
    if (fds[1].revents & POLLIN) {    // pty master: ash 输出
        int n = read(pty_fd, buf, sizeof(buf));
        handle_output(buf, n, fb, &info);
    }
}
```

#### 输入处理（handle_input）— 双模式

terminal.elf 通过 `ioctl(pty_master, TCGETS)` 持续检查 slave 的 termios 状态，
在 cooked 和 raw 两种模式间切换：

**Cooked 模式**（`c_lflag & ICANON`，终端初始状态）：
terminal.elf 本地做行编辑 + echo + ^C 处理。方向键按 VT100 透传。

**Raw 模式**（`!ICANON`，被 vi/less/readline 等程序设置）：
terminal.elf 将键盘输入逐字节透传给 PTY master，不做任何行编辑、echo 或信号生成。

模式切换时机：每次 poll 返回后，在消费数据前检查 termios。

```
对于每个字符 c（cooked 模式）：
  if c == '\r' || c == '\n':
      line_buf[line_len++] = '\n'
      write(pty_fd, line_buf, line_len)
      line_len = 0
      if echo: render_string("\r\n")
  else if c == '\x7f' || c == '\b':
      if line_len > 0:
          line_len--
          if echo: render_backspace()
  else if c == '\x03':                   // ^C
      if termios.c_lflag & ISIG:
          kill(ash_pid, SIGINT)
  else if c == '\x04' && line_len == 0: // ^D → EOF
      传送 EOF 到 PTY master
  else if c == '\x1b':                  // ESC 开始 → 方向键
      读后续 2 字节，透传完整 VT100 序列给 PTY master
  else if c >= ' ':
      line_buf[line_len++] = c
      if echo: render_char(c)

对于 raw 模式：
  所有字节（包括方向键）逐字节透传给 PTY master
  不做任何行缓冲、echo、^C 处理
```

**V1 限制**：仅 ash（cooked 模式程序）已验证。raw mode 程序（vi、less）的
透传路径已实现但尚未测试——因为 PTY 缺少 `TIOCPKT` 通知，terminal.elf
需轮询 termios，存在最多一个 poll 周期的延迟。

**已知交互: terminal.elf echo + ash readline**：PTY slave 初始 `!ECHO`，
ash 不自行 echo。terminal.elf 在 cooked 模式下 echo 每个字符到 fb。
ash 收到整行后执行命令。基本命令（`ls`、`echo hello`）正常。
但 ash 的 readline（退格、^U、^W、tab 补全）依赖逐字节交互——
terminal.elf 的行编辑已缓冲整行后才发送，readline 可能对已完成的
行进行第二次处理。此交互在 V1 可接受但不完美，后续优化方向是
terminal.elf 进入真正的逐字节透传模式对接 readline。

#### VT100 状态机（handle_output）

```
状态: NORMAL | ESC | CSI_PARAM

支持序列：
  ESC [ n A      上移光标
  ESC [ n B      下移光标
  ESC [ n C      右移光标
  ESC [ n D      左移光标
  ESC [ n K      清到行尾 (n=0: 光标到行尾, n=1: 行首到光标, n=2: 整行)
  ESC [ 2 J      清屏
  ESC [ H        光标归位 (0,0)
  ESC [ ? 25 h   显示光标
  ESC [ ? 25 l   隐藏光标

普通字符 → render_char_at_cursor(c)
\b → 光标左移 1
\t → 跳下一个 8 列对齐
\r → 光标列 = 0
\n → 光标行+1，必要时 scroll up
```

#### 渲染器

```c
// PSF2 字型到 fb——移植内核 printk.c 的 putchar_at 逻辑
void render_char(uint32_t *fb, struct fb_info *info,
                 psf2_t *font, int col, int row,
                 uint32_t fg, uint32_t bg, char c);

// 滚动：memmove fb 行 1..N-1 到 0..N-2，清最末行
void fb_scroll(uint32_t *fb, struct fb_info *info);
```

PSF2 字体内嵌到 terminal.elf（`objcopy -B i386 -I binary -O elf64-x86-64`）。

#### 文件布局

```
user/terminal.c      ~400 行 (新建)
  ├── vt100 parser    ~80 行
  ├── renderer        ~80 行 (PSF2 渲染 + scroll)
  ├── input handler   ~80 行 (行编辑 + echo)
  ├── main loop       ~50 行 (poll + dispatch)
  └── PTY/ash setup   ~40 行
```

---

### 7. busybox ash

#### 无需改动

ash 的 `setjobctl()` 正常运行：

```c
setjobctl(on):
    fd = open("/dev/tty", O_RDWR);   // → 查 current->ctty → PTY slave
    fcntl(fd, F_DUPFD_CLOEXEC, 10);  // → PTY slave 支持
    tcgetpgrp(fd);                   // → PTY slave TIOCGPGRP
    tcsetpgrp(fd, pgrp);             // → PTY slave TIOCSPGRP
```

ash 的读输入路径 `preadfd()` 不变——fd 0 已 dup2 到 PTY slave，`read(0, ...)` 等价于从 master→slave pipe 读。

目前 ash 的 `_PATH_TTY` 宏（`/dev/tty`）需要确保编译进 busybox 时是这个路径。检查现有 busybox 配置，如已是 `/dev/tty` 则无需改动。

---

## 总计

```
内核改动:
  kernel/driver/pty.c         | ~300  (新建：PTY alloc/free + master/slave I/O + ptmx_open + devfs 注册)
  kernel/driver/fb.c          |  ~60  (新建：fb mmap + read)
  kernel/include/kernel/pty.h |  ~30  (新建：pty_t + 宏)
  kernel/fs/file.c            |  ~30  (FD_PTY_MASTER case in fd_read/fd_write/fd_poll)
  kernel/include/kernel/file.h|   +5  (FD_PTY_MASTER + file->pty)
  kernel/fs/devfs.c           |  ~70  (mmap 分派 + ops struct + ptmx/pts + /dev/tty 魔数 + devfs open 回调)
  kernel/include/fs/devfs.h   |  ~15  (devfs_ops struct + open 回调)
  kernel/include/fs/vfs.h     |   +2  (vfs_ops.mmap)
  kernel/fs/vfs.c             |   +1  (默认 mmap = NULL)
  kernel/include/kernel/vma.h |   +2  (VM_IO)
  kernel/memory/vma.c         |  ~20  (do_mmap 设备分派 + VM_IO 校验)
  kernel/tty/console.c        | -120  (删除 CSI 状态机 + 光标)
  kernel/tty/tty.c            | -250  (删除 canonical + 简化 read)
  kernel/include/kernel/tty.h |  -20  (精简字段)
  kernel/include/kernel/console.h | +8 (更新声明)
  kernel/include/kernel/sched.h | +5 (ctty_type + ctty 字段, fork 继承)
  kernel/sched/task.c         |   +4  (fork 继承 ctty_type + ctty)
  kernel/driver/keyboard.c    |   +2  (方向键不再检查 ICANON)
  kernel/kernel/main.c        |  ~10  (注册 fb + PTY init)
  ─────────────────────────────────
  内核净增                     | ~400 行 (+610 新增, -390 删除)

用户态改动:
  user/terminal.c             | ~400  (新建)
  user/Makefile               |   +5  (terminal.elf 构建规则)
  ─────────────────────────────────
  用户态净增                   | ~400 行

总计净增                       | ~760 行
```

---

## 实现顺序

1. **devfs_ops struct 重构** — 包括 VFS vfs_ops.mmap + devfs_ops.open 回调
2. **FD_PTY_MASTER** — file_t 加 pty 指针、fd_read/fd_write/fd_poll 分派
3. **VM_IO + do_mmap 设备分派** — + /dev/fb mmap
4. **tty.c 退化** — 键盘输入路径简化
5. **console.c 退化** — 移除 VT100 解析
6. **PTY 实现** — pty_alloc、master/slave I/O、current->ctty、/dev/tty 魔数 open、master TCGETS、ptmx_open → FD_PTY_MASTER
7. **terminal.elf** — 用户态终端（含 termios 感知双模式）
8. **busybox ash 对接 + init 编排** — 集成测试
9. **测试 + 文档**

---

## Deferred Items

以下功能评审中确认暂缓，V1 不实现：

| 项目 | 说明 |
|---|---|
| `ptsname`/`grantpt`/`unlockpt` | libc stub 缺失；terminal.elf 硬编码 `/dev/pts0`。后续支持多 PTY 时需实现 |
| `TIOCPKT` 包模式 | PTY master 在 slave termios/winsize 变化时无通知。terminal.elf 改用 poll 轮询 TCGETS，最多一个 poll 周期延迟 |
| `TIOCSCTTY` | 允许进程主动设置控制终端（某些 daemon 化路径需要）。V1 没有调用者 |
| `kill_pgrp` / SIGWINCH | `task_send_signal` 仅支持单 PID，无进程组广播。TIOCSWINSZ 存储 winsize 但不发信号。terminal.elf 通过 VT100 序列控制显示尺寸 |
| `/dev/tty` strcmp 路径 | 硬编码字符串是设计瑕疵。后续改为 VFS_REDIRECT 节点类型 + devfs 特殊节点 |
| devfs 子目录 `/dev/pts/N` | 当前用 `/dev/pts0` 扁平命名。后续需 devfs_mkdir + 多级 lookup → `/dev/pts/0` |
| 键盘扩展键 (PGUP/PGDN/INSERT) | `ext_scancode_tbl` 中 0x49(PGUP)、0x51(PGDN)、0x52(INSERT) 当前映射为 UP/DOWN/HOME，后续可独立处理 |
| terminal.elf echo + readline | 目前 terminal.elf 缓冲整行再发送。后续改为逐字节透传适配 ash readline 的退格/补全/^U 等 |
| fb mmap 2MB huge page | V1 使用 4KB 页映射 fb，正确但 TLB 效率低。后续改为 2MB 页减少 TLB 压力 |
| fb stride padding | V1 stride = width * 4。部分 UEFI GOP 有行末 padding，需要 `Pos.pixels_per_line` 字段 |
| raw mode 程序测试 | V1 仅测试 cooked mode（ash）；vi/less 等 raw mode 程序的透传路径已实现但未验证 |
| devfs_register_chrdev 迁移 | `devfs_init` 中 5 个现有设备（null/zero/random/serial/tty）需迁移到 ops struct |
