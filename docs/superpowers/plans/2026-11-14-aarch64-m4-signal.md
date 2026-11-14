# AArch64 M4.14：原生信号与 `sigreturn` 闭环实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在双架构共享系统调用层实现原生 `SYS_kill`（38）、`SYS_signal`（39）、`SYS_sigprocmask`（42）、`SYS_sigreturn`（43），在 AArch64 编入 `kernel/sched/signal.c` 并实现 EL0 信号栈帧 `struct aarch64_sigframe`、`aarch64_do_signal_delivery`、`arch_syscall_sigreturn` 以及 `exec` 信号处理器重置，以精简的宿主测试、静态审计与单一 QEMU 内核自测 `aarch64_user_signal` 完成验证。

**Architecture:** `kernel/syscall/sys_native.c` 承载 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn`，并在 `kernel/syscall/dispatch.c` 中移入双架构共享区（AArch64 原生共享系统调用增至 15 项）。AArch64 在 `user_task_prepare` 中初始化 `tsk->addr_limit = AARCH64_USER_LIMIT`，在 `arch_syscall_exec` / `aarch64_user_task_exec` 提交区重置 `t->sighand` 为 `SIG_DFL`，并在 `kernel/arch/aarch64/cpu/user_task.c` 中实现 `aarch64_do_signal_delivery(pt_regs_t *regs)`（遵循 `task->signal` 用 `1ULL << sig`、`task->blocked` 用 `1ULL << (sig - 1)` 的位编码约定）与 `arch_syscall_sigreturn(void *arch_frame)`（校验 `(frame.regs.spsr_el1 & ~0xf0000000ULL) == AARCH64_USER_TASK_PSTATE`），挂接于 `aarch64_user_syscall` 写回 `regs->x0` 之后、`AARCH64_USER_SVC_YIELD` 写回 `regs->x0 = 0` 之后与 `aarch64_user_first_entry` 首次进入 EL0 前。

**Tech Stack:** C11、AArch64 汇编、Clang/LLD、GNU Make、Python `unittest` 静态审计、宿主 C 单测、QEMU `virt,gic-version=3` 内核自测。

**Spec:** [`docs/superpowers/specs/2026-11-14-aarch64-m4-signal-design.md`](../specs/2026-11-14-aarch64-m4-signal-design.md)

---

## 文件变动总览

| 文件 | 变更类型 | 职责 |
|---|---|---|
| `kernel/include/arch/syscall.h` | Modify | 声明 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` |
| `kernel/syscall/dispatch.c` | Modify | 将 `SYS_kill`、`SYS_signal`、`SYS_sigprocmask`、`SYS_sigreturn` 移入双架构共享区并从 `#ifndef OS01_SYSCALL_A64_SUBSET` 块中移除旧项 |
| `kernel/syscall/sys_native.c` | Modify | 实现 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` |
| `kernel/arch/x86_64/syscall/sys_proc.c` | Modify | 移除已迁入 `kernel/syscall/sys_native.c` 的 `SYS_kill`、`SYS_signal`、`SYS_sigprocmask`、`SYS_sigreturn` 分支 |
| `kernel/Makefile` | Modify | `ARCH=aarch64` 的 `KERNEL_C_SOURCES` 与 `SCHED_COMMON_COMPILE_TUS` 加入 `sched/signal.c`；`AARCH64_EL0_TEST=1` 加入 `test_aarch64_user_signal.c` 与 `aarch64_user_signal_image.o` |
| `kernel/include/arch/aarch64/syscall.h` | Modify | 定义 `struct aarch64_sigframe`（含 280 字节 `_Static_assert`）并声明 `aarch64_do_signal_delivery(pt_regs_t *regs)` |
| `kernel/arch/aarch64/intr/syscall.c` | Modify | 在 `aarch64_user_syscall` 最末尾（`if (!ctx.suppress_writeback) regs->x0 = (uint64_t)ret;` 之后）调用 `aarch64_do_signal_delivery(regs)` |
| `kernel/arch/aarch64/cpu/user_task.c` | Modify | `user_task_prepare` 置 `tsk->addr_limit = AARCH64_USER_LIMIT`；`exec` 提交区重置 `t->sighand`；实现 `aarch64_do_signal_delivery` 与 `arch_syscall_sigreturn`；在 `aarch64_user_first_entry` 与 `AARCH64_USER_SVC_YIELD` 挂接 `aarch64_do_signal_delivery` |
| `hosttests/cases/test_syscall_dispatch.c` | Modify | 新增 4 个 `NATIVE_SIG*_MARK` 与桩函数，并将 `SYS_kill`、`SYS_signal`、`SYS_sigprocmask`、`SYS_sigreturn` 加入 `shared[]` |
| `hosttests/cases/test_compat_linux.c` | Modify | 补充 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` 链接桩 |
| `hosttests/cases/test_nofile_limits.c` | Modify | 补充 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` 链接桩 |
| `hosttests/cases/test_waitpid.c` | Modify | 补齐 `syscall_check_user_range`、`task_wake`、`task_send_signal`、`signal_pgrp`、`arch_syscall_sigreturn` 受控桩并新增 `test_sys_native_signal_syscalls()` |
| `qemutests/intr_dispatch_audit.py` | Modify | `_A64_DISASM_SYMS` 与 `_A64_VM_EDGES` 加入信号相关符号与调用边校验 |
| `qemutests/test_intr_dispatch_audit.py` | Modify | 同步合成符号表/反汇编夹具并增加信号边缺失负向单测 |
| `kernel/selftest/fixtures/aarch64_user_signal.S` | Create | EL0 信号自测汇编固件（覆盖非法指针/伪造帧、`sigprocmask` 阻塞/解阻塞、自定义 handler + `sigreturn` 恢复、`fork` + `exec` 重置 handler + `SIGUSR1` 默认终止 + `waitpid`） |
| `kernel/selftest/test_aarch64_user_signal.c` | Create | 第 27 项 AArch64 内核自测驱动 |
| `kernel/include/arch/aarch64/boot.h` | Modify | `AARCH64_EL0_TEST` 下 `ARCH_SELFTEST_EXPECTED_LATE` 由 `19U` 更新为 `20U` |
| `kernel/selftest/selftest.c` | Modify | `AARCH64_EL0_TEST` 分支注册 `selftest_register_late("aarch64_user_signal")` |
| `kernel/selftest/test_aarch64_scheduler.c` | Modify | `AARCH64_EL0_TEST` 尾部调用并记录 `test_aarch64_user_signal()` |
| `docs/syscall/syscall.md` | Modify | 更新 `SYS_kill` / `SYS_signal` / `SYS_sigprocmask` / `SYS_sigreturn` 跨架构共享标注与计数（15 项） |
| `docs/roadmap.md` | Modify | 记录 AArch64 M4.14 原生信号与 `sigreturn` 闭环状态 |

---

### Task 1: 跨架构共享信号系统调用（`SYS_kill` / `SYS_signal` / `SYS_sigprocmask` / `SYS_sigreturn`）与宿主单测

**Files:**
- Modify: `kernel/include/arch/syscall.h`
- Modify: `kernel/syscall/dispatch.c`
- Modify: `kernel/syscall/sys_native.c`
- Modify: `kernel/arch/x86_64/syscall/sys_proc.c`
- Modify: `hosttests/cases/test_syscall_dispatch.c`
- Modify: `hosttests/cases/test_compat_linux.c`
- Modify: `hosttests/cases/test_nofile_limits.c`
- Modify: `hosttests/cases/test_waitpid.c`

- [ ] **Step 1: 在既有宿主测试文件中编写 4 个信号系统调用的分派与行为测试（RED）**

1. 在 `hosttests/cases/test_syscall_dispatch.c` 中：
   - 新增标记宏与桩函数：
     ```c
     #define NATIVE_KILL_MARK        (-1008)
     #define NATIVE_SIGNAL_MARK      (-1009)
     #define NATIVE_SIGPROCMASK_MARK (-1010)
     #define NATIVE_SIGRETURN_MARK   (-1011)

     int64_t sys_native_kill(syscall_ctx_t *ctx) { received = ctx; return NATIVE_KILL_MARK; }
     int64_t sys_native_signal(syscall_ctx_t *ctx) { received = ctx; return NATIVE_SIGNAL_MARK; }
     int64_t sys_native_sigprocmask(syscall_ctx_t *ctx) { received = ctx; return NATIVE_SIGPROCMASK_MARK; }
     int64_t sys_native_sigreturn(syscall_ctx_t *ctx) { received = ctx; return NATIVE_SIGRETURN_MARK; }
     ```
   - 在 `expected_return(uint64_t nr)` 中将 `SYS_kill`、`SYS_signal`、`SYS_sigprocmask`、`SYS_sigreturn` 从 `PROC_MARK` 分支移至各自的 `NATIVE_*_MARK`；
   - 在 `#ifdef OS01_SYSCALL_A64_SUBSET` 的 `shared[]` 数组中加入 `SYS_kill, SYS_signal, SYS_sigprocmask, SYS_sigreturn`。
