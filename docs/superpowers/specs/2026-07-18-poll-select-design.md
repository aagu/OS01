# poll/select 实现方案 — v2 (评审后修订)

> **日期**: 2026-07-18
> **修订**: 2026-07-18 — 14 条评审意见全量修复
> **范围**: syscall `poll`/`select`/`ppoll` (Linux ABI 7/23/271)
> **核心依赖**: pipe wait queue 改造

---

## 1. 现状分析

### 1.1 已有能力

| 能力 | 实现 | 文件 |
|------|------|------|
| 通用 wait queue | `wait_queue_t` + 4 原语 (init/sleep/wake_one/wake_all) | `kernel/intr/wait.c` |
| mutex/futex 阻塞 | 用 `wait_queue_t` 的标准 enqueue+sleep+双检模式 | `kernel/mutex.c`, `kernel/futex.c` |
| TTY 阻塞读 | 使用 `tty->read_wait` (raw `list_t`)，手动管理 enqueue/sleep/wake | `kernel/tty/tty.c:189` |
| TTY FIONREAD | `ioctl(0x541B)` → 返回 cooked ring buffer 可读字节数 | `kernel/tty/tty.c:366` |
| `task_t.io_wait_node` | 每个 task 内嵌 1 个 `list_t`，可挂到一个 wait queue | `kernel/include/kernel/task.h:144` |
| libc `poll()` | **假的 stub**！只处理 fd=0 且只识别 stdin | `libc/unistd/busybox_stubs.c:42` |

### 1.2 缺失项

| 缺失 | 严重度 | 说明 |
|------|--------|------|
| **pipe 无 wait queue** | 🔴 致命 | `fd_read`/`fd_write` 对 pipe 用 busy-loop `schedule()` |
| **无 poll/select syscall** | 🔴 致命 | syscall table 最多到 47 (SYS_futex) |
| **无 fd_poll 能力** | 🔴 致命 | file_t/vfs_ops 都没有 poll 方法 |
| **单 io_wait_node** | 🟡 重大 | 一个 task 只能挂在一个 wait queue 上，poll 需要同时等多 fd |
| **devfs 无 poll 转发** | 🟡 重大 | TTY 的 poll 需要整个 devfs→tty 链路 |
| **测试框架无 pipe/poll 覆盖** | 🟢 中等 | systest 有 pipe 基本测试但无 poll 用例 |

### 1.3 当前数据流

```
pipe 写:  fd_write() → spin_lock → while(!pipe_full) copy → spin_unlock
                                                          → schedule()  (busy-wait!)

pipe 读:  fd_read()  → spin_lock → while(!pipe_empty) copy → spin_unlock
                                                          → schedule()  (busy-wait!)

TTY 读:   tty_read() → drain cooked buffer → 有数据? return
                                            → list_add_to_before(&tty->read_wait, io_wait_node)
                                            → schedule()  (proper blocking!)
                                            → 被 IRQ→tty_wake_waiters 唤醒

libc poll: poll(fds,1,-1) → fds[0].revents = POLLIN → return 1  (总是返回 readable!)
          poll(fds,1,50)  → for (t=0; t<50*10000; t++) FIONREAD  (spin-wait!)
```

**核心矛盾**: pipe 和 TTY 的等待机制不对称。TTY 有正确的阻塞语义，pipe 没有。

---

## 2. 设计目标

1. **正确性**: poll 必须在 fd 真正可读/可写时才返回，不能虚假唤醒
2. **超时支持**: timeout=-1(无限), 0(非阻塞), >0(毫秒)
3. **多 fd 同时等待**: 这是 poll 区别于 read 的核心价值
4. **最小侵入**: 尽量沿用已有的 wait_queue_t 和 list_t 基础设施
5. **解锁网络栈**: socket poll 回调是 lwIP 集成的硬依赖

---

## 3. 核心设计挑战：多 fd 同时等待

### 3.1 问题

`task_t` 只有一个 `io_wait_node`（`list_t`），只能挂在一个 wait queue 上。`poll(fds, 3, -1)` 需要同时等 stdin (TTY)、pipe 读端、pipe 写端三个 fd 中的任意一个就绪。

### 3.2 方案选择：poll_table + 级联唤醒 + 双队列

给每个支持 poll 的对象（pipe, tty, socket）添加一个独立的 `list_t` 队列专门给 poll entry 使用（与 task 直接阻塞用的 `wait_queue_t` 分离）。poll 调用时注册 poll_wait_entry 到每个 fd 的 poll 队列上。任一 fd 就绪时级联唤醒 poller。

```
                     ┌──────────────────┐
                     │   do_poll()      │
                     │   poll_table pt  │  ← 栈对象
                     │   .wq (local)    │
                     │   .entries[16]   │
                     └──┬───┬───┬───────┘
                        │   │   │
              ┌─────────┘   │   └──────────┐
              ▼             ▼              ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ pipe_t   │ │ tty_t    │ │ socket   │
        │ .rw_poll │ │ .r_poll  │ │ .wq      │
        │  ├─entry │ │  ├─entry │ │  ├─entry │
        │ .rw_wait │ │ .r_wait  │ │          │
        │  └─task  │ │  └─task  │ │          │
        └──────────┘ └──────────┘ └──────────┘

        pipe 的双队列:
        - read_wait  (wait_queue_t):  挂 task_t.io_wait_node (直接 read() 阻塞)
        - write_wait (wait_queue_t):  挂 task_t.io_wait_node (直接 write() 阻塞)
        - read_poll  (list_t):        挂 poll_wait_entry_t.node (poll 等待)
        - write_poll (list_t):        挂 poll_wait_entry_t.node (poll 等待)

        每个 entry.callback → 唤醒 pt.wq → 重新扫描所有 fd
```

