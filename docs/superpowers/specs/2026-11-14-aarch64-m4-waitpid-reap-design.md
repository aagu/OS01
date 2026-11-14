# AArch64 M4.12：原生 `SYS_waitpid` 与父子退出 / ZOMBIE 回收闭环设计

## 1. 目标与范围

在 AArch64 单核（BSP / CPU0）、ASID 0、4 KiB 页、协作式调度与现有 M4.7–M4.11 用户态模型下，把共享系统调用表中的 `SYS_waitpid`（12）向 AArch64 EL0 正式开放，并打通 `fork` 子进程退出转入 `TASK_ZOMBIE`、父进程阻塞/非阻塞等待与同步回收（reap）的完整生命周期闭环。

- **在范围内**：
  - 共享系统调用表开通 `[SYS_waitpid] = { sys_native_waitpid, "waitpid" }`，双架构共用 `sys_native_waitpid` 与 `do_waitpid`。
  - 抽取跨架构共享的 `do_waitpid`、`waitpid_should_unblock`（`kernel/sched/wait.c`）与僵尸任务内核 slab 回收原语 `sched_free_reaped_task`（`kernel/sched/kthread.c`，对接 `kthread_observe_free` 记账）。
  - `SYS_waitpid(pid, user_status, options)` 参数与 `user_status` 写前校验/预备契约（`syscall_prepare_user_write`）：兼容 AArch64 惰性匿名页与 `fork` 后只读 COW 页写回，同时保持坏指针返回 `-EFAULT` 且不误摘除/不泄漏 ZOMBIE 子进程。
  - `blocker_wait` 跨阻塞调度的调用方中断掩码保持契约（保留入口 `entry_flags`，避免唤醒后第 2 段加锁覆盖入口中断状态或无条件开中断破坏 AArch64 EL0 同步异常栈帧）。
  - AArch64 `aarch64_user_task_exit` 生命周期演进：先彻底卸载并销毁用户地址空间与用户根，再在 `task_list_lock` 下原子完成（a）孤儿运行中子进程重挂 `idle`、（b）未等待孤儿 ZOMBIE 子进程摘除与锁外回收、（c）按父任务是否为存活 `PF_PROCESS` 决定自身进入 `TASK_ZOMBIE` 留存链表唤醒父进程，或走 `task_exit_self_reap` 自回收。
  - 宿主单测、QEMU `AARCH64_EL0_TEST=1` 端到端验收 `aarch64_user_waitpid` 与生产镜像静态审计。
- **不在范围内**：
  - `WUNTRACED` / `WCONTINUED` / 作业控制停止态、`waitid` / `wait4 rusage`、进程组等待（`pid == 0` 或 `pid < -1`）。
  - `SIGCHLD` 异步信号投递到 EL0 用户处理函数（AArch64 M4 信号投递属独立里程碑；本次仅通过 `task_wake_parent` / `blocker_wait` 完成内核态父进程阻塞唤醒）。
  - 用户态 `/bin/init` 常驻进程与 VFS/ELF 磁盘加载（无存活用户父进程的顶层或孤儿任务仍由内核在退出或父退出时无泄漏回收）。

## 2. 跨架构职责划分与共享 `SYS_waitpid` 通路

### 2.1 系统调用表与入口分发

- `kernel/syscall/dispatch.c` 顶部共享区（x86_64 与 `OS01_SYSCALL_A64_SUBSET=1` 共用）新增声明与表项：
  - 声明 `int64_t sys_native_waitpid(syscall_ctx_t *ctx);`（同步声明于 `kernel/include/arch/syscall.h`）；
  - 共享表项 `[SYS_waitpid] = { sys_native_waitpid, "waitpid" }`（系统调用号 12）；
  - 从 `kernel/syscall/dispatch.c` 的 `#ifndef OS01_SYSCALL_A64_SUBSET` 块中移除旧 `[SYS_waitpid] = { sys_proc_dispatch, "waitpid" }`，并从 `kernel/arch/x86_64/syscall/sys_proc.c` 的 `sys_proc_dispatch` 中移除已迁出的 `case SYS_waitpid:` 分支（Linux 兼容层 `kernel/syscall/compat/proc.c` 的 `compat_sys_wait4` 继续直接调用 `do_waitpid`）。