2. 在 `hosttests/cases/test_compat_linux.c` 中：
   - 新增 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` 桩（其中 `sys_native_signal` 返回 `12345` 以匹配 `compat_sys_rt_sigaction` 既有断言 `compat_linux_dispatch(&ctx) == 12345`）。
3. 在 `hosttests/cases/test_nofile_limits.c` 中：
   - 补充 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn` 4 行返回 `-ENOSYS` 的链接桩。
4. 在 `hosttests/cases/test_waitpid.c` 中：
   - 补齐 `kernel/syscall/sys_native.c` 链接所需的全部 5 个受控测试桩：
     - `bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)`（当 `addr == 0 || g_uaccess_mode == UACCESS_FAIL_PREPARE` 时返回 `false`，否则返回 `true`）；
     - `void task_wake(task_t *t)`（将 `t->state = TASK_RUNNING` 并递增唤醒计数 `g_wake_calls`）；
     - `int task_send_signal(int pid, int sig)`、`int signal_pgrp(pid_t pgrp, int sig)`、`int64_t arch_syscall_sigreturn(void *arch_frame)`；
   - 新增 `test_sys_native_signal_syscalls()`，覆盖：
     - `sys_native_kill`：`sig == 0` 或 `sig >= NSIG` 返回 `-EINVAL`；`pid > 0` 调用 `task_send_signal`；`pid == 0` 与 `pid < -1` 调用 `signal_pgrp`；`pid == -1` 广播对非 `current`、非 `PF_KTHREAD`、非 `pid == 1` 的任务置位 `t->signal |= (1ULL << sig)` 并在 `TASK_INTERRUPTIBLE` 时调用 `task_wake(t)`；
     - `sys_native_signal`：`sa_handler >= current->addr_limit` 返回 `-EINVAL`；正常安装 `SIGUSR1` handler 并通过 `oldact` 读出旧配置；`SIGKILL` 忽略新 handler 安装但返回 `0`；
     - `sys_native_sigprocmask`：`SIG_BLOCK`、`SIG_UNBLOCK`、`SIG_SETMASK` 正确修改 `current->blocked`，且试图屏蔽 `SIGKILL`/`SIGSTOP` 时对应位 `(1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1))` 被自动清除；
     - `sys_native_sigreturn`：`arch_syscall_sigreturn` 返回 `0` 时 `ctx.suppress_writeback == true`，返回 `-EINVAL` 时 `ctx.suppress_writeback == false`。