| 优点 | 缺点 |
|------|------|
| 精确唤醒（无虚假唤醒） | 需要引入 `poll_wait_entry` 新概念 |
| task node 和 poll entry 类型完全隔离 | 每个 pipe 多 2 个 `list_t` (32 bytes) |
| 与 Linux 的 `poll_table` 模式一致 | TTY/devfs 各需增加 poll 回调 |
| 网络栈可直接复用 | |

---

## 4. 数据结构设计

### 4.1 新增: poll_wait_entry_t

```c
// kernel/include/kernel/poll.h (新文件)

#define POLL_MAX_FDS  16    // 一次 poll 最多等 16 个 fd

// 挂载在 fd 私有 poll 队列上的轻量条目
// 当 fd 就绪时，其 wake 路径级联唤醒 entry.poll_wq
//
// 关键字段 fd_lock：poll_table_cleanup 和 fd 的 wake 路径可能并发
// 摘除 node，必须持有同一个锁保护链表。fd_lock 指向保护 poll_list
// 的锁（通常 fd 对象的 spinlock）。
typedef struct poll_wait_entry {
    list_t       node;        // 挂在 fd 的 poll 链表上（如 pipe_t.read_poll）
    wait_queue_t *poll_wq;    // 指回 poll 系统调用的 wait queue（栈变量）
    spinlock_T   *fd_lock;    // 保护 node 所在链表的锁（对应 pipe_t.lock / tty_t.xxx_lock）
} poll_wait_entry_t;

// 每个 poll() 系统调用的栈上上下文
typedef struct poll_table {
    wait_queue_t        wq;                      // 主等待队列（局部变量）
    poll_wait_entry_t   entries[POLL_MAX_FDS];   // 静态数组，避免堆分配
    int                 nent;                    // 活跃 entry 数量
    bool                triggered;               // 短路：已有 fd 就绪
} poll_table_t;
```

### 4.2 修改: pipe_t — 双队列

```diff
  // kernel/include/kernel/file.h
  typedef struct pipe {
      char          buf[PIPE_SIZE];
      int           head, tail;
      int           readers, writers;
      spinlock_T    lock;
+     wait_queue_t  read_wait;    // task 直接阻塞 (fd_read, task_t.io_wait_node)
+     wait_queue_t  write_wait;   // task 直接阻塞 (fd_write)
+     list_t        read_poll;    // poll entry (fd_poll, poll_wait_entry_t.node)
+     list_t        write_poll;   // poll entry (fd_poll)
  } pipe_t;
```

### 4.3 修改: tty_t — 双队列

```diff
  // kernel/tty/tty.c (tty_t 结构体)
  typedef struct tty_struct {
      ...
-     list_t        read_wait;    // 原有
+     list_t        read_wait;    // task 直接阻塞 (tty_read, task_t.io_wait_node)
+     list_t        read_poll;    // poll entry (poll_wait_entry_t.node)
      spinlock_T    cooked_lock;
      ...
  } tty_t;
```

### 4.4 新增: devfs poll 回调

```diff
  // kernel/include/fs/devfs.h
  typedef struct devfs_device {
      const char *name;
      void       *private_data;
      int  (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
      int  (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
+     // poll: 检查设备是否可读/可写。pt 用于在设备不可读时注册 poll entry。
+     // 若 ops 不提供此方法，fd_poll 回退到默认行为（总是 ready）。
+     uint32_t (*poll)(void *priv, poll_table_t *pt);
  } devfs_device_t;
```

同时修改注册 API 签名，增加 poll 参数：

```diff
  // kernel/include/fs/devfs.h — 公开 API
  int devfs_register_chrdev(const char *name, void *private_data,
      int (*read)(vfs_node_t *, uint64_t, uint64_t, void *),
-     int (*write)(vfs_node_t *, uint64_t, uint64_t, void *));
+     int (*write)(vfs_node_t *, uint64_t, uint64_t, void *),
+     uint32_t (*poll)(void *priv, poll_table_t *pt));

  // 内核代码中所有调用点需更新（约 5 处）：
  // - main.c: keyboard_devfs_read → 传 NULL poll
  // - main.c: fb_dev_write      → 传 NULL poll
  // - devfs.c devfs_init(): null/zero/random/serial/tty → 传对应 poll 或 NULL
```

### 4.5 不变的部分

- `file_t` **不加** poll 函数指针。`fd_poll()` 通过 `switch (f->type)` 分发（与 `fd_read`/`fd_write` 一致）。
- `task_t` **不加** 字段。poll_wait_entry 在 poll 系统调用的栈上分配。

---

## 5. poll_table 操作原语

