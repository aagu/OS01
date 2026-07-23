# select/pselect 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 select() 和 pselect6() 系统调用，作为 fd_set → pollfd → do_poll_core → revents → fd_set 适配层。poll_table_t 从固定栈数组重构为动态分配，提取 do_poll_core() 供 poll 和 select 共享。

**Architecture:** 3 层——libc wrapper (SYS_select=50 / SYS_pselect6=51) → kernel/fs/select.c (fd_set↔pollfd 转换 + 用户内存安全) → kernel/fs/poll.c do_poll_core (共享轮询循环)。

**Tech Stack:** C11, OS01 内核 (x86_64), clang, QEMU

**Design spec:** `docs/superpowers/specs/2026-07-23-select-design.md`

## Global Constraints

- 所有内核头文件修改必须同步更新 `test/include/` 镜像
- 内核 API 签名对用户指针使用 `void *`（匹配 `trap.c` 现有模式）
- `fd_set` 128 字节（`long __fds_bits[16]` = `kernel_fd_set { uint64_t __bits[16] }`）
- `nfds==0 && timeout==NULL` → `-ENOSYS`（wait_queue_sleep 无信号唤醒）
- 时间验证：`tv_sec > 100000000` 拒绝（溢出保护），无 `< 0` 检查（uint64_t）
- pselect6 mask 原子性：swap 在 `do_pselect6` 中，信号检查由 `do_poll_core` 负责
- 内核和 libc 的 Makefile 均使用 `$(wildcard ...)` 自动发现源文件，无需修改

---

### Task 1: poll_table_t 重构为动态 entries

**Files:**
- Modify: `kernel/include/kernel/poll.h`
- Modify: `test/include/kernel/poll.h`
- Modify: `kernel/fs/poll.c`

**Interfaces changed:**
- `poll_table_t`：`entries[16]` → `*entries + max_entries`
- `poll_table_setup`：返回 `int`（0 成功，`-ENOMEM` 失败），新增 `max_entries` 参数
- 新增 `poll_table_destroy(pt)` — `kfree(pt->entries)`
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
- `int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int timeout_val, poll_table_t *pt)` — 不触碰用户内存的纯轮询循环
- `do_poll()` 重构为包装器

- [ ] **Step 1: 在 `kernel/fs/poll.c` 中提取 `do_poll_core`**
  1. 将现有 `do_poll` 循环体（line 204-282）——包括首次信号检查、扫描、sleep、超时检查——提取为新的 `do_poll_core` 函数
  2. 契约：调用者提供 kfds、pt（已初始化）；调用者在 `poll_table_setup` 之前已做首次信号检查
  3. 删除 `do_poll_core` 内部对 `user_fds` 的引用（不进行用户内存复制）
  4. `do_poll_core` 返回 -EINTR / 0 / 正数计数

- [ ] **Step 2: 重构 `do_poll()` 为包装器**
  1. nfds==0 行为：`nfds==0 && timeout<=0` → return 0；其他 → 继续（让 do_poll_core 处理睡眠/超时）
  2. `nfds > 0` 时 memcpy user_fds → 栈 kfds，初始化 revents
  3. `poll_table_setup(pt_ptr, (nfds==0) ? 1 : nfds)`，检查 NULL 返回
  4. 首次信号检查（`poll_table_setup` 之前）
  5. 调用 `do_poll_core(kfds, nfds, timeout, pt)`
  6. `ret >= 0 && nfds > 0` → copy revents 返回给 user_fds
  7. `poll_table_destroy(pt_ptr)`

- [ ] **Step 3: 在 `kernel/include/kernel/poll.h` 中添加 `do_poll_core` 声明**

- [ ] **Step 4: 修复 do_poll 中 nfds==0 且 timeout>0 的处理**
  1. 删除现有 `if (nfds == 0) return 0;`（line 186-187）
  2. nfds==0 路径的 kfds 数组分配 0 大小，`poll_table_setup(pt_ptr, 1)`
  3. 验证：`poll(0, NULL, 0, 500)` → 超时，返回 0（而非立即返回）

---

