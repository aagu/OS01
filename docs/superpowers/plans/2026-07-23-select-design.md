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
- SMP 注释：`current_poll_wq` 和 `poll_deadline_jiffies` 全局变量在 SMP 下有竞态（已有限制，非本次引入）。实施时在 do_poll_core 附近标注
- `kfree(NULL)` 安全：OS01 的 slab `kfree` 已有 NULL 守卫（参见 commit 62199b9），所有 `kfree(pfds)` 路径在 pfds 有效时才调用——无需守卫

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
- `int64_t do_select_common(int nfds, kernel_fd_set *kr, kernel_fd_set *kw, kernel_fd_set *ke, struct pollfd *pfds, poll_table_t *pt, int64_t ms, void *ur, void *uw, void *ue)` — **仅在 select.c 中 static**，头文件中不声明（避免参数重复+签名不一致）
- `do_select()`、`do_pselect6()` 原型（用户指针用 `void *`）
- `kern_fd_*` 内联位操作函数 + 边界检查

- [ ] **Step 1: 写入头文件**
  1. Include guard + `#include <stdint.h>` + `#include <stddef.h>`（`size_t` 用于 `pselect6_sigmask.ss_len`）
  2. `typedef unsigned long sigset_t` —— 解决 `trap.c:42` 局部定义在 `select.c` 不可见的问题
  3. `struct pselect6_sigmask { const void *ss; size_t ss_len; }` —— `void*` 避免依赖 `<signal.h>`
  4. `typedef struct { uint64_t __bits[16]; } kernel_fd_set;`
  5. `#define FD_SETSIZE 1024`
  6. `kern_fd_zero`、`kern_fd_set`、`kern_fd_clr`、`kern_fd_isset` 内联函数（含 `(uint32_t)fd < FD_SETSIZE` 边界检查）
  7. `do_select`、`do_pselect6` 原型（用户指针用 `void *`）。
     （注意：`do_select_common` 不在此处——它是 select.c 中的 `static`，不暴露给其他翻译单元。）

---

### Task 4: 新增 `kernel/fs/select.c`

**Files:**
- New: `kernel/fs/select.c`

**Interfaces produced:**
- `static int64_t do_select_common(int nfds, kernel_fd_set *kr, kernel_fd_set *kw, kernel_fd_set *ke, struct pollfd *pfds, poll_table_t *pt, int64_t ms, void *ur, void *uw, void *ue)` — 内部共享（**仅在 select.c 中 static，头文件中不声明**）
- `static int64_t do_select_nofds(int64_t ms)` — nfds==0 共享路径（`do_select` 和 `do_pselect6` 共用）
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

- [ ] **Step 1: 实现 `do_select_nofds(ms)` — nfds==0 共享路径**
  1. `poll_table_t pt; if (poll_table_setup(&pt, 1) != 0) return -ENOMEM;`
  2. `int64_t ret = do_poll_core(NULL, 0, ms, &pt);`
  3. `poll_table_destroy(&pt);`
  4. `return (ret < 0) ? ret : 0;`

