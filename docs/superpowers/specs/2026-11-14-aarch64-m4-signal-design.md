# AArch64 M4.14：原生信号与 `sigreturn` 闭环设计规范

**日期**：2026-11-14
**里程碑**：AArch64 Phase 4 — M4.14（`SYS_kill` / `SYS_signal` / `SYS_sigprocmask` / `SYS_sigreturn` 与 EL0 信号投递闭环）
**前置依赖**：
- M4.7 原生 SVC 分派表（`kernel/syscall/dispatch.c`、`kernel/arch/aarch64/intr/syscall.c`）
- M4.8–M4.11 匿名虚拟内存、`fork` + 写时复制（COW）与跨界拷贝准备（`user_vm_prepare_copy` / `copy_to_user_ft` / `copy_from_user_ft`）
- M4.12 进程树与 `SYS_waitpid`（`do_waitpid` / `blocker_wait` 可被未屏蔽信号唤醒并返回 `-EINTR`）
- M4.13 原生 `SYS_exec` 与用户栈参数传递（`arch_syscall_exec`）

---

## 1. 背景与目标

在 M4.12（`SYS_waitpid`）与 M4.13（`SYS_exec`）闭环后，AArch64 已具备 `fork() + exec() + waitpid()` 进程生命周期三原语，且 `kernel/sched/core.c` 的 `blocker_wait` 已支持被未屏蔽信号打断并返回 `-EINTR`。但信号子系统在 AArch64 上仍存在以下缺口：

1. **信号发送原语未编入 AArch64**：`kernel/sched/signal.c`（`task_send_signal` / `signal_pgrp`）尚未加入 `ARCH=aarch64` 的 `kernel/Makefile` 构建列表与 `SCHED_COMMON_COMPILE_TUS`。
2. **四个信号系统调用仍困在 x86 专有文件**：`SYS_kill`（38）、`SYS_signal`（39）、`SYS_sigprocmask`（42）、`SYS_sigreturn`（43）位于 `kernel/arch/x86_64/syscall/sys_proc.c` 的 `#ifndef OS01_SYSCALL_A64_SUBSET` 分支内，而这四个入口本身完全不依赖 x86 寄存器布局。
3. **AArch64 缺少 EL0 信号投递与 `sigreturn` 栈帧恢复**：AArch64 尚未定义用户栈信号帧 `struct aarch64_sigframe`，尚未实现返回 EL0 前的 `aarch64_do_signal_delivery(pt_regs_t *regs)` 与 `arch_syscall_sigreturn(void *arch_frame)`，且 `exec` 成功后尚未将已注册的信号处理函数重置为 `SIG_DFL`。

**M4.14 目标**：
1. 在 AArch64 编入 `kernel/sched/signal.c`，并将 `SYS_kill`（38）、`SYS_signal`（39）、`SYS_sigprocmask`（42）、`SYS_sigreturn`（43）提升至 `kernel/syscall/sys_native.c` 与 `kernel/syscall/dispatch.c` 双架构共享区（AArch64 原生共享系统调用由 11 项扩展至 **15 项**）。
2. 在 AArch64 `user_task_prepare` 中显式初始化 `tsk->addr_limit = AARCH64_USER_LIMIT`，并在 `sys_native_signal` / `sys_native_sigprocmask` 的用户态写回路径使用跨架构 `syscall_prepare_user_write` + `copy_to_user_ft`，确保 `fork()` 后的 COW 用户栈变量与惰性匿名页均可安全写回。
3. 定义 AArch64 用户态信号栈帧 `struct aarch64_sigframe`（280 字节），实现 `aarch64_do_signal_delivery(pt_regs_t *regs)`（严格遵循 `task->signal` 用 `1ULL << sig`、而 `task->blocked` 用 BSD `1ULL << (sig - 1)` 的既有位编码约定，覆盖 `SIG_IGN`、`SIG_DFL` 忽略/终止、自定义 `sa_handler` + `sa_restorer` 跳板入栈）与 `arch_syscall_sigreturn(void *arch_frame)`（含 `(frame.regs.spsr_el1 & ~0xf0000000ULL) == AARCH64_USER_TASK_PSTATE` 严格校验，防止伪造帧提权或篡改 DAIF）。
4. 在 `arch_syscall_exec` 与 `aarch64_user_task_exec` 的原子提交区重置 `t->sighand` 为 `SIG_DFL`（保留 `t->blocked` 与 `t->signal`）。
5. 采用精简验证策略：最小化复用既有宿主单测、生产镜像静态审计与单一紧凑的 QEMU 内核自测 `aarch64_user_signal`。

