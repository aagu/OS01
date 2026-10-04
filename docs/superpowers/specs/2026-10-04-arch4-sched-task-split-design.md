---
title: OS01 ARCH-4 sched/task.c 拆分方案设计
created: 2026-10-04
updated: 2026-10-04
type: spec
status: draft-for-review
tags: [osdev, sched, architecture, refactor]
related: [docs/roadmap.md ARCH-4, docs/superpowers/specs/2026-10-04-arch1-syscall-layer-design.md, docs/sched/scheduler.md]
---

# OS01 ARCH-4 sched/task.c 拆分方案设计

## 1. 背景与现状调研

### 1.1 现状全景
`kernel/sched/task.c` 目前共 **2785 行**，是内核中最大的单体文件之一。它历史演进跨越了多个开发阶段，承载了原本不属于同一职责层次的多个功能模块：

| 模块序号 | 功能职责 | 当前所在行范围 | 代码行数 | 主要函数与符号 |
|---|---|---|---|---|
| **M1** | 全局任务数据与链表操作 | L54-104 | ~50 行 | `init_mm`, `init_thread`, `init_task_union`, `init_task[NR_CPUS]`, `task_list_lock`, `task_list_add`, `task_list_next` |
| **M2** | EEVDF 公平调度引擎 | L105-108, L156-220 | ~80 行 | `update_curr`, `cmp_deadline`, `enqueue_task`, `dequeue_task`, `pick_eevdf` |
| **M3** | SMP 放置与负载均衡 | L113-155, L456-554 | ~140 行 | `sched_pick_cpu`, `sched_notify_remote`, `sched_balance` |
| **M4** | 调度核心与上下文唤醒 | L221-293, L308-455, L555-770, L2604-2744 | ~550 行 | `schedule`, `task_finish_switch`, `task_wake`, `blocker_wait`, `blocker_wake`, `sched_unblock_blocked`, `idle_task_resume`, `task_init` |
| **M5** | 进程退出与收割 (Exit/Wait) | L771-1127, L2581-2603 | ~380 行 | `do_exit`, `waitpid_should_unblock`, `do_waitpid`, `task_files_pin_by_pid` |
| **M6** | 映像加载与用户栈 (Exec/Spawn) | L1128-1821, L2745-2784 | ~740 行 | `fpu_area_alloc`, `startup_args_count`, `setup_user_stack_check_capacity`, `setup_user_stack`, `destroy_unpublished_user_mm`, `spawn_user_task`, `sys_exec`, `task_selftest_auxv_probe` |
| **M7** | 历史残留文件系统系统调用 | L31-45, L1822-2016 | ~210 行 | `COPY_USER_STR`, `sys_symlink`, `sys_readlink`, `sys_lstat`, `sys_fstatat` |
| **M8** | 进程创建与 COW 复制 (Fork) | L2017-2528 | ~530 行 | `fork_mm_copy`, `do_fork`, `kernel_thread`, `create_kthread` |
| **M9** | 信号投递与进程组广播 | L2525-2580 | ~60 行 | `task_send_signal`, `signal_pgrp` |

### 1.2 耦合关系与状态共享分析
经过对 `task.c` 内部调用图和状态引用的静态分析，内部状态和跨模块调用具有高度清晰的边界：

1. **共享状态 (Global State)**：
   - `init_task_union` / `init_mm` / `task_list_lock`：整个调度与生命周期的核心全局对象（ARCH-3 已消除头定义 muldefs，规范定于 C 源文件）。
   - `pid_counter` (L296)：全局原子 PID 计数器，仅在 `do_fork` (Fork) 和 `spawn_user_task` (Exec) 中分配新 PID。
   - `user_init_task` / `user_init_pid` (L301-302)：首个用户进程指针与 PID，由 `spawn_user_task` 在生成 `/init.elf` 时设置；`sched_balance` 读取其避免抢走 init；`do_exit` 读取其保护 init 不退出，并在父进程退出时将孤儿进程重定向过继给 init。
