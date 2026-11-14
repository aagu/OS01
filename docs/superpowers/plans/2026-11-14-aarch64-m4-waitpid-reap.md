# AArch64 M4.12：原生 `SYS_waitpid` 与父子退出 / ZOMBIE 回收闭环实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 AArch64 单核（BSP）、ASID 0、4 KiB 页与协作式调度模型下开放共享 `SYS_waitpid`（12），将共享 `do_waitpid` / `sched_free_reaped_task` 下沉至跨架构调度层，补齐 `syscall_prepare_user_write` 与 `blocker_wait` 中断掩码保持契约，并打通 AArch64 `fork` 子进程退出转入 `TASK_ZOMBIE`、父进程阻塞/非阻塞等待与孤儿清理的零泄漏闭环。

**Architecture:** 共享系统调用表将 `[SYS_waitpid] = { sys_native_waitpid, "waitpid" }` 纳入双架构共享区，`sys_native_waitpid` 先经 `syscall_prepare_user_write` 完成 `user_status` 写前校验与惰性/COW 页预备，再调用跨架构 `kernel/sched/wait.c::do_waitpid`。AArch64 `aarch64_user_task_exit` 在先行卸载并销毁用户地址空间与用户根后，于 `task_list_lock` 下原子处理运行中孤儿重挂 `idle`、未等待孤儿 ZOMBIE 锁外回收，并按父任务是否为存活 `PF_PROCESS` 决定自身进入 `TASK_ZOMBIE` 留存链表唤醒父进程或走 `PF_SELF_REAP` 自回收。

**Tech Stack:** C11、AArch64 / x86_64 汇编、Clang/LLD、GNU Make、Python `unittest` 静态审计、宿主 C 单测、QEMU `virt,gic-version=3` / x86_64 E2E 测试。

**Spec:** [`docs/superpowers/specs/2026-11-14-aarch64-m4-waitpid-reap-design.md`](../specs/2026-11-14-aarch64-m4-waitpid-reap-design.md)

---

## 文件变动总览

