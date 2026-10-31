# aarch64 AP 协作式调度与跨核唤醒设计

日期：2026-10-31。基线：`e8412d1e`。状态：独立评审 **APPROVE**（跨日修订细化亦经P1评审确认，见评审记录）。

本轮仅交付设计、两个实施 plan 和评审记录，不实施产品代码。后续由 Luna 每次执行一个完整 plan；P1 验收后才能执行 P2，执行前核对实际合入树与本基线的差异。

## 1. 意图和交付边界

在现有 EL1 BSP 协作式 EEVDF 调度之上，让 AP 真正执行内核线程，并最终在没有 AP 周期 timer 的情况下可靠休眠、接受远端唤醒。

| 交付 | 行为 | 明确保留的限制 |
|---|---|---|
| P1 | 每核独立 idle/task 栈；显式 pinned kthread 在目标 CPU 创建、yield、退出并回收；idle 轮询工作和运行队列 | 所有 aarch64 调度 CPU 的 idle 忙等；无 resched SGI；无抢占 |
| P2 | 远端入队/显式唤醒和 `ap_work` 提交发 resched SGI；idle 可执行 `wfi`，无 check-to-sleep 漏唤醒 | 线程仍须 yield/阻塞/返回；无 AP timer、迁移或自动负载均衡 |

不包含 EL0/SVC、用户 MM/ASID/COW、fork/exec、信号、FP/SIMD/SVE/SME、每核 clockevent、IRQ 返回抢占、统一 kernel_main 或 SMP 启动重构。线程及可达 libk 继续 `-mgeneral-regs-only`。不改变 UAPI、boot_context、thread_t 或 percpu_t 布局。

## 2. 现状与方案选择

依据当前源码而非旧 roadmap：

- `cpu/task_arch.c` 的 `arch_kthread_prepare()` 拒绝非 CPU0 并把亲和性设为 1；公共构造器本来支持 pinned API。
- `smp/smp.c::secondary_idle()` 在 `ipi_ready` 后循环 `ap_work_run_one()` + pause，尚未进入任务栈；`current` 此时不可用。
- `cpu/idle.c` 目前只供 BSP 使用，先开 IRQ 再 WFI 的竞争靠 BSP tick 兜底，不能复制给无 timer AP。
- 公共 `sched/balance.c` 用 `ARCH_HAS_REMOTE_RESCHED` 分支硬编码 x86 IPI；`task_wake()` 仅写远端 need_resched，没有发 IPI。
- `ap_work` 支撑 M3/shootdown 探针；其 READY 发布目前不发送唤醒。
- 当前 scheduler 自测断言 AP 不调度，且首次入口、资源回收、switch 观测使用全局变量；多核后不能原样并发写入。

考虑了三种方式：直接做 SMP 抢占和用户态，故障面太大；把所有内容放一个长 plan，Luna 需同时处理栈、SGI、休眠与证据协议；选用本设计的两个独立 plan。P1 独立交付可工作的轮询调度，P2 只扩展通知与休眠。总设计冻结跨期接口和最终行为，P2 执行前必须依据 P1 实际实现重新核对文件、接口及证据。

## 3. 启动和状态发布

### 3.1 AP idle 存储与生命周期

新增 `kernel/arch/aarch64/cpu/ap_sched.c` 与对应公开头 `kernel/include/arch/aarch64/ap_sched.h`。使用静态、STACK_SIZE 对齐的 AP task_union 数组及独立 thread_t 数组，最多 NR_CPUS-1 个，内核永久拥有，不消耗 PID、不进入自回收路径。每个 idle 的 pid=0，cpu 与单 bit allowed_cpus 固定，PF_KTHREAD，mm=&init_mm，files/fpu_save=NULL。list/rbtree 节点独立初始化，不复制 BSP 的链接指针；加入公共 task list 必须持 task_list_lock，且只加入一次。不重置全局 task list 或任何活跃 runqueue。

idle 元数据在 BSP 的 `arch_aarch64_prepare_bsp_handoff()` 调用前后同一启动阶段准备，必须在发布下述 release gate 前完成；不需要在 CPU_ON 前触碰 AP 的运行时 runqueue。静态 BSS 必须被 PMM 内核保留范围覆盖，验收 ELF 对齐和内存预留。

### 3.2 两个不同的就绪概念

`online` / `ipi_ready` 继续代表现有 SMP/TLB 通道状态；`scheduler_ok` 才允许发布 runnable task。失败或被拒绝的 AP 即使迟到，也不得被调度器录用。

新增接口（均 arch-private）：

