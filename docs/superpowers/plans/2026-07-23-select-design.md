# select/pselect 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 select() 和 pselect6() 系统调用，作为 fd_set → pollfd → do_poll_core → revents → fd_set 适配层。poll_table_t 从固定栈数组重构为动态分配，提取 do_poll_core() 供 poll 和 select 共享。

**Architecture:** 3 层——libc wrapper (SYS_select=50 / SYS_pselect6=51) → kernel/fs/select.c (do_select, do_pselect6 各自处理 timeout 格式 + sigmask swap，之后调用 do_select_common) → kernel/fs/poll.c do_poll_core (共享轮询循环)。

**Tech Stack:** C11, OS01 内核 (x86_64), clang, QEMU

**Design spec:** `docs/superpowers/specs/2026-07-23-select-design.md`

## Global Constraints

- 所有内核头文件修改必须同步更新 `test/include/` 镜像
- 内核 API 签名对用户指针使用 `void *`（匹配 `trap.c` 现有模式）
- `do_poll_core` 的 `timeout_val` 参数类型为 `int64_t`（int32 在 `tv_sec*1000` 时溢出——24.8 天即超限）
- `fd_set` 128 字节（`long __fds_bits[16]` = `kernel_fd_set { uint64_t __bits[16] }`）
- `nfds==0 && timeout==NULL` → `-ENOSYS`（wait_queue_sleep 无信号唤醒）
- 时间验证：`tv_sec > INT32_MAX/1000` 拒绝（~24.8 天，防止 ms 转换正整数溢出）
- pselect6 mask 原子性：swap 在 `do_pselect6` 中，信号检查由 `do_poll_core` 负责
- 内核和 libc 的 Makefile 均使用 `$(wildcard ...)` 自动发现源文件，无需修改
- `addr_limit` 校验使用 base+length 模式（非仅 base）：`ptr + size > addr_limit → -EFAULT`
- nfds 参数在信号检查之前验证
- **重构后务必 `make clean`**：`poll_table_t` struct 布局改变，头文件无依赖跟踪

---

### Task 1: poll_table_t 重构为动态 entries

**Files:**
- Modify: `kernel/include/kernel/poll.h`
- Modify: `test/include/kernel/poll.h`
- Modify: `kernel/fs/poll.c`

**Interfaces changed:**
- `poll_table_t`：`entries[POLL_MAX_FDS]` → `poll_wait_entry_t *entries` + `int max_entries`
- `poll_table_setup`：返回 `int`（0 成功，`-ENOMEM` 失败），新增 `int max_entries` 参数
- 新增 `void poll_table_destroy(poll_table_t *pt)` — `kfree(pt->entries)`
- `poll_wait`：`POLL_MAX_FDS` → `pt->max_entries`

- [ ] **Step 1: 修改 `kernel/include/kernel/poll.h` — poll_table_t 结构体 + API**
  1. 将 `poll_wait_entry_t entries[POLL_MAX_FDS]` 改为 `poll_wait_entry_t *entries`
  2. 添加 `int max_entries` 字段
  3. 将 `poll_table_setup` 声明从 `void` 改为 `int poll_table_setup(poll_table_t *pt, int max_entries)`
  4. 添加 `void poll_table_destroy(poll_table_t *pt)` 声明
  5. 更新 `poll_wait` 注释：`POLL_MAX_FDS` → `pt->max_entries`

- [ ] **Step 2: 同步更新 `test/include/kernel/poll.h`**（与 kernel 镜像一致）

- [ ] **Step 3: 修改 `kernel/fs/poll.c` — 实现动态 setup/destroy + 更新 poll_wait**
  1. 重写 `poll_table_setup`：`kmalloc(max_entries * sizeof(poll_wait_entry_t))`，失败返回 `-ENOMEM`；设置 `pt->max_entries`；初始化每个 entry 节点
  2. 实现 `poll_table_destroy`：`kfree(pt->entries); pt->entries = NULL`
  3. 将 `poll_wait` 第 50 行从 `POLL_MAX_FDS` 改为 `pt->max_entries`
  4. 添加 `#include <kernel/slab.h>`（如尚未包含）