### Task 3: 新增 `kernel/include/kernel/select.h`

**Files:**
- New: `kernel/include/kernel/select.h`

**Interfaces produced:**
- `kernel_fd_set { uint64_t __bits[16] }` — 不同于 libc 的 `fd_set`（避免命名冲突）
- `sigset_t` typedef — `typedef unsigned long sigset_t`
- `struct pselect6_sigmask { const void *ss; size_t ss_len; }` — pselect6 打包结构体（内核侧定义）
- `FD_SETSIZE` (1024)
- `do_select()`、`do_pselect6()` 原型（用户指针用 `void *`）
- `kern_fd_*` 内联位操作函数

- [ ] **Step 1: 写入头文件**
  1. Include guard + `#include <stdint.h>`
  2. `typedef unsigned long sigset_t`
  3. `struct pselect6_sigmask` — 用 `const void *ss`（避免依赖 `<signal.h>`）
  4. `kernel_fd_set` typedef
  5. `FD_SETSIZE 1024`
  6. `kern_fd_zero`、`kern_fd_set`、`kern_fd_clr`、`kern_fd_isset` 内联函数（含边界检查）
  7. `do_select`、`do_pselect6` 原型

---

### Task 4: 新增 `kernel/fs/select.c`

**Files:**
- New: `kernel/fs/select.c`

**Interfaces produced:**
- `do_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout_tv)` → int64_t
- `do_pselect6(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout_ts, const void *sigmask_packed)` → int64_t

- [ ] **Step 1: 实现 `do_select()`**
  1. 验证 nfds（< 0 → `-EINVAL`，> FD_SETSIZE → `-EINVAL`）。nfds==0 特殊路径见 step 3
  2. 首次信号检查（`poll_table_setup` 之前）：`current->signal & ~current->blocked` → `-EINTR`
  3. `addr_limit` 校验 + `memcpy`：3×`fd_set`（用户 `void *` → 内核 `kernel_fd_set kr/kw/ke`）。NULL 参数 → 跳过
  4. `addr_limit` 校验 + `memcpy`：用户 `timeval` → `struct timeval ktv`。验证：`tv_sec > 100000000` → `-EINVAL`，`tv_usec >= 1000000` → `-EINVAL`。NULL → timeout=-1
  5. 转换 `timeval → ms`：`(int)(tv_sec * 1000 + (tv_usec + 999) / 1000)`
  6. `kmalloc(nfds * sizeof(struct pollfd))` + fd_set → pollfd 转换：对每个 fd 0..nfds-1，如果 `kern_fd_isset(fd, r/w/e)` → 设置 events。events 有 POLLIN、POLLOUT、POLLPRI 对应位
  7. `poll_table_setup(&pt, (nfds==0) ? 1 : nfds)`，检查返回码
  8. 调用 `do_poll_core(pollfd_ptr, nfds, ms, &pt)`
  9. 如果 `ret < 0` → cleanup + 返回错误
  10. 反向映射：`memset(kr/kw/ke, 0, 128)` → 遍历 pollfd[]，`revents & 对应位` → `kern_fd_set(fd, &kr/kw/ke)`，计数
  11. `memcpy` kr/kw/ke → 用户指针（`readfds/writefds/exceptfds`），NULL 跳过
  12. `kfree(pollfd_ptr)`、`poll_table_destroy(&pt)`，返回 count

- [ ] **Step 2: 实现 `do_pselect6()`**
  1. 解包 sigmask_packed（如非 NULL）：`addr_limit` 校验 → memcpy `struct pselect6_sigmask` → 验证 `ss_len == sizeof(sigset_t)` → `addr_limit` 校验 + memcpy `*sigmask`（如非 NULL）
  2. sigmask 交换：`old_blocked = current->blocked; if sigmask_ptr: current->blocked = *sigmask_ptr`
  3. 从 `timespec` 转换 timeout（`(tv_sec*1000 + (tv_nsec+999999)/1000000)`）
  4. 调用 `do_select` 的步骤 1-9（同上，timespec 替代 timeval）
  5. 恢复：`if sigmask_ptr: current->blocked = old_blocked`
  6. 返回