- `kernel/syscall/sys_native.c` 实现 `sys_native_waitpid(syscall_ctx_t *ctx)`：
  - 参数封送：`int64_t pid = (int64_t)(int)ctx->args[0]`（对 32 位有符号 `pid_t` 做符号扩展，确保用户态传入 `w0 = -1` / `0xffffffff` 归一为 `(int64_t)-1`）、`int *user_status = (int *)(uintptr_t)ctx->args[1]`、`int options = (int)ctx->args[2]`。
  - 若 `user_status != NULL`，先执行跨架构写前校验/预备 `int rc = syscall_prepare_user_write((uint64_t)(uintptr_t)user_status, sizeof(int));`；若 `rc < 0` 直接返回 `rc`（`-EFAULT` 或 `-ENOMEM`），绝不进入链表扫描、摘除或睡眠。
  - 校验通过后调用共享 `do_waitpid(pid, user_status, options)`。

### 2.2 共享 `do_waitpid` 与僵尸任务 slab 回收

- 将 `waitpid_should_unblock` 与 `do_waitpid` 从 `kernel/arch/x86_64/sched/exit.c` 下沉至跨架构共享调度模块 `kernel/sched/wait.c`（`kernel/include/sched/task.h` 保持既有公开声明 `int64_t do_waitpid(int64_t pid, int *user_status, int options)`）：
  - **构建与静态编译清单（`kernel/Makefile`）**：
    - x86_64 通过 `$(wildcard sched/*.c)` 自动编入 `sched/wait.c`；
    - AArch64 在 `ifeq ($(ARCH),aarch64)` 的 `KERNEL_C_SOURCES` 显式列表以及 `SCHED_COMMON_COMPILE_TUS`（供 `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` 检查）中同步加入 `sched/wait.c`。
  - **Pass 1（查找并摘除可回收 ZOMBIE）**：在 `task_list_lock`（关中断）下扫描 `init_task_union.task.list`，匹配 `t->parent == current`、`t->state == TASK_ZOMBIE`、`pid == -1 || t->pid == pid` 且 `__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) == 0` 的子任务；先读取 `child_pid = t->pid` 与 `exit_code = t->exit_code`，再执行 `list_del(&t->list)` 并清空 `t->list.next = NULL; t->list.prev = NULL;`。
  - **状态写回与同步回收**：释放 `task_list_lock` 后，若 `user_status != NULL`，通过 `copy_to_user_ft(user_status, &status, sizeof(status))` 写回 `int status = (int)exit_code`；随后无条件调用 `sched_free_reaped_task(child)` 同步释放 `child->thread`（kind 2）、`child->fpu_save`（kind 3）与 `child->stack_alloc_base`（kind 1），最后返回 `status_rc < 0 ? status_rc : child_pid`。
  - **Pass 2（存在性判定与阻塞/非阻塞分支）**：若 Pass 1 未命中可回收子任务，在 `task_list_lock` 下再次扫描是否存在 `t->parent == current && (pid == -1 || t->pid == pid)` 的子任务（含仍处于 `on_cpu == 1` 退出尾部的 `TASK_ZOMBIE` 子任务）：
    - 若不存在任何匹配子任务，返回 `-ECHILD`（因此 `pid == 0`、`pid < -1`、不存在的 PID、非自身子进程 PID、已回收的子进程 PID 均返回 `-ECHILD`）。
    - 若存在匹配子任务且 `(options & WNOHANG) != 0`，立即返回 `0`。
    - 否则设置 `current->blocker_data.waited_pid = pid`，调用 `blocker_wait(waitpid_should_unblock, BLOCKER_WAITPID, true)` 阻塞让出 CPU；被唤醒后（含 `-EINTR`）循环回到 Pass 1 重新扫描。
- `kernel/sched/kthread.c` 导出 `void sched_free_reaped_task(task_t *child)`（声明于 `kernel/include/sched/internal.h`，并在宿主桩头 `hosttests/mock/switch_complete_test_runtime.h` 的生产分支补齐前置声明以兼容 `switch_complete_production.o` 编译）：
  - 调用方在摘除前已通过 `__atomic_load_n(&child->on_cpu, __ATOMIC_ACQUIRE) == 0` 确认子任务已彻底离开 CPU（函数内部无需自旋等待）；
  - 复用 `kthread_observe_free` 协议依次释放 `child->thread`（kind 2）、`child->fpu_save`（kind 3）、`child->stack_alloc_base`（kind 1），确保 AArch64 `OS01_SELFTEST` 下 `aarch64_sched_observer` 的栈与线程上下文释放计数与 `PF_SELF_REAP` 路径严格一致。