2. **调度引擎接口 (Sched Engine Interface)**：
   - `enqueue_task` / `dequeue_task` / `pick_eevdf` / `update_curr` 构成 EEVDF 调度器的自闭环算法；外部调用点仅为 `schedule`（切出/切入）、`task_wake`（唤醒入队）、`sched_balance`（跨核窃取）、`do_fork`（新建入队）和 `spawn_user_task`（新建入队）。
3. **SMP 负载平衡接口 (Balance Interface)**：
   - `sched_pick_cpu`（选择负载最小 CPU）和 `sched_notify_remote`（发送 reschedule IPI）仅在任务新建（`do_fork` / `spawn_user_task`）后调用；
   - `sched_balance` 仅在 `schedule()` 调度主循环中被触发。
4. **用户栈与 auxv 构造**：
   - `setup_user_stack` 与 `destroy_unpublished_user_mm` 是 Exec 与 Spawn 共享的底层基础设施。`fpu_area_alloc` 被 Fork 与 Exec 共用。
5. **FS 系统调用完全异类**：
   - `sys_symlink`, `sys_readlink`, `sys_lstat`, `sys_fstatat` 仅调用 VFS（`vfs_lookup_at`, `vfs_stat` 等）与用户拷贝函数，完全不碰任何调度器内部结构，其留在 `sched/task.c` 纯属历史遗留（此前为与 `COPY_USER_STR` 暂存一起）。

### 1.3 关键暗桩与工程陷阱（调研发现）
1. **静态代码审计依赖**：
   `mk/components/run.mk:566` 中的 `test-runtime` 阶段直接硬编码检查了 `$(KERNEL_BUILD_DIR)/sched/task.o`：
   ```makefile
   python3 qemutests/stack_canary_audit.py \
     --object "$(KERNEL_BUILD_DIR)/sched/task.o" \
     ...
   ```
   如果直接删除 `task.o` 而不同步调整 `run.mk` 指向 `core.o`（或其他具备 `-fstack-protector-strong` 的调度核心目标），`make test-static` 将直接报错。
2. **宿主单元测试源码探测 (Source-level Slurping)**：
   - `hosttests/cases/test_fork_user_map.c:454`: 测试硬编码打开并读取 `kernel/sched/task.c`，断言 `fork_mm_copy` 与 `do_fork` 存在以及断言 PMD 共享回退已清除。
   - `hosttests/cases/test_process_image_lifecycle.c:462`: 测试硬编码读取 `kernel/sched/task.c`，断言 `spawn_user_task` 的生命周期与回滚清理逻辑。
   文件拆分后，必须同步将测试中的源码搜索路径更新为对应的新目标文件（`sched/fork.c` 与 `sched/exec.c`），否则宿主测试 `make test-host` 将失败。
3. **架构规范约束**：
   根据 `AGENTS.md` 目录规范第一条：**源目录 ↔ 头目录一一对称**，公开头统一放 `kernel/include/<subsys>/*.h`，禁止散落在源目录旁。roadmap 中的简记 `kernel/fork.c` / `kernel/exec.c` 若直接放在 `kernel/` 根目录，既违反构建系统 `KERNEL_C_SOURCES` 的自动发现规则（`kernel/Makefile` 仅 wildcard 子目录），又破坏目录对称性。因此拆分文件必须规范归入 `kernel/sched/` 子系统（或新建对称子系统）。

---

## 2. 方案设计与选型

### 2.1 目录归属选型

- **方案 A（推荐）：收敛于 `kernel/sched/`，进程生命周期与调度同属调度/任务子系统**
  - 文件布局：
    - `kernel/sched/core.c`：调度主循环、上下文切换收尾、唤醒、阻塞框架、初始化
    - `kernel/sched/fair.c`：EEVDF 调度算法实体
    - `kernel/sched/balance.c`：SMP 负载均衡与核选择
    - `kernel/sched/fork.c`：进程克隆、COW 页表复制、内核线程
    - `kernel/sched/exec.c`：ELF 加载启动、用户栈构造、auxv 填充、exec/spawn
    - `kernel/sched/exit.c`：进程退出、waitpid 阻塞等待与收割、fd 表安全引用
    - `kernel/sched/signal.c`：信号投递、进程组广播
    - `kernel/include/sched/internal.h`：子系统内部共享原型与全局变量 extern
    - `kernel/include/sched/task.h`：原有对外公开 API 保持不变
  - **优势**：
    1. 遵循 `sched/` 现有子系统边界，无需改动 `kernel/Makefile` 的发现逻辑（已有 `$(wildcard sched/*.c)`）；
    2. 全仓现有 37+ 处 `#include <sched/task.h>` 完全不受影响，零 ABI / 头文件路径扰动；
    3. 职责高度聚焦，每个单文件控制在 100~700 行之内，易于维护与审计。

