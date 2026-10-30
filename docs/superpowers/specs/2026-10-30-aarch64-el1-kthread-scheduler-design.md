# aarch64 调度接入第一期：EL1 内核线程

- 日期：2026-10-30
- 状态：v2，子 agent 复审 APPROVE，人工评审通过（2026-10-30）；不代表实现已完成
- 代码基线：`c6cdf45d`
- 范围：设计文档；后续另写实施计划

## 1. 目标与动机

aarch64 已具备 UEFI 启动、PMM、Slab、运行期内核 VMM、SMP TLB shootdown、GIC 和 BSP Generic Timer。本期让 BSP 使用公共 EEVDF 调度器运行 EL1 内核线程，形成“创建 → 首次执行 → 主动让出 → 阻塞/唤醒 → 返回退出 → 切出后回收”的闭环，为后续 SMP 调度及 M4 用户态建立基础。

用户已选择上一轮建议的“先单核 EL1 线程，后 AP 调度，再 EL0/M4”方向。协作式调度、保持 AP 工作循环，以及以下接口拆分是本设计的技术选择。

本期完成意味着：普通 aarch64 镜像进入具有调度能力的 BSP idle；自测镜像在同一生产路径上验证内核线程。本期不表示 aarch64 已有用户态，也不把启动了多个 CPU 等同于多核任务调度。

## 2. 当前代码事实与前置缺口

| 代码位置 | 当前状态 | 本期影响 |
| --- | --- | --- |
| `kernel/Makefile` aarch64 源白名单 | 未编入 `sched/core.c`、`fair.c`、`balance.c`、`fork.c`、`exit.c` | 不能仅添加切栈汇编就声称已接入公共调度器 |
| `kernel/include/sched/task.h` | `thread_t` 是 x86 字段；`current` 用 `%rsp` 掩码；`switch_to` 含 x86 inline asm | 分离架构上下文、取 current 和切换操作 |
| `kernel/sched/core.c` | 调度、blocker、x86 初始线程、TTY/文件表、用户 init 和 late selftest 混在同一 TU | 拆开公共调度机制和现有 x86 系统启动 |
| `kernel/sched/fork.c` | `kernel_thread()` 构造 x86 `pt_regs_t` 并调用 `do_fork()`；含 x86 PTE 和 FPU 依赖 | aarch64 不能直接编译该文件 |
| `kernel/sched/exit.c` | 内核线程退出仍经过 VMA/files 清理；含 x86 PTE/uaccess 与 `hlt` | 分离内核线程终止与用户进程退出 |
| `kernel/arch/x86_64/cpu/switch.c` | 已在 next 栈上执行 `task_finish_switch(prev)` 和 `PF_SELF_REAP` 回收 | 保留时序，抽出公共尾处理供 aarch64 使用 |
| `kernel/arch/aarch64/boot/main.c` | 末尾永久 `wfi`；自测含 pre-SMP 和 post-SMP 两阶段 | 在既有 probe 后进行一次 BSP 栈交接，再启用调度 |
| `kernel/arch/aarch64/smp/smp.c` | AP 已接收 IRQ/SGI，末尾执行 `ap_work_run_one()`；没有 runnable task | 保持 online / ipi_ready 和工作槽契约 |
| `kernel/time/tick.c` | tick 设置 `need_resched`；accounting 仅 x86 编译 | 本期不加入 IRQ 返回抢占，也不扩展 monitor accounting |
| `kernel/sched/balance.c` | CPU picker 检查 `scheduler_ok`，但 balance 的候选扫描只检查 online；remote notify 使用 x86 IPI vector | 明确禁用 aarch64 跨核迁移与 remote notify，不能只靠 online |

基线还有两个容易误用的地址约束：公共调度器诊断使用固定的 x86 高地址窗口；aarch64 启动栈并非 task_union 栈。两者必须实质处理，禁止通过删掉诊断或假装 boot SP 可掩码取得 current 来绕过。

## 3. 范围与方案选择

考虑过三个方案：