```c
// kernel/fs/poll.c

// 用户态 poll 事件定义 (与 Linux ABI 一致)
#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020
#define POLLRDNORM  0x040
#define POLLRDBAND  0x080
#define POLLWRNORM  0x100
#define POLLWRBAND  0x200

// ── poll_table 操作 ──────────────────────────────────────────

// 初始化 per-syscall poll table
// 每次循环入口调用 — wq 只初始化一次，entry 数组每次重置
static void poll_table_init(poll_table_t *pt) {
    pt->nent = 0;
    pt->triggered = false;
}

// 首次调用时初始化 wq + 所有 entry node
static void poll_table_setup(poll_table_t *pt) {
    wait_queue_init(&pt->wq);
    for (int i = 0; i < POLL_MAX_FDS; i++)
        list_init(&pt->entries[i].node);
}

// 将当前 fd 的 poll 链表注册到 poll table
// poll_list:  fd 的 poll 队列（如 &pipe->read_poll）
// fd_lock:    保护该 poll_list 的锁（如 &pipe->lock）
//
// 当 fd 就绪时，fd 的 wake 路径遍历 poll_list，对每个 entry
// 调用 wait_queue_wake_all(e->poll_wq) 级联唤醒 polling task。
static void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock)
{
    if (pt->nent >= POLL_MAX_FDS || pt->triggered)
        return;

    poll_wait_entry_t *e = &pt->entries[pt->nent++];
    e->poll_wq = &pt->wq;
    e->fd_lock = fd_lock;

    // 挂在 fd 的 poll 链表上。fd 持有 fd_lock 保护此链表。
    list_add_to_before(poll_list, &e->node);
}

// 清理所有 poll entry — 从 fd 的 poll 链表中安全摘除
//
// 并发安全：fd 的 wake 路径持有 fd_lock 或 cooked_lock 来访问
// poll_list。poll_table_cleanup 从 do_poll 外层（sleep 返回后）
// 调用，此时 fd 的 IRQ/其他 task 可能正在访问同一个 poll_list。
// 通过 entry.fd_lock 获取正确的锁，保证互斥。
static void poll_table_cleanup(poll_table_t *pt)
{
    for (int i = 0; i < pt->nent; i++) {
        poll_wait_entry_t *e = &pt->entries[i];
        if (!list_is_empty(&e->node) && e->fd_lock) {
            uint64_t flags = spin_lock_irqsave(e->fd_lock);
            if (!list_is_empty(&e->node))   // 双检：wake 路径可能已摘除
                list_del_init(&e->node);
            spin_unlock_irqrestore(e->fd_lock, flags);
        }
    }
    pt->nent = 0;
}
```

### 5.1 为什么双队列而非 ptr 标记位

| 方案 | 问题 |
|------|------|
| 指针低 1 位标记 (prev \|= WAIT_NODE_POLL) | 脆弱，每个访问 prev 的地方都要 mask |
| container_of + 可能性排除 | 在 task 和 poll_entry 间无法区分 |

**双队列**：task 节点挂在 `read_wait` (wait_queue_t)，poll entry 挂在 `read_poll` (list_t)。wake 时分别遍历，代码清晰，零歧义。每个 pipe 多 32 字节，完全可接受。

---

## 6. fd_poll() 实现

```c
// kernel/fs/poll.c

// 检查单个 fd 的就绪状态
// pt: 若非 NULL 且 fd 不 ready，调用 poll_wait 注册到 fd 的 poll 队列
// 返回: revents 掩码
static uint32_t fd_poll(file_t *f, poll_table_t *pt)
{
    if (!f) return POLLNVAL;

    switch (f->type) {

    case FD_VFS:
        // 普通 VFS 文件总是可读可写
        // （不支持 poll 的 fs 例如当前 ext2/fat32/tmpfs/procfs）
        if (f->flags == O_RDONLY || f->flags == O_RDWR)
            return POLLIN | POLLRDNORM;
        if (f->flags == O_WRONLY || f->flags == O_RDWR)
            return POLLOUT | POLLWRNORM;
        return 0;

    case FD_DEV:
        // /dev/ 设备 — 通过 devfs_poll() 封装转发。
        // devices[] 是 devfs.c 的 static 全局数组，poll.c 不应
        // 直接访问。devfs_poll() 内部做 idx → &devices[idx] 解析
        // 并调用 dev->poll(priv, pt)。
        if (!f->node) return POLLNVAL;
        return devfs_poll(f->node, pt);

    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return POLLERR;

        uint32_t mask = 0;
        uint64_t flags = spin_lock_irqsave(&p->lock);

        if (f->flags == O_RDONLY) {
            if (!pipe_empty(p))
                mask |= POLLIN;
            else if (p->writers == 0)
                mask |= POLLHUP;   // EOF: 所有写端已关闭
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->read_poll, &p->lock);
        }

        if (f->flags == O_WRONLY) {
            if (!pipe_full(p))
                mask |= POLLOUT;
            else if (p->readers == 0)
                mask |= POLLERR;   // 读端全关 — broken pipe
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->write_poll, &p->lock);
        }

        if (f->flags == O_RDWR) {
            // 读写两端都检查
            if (!pipe_empty(p))
                mask |= POLLIN;
            else if (p->writers == 0)
                mask |= POLLHUP;
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->read_poll, &p->lock);

            if (!pipe_full(p))
                mask |= POLLOUT;
            else if (p->readers == 0)
                mask |= POLLERR;
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->write_poll, &p->lock);
        }

        spin_unlock_irqrestore(&p->lock, flags);
        return mask;
    }

    default:
        return POLLNVAL;
    }
}
```

---

## 7. TTY / devfs poll 路径

### 7.1 完整调用链

