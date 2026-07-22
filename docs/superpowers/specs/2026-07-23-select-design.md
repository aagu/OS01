# select/pselect 系统调用 — 设计文档

> **日期**: 2026-07-23
> **状态**: v3
> **依赖**: `do_poll()` + `poll_table` 基础设施 (kernel/fs/poll.c)

---

## 目标

在现有的 poll 基础设施上实现 `select(2)` 和 `pselect6(2)`，作为 `fd_set → pollfd → do_poll_core → revents → fd_set` 的适配层。

---

## 评审驱动变更

### v2 → v3

| # | 问题 | 解决方案 |
|---|------|----------|
| 1 | `copy_from_user`/`copy_to_user` 在本内核中不存在 | 改为内核实际模式：`addr_limit` 校验 + `memcpy` 直接解引用 |
| 2 | `fd_set` 未在内核头文件中定义 | 新增 `kernel_fd_set` 类型（`uint64_t __bits[16]`），定义在 `kernel/include/kernel/select.h` |
| 3 | `do_select_core` 残留引用 | 统一为 `do_select()`（pselect 信号章节） |
| 4 | `poll_table_setup` kmalloc 失败无错误路径 | 返回 `int`（0=成功，-ENOMEM=失败），调用方检查 |
| 5 | Makefile 变更不需要（wildcard 自动发现） | 从文件清单中移除 |
| 6 | `do_poll_core` 信号检查契约不明确 | 添加明确的契约：每次扫描前 + 每次唤醒后进行信号检查 |
| 7 | 内核/用户态重复定义 packed 结构体 | 提取到 `kernel/include/uapi/pselect.h`，两端 #include |
| 8 | 较大 nfds 下的堆分配压力 | 标注：nfds=1024 → 48KB heap，slab 可接受 |
| 9 | `nfds` 负值验证顺序 | 确认：`nfds < 0` 检查在转换为 `uint64_t` 之前 |
| 10 | 测试计时精度 | 标注 ±10ms（PIT 100Hz 基准） |

### v1 → v2

| 问题 | 解决方案 |
|------|----------|
| `POLL_MAX_FDS=16` — select 需要 1024 | 重构 `poll_table_t` 为动态 entries 数组；提取 `do_poll_core()` |
| 用户态指针直访 | 入口处校验 + memcpy → 内核栈 fd_set，返回时写回 |
| `nfds==0` 丢失 timeout | nfds==0 且 timeout>0 → 休眠到超时 |
| pselect6 打包 | Linux 兼容 `{sigset_t* ss, size_t ss_len}` 结构体 |
| 超时 ceil 公式 | `(tv_usec + 999) / 1000` |
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

所有后续示例使用此模式。

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

- `int poll_table_setup(pt, max_entries)` — `kmalloc` entries 数组，初始化节点。返回 0 或 `-ENOMEM`。（签名从 `void` 改为 `int`）
- `poll_table_init(pt)` — 重置 nent/triggered（不变）
- `poll_wait(pt, poll_list, fd_lock)` — `if (pt->nent >= pt->max_entries) return;`（`max_entries` 替代 `POLL_MAX_FDS`）
- `poll_table_cleanup(pt)` — 不变（遍历 `pt->nent`）
- `void poll_table_destroy(pt)` — `kfree(pt->entries)`（新增）
- 调用方模式：
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
              │ do_select(nfds, r, w, e, tv)             │
              │   → nfds<0 → -EINVAL (先于 uint64_t 转换) │
              │   → addr_limit + memcpy: 3×fd_set+timeval│
              │   → timeval_to_ms(tv)                    │
              │   → fd_sets → pollfd[nfds] (kmalloc)     │
              │   → poll_table_setup(&pt, nfds)           │
              │   → do_poll_core(pollfd, nfds, ms, &pt)  │
              │   → revents → 3×fd_set                    │
              │   → addr_limit + memcpy 写回 3×fd_set     │
              │   → kfree(pollfd); poll_table_destroy     │
              │                                          │
              │ do_pselect6(nfds, r, w, e, ts, packed)   │
              │   → 解包 packed_sigmask 结构体             │
              │   → 验证 sigsetsize                       │
              │   → save old_blocked, swap *sigmask       │
              │   → do_select 相同逻辑 (timespec→ms)      │
              │   → restore old_blocked                   │
              └──────────────────────────────────────────┘
                       │
                       ▼
              kernel/fs/poll.c
              ┌──────────────────────────────────────────┐
              │ do_poll_core(kfds, nfds, timeout, pt)    │
              │   (共享轮询循环 + 信号检查，不碰用户内存)   │
              │                                          │
              │ do_poll(user_fds, nfds, timeout)         │
              │   → memcpy → do_poll_core → memcpy       │
              └──────────────────────────────────────────┘