---

## 2. 范围与非目标

### 2.1 范围内（In Scope）
- `kernel/Makefile`：`ARCH=aarch64` 的 `KERNEL_C_SOURCES` 与 `SCHED_COMMON_COMPILE_TUS` 加入 `sched/signal.c`。
- `kernel/syscall/dispatch.c` 与 `kernel/syscall/sys_native.c`：上提 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` 到双架构共享区，并从 `kernel/arch/x86_64/syscall/sys_proc.c` 移除对应分支。
- `kernel/include/arch/aarch64/syscall.h`：定义 `struct aarch64_sigframe`（含 `_Static_assert` 280 字节校验）并声明 `aarch64_do_signal_delivery`。
- `kernel/arch/aarch64/cpu/user_task.c` 与 `kernel/arch/aarch64/intr/syscall.c`：
  - `user_task_prepare` 设置 `tsk->addr_limit = AARCH64_USER_LIMIT`；
  - `arch_syscall_exec` 与 `aarch64_user_task_exec` 提交区重置 `t->sighand` 为 `SIG_DFL`；
  - 实现 `aarch64_do_signal_delivery(pt_regs_t *regs)` 与 `arch_syscall_sigreturn(void *arch_frame)`；
  - 在 `aarch64_user_syscall` 写回 `regs->x0` 之后、`AARCH64_USER_SVC_YIELD` 写回 `regs->x0 = 0` 之后以及 `aarch64_user_first_entry` 首次进入 EL0 前调用 `aarch64_do_signal_delivery`。
- 精简测试与审计：`test_syscall_dispatch.c`、`test_compat_linux.c`、`test_nofile_limits.c`、`test_waitpid.c`、`intr_dispatch_audit.py` 及第 27 项 AArch64 内核自测 `aarch64_user_signal`。

### 2.2 非目标（Out of Scope）
- **不扩展 `SA_SIGINFO` / `ucontext_t` / 备用信号栈（`sigaltstack`）**：与 x86_64 保持相同的轻量 `struct sigaction`（`sa_handler`、`sa_flags`、`sa_restorer`、`sa_mask`）契约。
- **不修改 EL0 硬件异步中断（`aarch64_el0_irq`）的不调度契约**：EL0 下的周期性抢占与异步中断返回信号投递留待后续专门的抢占演进处理；当前通过系统调用返回点、协作让出点与首次进入 EL0 入口点完成全部确定性信号投递（含唤醒阻塞中的 `SYS_waitpid` 返回 `-EINTR` 后投递信号）。
- **不引入 FP/SIMD 上下文保存**：AArch64 当前禁用 EL0 FP/SIMD（`CPACR_EL1.FPEN = 0`），信号帧仅保存 34 个通用/系统异常寄存器（`pt_regs_t`）与 `blocked` 掩码。

---

## 3. 详细设计

### 3.1 跨架构信号系统调用共享（`sys_native.c` & `dispatch.c`）

在 `kernel/include/arch/syscall.h` 与 `kernel/syscall/dispatch.c` 中声明并注册四个共享原生入口：

```c
int64_t sys_native_kill(syscall_ctx_t *ctx);
int64_t sys_native_signal(syscall_ctx_t *ctx);
int64_t sys_native_sigprocmask(syscall_ctx_t *ctx);
int64_t sys_native_sigreturn(syscall_ctx_t *ctx);
```

`kernel/syscall/dispatch.c` 的双架构共享区扩展至 15 项：
- `SYS_exit`、`SYS_getpid`、`SYS_getppid`、`SYS_clock_gettime`
- `SYS_brk`、`SYS_mmap`、`SYS_mprotect`、`SYS_munmap`
- `SYS_fork`、`SYS_waitpid`、`SYS_exec`
- `SYS_kill`、`SYS_signal`、`SYS_sigprocmask`、`SYS_sigreturn`

**位编码约定（与 `kernel/sched/signal.c` 及 x86_64 严格一致）**：
- `task->signal`（待决信号位图）：信号 `sig`（`1 <= sig < NSIG`）对应 **`1ULL << sig`**（例如 `task_send_signal`、`signal_pgrp`、`do_exit` 发送 `SIGCHLD` 均置位 `1ULL << sig`）。
- `task->blocked` 与 `sigset_t`（信号屏蔽字）：采用 BSD 编码，信号 `sig` 对应 **`1ULL << (sig - 1)`**（见 `user/systest/test_proc.c:345` 与 `kernel/arch/x86_64/intr/trap.c:1148`）。

各处理函数在 `kernel/syscall/sys_native.c` 中的契约（注意遵守 `syscall_boundary_audit.py`：代码与注释中均不得出现 `pt_regs` 或架构寄存器字段名）：

1. **`sys_native_kill(syscall_ctx_t *ctx)`**：
   - 提取 `int pid = (int)(int64_t)ctx->args[0]`、`int sig = (int)ctx->args[1]`。
   - 若 `sig < 1 || sig >= NSIG`，返回 `-EINVAL`。
   - `pid > 0`：调用 `task_send_signal(pid, sig)`。
   - `pid == 0`：调用 `signal_pgrp(current->pgrp, sig)`。
   - `pid == -1`：保持与原 `sys_proc.c:162-177` 完全一致的广播语义——在 `spin_lock_irqsave(&task_list_lock)` 下遍历 `init_task_union.task.list`，跳过 `t == current`、`t->flags & PF_KTHREAD` 及 `t->pid == 1`，对每个匹配任务执行 `t->signal |= (1ULL << sig); if (t->state == TASK_INTERRUPTIBLE) task_wake(t); matched++;`，解锁后返回 `matched > 0 ? 0 : -ESRCH`。
   - `pid < -1`：调用 `signal_pgrp(-pid, sig)`。

2. **`sys_native_signal(syscall_ctx_t *ctx)`**：
   - 提取 `int signum = (int)ctx->args[0]`、`const struct sigaction *act = (const struct sigaction *)(uintptr_t)ctx->args[1]`、`struct sigaction *oldact = (struct sigaction *)(uintptr_t)ctx->args[2]`。
   - 若 `signum < 1 || signum >= NSIG`，返回 `-EINVAL`。
   - 若 `act != NULL`：
     - 先调用 `syscall_check_user_range((uint64_t)(uintptr_t)act, sizeof(kact), false)`，失败返回 `-EFAULT`；
     - 再调用 `copy_from_user_ft(&kact, act, sizeof(kact))`，`< 0` 返回 `-EFAULT`。
   - 若 `oldact != NULL`：
     - 调用 `int rc = syscall_prepare_user_write((uint64_t)(uintptr_t)oldact, sizeof(kold))`（在 x86_64 上等价于 `syscall_check_user_range(..., true)`；在 AArch64 上同时为惰性匿名页或 `fork()` 后的 COW 栈页完成写前准备），若 `rc < 0` 返回 `rc`；
     - 构造当前 `current->sighand[signum]` 的内核副本 `kold`，通过 `copy_to_user_ft(oldact, &kold, sizeof(kold))` 写回用户态，`< 0` 则返回该错误码。
   - 若 `act != NULL && signum != SIGKILL && signum != SIGSTOP`：
     - 校验函数指针位于用户空间：若 `(uint64_t)(uintptr_t)kact.sa_restorer >= current->addr_limit || (uint64_t)(uintptr_t)kact.sa_handler >= current->addr_limit`，返回 `-EINVAL`（注：`SIG_DFL` = `0`、`SIG_IGN` = `1` 均小于 `addr_limit`）；
     - 将 `kact` 写入 `current->sighand[signum]`。
   - 返回 `0`。

3. **`sys_native_sigprocmask(syscall_ctx_t *ctx)`**：
   - 提取 `int how = (int)ctx->args[0]`、`const sigset_t *set = (const sigset_t *)(uintptr_t)ctx->args[1]`、`sigset_t *oldset = (sigset_t *)(uintptr_t)ctx->args[2]`。
   - 若 `oldset != NULL`：
     - 调用 `int rc = syscall_prepare_user_write((uint64_t)(uintptr_t)oldset, sizeof(sigset_t))`，`< 0` 返回 `rc`；
     - 将 `(sigset_t)current->blocked` 通过 `copy_to_user_ft(oldset, &kold, sizeof(kold))` 写回用户态。
   - 若 `set != NULL`：
     - 调用 `syscall_check_user_range((uint64_t)(uintptr_t)set, sizeof(sigset_t), false)` 与 `copy_from_user_ft(&kset, set, sizeof(kset))`，失败返回 `-EFAULT`；
     - 按 `how`（`SIG_BLOCK` = `0`、`SIG_UNBLOCK` = `1`、`SIG_SETMASK` = `2`）更新 `current->blocked`，其他 `how` 值返回 `-EINVAL`；
     - 强制清除不可屏蔽位：`current->blocked &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)))`。
   - 返回 `0`。

4. **`sys_native_sigreturn(syscall_ctx_t *ctx)`**：
   - 调用 `int64_t result = arch_syscall_sigreturn(ctx->arch_frame);`；
   - 若 `result == 0`，置 `ctx->suppress_writeback = true;`；
   - 返回 `result`。

---

### 3.2 AArch64 用户态信号栈帧 `struct aarch64_sigframe`

在 `kernel/include/arch/aarch64/syscall.h` 中定义：

```c
struct aarch64_sigframe {
    pt_regs_t regs;     /* 272 bytes (34 * 8): x0..x30, sp_el0, elr_el1, spsr_el1 */
    uint64_t  blocked;  /* 8 bytes: prior signal mask */
};