- [ ] **Step 2: 运行宿主测试确认 RED**

Run:
```bash
make -C hosttests test-syscall-dispatch test-compat-linux test-waitpid
```
Expected: FAIL（`sys_native_kill` 等尚未在 `sys_native.c` 与 `dispatch.c` 中定义和注册）。

- [ ] **Step 3: 在 `sys_native.c`、`dispatch.c`、`syscall.h` 与 `sys_proc.c` 中完成迁移实现（GREEN）**

1. 在 `kernel/include/arch/syscall.h` 中声明：
```c
int64_t sys_native_kill(syscall_ctx_t *ctx);
int64_t sys_native_signal(syscall_ctx_t *ctx);
int64_t sys_native_sigprocmask(syscall_ctx_t *ctx);
int64_t sys_native_sigreturn(syscall_ctx_t *ctx);
```
2. 在 `kernel/syscall/dispatch.c` 中：
   - 顶部前置声明加入 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn`；
   - 在共享区加入：
     ```c
     [SYS_kill]          = { sys_native_kill,          "kill" },
     [SYS_signal]        = { sys_native_signal,        "signal" },
     [SYS_sigprocmask]   = { sys_native_sigprocmask,   "sigprocmask" },
     [SYS_sigreturn]     = { sys_native_sigreturn,     "sigreturn" },
     ```
   - 从 `#ifndef OS01_SYSCALL_A64_SUBSET` 块中移除旧的 `[SYS_kill]`、`[SYS_signal]`、`[SYS_sigprocmask]`、`[SYS_sigreturn]` 表项。