- [ ] **Step 2: 实现 `do_select()`**
  1. **nfds 验证优先**（`< 0 → -EINVAL`，`> FD_SETSIZE → -EINVAL`）
  2. **nfds==0 路径**（在首次信号检查之前——因无 fd 也无 poll_table 分配；需前移 timeval 的 copy+验证，与 do_pselect6 Step 4.1 对称）：
     ```c
     if (!timeout_tv) return -ENOSYS;
     struct timeval ktv;
     if ((uint64_t)timeout_tv + sizeof(ktv) > current->addr_limit) return -EFAULT;
     memcpy(&ktv, timeout_tv, sizeof(ktv));
     if (ktv.tv_sec > INT32_MAX/1000) return -EINVAL;
     if (ktv.tv_usec >= 1000000) return -EINVAL;
     if (ktv.tv_sec == 0 && ktv.tv_usec == 0) return 0;
     int64_t ms = (int64_t)(ktv.tv_sec * 1000 + (ktv.tv_usec + 999) / 1000);
     return do_select_nofds(ms);
     ```
  3. 首次信号检查（`poll_table_setup` 前）
  4. `addr_limit` 校验（**base+length 模式**）+ `memcpy`：3×`fd_set` → `kernel_fd_set kr/kw/ke`。**NULL 参数跳过**（不校验 addr_limit）。**kr/kw/ke 声明时零初始化**：`kernel_fd_set kr = {0}, kw = {0}, ke = {0};` — 跳过 NULL 时可确保残留随机位不泄漏到 pollfd events 中
  5. `addr_limit` 校验 + `memcpy`：用户 `timeval` → `struct timeval ktv`。验证：`tv_sec > INT32_MAX/1000` → `-EINVAL`，`tv_usec >= 1000000` → `-EINVAL`。**NULL → ms = -1**
  6. 非 NULL timeout 转换：`int64_t ms = (int64_t)(tv_sec * 1000 + (tv_usec + 999) / 1000);`
  7. `kmalloc(nfds * sizeof(struct pollfd))` + fd_set → pollfd 转换：
     - 对每个 fd 0..nfds-1，`pollfds[i].fd = i; .events = 0; .revents = 0`
     - **fd 越界保护**：`if ((uint32_t)i >= current->files->max_fds) { pollfds[i].fd = -1; continue; }`
     - `kr` 有值且 `kern_fd_isset(i, &kr)` → `events |= POLLIN | POLLRDNORM`
     - `kw` 类似 → `events |= POLLOUT | POLLWRNORM`；`ke` → `events |= POLLPRI`
  8. `poll_table_setup(&pt, nfds)`；**失败则 `kfree(pfds)` 并返回 `-ENOMEM`**
  9. `ret = do_select_common(nfds, &kr, &kw, &ke, pfds, &pt, ms, readfds, writefds, exceptfds)`
  10. return ret

- [ ] **Step 3: 实现 `do_select_common()`（共享核心——仅 select.c 中 static）**
  1. `int64_t count = 0;` — **在函数顶部声明**（out label 跨作用域引用；不在反向映射块内声明）
  2. `int64_t ret = do_poll_core(pfds, nfds, ms, pt);`
  2. 如果 `ret < 0` → 转到 out（仅释放内存，**不**覆盖用户 fd_set）
  3. 反向映射：对每个 pollfd[0..nfds-1]（每 fd 计一次，非每集合计一次）：
     ```c
     memset(kr, 0, sizeof(kernel_fd_set));
     memset(kw, 0, sizeof(kernel_fd_set));
     memset(ke, 0, sizeof(kernel_fd_set));
     // kr/kw/ke 始终为调用方栈变量 -> 无需 NULL 检查；写回由 ur/uw/ue 控制（可为 NULL）
     int64_t count = 0;
     for (uint32_t i = 0; i < nfds; i++) {
         int fd = pfds[i].fd;
         if (fd < 0) continue;
         uint32_t r = pfds[i].revents;
         if (r == 0) continue;
         bool ready = false;
         if (r & (POLLIN | POLLRDNORM | POLLHUP | POLLERR))
             { kern_fd_set(fd, kr); ready = true; }
         if (r & (POLLOUT | POLLWRNORM | POLLERR))
             { kern_fd_set(fd, kw); ready = true; }
         if (r & (POLLPRI | POLLERR))
             { kern_fd_set(fd, ke); ready = true; }
         if (ready) count++;
     }
     ```
  4. **ret >= 0** 时将 kr/kw/ke memcpy 写回用户空间（ur/uw/ue 为 NULL 时跳过）；ret < 0 时跳过（fd_set 不改动）
  5. out：`kfree(pfds)`；`poll_table_destroy(pt)`；`return (ret < 0) ? ret : count;`