```c
void aarch64_ap_sched_prepare(void); /* BSP boot stack, once; no current */
void aarch64_ap_sched_release(void); /* BSP task stack after BSP ready */
bool aarch64_ap_sched_released(uint32_t cpu);
void aarch64_ap_sched_enter(uint32_t cpu) __attribute__((noreturn));
void aarch64_ap_idle_handoff(uint32_t cpu) __attribute__((noreturn));
void aarch64_ap_stack_enter(uint64_t sp, thread_t *ctx, uint32_t cpu)
    __attribute__((noreturn));
uint64_t aarch64_smp_accepted_mask(void); /* smp.h; frozen boot ACK mask */
```

gate 使用独立 BSS release/acquire；CPU admission mask 来自冻结的 `smp_boot_result.online_mask`，不能用 `(1<<active)-1`，也不能仅看 runtime online。只对通过 boot ACK 且完成 ipi_ready 的 CPU 开放。BSP 在已有启动探针、M3工作和 BSP task-stack handoff 之后发布自身 ready，再 release AP gate。AP release 前保留当前 ap_work 轮询；release 后完成单向栈切换。

AP trampoline IRQ masked，验证 ctx/SP 的高半区、32 KiB 栈归属、16-byte 对齐、usable_low 与 top 边界，保留 cpu 参数，安装 SP 后才进入 C handoff；不得复用硬编码 init_thread 的 BSP trampoline。TPIDR_EL1 保持本 CPU runtime percpu；不重写 TTBR、VBAR、SP_EL0，不重新 percpu_init。

handoff 在真实 task 栈上确认 current/CPU/ctx，安装 rq->idle，idle RUNNING/on_cpu=1/on_rq=0/in_schedule=0；确认该 AP runqueue 为空而不清空它；初始化完成后 enable IRQ+ISB，最后 release-store scheduler_ok=1。读者 acquire-load ready，读取其他字段及发表工作。BSP 原 ready 发布也升级为 release，公共 admission/placement/balance 对该状态读取使用 acquire；x86 不得改变启动窗口语义。

BSP release 后等待 admission mask 中全部 AP ready，使用 arch_cycle_counter/cntfrq 的 2 秒绝对期限，等待时 IRQ 开、不能持 rq/task-list 锁；缺失则 log_err 并 fail-stop，不回收可能已被 AP 使用的静态栈。no-ack 等旧故障模式保留未录用 AP 的旧工作循环，不等待其 scheduler ready。普通镜像也执行此生产路径。

## 4. 调度策略与跨期接口

新增 `kernel/include/arch/sched.h` facade；后端放 `arch/{x86_64,aarch64}/cpu/sched_notify.c`，不在公共调度代码 include per-arch 头。

```c
uint64_t arch_kthread_default_mask(void);
void arch_sched_notify_remote(unsigned cpu);
bool arch_sched_scan_blockers(void);
```

- x86 默认 mask 保持现有 num_cpus/NR_CPUS 算法，notify 保持 `ipi_send(arch_processor_id, IPI_VECTOR_RESCHED)`；不得改变 x86 入队/EOI/抢占行为。
- aarch64 默认 mask 恒为 1；普通 `kernel_thread/create_kthread` 留在 BSP，显式 `kernel_thread_on_cpu` 支持任一 ready CPU，arch prepare 保留已验证的单 bit mask，不再强制 CPU0。所有 aarch64 新 kthread 最终都是单 bit affinity，现有 balancer 无可迁移线程，不新增迁移策略。
- `sched_notify_remote_cpu()` 验证边界与 ready，local 返回；远端先 atomic release-store need_resched=1，再调用 arch hook。P1 aarch64 hook 是明确的轮询通知后端，无 SGI；P2 后端发 SGI。删除旧 capability 分支及“远端为违规”的诊断，不能为开启 aarch64 而误连 x86 ipi_send。
- 默认 mask 的共同构造器最终 revalidation、先 task_list_lock 后 rq_lock 的发布顺序、失败回滚、out_pid 在发表前捕获的规则不变。不得在解锁后解引用可能已自回收的子 task。

P1 所有 aarch64 调度 idle（包括 BSP）每次最多消费一个 ap_work，再通过 rq_lock 检查运行队列，必要时 schedule，恢复 IRQ；即使没有 need_resched 也检查队列。空闲仅 pause，不 WFI。BSP 也轮询是为支持 AP 创建 BSP 线程；不能依赖 BSP tick 掩盖通知缺失。已有 ap_work 协议和迟到 AP 工作循环保持兼容。