### 2.3 `blocker_wait` 调用方中断状态保持与 `user_vm` 原生授权协议

- **`blocker_wait` 两段临界区的中断掩码恢复契约（`kernel/sched/core.c`）**：
  - `blocker_wait` 包含 `schedule()` 前后两段独立的 `task_list_lock` 临界区。当 `schedule()` 发生真实上下文切换并返回时，CPU 处于**关中断**状态（`RFLAGS.IF = 0` / `DAIF.I = 1`）。
  - 现行代码在 `schedule()` 后无条件调用 `arch_local_irq_enable()`，再执行第 2 段 `tl_flags = spin_lock_irqsave(&task_list_lock)` 与 `spin_unlock_irqrestore(&task_list_lock, tl_flags)`。这在 AArch64 EL0 `svc #0` 入口（`el0_sync_entry -> aarch64_user_syscall`，全程要求 `PSTATE.I = 1` 关中断直至 `restore_pt_regs` 完成 `SP_EL1` 异常帧弹出）下会把调用方原本的关中断状态篡改为开中断。
  - 反之，若仅删去 `arch_local_irq_enable()`，第 2 段 `tl_flags = spin_lock_irqsave(&task_list_lock)` 会用 `schedule()` 返回时的关中断状态覆盖入口 `tl_flags`，导致开中断调用方（x86_64 系统调用、内核线程及 `test_blocker_remote_wake`）在 `blocker_wait` 返回后被错误关中断。
  - **确定性修复契约**：在 `blocker_wait` 入口用 `uint64_t entry_flags = spin_lock_irqsave(&task_list_lock);` 保存调用方原始中断状态；第 1 段结束时 `spin_unlock_irqrestore(&task_list_lock, entry_flags)`；`schedule()` 返回后，先调用 `arch_local_irq_restore(entry_flags)` 恢复调用方进入 `blocker_wait` 时的中断掩码，再进入第 2 段 `uint64_t resume_flags = spin_lock_irqsave(&task_list_lock); ... spin_unlock_irqrestore(&task_list_lock, resume_flags);`。由此保证开中断调用方与关中断调用方在 `schedule()` 唤醒后及函数返回时均精确恢复其原始中断状态。
- **`user_vm` 原生授权与阻塞后写回契约**：
  - 采用**入口一次性写前预备（Option A，零架构污染）**：`sys_native_waitpid` 在调用 `do_waitpid` **之前**（此时刚经过 `aarch64_vm_native_enter(regs, SYS_waitpid)`，尚未发生任何阻塞切换，`g_native_auth` 必然有效）调用 `syscall_prepare_user_write((uint64_t)(uintptr_t)user_status, sizeof(int))`。
  - 该调用在父进程睡眠前已将 `[user_status, user_status + sizeof(int))` 涉及的惰性 RW 页或 COW 共享只读页在 `parent->mm` 中实例化/拆页为私有可写硬件叶 PTE（`VM_USER_RW`）。
  - 睡眠期间子进程拥有独立 `mm_t`，单核 AArch64 无其他线程共享 `parent->mm`；当父进程从 `blocker_wait` 唤醒并在 `do_waitpid` Pass 1 调用 `copy_to_user_ft(user_status, &status, sizeof(int))` 时，`aarch64_user_range_precheck` 直接命中 `AARCH64_RANGE_OK` 快速路径完成 `STTRB` 写回，无需在唤醒后再次进入 `aarch64_user_vm_prepare_copy`，随后 `aarch64_vm_native_leave()` 幂等清零授权状态。`kernel/sched/wait.c` 保持 100% 架构无关。

## 3. `user_status` 写前校验与惰性/COW 预备契约