---

### Task 2: 提取 do_poll_core() 共享轮询循环

**Files:**
- Modify: `kernel/fs/poll.c`
- Modify: `kernel/include/kernel/poll.h`（添加声明）

**Interfaces produced:**
- `int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int64_t timeout_val, poll_table_t *pt)` — 不触碰用户内存的纯轮询循环。**timeout_val 为 `int64_t`**，避免 `tv_sec*1000` 溢出 int32
- `do_poll()` 重构为包装器

**⚠️ poll_table_destroy 覆盖要求**：重构后 `poll_table_setup` 内部有 kmalloc。`do_poll` 的**每个 return 路径必须调用 `poll_table_destroy`**。建议使用 goto 集中清理模式：

```c
int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout_val) {
    // ... validate ...
    poll_table_t pt;
    if (poll_table_setup(&pt, (nfds==0) ? 1 : POLL_MAX_FDS) != 0)
        return -ENOMEM;

    int64_t ret = do_poll_core(kfds, nfds, timeout_val, &pt);

    // 单点清理（无论成功、超时、信号中断）
    if (ret >= 0 && nfds > 0) {
        for (uint32_t i = 0; i < nfds; i++)
            user_fds[i].revents = kfds[i].revents;
    }
    poll_table_destroy(&pt);
    return ret;
}
```

这样 `do_poll_core` 内部的所有 `return`（超时、EINTR 等）都回到包装器中单点调用 `poll_table_destroy`。

- [ ] **Step 1: 在 `kernel/fs/poll.c` 中提取 `do_poll_core`**
  1. 签名：`int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int64_t timeout_val, poll_table_t *pt)`
  2. 内部使用 `int64_t timeout_val`：超时 tick 计算改为 `int64_t` 兼容
  3. 将现有 `do_poll` 循环体（line 204-282）——首次信号检查、扫描、sleep、超时检查、`poll_table_init`、`poll_table_cleanup`——提取为 `do_poll_core`
  4. 契约：调用者提供 kfds、pt（已通过 `poll_table_setup` 初始化）。调用者在 setup 之前已做首次信号检查
  5. 删除对 `user_fds` 的所有引用
  6. `do_poll_core` 返回 -EINTR / 0 / 正数计数。**pt 的 destroy 由调用者负责（不在 do_poll_core 中）**

- [ ] **Step 2: 重构 `do_poll()` 为包装器**
  1. 入口校验：`addr_limit`（base+length 模式）、`nfds > POLL_MAX_FDS → -EINVAL`
  2. nfds==0：`nfds==0 && timeout_val <= 0` → return 0；其他 → 继续（do_poll_core 管理睡眠/超时）
  3. `nfds > 0` 时 memcpy user_fds → 栈 `kfds[POLL_MAX_FDS]`，然后清零所有 revents
  4. **memcpy 后必须显式清零 revents**：`for (uint32_t i = 0; i < nfds; i++) kfds[i].revents = 0;`
  5. 首次信号检查（**`poll_table_setup` 之前**——预分配内存仍可返回 EINTR）
  6. `poll_table_setup(&pt, (nfds==0) ? 1 : POLL_MAX_FDS)`，检查 ret
  7. `ret = do_poll_core(kfds, nfds, timeout_val, &pt)`
  8. `ret >= 0 && nfds > 0` → copy revents 返回给 user_fds
  9. `poll_table_destroy(&pt)`（单点——所有 return 路径）
  10. return ret

- [ ] **Step 3: 在 `kernel/include/kernel/poll.h` 中添加 `do_poll_core` 声明**