1. 只写 aarch64 两栈切换演示。改动少，但不能覆盖公共 runqueue、blocker 和回收，因此不选。
2. 接入公共调度机制，首期仅 BSP 协作式 EL1 线程。需要拆开现有架构依赖，但具备独立、可复现的验收边界，采用此方案。
3. 同时完成 SMP 抢占调度、EL0/SVC 与 M4。跨越 timer/IPI、地址空间、异常恢复和用户生命周期，故障归因面过大，后续独立设计。

### 3.1 纳入

- 复用同一份 `schedule()`、EEVDF runqueue、`task_wake()`、blocker 与 `task_finish_switch()`，不复制一份 ARM 调度器。
- 架构上下文接口、aarch64 EL1 切换、BSP idle 栈交接、首次线程入口。
- 内核线程创建和终止的公共机制与架构边界；已存在的 x86 API 保持兼容。
- 单核主动让出、条件阻塞/显式唤醒、内核线程自回收；失败回滚和状态诊断。
- 正常 1/2/4/8 核启动中的 BSP 调度；其余 CPU 保持原有 AP 工作循环。
- x86 回归、aarch64 QEMU 正反例及测试框架接线。

### 3.2 不纳入

EL0/SVC、用户页表/ASID、fork/exec、用户 VMA/COW/uaccess、信号投递、每核 timer、AP 调度/负载均衡、resched IPI、IRQ 返回抢占、FPU/SIMD/SVE/SME 上下文、统一 `kernel_main`、统一 SMP 启动接口和 scheduler 策略调整。

内核线程必须合作：不主动让出或阻塞的计算循环会独占 BSP。这是本期公开限制；不能用 timer 正常打印证明抢占已实现。AP 的 SGI/TLBI 仍须可用。

## 4. 公共机制与架构文件边界

以下文件名为设计指定；私有 helper 名称可在实施计划中细化。新增公开头均位于 `kernel/include/`。

| 文件/目录 | 职责 |
| --- | --- |
| `kernel/include/arch/context.h` | 架构选择 facade：上下文类型、current accessor、切换操作、上下文初始化/验证契约 |
| `kernel/include/arch/{x86_64,aarch64}/context.h` | 每架构 `thread_t` 及 assembler 可用偏移；x86 保留原字段顺序和 inline asm 语义 |
| `kernel/arch/aarch64/cpu/switch.S` | 保存/恢复 EL1 执行上下文，切入 next 栈并调用公共尾处理 |
| `kernel/arch/aarch64/cpu/thread_entry.S` | 首次线程与 BSP 栈交接 trampoline；不复用异常返回 frame |
| `kernel/arch/aarch64/cpu/task_arch.c` | idle/kthread 构造、架构验证、启动栈交接准备 |
| `kernel/arch/x86_64/cpu/{switch.c,task_arch.c,thread_entry.S}` | 保持 TSS、FS、CR3、FPU、首次入口及 IRQ 返回行为 |
| `kernel/sched/core.c` | 公共调度、blocker、task list 与全局对象；去除 x86 asm/地址常量及系统服务启动 |
| `kernel/sched/boot.c` + `kernel/include/sched/boot.h` | 公共 runqueue/idle 发布、调度启动接口；现有 x86 `task_init()` 系统启动段独立保留 |
| `kernel/sched/kthread.c` + `kernel/include/sched/internal.h` | 架构中立的 kthread 创建、退出状态转换、切换尾处理与资源回收 |
| `kernel/sched/{fork.c,exit.c}` | x86 用户生命周期保留；仅抽出 kthread 共用段，不迁移用户 MM |
| `kernel/selftest/test_aarch64_scheduler.c` | BSP 调度 late selftest；使用生产创建/切换/退出代码 |
| `qemutests/aarch64_scheduler.py` | 新 `MODE=sched` 的运行与严格证据校验 |

不能通过为未移植的 VFS/VMA/fork 提供“返回成功”的空实现来使链接通过。公共 core 的依赖必须拆开；确实仍需调用的诊断提供有效 aarch64 后端。

### 4.1 构建与头文件