- **问题根因**：
  - 在 x86_64 上，`syscall_check_user_range(addr, sizeof(int), true)` 接受已映射可写页及合法 `PAGE_COW` 页；但在 AArch64 上，M4.9/M4.11 冻结了 `syscall_check_user_range` 作为纯硬件页表快照（`aarch64_user_range_precheck == AARCH64_RANGE_OK`，不触发惰性分配与 COW 拆页）。
  - 若父进程在 `fork()` 后立即调用 `waitpid(child_pid, &status_on_stack, 0)`，父进程栈页此时仍处于只读 COW 共享态（`VM_USER_RO` + `PAGE_COW`）；或 `user_status` 位于刚通过 `mmap`/`brk` 申请尚未触碰的惰性可写匿名页。若直接调用 `syscall_check_user_range`，AArch64 会误判为 `-EFAULT`。
  - 反之，若跳过入口校验、等到摘除 ZOMBIE 子任务后再调 `copy_to_user_ft`，则当 `user_status` 为坏指针时：（1）无子进程调用 `waitpid(999, (int *)0x1, 0)` 会错误返回 `-ECHILD` 而非 `-EFAULT`（违反 `h6_waitpid_bad_status`）；（2）有 ZOMBIE 子进程调用 `waitpid(cpid, bad_ptr, 0)` 会在 `copy_to_user_ft` 失败前就把子任务摘除并释放（违反 `h6_waitpid_no_leak`）。
- **统一契约 `int syscall_prepare_user_write(uint64_t addr, uint64_t len)`（声明于 `kernel/include/memory/uaccess.h`）**：
  - `len == 0` 返回 `0`。
  - **x86_64 实现（`kernel/arch/x86_64/memory/uaccess.c`）**：
    - 返回 `syscall_check_user_range(addr, len, true) ? 0 : -EFAULT`，保持 x86 既有行为分毫不差。
  - **AArch64 实现（`kernel/arch/aarch64/memory/uaccess.c`）**：
    - 在关中断快照（`arch_local_irq_save`）下调用 `aarch64_user_range_precheck(addr, len, true, false, &mm, &root_pa)`：
      - 若为 `AARCH64_RANGE_OK`：硬件叶 PTE 已就绪且可写，恢复中断并返回 `0`。
      - 若为 `AARCH64_RANGE_DENIED`（空指针/低地址、内核地址、越界、root 未发布或与 `TTBR0_EL1` 不一致）：恢复中断并返回 `-EFAULT`。
      - 若为 `AARCH64_RANGE_PERMISSION`：调用 `aarch64_user_vm_prepare_copy(mm, addr, len, true)`（在当前 `aarch64_vm_native_enter` 授权下完成惰性 RW 匿名页分配清零或 COW 共享页拆页提升为私有 `VM_USER_RW`）：
        - 若返回 `-ENOMEM`，恢复中断并返回 `-ENOMEM`；
        - 若返回非 0，恢复中断并返回 `-EFAULT`；
        - 若返回 `0`，再次执行 `aarch64_user_range_precheck(addr, len, true, false, &mm, &root_pa)` 确认硬件叶已变为 `AARCH64_RANGE_OK`，恢复中断后返回 `0`（否则返回 `-EFAULT`）。
    - 保持 `syscall_check_user_range` 自身纯快照语义不变。

## 4. AArch64 用户任务退出、`TASK_ZOMBIE` 留存与孤儿回收契约

### 4.1 地址空间先行卸载原则

在 `kernel/arch/aarch64/cpu/user_task.c::aarch64_user_task_exit(uint64_t status)` 中，无论当前任务最终进入 `TASK_ZOMBIE` 等待父进程回收还是走 `PF_SELF_REAP` 自回收，第一步始终是：

1. 关中断，调用 `aarch64_user_restore_kernel_root()` 切回 `g_kernel_root_pa` 并撤销当前用户根发布；
2. 解绑 `owner->image = NULL`、`t->mm = NULL`、`t->thread->user_task = NULL`；
3. 开中断销毁 `aarch64_user_image_destroy(image)` 并 `kfree(owner)`，再关中断。

因此，处于 `TASK_ZOMBIE` 的子任务**不持有任何用户 4 KiB 物理页、COW 引用、VMA 节点或已发布页表根**，仅保留内核 `task_union`（`stack_alloc_base`）与 `thread_t`（`thread`）供父进程 `do_waitpid` 读取 `pid` / `exit_code` 后释放。

### 4.2 子任务重挂、孤儿 ZOMBIE 清理与自身退出模式判定

由于 AArch64 当前尚未运行常驻用户态 `/bin/init`（`OS01_HAS_USERSPACE == 0`），顶层通过 `aarch64_user_task_spawn` 创建的用户任务挂在 `idle = percpu_data[0].idle` 下，而 `idle` 不会调用 `waitpid`。为保证任何退出顺序下均无 ZOMBIE 泄漏，`aarch64_user_task_exit` 在销毁用户镜像后、单次 `task_list_lock`（关中断）临界区内原子完成以下两步：

