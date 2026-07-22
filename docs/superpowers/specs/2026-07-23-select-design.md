# select/pselect 系统调用 — 设计文档

> **日期**: 2026-07-23
> **状态**: v2 (reviewed)
> **依赖**: `do_poll()` + `poll_table` 基础设施 (kernel/fs/poll.c)

---

## 目标

在现有的 poll 基础设施上实现 `select(2)` 和 `pselect6(2)`，作为 `fd_set → pollfd → do_poll_core → revents → fd_set` 的适配层。

---

## 评审驱动变更 (v1 → v2)

| 严重度 | 问题 | 解决方案 |
|--------|------|----------|
| 🔴 严重 | `POLL_MAX_FDS=16` 硬限制 — select 需要 1024 | 重构 `poll_table_t` 为动态 entries 数组；提取 `do_poll_core()` 供两者共享 |
| 🔴 严重 | 用户态指针直访 (`FD_ISSET(i, readfds)`) | 入口处 `copy_from_user` → 内核栈 fd_set，返回时 `copy_to_user` 写回 |
| 🟡 中等 | `nfds==0` 丢失 timeout 休眠 | `nfds==0` 且 timeout>0 → 休眠到超时；`nfds==0` 且 timeout=NULL → 无限休眠直到信号 |
| 🟡 中等 | pselect6 内核签名未指定 syscall 解包 | 定义 Linux 兼容的打包结构，libc wrapper 打包，内核 syscall 解包 |
| 🟡 中等 | 超时转换 `ceil()` 无 C 表达式 | 明确 `(tv_usec + 999) / 1000`，记录 PIT 10ms 精度舍入 |
| 🟢 次要 | NULL fd_set 错误映射为 -EFAULT | NULL 表示"不关心该集合"（POSIX 允许），`-EFAULT` 仅用于 `copy_from_user` 失败 |
| 🟢 次要 | sigmask 原子性断言过于乐观 | 记录已知限制：`wait_queue_sleep` 不与信号机制集成，信号仅在 sleep 前后检查 |
| 🟢 次要 | `current_poll_wq` SMP 全局变量 | 记录 SMP 限制（单核安全），标注为预存问题 |

---

## 架构

### 核心重构：poll_table_t 动态化 + do_poll_core 提取

当前 `poll_table_t` 使用固定大小栈数组 `poll_wait_entry_t entries[POLL_MAX_FDS]`（16 个条目）。要支持 select 的 FD_SETSIZE=1024，需要动态分配。

**变更：`poll_table_t` 内部 entries 改为指针**

```c
// kernel/include/kernel/poll.h

typedef struct poll_table {
    wait_queue_t        wq;
    poll_wait_entry_t  *entries;      // 动态分配（曾为 entries[POLL_MAX_FDS]）
    int                 max_entries;   // 容量
    int                 nent;          // 活跃条目数
    bool                triggered;     // 快捷退出：已有 fd 就绪
} poll_table_t;
```

- `poll_table_setup(pt, max_entries)` — `kmalloc` entries 数组，初始化节点
- `poll_table_init(pt)` — 重置 nent/triggered（不变）
- `poll_wait(pt, poll_list, fd_lock)` — 使用 `pt->max_entries` 替代 `POLL_MAX_FDS`
- `poll_table_cleanup(pt)` — 不变（使用 `pt->nent`）
- `poll_table_destroy(pt)` — `kfree(pt->entries)`（新增）

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
              │   → copy_from_user: 3×fd_set + timeval   │
              │   → timeval_to_ms(tv)                    │
              │   → fd_sets → pollfd[nfds] (heap)        │
              │   → poll_table_setup(&pt, nfds) (heap)   │
              │   → do_poll_core(pollfd, nfds, ms, &pt)  │
              │   → revents → 3×fd_set                    │
              │   → copy_to_user: 3×fd_set                │
              │   → kfree(pollfd, pt.entries)             │
              │                                          │
              │ do_pselect6(nfds, r, w, e, ts, packed)   │
              │   → copy_from_user: packed_sigmask 结构    │
              │   → 验证 sigsetsize == sizeof(sigset_t)   │
              │   → copy_from_user: *sigmask              │
              │   → save old_blocked, swap *sigmask       │
              │   → do_select 相同逻辑 (timespec→ms)      │
              │   → restore old_blocked                   │
              └──────────────────────────────────────────┘
                       │
                       ▼
              kernel/fs/poll.c
              ┌──────────────────────────────────────────┐
              │ do_poll_core(kfds, nfds, timeout, pt)    │
              │   (提取的共享轮询循环，不触碰用户内存)      │
              │                                          │
              │ do_poll(user_fds, nfds, timeout)         │
              │   → copy_from_user → do_poll_core        │
              │   → copy_to_user (不变，重构为包装器)     │
              └──────────────────────────────────────────┘