| 文件 | 变更类型 | 职责 |
|---|---|---|
| `kernel/include/memory/uaccess.h` | Modify | 声明跨架构写前校验/预备原语 `int syscall_prepare_user_write(uint64_t addr, uint64_t len)` |
| `kernel/arch/x86_64/memory/uaccess.c` | Modify | 实现 x86_64 `syscall_prepare_user_write`（转发 `syscall_check_user_range(addr, len, true)`） |
| `kernel/arch/aarch64/memory/uaccess.c` | Modify | 实现 AArch64 `syscall_prepare_user_write`（快照预检 + `AARCH64_RANGE_PERMISSION` 时调用 `aarch64_user_vm_prepare_copy` + 二次预检） |
| `kernel/include/sched/internal.h` | Modify | 声明 `void sched_free_reaped_task(task_t *child)` |
| `kernel/sched/kthread.c` | Modify | 实现 `sched_free_reaped_task(task_t *child)`（复用 `kthread_observe_free` 释放 `thread` / `fpu_save` / `stack_alloc_base`） |
| `kernel/sched/core.c` | Modify | `blocker_wait` 保存入口 `entry_flags` 并在 `schedule()` 返回后执行 `arch_local_irq_restore(entry_flags)`，保持调用方进入时的中断状态 |
| `kernel/sched/wait.c` | Create | 跨架构共享 `waitpid_should_unblock` 与 `do_waitpid`（自 `kernel/arch/x86_64/sched/exit.c` 迁出并改用 `sched_free_reaped_task`） |
| `kernel/arch/x86_64/sched/exit.c` | Modify | 移除已迁入 `kernel/sched/wait.c` 的 `waitpid_should_unblock` 与 `do_waitpid` |
| `kernel/include/arch/syscall.h` | Modify | 声明 `int64_t sys_native_waitpid(syscall_ctx_t *ctx)` |
| `kernel/syscall/dispatch.c` | Modify | 将 `[SYS_waitpid] = { sys_native_waitpid, "waitpid" }` 移入双架构共享区 |
| `kernel/syscall/sys_native.c` | Modify | 实现 `sys_native_waitpid(syscall_ctx_t *ctx)`（32 位 `pid_t` 符号扩展 + `syscall_prepare_user_write` + `do_waitpid`） |
| `kernel/arch/x86_64/syscall/sys_proc.c` | Modify | 移除 `sys_proc_dispatch` 中已由 `sys_native_waitpid` 接管的 `case SYS_waitpid:` |
| `kernel/arch/aarch64/cpu/user_task.c` | Modify | `aarch64_user_task_exit` 实现未等待孤儿 ZOMBIE 子进程摘除与锁外回收，以及父任务为存活 `PF_PROCESS` 时进入 `TASK_ZOMBIE` 留存链表并唤醒父进程 |
| `kernel/Makefile` | Modify | AArch64 `KERNEL_C_SOURCES` 与 `SCHED_COMMON_COMPILE_TUS` 加入 `sched/wait.c`；`AARCH64_EL0_TEST=1` 新增 `test_aarch64_user_waitpid` 与测试固件规则 |
| `docs/syscall/syscall.md` | Modify | 标注 `SYS_waitpid`（12）为 x86_64 + AArch64 共享原生入口 |
| `hosttests/mock/switch_complete_test_runtime.h` | Modify | 生产分支补齐 `void sched_free_reaped_task(task_t *child);` 前置声明 |
| `hosttests/cases/test_syscall_dispatch.c` | Modify | `SYS_waitpid` 移入 `shared[]` 列表并新增 `sys_native_waitpid` 计数桩 |
| `hosttests/cases/test_compat_linux.c` | Modify | 补充 `sys_native_waitpid` 桩 |
| `hosttests/cases/test_nofile_limits.c` | Modify | 补充 `sys_native_waitpid` 桩 |
| `hosttests/cases/test_blocker_remote_wake.c` | Modify | 新增验证 `blocker_wait` 对关中断与开中断调用方均精确恢复入口中断掩码的回归用例 |
| `hosttests/cases/test_waitpid.c` | Create | 链接真实 `kernel/sched/wait.c` 与 `kernel/syscall/sys_native.c` 的宿主单测套件 |
| `hosttests/mk/sched.mk` | Modify | 注册并构建 `test_waitpid` |
| `kernel/include/arch/aarch64/boot.h` | Modify | `AARCH64_EL0_TEST` 下 `ARCH_SELFTEST_EXPECTED_LATE` 由 `17U` 更新为 `18U` |
| `kernel/selftest/selftest.c` | Modify | `AARCH64_EL0_TEST` 分支注册 `selftest_register_late("aarch64_user_waitpid")` |
| `kernel/selftest/test_aarch64_scheduler.c` | Modify | `AARCH64_EL0_TEST` 尾部调用并记录 `test_aarch64_user_waitpid()` |
| `kernel/selftest/test_aarch64_user_fork_cow.c` | Modify | `aarch64_user_fork_cow_test_reap` 将非退出切换过滤条件由 `!(prev->flags & PF_SELF_REAP)` 更新为 `prev->state != TASK_ZOMBIE` |
| `kernel/selftest/fixtures/aarch64_user_waitpid.S` | Create | `aarch64_user_waitpid` EL0 验收汇编固件（覆盖阻塞等待、`WNOHANG`、指定 PID/`-1`、COW 栈/惰性匿名页写回、坏指针 `-EFAULT` 不丢 ZOMBIE、孤儿与未等待 ZOMBIE 清理） |
| `kernel/selftest/test_aarch64_user_waitpid.c` | Create | 第 25 项 AArch64 内核自测驱动与零泄漏闭环断言 |
| `qemutests/intr_dispatch_audit.py` | Modify | 新增 `syscall_prepare_user_write` 与 `sys_native_waitpid -> do_waitpid` 审计要求 |
| `qemutests/test_intr_dispatch_audit.py` | Modify | 同步合成符号表/反汇编夹具并增加 `sys_native_waitpid -> do_waitpid` 负向缺失测试 |
| `docs/build/build.md` | Modify | 更新 `AARCH64_EL0_TEST=1` 自测说明（新增 `aarch64_user_waitpid`，总数 `24 -> 25`） |
| `docs/roadmap.md` | Modify | 记录 AArch64 M4.12 `SYS_waitpid` 与父子退出 / ZOMBIE 回收闭环状态 |

---

### Task 1: `blocker_wait` 调用方中断状态保持与 `sched_free_reaped_task` 抽取

**Files:**
- Modify: `kernel/include/sched/internal.h`
- Modify: `kernel/sched/kthread.c`
- Modify: `kernel/sched/core.c`
- Modify: `hosttests/mock/switch_complete_test_runtime.h`
- Modify: `hosttests/cases/test_blocker_remote_wake.c`

