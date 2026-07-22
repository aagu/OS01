# select/pselect 系统调用 — 设计文档

> **日期**: 2026-07-23
> **状态**: v4
> **依赖**: `do_poll()` + `poll_table` 基础设施 (kernel/fs/poll.c)

---

## 目标

在现有的 poll 基础设施上实现 `select(2)` 和 `pselect6(2)`，作为 `fd_set → pollfd → do_poll_core → revents → fd_set` 的适配层。

---

## 评审变更摘要

### v3 → v4

| # | 问题 | 解决方案 |
|---|------|----------|
| 🔴 1 | `fd_set *` 在内核未定义，API 签名无法编译 | 内核签名改为 `void *`（匹配 `trap.c` 现有模式），内部 memcpy 到 `kernel_fd_set` |
| 🔴 2 | `tv_sec < 0` 在 `uint64_t` 上是死代码 | 改为溢出保护：`tv_sec > 100000000`（~3 年）拒绝，删除无意义的 `< 0` 检查 |
| 🔴 3 | `nfds==0+timeout==NULL` 永久挂起 | 返回 `-ENOSYS`（`wait_queue_sleep` 无信号唤醒，`blocker.type==BLOCKER_NONE`） |
| 🟡 4 | `memcpy(kfds, NULL, 0)` 未定义行为 | 添加 `if (nfds > 0)` 守卫 |
| 🟡 5 | `kmalloc(0)` 实现定义行为 | nfds==0 路径显式传 `max_entries=1` |
| 🟡 6 | 128 字节 fd_set 跨页未保护 | 记录为已有假设（内核无交换，与现有 `sys_write` buf 等模式一致） |
| 🟡 8 | `do_poll_core` 入口前信号检查归属不清 | 调用方（`do_poll`/`do_select`）在 `poll_table_setup` 之前负责首次信号检查 |
| 🟢 9 | `poll_wait` 中 `POLL_MAX_FDS→max_entries` 未显式 | 在 poll.h 重构章节显式标注 |
| 🟢 11 | `kernel_fd_set.__bits` 与 `fd_set.__fds_bits` 字段名不一致 | memcpy 逐字节，128 字节相同布局，标注确认 |
| 🟢 12 | `uint64_t` vs `long` 非可移植性 | 标注 x86_64 LP64 假设 |
| 🟢 13 | 写回 addr_limit 校验不对称 | 入口校验一次（整个 syscall 期间不变），文档标注 |

### v2 → v3

| 问题 | 解决方案 |
|------|----------|
| `copy_from_user`/`copy_to_user` 不存在 | 改为内核实际模式：`addr_limit` 校验 + `memcpy` 直接解引用 |
| `fd_set` 未在内核头文件中定义 | 新增 `kernel_fd_set` 类型，定义在 `select.h` |
| `do_select_core` 残留引用 | 统一为 `do_select()` |
| `poll_table_setup` kmalloc 失败无路径 | 返回 `int`（0/-ENOMEM），调用方检查 |
| Makefile 变更不需要（wildcard 自动发现） | 从清单中移除 |
| `do_poll_core` 信号契约不明确 | 添加明确的 3 个检查时间点 |
| pselect6 结构体重复定义 | 两边各自定义（内核在 `kernel/select.h`，libc 在 `select.c` 内联）；libc 无法引用 kernel include 路径下的头文件 |

### v1 → v2

| 问题 | 解决方案 |
|------|----------|
| `POLL_MAX_FDS=16` — select 需 1024 | poll_table_t 动态 entries + do_poll_core 提取 |
| 用户态指针直访 | addr_limit + memcpy → 内核栈，返回时写回 |
| `nfds==0` 丢失 timeout | 休眠到超时（timeout>0） |
| pselect6 打包 | Linux 兼容 `{sigset_t*, sigsetsize}` 结构体 |
| 超时 ceil | `(tv_usec + 999) / 1000` |
| NULL fd_set 语义 | NULL = 跳过（POSIX），非 EFAULT |
| sigmask 原子性限制 | 记录 signal wake 不通过 blocker 集成 |
| SMP 全局变量 | 标注为已有限制 |