```
fd_poll(f, &pt)
  → switch f->type == FD_DEV
  → dev = (devfs_device_t *)(uintptr_t)f->node->fs_data
  → dev->poll(dev->private_data, &pt)
    → tty_poll(tty, &pt)
      → 检查 cooked buffer
      → 无数据 → poll_wait(&pt, &tty->read_poll, &tty->cooked_lock)
```

关键点：
- `poll_wait_entry_t` 挂在 `tty->read_poll` 上，由 `tty->cooked_lock` 保护
- TTY 的 `read_wait` 保持不变 — 它只挂 `task_t.io_wait_node`
- Wake 路径分开处理

### 7.2 devfs_poll() 封装

`devices[]` 是 devfs.c 的 `static` 全局数组，poll.c 不能直接访问。提供封装函数：

```c
// kernel/fs/devfs.c — 新增

// 封装函数供 fd_poll(FD_DEV) 调用
// node->fs_data 存储整数索引，通过它找到 devfs_device_t
uint32_t devfs_poll(vfs_node_t *node, poll_table_t *pt)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return POLLNVAL;

    devfs_device_t *dev = &devices[idx];

    if (dev->poll)
        return dev->poll(dev->private_data, pt);

    // 无 poll 回调的设备：默认总是就绪
    return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
}
```

### 7.3 设备 poll 回调实现

`/dev/tty` 注册时 `private_data = NULL`（实际 `dev_tty_read`/`dev_tty_write` 通过 `get_dev_tty()` 获取全局 TTY），因此 poll 回调也需走相同路径：

```c
// kernel/fs/devfs.c

// /dev/tty 的 poll — 通过 get_dev_tty() 获取全局 TTY（与 dev_tty_read 一致）
static uint32_t dev_tty_poll(void *priv, poll_table_t *pt)
{
    (void)priv;
    tty_t *tty = get_dev_tty();
    if (!tty)
        return POLLERR;
    return tty_poll(tty, pt);
}

// /dev/keyboard 的 poll — 检查 scancode ring buffer
static uint32_t dev_keyboard_poll(void *priv, poll_table_t *pt)
{
    (void)priv; (void)pt;
    uint32_t mask = POLLOUT | POLLWRNORM;  // 总是可写（/dev/keyboard 只读）
    // scancode ring buffer 非空 → 可读
    if (!ring_empty())
        mask |= POLLIN | POLLRDNORM;
    // 注意：键盘无 IRQ→poll 级联唤醒（v1 限制），但 poll+timeout 可工作
    return mask;
}
```

### 7.4 tty_poll()

```c
// kernel/tty/tty.c

uint32_t tty_poll(tty_t *tty, poll_table_t *pt)
{
    uint32_t mask = 0;

    // TTY 总是可写
    mask |= POLLOUT | POLLWRNORM;

    // 检查 cooked ring buffer 是否有数据
    uint64_t flags = spin_lock_irqsave(&tty->cooked_lock);
    if (tty->head != tty->tail) {
        mask |= POLLIN | POLLRDNORM;
    } else if (pt && !pt->triggered) {
        // 无数据：注册 poll_entry 到 tty 的 poll 队列
        // cooked_lock 同时保护 cooked ring 和 read_poll 链表
        poll_wait(pt, &tty->read_poll, &tty->cooked_lock);
    }
    spin_unlock_irqrestore(&tty->cooked_lock, flags);

    return mask;
}
```

### 7.5 TTY wake 路径扩展

```c
// kernel/tty/tty.c — tty_wake_waiters() 扩展版

static void tty_wake_waiters(tty_t *tty)
{
    uint64_t flags = spin_lock_irqsave(&tty->cooked_lock);

    // 1. 唤醒直接阻塞的 reader task（tty_read 路径）
    while (!list_is_empty(&tty->read_wait)) {
        list_t *node = tty->read_wait.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }

    // 2. 级联唤醒 poll 等待者
    while (!list_is_empty(&tty->read_poll)) {
        list_t *node = tty->read_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);  // 唤醒 polling task
    }

    spin_unlock_irqrestore(&tty->cooked_lock, flags);
}
```

---

## 8. pipe wake 路径

### 8.1 pipe_wake_readers()

```c
// kernel/fs/file.c

static void pipe_wake_readers(pipe_t *p)
{
    // p->lock 已被调用方持有

    // 唤醒直接阻塞的 reader task（fd_read 路径）
    wait_queue_wake_one(&p->read_wait);

    // 级联唤醒所有 poll 等待者（fd_poll 路径）
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
```

### 8.2 Wake 触发点

| 触发条件 | 调用 |
|----------|------|
| `fd_write` 写完数据 | `pipe_wake_readers(p)` |
| `fd_read` 读完数据 | `pipe_wake_writers(p)` |
| `fd_close` 关闭写端 → `p->writers == 0` | `pipe_wake_readers(p)` → reader 收到 EOF, poll 收到 POLLHUP |
| `fd_close` 关闭读端 → `p->readers == 0` | `pipe_wake_writers(p)` → writer 收到 -EPIPE, poll 收到 POLLERR |

### 8.3 fd_close 修改