1. **处理当前正在退出的任务 `t` 的所有子任务（`child->parent == t`）**：
   - 初始化局部待释放链表 `list_t reap_head; list_init(&reap_head);`，遍历 `init_task_union.task.list`（使用 `p->next` 预取以支持链表摘除）：
   - 对每个 `child->parent == t` 的子任务：
     - 将 `child->parent` 重挂为 `idle`。
     - **若 `child->state == TASK_ZOMBIE`**（即子进程先于父进程 `t` 退出成为 ZOMBIE，但父进程 `t` 未调用 `waitpid` 就退出了）：
       - 若 `__atomic_load_n(&child->on_cpu, __ATOMIC_ACQUIRE) == 0`，立即将其从 `init_task_union.task.list` 摘除（`list_del(&child->list)`）并挂入局部链表 `list_add_to_before(&reap_head, &child->list)`，在释放 `task_list_lock` 后逐个从 `reap_head` 摘下并调用 `sched_free_reaped_task(child)` 同步回收其 `thread` / `fpu_save` / `stack_alloc_base`；
       - 若 `child->on_cpu != 0`（防御性分支），从全局链表摘除（`list_del(&child->list); child->list.next = NULL; child->list.prev = NULL;`）并置 `child->flags |= PF_SELF_REAP`，交由其 `task_switch_complete` 尾部回收。
     - **若 `child->state != TASK_ZOMBIE`**（子进程仍存活，成为孤儿进程）：
       - 其 `child->parent` 已重挂为 `idle`；当该孤儿进程稍后退出时，在下一步判定中会发现其父任务为 `idle`，从而自动走 `PF_SELF_REAP` 自回收路径。
2. **在同一 `task_list_lock` 下判定当前任务 `t` 自身的退出模式**：
   - 判定 `t->parent` 是否为存活的用户进程：
     `bool waitable_parent = (t->parent != NULL) && (t->parent != idle) && ((t->parent->flags & PF_PROCESS) != 0) && (t->parent->state != TASK_ZOMBIE);`
   - **分支 A（`!waitable_parent`：顶层 spawn 任务，或父进程已先退出的孤儿任务）**：
     - 解锁 `task_list_lock`，释放 `reap_head` 中的孤儿 ZOMBIE 子任务，随后调用 `task_exit_self_reap(status)`（从 `task_list` 摘除、置 `PF_SELF_REAP | TASK_ZOMBIE`，由下一个任务的 `task_switch_complete` 释放自身栈与 `thread_t`）。
   - **分支 B（`waitable_parent == true`：父进程仍存活的 `fork` 子进程）**：
     - 保持 `t` 留在 `init_task_union.task.list` 上（**不**置 `PF_SELF_REAP`，**不**调用 `list_del`）；
     - 记录 `t->exit_code = status` 并置 `__atomic_store_n(&t->on_cpu, 1, __ATOMIC_RELEASE)`；
     - 解锁 `task_list_lock`，释放 `reap_head` 中的孤儿 ZOMBIE 子任务；
     - 调用 `task_wake_parent(t)` 唤醒正在 `blocker_wait(BLOCKER_WAITPID)` 睡眠的父进程，再调用 `task_exit_zombie()`（在 `rq_lock` 下置 `t->state = TASK_ZOMBIE`）并调用 `schedule()` 永久切出 CPU；
     - 下一个任务在 `task_switch_complete(t) -> task_finish_switch(t)` 中以 release 语义将 `t->on_cpu` 清零，使 `t` 对父进程的 `waitpid_should_unblock` 与 `do_waitpid` Pass 1 变为可回收状态。

### 4.3 与既有 `aarch64_user_fork_cow` 自测（`FC_MODE_SHORT`）的兼容性

- `kernel/selftest/test_aarch64_user_fork_cow.c` 的 `aarch64_user_fork_cow_test_reap(struct task_struct *prev)` 在 `task_switch_complete(prev)` 中观测退出任务的栈切换安全与用户根撤销。
- 原实现以 `!(prev->flags & PF_SELF_REAP)` 过滤非退出切换；当 `FC_MODE_SHORT` 中的短命子进程退出时，因其父进程 `short_parent` 仍存活，子进程现在作为真实 `TASK_ZOMBIE`（无 `PF_SELF_REAP`）切出并在 `short_parent` 退出时作为孤儿 ZOMBIE 被同步回收。
- 将 `aarch64_user_fork_cow_test_reap` 的过滤条件更新为 `prev->state != TASK_ZOMBIE`（任务在 `task_switch_complete` 时处于 `TASK_ZOMBIE` 当且仅当其正从退出尾部永久切出，无论 `PF_SELF_REAP` 还是留存等待父进程回收），既保持 `aarch64_user_fork_cow` 全部既有断言成立，又自然覆盖了「父进程不调用 `waitpid` 直接退出时自动回收其 ZOMBIE 子进程」的孤儿清理路径。