---

## 内核用户内存访问模式

本内核不使用 `copy_from_user`/`copy_to_user`。用户内存通过 `addr_limit` 校验 + 直接 `memcpy` 访问（ring 0 下通过当前任务 CR3 可访问用户页）：

```c
// 读用户内存（已有模式，参见 trap.c write/read/exec 等）
if ((uint64_t)user_ptr >= current->addr_limit)
    return -EFAULT;
memcpy(&kernel_copy, user_ptr, size);

// 写用户内存
if ((uint64_t)user_ptr >= current->addr_limit)
    return -EFAULT;
memcpy(user_ptr, &kernel_copy, size);
```

**跨页假设**：内核当前不实现分页交换（demand paging 仅用于 ZONE_NORMAL 的 4KB 匿名页）。用户页一旦分配 / 从文件读取映射后，在进程生命周期内始终存在。因此 `fd_set`（128 字节）即使跨越页边界，`memcpy` 也能安全访问（两边页都已映射）。这与现有代码（`trap.c:SYS_write` → `pipe_write` → 任意长度 `memcpy`）的假设一致。所有 `addr_limit` 校验在 syscall 入口进行一次（`addr_limit` 和 CR3 在 syscall 期间不变）。

---

## 架构

### 核心重构：poll_table_t 动态化 + do_poll_core 提取

当前 `poll_table_t` 使用固定大小栈数组 `poll_wait_entry_t entries[POLL_MAX_FDS]`（16 个条目）。要支持 select 的 FD_SETSIZE=1024，需要动态分配。

**变更：`poll_table_t` 内部 entries 改为指针**

```c
// kernel/include/kernel/poll.h

typedef struct poll_table {
    wait_queue_t        wq;
    poll_wait_entry_t  *entries;      // kmalloc'd（曾为 entries[POLL_MAX_FDS]）
    int                 max_entries;   // 容量
    int                 nent;          // 活跃条目数
    bool                triggered;     // 快捷退出：已有 fd 就绪
} poll_table_t;
```

- `int poll_table_setup(pt, max_entries)` — `kmalloc` entries，初始化节点，返回 0 或 `-ENOMEM`
- `poll_table_init(pt)` — 重置 nent/triggered（不变）
- `poll_wait(pt, poll_list, fd_lock)` — `if (pt->nent >= pt->max_entries) return;`（`POLL_MAX_FDS` 被 `pt->max_entries` 替代）
- `poll_table_cleanup(pt)` — 不变（遍历 `pt->nent`）
- `void poll_table_destroy(pt)` — `kfree(pt->entries)`（新增）

调用方模式：
```c
poll_table_t pt;
if (poll_table_setup(&pt, max_entries) != 0)
    return -ENOMEM;
// ... 使用 ...
poll_table_destroy(&pt);
```

### 数据流

```
              libc
              ┌───────────────────────────────────────────┐
              │ select(nfds, r, w, e, tv)                 │
              │ pselect(nfds, r, w, e, ts, sigmask)       │
              │   → 打包 pselect6_sigmask 结构体            │
              │   → syscall6(SYS_select/pselect6, ...)     │
              └───────────────────────────────────────────┘
                       │ SYS_select(5 regs) / SYS_pselect6(6 regs)
                       ▼
              kernel/fs/select.c
              ┌──────────────────────────────────────────┐
              │ do_select(nfds, r/w/e (void*), tv)       │
              │ (1) nfds<0 → -EINVAL                      │
              │ (2) 首次信号检查 (pre-poll_table_setup)    │
              │ (3) addr_limit+memcpy: 3×fd_set→kern copy │
              │ (4) addr_limit+memcpy: timeval→kern copy  │
              │ (5) timeval→ms (溢出保护)                  │
              │ (6) fd_sets → pollfd[nfds] (kmalloc)     │
              │ (7) poll_table_setup(&pt, max(nfds,1))    │
              │ (8) do_poll_core(pollfd, nfds, ms, &pt)  │
              │ (9) revents → 3×kernel_fd_set              │
              │ (10) addr_limit+memcpy 写回 3×fd_set      │
              │ (11) kfree(pollfd); poll_table_destroy    │
              │                                          │
              │ do_pselect6(...sigmask_packed (void*))    │
              │   同 do_select +                          │
              │   (3a) 解包 pselect6_sigmask 结构体        │
              │   (3b) save old_blocked, swap *sigmask    │
              │   (10a) restore old_blocked               │
              └──────────────────────────────────────────┘
                       │
                       ▼
              kernel/fs/poll.c
              ┌──────────────────────────────────────────┐
              │ do_poll_core(kfds, nfds, timeout, pt)    │
              │   (共享循环: 扫描+睡眠+信号检查, 不碰用户内存)│
              │                                          │
              │ do_poll(user_fds, nfds, timeout)         │
              │   → memcpy → do_poll_core → memcpy       │
              └──────────────────────────────────────────┘
```