P1 只承诺创建/yield/返回退出；跨核 blocker 正确性与行为验收在 P2。P1的 `arch_sched_scan_blockers()`：x86恒true，aarch64仅cpu_id()==0为true，公共schedule据此限制全局scan；同时aarch64 scanner先按不可迁移的 t->cpu 跳过非CPU0任务，再读取state/blocker。AP不允许调用blocker_wait（返回-EOPNOTSUPP），以免BSP扫描未同步发布的AP blocker。BSP现有blocker自测保持通过。P2移除AP调用限制并允许BSP扫描AP blocker，补齐下述锁合同；仍由BSP独占scan。

## 5. P2：SGI 与无漏唤醒协议

### 5.1 SGI 生命周期

逻辑 `IPI_VECTOR_RESCHED=0x41` 映射到 SGI 4，保留 SGI 0/1/2 探针和 SGI 3 TLB。`gic_init()` 在任何 AP 起动前注册 handler；`gic_cpu_init()` 每核启用 SGI4，优先级沿用 SGI3。handler 签名沿用 gic_irq_handler_t，只 release-store 本 CPU need_resched=1；不 schedule、不获取 rq/task-list 锁、不自行 EOI。现有 GIC driver 对 raw IAR（含 source bits）恰好 EOI 一次。通知发送使用 `ipi_broadcast(IPI_VECTOR_RESCHED, 1ULL<<cpu)`，按逻辑 CPU mask 走已有 target byte，不把 arch_processor_id 当逻辑 CPU。

发送在 READY/入队/状态发表后，执行 `dsb ishst` 再写 GICD_SGIR；区分普通内存发布、MMIO顺序和 IRQ EOI。同 CPU通知无需 SGI，非法/未就绪目标拒绝。SGI 可合并、重复、延迟，不对应“一任务一中断”，正确性由队列/slot状态保证。

### 5.2 唤醒生产者必须闭环

以下生产者均在发表状态、释放 rq锁后通知：构造器已有调用；`task_wake()` 成功入队分支；`task_finish_switch()` 对 wake-during-switch 的补入队分支（捕获 cpu 后在释放 on_cpu/锁后通知，绝不再读 prev）；`ap_work_submit()` READY 发布后。

`task_wake()` 的 already-on-rq/on_cpu 路径保留不重复入队，on_cpu 分支由 finish-switch 负责；通知 cpu 必须在持 rq锁时捕获，不能解锁后读 t->cpu。blocker check 在 task_list_lock 下运行，通知不能反向取该锁。

ap_work 可早于 scheduler ready 提交，但现有生产调用均在目标 ipi_ready之后。P2冻结此前提：`ap_work_submit()` 验证cpu边界且acquire-observe ipi_ready=1，否则log_err并fail-stop，不发表READY。每次release-store READY之后无条件调用新增arch-private `void aarch64_sched_kick(unsigned cpu)`，不读取scheduler_ok来决定是否发送；kick只要求ipi_ready，执行dsb ishst+SGI4，local可直接返回。arch_sched_notify_remote通过同一kick发送。ready之前AP可接SGI且继续轮询，handler不得使用current；handoff后首次idle iteration强制扫描slot。这样不存在producer读旧scheduler_ok而省略kick的store-buffering漏洞。保留单生产者、seq、DONE/timeout原语义，不扩展成多生产者队列。

### 5.2a blocker元数据与对象生命周期

P2公共blocker协议统一使用task_list_lock保护blocker字段及安装/清理；同时改变state时按task_list_lock→rq_lock次序。新增core内 `static void blocker_wake_locked(task_t *task)`，供已持task_list_lock的scanner调用；公开blocker_wake获取task_list_lock后调用helper，不允许持该锁的调用者递归调用公开入口。核对全部调用者（core scanner、exit、旧aarch64 selftest）；exit当前调用不持task_list_lock，保留公开入口并把其锁外blocker.type预检查移到helper内。

blocker_wait安装、二次check、立即返回清理、恢复后清理均持task_list_lock，state访问另持目标rq_lock；schedule、测试hook在释放两锁之后调用。callback仅在task_list_lock内执行，不持rq_lock，不得睡眠、分配或获取task_list_lock/反向锁；已存在waitpid、nanosleep、fb drain callback逐个审计。scanner在持task_list_lock时以rq_lock读取state快照，再调用check。signal分支也先清理blocker再task_wake。