```c
// kernel/fs/file.c — fd_close()

void fd_close(files_t *fs, int fd)
{
    ...

    if (f->type == FD_PIPE && f->pipe) {
        uint64_t flags = spin_lock_irqsave(&f->pipe->lock);

        if (f->flags == O_RDONLY) {
            f->pipe->readers--;
            if (f->pipe->readers == 0)
                pipe_wake_writers(f->pipe);  // ← 新增：唤醒等待写端的 poller
        } else {
            f->pipe->writers--;
            if (f->pipe->writers == 0)
                pipe_wake_readers(f->pipe);  // ← 新增：唤醒等待读端的 poller
        }

        spin_unlock_irqrestore(&f->pipe->lock, flags);
    }

    ...
}
```

---

## 9. do_poll() 主循环

```c
// kernel/fs/poll.c

// Linux ABI: int poll(struct pollfd *fds, nfds_t nfds, int timeout)
// timeout: -1 = 无限, 0 = 非阻塞, >0 = 毫秒
//
// 返回: 就绪 fd 数量, 0 = 超时, <0 = -errno

int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout)
{
    // ── 用户指针合法性检查 ──────────────────────────
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;
    if (nfds == 0)
        return 0;
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;

    // ── Phase 1: 从用户态拷贝 pollfd 数组 ──────────
    struct pollfd kfds[POLL_MAX_FDS];
    for (uint32_t i = 0; i < nfds; i++) {
        kfds[i].fd      = user_fds[i].fd;
        kfds[i].events  = user_fds[i].events;
        kfds[i].revents = 0;
    }

    // ── 信号快速检查（进入阻塞前先检查） ──────────
    // POSIX: any unblocked signal 应中断 poll 返回 EINTR
    if (current->signal & ~current->blocked)
        return -EINTR;

    poll_table_t pt;
    poll_table_setup(&pt);

    int ready_count = 0;

    for (;;) {
        poll_table_init(&pt);  // reset nent=0, triggered=false

        // ── Phase 2: 扫描所有 fd ──────────────────────
        for (uint32_t i = 0; i < nfds; i++) {
            if (kfds[i].fd < 0) continue;

            file_t *f = current->files->fd[kfds[i].fd];
            if (!f) {
                kfds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }

            uint32_t revents = fd_poll(f, &pt);
            if (revents & kfds[i].events) {
                kfds[i].revents = revents & kfds[i].events;
                ready_count++;
                pt.triggered = true;  // 短路后续 poll_wait
            }
        }

        // ── Phase 3: 有就绪? 返回 ────────────────────
        if (ready_count > 0) {
            poll_table_cleanup(&pt);
            break;
        }

        // ── Phase 4: timeout == 0? 不等 ──────────────
        if (timeout == 0) {
            poll_table_cleanup(&pt);
            break;
        }

        // ── Phase 5: sleep 前信号检查 ──────────────────
        // 避免在已有 pending signal 的情况下不必要地睡眠
        if (current->signal & ~current->blocked) {
            poll_table_cleanup(&pt);
            return -EINTR;
        }

        // ── Phase 6: 设置超时定时器 ────────────────────
        // 使用一次性的 wait_queue 定时器
        // timeout == -1: 无超时，等待直到 fd 就绪或信号
        // timeout > 0:   定时器到期 → timer callback 调用
        //                 wait_queue_wake_all(&pt.wq)
        wait_queue_t timer_wq;
        bool timed_out = false;

        if (timeout > 0) {
            wait_queue_init(&timer_wq);
            // timer_start(timeout_ms, &timer_wq);
            // callback: wait_queue_wake_all(&timer_wq);
            // 实现：在 do_timer / PIT 回调中递减，到期时
            // 将 timer_wq 上所有 waiter 设为 RUNNING
        }

        // ── Phase 7: 阻塞等待 ──────────────────────────
        wait_queue_sleep(&pt.wq);

        // 被唤醒——清理旧 entry，回到 Phase 2 重扫
        poll_table_cleanup(&pt);

        // ── Phase 8: 超时检查 ────────────────────────
        // 检查是谁唤醒了我们
        if (timeout > 0) {
            // 如果是 timer callback 唤醒的 → timed_out = true
            // if (!list_is_empty(&timer_wq.head))
            //     wait_queue_wake_all(&pt.wq)  // ← 从 timer 边？
            //
            // 简化实现：检查 pt.wq.head 是否非空（timer 没摘除）
            // 更好的方式：在 pt 中加 timed_out 标记
        }
        if (timed_out) {
            return 0;
        }

        // ── Phase 9: 信号检查 ────────────────────────
        // POSIX: 任何非阻塞信号都应中断 poll()
        if (current->signal & ~current->blocked)
            return -EINTR;

        ready_count = 0;
    }

    // ── Phase 10: 回写 revents ──────────────────────
    for (uint32_t i = 0; i < nfds; i++)
        user_fds[i].revents = kfds[i].revents;

    return ready_count;
}
```

### 流程图