### 关键提取：do_poll_core

```c
// kernel/fs/poll.c — 内部函数，供 do_poll() 和 do_select() 共享
//
// 前置条件：
//   - kfds[0..nfds-1] 已填充 {fd, events, revents=0}
//      如果 nfds==0，kfds 可以为 NULL（循环体不执行）
//   - pt 已通过 poll_table_setup() 成功初始化
//   - 调用者负责：用户内存复制、fd_set 转换、sigmask 交换、pt 释放
//   - 调用者在 poll_table_setup 之前负责首次信号检查
//     (current->signal & ~current->blocked) → -EINTR
//
// 信号契约（与 pselect sigmask 原子性相关）：
//   do_poll_core 在以下时机检查 current->signal & ~current->blocked：
//     1. 每次扫描前（循环开始处，等同于 sleep 后）
//     2. 调用 wait_queue_sleep 前（睡眠前）
//     3. 从 wait_queue_sleep 返回后（唤醒后 → 回到时机 1）
//   call site 已负责 poll_table_setup 之前的首次检查。
//   这确保 pselect 的 sigmask 交换后，所有信号检查均使用新的 blocked mask。
//
// 返回：就绪 fd 数量，0=超时，负值=-errno
//   对 nfds==0：返回 0（超时）或 -EINTR（信号）

int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int timeout_val, poll_table_t *pt);
```

`do_poll()` 重构为包装器：
```c
int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout_val) {
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;

    // nfds==0 + timeout>=0: 同 select — 超时休眠或 -ENOSYS（无限）
    // 保持与原 do_poll 兼容：nfds==0 且 timeout<=0 立即返回 0
    if (nfds == 0 && timeout_val <= 0)
        return 0;
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;

    // 首次信号检查（在 poll_table_setup 之前）
    if (current->signal & ~current->blocked)
        return -EINTR;

    struct pollfd kfds[POLL_MAX_FDS];
    if (nfds > 0) {
        memcpy(kfds, user_fds, nfds * sizeof(struct pollfd));
        for (uint32_t i = 0; i < nfds; i++)
            kfds[i].revents = 0;
    }

    poll_table_t pt;
    if (poll_table_setup(&pt, (nfds == 0) ? 1 : nfds) != 0)
        return -ENOMEM;
    int64_t ret = do_poll_core(kfds, nfds, timeout_val, &pt);
    poll_table_destroy(&pt);

    if (ret >= 0 && nfds > 0) {
        for (uint32_t i = 0; i < nfds; i++)
            user_fds[i].revents = kfds[i].revents;
    }
    return ret;
}
```

**注意**：即使重构后，`do_poll()` 的 `POLL_MAX_FDS=16` 限制**保持不变**。select 路径使用自己的动态分配，大小由 call site 决定。

---

## 类型定义

### 内核 kernel_fd_set（新）

`fd_set` 定义在 `libc/include/sys/types.h`（`long __fds_bits[16]`），内核中不存在。**内核 API 签名使用 `void *`**（匹配 `trap.c` 现有模式），内部通过 `memcpy` 转为：