- [ ] **Step 1: 在 `hosttests/cases/test_blocker_remote_wake.c` 中新增 `blocker_wait` 关中断调用方状态保持失败测试（RED）**

复用 `hosttests/mock/blocker_runtime.h` 已有的 `mock_irq_state`（`0` 表示开中断，`1` 表示关中断；`__real_schedule()` 切往 `idle` 返回时 `mock_irq_state` 为 `1`），在 `hosttests/cases/test_blocker_remote_wake.c` 中新增测试函数验证：
1. 调用方在 `mock_irq_state == 0`（开中断）下调用 `blocker_wait` 阻塞唤醒后返回时，`mock_irq_state == 0`；
2. 调用方在 `mock_irq_state == 1`（关中断，如 AArch64 EL0 `svc #0`）下调用 `blocker_wait` 阻塞唤醒后返回时，`mock_irq_state == 1`。

- [ ] **Step 2: 运行 `test-blocker-remote-wake` 验证 RED**

Run:
```bash
make -C hosttests test-blocker-remote-wake
```
Expected: FAIL（现行 `blocker_wait` 在 `schedule()` 后无条件调用 `arch_local_irq_enable()`，导致关中断调用方返回时 `mock_irq_state` 变为 `0`）。

- [ ] **Step 3: 修改 `kernel/sched/core.c`、`kernel/include/sched/internal.h`、`kernel/sched/kthread.c` 与 `hosttests/mock/switch_complete_test_runtime.h`（GREEN）**

1. 在 `kernel/sched/core.c::blocker_wait` 中保留入口 `entry_flags` 并在 `schedule()` 返回后执行 `arch_local_irq_restore(entry_flags)`：
```c
    uint64_t entry_flags = spin_lock_irqsave(&task_list_lock);
    ...
    spin_unlock_irqrestore(&task_list_lock, entry_flags);

#if defined(OS01_SELFTEST) && defined(__aarch64__)
    aarch64_sched_before_block_switch(self);
#endif
    schedule();
    arch_local_irq_restore(entry_flags);
    // Woke up — clear blocker and check why (back under
    // task_list_lock: the waker cleared it under the same lock).
    uint64_t resume_flags = spin_lock_irqsave(&task_list_lock);
    ...
    spin_unlock_irqrestore(&task_list_lock, resume_flags);
    return interrupted ? -EINTR : 0;
```
2. 在 `kernel/include/sched/internal.h` 声明（并在 `hosttests/mock/switch_complete_test_runtime.h` 的 `#else` 生产对象分支同步声明）：
```c
void sched_free_reaped_task(task_t *child);
```
3. 在 `kernel/sched/kthread.c` 实现 `sched_free_reaped_task`：
```c
void sched_free_reaped_task(task_t *child)
{
    if (!child)
        return;
    if (child->thread)
        kthread_observe_free(child->thread, 2);
    if (child->fpu_save)
        kthread_observe_free(child->fpu_save, 3);
    if (child->stack_alloc_base)
        kthread_observe_free(child->stack_alloc_base, 1);
}
```

- [ ] **Step 4: 运行宿主调度测试验证 GREEN**

Run:
```bash
make -C hosttests test-blocker-remote-wake test-switch-complete test-kthread-ctor
```
Expected: 全部 PASS。

---

### Task 2: `syscall_prepare_user_write`、共享 `do_waitpid` 与 `sys_native_waitpid` 下沉及宿主单测

**Files:**
- Modify: `kernel/include/memory/uaccess.h`
- Modify: `kernel/arch/x86_64/memory/uaccess.c`
- Modify: `kernel/arch/aarch64/memory/uaccess.c`
- Create: `kernel/sched/wait.c`
- Modify: `kernel/arch/x86_64/sched/exit.c`
- Modify: `kernel/include/arch/syscall.h`
- Modify: `kernel/syscall/dispatch.c`
- Modify: `kernel/syscall/sys_native.c`
- Modify: `kernel/arch/x86_64/syscall/sys_proc.c`
- Modify: `kernel/Makefile`
- Modify: `docs/syscall/syscall.md`
- Modify: `hosttests/cases/test_syscall_dispatch.c`
- Modify: `hosttests/cases/test_compat_linux.c`
- Modify: `hosttests/cases/test_nofile_limits.c`
- Create: `hosttests/cases/test_waitpid.c`
- Modify: `hosttests/mk/sched.mk`