- **方案 B：拆分子系统，新建 `kernel/proc/` + `kernel/include/proc/`**
  - 将 fork/exec/exit/signal 独立为 `proc` 子系统，`sched` 仅保留 core/fair/balance。
  - **缺点**：与现有 `kernel/fs/procfs.c` 命名接近，增加概念认知混淆；需要改动 `kernel/Makefile` 增加 `proc/*.c`；需要重新划分 `task.h` 与 `proc.h`，引发全仓大面积 include 路径重构，扩散改动面。

**决策：采纳方案 A**。保持 `<sched/task.h>` 为公开头文件，内部拆分为 `kernel/sched/*.c` 并通过私有头 `kernel/include/sched/internal.h` 互联。

### 2.2 FS 系统调用 (`sys_symlink` 等) 的去向

根据 `docs/roadmap.md` ARCH-4 描述及 ARCH-1 的依赖关系：
1. **若 ARCH-1 先落地**：ARCH-1 已在规划中将 4 个 symlink 相关系统调用统一移至 `kernel/syscall/sys_fs.c`。
2. **若 ARCH-4 先落地或独立实施**：将此 4 个系统调用搬迁至 `kernel/fs/sys_fs.c`（或 `kernel/fs/symlink_syscalls.c`），原样保留 `(..., pt_regs_t *regs)` 参数，供 `trap.c` 直接调用；待 ARCH-1 接入时平滑并入通用分发表。

**决策**：拆分时不应让非调度代码滞留在 `kernel/sched/`。将 `sys_symlink`、`sys_readlink`、`sys_lstat`、`sys_fstatat` 及其辅助宏 `COPY_USER_STR` 搬迁至 `kernel/fs/sys_fs.c`，在 `kernel/include/fs/vfs.h` 或专用头中声明原型，`trap.c` 保持直调。彻底斩断 `sched/` 对文件系统系统调用的耦合。

---

## 3. 详细模块拆分映射表

### 3.1 拆分后文件职责与符号清单