```c
// kernel/include/kernel/select.h

#include <stdint.h>

#define FD_SETSIZE 1024

// signal mask type (unsigned long, matches libc/include/signal.h:47)
typedef unsigned long sigset_t;

typedef struct {
    uint64_t __bits[16];   // 1024 bits = FD_SETSIZE
} kernel_fd_set;

#define FD_SETSIZE 1024

// 内核原型：用户空间指针使用 void *（匹配 trap.c 模式）
int64_t do_select(int nfds, void *readfds, void *writefds,
                  void *exceptfds, void *timeout_tv);

int64_t do_pselect6(int nfds, void *readfds, void *writefds,
                    void *exceptfds, void *timeout_ts,
                    const void *sigmask_packed);

// 内核侧 FD 位操作
static inline void kern_fd_zero(kernel_fd_set *set) {
    memset(set, 0, sizeof(*set));
}
static inline void kern_fd_set(int fd, kernel_fd_set *set) {
    if ((uint32_t)fd < FD_SETSIZE)
        set->__bits[fd / 64] |= (1ULL << (fd % 64));
}
static inline void kern_fd_clr(int fd, kernel_fd_set *set) {
    if ((uint32_t)fd < FD_SETSIZE)
        set->__bits[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline int kern_fd_isset(int fd, kernel_fd_set *set) {
    return (uint32_t)fd < FD_SETSIZE
        && !!(set->__bits[fd / 64] & (1ULL << (fd % 64)));
}
```

**布局一致性**：libc `fd_set.long __fds_bits[16]` 与内核 `kernel_fd_set.uint64_t __bits[16]` 均为 16×8=128 字节，在 x86_64 LP64 下位序一致。`memcpy` 逐字节拷贝不依赖字段名。非可移植到 ILP32（`sizeof(long)=4`），但本项目仅支持 x86_64。

### pselect6 打包结构体（各自定义）

libc 无法 `#include` kernel include 路径下的头文件（libc Makefile 的 `-I.` 仅指向 `libc/include/`）。`pselect6_sigmask` 结构体在两端各自定义，保证 ABI 布局一致即可。

**内核端**（在 `kernel/include/kernel/select.h`）：
```c
struct pselect6_sigmask {
    const void *ss;       // const sigset_t *（void* 避免依赖 signal.h）
    size_t      ss_len;   // 必须 == sizeof(sigset_t) (8 字节)
};
```

**libc 端**（在 `libc/unistd/select.c` 内联定义，该文件唯一使用者）：
```c
// libc/unistd/select.c
struct pselect6_sigmask {     // 与内核端布局一致
    const sigset_t *ss;       // libc 可用 sigset_t（来自 <signal.h>）
    size_t          ss_len;
};
```

LP64 下两端布局相同：指针 8 字节 + size_t 8 字节 = 16 字节，无 padding。AArch64 同理。

### libc fd_set（已存在）

```c
// libc/include/sys/types.h:27-28
typedef struct {
    long __fds_bits[16];   // 1024 bits = FD_SETSIZE
} fd_set;
```

### timeval / timespec（已存在）

```c
// kernel/include/uapi/time.h
struct timeval  { uint64_t tv_sec; uint64_t tv_usec; };
struct timespec { uint64_t tv_sec; uint64_t tv_nsec; };
```

---

## 用户内存安全实现