aarch64 白名单纳入公共 core、boot、kthread、fair、identity 和满足本期边界的 placement/balance 代码，不纳入 fork/exec/用户 exit、VFS、网络。x86 编译原功能集合加新拆出的公共文件，移除重复定义。

当前 aarch64 kernel/libk 已能使用 list/rbtree/Slab，但没有完整用户 libc sysroot。只增加必要的内核构建头搜索路径和 freestanding 类型依赖，不引入用户 syscall wrapper。公共头不能再把 x86 汇编或 x86 PTE 带入 aarch64 TU；`task.h` 已有 file 类型依赖可先保留声明，链接依赖必须切断。

结构变更必须同步 assembler 偏移、`_Static_assert`，并依仓库规则执行 `make clean` 后重建。优先不改变 `percpu_t`，特别是不移动 `ipi_ready/tlb_ack_gen` 尾字段。

## 5. 上下文与切换契约

### 5.1 接口

`arch_task_switch(prev, next)` 是架构操作，调用者已屏蔽 IRQ。它可在 x86 继续展开为原 inline asm，在 aarch64 使用独立汇编函数；不要求两个后端有相同的机器级调用方式。

- 切出时保存的是 prev 的执行续点；该调用只有在 prev 再次被选中时才返回。
- 切入 next 后，必须在 next 栈上完成架构后处理与 `task_switch_complete(actual_prev)`，然后才能进入 next 的 C 续点或首次入口。
- `actual_prev` 必须来自本次实际切换传递的参数，不能取 next 早先挂起的 `schedule()` 局部变量；该旧变量可能已释放。
- 对 `prev == next`，公共调度器优先跳过真实切换；若架构操作仍被调用，禁止清除当前任务的 on_cpu 或回收自己。
- x86 的 TSS/FS/CR3/FPU 操作及其先后顺序保持，GS base 不随任务切换。
- aarch64 本期不写 TTBR0/TTBR1、VBAR、TPIDR_EL1/SP_EL0；所有线程共享当前内核地址空间，TPIDR_EL1 始终指向本 CPU 的 runtime percpu。

`task.h` 的旧 `switch_to` 调用可保留为兼容别名，但实现与 `GET_CURRENT` 等 x86 asm 必须进入架构私有头。公共调度代码不再直接读取 `thread->rip/rsp/cr3`，改用初始化、验证或诊断接口；`sched_flags` 保持公共可用的每线程 IRQ 存储。

### 5.2 aarch64 保存集合与 ABI

EL1 协作式切换按普通 AAPCS64 调用边界保存 x19–x29、x30（返回续点）与 SP。x0–x17 和 NZCV 为调用者易失状态；本期 x18 无平台 TLS/current 用途，按临时寄存器处理。不得用 x18 代替 TPIDR_EL1。

`thread_t` 的 aarch64 形态包含上述保存区、栈顶/范围及 `sched_flags`；汇编使用共享偏移常量并由 C 对每个偏移和大小断言。SP 和所有汇编调用 C 的边界保持 16 字节对齐。记 task_union 基址为 base、上界为 top=base+STACK_SIZE、可用栈下界为 usable_low（覆盖 task_t 和诊断保留区之后的对齐地址）；保存或执行中的 SP 必须满足 `usable_low <= SP < top`，不得等于 top。

首次线程的 x19/x20 初始化为 fn/arg，x30 初始化为首次入口；初始 SP 使用 top-16 的对齐入口保留帧，其整个生命周期不弹到 top。入口通过 x0 传参调用 `fn(arg)`，将返回值传给内核线程终止路径。没有 fake `pt_regs_t`，没有 `eret`，线程入口不得返回到伪造的调用者。不能依赖 C 函数序言先调整 SP 才使 current 正确。

汇编调用公共尾处理会覆盖 LR，必须先在 next 栈保留其恢复续点；尾处理返回后恢复该续点再跳转/返回。使用的 next 栈临时空间属于其有效可用区，且不覆盖 task_t 或保存现场。尾处理释放 prev 后，汇编与 C 均不得再读取 prev。