3. 在 `kernel/syscall/sys_native.c` 中（引入 `<kernel.h>` 与 `<arch/spinlock.h>`，注意严禁在代码或注释中出现 `pt_regs` 或寄存器字段名以符合 `syscall_boundary_audit.py`）：
   - 定义 `#ifndef SIG_BLOCK` 常量（`SIG_BLOCK=0`、`SIG_UNBLOCK=1`、`SIG_SETMASK=2`）；
   - 实现 `sys_native_kill`、`sys_native_signal`、`sys_native_sigprocmask`、`sys_native_sigreturn`（按 spec §3.1，其中：
     - `sys_native_kill` 的 `pid == -1` 分支保持原 `sys_proc.c:162-177` 的 `t->signal |= (1ULL << sig); if (t->state == TASK_INTERRUPTIBLE) task_wake(t);`；
     - `sys_native_signal` 与 `sys_native_sigprocmask` 中 `oldact` 与 `oldset` 写前调用 `syscall_prepare_user_write` + `copy_to_user_ft`；
     - `sys_native_sigprocmask` 强制清除 `current->blocked &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)))`）。
4. 在 `kernel/arch/x86_64/syscall/sys_proc.c` 中移除已迁出的 `case SYS_kill:`、`case SYS_signal:`、`case SYS_sigprocmask:`、`case SYS_sigreturn:` 及不再使用的 `#ifndef SIG_BLOCK` 宏。

- [ ] **Step 4: 运行宿主单测与静态边界审计确认 GREEN**

Run:
```bash
make test-host
make test-static CASE=syscall-boundary
```
Expected: 全部 PASS。

---

### Task 2: AArch64 信号投递、`arch_syscall_sigreturn` 与 `exec` 信号重置

**Files:**
- Modify: `kernel/Makefile`
- Modify: `kernel/include/arch/aarch64/syscall.h`
- Modify: `kernel/arch/aarch64/intr/syscall.c`
- Modify: `kernel/arch/aarch64/cpu/user_task.c`

- [ ] **Step 1: 在 `kernel/Makefile` 编入 `sched/signal.c` 并扩展 `kernel/include/arch/aarch64/syscall.h`**

1. 在 `kernel/Makefile` 中：
   - 在 `ifeq ($(ARCH),aarch64)` 的 `KERNEL_C_SOURCES`（`sched/wait.c` 旁）加入 `sched/signal.c`；
   - 在 `SCHED_COMMON_COMPILE_TUS` 列表中同样加入 `sched/signal.c`。
2. 在 `kernel/include/arch/aarch64/syscall.h` 中定义 `struct aarch64_sigframe` 并声明 `aarch64_do_signal_delivery`：
```c
/* User-stack signal trampoline frame pushed before entering sa_handler
 * and consumed by SYS_sigreturn (arch_syscall_sigreturn). */
struct aarch64_sigframe {
    pt_regs_t regs;
    uint64_t  blocked;
};

_Static_assert(sizeof(struct aarch64_sigframe) == 280,
               "aarch64_sigframe must be 280 bytes");

/* Deliver pending unblocked signal(s) before returning to EL0. */
void aarch64_do_signal_delivery(pt_regs_t *regs);
```

- [ ] **Step 2: 在 `kernel/arch/aarch64/cpu/user_task.c` 与 `kernel/arch/aarch64/intr/syscall.c` 实现信号投递、`sigreturn` 与 `exec` 重置**