```c
// kernel/fs/select.c — do_select()

// (1) 首次信号检查（在 poll_table_setup 之前）
if (current->signal & ~current->blocked)
    return -EINTR;

// (2) 验证 + 复制 fd_set → kernel_fd_set
kernel_fd_set kr, kw, ke;
kernel_fd_set *kr_ptr = NULL, *kw_ptr = NULL, *ke_ptr = NULL;

if (readfds) {
    if ((uint64_t)readfds >= current->addr_limit) return -EFAULT;
    memcpy(&kr, readfds, sizeof(kernel_fd_set));     // 128 字节（跨页安全，见假设）
    kr_ptr = &kr;
}
if (writefds) {
    if ((uint64_t)writefds >= current->addr_limit) return -EFAULT;
    memcpy(&kw, writefds, sizeof(kernel_fd_set));
    kw_ptr = &kw;
}
if (exceptfds) {
    if ((uint64_t)exceptfds >= current->addr_limit) return -EFAULT;
    memcpy(&ke, exceptfds, sizeof(kernel_fd_set));
    ke_ptr = &ke;
}

// (3) 验证 + 复制 timeout
struct timeval ktv;
struct timeval *ktv_ptr = NULL;
if (timeout_tv) {
    if ((uint64_t)timeout_tv >= current->addr_limit) return -EFAULT;
    memcpy(&ktv, timeout_tv, sizeof(ktv));
    // tv_sec 是 uint64_t（无符号）, 不含 < 0 检查
    // 溢出保护：约 3 年上限
    if (ktv.tv_sec > 100000000) return -EINVAL;
    if (ktv.tv_usec >= 1000000) return -EINVAL;
    ktv_ptr = &ktv;
}

// (4) 返回：将 kernel_fd_set 写回用户 fd_set
// addr_limit 在 syscall 期间不变，无需重复校验
if (readfds)
    memcpy(readfds, kr_ptr, sizeof(kernel_fd_set));
if (writefds)
    memcpy(writefds, kw_ptr, sizeof(kernel_fd_set));
if (exceptfds)
    memcpy(exceptfds, ke_ptr, sizeof(kernel_fd_set));
```

---

## 参数验证

| 条件 | 返回值 | 说明 |
|------|--------|------|
| `nfds < 0` | `-EINVAL` | 在 `uint64_t` 转换**之前**检查 |
| `nfds == 0` + timeout == NULL | `-ENOSYS` | 不支持（wait_queue_sleep 无信号唤醒） |
| `nfds == 0` + timeout == {0,0} | 0 | 非阻塞，立即返回 |
| `nfds == 0` + timeout > 0 | 0（超时后） | 休眠到超时，工作正常 |
| `nfds > FD_SETSIZE (1024)` | `-EINVAL` | |
| `timeout_tv->tv_sec > 100000000` | `-EINVAL` | 溢出保护（防止 `tv_sec*1000` 溢出） |
| `timeout_tv->tv_usec >= 1000000` | `-EINVAL` | |
| `timeout_ts->tv_sec > 100000000` | `-EINVAL` | 同上 |
| `timeout_ts->tv_nsec >= 1000000000` | `-EINVAL` | |
| `pselect6: ss_len != sizeof(sigset_t)` | `-EINVAL` | ABI 安全检查 |
| 用户指针 ≥ `addr_limit` | `-EFAULT` | |
| `kmalloc` 失败 | `-ENOMEM` | |
| 被信号中断 | `-EINTR` | fd_set 不改动 |

### nfds==0 处理

| timeout | 行为 | POSIX 兼容？ |
|---------|------|------------|
| `{0,0}` | 返回 0 | ✓ 非阻塞轮询 |
| `{t>0, _}` | 休眠到超时，返回 0 | ✓ 可移植亚秒级 sleep |
| NULL | 返回 `-ENOSYS` | ✗ POSIX 要求阻塞直到信号 — 当前无法实现 |

`select(0, NULL, NULL, NULL, NULL)` 的 POSIX 语义是"阻塞直到信号"。本内核 `wait_queue_sleep` 不通过 `blocker` 框架（`blocker.type == BLOCKER_NONE`），`sched_unblock_blocked()` 不会唤醒它。因此返回 `-ENOSYS`。

---

## 超时转换

```c
// select:  timeval → ms（tv_sec 溢出已由验证保护）
ms = (int)(tv_sec * 1000 + (tv_usec + 999) / 1000);

// pselect: timespec → ms
ms = (int)(tv_sec * 1000 + (tv_nsec + 999999) / 1000000);

// NULL timeout → -1（无限等待）
```