IRQ 的完整 x0–x30/ELR/SPSR 保存继续归现有异常入口。同步切换保存集合不能被误用为异步抢占现场。所有可运行内核代码和可达 libk 继续使用 `-mgeneral-regs-only`；新增汇编也不能使用 FP/SIMD。无 FP 状态保存就不能接纳需要这些状态的线程。

依据：[Arm AAPCS64 2025Q1，General-purpose Registers / Stack constraints](https://github.com/ARM-software/abi-aa/blob/2025Q1/aapcs64/aapcs64.rst)。这里只采用基础寄存器和栈约束，不实现完整用户进程 ABI。

### 5.3 IRQ 状态与重入

沿用当前公共 `schedule()` 的行为，不擅自把它改为通用 irqrestore 函数：

| 路径 | 返回时 IRQ 状态 |
| --- | --- |
| scheduler 未启用或 in_schedule 早退 | 保持进入状态 |
| 已保存 IRQ 的 no-switch 早退 | 恢复该任务进入时的 `sched_flags` |
| 完成真实切换、任务再次恢复 | IRQ 保持屏蔽；`in_schedule` 已清零 |
| 首次线程入口 | 尾处理完成、on_cpu=1、in_schedule=0 后才开放 IRQ |

新增公共内核 `void kthread_yield(void)` 包装明确保存调用者 IRQ 状态、调用 schedule、在返回后恢复该状态；测试线程和后续正常协作线程使用它，不能散布无条件 enable。名称与已有 libc 的 `int sched_yield(void)` 区分，不修改用户 ABI，也不链接到其返回 0 的空实现。底层 schedule 的既有 x86 调用者语义不变。

aarch64 IRQ 返回本期只恢复异常现场，不调用 schedule；timer 只推进现有时钟/softirq 标记和 `need_resched`。不得在 GIC handler、持锁区或 SGI/TLBI 回调中调用 yield/block/exit。blocker 本期支持的线程调用前提为 IRQ 开启、无持有自旋锁，`signal_can_wake=false`；其现有 x86 信号语义不变。

## 6. BSP idle、current 与启动顺序

采用与现有 task_union 相容的栈掩码 current：aarch64 在有效调度栈上用 `mov ..., sp` 后 `& ~(STACK_SIZE-1)`；x86 保留现有实现。`STACK_SIZE=32 KiB`，task_t 位于区域底部，栈向低地址增长。

调度前的 boot 栈及 AP boot/work 栈没有 task_t，不得调用 current；不能通过掩码后“恰好像地址”判断有效。所有新增 timer/日志/验证调用必须审计这一限制。

BSP 在已有的内存/SMP/GIC/timer/probe 流程完成后执行一次不可返回的 handoff：

1. 保留现有 pre-SMP 和 post-SMP 自测及 shootdown probe 的顺序、标记和执行次数。
2. 屏蔽 BSP IRQ。准备 `init_task_union`、`init_thread`、空 runqueue、task list；不得再次调用 percpu_init 或重置已发布的 IPI 状态。
3. `init_task_union` 在链接脚本中按 STACK_SIZE 对齐、整段保留。boot SP 上只准备元数据，不发布 `scheduler_ok`，不调用依赖 current 的 helper。
4. 汇编把 SP 置为 BSP idle 的 top-16，先保留 16 字节对齐入口帧，再清理 FP 链并跳到不可返回的调度启动续点，禁止返回旧 boot 栈；在任何 current accessor 或 C 调用之前，SP 已严格落在本 task_union 内。此时 current 必须等于 `&init_task_union.task`，不能先设为 top 再依赖 C 压栈。
5. 在 idle 栈上设置 `cpu=0`、idle 身份、RUNNING/on_cpu=1/on_rq=0、允许 CPU0，最后发布 `percpu_data[0].scheduler_ok=1`。不创建 TTY/文件表，不加载用户 init。
6. 开放 IRQ；自测构建运行本期 late selftest，普通构建进入 BSP 调度 idle。boot 栈仍由启动保留内存持有，本期不回收它。

idle 不入 runnable rbtree，退出被拒绝；第一次从 idle 切出后可恢复到原调用续点，不再使用公共代码修改 x86 `rip` 的兜底。每个架构的初始化保证 idle 上下文可用。

idle 循环必须先检查 runnable/need_resched 再休眠，并覆盖“检查后、WFI 前唤醒”的窗口。本期 BSP 周期 CNTP 保持开启，至多下一个 tick 退出 WFI 后再次检查；测试要求有限时间恢复，不依赖 AP 发送 resched IPI。所有正常 idle、yield、block 返回均恢复约定的 IRQ 状态。

## 7. CPU 可调度集合与 AP 隔离

保留 `num_cpus` 为真实启动/枚举数量，不把它篡改成 1；AP 的 `online=1` 和 `ipi_ready=1` 继续服务 TLB shootdown。

aarch64 本期仅 CPU0 `scheduler_ok=1`；新 kthread 均为 `cpu=0, allowed_cpus=1`。AP 的 scheduler_ok 永不置位、不注册 idle task、不调用 schedule/current。`kernel_thread_on_cpu()` 指向非 0 CPU 时返回 `-EINVAL`，即使 AP 已 online；失败不影响下一次创建。

公共运行期 placement/balance 需显式依赖“online 且 scheduler_ok”，并尊重 allowed_cpus；唯一 pre-scheduler 创建例外见 §8.1。aarch64 的 remote-notify 分支在本期不得调用 x86 IPI 编码：本地路径直接返回，意外远程目标作为不变量错误报告并拒绝发布，不能 silent success。可用架构能力门控消除不可达的 remote IPI 链接依赖；不能提供虚假的成功 IPI 后端。

AP 保持 `ap_work_run_one()` + IRQ 的现有路径；不改 CNTP 为每核启动，不占用已有 SGI 分配，不改 boot_go/online/ipi_ready 的发布协议。

## 8. 内核线程创建、阻塞与退出

### 8.1 创建与发布

对外保留 `kernel_thread(fn,arg,flags)`、`kernel_thread_on_cpu(fn,arg,cpu)`、`create_kthread(fn,arg,name)` 签名。抽取公共 kthread 构造器，与 x86 用户 `do_fork` 分开；两个架构的 kthread API 最终使用该构造器，用户 fork 保留现状。

构造按“资源齐备后发布”实施：

1. 校验 fn、目标 CPU/affinity 和调用环境；除下述 x86 boot 例外外，无有效目标时返回负 errno，不能索引 NR_CPUS 哨兵。
2. 分配 raw task_union 空间（留足对齐余量）和 thread，按 STACK_SIZE 对齐；清零所有新节点/状态，保存真实 `stack_alloc_base`。检查 task_t 与栈保留余量，不能让栈覆盖结构体。
3. 设置新 PID/identity/comm、父子及会话元数据、空 blocker/wait/io 节点、vruntime/deadline 和 CPU mask。新任务显式为 PF_KTHREAD，不继承 PF_SELF_REAP/on_cpu/on_rq/in_schedule。
4. aarch64 使用共享 `init_mm`，`files=NULL`、`fpu_save=NULL`，不创建/销毁页表；x86 保留现有内核线程的 mm/CR3、files 复制和 FPU 初始状态行为，由可失败的资源准备 helper 完成。不能把用户 fork 的 COW 路径带入公共构造器。
5. 架构初始化首次上下文，所有可失败步骤完成后，按 task_list_lock → rq_lock 顺序发布全局表及 RUNNING 入队。publication 前 IRQ/并发观察不到半初始化任务；create_kthread 的 name 在发布前写好。
6. 返回 PID（kernel_thread）或非持有引用的 task 指针（create_kthread）；后者仅在调用者另有生命周期同步时可持久使用。测试用独立 completion 存储，不在自回收后读 task 指针。

x86 boot 例外规则：仅普通（未指定 pinned CPU）的 kernel_thread/create_kthread，在 BSP percpu、当前 task 栈与 CPU0 runqueue 已初始化、BSP 尚未首次发布 scheduler_ok 的启动阶段，允许目标回退 CPU0 并排队，稍后由 task_init 开启调度。保留既有默认 allowed_cpus 语义，不提前置 scheduler_ok，也不重置既有 runqueue。该资格在首次 BSP scheduler_ok 发布时永久关闭，不能把运行期“无目标”误认成启动期。必须覆盖 `net_lwip_init()` 的 UP 启动调用；AP 已可调度时仍优先沿用正常 placement。aarch64 和所有 kernel_thread_on_cpu 调用均无此例外，仍严格验证 online/scheduler_ok。

`flags` 的现有调用形态须在实施计划中列清，并保留有效 PF_KTHREAD 组合，禁止带入 SELF_REAP/用户进程语义。

所有分配失败路径须逆序释放未发布资源，无 runnable/list 残留。PID/identity 允许留空洞，不承诺回滚计数。不得吞掉 files/FPU 等资源初始化失败后发布半成品。

### 8.2 阻塞与唤醒

复用 `blocker_wait`、`blocker_wake`、`task_wake` 和 scan fallback。线程检查条件、安装 blocker、二次检查、切走；唤醒必须经 rq_lock 和 on_cpu/on_rq 协议，不能直接操作 rbtree。

测试同时覆盖条件已满足不睡、实际切出后显式唤醒、条件检查与切走之间的唤醒、仅 scan fallback 恢复。不得用“线程跑到了第二行”替代验证确实发生阻塞。阻塞回调本期不涉及用户地址、信号和文件系统，且不可睡眠或获取会反向嵌套 task_list_lock 的锁。

### 8.3 退出与自回收

抽出 `kthread_exit(code)`，只处理内核线程资源；首次入口把 fn 返回值交给它。x86 `do_exit()` 的 kthread 分支委托该路径，用户退出仍留原文件。

- 不对共享 `init_mm` 执行 vma_free_all，不销毁共享页表。
- x86 复制的 files 引用先按现有 pin 协议解除；aarch64 无该资源。全部可能读 current 的退出清理都在旧栈有效时完成。
- 在 task_list_lock/IRQ 保护下从全局链表删除，设置 PF_SELF_REAP 和 ZOMBIE；on_cpu 仍为 1。禁止 idle/非 kthread 走此入口。
- 调用 schedule 永久切出，返回即为 fatal；halt 必须走 arch helper，不能留下 `hlt`。
- next 栈上的 `task_switch_complete(actual_prev)` 在调用 `task_finish_switch` 前捕获 self_reap 标志及待释放对象指针；`task_finish_switch` release 清 on_cpu 后不再读取普通 prev。
- 自回收任务无 waitpid 消费者；按既有 thread/FPU/raw stack 所有权释放一次，raw stack 最后释放。不能在 prev 栈上 kfree 自己，不能二次入队或稍后读 prev。

公共尾处理同时用于 x86 首次切入和恢复路径；架构后处理在清 on_cpu 之前完成所有 prev 访问。测试记录完成和回收是两个事件，fn 返回/写 completion 并不证明栈已释放。

## 9. 诊断与现有行为保护

- 上下文验证在解引用前检查空指针、保存 SP 是否满足 §5.2 的半开可用区间、对齐和续点是否为本架构合法内核执行地址。aarch64 普通续点属于实际高半区 `.text`（`0xffff0000...`）；静态 idle/thread 位于已映射的高半区镜像，动态栈/thread 位于 RAM 直映范围。仅 `.boot` 位于低地址，必要的低地址启动 trampoline 只能按显式符号范围放行，普通线程续点不接受任意低地址。不能把所有直映 RAM 当可执行，也不能沿用 x86 固定 `0xffff800020000000` 上界。
- x86 原有 stack/UAF 诊断不得退化为 always true；aarch64 新后端须有真实验证。hang/watchdog 报告依赖中的架构字段也须拆分或给出实际后端，禁止空桩掩盖错误。
- 诊断使用 log_err/log_warn/log_info/log_debug；仅测试协议标记使用许可的原始输出。aarch64 当前日志格式化能力有限，新增诊断必须实际输出需要的数值，不能把 variadic fmt 当已格式化。
- IRQ frame、boot_context、UAPI、syscall 号不变。既有 x86 signal、用户 init、fd 继承、热管理 worker 与 late selftest 顺序保持。
- 与本期无关的 selftest 失败要单独记录基线；不能因新用例通过就称全部测试通过。

## 10. 验收与证据

下列为实施验收要求，不是当前已运行结果。测试须覆盖生产代码；源码扫描和 host 模拟不能替代真实切栈。

### 10.1 静态/host

1. 两架构公共调度 TU 编译；aarch64 无 x86 PTE/asm/用户 MM 链接依赖，无成功空桩；x86 thread 布局与旧偏移不漂移。
2. C/汇编全部上下文偏移、task_union 对齐、task_t/栈余量的编译期断言；链接地址检查覆盖 BSP idle 区。
3. 分配 fault injection 覆盖 raw stack、thread、arch resource prepare 的每个失败点；验证回滚无 list/rq/分配残留，随后正常创建成功。
4. placement 覆盖 online 但 scheduler_ok=0 的 AP、空 mask、非法 CPU、失败 pin 后普通创建；锁顺序与 on_cpu/on_rq 唤醒交接覆盖。x86 UP focused 用例须在无 scheduler_ok 目标的合法 boot 阶段创建成功、保持 CPU0 尚未 scheduler_ok、稍后启用调度并实际执行；运行期无目标与 pinned API 相同场景均须拒绝。
5. 新 runner parser 覆盖缺失/重复/乱序/部分 PASS、FAIL/SKIP、崩溃和超时；通过现有 report schema，失败退出码非零。

### 10.2 aarch64 QEMU 必须项

新增 `make PROFILE=aarch64-clang test-aarch64 MODE=sched`；这是拟新增入口，目前不存在。沿用现有 `test-aarch64` bucket，不增加别名。

该模式构建 KERNEL_SELFTEST 的独立 `sched` kernel/image 变体；模式选择器与 sync-fault/M1/fault/systest 等冲突在解析期拒绝。接线须覆盖 profile variant、受控 submake 变量、构建源清单、help、tests 和 docs/build/build.md。普通镜像不包含故障注入符号。

| 用例 | 必须观察的行为 |
| --- | --- |
| 两线程交替 | 每线程至少 10,000 次 yield；两个计数均达标，存在真实跨任务切换；不强求 EEVDF 每次严格 AB 交替 |
| 寄存器/栈 | 独立汇编 probe 在调用 yield 前后检查 x19–x29、SP 与返回续点；多个栈局部哨兵保持；不同线程使用不同模式 |
| 首次入口 | fn/arg 精确一次、ABI 栈对齐、IRQ 开启、current 正确、in_schedule=0/on_cpu=1；BSP handoff 和首次线程在任何 C 序言前以汇编检查 SP 已在半开可用区间 |
| IRQ 状态 | IRQ 开/关两种 yield 入口都恢复原状态；真实 switch 与 no-switch 均覆盖；首次入口不继承父线程的屏蔽状态 |
| blocker | 四种 §8.2 时序全部验证，结束 blocker 已清理、无重复入队；超时失败 |
| 回收 | 至少 1,000 个线程分批创建/退出；空闲线程表/rq 计数回到基线，专用分配账本的 stack/thread/FPU 对象净额为零；观察回收发生在 next 栈 |
| idle 恢复 | 最后线程退出后返回 BSP idle；idle 不在树内；线程全阻塞后由 BSP timer/条件检查唤醒，有限 tick 内恢复 |
| timer 与 IRQ | 切换压力期间 jiffies 持续增长，IRQ frame probe 保持；没有嵌套 schedule；timer IRQ 不执行任务切换 |
| AP 隔离 | 在 2/4/8 核启动下所有任务只在 CPU0；AP scheduler_ok=0、runqueue 空，原 AP 工作槽和 TLB probe 仍成功 |
| 错误上下文 | 独立注入变体给保存 SP/对齐或入口续点置坏值；包含 SP==top、SP<usable_low、任意低地址或非执行 RAM 续点，在切栈前 fatal，不能继续输出 PASS |

所有正常用例在 `-smp 1/2/4/8` 至少各运行一次；1 核 shootdown 的既有 SKIP 合法，但任何 scheduler 子用例 SKIP 均失败。栈/寄存器、blocker 和回收 stress 在 1/2 核各重复三次。每次 QEMU 有有限超时，持续 tick 不延长超时。

late selftest 接入现有 protocol-v1 coordinator：本期 case 在 END 前注册/记录，END 恰好一次且覆盖本期 case；不得在 boot main 已输出 END 后另加自称成功的第二套协议。如需调整既有 aarch64 END 位置，要保留所有 pre/post-SMP case 记录，更新旧 parser 回归。

MODE=sched 的报告还必须断言全体预期 scheduler case 恰好一次 PASS、无缺项/重复、无 scheduler FAIL/SKIP、无 fatal。保留 per-case QEMU 日志、CPU 数、构建变体、测试列表和失败理由。

### 10.3 回归门禁

- `make PROFILE=x86_64-clang test-host`、`test-static`。
- `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 与 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`，分别覆盖 SMP 1/2（按现有 suite 的 SMP 参数接线），systest 不设 KERNEL_SELFTEST。
- x86 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`，覆盖 1/2/8 核，重点 mutex、kthread_self_reap、fd_refcount、thermal_affinity 和 blocker；记录整套结果。
- aarch64 现有 `MODE=smp`、`m3-probe`、`sync-fault`、`gic-spi` 及 M1 内存矩阵/负例；新增普通镜像 1/2/4/8 核 BSP idle ready 冒烟。
- `make test-harness` 和 `make PROFILE=aarch64-clang test-aarch64-audit`，包含新 mode/协议/parser/变体隔离检查。

## 11. 实施分段与完成定义

实施计划按下列依赖顺序展开，每段有独立验证；此处不是逐文件编码任务清单。

1. 提取 arch context/current 与公共切换尾处理；x86 行为回归，保留原有汇编切换结构。
2. 拆分调度启动、内核线程构造/终止与用户生命周期；x86 仍通过上述关键回归。
3. 实现 aarch64 EL1 上下文、BSP idle handoff 和 CPU0 发布；普通镜像启动并保持 AP probe。
4. 接入合作让出、blocker、回收和 strict QEMU 验收；再跑两架构完整门禁。

交付必须同时包含代码、测试接线、验证报告和文档更新。报告记录真实命令/退出码/CPU 数/日志路径，未跑项明确标注。roadmap 仅将“EL1 BSP 协作式线程调度”标为完成；M4、SMP 调度、IRQ 返回抢占和统一 kernel_main 保持待办。

## 12. 后续顺序

下一期设计 AP idle task、可调度 CPU 发布、每核 clockevent 与 resched IPI，再接 IRQ 返回安全点和 SMP 抢占。之后设计 EL0/SVC、用户地址空间与 M4。当前的共享内核 TTBR、EL1 保存集合和 BSP-only mask 不能直接延伸成用户/SMP 实现。

## 13. 评审记录

本节只记录对本文件的评审；approve 仅表示设计具备编写实施计划的条件。

- 第一轮：`spec_review_1`，结论 REQUEST_CHANGES。四项意见均对照源码核实后采纳：改用 kthread_yield；入口 SP 使用 top-16 并统一半开栈区间；给 x86 boot 创建规定唯一例外及测试；纠正高半区镜像/低地址 boot/直映 RAM 的地址验证。
- 第二轮：独立子 agent `spec_review_2` 重新读取 v2 并对照源码，结论 **APPROVE**。确认第一轮四项均解决，未发现阻碍编写实施计划的重要未决问题。评审覆盖创建/调度/退出/切换、boot placement 和自测协议；未声称实现或测试已完成。
- 人工评审：用户于 2026-10-30 明确批准设计，并授权 git commit。
- 复审后仅更新本文状态和评审记录，技术正文维持获评审的 v2。