1. 在 `kernel/arch/aarch64/cpu/user_task.c` 中（引入 `<memory/uaccess.h>` 与 `<uapi/syscall.h>`）：
   - 在 `user_task_prepare` 中设置 `tsk->addr_limit = AARCH64_USER_LIMIT;`；
   - 在 `aarch64_user_task_exec` 与 `arch_syscall_exec` 的关中断提交区内加入 `memset(t->sighand, 0, sizeof(t->sighand));`；
   - 实现 `aarch64_do_signal_delivery(pt_regs_t *regs)`（按 spec §3.3）：
     - 校验 `regs != NULL`、`(regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) == AARCH64_SPSR_MODE_EL0T` 且 `aarch64_user_task_is_user(cur)`；
     - 循环处理未屏蔽信号（对齐 `cur->signal` 的 `1ULL << sig` 与 `cur->blocked` 的 `1ULL << (sig - 1)`，优先处理 `SIGKILL`）：
       ```c
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
       ```
     - `SIG_IGN` 或默认忽略信号（`SIGCHLD`/`SIGCONT`/`SIGURG`/`SIGWINCH`/`SIGTSTP`/`SIGTTIN`/`SIGTTOU`）直接 `continue`；
     - `SIG_DFL` 致命信号：保护 `cur->pid == 1`，记录 `log_info("signal: task %ld killed by signal %d (default)\n", cur->pid, sig)`，调用 `aarch64_vm_native_leave()` 后调用 `arch_syscall_exit((uint64_t)sig)`；
     - 自定义 handler：若 `!restorer`（且 `cur->pid != 1`）记录 `log_warn` 并以 `arch_syscall_exit((uint64_t)sig)` 退出；否则构造 `struct aarch64_sigframe frame` 于 `new_sp = (regs->sp_el0 - sizeof(frame)) & ~UINT64_C(0xf)`，通过 `aarch64_vm_native_enter(regs, SYS_sigreturn)` + `copy_to_user_ft` + `aarch64_vm_native_leave()` 写入用户栈（失败则 `__atomic_fetch_or(&cur->signal, (1ULL << sig), __ATOMIC_SEQ_CST)` 恢复待决位并返回），更新 `cur->blocked`（置位 `(1ULL << (sig - 1)) | cur->sighand[sig].sa_mask` 并清除 `SIGKILL`/`SIGSTOP`）并设置 `regs->x0 = (uint64_t)sig`、`regs->x30 = (uint64_t)(uintptr_t)restorer`、`regs->sp_el0 = new_sp`、`regs->elr_el1 = (uint64_t)(uintptr_t)handler`、`regs->spsr_el1 = AARCH64_USER_TASK_PSTATE` 后返回；
   - 实现 `arch_syscall_sigreturn(void *arch_frame)`（按 spec §3.4）：
     - 校验 `aarch64_user_frame_is_valid(regs)` 与 `(regs->sp_el0 & 0xf) == 0`；
     - 调用 `syscall_check_user_range(regs->sp_el0, sizeof(frame), false)` 与 `copy_from_user_ft(&frame, (const void *)(uintptr_t)regs->sp_el0, sizeof(frame))`；
     - 校验 `(frame.regs.spsr_el1 & ~UINT64_C(0xf0000000)) == AARCH64_USER_TASK_PSTATE`，不满足则返回 `-EINVAL`；
     - 恢复 `*regs = frame.regs` 与 `cur->blocked = (int64_t)(frame.blocked & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1))))`，返回 `0`；
   - 在 `aarch64_user_first_entry` 的 `aarch64_el0_restore(frame)` 之前调用 `aarch64_do_signal_delivery((pt_regs_t *)frame)`；
   - 在 `aarch64_user_task_sync` 的 `case AARCH64_USER_SVC_YIELD:` 分支中，`schedule()` 之后立即设置 `regs->x0 = 0;` 并调用 `aarch64_do_signal_delivery(regs);`，最后在 `return;` 之前保留 `#if AARCH64_EL0_TEST if (g_arm_tick) { cntp_tval_el0_write(1); cntp_isb(); } #endif`。
2. 在 `kernel/arch/aarch64/intr/syscall.c` 的 `aarch64_user_syscall` 最末尾（必须在 `aarch64_vm_native_leave();` **以及** `if (!ctx.suppress_writeback) regs->x0 = (uint64_t)ret;` **之后**）调用 `aarch64_do_signal_delivery(regs)`。

- [ ] **Step 3: 编译 AArch64 内核与 `sched-common-compile` 静态检查确认零错误通过**

Run:
```bash
make PROFILE=aarch64-clang test-static CASE=sched-common-compile
make PROFILE=aarch64-clang aarch64-uefi-kernel
```
Expected: 编译与链接零错误通过。

---

### Task 3: 生产镜像静态审计与第 27 项精简 QEMU 自测 `aarch64_user_signal`

**Files:**
- Modify: `qemutests/intr_dispatch_audit.py`
- Modify: `qemutests/test_intr_dispatch_audit.py`
- Create: `kernel/selftest/fixtures/aarch64_user_signal.S`
- Create: `kernel/selftest/test_aarch64_user_signal.c`
- Modify: `kernel/include/arch/aarch64/boot.h`
- Modify: `kernel/selftest/selftest.c`
- Modify: `kernel/selftest/test_aarch64_scheduler.c`
- Modify: `kernel/Makefile`
- Modify: `docs/syscall/syscall.md`
- Modify: `docs/roadmap.md`