- [ ] **Step 1: 更新 `hosttests/cases/test_syscall_dispatch.c` 并编写 `hosttests/cases/test_waitpid.c`（RED）**

1. 在 `hosttests/cases/test_syscall_dispatch.c` 中：
   - 新增 `sys_native_waitpid` 计数桩，将 `SYS_waitpid` 加入 `shared[]` 数组（共 10 个共享号：4 原始 + 4 VM + `SYS_fork` + `SYS_waitpid`）；
   - 验证全量表与 `OS01_SYSCALL_A64_SUBSET=1` 子集表下 `syscall_has_handler(SYS_waitpid) == true`、`syscall_name(SYS_waitpid)` 为 `"waitpid"`、分发命中 `sys_native_waitpid`。
2. 在 `hosttests/cases/test_compat_linux.c` 与 `hosttests/cases/test_nofile_limits.c` 中补充 `sys_native_waitpid` 桩。
3. 创建 `hosttests/cases/test_waitpid.c` 并在 `hosttests/mk/sched.mk` 注册构建规则（链接真实的 `kernel/sched/wait.c` 与 `kernel/syscall/sys_native.c`），覆盖以下用例：
   - `test_waitpid_reap_specific_and_any`：`pid > 0` 与 `pid == -1`（含 `sys_native_waitpid` 下 32 位 `0xffffffffULL` 符号扩展为 `-1`）成功摘除 `TASK_ZOMBIE`（`on_cpu == 0`）、写回 32 位 `status`、调用 `sched_free_reaped_task` 并返回子 PID；
   - `test_waitpid_null_status`：`user_status == NULL` 时不调用 `syscall_prepare_user_write` / `copy_to_user_ft`，正常回收子任务；
   - `test_waitpid_wnohang_and_on_cpu_guard`：子进程为 `TASK_RUNNING` 或 `TASK_ZOMBIE` 但 `on_cpu == 1` 时，`WNOHANG` 返回 `0` 且子任务保留在链表；
   - `test_waitpid_blocking_wakeup_and_eintr_retry`：子进程未就绪时调用 `blocker_wait(waitpid_should_unblock, BLOCKER_WAITPID, true)`，模拟 `-EINTR` 重试及子进程变为 `TASK_ZOMBIE`（`on_cpu == 0`）后 `waitpid_should_unblock` 返回 `true` 并完成回收；
   - `test_waitpid_echild_cases`：无子进程、非自身子进程、`pid == 0`、`pid == -2`、以及同一子进程第二次重复 `waitpid` 均返回 `-ECHILD`；
   - `test_waitpid_bad_status_preserves_zombie_and_rejects_without_children`：`syscall_prepare_user_write` 返回 `-EFAULT` 或 `-ENOMEM` 时，即便无子进程也返回 `-EFAULT`/`-ENOMEM`；有可回收 ZOMBIE 子进程时绝不摘除或释放该子任务，紧接着传入合法 `&status` 再次调用 `sys_native_waitpid` 仍能成功回收该子进程。

- [ ] **Step 2: 运行宿主测试验证 RED**

Run:
```bash
make -C hosttests test-syscall-dispatch
```
Expected: FAIL（`SYS_waitpid` 尚未移入 `dispatch.c` 共享区，`kernel/sched/wait.c` 与 `sys_native_waitpid` 尚未创建）。

- [ ] **Step 3: 实现 `syscall_prepare_user_write`、`kernel/sched/wait.c` 与 `sys_native_waitpid`（GREEN）**