**精度**：`do_poll_core` 将 ms 转为 PIT ticks（`(ms + 9) / 10`，100Hz=10ms/tick）。`tv_usec=1` → 1ms → PIT 舍入 ~10ms（±10ms 误差）。与 Realtek/Linux 10ms 时钟粒度行为一致。

---

## 事件映射

### fd_set → pollfd.events

| fd_set | poll 事件 |
|--------|----------|
| readfds | `POLLIN \| POLLRDNORM` |
| writefds | `POLLOUT \| POLLWRNORM` |
| exceptfds | `POLLPRI` |

events==0 的 fd 跳过。`fd_poll` 对已关闭的 fd 返回 `POLLNVAL`。

### pollfd.revents → fd_set 反向映射

| revents 中置位 | 填入 fd_set |
|---|---|
| `POLLIN\|POLLRDNORM\|POLLHUP\|POLLERR` | readfds |
| `POLLOUT\|POLLWRNORM\|POLLERR` | writefds |
| `POLLPRI\|POLLERR` | exceptfds |

POLLHUP 出现在 readfds（EOF-readable），POLLERR 出现在所有三个集合。POLLNVAL 计入返回值但不设置任何 fd_set 位 — select 调用者无法通过位掩码得知哪个 fd 无效，只能看到返回计数增加。

---

## pselect6：syscall 打包

### libc 包装器

```c
// libc/unistd/select.c

struct pselect6_sigmask {          // 内联定义，与内核 select.h 中布局一致
    const sigset_t *ss;
    size_t          ss_len;
};

int pselect(int nfds, fd_set *r, fd_set *w, fd_set *e,
            const struct timespec *ts, const sigset_t *sigmask)
{
    struct pselect6_sigmask packed;
    packed.ss     = sigmask;                   // NULL = 不修改 mask
    packed.ss_len = sizeof(sigset_t);          // 8 字节
    int64_t ret = syscall6(SYS_pselect6,
                           (uint64_t)(int64_t)nfds,
                           (uint64_t)r, (uint64_t)w, (uint64_t)e,
                           (uint64_t)ts, (uint64_t)&packed);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

### 内核解包

```c
// kernel/fs/select.c — do_pselect6()
// struct pselect6_sigmask 定义在 kernel/include/kernel/select.h

sigset_t sigmask_kern, *sigmask_ptr = NULL;

if (sigmask_packed) {
    if ((uint64_t)sigmask_packed >= current->addr_limit) return -EFAULT;
    struct pselect6_sigmask packed;
    memcpy(&packed, sigmask_packed, sizeof(packed));

    if (packed.ss_len != sizeof(sigset_t))   // 必须 8 字节
        return -EINVAL;

    if (packed.ss) {
        if ((uint64_t)packed.ss >= current->addr_limit) return -EFAULT;
        memcpy(&sigmask_kern, packed.ss, sizeof(sigset_t));
        sigmask_ptr = &sigmask_kern;
    }
    // packed.ss == NULL → 不修改 mask（退化为 select 行为）
}
```

---

## Syscall 寄存器映射

| Syscall | 寄存器 | 参数 |
|---------|--------|------|
| SYS_select (50) | rdi, rsi, rdx, r10, r8 | nfds, readfds, writefds, exceptfds, timeout_tv |
| SYS_pselect6 (51) | rdi, rsi, rdx, r10, r8, r9 | nfds, readfds, writefds, exceptfds, timeout_ts, sigmask_packed |

select 通过 `syscall6(id, a1..a5)`（arg6=0）。pselect6 通过 `syscall6(id, a1..a6)`（全部 6 个）。

---

## pselect 信号原子性

sig mask 由 `do_pselect6()` 在调用 `do_select()` 前后保存/交换/恢复。`do_select()` 通过 `do_poll_core` 间接进行信号检查，后者在每个循环迭代中检查 `current->signal & ~current->blocked`。

```
do_pselect6(...sigmask_packed):
    解包 sigmask_packed
    首次信号检查 → EINTR? return          // 步骤 A（用 OLD blocked mask）

    if sigmask:
        old_blocked = current->blocked
        current->blocked = sigmask_kern   // 交换完成

    ret = do_select(nfds, r, w, e, ts)    // do_select 入口也做信号检查（步骤 B，用 NEW mask）
                                          // (此后由 do_poll_core 循环检查处理)

    if sigmask:
        current->blocked = old_blocked    // 始终恢复，即使出错
    return ret