```
do_poll(fds, 3, -1):
│
├─[Phase 1] addr_limit 检查 + copy_from_user(fds)
├─[Phase 1b] 信号快速检查 → 有 pending? return -EINTR
│
└─► ┌──────────────────────────────────┐
    │  poll_table_init(&pt)            │  ← 重置 nent, triggered
    │                                  │
    │  for fd in fds:                  │
    │    revents = fd_poll(f, &pt)     │──→ TTY: tty_poll() → cooked buffer?
    │    if revents & events:          │   Pipe: pipe_empty? pipe_full?
    │      kfds[i].revents = revents   │   不 ready → poll_wait(pt, &p->read_poll, &p->lock)
    │      ready++, pt.triggered=true  │
    │                                  │
    │  ready > 0? ──YES──→ cleanup→return│
    │  timeout==0?──YES──→ cleanup→return 0│
    │                                  │
    │  pre-sleep: 信号？──YES→ -EINTR  │  ← 避免不必要睡眠
    │                                  │
    │  timeout>0: set timer(wq)        │
    │  wait_queue_sleep(&pt.wq)       │──→ block until:
    │                                  │    - pipe_write() → pipe_wake_readers
    │  [woken]                          │      → walk read_poll → wake_all(pt.wq)
    │  poll_table_cleanup(&pt)         │    - keyboard ISR → tty_wake_waiters
    │  timed_out? ──YES──→ return 0    │      → walk read_poll → wake_all(pt.wq)
    │  信号? ──YES──→ return -EINTR    │    - timer expired → wake_all(pt.wq)
    │                                  │
    │  ready_count=0, goto 循环入口     │
    └──────────────────────────────────┘
```

---

## 10. 信号语义

### 10.1 POSIX 要求

任何**未阻塞**的信号（不管是否有 handler）都应中断 `poll()` 返回 `-EINTR`。这与 `signal_pending_fatal()`（只检查致命信号）不同。

```c
// 正确的检查: 任何 pending 的非阻塞信号都应中断
if (current->signal & ~current->blocked)
    return -EINTR;
```

`signal_pending_fatal()` 用于 TTY 读路径是因为 TTY read 需要在信号处理后才返回。poll 不需要这个语义——poll 被信号中断后，由用户态重试。

### 10.2 三个检查点

1. **进入 do_poll 时**：快速返回，避免不必要的 poll_table 构建
2. **sleep 前**：避免在已有 pending signal 时进入睡眠
3. **sleep 后**：检查是否被信号唤醒

---

## 11. 超时实现 (具体方案)

### 11.1 机制

```c
// do_poll 中:

int64_t deadline_ticks = 0;
if (timeout > 0) {
    // 将 ms 转换为 PIT tick 计数（假设 PIT 100Hz → 10ms/tick）
    int ticks = (timeout + 9) / 10;   // 向上取整
    if (ticks < 1) ticks = 1;

    // 全局变量：poll 上下文（简化实现，单 CPU 下安全）
    deadline_ticks = g_sched_ticks + ticks;
    current_poll_wq = &pt.wq;  // timer 回调需要唤醒此 wq
}
```

### 11.2 Timer 回调 (PIT do_timer 中)

```c
// kernel/timer/timer.c — do_timer() 中:
if (current_poll_wq && g_sched_ticks >= poll_deadline) {
    wait_queue_wake_all(current_poll_wq);
    current_poll_wq = NULL;
}
```

### 11.3 do_poll 中检测超时

```c
// wait_queue_sleep 返回后:
poll_table_cleanup(&pt);

if (timeout > 0 && g_sched_ticks >= deadline_ticks) {
    current_poll_wq = NULL;
    return 0;
}
```

**简化版 (v1 可接受)**: 不做精确超时，`wait_queue_sleep(&pt.wq)` 之后如果 `timeout > 0` 且 `ready_count == 0`（无 fd 就绪），逐次递减 ticks，直至 0 返回。

**精确版 (推荐)**: 使用 `wait_queue_t timer_wq`，timer callback 直接 wake pt.wq。do_poll 通过 deadline 检测是否是 timer 唤醒。

---

## 12. syscall 注册

### 12.1 新增 syscall 编号

```c
// libc/include/sys/syscall.h — 在 SYS_futex(47) 之后追加:
#define SYS_poll     48
#define SYS_ppoll    49   // v1 不实现
#define SYS_select   50   // v1 用 select→poll 适配层
```

### 12.2 do_system_call 分发

```c
// kernel/arch/x86_64/trap.c: do_system_call()
case SYS_poll:
    ret = do_poll((struct pollfd *)regs->rdi, regs->rsi, (int)regs->rdx);
    break;
```

### 12.3 syscall_names 数组扩展

```c
// kernel/arch/x86_64/trap.c — syscall_names[] 从 [64] 扩展到至少 [51]
static const char *syscall_names[64] = {
    // ... 现有 0..47
    [48] = "poll",
    [49] = "ppoll",
    [50] = "select",
};
```

### 12.4 libc 包装

```c
// libc/unistd/poll.c (新文件，替代 busybox_stubs.c 中的假实现)
#include <poll.h>
#include <errno.h>
#include <sys/syscall.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    int64_t ret = syscall(SYS_poll, (uint64_t)fds, (uint64_t)nfds, (uint64_t)timeout);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}
```

### 12.5 busybox_stubs.c 清理

删除 `busybox_stubs.c:42-80` 的假 poll() 实现。`libc/unistd/poll.c` 作为 libc 的正式 poll 实现。

---

## 13. 实施计划

### Phase 1: pipe wait queue (前置 — 独立可测试)