- [ ] **Step 4: 修复 do_poll 中 nfds==0 且 timeout>0 的处理**
  1. 删除现有 `if (nfds == 0) return 0;`（line 186-187）
  2. nfds==0 路径：kfds 为局部空数组，`poll_table_setup(&pt, 1)`
  3. 验证：`poll(0, NULL, 0, 500)` → 超时，返回 0（而非立即返回）

---

### Task 3: 新增 `kernel/include/kernel/select.h`

**Files:**
- New: `kernel/include/kernel/select.h`

**Interfaces produced:**
- `kernel_fd_set { uint64_t __bits[16] }` — 内核侧 fd_set（不同于 libc 的 `fd_set`）
- `typedef unsigned long sigset_t` — 从 `trap.c:42` 移入此头文件
- `struct pselect6_sigmask { const void *ss; size_t ss_len; }` — pselect6 打包结构体
- `FD_SETSIZE` (1024)
- `int64_t do_select_common(int nfds, kernel_fd_set *kr, kernel_fd_set *kw, kernel_fd_set *ke, int64_t ms)` — do_select/do_pselect6 的共享核心（见 Task 4 Critical #2 修复）
- `do_select()`、`do_pselect6()` 原型（用户指针用 `void *`）
- `kern_fd_*` 内联位操作函数 + 边界检查

- [ ] **Step 1: 写入头文件**
  1. Include guard + `#include <stdint.h>` + `#include <stddef.h>`（`size_t` 用于 `pselect6_sigmask.ss_len`）
  2. `typedef unsigned long sigset_t` —— 解决 `trap.c:42` 局部定义在 `select.c` 不可见的问题
  3. `struct pselect6_sigmask { const void *ss; size_t ss_len; }` —— `void*` 避免依赖 `<signal.h>`
  4. `typedef struct { uint64_t __bits[16]; } kernel_fd_set;`
  5. `#define FD_SETSIZE 1024`
  6. `kern_fd_zero`、`kern_fd_set`、`kern_fd_clr`、`kern_fd_isset` 内联函数（含 `(uint32_t)fd < FD_SETSIZE` 边界检查）
  7. `do_select_common`、`do_select`、`do_pselect6` 原型

---

### Task 4: 新增 `kernel/fs/select.c`

**Files:**
- New: `kernel/fs/select.c`

**Interfaces produced:**
- `int64_t do_select_common(int nfds, kernel_fd_set *kr, kernel_fd_set *kw, kernel_fd_set *ke, int64_t ms)` — 内部共享函数（参见 Critical #2 去重修复）
- `int64_t do_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout_tv)`
- `int64_t do_pselect6(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout_ts, const void *sigmask_packed)`

**关键去重设计（Critical #2 修复）**：
`do_select` 和 `do_pselect6` 共享 ~70% 的代码（nfds 验证、fd_set memcpy、pollfd 转换、`do_poll_core`、反向映射、写回）。提取 `do_select_common` 后，两者各自只负责 timeout 格式解析 + sigmask swap：

```c
// 前置条件：kr/kw/ke 已从用户空间 memcpy，NULL 指针已跳过
// pfds 已 kmalloc + 填充（fd_set→pollfd），pt 已 poll_table_setup
// 此函数执行：do_poll_core → 反向映射 → memcpy 写回 → kfree/destroy
static int64_t do_select_common(int nfds,
    kernel_fd_set *kr, kernel_fd_set *kw, kernel_fd_set *ke,
    struct pollfd *pfds, poll_table_t *pt, int64_t ms,
    void *ur, void *uw, void *ue);
```