- [ ] **Step 1: 更新 `qemutests/intr_dispatch_audit.py` 与 `qemutests/test_intr_dispatch_audit.py`（RED -> GREEN）**

1. 在 `qemutests/intr_dispatch_audit.py` 中：
   - `_A64_DISASM_SYMS` 加入 `"sys_native_kill"`、`"task_send_signal"`、`"signal_pgrp"`、`"sys_native_signal"`、`"sys_native_sigprocmask"`、`"sys_native_sigreturn"`、`"arch_syscall_sigreturn"`、`"aarch64_do_signal_delivery"`；
   - `_A64_VM_EDGES` 加入：
     ```python
     ("sys_native_kill", "task_send_signal",
      "the shared kill handler must reach task_send_signal"),
     ("sys_native_sigreturn", "arch_syscall_sigreturn",
      "the shared sigreturn handler must reach the architecture sigreturn entry"),
     ("aarch64_user_syscall", "aarch64_do_signal_delivery",
      "the AArch64 native syscall exit path must deliver pending signals"),
     ```
2. 在 `qemutests/test_intr_dispatch_audit.py` 中：
   - 在 `A64_NM` 与 `A64_DISASM` 合成夹具中补齐上述符号与 `bl` 指令（含在 `aarch64_user_syscall` 块中加入 `bl aarch64_do_signal_delivery`）；
   - 新增负向单测验证缺少 `sys_native_sigreturn -> arch_syscall_sigreturn` 或 `aarch64_user_syscall -> aarch64_do_signal_delivery` 时审计报错。
3. 运行：
```bash
python3 -m unittest qemutests/test_intr_dispatch_audit.py
make PROFILE=aarch64-clang test-aarch64-audit
```
Expected: 全部 PASS。

- [ ] **Step 2: 编写精简 EL0 汇编固件 `kernel/selftest/fixtures/aarch64_user_signal.S`**

创建 `kernel/selftest/fixtures/aarch64_user_signal.S`，在一个自包含 EL0 镜像内依次验证：
1. **入口 `argc` 分流（验证 `exec` 重置已捕获的信号处理器为 `SIG_DFL`）**：
   - `_start` 检查 `cmp x0, #2`（`argc == 2` 表示 `fork` 后经 `SYS_exec("/bin/user_signal", {"/bin/user_signal", "child", NULL}, NULL)` 进入的新镜像子进程）：
   - 若 `x0 == 2`，跳转至 `post_exec_child`：调用 `SYS_getpid()` + `SYS_kill(pid, SIGUSR1)` 向自身发送 `SIGUSR1`（`10`）。由于 `SYS_exec` 已将继承自父进程的 `sigusr1_handler` 重置为 `SIG_DFL`，子进程在此被默认终止（`exit_code = 10`）；若 `SYS_kill` 意外返回则调用 `SYS_exit(0xee)`。
2. **负向校验（父进程 `argc == 0`）**：
   - 构造 `sa_handler = 0xffff800000000000` 的 `struct sigaction` 调用 `SYS_signal(SIGUSR1, &bad_act, NULL)`，断言返回 `-EINVAL`（`-22`）；
   - 在栈上构造 `spsr_el1 = 0x5`（EL1h）的伪造 `struct aarch64_sigframe`（280 字节）并调用 `SYS_sigreturn`，断言返回 `-EINVAL`（`-22`）；
   - 将 `sp` 临时减 8（非 16 字节对齐）调用 `SYS_sigreturn` 后恢复 `sp`，断言返回 `-EFAULT`（`-14`）。
3. **`sigprocmask` 阻塞/解阻塞 + 自定义 `sa_handler` / `sa_restorer` / `SYS_sigreturn` 闭环**：
   - 安装 `SIGUSR1`（10）处理器 `sigusr1_handler` 与 `sigreturn_restorer`；
   - 调用 `SYS_sigprocmask(SIG_BLOCK, &usr1_mask, NULL)` 屏蔽 `SIGUSR1`（`usr1_mask = 1ULL << (10 - 1) = 0x200`）；
   - 调用 `SYS_kill(getpid(), SIGUSR1)` 发送给自身，断言返回 `0` 且 `g_sig_count == 0`（被屏蔽未投递）；
   - 将 `x19` 置为 `0xdeadbeef`，调用 `SYS_sigprocmask(SIG_UNBLOCK, &usr1_mask, &old_mask)`：
     - 解屏蔽返回 EL0 时触发 `sigusr1_handler`：handler 校验 `x0 == 10`（`SIGUSR1`）且 `(sp & 0xf) == 0`，将 `g_sig_count` 置 `1`，并修改栈上 `frame.regs.x19`（偏移 `19 * 8 = 152`）为 `0x13572468`，随后 `ret` 到 `sigreturn_restorer` 执行 `mov x8, #43; svc #0`（`SYS_sigreturn`）；
   - 回到主流程后，断言 `SYS_sigprocmask` 返回值 `x0 == 0`、`g_sig_count == 1` 且 `x19 == 0x13572468`（保持 `sigusr1_handler` 仍注册在 `SIGUSR1` 上）。