1. `kernel/include/memory/uaccess.h`：声明 `int syscall_prepare_user_write(uint64_t addr, uint64_t len);`。
2. `kernel/arch/x86_64/memory/uaccess.c`：
```c
int syscall_prepare_user_write(uint64_t addr, uint64_t len)
{
    if (len == 0)
        return 0;
    return syscall_check_user_range(addr, len, true) ? 0 : -EFAULT;
}
```
3. `kernel/arch/aarch64/memory/uaccess.c`：
```c
int syscall_prepare_user_write(uint64_t addr, uint64_t len)
{
    if (len == 0)
        return 0;
    if (len > (uint64_t)SSIZE_MAX)
        return -EFAULT;

    arch_irq_state_t state = arch_local_irq_save();
    struct mm_struct *mm = NULL;
    uint64_t root_pa = 0;
    enum aarch64_range_result r =
        aarch64_user_range_precheck(addr, (size_t)len, true, false,
                                    &mm, &root_pa);
    if (r == AARCH64_RANGE_OK) {
        arch_local_irq_restore(state);
        return 0;
    }
    if (r == AARCH64_RANGE_DENIED) {
        arch_local_irq_restore(state);
        return -EFAULT;
    }

    int prc = aarch64_user_vm_prepare_copy(mm, addr, (size_t)len, true);
    if (prc == -ENOMEM) {
        arch_local_irq_restore(state);
        return -ENOMEM;
    }
    if (prc != 0) {
        arch_local_irq_restore(state);
        return -EFAULT;
    }

    mm = NULL;
    root_pa = 0;
    bool ok = (aarch64_user_range_precheck(addr, (size_t)len, true, false,
                                           &mm, &root_pa) == AARCH64_RANGE_OK);
    arch_local_irq_restore(state);
    return ok ? 0 : -EFAULT;
}
```
4. 创建 `kernel/sched/wait.c`：从 `kernel/arch/x86_64/sched/exit.c` 迁入 `waitpid_should_unblock` 与 `do_waitpid`，将三处 `kfree(child->...)` 替换为 `sched_free_reaped_task(child)`；从 `kernel/arch/x86_64/sched/exit.c` 删除原实现。
5. 在 `kernel/include/arch/syscall.h` 与 `kernel/syscall/dispatch.c` 声明 `int64_t sys_native_waitpid(syscall_ctx_t *ctx);`，在 `kernel/syscall/dispatch.c` 共享区挂接 `[SYS_waitpid] = { sys_native_waitpid, "waitpid" }` 并删除 `#ifndef OS01_SYSCALL_A64_SUBSET` 中的旧表项；从 `kernel/arch/x86_64/syscall/sys_proc.c` 删除 `case SYS_waitpid:`。
6. 在 `kernel/syscall/sys_native.c` 实现 `sys_native_waitpid`：
```c
int64_t sys_native_waitpid(syscall_ctx_t *ctx)
{
    int64_t pid = (int64_t)(int)ctx->args[0];
    uint64_t status_addr = ctx->args[1];
    int options = (int)ctx->args[2];

    if (status_addr) {
        int rc = syscall_prepare_user_write(status_addr, sizeof(int));
        if (rc < 0)
            return rc;
    }
    return do_waitpid(pid, (int *)(uintptr_t)status_addr, options);
}
```
7. 在 `kernel/Makefile` 的 `ifeq ($(ARCH),aarch64)` `KERNEL_C_SOURCES` 与 `SCHED_COMMON_COMPILE_TUS` 中加入 `sched/wait.c`，并更新 `docs/syscall/syscall.md`。

- [ ] **Step 4: 运行宿主单测与静态边界审计验证 GREEN**

Run:
```bash
make test-host
python3 qemutests/syscall_boundary_audit.py
python3 qemutests/mm_header_boundary.py
make PROFILE=aarch64-clang test-static CASE=sched-common-compile
```
Expected: 全部 PASS。

---

### Task 3: AArch64 `aarch64_user_task_exit` ZOMBIE 留存、父唤醒与孤儿 ZOMBIE 回收

**Files:**
- Modify: `kernel/arch/aarch64/cpu/user_task.c`
- Modify: `kernel/selftest/test_aarch64_user_fork_cow.c`

- [ ] **Step 1: 更新 `kernel/arch/aarch64/cpu/user_task.c::aarch64_user_task_exit` 生命周期**

在 `kernel/arch/aarch64/cpu/user_task.c` 中包含 `<sched/internal.h>`，并在 `aarch64_user_task_exit(uint64_t status)` 完成 `aarch64_user_restore_kernel_root()`、`aarch64_user_image_destroy(image)`、`kfree(owner)` 及 `g_stats` 计数后，按 spec §4.2 实现原子重挂、孤儿 ZOMBIE 收集与自身退出分支：