helper在task_list_lock保护下确认条件，先清除blocker，再调用task_wake发表RUNNING/入队；task_list_lock使已恢复线程无法完成blocker清理/退出/回收，helper解锁后不得再读task。直接task_wake的消费者也在恢复后按同一锁清理blocker。测试控制对象通过独立BSS记录完成/回收，不以自回收task指针充当所有权。保留check已满足立即返回、wake-before-switch的on_cpu交接和两次并发wake只入队一次。

### 5.3 idle 检查与 WFI

只允许在 idle task 栈、IRQ 原先开启、无任何锁时休眠。一次迭代：

1. IRQ开启时执行至多一个 ap_work（扩展处理可能访问 VMM，不能在 rq锁或 IRQ屏蔽期间执行）。
2. 屏蔽本核 IRQ；持 rq_lock 检查 need_resched/runqueue，同时 acquire-load ap_work.state。队列非空或 need_resched 时先离开锁，再 schedule；READY 时离开锁并恢复 IRQ，回到消费路径。
3. 确认无工作后释放 rq_lock，保持 DAIF.I=1；执行 `dsb sy; wfi`，随后 enable IRQ+ISB，再重检状态。不能在 WFI 前开 IRQ，否则 SGI 可能被处理完后才睡眠。整个 WFI 期间不得持 rq锁。

这里依赖 GIC CPU interface、priority mask、distributor 仍启用且向 EL1 路由的物理 IRQ；PSTATE.I 仅阻止异常进入，不使 pending IRQ 失去 WFI 唤醒能力。入队发生在锁内检查之前则被观察，之后则 SGI 在 IRQ masked 的窗口保持 pending 并使 WFI 返回；虚假唤醒只重检。不得用 AP timer 或周期轮询兜底，也不得声称打断不合作的运行中线程。