_Static_assert(sizeof(struct aarch64_sigframe) == 280,
               "aarch64_sigframe must be 280 bytes");
```

当信号 `sig` 触发用户自定义处理函数 `sa_handler` 时，内核在当前 `regs->sp_el0` 下方分配 16 字节对齐的 `struct aarch64_sigframe`：

```
高地址  +-------------------------------+  <- 原 regs->sp_el0
        | (0 或 8 字节对齐填充)         |
        +-------------------------------+  <- new_sp + 280
        | uint64_t  blocked             |  offset +272
        +-------------------------------+
        | pt_regs_t regs (x0..spsr_el1) |  offset +0 .. +271
低地址  +-------------------------------+  <- new_sp = (regs->sp_el0 - 280) & ~0xfUL
```

---

### 3.3 AArch64 信号投递 `aarch64_do_signal_delivery(pt_regs_t *regs)`

在 `kernel/arch/aarch64/cpu/user_task.c` 中实现 `void aarch64_do_signal_delivery(pt_regs_t *regs)`：

1. **前置门控**：
   - 若 `!regs` 或 `(regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) != AARCH64_SPSR_MODE_EL0T`，直接返回（严禁向 EL1 帧投递信号）。
   - 获取 `task_t *cur = arch_current_task()`；若 `!aarch64_user_task_is_user(cur)`，直接返回。

2. **待处理信号扫描与位对齐（对齐 `cur->signal` 的 `1ULL << sig` 与 `cur->blocked` 的 `1ULL << (sig - 1)`）**：
   - 读取待决位并计算未屏蔽掩码（`SIGKILL` 与 `SIGSTOP` 永不可屏蔽，有效信号位为 `1 .. NSIG - 1`）：
     ```c
     for (;;) {
         uint64_t sig_bits = (uint64_t)__atomic_load_n(&cur->signal, __ATOMIC_ACQUIRE);
         uint64_t unblocked_mask = ~(((uint64_t)cur->blocked) << 1) |
                                   (1ULL << SIGKILL) | (1ULL << SIGSTOP);
         uint64_t deliverable = sig_bits & unblocked_mask & ((1ULL << NSIG) - 2ULL);
         if (!deliverable)
             return;
         int sig = (deliverable & (1ULL << SIGKILL))
                   ? SIGKILL
                   : __builtin_ctzll(deliverable);
         __atomic_fetch_and(&cur->signal, ~(1ULL << sig), __ATOMIC_SEQ_CST);
         ...
     ```
   - 读取 `sighandler_t handler = cur->sighand[sig].sa_handler;`：
     - **分支 A（`handler == SIG_IGN`）**：忽略该信号，`continue` 继续扫描剩余信号。
     - **分支 B（`handler == SIG_DFL`）**：
       - 若 `sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU`：默认忽略，`continue`；
       - 否则（默认终止）：
         - 若 `cur->pid == 1`，保护 init 进程不死，`continue`；
         - 打印 `log_info("signal: task %ld killed by signal %d (default)\n", cur->pid, sig);`；
         - 调用 `aarch64_vm_native_leave()` 确保不遗留任何 VM 事务授权，随后调用 `arch_syscall_exit((uint64_t)sig)` 终止任务（`exit_code = sig`，父进程 `waitpid` 可观察到低字节信号值 `WIFSIGNALED(status) && WTERMSIG(status) == sig`）。
     - **分支 C（自定义 `handler`）**：
       - 检查 `void (*restorer)(void) = cur->sighand[sig].sa_restorer;`；
       - 若 `!restorer`：
         - 若 `cur->pid == 1`，`continue`；
         - 打印 `log_warn("signal: task %ld sig %d has no restorer, killing\n", cur->pid, sig);`；
         - 调用 `aarch64_vm_native_leave()` 并调用 `arch_syscall_exit((uint64_t)sig)`（与 x86_64 `trap.c:1212` `do_exit((uint64_t)sig)` 一致）；
       - 构造内核态临时帧：
         ```c
         struct aarch64_sigframe frame = {
             .regs    = *regs,
             .blocked = (uint64_t)cur->blocked,
         };
         uint64_t new_sp = (regs->sp_el0 - sizeof(struct aarch64_sigframe)) & ~UINT64_C(0xf);
         ```
       - 在临时开启的 VM 原生授权窗口内将 `frame` 写入用户栈（确保 `fork()` 后的 COW 栈页可自动分裂，且阻塞唤醒后 `g_auth_mm` 指向当前任务的 `mm`）：
         ```c
         aarch64_vm_native_enter(regs, SYS_sigreturn);
         ssize_t copy_rc = copy_to_user_ft((void *)(uintptr_t)new_sp, &frame, sizeof(frame));
         aarch64_vm_native_leave();
         if (copy_rc < 0) {
             __atomic_fetch_or(&cur->signal, (1ULL << sig), __ATOMIC_SEQ_CST);
             return;
         }
         ```
       - 更新信号屏蔽字（按 BSD `sig - 1` 编码屏蔽当前信号及 `sa_mask`，但永不屏蔽 `SIGKILL`/`SIGSTOP`）：
         ```c
         uint64_t add_mask = (1ULL << (sig - 1)) | (uint64_t)cur->sighand[sig].sa_mask;
         add_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
         cur->blocked |= (int64_t)add_mask;
         ```
       - 重定向异常帧至 `handler`：
         ```c
         regs->x0       = (uint64_t)sig;
         regs->x30      = (uint64_t)(uintptr_t)restorer;
         regs->sp_el0   = new_sp;
         regs->elr_el1  = (uint64_t)(uintptr_t)handler;
         regs->spsr_el1 = AARCH64_USER_TASK_PSTATE;
         return;
         ```

3. **三个调用挂接点与精确时序**：
   - **`aarch64_user_syscall(pt_regs_t *regs)`**（`kernel/arch/aarch64/intr/syscall.c`）：必须在 `aarch64_vm_native_leave()` **且** `if (!ctx.suppress_writeback) regs->x0 = (uint64_t)ret;` **之后**（函数最末尾）调用 `aarch64_do_signal_delivery(regs)`：
     ```c
     aarch64_vm_native_leave();

     if (!ctx.suppress_writeback)
         regs->x0 = (uint64_t)ret;

     aarch64_do_signal_delivery(regs);
     ```
     这样既保证保存进 `frame.regs.x0` 的是系统调用返回值 `ret`，又避免 `regs->x0 = (uint64_t)ret` 覆盖信号处理函数的首个参数 `regs->x0 = (uint64_t)sig`。
   - **`AARCH64_USER_SVC_YIELD`**（`kernel/arch/aarch64/cpu/user_task.c`）：在 `schedule()` 返回后，先写 `regs->x0 = 0;`，再调用 `aarch64_do_signal_delivery(regs);`，最后在返回前执行 `#if AARCH64_EL0_TEST if (g_arm_tick) { cntp_tval_el0_write(1); cntp_isb(); } #endif`：
     ```c
     case AARCH64_USER_SVC_YIELD:
         schedule();
         regs->x0 = 0;
         aarch64_do_signal_delivery(regs);
     #if AARCH64_EL0_TEST
         if (g_arm_tick) {
             cntp_tval_el0_write(1);
             cntp_isb();
         }
     #endif
         return;
     ```
   - **`aarch64_user_first_entry(void)`**（`kernel/arch/aarch64/cpu/user_task.c`）：在 `arch_aarch64_entry_prepare()` 之后、`aarch64_el0_restore(frame)` 之前调用 `aarch64_do_signal_delivery((pt_regs_t *)frame)`，闭合 `fork()` 后父进程在子进程首次进入 EL0 前即向其发送信号（如 `SIGTERM`/`SIGKILL`）的竞态窗口。

---

### 3.4 AArch64 `arch_syscall_sigreturn(void *arch_frame)`

在 `kernel/arch/aarch64/cpu/user_task.c` 中实现：

```c
int64_t arch_syscall_sigreturn(void *arch_frame)
{
    pt_regs_t *regs = (pt_regs_t *)arch_frame;
    if (!aarch64_user_frame_is_valid(regs))
        return -EFAULT;

    if ((regs->sp_el0 & UINT64_C(0xf)) != 0)
        return -EFAULT;

    struct aarch64_sigframe frame;
    if (!syscall_check_user_range(regs->sp_el0, sizeof(frame), false))
        return -EFAULT;
    if (copy_from_user_ft(&frame, (const void *)(uintptr_t)regs->sp_el0,
                          sizeof(frame)) < 0)
        return -EFAULT;

    /* Require exact AARCH64_USER_TASK_PSTATE (0x340: EL0t with D/A/F masked
     * and I enabled) in all non-NZCV bits; reject forged EL1 modes or DAIF edits. */
    if ((frame.regs.spsr_el1 & ~UINT64_C(0xf0000000)) != AARCH64_USER_TASK_PSTATE)
        return -EINVAL;

    *regs = frame.regs;
    task_t *cur = arch_current_task();
    cur->blocked = (int64_t)(frame.blocked &
                             ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1))));
    return 0;
}
```

- 每个正式 EL0 用户任务的 `spsr_el1` 除高 4 位 `NZCV`（`0xf0000000ULL`）外恒为 `AARCH64_USER_TASK_PSTATE`（`0x340`）。校验 `(frame.regs.spsr_el1 & ~0xf0000000ULL) != AARCH64_USER_TASK_PSTATE` 既允许合法保存帧（含任意 `NZCV` 条件码）无损恢复，又严格拒绝任何伪造 `EL1h`（`0x5`）或篡改 `DAIF` 中断掩码的恶意栈帧（返回 `-EINVAL`）。
- 当用户栈指针未 16 字节对齐、跨越未映射页或不可读时，返回 `-EFAULT`（`suppress_writeback` 保持 `false`，`-EFAULT` 写入 `regs->x0`，其余寄存器保持不变）。
- 成功时恢复全部通用寄存器、`sp_el0`、`elr_el1`、`spsr_el1` 与 `cur->blocked`，返回 `0` 并由 `sys_native_sigreturn` 置 `ctx->suppress_writeback = true`。

---

### 3.5 `user_task_prepare` 地址上限与 `exec` 信号重置

1. **`user_task_prepare`（`kernel/arch/aarch64/cpu/user_task.c`）**：
   - 在 `tsk->mm = aarch64_user_image_mm(owner->image);` 旁显式设置：
     ```c
     tsk->addr_limit = AARCH64_USER_LIMIT;
     ```
   - 这样无论是 `aarch64_user_task_spawn` 还是 `arch_syscall_fork` 创建的 AArch64 用户进程，其 `tsk->addr_limit` 均为 `0x80000000ULL`（2 GiB），`sys_native_signal` 对 `sa_handler >= current->addr_limit` 或 `sa_restorer >= current->addr_limit` 的校验可直接拦截任何越出 AArch64 用户地址空间的指针。

2. **`arch_syscall_exec` 与 `aarch64_user_task_exec`（`kernel/arch/aarch64/cpu/user_task.c`）**：
   - 在关中断原子提交区内（与 x86_64 `kernel/arch/x86_64/sched/exec.c` 对齐），将 `t->sighand` 全部重置为 `SIG_DFL`：
     ```c
     memset(t->sighand, 0, sizeof(t->sighand));
     ```
   - `t->blocked` 与 `t->signal` 在 `exec` 前后保持不变。

---

## 4. 精简测试与验证设计

遵循用户「尽量简化测试验证」的要求，不新建冗余宿主测试可执行文件，仅在既有测试与单自测固件中完成闭环：

### 4.1 宿主单测（复用既有 4 个文件）
- `hosttests/cases/test_syscall_dispatch.c`：为 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` 增加标记桩，并将 `SYS_kill`、`SYS_signal`、`SYS_sigprocmask`、`SYS_sigreturn` 加入 `OS01_SYSCALL_A64_SUBSET` 的 `shared[]` 列表（共 15 项）。
- `hosttests/cases/test_compat_linux.c` 与 `hosttests/cases/test_nofile_limits.c`：各补充 4 行链接桩（`test_compat_linux.c` 中 `sys_native_signal` 返回 `12345` 以保持 `rt_sigaction` 映射断言通过）。
- `hosttests/cases/test_waitpid.c`：由于该单测直接编译 `kernel/syscall/sys_native.c`，在其内补齐 `sys_native.c` 所需的 5 个受控桩函数：
  - `syscall_check_user_range`（感知 `g_uaccess_mode` 与空指针）；
  - `task_wake`（置 `t->state = TASK_RUNNING` 并计数）；
  - `task_send_signal`、`signal_pgrp`、`arch_syscall_sigreturn`；
  并新增 `test_sys_native_signal_syscalls()`，验证：
  - `SYS_kill`：非法信号返回 `-EINVAL`、`pid > 0` 转发 `task_send_signal`、`pid == 0` / `pid < -1` 转发 `signal_pgrp`、`pid == -1` 广播置位 `(1ULL << sig)` 并唤醒 `TASK_INTERRUPTIBLE` 任务；
  - `SYS_signal`：越界 `sa_handler >= addr_limit` 返回 `-EINVAL`、正常注册与读取 `oldact`、`SIGKILL` 不可捕获；
  - `SYS_sigprocmask`：`SIG_BLOCK` / `SIG_UNBLOCK` / `SIG_SETMASK` 掩码更新且 `SIGKILL`/`SIGSTOP` 不可被屏蔽；
  - `SYS_sigreturn`：`arch_syscall_sigreturn` 返回 `0` 时置 `ctx.suppress_writeback = true`，返回负数时 `suppress_writeback == false`。

### 4.2 静态审计（`qemutests/intr_dispatch_audit.py` & `test_intr_dispatch_audit.py`）
- `_A64_DISASM_SYMS` 加入 `sys_native_kill`、`task_send_signal`、`signal_pgrp`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn`、`arch_syscall_sigreturn`、`aarch64_do_signal_delivery`。
- `_A64_VM_EDGES` 增加 3 条关键边：
  - `sys_native_kill -> task_send_signal`
  - `sys_native_sigreturn -> arch_syscall_sigreturn`
  - `aarch64_user_syscall -> aarch64_do_signal_delivery`

### 4.3 单一紧凑 QEMU 内核自测 `aarch64_user_signal`（第 27 项 late selftest）
- 新增单个汇编固件 `kernel/selftest/fixtures/aarch64_user_signal.S` 与驱动 `kernel/selftest/test_aarch64_user_signal.c`，在**一次**进程树运行中端到端验证全部 M4.14 特性：
  1. **入口分流（支持 `exec` 重置验证）**：
     - `_start` 检查 `x0 == 2`（`argc == 2` 表示 `fork` 后经 `SYS_exec("/bin/user_signal", {"/bin/user_signal", "child", NULL}, NULL)` 进入的新镜像子进程）：若 `x0 == 2`，直接调用 `SYS_kill(getpid(), SIGUSR1)` 向自身发送 `SIGUSR1`（由于 `SYS_exec` 已将继承自父进程的 `sigusr1_handler` 重置为 `SIG_DFL`，子进程立即以默认信号终止状态 `status == SIGUSR1` = `10` 退出；若未重置而意外返回则以 `0xee` 退出）。
  2. **负向边界（父进程 `argc == 0`）**：
     - `SYS_signal(SIGUSR1, act_with_kernel_handler, NULL)` 返回 `-EINVAL`；
     - `SYS_sigreturn` 传入栈上伪造 `spsr_el1 = 0x5`（EL1h）的 `aarch64_sigframe` 返回 `-EINVAL`；
     - `SYS_sigreturn` 传入未对齐 `sp_el0` 返回 `-EFAULT`。
  3. **`sigprocmask` 阻塞与解阻塞投递 + 自定义 `sa_handler` / `sa_restorer` / `SYS_sigreturn` 完整恢复**：
     - 注册 `SIGUSR1` 自定义 handler（校验 `x0 == SIGUSR1` 且 `(sp & 0xf) == 0`，将全局计数递增，并将栈帧中保存的 `frame.regs.x19` 改为魔数 `0x13572468` 后 `ret` 到 `sa_restorer` 执行 `SYS_sigreturn`）；
     - 先用 `SYS_sigprocmask(SIG_BLOCK, &mask, NULL)` 屏蔽 `SIGUSR1`（`mask = 1ULL << (SIGUSR1 - 1)`），调用 `SYS_kill(getpid(), SIGUSR1)`，断言 handler **未**触发；
     - 再调用 `SYS_sigprocmask(SIG_UNBLOCK, &mask, &oldmask)` 解屏蔽，断言返回后 handler **已触发恰好 1 次**、`SYS_sigprocmask` 返回值 `x0 == 0` 未被破坏，且 `x19 == 0x13572468`（保持 `SIGUSR1` handler 处于已安装状态进入下一步）。
  4. **`fork` 继承信号表 + `SYS_exec` 重置 `sighand` 为 `SIG_DFL` + 默认致命信号终止 + `waitpid` 回收**：
     - 父进程调用 `SYS_fork()`：
       - 子进程（继承了 `SIGUSR1` 的 `sigusr1_handler`）在栈上构造 `argv = { "/bin/user_signal", "child", NULL }`（`argc = 2`）并调用 `SYS_exec("/bin/user_signal", argv, NULL)`；新镜像入口（`x0 == 2`）向自身发送 `SIGUSR1`，因 `exec` 重置为 `SIG_DFL` 而以 `exit_code == 10`（`SIGUSR1`）终止；
       - 父进程调用 `SYS_waitpid(child_pid, &status, 0)`，断言返回值为 `child_pid` 且 `status == SIGUSR1`（`10`）。
  5. **内核侧零泄漏断言**：
     - 父进程以 `SYS_exit(0x66)` 退出；内核驱动断言观察到子进程 `exit_code == SIGUSR1`（`10`）与父进程 `exit_code == (0x66 << 8)`、`aarch64_pt_published_root_count() == 1`、`pmm_probe_4k_allocated_count() == alloc_before`。
