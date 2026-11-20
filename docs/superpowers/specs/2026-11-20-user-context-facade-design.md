# 用户态进入与返回门面设计

- 日期：2026-11-20
- 基线：`7b5a399f`
- 路线图：[上下文切换统一（剩余项）](../../roadmap.md#架构统一推进链迈向单一-kernel_main)
- 现有契约：[架构与启动](../../arch.md)、[EL1 调度门面设计](2026-10-30-aarch64-el1-kthread-scheduler-design.md)

## 目标与范围

在 `<arch/context.h>` 提供用户任务首次调度准备、返回工作编排和原生帧恢复三个交接点。x86_64 与 aarch64 共享适用的返回工作顺序，架构差异由静态链接解析。原生异常帧的构造、准入和物理恢复由各架构负责。运行中的任务执行 exec 或 sigreturn 时，重写当前帧并沿原异常返回链恢复。

范围限于用户态上下文交接及共用这条链的既有 task-stack 返回。运行期跨核迁移、EL0 FP/SIMD、中断分派与 ACK/EOI、syscall ABI、进程生命周期策略以及 UEFI/启动期异常级切换均在范围之外。

## 现状

| 路径 | x86_64 | aarch64 |
|---|---|---|
| 首次调度 | `spawn_user_task` 和用户 `do_fork` 直接将帧地址和 `ret_from_intr` 写入线程上下文。切栈后，`__switch_to` 的返回消费继续点 | spawn 与 fork 共用 `user_task_prepare`。该函数将帧地址与 `aarch64_user_first_entry` 写入 SP/X30。首入继续点准备线程状态、交付信号，再经 `aarch64_el0_restore` 交接 |
| task-stack IRQ 返回 | `ret_from_intr` 按 softirq、重调度、信号、`iretq` 的顺序执行。首次进入、`int 0x80` 和 #PF 也进入此尾部 | `aarch64_el0_irq` 先完成 GIC 分派、EOI 和 VM IRQ depth 退出，再校验正式用户帧、检查重调度、重新校验并交付一次信号，最终 `eret` |
| 同步返回 | `do_system_call` 写回结果后在原位置先做一次 CPL3 信号交付，随后进入上述完整尾部 | 原生 SVC 已注册编号在退出 VM 授权并处理写回后交付一次信号。内部 yield 在写回后交付一次。未知 syscall、其他内部 SVC 和成功的缺页重试按现有语义返回 |
| 特殊返回 | IST、NMI、spurious 使用 restore-only | EL1h IRQ 与可恢复的 EL1h sync 共用物理恢复。旧 M4 probe 有独立首入，可用异常帧回到 EL1 继续点 |

## 方案与选择

1. **统一入口和恢复名字，保留两份返回编排。** 改动较小，也能统一构造器对保存 PC/SP 的准备。softirq、重调度和信号的阶段顺序仍由两边分别维护。
2. **共享返回工作编排，链接选择各阶段实现。** 共享 C 代码确定先执行可选的延迟工作，再处理重调度与帧准入，最后处理信号。首次进入和同步返回可沿用各自实际需要的阶段。原生适配器承担栈对齐、帧选择及特权级切换。采用此方案，它在复用顺序的同时保留每条路径的实际行为。
3. **把异常/中断返回连同帧准入和恢复统一为一个 C 入口。** 这需要重定义 IST/EL1 栈、旧 probe 和硬件返回的契约，还会把原本只恢复帧的路径引入任务态调度。现有边界适合保留这些路径。

方案 2 给 x86_64 普通返回增加一次 C 编排调用，给两侧增加固定的阶段调用。保留原无待办快速判断，使用静态直接调用。后续用真实 ELF 确认调用及栈形状，使用现有 IRQ 和产品冒烟验证行为。

## 首次调度契约

`kernel/include/arch/context.h` 前向声明 `struct pt_regs`，声明以下 C 函数。帧由调用方构造，位于目标任务的内核栈顶减去原生帧大小，满足本架构对齐要求，在首次运行前保持有效。

```c
void arch_task_prepare_user_entry(task_t *task, struct pt_regs *frame);
```

- 两架构各提供一个强定义。只写入尚未可运行任务保存的 SP 与首次继续点，无资源操作、错误返回、对 `current` 的依赖或帧内容修改。栈元数据、MMU/TLS、IRQ 状态与发布顺序由现有代码负责。
- x86_64 定义放在 `kernel/arch/x86_64/cpu/task_arch.c`，设置保存的 RSP 与指向 `ret_from_intr` 的 RIP。由 `spawn_user_task` 与用户 `do_fork` 在帧写完后调用。将 `ret_from_intr` 的 C 声明从公共 `sched/task.h` 移至后端使用处。保留 fork 中遗留的 `PF_KTHREAD` 保存 RSP 与 `regs->rip` 赋值。当前成功的 CPL3 fork 会清除此标志，CPL0 会提前被拒绝，这一遗留分支用源码/编译契约验证。
- aarch64 定义放在 `kernel/arch/aarch64/cpu/user_task.c`，设置保存的 SP 与指向文件内 `aarch64_user_first_entry` 的 X30，由 `user_task_prepare` 在帧填好后调用。首入执行 `arch_aarch64_entry_prepare()`，取得当前任务的帧，执行共享的单次信号阶段，然后调用 `aarch64_el0_restore`。该汇编适配器的 C 声明收至此文件，参数类型为 `struct pt_regs *`，返回属性为 `noreturn`，仍用 x0 传帧地址。
- 保留 spawn 的入口值、fork 子返回值零、现有 `arch_task_switch` 和 `task_switch_complete`。保留 x86_64 TSS/CR3 与 aarch64 TTBR0/ASID/TLS 在原切换序列中的位置。内核线程启动及 exec/sigreturn 不调用首次调度准备函数。

## 共享返回工作和链接契约

共享实现放在 `kernel/sched/context_return.c`，进入双架构和各变体构建。公共声明在 `<arch/context.h>`。汇编可读取数值为 `0` 的 `ARCH_CONTEXT_RETURN_FULL`，C 枚举用同一常量初始化。

```c
enum context_return_mode {
    CONTEXT_RETURN_FULL = ARCH_CONTEXT_RETURN_FULL,
    CONTEXT_RETURN_SIGNAL_ONLY = 1,
};

void context_run_return_work(struct pt_regs *frame, enum context_return_mode mode);
void arch_context_run_deferred_work(void);
bool arch_context_reschedule_return(struct pt_regs *frame);
void arch_context_deliver_return_signals(struct pt_regs *frame,
                                         enum context_return_mode mode);
```

| 符号 | 链接与行为 |
|---|---|
| `context_run_return_work` | 共享强定义。在 `FULL` 模式先调用延迟工作，再调用重调度阶段。重调度阶段返回 false 时直接回到适配器，否则执行信号阶段。`SIGNAL_ONLY` 直接执行信号阶段。帧始终由调用方拥有 |
| `arch_context_run_deferred_work` | 唯一弱默认为空操作，aarch64 使用它。x86_64 提供强定义，只有 `softirq_status` 非零才调用 `do_softirq()`。普通声明保持强引用，弱属性只写在默认定义，链接时强定义覆盖弱版本 |
| `arch_context_reschedule_return` | 每架构一个强定义，表示该次返回是否继续信号阶段。x86_64 读取现有的 GS 相对 `need_resched`，非零时清标志并调用 `schedule()`，然后返回 true。保留实际访问与清除时使用当前 CPU 的 GS 语义，支持 IF 已打开的 trap-gate 返回及可能的续跑迁移。aarch64 保留正式 IRQ 返回对帧、VM edit、scheduler ready 的前置约束，对 `need_resched` 的 acquire 读取、一次 `schedule()` 与前后自测观察，以及续跑后对原任务和帧/根的校验。拒绝时返回 false |
| `arch_context_deliver_return_signals` | 每架构一个强定义。x86_64 的 `FULL` 保留 pending 快速检查和 `arch_do_signal_delivery(frame)` 的原循环，以无 pending 或返回 0 为终止条件。其 `SIGNAL_ONLY` 在原 CPL3 条件下交付一次。aarch64 调用原 `aarch64_do_signal_delivery(frame)` 一次。两侧保留现行的用户级、挂起与屏蔽检查。x86_64 TTY 的 NULL 帧路径维持直接调用旧信号函数 |
| `arch_context_restore_frame` | 汇编门面目标，双架构各一个强文本符号，和现有原生恢复块同址。不存在弱默认。公共汇编名 `ARCH_CONTEXT_RESTORE_FRAME` 固定展开为该符号，本机只链接对应架构实现 |

任何缺少强实现的必需阶段都会报链接错误。共享代码没有编译期架构分支。重调度可挂起当前 C 链，恢复后帧仍由原任务拥有，适配器在信号改写后使用同一帧。

### 接入位置和返回顺序

- **x86_64 完整尾部。** 保留 `ret_from_intr` 的已有调用者、`OS01_SELFTEST` per-CPU 计数和 BOOT-2 回调。汇编以已入帧且可由 C ABI 保存的 RBX 保存帧地址，向 RDI 传帧、向 ESI 传 `ARCH_CONTEXT_RETURN_FULL`，向下对齐实时 RSP 至 16 字节后执行 SysV call。确保 C ABI 的 DF 为清零状态。调用后恢复 RSP 为帧地址，BOOT-2 仍在工作完成后对同一帧取证，然后跳入恢复目标。`FULL` 同样服务原 CPL0 task-stack 返回，信号阶段处理 CPL0 的既有清理语义。保留入口的实时 IF 语义及帧中的 RFLAGS，保留 syscall/#PF 的 `error_code` 选择与普通 IRQ/定时器/IPI 的返回边。IST/NMI/spurious 保留直达原 restore-only 并绕过共享返回工作。
- **aarch64 正式 lower-EL IRQ。** `aarch64_el0_irq` 仍在 GIC EOI、`aarch64_vm_irq_leave()` 和已有 probe 判定之后才调用 `FULL`。用返回布尔值的强重调度阶段替换原 `aarch64_user_irq_return`，其末尾信号调用移到共享阶段。C 返回后 `el0_irq_entry` 分支到恢复目标。probe 分支可就地把帧改为 EL1h，因此该汇编恢复目标也接受同级帧。
- **首次进入与同步交付。** x86_64 首入复用 `FULL`。其 `do_system_call` 尾部原一次交付替换为 `SIGNAL_ONLY`，随后仍经过完整尾部。aarch64 首入、原生 SVC 已注册编号（离开 VM 授权并处理写回后）与内部 yield（`schedule()`、写回 x0 后、测试 tick 重装前）在原处走 `SIGNAL_ONLY`。未知 syscall、其他内部 SVC 和可恢复用户缺页不新增阶段。aarch64 `el0_sync_entry` 按 `aarch64_el0_sync_route` 的 true 分支恢复，包括正式用户态返回以及继续运行的旧 probe。false 的 legacy EL1h unwind 和普通镜像的停机分支保持原状。

### 汇编恢复契约

汇编消费者从 `<arch/context.h>` 获取 `ARCH_CONTEXT_RESTORE_FRAME`，以分支交接，进入时 RSP/SP 已指向待恢复的原生帧。本机恢复块按帧恢复 GPR、段或系统寄存器并执行 `iretq` / `eret`，不在此调用 C 或判断返回级别。保存布局、跳过的槽及恢复指令顺序不变。aarch64 `aarch64_el0_restore` 仍以 x0 安装 SP 后分支到同一目标，`el0_irq_entry` 与 `el0_sync_entry` true 分支使用此目标。x86_64 保留 `RESTORE_ALL` 与 `x86_intr_restore_only` 同址的现有直接入口，aarch64 保留 `restore_pt_regs` 同址及 EL1h IRQ/sync 的现有恢复入口。旧 probe 的直接首入及启动汇编里的 `eret` 保持原路径。

两架构保持 `thread_t`、`task_t`、`pt_regs` 布局与 32 KiB task stack。共享返回工作和适配器不得额外切换 IRQ 屏蔽状态、改变 task/IST 的归属或推迟 ACK/EOI。

## 验收

1. 测试共享编排的阶段顺序、重调度阶段拒绝、仅信号路径、弱默认及强覆盖。检查 C 头可在两架构独立编译，无需完整 `struct pt_regs`。汇编路径使用相同恢复名与参数常量。验证 x86_64 三个以生产 `exec.c` 链接的宿主目标改用明确的测试替身或真实实现。
2. 两份普通 ELF 均证明共享编排、必需强定义、x86_64 强 softirq 覆盖/aarch64 弱默认、首次入口与物理恢复边。原中断审计对同址符号用地址证据，并把 aarch64 原 syscall→信号审计更新为经共享编排的完整链。保留 IST/NMI/spurious、EL1 可恢复 sync、GIC EOI 和恢复序列相关现有断言。变异测试应拒绝弱必需阶段、绕过共享编排、错路由及进入恢复前丢失帧。普通 ELF 审计的 legacy/test 符号排除约束继续适用。
3. x86_64 跑宿主测试、静态审计、`intr-x86`、`boot-irq-time`、syscall E2E 与独立内核自测。syscall E2E 依 [AGENTS.md](../../../AGENTS.md) 从顶层执行 `make OS01_SYSTEST=1 test-syscall`。内核自测另行执行 `make KERNEL_SELFTEST=1 test-kernel-selftest`。验证用户首次启动、fork/exec/waitpid、信号往返及特殊 IRQ 返回。
4. aarch64 用 `test-aarch64-audit` 审计普通 ELF，以 `KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1` 的独立镜像运行旧 probe 与 M4 正式用户态自测，再运行 `MODE=user-runtime` 的 `irq`、`asid`、`smp`、`product`。核对原 runner 的 EOI 后抢占、任务和帧归属、固定亲和 SMP、信号与产品 shell/respawn 证据。正式用户态与旧 probe 的覆盖分别来自产品/运行期变体与 M4 自测镜像。
5. 验收通过后更新 `docs/arch.md`、`docs/roadmap.md`、`docs/changelog.md`，记录覆盖的路径及实际验证证据。本设计文档的通过只批准进入实现计划阶段。