## 5. 验证与验收标准

### 5.1 宿主单测（`make test-host`）

1. 既有宿主套件适配：
   - `hosttests/cases/test_syscall_dispatch.c`：`SYS_waitpid`（12）移入 `shared[]` 列表并提供 `sys_native_waitpid` 计数桩，验证 x86_64 与 `OS01_SYSCALL_A64_SUBSET=1` 均通过 `syscall_has_handler(SYS_waitpid)` 并路由至 `sys_native_waitpid`；
   - `hosttests/cases/test_compat_linux.c`、`hosttests/cases/test_nofile_limits.c`：补齐 `sys_native_waitpid` 与 `syscall_prepare_user_write` 桩；
   - `hosttests/mock/switch_complete_test_runtime.h`：在生产对象分支补齐 `void sched_free_reaped_task(task_t *child);` 前置声明，确保 `switch_complete_production.o`（编译 `kernel/sched/kthread.c`）零警告通过。
2. 新增 `hosttests/cases/test_waitpid.c`（注册于 `hosttests/mk/sched.mk`）：
   - 链接真实的 `kernel/sched/wait.c` 与 `kernel/syscall/sys_native.c`（配合可控的 `task_list`、`blocker_wait`、`sched_free_reaped_task` 与 `uaccess` 桩），验证：
     - `pid == -1`（任意子进程）与 `pid > 0`（指定子进程 PID）成功摘除 `TASK_ZOMBIE`（`on_cpu == 0`）、写回 32 位 `status`、调用 `sched_free_reaped_task` 释放子任务并返回子进程 PID；
     - `user_status == NULL` 时正常回收子任务且不触发用户内存写；
     - 32 位负 `pid`（如 `args[0] = 0xffffffffULL`）经 `sys_native_waitpid` 符号扩展为 `-1`；
     - 子进程仍在运行或处于 `TASK_ZOMBIE` 但 `on_cpu == 1` 时：`WNOHANG` 返回 `0` 且不摘除子任务；阻塞模式进入 `blocker_wait(BLOCKER_WAITPID)` 并在唤醒后完成回收；
     - 无子进程、非自身子进程、`pid == 0`、`pid < -1` 或第二次重复 `waitpid` 已回收子进程时返回 `-ECHILD`；
     - `user_status` 为坏指针（`-EFAULT`）或预备期 `-ENOMEM` 时：无子进程亦优先返回 `-EFAULT`/`-ENOMEM`，有 ZOMBIE 子进程时不摘除、不释放该子任务，后续使用合法指针再次 `waitpid` 仍可成功回收。

### 5.2 QEMU 端到端自测（`make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`）

新增第 25 项内核自测 `aarch64_user_waitpid`（`kernel/selftest/test_aarch64_user_waitpid.c` + 嵌入式 EL0 固件 `kernel/selftest/fixtures/aarch64_user_waitpid.S`），在同一单核 BSP 环境验证以下完整场景：

1. **阻塞式 `waitpid` 与 `blocker_wait` 唤醒**：
   - 父进程 `fork()` 出子进程 1；子进程 1 先多次调用 `AARCH64_USER_SVC_YIELD`（`svc #0x4d40`）让出 CPU 再调用 `SYS_exit(0x2a)`；
   - 父进程立即调用阻塞式 `SYS_waitpid(child1_pid, &status, 0)`（此时子进程 1 尚未退出，父进程进入 `TASK_INTERRUPTIBLE` 睡眠）；
   - 子进程 1 退出转入 `TASK_ZOMBIE` 并唤醒父进程；父进程返回 `child1_pid`，读出 `status == (0x2a << 8)`，且 `&status` 所在父栈 COW 页在写前预备中被正确拆分为私有可写页。