```c
    task_t *idle = percpu_data[0].idle;
    list_t reap_head;
    list_init(&reap_head);
    bool waitable_parent = false;

    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    if (idle) {
        list_t *p = init_task_union.task.list.next;
        while (p != &init_task_union.task.list) {
            task_t *child = container_of(p, task_t, list);
            p = p->next;
            if (child->parent != t)
                continue;
            child->parent = idle;
            if (child->state == TASK_ZOMBIE) {
                list_del(&child->list);
                if (__atomic_load_n(&child->on_cpu, __ATOMIC_ACQUIRE) == 0) {
                    list_add_to_before(&reap_head, &child->list);
                } else {
                    child->list.next = NULL;
                    child->list.prev = NULL;
                    child->flags |= PF_SELF_REAP;
                }
            }
        }
    }

    task_t *parent = t->parent;
    if (parent && parent != idle &&
        (parent->flags & PF_PROCESS) &&
        parent->state != TASK_ZOMBIE) {
        waitable_parent = true;
        t->exit_code = status;
        __atomic_store_n(&t->on_cpu, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);

    while (!list_is_empty(&reap_head)) {
        list_t *node = reap_head.next;
        list_del(node);
        node->next = NULL;
        node->prev = NULL;
        task_t *orphan = container_of(node, task_t, list);
        sched_free_reaped_task(orphan);
    }

    if (!waitable_parent)
        task_exit_self_reap(status);

    task_wake_parent(t);
    task_exit_zombie();
    schedule();
    for (;;)
        arch_cpu_halt();
```

- [ ] **Step 2: 更新 `kernel/selftest/test_aarch64_user_fork_cow.c` 退出切换观测过滤条件**

在 `kernel/selftest/test_aarch64_user_fork_cow.c::aarch64_user_fork_cow_test_reap` 中，将：
```c
    if (!prev || !(prev->flags & PF_SELF_REAP))
        return;
```
更新为：
```c
    if (!prev || prev->state != TASK_ZOMBIE)
        return;
```
使 `FC_MODE_SHORT` 中成为 `TASK_ZOMBIE` 的短命子进程在 `task_switch_complete` 切出时照常记录栈切换安全，并在 `short_parent` 退出时由上述 `reap_head` 孤儿 ZOMBIE 清理路径自动释放。

- [ ] **Step 3: 编译 AArch64 生产与 `AARCH64_EL0_TEST=1` 镜像验证现有 24 项自测无回归**

Run:
```bash
make PROFILE=aarch64-clang test-static CASE=sched-common-compile
make PROFILE=aarch64-clang test-static CASE=mm-header-boundary
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi
```
Expected: `test-aarch64 MODE=smp` 输出 `[TEST] END suite=kernel-selftest total=19 passed=19 failed=0 skipped=0`；`AARCH64_EL0_TEST=1` 串口输出 `[TEST] END suite=kernel-selftest total=24 passed=24 failed=0 skipped=0`（含 `aarch64_user_fork_cow` 的 `FC_MODE_MAIN` 与 `FC_MODE_SHORT`）。

---

### Task 4: 新增第 25 项 QEMU 端到端自测 `aarch64_user_waitpid` 与生产镜像静态审计

**Files:**
- Create: `kernel/selftest/fixtures/aarch64_user_waitpid.S`
- Create: `kernel/selftest/test_aarch64_user_waitpid.c`
- Modify: `kernel/include/arch/aarch64/boot.h`
- Modify: `kernel/selftest/selftest.c`
- Modify: `kernel/selftest/test_aarch64_scheduler.c`
- Modify: `kernel/Makefile`
- Modify: `qemutests/intr_dispatch_audit.py`
- Modify: `qemutests/test_intr_dispatch_audit.py`
- Modify: `docs/build/build.md`
- Modify: `docs/roadmap.md`

- [ ] **Step 1: 在 `qemutests/intr_dispatch_audit.py` 与 `qemutests/test_intr_dispatch_audit.py` 中增加 `SYS_waitpid` 接线与符号审计（RED -> GREEN）**

1. 在 `qemutests/intr_dispatch_audit.py` 中：
   - `_A64_DISASM_SYMS` 加入 `"sys_native_waitpid"`、`"do_waitpid"`；
   - `_A64_UACCESS_SYMS` 加入 `"syscall_prepare_user_write"`；
   - `_A64_VM_EDGES` 加入 `("sys_native_waitpid", "do_waitpid", "the shared waitpid handler must reach do_waitpid")`。
2. 在 `qemutests/test_intr_dispatch_audit.py` 中：
   - 在 `A64_NM` 与 `A64_DISASM` 合成夹具中补齐 `syscall_prepare_user_write`、`sys_native_waitpid`、`do_waitpid` 及 `sys_native_waitpid -> do_waitpid` 调用指令；
   - 新增负向单测验证缺少 `sys_native_waitpid -> do_waitpid` 边或缺少 `syscall_prepare_user_write` 时审计失败。
3. 运行 `python3 -m unittest qemutests/test_intr_dispatch_audit.py` 验证通过。