- [ ] **Step 4: 实现 `do_pselect6()`** —— ⚠️ sigmask 交换后**所有**错误路径必须恢复 blocked
  1. **nfds 验证 + nfds==0 路径**（在 sigmask swap **之前**——避免 restore 缺失）：
     - `nfds < 0 || nfds > FD_SETSIZE` → `-EINVAL`
     - nfds==0 且 timeout NULL → `-ENOSYS`
     - nfds==0 且 timeout 非 NULL → **预 copy timespec**（将 addr_limit 校验 + memcpy + 验证 + ms 转换前移到此处）：`addr_limit` 校验 → memcpy `kts` → 验证 `tv_sec`/`tv_nsec` → `ms = timespec_to_ms(kts)` → `{0,0}` → return 0；`>0` → `return do_select_nofds(ms)`
     （nfds==0 路径**完全独立**，不进入 sigmask swap 区域——Critical #2 方案 A）
  2. 解包 sigmask_packed（如非 NULL）：`addr_limit` 校验 → memcpy `struct pselect6_sigmask` → 验证 `ss_len == sizeof(sigset_t)` → 非 NULL 则 `addr_limit` 校验 + memcpy 到 `sigmask_kern`
  3. **sigmask swap + goto out 模式**：
     ```c
     uint64_t old_blocked = 0;
     bool mask_swapped = false;
     int64_t ret = 0;     // 默认值，所有 goto out 之前显式设置
     if (sigmask_ptr) {
         old_blocked = current->blocked;
         current->blocked = *sigmask_ptr;
         mask_swapped = true;
     }
     ```
  4. 首次信号检查（使用新 blocked mask）→ `ret = -EINTR; goto out;`
  5. **NULL timeout 处理**：`if (!timeout_ts) { ms = -1; goto after_timeout; }`
     `addr_limit` 校验 + `memcpy`：用户 `timespec` → `struct timespec kts`。验证失败 → `ret = -EFAULT; goto out;`（或 -EINVAL）
     验证内容：`tv_sec > INT32_MAX/1000` → `-EINVAL`，`tv_nsec >= 1000000000` → `-EINVAL`
     转换：`ms = (int64_t)(tv_sec * 1000 + (tv_nsec + 999999) / 1000000);`
     `after_timeout:`
  6. **fd_set 处理**：三组各自检查：
     ```c
     if (readfds) {
         if ((uint64_t)readfds + sizeof(kernel_fd_set) > current->addr_limit)
             { ret = -EFAULT; goto out; }
         memcpy(&kr, readfds, sizeof(kernel_fd_set));
     }
     // 类似 writefds, exceptfds
     ```
     （NULL 指针——跳过 addr_limit 和 memcpy，kr/kw/ke 为零填充的栈变量）
  7. `kmalloc` pollfd 数组 + 填充（同 do_select）。`kmalloc` 失败 → `ret = -ENOMEM; goto out;`
  8. `poll_table_setup(&pt, nfds)`；失败 → `kfree(pfds); ret = -ENOMEM; goto out;`
     （nfds 已确保 >0 在 Step 4.1，无需零值守卫）
  9. `ret = do_select_common(nfds, &kr, &kw, &ke, pfds, &pt, ms, readfds, writefds, exceptfds)`
  10. **out:** `if (mask_swapped) current->blocked = old_blocked; return ret;`

---

### Task 5: 注册 syscall 分发

**Files:**
- Modify: `kernel/include/uapi/syscall.h`
- Modify: `kernel/arch/x86_64/trap.c`

- [ ] **Step 1: `kernel/include/uapi/syscall.h` — 添加 `SYS_pselect6`**
  ```c
  #define SYS_pselect6  51
  ```

- [ ] **Step 2: `kernel/arch/x86_64/trap.c` — 移出局部 typedef + 添加 dispatch**
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
  | 1 | `test_select_basic()` | pipe 写 → select 报告可读；读空 → 超时 0 返回 0；验证非就绪 fd 位已清除 |
  | 2 | `test_select_write()` | 空 pipe → writable；填满 → 超时 0 返回 0 |
  | 3 | `test_select_timeout()` | 50ms 超时无 fd → 返回 0，耗时 ~50ms ±20ms |
  | 4 | `test_select_null_timeout()` | NULL timeout → fork 子进程，子进程 sleep 后写入，父进程 select 返回 1 |
  | 5 | `test_select_multifd()` | 3 pipes，只写 1 个 → select 返回 1，**仅该 fd 置位**；验证非就绪 fd 位已清除（`!FD_ISSET(unready_fd)`） |
  | 6 | `test_select_zero_timeout()` | `{0,0}` 非阻塞轮询 → 立即返回 0 |
  | 7 | `test_select_sleep()` | `select(0, NULL, NULL, NULL, &(struct timeval){0,50000})` → 返回 0，耗时 ~50ms |
  | 8 | `test_pselect_sleep()` | `pselect(0, NULL, NULL, NULL, &(struct timespec){0,50000000}, NULL)` → 返回 0（nfds==0+pselect 路径） |
  | 9 | `test_select_invalid_fd()` | nfds=1 的已关闭 fd → POLLNVAL 计入计数，无 fd_set 置位 |
  | 10 | `test_pselect_null_sigmask()` | pselect with sigmask=NULL → 与 select 行为相同 |
  | 11 | `test_pselect_bad_ss_len()` | pselect with ss_len=999 → errno=EINVAL |

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
- [ ] **Step 4: 修复任何失败的测试**，迭代至 11/11 select + 8/8 poll + 原有测试全部 pass