```

### 关键提取：do_poll_core

```c
// kernel/fs/poll.c — 新的内部函数，由 do_poll() 和 do_select() 共享
//
// 前置条件：
//   - kfds[0..nfds-1] 已填充 {fd, events, revents=0}
//   - pt 已通过 poll_table_setup() 成功初始化
//   - 调用者负责：用户内存复制、fd_set 转换、sigmask 交换、pt 释放
//
// 信号契约（与 pselect sigmask 原子性相关）：
//   每次调用时，do_poll_core 在以下时机检查 current->signal & ~current->blocked：
//     1. 开始扫描前（首次检查）
//     2. 调用 wait_queue_sleep 前（睡眠前检查）
//     3. 从 wait_queue_sleep 返回后（唤醒后检查）
//   这确保 pselect 的 sigmask 交换在首次信号检查前生效，
//   且 sleep 前后检查受新的 blocked mask 管辖。
//
// 返回：就绪 fd 数量，0=超时，负值=-errno

int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int timeout_val, poll_table_t *pt);
```

`do_poll()` 重构为包装器：
```c
int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout_val) {
    // addr_limit 校验 + memcpy 从 user_fds 读入
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;
    if (nfds == 0 && timeout_val <= 0) return 0;   // 或超时休眠（见 nfds==0 章节）
    if (nfds > POLL_MAX_FDS) return -EINVAL;

    struct pollfd kfds[POLL_MAX_FDS];               // 栈分配（16 个足矣）
    memcpy(kfds, user_fds, nfds * sizeof(struct pollfd));
    for (uint32_t i = 0; i < nfds; i++)
        kfds[i].revents = 0;

    poll_table_t pt;
    if (poll_table_setup(&pt, POLL_MAX_FDS) != 0)
        return -ENOMEM;
    int64_t ret = do_poll_core(kfds, nfds, timeout_val, &pt);
    poll_table_destroy(&pt);

    // 写回 revents（仅当调用方需要时）
    if (ret >= 0) {
        for (uint32_t i = 0; i < nfds; i++)
            user_fds[i].revents = kfds[i].revents;
    }
    return ret;
}
```

**注意**：即使重构后，`do_poll()` 的 `POLL_MAX_FDS=16` 限制**保持不变**（poll 用户看不到行为变化）。select 路径使用自己的动态分配，大小由 call site 决定。

---

## 用户内存安全

使用内核实际的 `addr_limit` + `memcpy` 模式：

```c
// kernel/fs/select.c — do_select()

// ── 入口：验证 + 复制三个 fd_set 到内核栈 ──
fd_set kr, kw, ke;   // libc fd_set（long __fds_bits[16] = 128 字节）
fd_set *kr_ptr = NULL, *kw_ptr = NULL, *ke_ptr = NULL;

if (readfds) {
    if ((uint64_t)readfds >= current->addr_limit) return -EFAULT;
    memcpy(&kr, readfds, sizeof(fd_set));
    kr_ptr = &kr;
}
if (writefds) {
    if ((uint64_t)writefds >= current->addr_limit) return -EFAULT;
    memcpy(&kw, writefds, sizeof(fd_set));
    kw_ptr = &kw;
}
if (exceptfds) {
    if ((uint64_t)exceptfds >= current->addr_limit) return -EFAULT;
    memcpy(&ke, exceptfds, sizeof(fd_set));
    ke_ptr = &ke;
}