- [ ] **Step 2: 编写 EL0 汇编固件 `kernel/selftest/fixtures/aarch64_user_waitpid.S`**

复用 `selftest/fixtures/aarch64_user_task.ld` 链接布局（代码页 `0x400000`、RW 数据页 `0x401000`、用户栈 `0x7ffff000..0x80000000`），在一个自包含 EL0 程序中依次执行以下 4 个阶段，任何阶段断言失败即以非零错误码 `SYS_exit(step)` 退出，全部通过则以 `SYS_exit(0x66)` 退出：
1. **Stage 1（阻塞式 `waitpid` + 父栈 COW 页写回）**：
   - `SYS_fork` 创建子进程 1；子进程 1 连续执行 3 次 `svc #0x4d40`（`AARCH64_USER_SVC_YIELD`）后调用 `SYS_exit(0x2a)`；
   - 父进程不写栈、直接将未拆分 COW 栈上的 `&status` 传给 `SYS_waitpid(child1_pid, &status, 0)` 阻塞等待；
   - 验证返回值等于 `child1_pid` 且栈上 `status == (0x2a << 8)`。
2. **Stage 2（`WNOHANG` 轮询 + 指定 PID 与 `pid == -1` 回收）**：
   - 父进程连续 `SYS_fork` 出子进程 2（先执行 2 次 `svc #0x4d40` 再 `SYS_exit(0x11)`）与子进程 3（先执行 2 次 `svc #0x4d40` 再 `SYS_exit(0x22)`）；
   - 子进程 2 仍在运行时，父进程调用 `SYS_waitpid(child2_pid, &status, WNOHANG)`，验证返回 `0`；
   - 父进程循环 `svc #0x4d40` 直至 `SYS_waitpid(child3_pid, &status, WNOHANG)` 返回 `child3_pid`，验证 `status == (0x22 << 8)`；
   - 父进程调用 `SYS_waitpid(-1, &status, 0)`（传入 `w0 = -1`），验证返回 `child2_pid` 且 `status == (0x11 << 8)`。
3. **Stage 3（坏指针 `-EFAULT` 不丢 ZOMBIE + 惰性匿名页写回 + `-ECHILD`）**：
   - 父进程调用 `SYS_mmap` 申请 2 页 `PROT_READ | PROT_WRITE` 匿名页（第 1 页完全不触碰保持惰性未映射，第 2 页调用 `SYS_mprotect` 设为 `PROT_READ`），并 `SYS_fork` 出子进程 4（立即 `SYS_exit(0x55)`）；
   - 父进程执行一次 `svc #0x4d40`（`AARCH64_USER_SVC_YIELD`）使子进程 4 先退出进入 `TASK_ZOMBIE`（`on_cpu == 0`），随后依次以 `user_status = (int *)0x1` 与 `PROT_READ` 页地址调用 `SYS_waitpid(child4_pid, bad_ptr, 0)`，验证两次均返回 `-EFAULT`（`-14`）且未丢失子进程 4；
   - 父进程将未触碰的惰性 RW `mmap` 页地址作为 `user_status` 调用 `SYS_waitpid(child4_pid, lazy_rw_ptr, 0)`，验证返回 `child4_pid` 且该页读出 `0x55 << 8`（`0x5500`）；
   - 紧接着调用 `SYS_waitpid(child4_pid, NULL, 0)` 与 `SYS_waitpid(-1, NULL, 0)`，验证均返回 `-ECHILD`（`-10`）；调用 `SYS_waitpid(-1, (int *)0x1, 0)`，验证无子进程时坏指针仍优先返回 `-EFAULT`（`-14`）。
4. **Stage 4（未等待孤儿 ZOMBIE 子进程与存活孤儿进程清理）**：
   - 父进程 `SYS_fork` 出子进程 5（立即 `SYS_exit(0x33)` 成为 ZOMBIE，父进程故意不 wait）与子进程 6（循环 `svc #0x4d40` 并轮询 `SYS_getppid()` 直至等于 `0` 即 `idle`，随后 `SYS_exit(0x44)`）；
   - 父进程执行一次 `svc #0x4d40` 使子进程 5 先进入 `TASK_ZOMBIE`，随后父进程直接调用 `SYS_exit(0x66)`。

- [ ] **Step 3: 编写 `kernel/selftest/test_aarch64_user_waitpid.c` 并接入自测框架与 `kernel/Makefile`**