架构依据：[Arm Critical Interrupt Prioritization, WFI wake-up event 与 PSTATE mask](https://documentation-service.arm.com/static/5ed6547eca06a95ce53f9321)。实施时对照该契约及现有 GIC 配置；测试必须覆盖已 pending 与进入 WFI 后到达两种情况。

scan-only blocker 兼容：BSP 独占 CNTP 继续推进全局时间；BSP idle 观察 need_resched 后调用 schedule，其公共 scan 可发现其他 CPU 上满足条件的 blocker，再经 task_wake 通知目标。不得让 AP 同时推进全局 jiffies/softirq。显式唤醒不依赖该 fallback，测试用计数证明实际走哪条路径。

## 6. 并发、诊断与兼容

- task-list 与 rq 锁顺序沿用公共实现；不在 rq锁中发送 IPI，不在 IRQ handler中调度。
- P1即统一aarch64 need_resched 的读/写/清零为 __atomic builtins，并审查 common core、fair::update_curr、notify、tick 的同字段访问；不允许混用 plain/volatile访问构成跨核 C 数据竞争。清零只在 rq锁保护的调度决策中，不可在解锁后抹掉新通知。scheduler_ok审计包括common boot、aarch64 handoff、x86 smp AP发布，以及构造器/placement/balance/schedule全部读者；配对访问统一atomic release/acquire。x86 汇编字段偏移不变。
- AP 无周期 tick；cooperative 线程可能长时间独占 CPU，这是公开边界。counter timeout 用硬件计数器，不能用 AP本地 tick。
- 多核 selftest：首次入口汇编观测改为每 CPU槽；资源跟踪表在 IRQ-safe专用锁下保护，禁止调用 allocator/logger；switch/timer/nested计数每CPU；BSP最终聚合输出。现有 fault 目标仅 CPU0，AP不得污染其观测。BSP在 AP参与测试前开启观测，所有目标已退出且完成回收后再关观测；晚到 observer 不能读已销毁测试状态。
- 资源测试按CPU分批执行，全局一次仅一个活动批次；新case每批256个任务，旧1000任务case分4批250并保留累计验收。批次目标标签不按分配/释放所在CPU判定。每次alloc登记单调instance token，free前标FREEING并捕获token，实际kfree之后按token记录完成，不在释放后解引用对象；同地址新alloc不能被旧token误释放。完成回收后才能重置代次；表容量至少512条活跃记录，满表必须失败。每CPU分别验证256个stack和256个context的真实分配/释放平衡。
- 普通版本不链接故障/测试 hooks。新增运行日志仅 log_*；协议输出集中在 selftest允许路径，AP不并发打印完整协议行。
- 汇编fatal路径在进入共享emergency stack之前，以无栈的ldaxr/stxr一次性争抢BSS fatal-owner。只有获胜CPU安装原emergency SP并调用C诊断；其他CPU屏蔽IRQ后无栈停机，owner永久不释放。不依赖current或失效SP选择栈。新增双CPU同时拒绝上下文故障测试，确认arrivals_mask=3、entrants=1、losers=1后才接受单owner证据；保留原CPU0单故障证据。
- 旧 scheduler AP-case 要从“AP拒绝/CPU0-only”升级为“默认仍 CPU0、显式AP成功”，对应 parser 同步升级，不通过删掉整个用例逃避。旧 smp/M3/no-ack/fault suite保持各自断言语义。

## 7. 验收合同

两个 plan 分别新增 `MODE=sched-ap` 与 `MODE=sched-wake`，不覆盖 MODE=sched。共用新 runner `qemutests/aarch64_ap_scheduler.py --phase ap|wake --self-test`；各自产出独立 selftest variant `sched-ap`/`sched-wake`。selector 为 `AARCH64_AP_SCHED_TEST=ap|wake|fatal-owner`，必须 KERNEL_SELFTEST=1，拒绝与旧 AARCH64_SCHED_TEST、M1、sync-fault、no-ack、OS01_SYSTEST 等互斥组合。P1接受ap和独立2核fatal-owner故障验证（variant sched-ap-fatal-owner，AP_SCHED_CASE=fatal-owner）；P2才扩为wake。所有递归make白名单、help、catalog、test-harness接线和文档一起更新。

统一证据：BSP单写，每行 `[ap-sched] phase=<ap|wake> case=<id> cpu=<logical> ...`，START/END各一次，固定case集合，每case每CPU只一次。parser拒绝重复、未知、缺失、越界CPU、错误phase、非数字、负值和timeout；不因仅有PASS字样放行。运行报告保存命令、revision/dirty、CPU、image/ELF/firmware SHA256、日志和退出原因。测试marker只证明所记录行为，不能证明物理省电。

P1 case IDs：ready、entry、yield、reap、admission、default-mask、work。ready/entry/yield/reap/work 对每个录用CPU（包括BSP），admission/default-mask为BSP聚合。每CPU两个任务，各32次yield，校验寄存器/栈哨兵、cpu及单bit亲和性；每CPU256个短命任务，原始栈/context恰好释放一次且在其他栈上，idle永不释放。远端任务可能在构造器返回前退出，测试不能保存无所有权指针再解引用。覆盖 AP→BSP、BSP→AP与AP间创建（单核退化本地），无效/未ready目标 -EINVAL且零发表。ap_work在handoff前后均完成原seq协议。

P2 case IDs 在P1基础增加：remote-wake、wake-switch、idle-race、sgi、work-sleep、scan-fallback。用可控屏障分别触发检查前、检查后/WFI前、WFI等待期间、wake-during-switch四种顺序；每AP每种至少100轮，序号/完成计数不丢不重。sgi记录实际SGI4 handler进入与EOI证据，重复SGI允许但不得重复执行任务。测试不依赖 BSP周期scan为显式唤醒兜底，独立计数验证 explicit路径；WFI证据是进入/返回计数和pending IRQ时序，不宣称证明QEMU实际低功耗。无新timer、IRQ内context switch计数为0。

矩阵：1/2/4/8核各2次boot，512 MiB，每boot120秒硬超时，所有条件等待2秒硬件计数期限。单核无AP项标为明确not-applicable而不伪造成功，aggregate项仍运行。P2显式跨核项要求2核及以上。

两期完成均需：x86 test-host、test-static、phase-0、独立kernel-selftest(SMP2/8)、`OS01_SYSTEST=1 test-qemu SUITE=systest`；aarch64 common compile、audit、MODE=sched/smp/no-ack/m3-probe/gic-spi/sync-fault、新phase suite；test-harness。P2还回归MODE=sched-ap。struct若确需改变必须先修订设计并make clean；一般头/汇编接口变化也clean两个profile再验收。报告未跑项不可记PASS。

## 8. 文档、评审和实施顺序

1. 独立agent评审本spec，逐项核对实际源码，修订直到明确APPROVE。
2. 作者依次编写P1、P2 plan，逐份独立评审、修订直到明确APPROVE；P2执行前仍需rebase核对。
3. 保存每轮结论、发现、处理理由与最终文件SHA256到同一review报告。agent批准只代表文档可实施，不代表产品通过测试。
4. 本轮到此停止。后续另行授权Luna执行；不得由本轮评审agent进入实施。

实施完成后更新roadmap、arch文档、build说明与changelog，明确P1轮询边界/P2协作式边界；旧设计的BSP-only陈述标为历史阶段，不回写历史验收成绩。