// ── 入口：验证 + 复制 timeout ──
struct timeval ktv;
struct timeval *ktv_ptr = NULL;
if (timeout_tv) {
    if ((uint64_t)timeout_tv >= current->addr_limit) return -EFAULT;
    memcpy(&ktv, timeout_tv, sizeof(ktv));
    // 验证 tv_sec, tv_usec 范围
    if (ktv.tv_sec < 0 || ktv.tv_usec < 0 || ktv.tv_usec >= 1000000)
        return -EINVAL;
    ktv_ptr = &ktv;
}

// ── 返回：将修改后的 fd_set 写回用户空间 ──
if (readfds) {
    if ((uint64_t)readfds >= current->addr_limit) return -EFAULT;
    memcpy(readfds, kr_ptr, sizeof(fd_set));
}
if (writefds) {
    if ((uint64_t)writefds >= current->addr_limit) return -EFAULT;
    memcpy(writefds, kw_ptr, sizeof(fd_set));
}
if (exceptfds) {
    if ((uint64_t)exceptfds >= current->addr_limit) return -EFAULT;
    memcpy(exceptfds, ke_ptr, sizeof(fd_set));
}
```

**规则**：`fd_set*` 为 NULL → 跳过该集合（POSIX 允许，不产生错误）。`-EFAULT` **仅在** `addr_limit` 校验失败时返回。（`memcpy` 在合法用户地址上不会失败，因为 ring 0 可访问当前任务 CR3 映射的任何页。）

---

## 类型定义

### 内核 fd_set（kernel_fd_set）

`fd_set` 定义在 `libc/include/sys/types.h`，内核中不存在。内核使用自己的类型来避免命名冲突：

```c
// kernel/include/kernel/select.h

typedef struct {
    uint64_t __bits[16];   // 1024 bits = FD_SETSIZE
} kernel_fd_set;

#define FD_SETSIZE 1024