```

### 关键提取：do_poll_core

```c
// kernel/fs/poll.c — 新的内部函数，由 do_poll() 和 do_select_core() 共享
//
// 假设：
//   - kfds[0..nfds-1] 已填充 {fd, events, revents=0}
//   - pt 已通过 poll_table_setup(&pt, max_entries) 初始化
//   - 调用者负责：用户内存复制（in+out）、fd_set 转换、sigmask 交换、pt 释放
//
// 返回：就绪 fd 数量，0=超时，负值=-errno

int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int timeout_val, poll_table_t *pt);
```

`do_poll()` 重构为包装器：
```c
int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout_val) {
    // ... copy_from_user ...
    struct pollfd kfds[POLL_MAX_FDS];           // 栈分配（16 个足矣）
    poll_table_t pt;
    poll_table_setup(&pt, POLL_MAX_FDS);        // kmalloc 16 个 entry
    int64_t ret = do_poll_core(kfds, nfds, timeout_val, &pt);
    poll_table_destroy(&pt);
    // ... copy_to_user ...
    return ret;
}
```

**注意**：即使重构后，`do_poll()` 的 `POLL_MAX_FDS=16` 限制**保持不变**（poll 用户看不到行为变化）。select 路径使用自己的动态分配，大小由 call site 决定。

---

## 用户内存安全

每个用户态指针在解引用前必须验证并复制：

```c
// ── 入口：将三个 fd_set 从用户空间复制到内核栈 ──

fd_set kr, kw, ke;
fd_set *kr_ptr = NULL, *kw_ptr = NULL, *ke_ptr = NULL;

if (readfds) {
    if ((uint64_t)readfds >= current->addr_limit) return -EFAULT;
    if (copy_from_user(&kr, readfds, sizeof(fd_set))) return -EFAULT;
    kr_ptr = &kr;
}
if (writefds) {
    if ((uint64_t)writefds >= current->addr_limit) return -EFAULT;
    if (copy_from_user(&kw, writefds, sizeof(fd_set))) return -EFAULT;
    kw_ptr = &kw;
}
if (exceptfds) {
    if ((uint64_t)exceptfds >= current->addr_limit) return -EFAULT;
    if (copy_from_user(&ke, exceptfds, sizeof(fd_set))) return -EFAULT;
    ke_ptr = &ke;
}

// ── 返回前：将修改后的 fd_set 写回用户空间 ──
if (readfds   && copy_to_user(readfds,   kr_ptr, sizeof(fd_set))) return -EFAULT;
if (writefds  && copy_to_user(writefds,  kw_ptr, sizeof(fd_set))) return -EFAULT;
if (exceptfds && copy_to_user(exceptfds, ke_ptr, sizeof(fd_set))) return -EFAULT;
```

**规则**：`fd_set*` 为 NULL → 跳过该集合（POSIX 允许，不产生错误）。`-EFAULT` **仅在** `copy_from_user` / `copy_to_user` 失败时返回。

Timeout 指针同样处理：
```c
struct timeval ktv;
if (timeout_tv) {
    if ((uint64_t)timeout_tv >= current->addr_limit) return -EFAULT;
    if (copy_from_user(&ktv, timeout_tv, sizeof(ktv))) return -EFAULT;
    // 验证 tv_sec, tv_usec 范围
}
```

## 参数验证

| 条件 | 返回值 |
|------|--------|
| `nfds < 0` | `-EINVAL` |
| `nfds == 0` + timeout == NULL | 休眠直到收到信号（见下文） |
| `nfds == 0` + timeout == {0,0} | 0（立即返回） |
| `nfds == 0` + timeout > 0 | 休眠到超时，返回 0 |
| `nfds > FD_SETSIZE (1024)` | `-EINVAL` |
| `timeout_tv->tv_sec < 0 \|\| tv_usec < 0 \|\| tv_usec >= 1000000` | `-EINVAL` |
| `timeout_ts->tv_sec < 0 \|\| tv_nsec < 0 \|\| tv_nsec >= 1000000000` | `-EINVAL` |
| `copy_from_user` / `copy_to_user` 失败 | `-EFAULT` |
| `kmalloc` 失败 | `-ENOMEM` |
| 被信号中断 | `-EINTR`（fd_set 不改动） |

### nfds==0 且 timeout 不为零

POSIX 允许 `select(0, NULL, NULL, NULL, &tv)` 作为可移植的亚秒级休眠方式。实现方法：

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

## 超时转换

```
select:  ms = tv_sec * 1000 + (tv_usec + 999) / 1000     // 向零取整向上
pselect: ms = tv_sec * 1000 + (tv_nsec + 999999) / 1000000