- [ ] **Step 3: nfds==0 特殊路径**
  1. nfds==0 且 timeout 是 `{0,0}` → 立即返回 0
  2. nfds==0 且 timeout 是 NULL → 返回 `-ENOSYS`（无法用信号唤醒）
  3. nfds==0 且 timeout > 0 → `poll_table_setup(&pt, 1)` → `do_poll_core(NULL, 0, ms, &pt)`（休眠到超时，返回 0）

---

### Task 5: 注册 syscall 分发

**Files:**
- Modify: `kernel/include/uapi/syscall.h`
- Modify: `kernel/arch/x86_64/trap.c`

- [ ] **Step 1: `kernel/include/uapi/syscall.h` — 添加 `SYS_pselect6`**
  ```c
  #define SYS_pselect6  51
  ```

- [ ] **Step 2: `kernel/arch/x86_64/trap.c` — 添加两个 syscall 的 dispatch**
  1. `case SYS_select:`（line 2091）— 将 `-ENOSYS` stub 替换为：
     ```c
     case SYS_select: {
         regs->rax = do_select((int)regs->rdi,
                               (void *)regs->rsi, (void *)regs->rdx,
                               (void *)regs->r10, (void *)regs->r8);
         break;
     }
     ```
  2. `case SYS_ppoll:`（line 2087）— 保留为 `-ENOSYS`（stub）
  3. 在 `case SYS_select:` 之后添加 `case SYS_pselect6:`:
     ```c
     case SYS_pselect6: {
         regs->rax = do_pselect6((int)regs->rdi,
                                 (void *)regs->rsi, (void *)regs->rdx,
                                 (void *)regs->r10, (void *)regs->r8,
                                 (const void *)regs->r9);
         break;
     }
     ```
  4. 添加 `#include <kernel/select.h>`（如尚未包含）
  5. 将 `syscall_names` 数组扩展至 52，添加 `[50] = "select"`，`[51] = "pselect6"`

---

### Task 6: 新增 `libc/include/sys/select.h`

**Files:**
- New: `libc/include/sys/select.h`

- [ ] **Step 1: 写入 libc 头文件**
  1. Include guard + `#include <sys/types.h>`（`fd_set`）、`<sys/time.h>`（`timeval`/`timespec`）、`<signal.h>`（`sigset_t`）
  2. `FD_SETSIZE 1024`
  3. `FD_BITPOS` / `FD_IDX` 辅助宏
  4. `FD_ZERO`、`FD_SET`、`FD_CLR`、`FD_ISSET` 内联函数（含边界检查 `(unsigned)fd < FD_SETSIZE`）
  5. `select()` 和 `pselect()` 原型（标准 POSIX 签名）

---

### Task 7: 新增 `libc/unistd/select.c`

**Files:**
- New: `libc/unistd/select.c`

- [ ] **Step 1: 实现 `select()` wrapper**
  1. 签名：`int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)`
  2. `int64_t ret = syscall6(SYS_select, nfds, r, w, e, tv, 0)`
  3. `ret < 0` → `{ errno = -ret; return -1; }`
  4. return `(int)ret`

- [ ] **Step 2: 实现 `pselect()` wrapper**
  1. 内联定义 `struct pselect6_sigmask { const sigset_t *ss; size_t ss_len; }`（与内核 select.h 中的布局一致）
  2. 签名：`int pselect(int nfds, fd_set*r, fd_set*w, fd_set*e, const struct timespec*ts, const sigset_t*sigmask)`
  3. 填充 packed：`{.ss=sigmask, .ss_len=sizeof(sigset_t)}`
  4. `int64_t ret = syscall6(SYS_pselect6, nfds, r, w, e, ts, &packed)`
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

- [ ] **Step 1: `make clean && make`** — 验证零错误编译
- [ ] **Step 2: `make run`** — 启动 QEMU，验证 shell 启动正常
- [ ] **Step 3: 运行 `systest`** — 验证所有现有 test_select 用例 + poll 用例全部通过
- [ ] **Step 4: 修复任何失败的测试**，迭代至 10/10 select + 8/8 poll pass