4. **`fork` 继承 handler + `SYS_exec` 重置为 `SIG_DFL` + 默认致命信号终止 + `waitpid` 回收**：
   - 调用 `SYS_fork()`：
     - 子进程（`x0 == 0`，继承了 `sigusr1_handler`）：在栈上构造 `argv = { "/bin/user_signal", "child", NULL }`（`argc = 2`），调用 `SYS_exec("/bin/user_signal", argv, NULL)`；新镜像在 `_start` 走 `x0 == 2` 分支向自身发 `SIGUSR1` 并因 `SIG_DFL` 以状态 `10`（`SIGUSR1`）终止；
     - 父进程（`x0 > 0`）：调用 `SYS_waitpid(child_pid, &status, 0)`，断言返回值等于 `child_pid` 且 `status == 10`（`SIGUSR1`）。
5. 全部断言通过后调用 `SYS_exit(0x66)`。

- [ ] **Step 3: 编写 `kernel/selftest/test_aarch64_user_signal.c` 并接入构建与自测表**

1. 创建 `kernel/selftest/test_aarch64_user_signal.c`（整文件以 `#if AARCH64_EL0_TEST ... #endif` 门控）：
   - 构造单条目 `catalog[0] = { _binary_aarch64_user_signal_elf_start, ..., "/bin/user_signal" }`；
   - 记录 `alloc_before = pmm_probe_4k_allocated_count()`，打开 `aarch64_sched_observer_begin(0, 0x4d14U)`，挂接 `reap` 观察钩子；
   - 调用 `aarch64_user_task_spawn(catalog, 1, 0, "user_sig")` 启动父进程，循环 `kthread_yield()` 等待父子共 2 个任务退出并回收；
   - 断言子进程 `exit_code == 10`（`SIGUSR1`）、父进程 `exit_code == (0x66 << 8)`、`aarch64_sched_observer_end(0, 0x4d14U) == 0`、`aarch64_pt_published_root_count() == 1` 且 `pmm_probe_4k_allocated_count() == alloc_before`。
2. 在 `kernel/include/arch/aarch64/boot.h` 中将 `#elif AARCH64_EL0_TEST` 下的 `ARCH_SELFTEST_EXPECTED_LATE` 由 `19U` 更新为 `20U`。
3. 在 `kernel/selftest/selftest.c` 注册 `selftest_register_late("aarch64_user_signal")`；在 `kernel/selftest/test_aarch64_scheduler.c` 声明并调用 `selftest_record_late("aarch64_user_signal", test_aarch64_user_signal())`。
4. 在 `kernel/Makefile` 中：
   - `ifeq ($(AARCH64_EL0_TEST),1)` 下加入 `KERNEL_C_SOURCES += selftest/test_aarch64_user_signal.c`；
   - `ifeq ($(ARCH):$(AARCH64_EL0_TEST),aarch64:1)` 下加入 `aarch64_user_signal_image.o` 及对应的汇编、链接、二进制嵌入规则。
5. 更新 `docs/syscall/syscall.md` 与 `docs/roadmap.md`。

- [ ] **Step 4: 运行精简验证门禁确认全部通过**

Run:
```bash
make test-host
make test-static
make PROFILE=aarch64-clang test-static CASE=sched-common-compile
make PROFILE=aarch64-clang test-aarch64-audit
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi
```
Expected:
- `make test-host` 全部 PASS；
- `make test-static`、`CASE=sched-common-compile` 与 `test-aarch64-audit` 全部 PASS；
- `run-aarch64-uefi` 输出 `[TEST] PASS aarch64_user_signal` 与 `[TEST] END suite=kernel-selftest total=27 passed=27 failed=0 skipped=0`。