NULL timeout → -1（无限等待）
```

**精度说明**：`do_poll_core` 内部将 ms 转换为 PIT 滴答数（`(ms + 9) / 10`，100Hz=10ms/tick）。因此 `tv_usec=1` → `ceil(1/1000)=1ms` → PIT 舍入为 ~10ms。这与具备 10ms 粒度时钟的实际硬件 Linux 行为一致。

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

## pselect6：syscall 打包

### Linux ABI 与我们的适配

Linux 的实际 syscall 签名为：
```c
// Linux: pselect6(int nfds, fd_set *r, fd_set *w, fd_set *e,
//                 const struct timespec *ts,
//                 const void *sigmask_packed)
//
// 其中 sigmask_packed 指向：
// struct { const sigset_t *ss; size_t ss_len; }
```

我们使用相同的打包方案。libc wrapper 打包，内核解包。

### libc 包装器

```c
// libc/unistd/select.c

struct pselect6_sigmask {
    const sigset_t *ss;
    size_t ss_len;
};

int pselect(int nfds, fd_set *r, fd_set *w, fd_set *e,
            const struct timespec *ts, const sigset_t *sigmask)
{
    struct pselect6_sigmask packed;
    packed.ss = sigmask;                  // NULL = 不修改 mask
    packed.ss_len = sizeof(sigset_t);
    int64_t ret = syscall6(SYS_pselect6,
                           (uint64_t)(int64_t)nfds,
                           (uint64_t)r, (uint64_t)w, (uint64_t)e,
                           (uint64_t)ts, (uint64_t)&packed);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

### 内核验证

```c
// kernel/fs/select.c — do_pselect6()

if (sigmask_packed) {
    if ((uint64_t)sigmask_packed >= current->addr_limit) return -EFAULT;
    struct { const sigset_t *ss; size_t ss_len; } packed;
    if (copy_from_user(&packed, sigmask_packed, sizeof(packed))) return -EFAULT;

    // 验证 sigsetsize 与 sizeof(sigset_t) 匹配
    if (packed.ss_len != sizeof(sigset_t)) return -EINVAL;

    if (packed.ss) {
        if ((uint64_t)packed.ss >= current->addr_limit) return -EFAULT;
        if (copy_from_user(&sigmask_kern, packed.ss, sizeof(sigset_t))) return -EFAULT;
        sigmask_ptr = &sigmask_kern;
    }
    // packed.ss == NULL → 不修改 mask（退化为 select 行为）
}
```

## Syscall 注册参数

| Syscall | 寄存器 | 参数 |
|---------|--------|------|
| SYS_select (50) | rdi, rsi, rdx, r10, r8 | nfds, readfds, writefds, exceptfds, timeout_tv |
| SYS_pselect6 (51) | rdi, rsi, rdx, r10, r8, r9 | nfds, readfds, writefds, exceptfds, timeout_ts, sigmask_packed |

select 使用 5 个寄存器，通过 `syscall6(id, a1, a2, a3, a4, a5)`（arg6=0）调用。pselect6 使用全部 6 个寄存器。

## pselect 信号原子性

sig mask 的 save/swap/restore 由 `do_pselect6()` 处理，**不在** `do_select_core()` 内部。`do_select_core()` 不感知信号。

```
do_pselect6(...sigmask_packed):
    解包 + 验证 sigmask_packed
    copy_from_user → sigmask_kern

    if sigmask:
        old_blocked = current->blocked
        current->blocked = sigmask_kern

    ret = do_select_core(...)   // 不感知信号

    if sigmask:
        current->blocked = old_blocked   // 始终恢复，即使出错
    return ret
```

### 已知限制

`do_poll_core()` 内部的 `wait_queue_sleep()` **不与信号机制集成** — 它不通过 `blocker` 框架，因此挂起信号不会自动唤醒睡眠中的任务。信号仅在以下时机检查：
1. 睡眠前：`current->signal & ~current->blocked`
2. 睡眠后（由 fd 事件或超时唤醒）：信号再次检查

这意味着对于**无限超时**（timeout=-1），如果只有信号变为 pending（没有 fd 事件或超时），任务**不会自动唤醒**。这是 `do_poll` 的已有问题（详情见评审意见第 8 条），不属于 select 的变更范围。实际上，shell 在大多数情况下使用超时（如 busybox ash 的 `SAFE_POLL` 循环），因此这个问题在典型场景中不会显现。SMP 下可能需要重新审视。

## API 签名

### 内核

```c
// kernel/include/kernel/select.h

#define FD_SETSIZE 1024

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

// ── 宏（作为内联函数实现，便于类型安全调试）──
void FD_ZERO(fd_set *set);
void FD_SET(int fd, fd_set *set);
void FD_CLR(int fd, fd_set *set);
int  FD_ISSET(int fd, fd_set *set);

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);
```

注意：`fd_set` 定义在 `<sys/types.h>` 中（已存在），不在 `<sys/select.h>` 中重复。`FD_SETSIZE` 由 `<sys/select.h>` 提供，与 `__fds_bits[16]` 的 1024 位容量匹配。

### FD 宏实现

```c
// 以 inline 函数实现，而非类函数宏，避免多次求值
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
```

内联函数优于宏（类型安全，无多次求值）。`memset` 也可用于 `FD_ZERO`（`memset(set, 0, sizeof(fd_set))`）——两者等价，选择更清晰的即可。

## Syscall 编号

`kernel/include/uapi/syscall.h`：

```c
#define SYS_select    50   // 已有，当前返回 -ENOSYS
#define SYS_pselect6  51   // 新增
```

`kernel/arch/x86_64/trap.c` 的 `syscall_names` 表需要为两个都添加条目（索引 50 和 51）。

## 文件变更清单

| 类别 | 文件 | 行数 | 说明 |
|------|------|------|------|
| **修改** | `kernel/include/kernel/poll.h` | +10/−8 | poll_table_t 动态 entries，max_entries 字段，API 变更 |
| **修改** | `kernel/fs/poll.c` | +50/−30 | 提取 do_poll_core()，重构 poll_table_setup/destroy，nfds==0 超时修复 |
| **新增** | `kernel/fs/select.c` | ~150 | do_select，do_pselect6，do_select_core |
| **新增** | `kernel/include/kernel/select.h` | ~25 | 原型，FD_SETSIZE |
| **新增** | `libc/include/sys/select.h` | ~55 | FD 宏，select()，pselect() 声明 |
| **新增** | `libc/unistd/select.c` | ~40 | libc 包装器 + pselect6 打包 |
| **修改** | `kernel/include/uapi/syscall.h` | +1 | SYS_pselect6 |
| **修改** | `kernel/arch/x86_64/trap.c` | +15 | dispatch + syscall_names[50,51] |
| **修改** | `kernel/fs/Makefile` | +1 | select.o |
| **修改** | `libc/Makefile` | +1 | select.o |
| **修改** | `user/systest.c` | +120 | 8-10 个测试用例 |
**总计：~490 行**（含 poll_table 重构）

注：`poll_table_setup` 仅在 `kernel/fs/poll.c` 的 `do_poll()` 中调用（以及新增的 `select.c`）。`tty.c`、`devfs.c`、`futex.c` 通过 `fd_poll()` → `poll_wait()` 使用 poll_table，不受 setup API 变更影响。

## 测试计划

在 `user/systest.c` 中新增 `test_select`：

| # | 测试 | 描述 |
|---|------|------|
| 1 | `test_select_basic` | pipe 写 → select 报告可读；读空 → 0ms 超时返回 0 |
| 2 | `test_select_write` | 空 pipe → 可写；填满 → 0ms 超时返回 0 |
| 3 | `test_select_timeout` | 50ms 超时，无 fd 就绪 → 返回 0，约 50ms 已过 |
| 4 | `test_select_null_timeout` | NULL 超时 → 阻塞直到 fork 子进程写入管道 |
| 5 | `test_select_multifd` | 3 个管道，仅 1 个有数据 → 返回 1，仅对应 fd 在 readfds 中置位 |
| 6 | `test_select_zero_timeout` | `select(nfds, r, w, e, {0,0})` → 非阻塞轮询，立即返回 0 |
| 7 | `test_select_null_sets` | `select(0, NULL, NULL, NULL, &{0,50000})` → 休眠 50ms，返回 0 |
| 8 | `test_select_invalid_fd` | nfds=1，fd 不在表中 → 返回 1，POLLNVAL 相关映射 |
| 9 | `test_pselect_sigmask` | 阻塞 SIGINT，pselect 期间解除阻塞 → SIGINT 中断，errno=EINTR，mask 恢复 |
| 10 | `test_pselect_null_sigmask` | pselect with sigmask=NULL → 退化为 select 行为 |

**前 8 个测试**覆盖 select，**后 2 个**覆盖 pselect。

## 已知限制（SMP 相关）

- **`current_poll_wq` 全局变量**（`kernel/fs/poll.c:178`）：在 SMP 下不安全；需要 per-CPU 或 per-task 存储。select 实现将遵循相同模式（使用该全局变量）。这是已有问题，不是 select 引入的。
- **信号唤醒**：参见上述 pselect 信号原子性章节。无限超时下的信号唤醒需要未来的 blocker 框架集成。
- **`nfds` 类型**：`do_select` 接受 `int nfds`（POSIX），`do_poll_core` 需要 `uint64_t`。调用处将 `int` 转换为 `uint64_t`（零扩展）。传入负数 `nfds` 的上限检查和 POSIX 错误语义在 `do_select` / `do_pselect6` 中处理（负数 → `-EINVAL`）。