2. **`WNOHANG` 非阻塞轮询与指定 PID / `-1` 回收**：
   - 父进程连续 `fork()` 出子进程 2 与子进程 3；
   - 子进程 2 仍在运行时，父进程调用 `SYS_waitpid(child2_pid, &status, WNOHANG)` 返回 `0`；
   - 让出 CPU 待子进程 2（退出码 `0x11`）与子进程 3（退出码 `0x22`）退出后，父进程先以 `SYS_waitpid(child3_pid, &status, WNOHANG)` 精确回收子进程 3（验证 `status == (0x22 << 8)`），再以 `SYS_waitpid(-1, &status, 0)` 回收剩余的子进程 2（验证返回 `child2_pid` 且 `status == (0x11 << 8)`）。
3. **惰性匿名页 `status` 写回与坏指针 `-EFAULT` 不丢 ZOMBIE**：
   - 父进程 `mmap` 一页尚未触碰的惰性 `PROT_READ | PROT_WRITE` 匿名页及一页 `PROT_READ` 只读页，并 `fork()` 出子进程 4（退出码 `0x55`）；
   - 待子进程 4 退出成为 `TASK_ZOMBIE` 后，父进程先对低地址 `0x1` 与 `PROT_READ` 页地址调用 `SYS_waitpid(child4_pid, bad_ptr, 0)`，两次均返回 `-EFAULT` 且子进程 4 仍保留在 `task_list` 中；
   - 父进程随后将 `user_status` 指向未触碰的惰性 RW `mmap` 页调用 `SYS_waitpid(child4_pid, lazy_rw_ptr, 0)`，成功触发写前预备分配清零页、写回 `(0x55 << 8)` 并返回 `child4_pid`；
   - 再次调用 `SYS_waitpid(-1, NULL, 0)` 与 `SYS_waitpid(child4_pid, NULL, 0)` 均返回 `-ECHILD`；对坏指针 `0x1` 调用 `SYS_waitpid(-1, (int *)0x1, 0)` 仍返回 `-EFAULT`。
4. **孤儿进程与未等待 ZOMBIE 子进程退出清理**：
   - 父进程 `fork()` 出子进程 5（立即退出成为 ZOMBIE，父进程故意不调用 `waitpid`）与子进程 6（在父进程退出后才退出，验证 `getppid()` 变为 `idle` PID 0 并走 `PF_SELF_REAP`），随后父进程退出。
5. **资源零泄漏闭环断言**：
   - 全部任务退出并回收后，验证：`pmm_probe_4k_allocated_count() == alloc_before`（4 KiB 物理页零泄漏）、`aarch64_pt_published_root_count() == 1`（仅内核根在线）、`aarch64_sched_observer` 批次内 `created_delta == 14`（7 个任务 × stack/thread 2 项）、`released(STACK)` 与 `released(THREAD)` 增量均为 `7` 且 `aarch64_sched_observer_end() == 0`（ZOMBIE 回收与孤儿 ZOMBIE 回收无任何栈或 `thread_t` 泄漏）。

### 5.3 生产镜像静态审计与构建门禁（`make test-static`）

- `kernel/Makefile`：
  - `sched/wait.c` 加入 `ifeq ($(ARCH),aarch64)` 的 `KERNEL_C_SOURCES` 以及 `SCHED_COMMON_COMPILE_TUS`（使 `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` 覆盖 `sched/wait.c`）；
  - `AARCH64_EL0_TEST=1` 分支新增 `selftest/test_aarch64_user_waitpid.c` 与嵌入式测试 ELF 构建规则。
- `qemutests/intr_dispatch_audit.py`（及 `qemutests/test_intr_dispatch_audit.py`）：
  - `_A64_DISASM_SYMS`、`_A64_UACCESS_SYMS` 与 `_A64_VM_EDGES` 增加 `syscall_prepare_user_write`、`sys_native_waitpid -> do_waitpid` 调用边证明；
  - `_A64_FORBIDDEN_TEST_SYMS` 收录 `aarch64_user_waitpid` 新增的测试观测符号（如有），`_A64_FORBIDDEN_ELF_PREFIXES`（`_binary_aarch64_`）自动拦截嵌入式测试固件 `_binary_aarch64_user_waitpid_*`，确保生产 `PROFILE=aarch64-clang` 镜像零测试符号泄漏。
- x86_64 回归：`make test-host`、`make test-static`、`make OS01_SYSTEST=1 test-qemu SUITE=systest` 全部通过。