| # | 任务 | 文件 | 工作量 |
|---|------|------|--------|
| 1.1 | `pipe_t` 添加 `read_wait`/`write_wait`/`read_poll`/`write_poll` | `kernel/include/kernel/file.h` | 10 min |
| 1.2 | `pipe_alloc()` 初始化 4 个新字段 | `kernel/fs/file.c` | 10 min |
| 1.3 | `fd_read` pipe 路径: 用 `wait_queue_sleep` 替代 `schedule()` | `kernel/fs/file.c` | 30 min |
| 1.4 | `fd_write` pipe 路径: 用 `wait_queue_sleep` 替代 `schedule()` | `kernel/fs/file.c` | 30 min |
| 1.5 | `pipe_wake_readers()` + `pipe_wake_writers()` 实现 | `kernel/fs/file.c` | 20 min |
| 1.6 | `fd_write` 完成时调用 `pipe_wake_readers()` | `kernel/fs/file.c` | 10 min |
| 1.7 | `fd_read` 完成时调用 `pipe_wake_writers()` | `kernel/fs/file.c` | 10 min |
| 1.8 | `fd_close` 唤醒 poll waiter (writers→0 → pipe_wake_readers; readers→0 → pipe_wake_writers) | `kernel/fs/file.c` | 15 min |
| 1.9 | 同步 `test/include/kernel/file.h` | `test/include/` | 10 min |
| **小计** | | | **~半日** |

### Phase 2: poll 核心

| # | 任务 | 文件 | 工作量 |
|---|------|------|--------|
| 2.1 | 新增 `poll_table_t` + `poll_wait_entry_t` 定义 (含 `fd_lock` 字段) | `kernel/include/kernel/poll.h` | 20 min |
| 2.2 | `poll_wait()` / `poll_table_init()` / `poll_table_setup()` / `poll_table_cleanup()` | `kernel/fs/poll.c` (新) | 45 min |
| 2.3 | `fd_poll()` — FD_VFS / FD_DEV (devfs→poll) / FD_PIPE 完整分发 | `kernel/fs/poll.c` | 45 min |
| 2.4 | `do_poll()` 主循环 (含 addr_limit + 3 次信号检查 + 超时) | `kernel/fs/poll.c` | 90 min |
| 2.5 | `devfs_device_t` 添加 `poll` 回调 + `dev_tty_poll()` + `dev_keyboard_poll()` | `kernel/include/fs/devfs.h` + `kernel/fs/devfs.c` | 20 min |
| 2.5b | `devfs_register_chrdev()` 签名增加 poll 参数，更新所有调用点 | `kernel/include/fs/devfs.h` + `kernel/fs/devfs.c` + `kernel/kernel/main.c` (5 处调用) | 20 min |
| 2.6 | `tty_poll()` + `tty_wake_waiters()` 扩展 (读/写双队列: read_wait + read_poll) | `kernel/tty/tty.c` | 45 min |
| 2.7 | devfs_init() 中为 tty/keyboard 注册 poll 回调 | `kernel/fs/devfs.c` | 10 min |
| 2.8 | main.c 中 `devfs_register_chrdev` 调用点更新 (keyboard: 传 NULL poll; fb: 传 NULL poll) | `kernel/kernel/main.c` | 10 min |
| 2.9 | syscall 编号 + `do_system_call` 分发 + 扩展 `syscall_names[64]` | `libc/include/sys/syscall.h` + `kernel/arch/x86_64/trap.c` | 20 min |
| 2.10 | 同步 `test/include/` (poll.h, file.h, devfs.h) | `test/include/` | 15 min |
| **小计** | | | **~1 日** |

### Phase 3: 测试 + 集成

| # | 任务 | 文件 | 工作量 |
|---|------|------|--------|
| 3.1 | libc `poll()` 新文件 + 删除 `busybox_stubs.c` 假实现 | `libc/unistd/poll.c` (新) + `libc/unistd/busybox_stubs.c` | 20 min |
| 3.2 | systest 新增 poll 用例 (pipe poll, TTY poll, timeout, 多 fd) | `user/systest.c` | 30 min |
| 3.3 | `select()` 包装 (fd_set→pollfd→do_poll→revents→fd_set) | `kernel/fs/poll.c` | 60 min |
| 3.4 | `ppoll`/`pselect6` 编号注册 (v1 不实现，保留) | `libc/include/sys/syscall.h` | 10 min |
| 3.5 | busybox ash 验证 (safe_poll 路径) | 启动测试 | 30 min |
| **小计** | | | **~半日** |

### 总计: ~2 日

---

## 14. 风险与注意事项

| 风险 | 缓解 |
|------|------|
| **pipe 行为改变** — `fd_read`/`fd_write` 改为阻塞后，旧代码可能依赖 busy-loop 行为 | systest 已有 pipe+fork+dup2 测试；`pipe_wake` 确保不会永久阻塞 |
| **TTY 双队列并发** — `cooked_lock` 同时保护 `read_wait` 和 `read_poll` | `tty_wake_waiters()` 和 `tty_poll()` 都取 `cooked_lock` |
| **poll_table_cleanup 与 fd wake 并发** — 双方都在操作 poll_list | `poll_wait_entry.fd_lock` 确保互斥摘除 |
| **test/include/ 镜像** | 每改 `kernel/include/` 头文件时立即同步 `test/include/` |
| **nfds_t 是 8 字节 (LP64)** | 内核 `do_poll()` 参数用 `uint64_t` 接收, 内部截断后检查 `> POLL_MAX_FDS` |
| **select vs poll 的 ABI 差异** | 先只实现 poll，select 作为 `fd_set → pollfd[]` 转换适配层 |
| **网络栈集成** | `fd_poll()` 加 `FD_SOCKET` case 即可，不需要改架构 |
| **POLLERR vs POLLHUP** | 写端全关 → pipe 读端用 POLLHUP (EOF)，读端全关 → pipe 写端用 POLLERR (EPIPE) |