- [ ] **Step 1: 实现 `do_select()`**
  1. **nfds 验证优先**（`< 0 → -EINVAL`，`> FD_SETSIZE → -EINVAL`）。nfds==0 特殊路径 → 见 Step 3
  2. 首次信号检查（poll_table_setup 前）
  3. `addr_limit` 校验（**base+length 模式**：`(uint64_t)readfds + 128 > addr_limit → -EFAULT`）+ `memcpy`：3×`fd_set`（用户 `void *` → 内核 `kernel_fd_set kr/kw/ke`）。NULL 参数 → 跳过
  4. `addr_limit` 校验 + `memcpy`：用户 `timeval` → `struct timeval ktv`。验证：`tv_sec > INT32_MAX/1000` → `-EINVAL`（~24.8 天，防止 ms 溢出 int32），`tv_usec >= 1000000` → `-EINVAL`。NULL → timeout=-1
  5. 转换 `timeval → ms`：`int64_t ms = (int64_t)(tv_sec * 1000 + (tv_usec + 999) / 1000);`
  6. `kmalloc(nfds * sizeof(struct pollfd))` + fd_set → pollfd 转换：
     - 对每个 fd 0..nfds-1，设 `pollfds[i].fd = i`、`pollfds[i].events = 0`、`pollfds[i].revents = 0`
     - **fd 边界保护**（Medium #7）：`if ((uint32_t)fd < current->files->max_fds)` 仅在 fd 有效时才检查 `kern_fd_isset`
     - `kern_fd_isset(fd, kr) → events |= POLLIN | POLLRDNORM`
     - `kern_fd_isset(fd, kw) → events |= POLLOUT | POLLWRNORM`
     - `kern_fd_isset(fd, ke) → events |= POLLPRI`
  7. `poll_table_setup(&pt, (nfds==0) ? 1 : nfds)`，检查返回码
  8. `ret = do_select_common(nfds, &kr, &kw, &ke, pfds, &pt, ms, readfds, writefds, exceptfds)` —— 包含 do_poll_core + 反向映射 + 写回 + 清理
  9. 返回 ret

- [ ] **Step 2: 实现 `do_select_common()`（共享核心）**
  1. `ret = do_poll_core(pfds, nfds, ms, pt)`
  2. 如果 `ret < 0` → 转到 out（仅释放内存，**不**覆盖用户 fd_set）
  3. 反向映射：对每个 pollfd[0..nfds-1]：
     ```c
     memset(kr, 0, sizeof(kernel_fd_set));   // 清零后重建
     memset(kw, 0, sizeof(kernel_fd_set));
     memset(ke, 0, sizeof(kernel_fd_set));
     int64_t count = 0;
     for (uint32_t i = 0; i < nfds; i++) {
         int fd = pfds[i].fd;
         if (fd < 0) continue;
         uint32_t r = pfds[i].revents;
         if (r == 0) continue;
         // POLLERR 出现在所有三个集合，POLLHUP 在 readfds（Critical #4）
         if (kr && (r & (POLLIN | POLLRDNORM | POLLHUP | POLLERR))) {
             kern_fd_set(fd, kr); count++;
         }
         if (kw && (r & (POLLOUT | POLLWRNORM | POLLERR))) {
             kern_fd_set(fd, kw); count++;
         }
         if (ke && (r & (POLLPRI | POLLERR))) {
             kern_fd_set(fd, ke); count++;
         }
     }
     ```
  4. **成功时（ret >= 0）**将 kr/kw/ke memcpy 写回用户空间（non-NULL 指针写回）；失败时跳过（fd_set 不改动）
  5. out：`kfree(pfds)`、`poll_table_destroy(pt)`、return ret 或 count