| 目标文件 | 估算行数 | 包含函数 / 符号 | 内部依赖 | 对外暴露 (API) |
|---|---|---|---|---|
| **`kernel/sched/core.c`** | ~600 行 | `init_mm`, `init_thread`, `init_task_union`, `init_task[]`, `task_list_lock`<br>`task_list_add`, `task_list_next`<br>`schedule`, `task_finish_switch`<br>`task_wake`, `blocker_wait`, `blocker_wake`, `sched_unblock_blocked`<br>`idle_task_resume`, `task_init`<br>`user_init_task`, `user_init_pid`, `pid_counter`, `alloc_pid()` | `fair.h`, `balance.h` | `schedule`, `task_wake`, `blocker_*`, `task_init`, `task_list_*` |
| **`kernel/sched/fair.c`** | ~140 行 | `EEVDF_MIN_SLICE`, `EEVDF_LATENCY`<br>`update_curr`<br>`cmp_deadline`<br>`enqueue_task`<br>`dequeue_task`<br>`pick_eevdf` | 无外部依赖，纯算法 | `enqueue_task`, `dequeue_task`, `pick_eevdf`, `update_curr` (内部) |
| **`kernel/sched/balance.c`** | ~160 行 | `sched_pick_cpu`<br>`sched_notify_remote`<br>`sched_balance` | 读 `user_init_pid`，调用 `enqueue_task`, `dequeue_task` | `sched_pick_cpu`, `sched_notify_remote`, `sched_balance` (内部) |
| **`kernel/sched/fork.c`** | ~530 行 | `fork_mm_copy`<br>`do_fork`<br>`kernel_thread`<br>`create_kthread` | 调用 `alloc_pid()`, `sched_pick_cpu()`, `enqueue_task()`, `sched_notify_remote()`, `fpu_area_alloc()` | `do_fork`, `kernel_thread`, `create_kthread` |
| **`kernel/sched/exec.c`** | ~720 行 | `fpu_area_alloc`<br>`startup_args_count`<br>`setup_user_stack_check_capacity`<br>`setup_user_stack`<br>`destroy_unpublished_user_mm`<br>`spawn_user_task`<br>`sys_exec`<br>`task_selftest_auxv_probe` | 调用 `alloc_pid()`, `sched_pick_cpu()`, `enqueue_task()`, `sched_notify_remote()`, 读写 `user_init_*` | `spawn_user_task`, `sys_exec`, `fpu_area_alloc`, `task_selftest_auxv_probe` |
| **`kernel/sched/exit.c`** | ~380 行 | `do_exit`<br>`waitpid_should_unblock`<br>`do_waitpid`<br>`task_files_pin_by_pid` | 读 `user_init_*`，调用 `schedule()`, `blocker_wake()`, `blocker_wait()` | `do_exit`, `do_waitpid`, `task_files_pin_by_pid` |
| **`kernel/sched/signal.c`** | ~80 行 | `task_send_signal`<br>`signal_pgrp` | 调用 `task_wake()`，操作 `task_list_lock` | `task_send_signal`, `signal_pgrp` |
| **`kernel/fs/sys_fs.c`** | ~210 行 | `COPY_USER_STR`<br>`sys_symlink`, `sys_readlink`<br>`sys_lstat`, `sys_fstatat` | 纯 VFS 与 uaccess 依赖 | `sys_symlink`, `sys_readlink`, `sys_lstat`, `sys_fstatat` |

### 3.2 子系统内部头文件：`kernel/include/sched/internal.h`
为了让 `kernel/sched/` 内部文件干净地共享函数与变量，而不把仅属于调度内部实现的细节暴露到全局 `<sched/task.h>`，定义 `kernel/include/sched/internal.h`：

```c
#ifndef KERNEL_SCHED_INTERNAL_H
#define KERNEL_SCHED_INTERNAL_H

#include <sched/task.h>
#include <percpu/percpu.h>

/* ── 全局/核心状态访问 ── */
extern task_t *user_init_task;
extern int64_t  user_init_pid;
pid_t alloc_pid(void);

/* ── EEVDF 算法接口 (fair.c) ── */
void update_curr(task_t *task);
void enqueue_task(task_t *task, percpu_t *rq);
void dequeue_task(task_t *task, percpu_t *rq);
task_t *pick_eevdf(percpu_t *rq);

/* ── 负载均衡与放置 (balance.c) ── */
void sched_balance(percpu_t *rq);
uint32_t sched_pick_cpu(void);
void sched_notify_remote(task_t *tsk);

/* ── 内部辅助 (exec.c / fork.c 共享) ── */
void *fpu_area_alloc(void);

#endif /* KERNEL_SCHED_INTERNAL_H */
```

---

## 4. 实施阶段与操作步骤

为保证每一步改动均可编译且可回归，实施分为 4 个阶段：

### 阶段 1：剥离文件系统系统调用 (FS Syscalls Migration)
1. 创建 `kernel/fs/sys_fs.c`，迁移 `COPY_USER_STR` 宏与 `sys_symlink`, `sys_readlink`, `sys_lstat`, `sys_fstatat` 四个实现；
2. 在 `kernel/include/fs/vfs.h` 中补全这 4 个函数的原型声明；
3. 从 `kernel/sched/task.c` 中删除上述代码；
4. 运行 `make clean && make test-static && make OS01_SYSTEST=1 test-syscall` 验证通过。

