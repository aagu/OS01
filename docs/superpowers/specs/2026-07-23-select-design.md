# select/pselect 系统调用 — 设计文档

> **日期**: 2026-07-23
> **状态**: draft
> **依赖**: `do_poll()` (已实现, kernel/fs/poll.c)

---

## 目标

在现有的 `poll(2)` 基础设施上实现 `select(2)` 和 `pselect6(2)`，作为 `fd_set → pollfd → do_poll → revents → fd_set` 的适配层。

## 动机

- `SYS_select = 50` 已预留，目前返回 `-ENOSYS`
- `do_poll()` 和 `poll_table` 已完成（systest 78/78 pass），所有工作都在复用层
- 许多 busybox applet 使用 `select()` 而非 `poll()`，完成 select 解锁更多用户态工具

## 架构

```
              libc
              ┌────────────────────────┐
              │ select() / pselect()   │
              └────────────────────────┘
                       │ SYS_select=50 / SYS_pselect6=51
                       ▼
              kernel/fs/select.c
              ┌──────────────────────────────────────────┐
              │ do_select_core(nfds, r/w/e fd_set*, ms,  │
              │               sigmask*)                  │
              │                                         │
              │ 1. alloc pollfd[nfds] (heap)             │
              │ 2. for fd 0..nfds-1:                    │
              │       if fd in readfds  → POLLIN         │
              │       if fd in writefds → POLLOUT        │
              │       if fd in exceptfds→ POLLPRI         │
              │ 3. (pselect) save & swap sigmask         │
              │ 4. ret = do_poll(pollfd, nfds, ms)       │
              │ 5. (pselect) restore sigmask             │
              │ 6. if ret>0: zero 3 fd_sets,            │
              │       fill from pollfd[i].revents        │
              │ 7. kfree(pollfd); return ret             │
              └──────────────────────────────────────────┘
                       │
                       ▼
              kernel/fs/poll.c
              ┌──────────────────────────────────────────┐
              │             do_poll() (不变)              │
              └──────────────────────────────────────────┘
```

## 类型定义

### fd_set 和 FD macros（libc）

`fd_set` 已存在于 `libc/include/sys/types.h`：

```c
typedef struct {
    long __fds_bits[16];   // 1024 bits = FD_SETSIZE
} fd_set;
```

新增 `<sys/select.h>` 头文件，提供宏：

```
FD_ZERO(set)     → memset(set, 0, sizeof(fd_set))
FD_SET(fd, set)   → set->__fds_bits[fd/N] |=  (1L << (fd % N))
FD_CLR(fd, set)   → set->__fds_bits[fd/N] &= ~(1L << (fd % N))
FD_ISSET(fd, set) → !!(set->__fds_bits[fd/N] & (1L << (fd % N)))
```

其中 `N = 8 * sizeof(long)`。

### timeval / timespec

已存在：
- `struct timeval { uint64_t tv_sec; uint64_t tv_usec; }` — `libc/include/sys/time.h`
- `struct timespec { uint64_t tv_sec; uint64_t tv_nsec; }` — `libc/include/sys/time.h`

## 事件映射

### fd_set → pollfd.events

| fd_set | poll 事件 |
|--------|----------|
| readfds | `POLLIN | POLLRDNORM` |
| writefds | `POLLOUT | POLLWRNORM` |
| exceptfds | `POLLPRI` |

每个 pollfd 的 `events` 是上述三个集合的按位 OR。`fd` 字段设为索引 `i`。

### pollfd.revents → fd_set（反向映射）

| pollfd.revents 条件 | 填入 fd_set |
|---|---|
| `POLLIN\|POLLRDNORM\|POLLHUP\|POLLERR` 任意置位 | readfds |
| `POLLOUT\|POLLWRNORM\|POLLERR` 任意置位 | writefds |
| `POLLPRI\|POLLERR` 任意置位 | exceptfds |

POSIX 语义：`POLLHUP` 出现在 readfds 中（表示 EOF-readable），`POLLERR` 出现在所有三个集合中。

## 参数验证

| 条件 | 返回值 |
|------|--------|
| `nfds == 0` | 0（立即返回，不分配） |
| `nfds > FD_SETSIZE (1024)` | `-EINVAL` |
| `timeout_tv->tv_sec < 0 \|\| tv_usec < 0 \|\| tv_usec >= 1000000` | `-EINVAL` |
| `timeout_ts->tv_sec < 0 \|\| tv_nsec < 0 \|\| tv_nsec >= 1000000000` | `-EINVAL` |
| fd_set 指针为 NULL（且 nfds > 0） | `-EFAULT` |
| `kmalloc` 失败 | `-ENOMEM` |
| 被信号中断（pselect mask swap 后） | `-EINTR`（fd_set 不改动） |

## 超时转换

```
select:  tv_sec*1000 + ceil(tv_usec/1000)  →  ms
pselect: tv_sec*1000 + ceil(tv_nsec/1000000) →  ms

NULL timeout → -1（无限等待，与 poll 一致）
```

`do_poll()` 已支持 -1（无限等待）和正数 ms 值。

## pselect 信号原子性

sig mask 的 save/swap/restore 由 `do_pselect6()` 处理，**不在** `do_select_core()` 内部。`do_select_core()` 不感知信号 —— 它只负责 `fd_set ↔ pollfd` 转换。