// 内核侧的 FD 宏（在 select.h 或 select.c 中）
static inline void kern_fd_zero(kernel_fd_set *set) {
    memset(set, 0, sizeof(*set));
}
static inline void kern_fd_set(int fd, kernel_fd_set *set) {
    set->__bits[fd / 64] |= (1ULL << (fd % 64));
}
static inline void kern_fd_clr(int fd, kernel_fd_set *set) {
    set->__bits[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline int kern_fd_isset(int fd, kernel_fd_set *set) {
    return !!(set->__bits[fd / 64] & (1ULL << (fd % 64)));
}
```

内核 API 使用用户空间 `fd_set *` 指针作为 syscall 参数（匹配 POSIX ABI），但在验证 + memcpy 后内部转为 `kernel_fd_set` 操作。

注意：`uint64_t` 固定 8 字节，模拟 LP64 下的 `long` 位索引。libc 的 `fd_set` 使用 `long`（LP64 下也是 8 字节），内核的 `kernel_fd_set` 显式采用 `uint64_t` 确保平台无关性。

### pselect6 打包结构体（共享头文件）

```c
// kernel/include/uapi/pselect.h — 内核和 libc 两端 #include

#ifndef _UAPI_PSELECT_H
#define _UAPI_PSELECT_H

#include <stddef.h>   // size_t

struct pselect6_sigmask {
    const void *ss;       // const sigset_t *（void* 避免依赖 signal.h）
    size_t      ss_len;
};

#endif
```

### libc fd_set（已存在）

`libc/include/sys/types.h:27-28`：
```c
typedef struct {
    long __fds_bits[16];   // 1024 bits
} fd_set;
```

### timeval / timespec（已存在）

```c
// libc/include/sys/time.h
struct timeval  { uint64_t tv_sec; uint64_t tv_usec; };
struct timespec { uint64_t tv_sec; uint64_t tv_nsec; };
```

---

## 参数验证

| 条件 | 返回值 |
|------|--------|
| `nfds < 0` | `-EINVAL`（**在转换为 `uint64_t` 之前检查**） |
| `nfds == 0` + timeout == NULL | 休眠直到收到信号（见下文） |
| `nfds == 0` + timeout == {0,0} | 0（立即返回） |
| `nfds == 0` + timeout > 0 | 休眠到超时，返回 0 |
| `nfds > FD_SETSIZE (1024)` | `-EINVAL` |
| `timeout_tv->tv_sec < 0 \|\| tv_usec < 0 \|\| tv_usec >= 1000000` | `-EINVAL` |
| `timeout_ts->tv_sec < 0 \|\| tv_nsec < 0 \|\| tv_nsec >= 1000000000` | `-EINVAL` |
| `pselect6`: `ss_len != sizeof(sigset_t)` | `-EINVAL` |
| 用户指针 ≥ `addr_limit` | `-EFAULT` |
| `kmalloc` 失败 | `-ENOMEM` |
| 被信号中断 | `-EINTR`（fd_set 不改动） |

### nfds==0 且 timeout 不为零

POSIX 允许 `select(0, NULL, NULL, NULL, &tv)` 作为可移植的亚秒级休眠方式：

```
如果 nfds==0:
    如果 timeout==NULL 或 timeout>0:
        分配 1 个 entry 的 poll_table_t
        调用 do_poll_core(kfds=NULL, nfds=0, timeout, pt)
        → 没有 fd 可扫描 → 立即进 wait_queue_sleep
        → 仅由超时或信号唤醒 → 返回 0 或 -EINTR
    如果 timeout==0:
        返回 0
```

也需修复 `do_poll` 中相同的提前返回：将 `poll.c:186-187` 的 `if (nfds == 0) return 0;` 替换为上述逻辑。

---

## 超时转换

```c
// select:  timeval → ms
ms = (int)(tv_sec * 1000 + (tv_usec + 999) / 1000);

// pselect: timespec → ms
ms = (int)(tv_sec * 1000 + (tv_nsec + 999999) / 1000000);

// NULL timeout → -1（无限等待）
```

**精度说明**：`do_poll_core` 内部将 ms 转换为 PIT 滴答数（`(ms + 9) / 10`，100Hz=10ms/tick）。因此 `tv_usec=1` → `ceil(1/1000)=1ms` → PIT 舍入为 ~10ms（±10ms 误差）。这与具备 10ms 粒度时钟的实际硬件 Linux 行为一致。

---

## 事件映射

### fd_set → pollfd.events

| fd_set | poll 事件 |
|--------|----------|
| readfds | `POLLIN \| POLLRDNORM` |
| writefds | `POLLOUT \| POLLWRNORM` |
| exceptfds | `POLLPRI` |

每个 pollfd 的 `events` 是各集合按位 OR 的结果。`fd` 字段设为索引 `i`。events==0 的 fd 跳过（`fd_poll` 仍将其计入 `POLLNVAL`）。

### pollfd.revents → fd_set（反向映射）

| pollfd.revents 条件 | 填入 fd_set |
|---|---|
| `POLLIN\|POLLRDNORM\|POLLHUP\|POLLERR` 任意置位 | readfds |
| `POLLOUT\|POLLWRNORM\|POLLERR` 任意置位 | writefds |
| `POLLPRI\|POLLERR` 任意置位 | exceptfds |

POSIX 语义：`POLLHUP` 出现在 readfds（表示 EOF-readable），`POLLERR` 出现在所有三个集合。`POLLNVAL` 计入返回值计数但不设置任何 fd_set 位（文件描述符已关闭）。

---

## pselect6：syscall 打包

libc wrapper 将 `{sigmask, sigsetsize}` 打包为一个用户空间结构体；内核 syscall 接收该结构体并解包。结构体定义在共享的 `kernel/include/uapi/pselect.h` 中。

### libc 包装器

```c
// libc/unistd/select.c
#include <uapi/pselect.h>   // struct pselect6_sigmask

int pselect(int nfds, fd_set *r, fd_set *w, fd_set *e,
            const struct timespec *ts, const sigset_t *sigmask)
{
    struct pselect6_sigmask packed;
    packed.ss     = sigmask;                   // NULL = 不修改 mask
    packed.ss_len = sizeof(sigset_t);
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
#include <uapi/pselect.h>

sigset_t sigmask_kern, *sigmask_ptr = NULL;

if (sigmask_packed) {
    if ((uint64_t)sigmask_packed >= current->addr_limit) return -EFAULT;
    struct pselect6_sigmask packed;
    memcpy(&packed, sigmask_packed, sizeof(packed));

    // 验证 sigsetsize 与 sizeof(sigset_t) 匹配
    if (packed.ss_len != sizeof(sigset_t)) return -EINVAL;

    if (packed.ss) {
        if ((uint64_t)packed.ss >= current->addr_limit) return -EFAULT;
        memcpy(&sigmask_kern, packed.ss, sizeof(sigset_t));
        sigmask_ptr = &sigmask_kern;
    }
    // packed.ss == NULL → 不修改 mask（退化为 select 行为）
}
```

---

## Syscall 注册参数

| Syscall | 寄存器 | 参数 |
|---------|--------|------|
| SYS_select (50) | rdi, rsi, rdx, r10, r8 | nfds, readfds, writefds, exceptfds, timeout_tv |
| SYS_pselect6 (51) | rdi, rsi, rdx, r10, r8, r9 | nfds, readfds, writefds, exceptfds, timeout_ts, sigmask_packed |

select 使用 5 个寄存器，通过 `syscall6(id, a1, a2, a3, a4, a5)`（arg6=0）调用。pselect6 使用全部 6 个寄存器。

---

## pselect 信号原子性

sig mask 的 save/swap/restore 由 `do_pselect6()` 处理，在调用 `do_select()` 之前/之后执行。`do_select()` 不感知信号。

```
do_pselect6(...sigmask_packed):
    解包 + 验证 sigmask_packed
    memcpy → sigmask_kern

    if sigmask:
        old_blocked = current->blocked
        current->blocked = sigmask_kern

    ret = do_select(nfds, r, w, e, ts)   // 不感知信号
                                         // → do_poll_core 内部进行信号检查
                                         //    使用交换后的 blocked mask

    if sigmask:
        current->blocked = old_blocked   // 始终恢复，即使出错
    return ret
```

### 已知限制

`do_poll_core()` 内部的 `wait_queue_sleep()` **不与 blocker 框架集成** — 挂起信号不会自动唤醒睡眠中的任务。信号仅在以下时机检查：
1. 睡眠前：`current->signal & ~current->blocked`
2. 睡眠后（由 fd 事件或超时唤醒）：信号再次检查

这意味着对于**无限超时**（timeout=-1），如果只有信号变为 pending（没有 fd 事件或超时），任务**不会自动唤醒**。这是 `do_poll` 的已有问题，不属于 select 的变更范围。实际上，shell 在大多数情况下使用超时（如 busybox ash 的 `SAFE_POLL` 循环），因此这个问题在典型场景中不会显现。SMP 下可能需要重新审视。

---

## API 签名

### 内核

```c
// kernel/include/kernel/select.h

#define FD_SETSIZE 1024

typedef struct {
    uint64_t __bits[16];
} kernel_fd_set;

int64_t do_select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout);

int64_t do_pselect6(int nfds, fd_set *readfds, fd_set *writefds,
                    fd_set *exceptfds, struct timespec *timeout,
                    const void *sigmask_packed);
```

### libc

```c
// libc/include/sys/select.h

#include <sys/types.h>   // fd_set（已存在：long __fds_bits[16]）
#include <sys/time.h>    // struct timeval, struct timespec
#include <signal.h>      // sigset_t

#define FD_SETSIZE 1024

// ── 内联函数（并非类函数宏，避免多次求值）──
#define FD_BITPOS(fd)  ((fd) % (8 * (int)sizeof(long)))
#define FD_IDX(fd)     ((fd) / (8 * (int)sizeof(long)))

static inline void FD_ZERO(fd_set *set) {
    for (int i = 0; i < 16; i++) set->__fds_bits[i] = 0;
}
static inline void FD_SET(int fd, fd_set *set) {
    set->__fds_bits[FD_IDX(fd)] |= (1L << FD_BITPOS(fd));
}
static inline void FD_CLR(int fd, fd_set *set) {
    set->__fds_bits[FD_IDX(fd)] &= ~(1L << FD_BITPOS(fd));
}
static inline int FD_ISSET(int fd, fd_set *set) {
    return !!(set->__fds_bits[FD_IDX(fd)] & (1L << FD_BITPOS(fd)));
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);
```

---

## Syscall 编号

`kernel/include/uapi/syscall.h`：

```c
#define SYS_select    50   // 已有，当前返回 -ENOSYS
#define SYS_pselect6  51   // 新增
```

---

## 文件变更清单

内核和 libc 的 Makefile 均使用 `$(wildcard ...)` 自动发现源文件，**无需修改**。

| 类别 | 文件 | 行数 | 说明 |
|------|------|------|------|
| **修改** | `kernel/include/kernel/poll.h` | +12/−8 | poll_table_t 动态 entries + max_entries；`poll_table_setup` 返回 int |
| **修改** | `kernel/fs/poll.c` | +55/−30 | 提取 do_poll_core()；poll_table_setup/destroy 重构；nfds==0 超时修复；调用方错误检查 |
| **新增** | `kernel/fs/select.c` | ~160 | do_select()、do_pselect6()、fd_set↔pollfd 转换 |
| **新增** | `kernel/include/kernel/select.h` | ~30 | kernel_fd_set 类型、FD_SETSIZE、原型 |
| **新增** | `kernel/include/uapi/pselect.h` | ~12 | struct pselect6_sigmask（共享） |
| **新增** | `libc/include/sys/select.h` | ~55 | FD 宏、select()/pselect() 声明 |
| **新增** | `libc/unistd/select.c` | ~40 | libc wrappers + pselect 打包 |
| **修改** | `kernel/include/uapi/syscall.h` | +1 | SYS_pselect6 |
| **修改** | `kernel/arch/x86_64/trap.c` | +15 | dispatch + syscall_names[50,51] |
| **修改** | `user/systest.c` | +120 | 10 个测试用例 |

**总计：~500 行**

### 堆压力说明

- `poll_table_setup(pt, nfds)` 分配 `nfds * sizeof(poll_wait_entry_t)`。
- `poll_wait_entry_t` 大小约 48 字节（list_t 节点 + 2 个指针 + spinlock_T 指针）。
- `nfds=16`（典型 poll）→ ~768 字节；`nfds=1024`（极端 select）→ ~48KB。
- 内核 slab allocator 处理 48KB 分配没有问题，但 `pollfd` 数组额外需要 `nfds * 8` 字节（峰值 ~8KB）。
- 两个分配的总峰值 ~56KB — 在内核堆上可接受。

---

## 测试计划

在 `user/systest.c` 中新增 `test_select`：

| # | 测试 | 描述 | 精度说明 |
|---|------|------|----------|
| 1 | `test_select_basic` | pipe 写 → select 报告可读；读空 → 0ms 超时返回 0 | |
| 2 | `test_select_write` | 空 pipe → 可写；填满 → 0ms 超时返回 0 | |
| 3 | `test_select_timeout` | 50ms 超时，无 fd 就绪 → 返回 0 | ~50ms elapsed ±10ms（PIT 100Hz） |
| 4 | `test_select_null_timeout` | NULL 超时 → 阻塞直到 fork 子进程写入管道 | |
| 5 | `test_select_multifd` | 3 个管道，仅 1 个有数据 → 返回 1，仅对应 fd 在 readfds 中置位 | |
| 6 | `test_select_zero_timeout` | `select(nfds, r, w, e, {0,0})` → 非阻塞轮询，立即返回 0 | |
| 7 | `test_select_null_sets` | `select(0, NULL, NULL, NULL, &{0,50000})` → 休眠，返回 0 | ~50ms elapsed ±10ms |
| 8 | `test_select_invalid_fd` | nfds=1，fd 不在表中 → 返回 1，revents=POLLNVAL | |
| 9 | `test_pselect_sigmask` | 阻塞 SIGINT，pselect 期间解除阻塞 → SIGINT 中断，errno=EINTR，mask 恢复 | |
| 10 | `test_pselect_null_sigmask` | pselect with sigmask=NULL → 退化为 select 行为 | |

**前 8 个测试**覆盖 select，**后 2 个**覆盖 pselect。

---

## 已知限制（SMP 相关）

- **`current_poll_wq` 全局变量**（`kernel/fs/poll.c:178`）：在 SMP 下不安全；需要 per-CPU 或 per-task 存储。select 实现将遵循相同模式（使用该全局变量）。这是已有问题，不是 select 引入的。
- **信号唤醒**：参见上述 pselect 信号原子性章节。无限超时下的信号唤醒需要未来的 blocker 框架集成。