1. 创建 `kernel/selftest/test_aarch64_user_waitpid.c`（整文件以 `#if AARCH64_EL0_TEST ... #endif` 门控，使 `x86_64` 的 `$(wildcard selftest/*.c)` 与普通 AArch64 构建均为空编译单元）：
   - 调用 `aarch64_user_task_test_reset_stats()`，记录 `alloc_before = pmm_probe_4k_allocated_count()`；
   - 打开独立观察批次 `aarch64_sched_observer_begin(0, 0x4d12U)`，记录 `created_before = aarch64_sched_observer_created()`、`stack_rel_before = aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK)`、`thread_rel_before = aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD)`，并挂接 `aarch64_sched_set_reap_observer` 记录所有切出 `TASK_ZOMBIE` 任务的 PID 与 `exit_code`；
   - 通过 `aarch64_user_task_spawn` 启动 `_binary_aarch64_user_waitpid_elf_start`，循环 `kthread_yield()` 等待全部 7 个任务（1 个父进程 + 6 个 `fork` 子进程）退出且完成栈/线程上下文释放（`stack_released - stack_rel_before == 7` 且 `thread_released - thread_rel_before == 7`）；
   - 断言：
     - `aarch64_sched_observer_created() - created_before == 14`（每个任务记录 stack + thread 共 2 条记录，`2 * 7 == 14`）；
     - `aarch64_sched_observer_end(0, 0x4d12U) == 0`（批次内所有分配记录全部转为 `OBS_FREED` 且零错误标志）；
     - `aarch64_user_task_test_stats(&s)` 中 `s.exits == 7`，且 reap observer 观测到父进程以 `0x66 << 8`、6 个子进程分别以 `0x2a << 8`、`0x11 << 8`、`0x22 << 8`、`0x55 << 8`、`0x33 << 8`、`0x44 << 8` 退出；
     - `aarch64_pt_published_root_count() == 1` 且 `pmm_probe_4k_allocated_count() == alloc_before`。
2. 在 `kernel/Makefile` 中：
   - `ifeq ($(AARCH64_EL0_TEST),1)` 下加入 `KERNEL_C_SOURCES += selftest/test_aarch64_user_waitpid.c` 与 `KERNEL_EXTRA_OBJECTS += $(BUILD_DIR)/generated/aarch64_user_waitpid_image.o` 及对应的 `.o -> .elf -> _image.o` 规则。
3. 在 `kernel/include/arch/aarch64/boot.h` 将 `AARCH64_EL0_TEST` 下的 `ARCH_SELFTEST_EXPECTED_LATE` 更新为 `18U`；在 `kernel/selftest/selftest.c` 注册 `selftest_register_late("aarch64_user_waitpid")`；在 `kernel/selftest/test_aarch64_scheduler.c` 尾部调用并记录 `selftest_record_late("aarch64_user_waitpid", test_aarch64_user_waitpid())`。
4. 同步更新 `docs/build/build.md`（`AARCH64_EL0_TEST=1` 通过项加入 `aarch64_user_waitpid`，总数由 `total=24 passed=24` 更新为 `total=25 passed=25`）与 `docs/roadmap.md`。

- [ ] **Step 4: 运行全套静态审计、宿主测试、AArch64 QEMU 自测与 x86_64 回归**

Run:
```bash
make test-host
make test-static
make PROFILE=aarch64-clang test-static CASE=sched-common-compile
make PROFILE=aarch64-clang test-static CASE=mm-header-boundary
make PROFILE=aarch64-clang test-aarch64-audit
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi
make OS01_SYSTEST=1 test-qemu SUITE=systest
```
Expected:
- `make test-host` 全部 PASS（含 `test_waitpid`、`test_syscall_dispatch`、`test_blocker_remote_wake`）；
- `make test-static`、`CASE=sched-common-compile`、`CASE=mm-header-boundary` 与 `test-aarch64-audit` 全部 PASS（`intr_dispatch_audit.py` 证明生产 AArch64 镜像包含 `sys_native_waitpid -> do_waitpid` 与 `syscall_prepare_user_write` 且零测试符号/测试 ELF 泄漏）；
- `make PROFILE=aarch64-clang test-aarch64 MODE=smp` 输出 `total=19 passed=19 failed=0 skipped=0`；
- `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi` 输出 `[TEST] PASS aarch64_user_waitpid` 与 `[TEST] END suite=kernel-selftest total=25 passed=25 failed=0 skipped=0`；
- `make OS01_SYSTEST=1 test-qemu SUITE=systest` 122/122 PASS。