### 阶段 2：创建内部头与抽取调度纯逻辑 (Fair & Balance)
1. 创建 `kernel/include/sched/internal.h`；
2. 创建 `kernel/sched/fair.c`，迁入 EEVDF 常量与 `update_curr`, `cmp_deadline`, `enqueue_task`, `dequeue_task`, `pick_eevdf`；
3. 创建 `kernel/sched/balance.c`，迁入 `sched_pick_cpu`, `sched_notify_remote`, `sched_balance`；
4. `task.c` 引用 `internal.h`，删除对应代码段；
5. 运行 `make clean && make test-static && make test-kernel-selftest` 验证通过。

### 阶段 3：抽取进程生命周期与信号 (Fork, Exec, Exit, Signal)
1. 创建 `kernel/sched/signal.c`，迁入 `task_send_signal`, `signal_pgrp`；
2. 创建 `kernel/sched/exit.c`，迁入 `do_exit`, `waitpid_should_unblock`, `do_waitpid`, `task_files_pin_by_pid`；
3. 创建 `kernel/sched/fork.c`，迁入 `fork_mm_copy`, `do_fork`, `kernel_thread`, `create_kthread`；
4. 创建 `kernel/sched/exec.c`，迁入 `fpu_area_alloc`, `startup_args_*`, `setup_user_stack*`, `destroy_unpublished_user_mm`, `spawn_user_task`, `sys_exec`, `task_selftest_auxv_probe`；
5. 将 `task.c` 重命名为 `kernel/sched/core.c`，保留核心状态、调度器主入口 `schedule` 与 `task_init`。

### 阶段 4：测试与构建适配收尾 (Adapters & Verification)
1. **构建脚本适配**：
   修改 `mk/components/run.mk:566`，将 stack canary audit 的检查目标从 `$(KERNEL_BUILD_DIR)/sched/task.o` 调整为 `$(KERNEL_BUILD_DIR)/sched/core.o`；
2. **宿主测试探测适配**：
   修改 `hosttests/cases/test_fork_user_map.c`，将源码检查路径指向 `kernel/sched/fork.c`；
   修改 `hosttests/cases/test_process_image_lifecycle.c`，将源码检查路径指向 `kernel/sched/exec.c`；
3. **全量回归验证**：
   - `make clean`
   - `make test-static`
   - `make test-host`
   - `make OS01_SYSTEST=1 test-syscall`
   - `make KERNEL_SELFTEST=1 test-kernel-selftest`
4. **文档同步更新**：
   - 更新 `AGENTS.md` 中的 Key files 表格与架构说明；
   - 更新 `docs/sched/scheduler-reading-guide.md` 和 `docs/sched/scheduler.md`；
   - 更新 `docs/roadmap.md` 将 ARCH-4 标记为已完成。

---

## 5. 风险控制与验收标准

### 5.1 潜在风险与规避策略
1. **内联函数与符号可见性**：
   `enqueue_task` / `dequeue_task` / `update_curr` 从 static 变为跨 TU 调用，函数签名保持一致，严禁引入头文件全局变量（恪守 ARCH-3 准则，使用 `extern` 声明并在单个 `.c` 中定义）。
2. **锁顺序与竞态条件 (SMP Race Condition)**：
   本重构为**纯物理搬迁（Pure Refactoring）**，不修改任何锁范围（`task_list_lock`, `rq_lock`）、不修改内存屏障（`ACQUIRE` / `RELEASE`）、不修改执行逻辑。代码拆分前后逻辑严格等价。
3. **宿主测试静默失效**：
   通过第 4 阶段中对 `test_fork_user_map.c` 和 `test_process_image_lifecycle.c` 的显式更新与断言，确保宿主源码级审计继续有效。

### 5.2 验收标准
- [x] `kernel/sched/task.c` 不复存在，拆分后的各文件均小于 800 行；
- [x] `make test-static` 静态测试全部通过（包含 stack canary audit、runtime audit、header audit）；
- [x] `make test-host` 55 个测试套件全部 PASS（0 Failed）；
- [x] `make OS01_SYSTEST=1 test-qemu SUITE=systest` 334 个系统调用端到端测试 100% 通过（334 passed, 0 failed）；
- [x] `make KERNEL_SELFTEST=1 test-kernel-selftest` 31 项启动内建自测全部通过。