```

**步骤 A 的逻辑**：步骤 A 在 mask 交换前用旧 mask 检查信号，如果返回 EINTR，mask 未被修改（正确：pselect 尚未"开始"）。但步骤 A 存在冗余——步骤 B（`do_select` 入口检查）会捕获所有情况（无论信号在 swap 前或后到达）。步骤 A 不改变正确性，只是较保守。实现时两个检查都可保留，删除步骤 A 也不影响行为。


### 已知限制：无限超时下的信号唤醒

`do_poll_core()` 中的 `wait_queue_sleep()` 不通过 `blocker` 框架（`blocker.type == BLOCKER_NONE`）。`sched_unblock_blocked()` 只唤醒 `blocker.type != BLOCKER_NONE` 的任务。因此：

- **有限超时**（timeout > 0）：LAPIC 定时器/PIT 到期唤醒 → 循环重新检查信号 ✓
- **无限超时**（timeout = -1）：仅 fd 事件可唤醒。只有信号 pending 但无 fd 就绪 → 永远不唤醒
- **`nfds==0, timeout==NULL`** → `-ENOSYS`（显式拒绝）

这是 `do_poll` 的已有问题。实际操作中，shell 使用有限超时（busybox ash `SAFE_POLL` 循环），此问题不显现。

---

## API 签名

### 内核

```c
// kernel/include/kernel/select.h

#define FD_SETSIZE 1024

// 用户空间指针使用 void *（匹配 trap.c 现有模式）
int64_t do_select(int nfds, void *readfds, void *writefds,
                  void *exceptfds, void *timeout_tv);

int64_t do_pselect6(int nfds, void *readfds, void *writefds,
                    void *exceptfds, void *timeout_ts,
                    const void *sigmask_packed);
```

### libc

```c
// libc/include/sys/select.h

#include <sys/types.h>   // fd_set（long __fds_bits[16]）
#include <sys/time.h>    // struct timeval, struct timespec
#include <signal.h>      // sigset_t

#define FD_SETSIZE 1024

// ── FD 宏（inline 函数，非类函数宏，避免多次求值）──
#define FD_BITPOS(fd)  ((fd) % (8 * (int)sizeof(long)))
#define FD_IDX(fd)     ((fd) / (8 * (int)sizeof(long)))

