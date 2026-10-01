# 架构评审 — Group 5: 信号

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/arch/x86_64/trap.c` (do_signal_delivery:647-780, SYS_signal:1900-1937, SYS_kill:1859-1898, SYS_sigprocmask:1939-1977, SYS_sigreturn:1978-2007), `kernel/tty/tty.c` (呼叫端)

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P0 | kill | `SYS_kill` 在全局任务列表上无锁遍历+写 `target->signal`，并发 exit 会写入释放后内存 | 已修复 |
| 2 | P0 | kill | `SYS_kill` 无目标进程权限检查，任意进程可发任意信号给任意进程 | 已修复 |
| 3 | P1 | signal | `SYS_sigreturn` 不验证 sigframe 的完整性（可接受，但应记录） | 待处理 |
| 4 | P1 | signal | `SYS_signal` 不验证用户提供的 `sa_restorer` / `sa_handler` 地址的合法性 | 待处理 |
| 5 | P2 | signal | `do_signal_delivery` NULL regs 路径中的 `do_exit` 理论上可到达，依赖 `signal_pending_fatal` 前置防护 | 待处理 |
| 6 | P2 | signal | sigframe 用户栈地址翻译失败时静默跳过信号投递 | 待处理 |
| 7 | P2 | signal | `signal_pending_fatal` 在 `do_signal_delivery` 之前被 `do_system_call` 调用（line 2119），多一次全扫描 | 待处理 |
| 8 | P2 | signal | 信号投递的 blocked mask 增量更新无内存序（`|=` 非原子） | 待处理 |

---

### [P0] 1. `SYS_kill` 无锁访问全局任务列表

- **位置**: `kernel/arch/x86_64/trap.c:1870-1881, 1889-1895`
- **现象**: `SYS_kill` 遍历全局任务列表找目标 task，然后写 `target->signal` 和读 `target->state`，全程无锁。如果目标在遍历后、写 signal 前 exit 了（TASK_ZOMBIE → reaper 回收），则写的是已释放内存
- **建议**: 为全局任务列表添加锁（见 Group 4 #3），或使用 PID 哈希表 + RCU。对 `target->signal` 的写入使用 `__sync_fetch_and_or` 原子操作
- **修复**: `c14d2a3` — 封装 `task_send_signal` 函数，在 `task_list_lock` 保护下遍历 + 使用 `__sync_fetch_and_or` 原子写 `target->signal`

### [P0] 2. `SYS_kill` 无权限检查

- **位置**: `kernel/arch/x86_64/trap.c:1859-1898`
- **现象**: 任何进程可向任意其他进程发任意信号（包括给内核线程发 SIGKILL）
- **建议**: 添加 UID/EUID 检查，或至少禁止向 `PF_KTHREAD` 任务发信号（内核线程不要意外被杀）
- **修复**: `c14d2a3` — 在 `task_send_signal` 中添加权限检查：禁止向 `PF_KTHREAD` 发信号、禁止向 init 进程（pid=1）发 SIGKILL/SIGTERM/SIGSTOP、`SYS_kill` 全部委托 `task_send_signal`

### [P1] 3. `SYS_sigreturn` 不验证 sigframe 完整性

- **位置**: `kernel/arch/x86_64/trap.c:1981-1999`
- **现象**: sigreturn 直接从用户栈读 sigframe，不作校验。虽然这不构成提权（用户已控制寄存器），但伪造 frame 可以设置任意 CS/SS/RFLAGS 供 iretq 使用。CS 的 DPL 校验由 iretq 硬件保证，但不能防止用户设置危险位（如 IF=0 屏蔽中断）
- **建议**: 添加最小验证（检查 CS 的 ring 3、RSP 在用户地址范围内）

### [P1] 4. `SYS_signal` 不验证 `sa_restorer` / `sa_handler` 地址

- **位置**: `kernel/arch/x86_64/trap.c:1930-1934`
- **现象**: `act->sa_handler` 和 `act->sa_restorer` 直接存储，不做地址范围检查。信号投递时直接跳转到 `sa_handler`，恢复时返回 `sa_restorer`
- **建议**: 至少验证地址在用户空间范围内（`< current->addr_limit`），虽然不阻止所有恶意行为

### [P2] 5. `do_signal_delivery(NULL)` 路径中的 `do_exit`

- **位置**: `kernel/arch/x86_64/trap.c:674-676`
- **现象**: NULL regs 路径中，如果致命信号（非 SIGCHLD/SIGURG 等无害信号）的 handler 是 SIG_DFL，执行 `do_exit`。调用方 `tty.c:279` 前已通过 `signal_pending_fatal()` (line 256) 检查，所以此路径实际不可达。但代码中的 `default: do_exit` 破坏了防御性编码的清晰性
- **建议**: 将 NULL regs 路径的 default 分支改为 `break`（不做任何操作），让外层 `signal_pending_fatal()` 处理

### [P2] 6. sigframe 栈地址翻译失败静默跳过

- **位置**: `kernel/arch/x86_64/trap.c:753-755`
- **现象**: `frame_phys = user_va_to_phys(user_pml4, new_rsp + 8); if (!frame_phys) { continue; }` — 如果用户栈页未映射（例如栈溢出），信号投递被静默跳过。信号留在 pending 状态，下次系统调用再次尝试投递
- **建议**: 添加地址映射修复（如 `handle_mmap_fault`）或在日志中记录跳过

### [P2] 7. `signal_pending_fatal` 被调用两次

- **位置**: `kernel/arch/x86_64/trap.c:2118-2119` 和 `tty.c:256,281`
- **现象**: `do_system_call` 末尾无条件调用 `do_signal_delivery(regs)` (line 2119)，而 `do_signal_delivery` 内部已迭代 pending signals 处理。`signal_pending_fatal` 单独扫描增加了 O(n) 开销
- **建议**: 让 `do_signal_delivery` 返回值指示是否有未处理的致命信号，避免重复扫描

### [P2] 8. 信号 blocked mask 增量更新无内存序

- **位置**: `kernel/arch/x86_64/trap.c:773`
- **现象**: `current->blocked |= (1ULL << (sig - 1));` — 在信号投递时为屏蔽当前信号，使用非原子 `|=` 操作。如果另一个 CPU 同时执行 `sigprocmask` 修改 `current->blocked`，可能丢失更新
- **建议**: 使用 `__sync_fetch_and_or`，或确保 `blocked` 只有当前任务所在的 CPU 能修改（在单队列调度下成立，但 `sigprocmask` 来自系统调用，可能在不同 CPU 上执行）