- [ ] **Step 3: 实现 `do_pselect6()`**
  1. nfds 验证（同 do_select Step 1.1）
  2. 解包 sigmask_packed（如非 NULL）：`addr_limit` 校验 → memcpy `struct pselect6_sigmask` → 验证 `ss_len == sizeof(sigset_t)` → 非 NULL 则 `addr_limit` 校验 + memcpy `sigmask` 到 `sigmask_kern`
  3. sigmask swap：`old_blocked = current->blocked; if sigmask_ptr: current->blocked = *sigmask_ptr`
  4. 首次信号检查（使用新 blocked mask）
  5. `addr_limit` 校验 + `memcpy`：用户 `timespec` → `struct timespec kts`。验证：`tv_sec > INT32_MAX/1000` → `-EINVAL`，`tv_nsec >= 1000000000` → `-EINVAL`
  6. 转换 `timespec → ms`：`int64_t ms = (int64_t)(tv_sec * 1000 + (tv_nsec + 999999) / 1000000);`
  7. fd_set memcpy → kernel_fd_set
  8. `kmalloc` pollfd 数组并填充（同 do_select Step 1.6）
  9. `poll_table_setup`
  10. `ret = do_select_common(...)`
  11. 恢复 sigmask：`if sigmask_ptr: current->blocked = old_blocked`
  12. return ret

- [ ] **Step 4: nfds==0 特殊路径**
  1. nfds==0 且 timeout 是 `{0,0}` → 立即返回 0
  2. nfds==0 且 timeout 是 NULL → 返回 `-ENOSYS`（无法用信号唤醒）
  3. nfds==0 且 timeout > 0 → `poll_table_setup(&pt, 1)` → `do_poll_core(NULL, 0, ms, &pt)`（休眠到超时，返回 0；`poll_table_destroy` 在 do_poll_core 定义后由 do_select_common 处理）

**验证顺序（Medium #9）**：nfds 验证 → 首次信号检查 → addr_limit → 其余（无效参数应在信号检测前拒绝）。

---

### Task 5: 注册 syscall 分发

**Files:**
- Modify: `kernel/include/uapi/syscall.h`
- Modify: `kernel/arch/x86_64/trap.c`

- [ ] **Step 1: `kernel/include/uapi/syscall.h` — 添加 `SYS_pselect6`**
  ```c
  #define SYS_pselect6  51
  ```

- [ ] **Step 2: `kernel/arch/x86_64/trap.c` — s 移出局部 typedef + 添加 dispatch**
  1. 在 `#include` 区域（line 30 附近）添加 `#include <kernel/select.h>` —— 提供 sigset_t + do_select/do_pselect6 原型
  2. 删除 `trap.c:42` 的 `typedef unsigned long sigset_t;` —— 现在来自 `kernel/select.h`
  3. `case SYS_select:`（line 2091）—— 将 `-ENOSYS` stub 替换为：
     ```c
     case SYS_select: {
         regs->rax = do_select((int)regs->rdi,
                               (void *)regs->rsi, (void *)regs->rdx,
                               (void *)regs->r10, (void *)regs->r8);
         break;
     }
     ```
  4. `case SYS_ppoll:`（line 2087）—— 保留为 `-ENOSYS`（stub）
  5. 在 `case SYS_select:` 之后添加 `case SYS_pselect6:`:
     ```c
     case SYS_pselect6: {
         regs->rax = do_pselect6((int)regs->rdi,
                                 (void *)regs->rsi, (void *)regs->rdx,
                                 (void *)regs->r10, (void *)regs->r8,
                                 (const void *)regs->r9);
         break;
     }
     ```
  6. `syscall_names` 表：`[50] = "select"` 可能已存在，只需添加 `[51] = "pselect6"`；确保数组大小 ≥ 52

---

### Task 6: 新增 `libc/include/sys/select.h`

**Files:**
- New: `libc/include/sys/select.h`

- [ ] **Step 1: 写入 libc 头文件**
  1. Include guard + `#include <sys/types.h>`（`fd_set`）、`<sys/time.h>`（`timeval`/`timespec`）、`<signal.h>`（`sigset_t`）
  2. `#define FD_SETSIZE 1024`
  3. 辅助宏：`FD_BITPOS(fd)`、`FD_IDX(fd)`
  4. `FD_ZERO`、`FD_SET`、`FD_CLR`、`FD_ISSET` 内联函数（含 `(unsigned)fd < FD_SETSIZE` 边界检查）
  5. `select()` 和 `pselect()` 原型（标准 POSIX 签名）

---