```
do_select(...timeval *tv):
    ms = timeval_to_ms(tv)
    return do_select_core(nfds, r, w, e, ms)   // sigmask = NULL

do_pselect6(...timespec *ts, sigset_t *sigmask):
    ms = timespec_to_ms(ts)
    old_blocked = current->blocked
    if sigmask:
        current->blocked = *sigmask  // 挂载新 mask
        // 此时解除阻塞的 pending 信号变得可投递
    ret = do_select_core(nfds, r, w, e, ms)
    if sigmask:
        current->blocked = old_blocked  // 始终恢复
    return ret
```

在单核协作调度下，"swap mask" 到 `wait_queue_sleep` 之间没有抢占 → 天然满足 POSIX 原子性要求。

POSIX 保证：无论成功、超时还是被信号中断，返回时 sigmask 总是恢复到调用前的状态。

## API 签名

### 内核

```c
// kernel/include/kernel/select.h

#define FD_SETSIZE 1024

int64_t do_select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout);

int64_t do_pselect6(int nfds, fd_set *readfds, fd_set *writefds,
                    fd_set *exceptfds, struct timespec *timeout,
                    const sigset_t *sigmask, size_t sigsetsize);
```

### libc

```c
// libc/include/sys/select.h

#include <sys/types.h>   // fd_set
#include <sys/time.h>    // struct timeval, struct timespec
#include <signal.h>      // sigset_t

#define FD_SETSIZE 1024

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

## Syscall 编号

`kernel/include/uapi/syscall.h`：

```c
#define SYS_select    50   // 已有，当前 -ENOSYS
#define SYS_pselect6  51   // 新增
```

## 内部实现：do_select_core

```c
static int64_t do_select_core(int nfds, fd_set *readfds, fd_set *writefds,
                               fd_set *exceptfds, int timeout_ms)
{
    if (nfds == 0) return 0;
    if (nfds > FD_SETSIZE) return -EINVAL;

    struct pollfd *fds = kmalloc(nfds * sizeof(struct pollfd));
    if (!fds) return -ENOMEM;

    for (int i = 0; i < nfds; i++) {
        fds[i].fd = i;
        fds[i].events = 0;
        fds[i].revents = 0;
        if (readfds   && FD_ISSET(i, readfds))   fds[i].events |= (POLLIN  | POLLRDNORM);
        if (writefds  && FD_ISSET(i, writefds))  fds[i].events |= (POLLOUT | POLLWRNORM);
        if (exceptfds && FD_ISSET(i, exceptfds)) fds[i].events |= POLLPRI;
    }

    int64_t ret = do_poll(fds, (uint64_t)nfds, timeout_ms);
    if (ret < 0) { kfree(fds); return ret; }

    // 反向映射：revents → fd_set
    if (readfds)   memset(readfds,   0, sizeof(fd_set));
    if (writefds)  memset(writefds,  0, sizeof(fd_set));
    if (exceptfds) memset(exceptfds, 0, sizeof(fd_set));

    int64_t count = 0;
    for (int i = 0; i < nfds; i++) {
        if (fds[i].revents == 0) continue;
        count++;
        uint32_t r = fds[i].revents;
        if (readfds   && (r & (POLLIN | POLLRDNORM | POLLHUP | POLLERR))) FD_SET(i, readfds);
        if (writefds  && (r & (POLLOUT | POLLWRNORM | POLLERR)))         FD_SET(i, writefds);
        if (exceptfds && (r & (POLLPRI | POLLERR)))                      FD_SET(i, exceptfds);
    }

    kfree(fds);
    return count;
}
```

## 文件变更清单

| 类别 | 文件 | 行数 |
|------|------|------|
| **新增** | `kernel/fs/select.c` | ~120 |
| **新增** | `kernel/include/kernel/select.h` | ~20 |
| **新增** | `libc/include/sys/select.h` | ~45 |
| **新增** | `libc/unistd/select.c` | ~25 |
| **修改** | `kernel/include/uapi/syscall.h` | +1 (SYS_pselect6) |
| **修改** | `kernel/arch/x86_64/trap.c` | +10 (dispatch + name table) |
| **修改** | `kernel/fs/Makefile` | +1 (select.o) |
| **修改** | `libc/Makefile` | +1 (select.o) |
| **修改** | `user/systest.c` | +80 (6 test cases) |

**总计: ~300 行**

## 测试计划

在 `user/systest.c` 中新增 `test_select`（旁边是已有的 `test_poll`）：

| # | 测试 | 描述 |
|---|------|------|
| 1 | `test_select_basic` | pipe 写 → select 报告可读；读空 → 0ms 超时返回 0 |
| 2 | `test_select_write` | 空 pipe → writable；填满 → 0ms 超时返回 0 |
| 3 | `test_select_timeout` | 50ms 超时无就绪 → 返回 0，约 50ms 已过 |
| 4 | `test_select_null_timeout` | NULL 超时 → 阻塞直到 fork 子进程写入 |
| 5 | `test_select_multifd` | 3 个 pipe，仅 1 个有数据 → 返回 1，仅对应 fd_set 置位 |
| 6 | `test_pselect_sigmask` | 阻塞 SIGINT，pselect 等待期间解除阻塞 → SIGINT 中断，errno=EINTR，mask 恢复 |