---

## 15. 扩展路径

### 15.1 select() 实现

`do_select()` 把 `fd_set *` 转换为 `pollfd[]`，调用 `do_poll()`，再把 `revents` 翻译回 `fd_set`：

```c
int64_t do_select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout)
{
    struct pollfd pfds[FD_SETSIZE];
    int count = 0;

    for (int fd = 0; fd < nfds; fd++) {
        short events = 0;
        if (readfds   && FD_ISSET(fd, readfds))   events |= POLLIN;
        if (writefds  && FD_ISSET(fd, writefds))  events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
        if (events) {
            pfds[count].fd = fd;
            pfds[count].events = events;
            pfds[count].revents = 0;
            count++;
        }
    }

    int timeout_ms = (timeout)
        ? (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000)
        : -1;
    int ready = (int)do_poll(pfds, count, timeout_ms);
    if (ready < 0) return ready;

    // 回写 fd_set
    int total = 0;
    if (readfds)   FD_ZERO(readfds);
    if (writefds)  FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);

    for (int i = 0; i < count; i++) {
        if (pfds[i].revents & POLLIN)  { FD_SET(pfds[i].fd, readfds);   total++; }
        if (pfds[i].revents & POLLOUT) { FD_SET(pfds[i].fd, writefds);  total++; }
        if (pfds[i].revents & POLLPRI) { FD_SET(pfds[i].fd, exceptfds); total++; }
    }
    return total;
}
```

### 15.2 网络栈 socket poll

当 lwIP 集成时，`fd_poll()` 加一个 case：

```c
case FD_SOCKET:
    return socket_poll(f->socket, pt);
```

其中 `socket_poll()` 检查 lwIP 内部 recv/send 缓冲区，必要时注册到 lwIP 的 wait queue。

---

## 16. 设计总结

```
                    ┌──────────────────────────────────────┐
                    │          libc poll(fds,n,to)         │
                    │          syscall(SYS_poll,...)        │
                    └─────────────────┬────────────────────┘
                                      │ int $0x80
                    ┌─────────────────▼────────────────────┐
                    │  do_system_call()                     │
                    │  addr_limit check + arg unpack         │
                    │  do_poll(user_fds, n, timeout)        │
                    └─────────────────┬────────────────────┘
                                      │
                    ┌─────────────────▼────────────────────┐
                    │  do_poll():                           │
                    │  1. copy_from_user(fds)               │
                    │  2. signal pre-check → -EINTR          │
                    │  3. [loop]                             │
                    │     a. poll_table_init(&pt)            │
                    │     b. for each fd: fd_poll(f, &pt)   │
                    │     c. if ready → cleanup → return    │
                    │     d. if timeout==0 → return 0        │
                    │     e. signal pre-sleep → -EINTR       │
                    │     f. if timeout>0: set timer         │
                    │     g. wait_queue_sleep(&pt.wq)        │
                    │     h. cleanup → signal? timer?        │
                    │     i. goto loop                        │
                    │  4. copy_to_user(revents)              │
                    └────┬──────────┬──────────┬───────────┘
                         │          │          │
              ┌──────────▼──┐ ┌─────▼──────┐ ┌▼───────────┐
              │ fd_poll     │ │ fd_poll    │ │ fd_poll     │
              │ FD_DEV:     │ │ FD_PIPE:   │ │ FD_VFS:     │
              │ dev→poll()  │ │ pipe_empty │ │ always      │
              │  →tty_poll  │ │ →poll_wait │ │ POLLIN|OUT  │
              │  →poll_wait │ │  (&rpoll)  │ │             │
              └─────────────┘ └────┬───────┘ └─────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │ pipe_wake_readers(p)        │
                    │  wake_one(&p->read_wait)    │ ← task (read() blocked)
                    │  for e in read_poll:        │ ← poll entry
                    │    wake_all(e->poll_wq)     │    级联唤醒
                    │    list_del_init(&e->node)  │
                    └─────────────────────────────┘
```

**核心思想**:
1. 每个 fd 类型有两套队列 — `*_wait` (wait_queue_t, 挂 task) + `*_poll` (list_t, 挂 poll_entry)
2. poll 调用时通过 `poll_wait()` 注册 `poll_wait_entry` 到 fd 的 poll 队列
3. 任一 fd 就绪 → fd 的 wake 路径遍历 poll 队列 → 级联 `wake_all(pt.wq)` → 唤醒 polling task
4. polling task 被唤醒后重扫所有 fd → 返回所有就绪的 revents
5. `poll_wait_entry.fd_lock` 保证 cleanup 和 wake 路径互斥

**最小改动集**:
1. `pipe_t` + 4 个 list/wq 字段
2. `tty_t` + 1 个 `read_poll` 字段
3. `devfs_device_t` + 1 个 poll 回调
4. 新文件 `kernel/fs/poll.c` (~250 lines)
5. 新文件 `kernel/include/kernel/poll.h` (~35 lines)
6. `fd_read`/`fd_write`/`fd_close` pipe 路径重构 (~40 lines changed)
7. `tty_wake_waiters` 扩展 (~15 lines changed)
8. syscall 表 + 3 个新编号 + `syscall_names[]` 扩展