### Task 7: 新增 `libc/unistd/select.c`

**Files:**
- New: `libc/unistd/select.c`

- [ ] **Step 1: 实现 `select()` wrapper**
  1. 签名：`int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)`
  2. `int64_t ret = syscall6(SYS_select, (uint64_t)(int64_t)nfds, (uint64_t)r, (uint64_t)w, (uint64_t)e, (uint64_t)tv, 0)`
  3. `ret < 0` → `{ errno = (int)(-ret); return -1; }`
  4. return `(int)ret`

- [ ] **Step 2: 实现 `pselect()` wrapper**
  1. 内联定义 `struct pselect6_sigmask { const sigset_t *ss; size_t ss_len; }`（与内核 select.h 中布局一致；libc 端不可 include uapi 头）
  2. 签名：`int pselect(int nfds, fd_set*r, fd_set*w, fd_set*e, const struct timespec*ts, const sigset_t*sigmask)`
  3. 填充 packed：`{.ss=sigmask, .ss_len=sizeof(sigset_t)}`
  4. `int64_t ret = syscall6(SYS_pselect6, (uint64_t)(int64_t)nfds, (uint64_t)r, (uint64_t)w, (uint64_t)e, (uint64_t)ts, (uint64_t)&packed)`
  5. 错误处理同上

---

### Task 8: 新增 systest 测试用例

**Files:**
- Modify: `user/systest.c`

- [ ] **Step 1: 添加 include + 辅助函数**
  1. `#include <sys/select.h>` + `#include <sys/time.h>`
  2. 辅助：`static int64_t time_ms(void)`（用 `gettimeofday`）

- [ ] **Step 2: 实现 10 个测试用例**

  | # | 函数 | 测试 |
  |---|------|------|
  | 1 | `test_select_basic()` | pipe 写 → select 报告可读；读空 → 超时 0 返回 0 |
  | 2 | `test_select_write()` | 空 pipe → writable；填满 → 超时 0 返回 0 |
  | 3 | `test_select_timeout()` | 50ms 超时无 fd → 返回 0，耗时 ~50ms ±10ms |
  | 4 | `test_select_null_timeout()` | NULL timeout → fork 子进程，子进程 sleep 后写入，父进程 select 返回 1 |
  | 5 | `test_select_multifd()` | 3 pipes，只写 1 个 → select 返回 1，仅该 fd 置位 |
  | 6 | `test_select_zero_timeout()` | `{0,0}` 非阻塞轮询 → 立即返回 0 |
  | 7 | `test_select_sleep()` | `select(0, NULL, NULL, NULL, &(struct timeval){0,50000})` → 返回 0，耗时 ~50ms |
  | 8 | `test_select_invalid_fd()` | nfds=1 的已关闭 fd → POLLNVAL 计入计数，无 fd_set 置位 |
  | 9 | `test_pselect_null_sigmask()` | pselect with sigmask=NULL → 与 select 行为相同 |
  | 10 | `test_pselect_bad_ss_len()` | pselect with ss_len=999 → errno=EINVAL |

- [ ] **Step 3: 注册测试 + 更新测试数**
  1. 扩展 `g_test_table[]` 增加 10 个条目
  2. 更新测试数（`KERNEL_TESTS` 或等效常量）

---

### Task 9: 构建 + 运行 + 迭代修复

**Files:**
- （构建或运行路径修复）

- [ ] **Step 1: `make clean && make`** — ⚠️ **必须 `make clean`**（`poll_table_t` struct 布局改变，`.o` 文件中的 sizeof/offset 会错误）
- [ ] **Step 2: `make run`** — 启动 QEMU，验证 shell 启动正常
- [ ] **Step 3: 运行 `systest`** — 验证所有现有 test_select 用例 + poll 用例（88/88）全部通过
- [ ] **Step 4: 修复任何失败的测试**，迭代至 10/10 select + 8/8 poll + 70 原有 = 88/88 pass