static inline void FD_ZERO(fd_set *set) {
    for (int i = 0; i < 16; i++) set->__fds_bits[i] = 0;
}
static inline void FD_SET(int fd, fd_set *set) {
    if ((unsigned)fd < FD_SETSIZE)
        set->__fds_bits[FD_IDX(fd)] |= (1L << FD_BITPOS(fd));
}
static inline void FD_CLR(int fd, fd_set *set) {
    if ((unsigned)fd < FD_SETSIZE)
        set->__fds_bits[FD_IDX(fd)] &= ~(1L << FD_BITPOS(fd));
}
static inline int FD_ISSET(int fd, fd_set *set) {
    return (unsigned)fd < FD_SETSIZE
        && !!(set->__fds_bits[FD_IDX(fd)] & (1L << FD_BITPOS(fd)));
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);
```

---

## Syscall 编号

```c
// kernel/include/uapi/syscall.h
#define SYS_select    50   // 已有
#define SYS_pselect6  51   // 新增
```

---

## 文件变更清单

内核和 libc 的 Makefile 均使用 `$(wildcard ...)` 自动发现源文件，**无需修改**。

| 类别 | 文件 | 行数 | 说明 |
|------|------|------|------|
| **修改** | `kernel/include/kernel/poll.h` | +12/−8 | poll_table_t 动态 entries + max_entries；`poll_table_setup` 返回 int；`poll_wait` 使用 `max_entries` |
| **修改** | `kernel/fs/poll.c` | +60/−25 | 提取 do_poll_core()；poll_table_setup/destroy；nfds==0→timeout；memcpy(NULL)守卫；首次信号检查归属 |
| **新增** | `kernel/fs/select.c` | ~170 | do_select、do_pselect6、fd_set↔pollfd 转换 |
| **新增** | `kernel/include/kernel/select.h` | ~45 | kernel_fd_set + sigset_t + struct pselect6_sigmask、原型（void*）、FD_SETSIZE |
| **新增** | `libc/include/sys/select.h` | ~60 | FD 宏、select()/pselect() 声明 |
| **新增** | `libc/unistd/select.c` | ~40 | libc wrappers + pselect 打包 |
| **修改** | `kernel/include/uapi/syscall.h` | +1 | SYS_pselect6 |
| **修改** | `kernel/arch/x86_64/trap.c` | +15 | dispatch + syscall_names[50,51] |
| **修改** | `user/systest.c` | +130 | 10 个测试用例 |

**总计：~530 行**

### select.c 依赖头文件

```c
#include <kernel/select.h>    // kernel_fd_set, sigset_t, FD_SETSIZE, struct pselect6_sigmask, 原型
#include <kernel/poll.h>      // poll_table_t, do_poll_core, poll_table_setup/destroy
#include <kernel/task.h>      // current, current->signal, current->blocked, addr_limit
#include <uapi/time.h>        // struct timeval, struct timespec
#include <errno.h>            // EINTR, EINVAL, EFAULT, ENOMEM, ENOSYS
#include <string.h>           // memcpy, memset
#include <kernel/slab.h>      // kmalloc, kfree
```

### do_poll 与 do_select 的 nfds==0 行为差异

| 调用 | `nfds==0, timeout==NULL` | 原因 |
|------|-------------------------|------|
| `do_poll` | 立即返回 0 | 历史行为（poll with 0 fds 是空操作） |
| `do_select` | 返回 `-ENOSYS` | POSIX select(0,...,NULL) 要求信号可中断睡眠，当前无法实现 |

这种差异是故意的：`do_poll` 保持向后兼容，`do_select` 对新语义诚实。

### 堆压力

`nfds=1024`（极端）：`kmalloc(1024 × ~48) ≈ 48KB`（poll_table entries）+ `kmalloc(1024 × 8) = 8KB`（pollfd）≈ **56KB**。内核 slab 可接受。

---

## 测试计划

在 `user/systest.c` 中新增 `test_select`：

| # | 测试 | 描述 | 精度说明 |
|---|------|------|----------|
| 1 | `test_select_basic` | pipe 写 → select 报告可读；读空 → 0ms 返回 0 | |
| 2 | `test_select_write` | 空 pipe → 可写；填满 → 0ms 返回 0 | |
| 3 | `test_select_timeout` | 50ms 超时，无 fd → 返回 0 | ~50ms ±10ms |
| 4 | `test_select_null_timeout` | NULL 超时 → 阻塞直到 fork 子进程写入 | |
| 5 | `test_select_multifd` | 3 管道，仅 1 个有数据 → 返回 1 | |
| 6 | `test_select_zero_timeout` | `{0,0}` → 非阻塞轮询，立即 0 | |
| 7 | `test_select_sleep` | `select(0, NULL, NULL, NULL, &{0,50000})` → 休眠 50ms | |
| 8 | `test_select_invalid_fd` | fd 不在表中 → 返回 1，无 fd_set 位 | |
| 9 | `test_pselect_null_sigmask` | sigmask=NULL → 退化为 select | |
| 10 | `test_pselect_bad_ss_len` | pselect6 with ss_len=999 → 返回 -1, errno=EINVAL | |

**注**：`test_pselect_sigmask`（SIGINT 中断测试）依赖于无限超时下的信号唤醒（已知限制）。待 blocker 框架集成后作为后续测试添加。当前先实现 10 个确可工作的测试。
