# aarch64 Cross-CPU Wakeup P2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 用户指定后续实施者为 Luna，每次执行一个完整 plan。本轮只写文档及评审，禁止派实施 agent。

**Goal:** 以SGI4通知远端可运行工作，让aarch64 idle在无AP timer时安全执行WFI，支持跨核显式blocker唤醒和BSP scan fallback。

**Architecture:** 在P1已验收的AP task-stack调度上添加resched SGI。所有工作发表后通知；IRQ handler只置need_resched，调度只在协作安全点发生。idle保持DAIF.I=1跨越最终无工作检查到WFI，避免通知已被处理后才睡眠。

**Tech Stack:** freestanding C、AArch64汇编、GICv2 SGI/MMIO、QEMU、Python unittest、现有production-object hosttests。

**Spec:** [总设计](../specs/2026-10-31-aarch64-ap-cooperative-scheduler-design.md)。依赖[P1 plan](2026-10-31-aarch64-ap-sched-p1-cooperative.md)，其文档已独立APPROVE；P1产品尚未实施。本plan为基于冻结接口的实施合同，不能假定未来P1树已存在。

## Global Constraints

- 执行前P1必须实现、独立评审APPROVE、全门禁通过；记录实际P1 commit并逐项核对本plan文件/API，漂移先修订plan并重新评审，再实施。
- 不改变UAPI、boot_context、thread_t、percpu_t布局；继续-mgeneral-regs-only。
- 无AP timer、IRQ返回抢占、迁移、EL0或FP/SIMD；所有线程仍须yield/阻塞/返回。
- 普通aarch64线程默认CPU0，显式pinned任务单bit affinity；BSP独占全局CNTP/jiffies/softirq及blocker scan。
- 入队和状态发表在前，释放rq锁后通知；IPI handler无schedule/rq/task-list锁，无自行EOI。
- ap_work提交以ipi_ready为硬前提，READY后无条件kick，不读取scheduler_ok决定是否发送。
- WFI期间IRQ masked且无锁；工作处理在IRQ开启且无rq锁时执行。
- 所有条件等待2秒硬件计数期限；矩阵1/2/4/8核各2次、512 MiB、每boot120秒。
- task-list→rq锁序、生命周期及资源token观测沿用spec/P1；运行日志用log_*。

## Review Focus

1. 检查后/WFI前通知、已pending IRQ：SGI不在WFI前被消费（T3 masked-WFI测试）。
2. scheduler-ready与READY并发发表：不能因读旧ready省略kick（T3交接测试）。
3. wake-during-switch及两次wake：恰好一次入队，finish-switch不访问已释放prev（T2生产core测试、T4竞争矩阵）。
4. 远端线程恢复后立即退出：blocker清理完成后才可运行，waker不再读悬空task（T2锁/生命周期测试）。
5. SGI合并/重复与raw IAR source bits：不按中断数计任务，EOI恰好一次，旧TLB SGI3不退化（T1 MMIO/dispatch及T4共存测试）。

## 文件与任务边界

| Task | 修改/创建 | 负责内容 |
|---|---|---|
| T1 | 改arch/aarch64/{cpu/sched_notify.c,intr/ipi.c,intr/gic.c}、include/arch/aarch64/ipi.h；hosttests/cases/test_aarch64_ipi_broadcast.c，新增test_aarch64_resched_sgi.c及intr.mk注册 | SGI4、notify后端、handler/EOI |
| T2 | 改sched/core.c、sched/exit.c、include/sched/{task,internal}.h、selftest/test_aarch64_ap_scheduler.c、selftest/test_aarch64_scheduler.c；新增hosttests/cases/test_blocker_remote_wake.c及mock/blocker_runtime.h、sched.mk注册 | blocker锁/生命周期、所有enqueue通知 |
| T3 | 改arch/aarch64/{cpu/idle.c,smp/ap_work.c}和include/arch/aarch64/task_arch.h；新arch/aarch64/cpu/idle_wait.S；现有test_ap_work_protocol及mock；新hosttests/cases/test_aarch64_idle_wakeup.c、mock/aarch64_idle_runtime.h、sched.mk注册 | 无漏通知的ap_work与WFI |
| T4 | 改sched/core.c、arch/aarch64/cpu/{idle.c,ap_sched.c}、selftest/{test_aarch64_ap_scheduler.c,aarch64_sched_observer.c}及对应头、arch/aarch64/intr/gic_driver.c（仅测试观测hook）、qemutests/aarch64_ap_scheduler.py/test_aarch64_ap_scheduler.py、kernel/Makefile、mk profile/targets/run/catalog及catalog测试 | wake variant、时序控制、矩阵和交付 |

路径展开规则：表中及下文以arch/、sched/、include/、selftest/、time/开头的路径均是kernel-relative简写，完整路径统一加 `kernel/` 前缀；hosttests/、qemutests/、mk/、docs/、kernel/开头则已是仓库相对路径。mock/在hosttests任务中展开为hosttests/mock/，sched.mk/intr.mk展开为hosttests/mk/下对应文件。生产代码测试使用原TU和窄mock依赖，不能复制算法；host证明状态机/调用顺序，真正GIC/WFI由QEMU证明。test-harness无预构建ELF依赖，真实符号审计在suite prepare后执行且缺ELF失败。

### Task 1: 先接SGI传输，保留P1轮询idle

**Interfaces:** 保留 `void arch_sched_notify_remote(unsigned cpu)`；在include/arch/aarch64/ipi.h新增 `void aarch64_sched_kick(unsigned cpu)`、`void aarch64_resched_sgi_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)`。kick只依赖目标ipi_ready，不要求scheduler_ok；无效/未IPI就绪目标log_err并fail-stop，不发送。common notify已有admission检查不变。

- [ ] 扩展真实ipi.c+gic_driver.c host测试：0x41映射4，0x40仍映射3，mask=2和缓存targetbyte=2写SGIR=4|(2<<16)，空mask不写；unsupported fatal改测0x42而非旧0x41。添加handler test：need_resched由0变1、未schedule/未取锁。
- [ ] 注入raw IAR=(source_cpu<<10)|4，gic_dev_dispatch完整EOIR值等于原IAR，恰一次；同时验证SGI3仍调用TLB handler，重复SGI4不重复入队。运行已有 `make -C hosttests test-aarch64-ipi-broadcast`（若无该alias，先在intr.mk注册同名focused target）及新 `make -C hosttests test-aarch64-resched-sgi`，记录新增断言RED。
- [ ] ipi_vector_to_sgi增加0x41→4；GIC CPU whitelist启用4，gic_init在AP启动前注册resched handler，优先级与SGI3一致。handler只atomic release置本核need_resched，不访问current，不自行EOI；boot栈也能安全处理该IRQ。
- [ ] ARM sched_notify后端实现kick：边界检查→acquire ipi_ready→本地返回（common已置need，ap_work本地生产者不会同时睡眠）→dsb ishst→ipi_broadcast(0x41,1ULL<<cpu)。硬件屏障放arch后端，保持ipi.c/gic_driver.c host可编译，不向dependency-light driver塞target asm。
- [ ] 加构建检查确认普通ARM含SGI4 handler/notify，x86后端及TLB mapping不变。跑上述hosttests、ARM MODE=sched-ap/旧MODE=sched/m3-probe/gic-spi；此时仍忙等，先确认传输无回归。提交 `feat(aarch64): deliver remote reschedule requests with SGI4`。

### Task 2: blocker协议与全部任务唤醒生产者

**Interfaces:** 对外保留 `int blocker_wait(blocker_check_t check,int type,bool signal_can_wake)`、`void blocker_wake(task_t *task)`、`void task_wake(task_t *task)`。新增core内static `void blocker_wake_locked(task_t *task)`，只供持task_list_lock的路径调用，不公开给任意外部模块。arch_sched_scan_blockers仍仅在ARM BSP true，P2移除AP blocker_wait拒绝及scanner跳过非CPU0任务的限制。

- [ ] 新host harness编译真实core.c及依赖的最小生产fair/boot对象，用mock switch/IRQ/scheduler观察状态，禁止重新实现blocker函数。覆盖：check初始true不睡；发布后二次check为true不睡；blocked后显式wake；wake-before-switch on_cpu=1由finish-switch补入队；两次wake不双入队；waker清理完成后waiter立即退出；signal路径先清理再RUNNING。mock callback断言持task_list_lock、不持rq_lock；mock schedule断言两锁均未持有。
- [ ] 新 `make -C hosttests test-blocker-remote-wake` 首先RED；现有x86 wait/exit/selftest调用者用真实源码逐个核对，列出当前是否已持task_list_lock。scanner必须调locked helper；exit当前在锁外调用公开入口，但其blocker.type预检查必须移到helper内；旧selftest hook仍在schedule前、无锁时调用公开入口。
- [ ] 按spec §5.2a改blocker_wait：task_list_lock保护callback、blocker安装/二次check/立即清理和resume清理；改state时嵌套目标rq_lock。不持锁调用schedule或测试before-switch hook。callback不能sleep/分配/反取task-list锁；审计waitpid/nanosleep/fb_drain现有callback均满足后才改公共协议。
- [ ] blocker_wake公开入口的调用者必须已保证task存活，获取task_list_lock后调helper。helper以rq_lock读取状态快照，释放rq锁后check，在task_list_lock内清blocker，再task_wake；释放task_list_lock后不再读task。scanner同样在list→rq下取state快照，处理条件和signal时都先清理，再发表wake。此处不新增task引用计数；测试waker用等待对象/测试屏障保障存活，禁止在锁前传可能已退出的裸指针。
- [ ] task_wake在rq锁内捕获target_cpu，成功入队且释放锁后调用sched_notify_remote_cpu，删除旧解锁后读t->cpu的访问。already-on-rq/on_cpu不重复入队；on_cpu分支把RUNNING交由task_finish_switch处理。finish_switch在有效prev阶段捕获补入队与CPU，release on_cpu并解锁后仅用捕获值通知，不再解引用prev；自身CPU通知由common local路径跳过。
- [ ] 生命周期压力两类分开：立即退出用一次合法wake，确保helper解锁后无访问；双wake用目标退出屏障，两个waker完成后才允许target返回，避免把悬空API调用伪装成合法并发。触发FREEING token期间observer end仍失败。
- [ ] 同步升级P1的AP blocker拒绝测试：在旧scheduler和AP suite对应worker中把期望-EOPNOTSUPP改成condition初始true返回0，断言未阻塞/state保持RUNNING、blocker为空且无额外入队；新增host断言也同步。不得删除该检查或维持旧拒绝条件；远端真正睡眠压力留T4。
- [ ] 运行新host target及旧test-host、ARM旧MODE=sched和P1 MODE=sched-ap。x86跑phase-0、独立systest（顶层OS01_SYSTEST=1）和kernel-selftest SMP2/8，确认公共blocker改动不破坏用户等待。通过后提交 `fix(sched): serialize remote blocker publication and wakeup`。

### Task 3: ap_work通知与masked WFI一起交付

**Interfaces:** `ap_work_submit(cpu,seq,cmd,arg0,arg1)`签名不变，新增前提cpu<NR_CPUS且目标ipi_ready=1，违例log_err/fail-stop且不写slot；READY release-store之后无条件aarch64_sched_kick(cpu)。`arch_aarch64_idle_step(void)`签名不变，调用要求idle task栈且IRQ开启；返回也IRQ开启。新增arch-private `void aarch64_idle_wait_irq(void)` 在include/arch/aarch64/task_arch.h声明，生产实现cpu/idle_wait.S严格为 `dsb sy; wfi; ret`（入口IRQ masked，无锁，不改变DAIF）；C调用后才enable IRQ+ISB，host链接同签名mock。保留P1 before-release及未录用AP的轮询循环。

- [ ] 扩展真实ap_work生产TU hosttest：READY payload发布后才kick；scheduler_ok=0但ipi_ready=1仍kick；非法cpu/ipi_ready=0不发表slot；seq/DONE/timeout原用例保持。host mock添加kick和ready字段，不用测试专用宏跳过生产前提。
- [ ] 新idle hosttest编译真实idle.c，以预包含mock头替代IRQ accessor，链接mock aarch64_idle_wait_irq（不编ARM .S）。记录事件序列，断言rq-lock检查→unlock→wait helper期间IRQ masked，无锁跨WFI；有队列/need/READY时不睡；ap_work handler执行时IRQ开启。至少覆盖空检查前、检查后/DSB前、WFI返回后到达及虚假唤醒四条路径。真实ELF反汇编审计wait helper只有dsb sy/wfi/ret且无DAIF开中断；test-harness只用模拟反汇编输出。先运行 `make -C hosttests test-aarch64-idle-wakeup` 确认RED。
- [ ] 实现spec §5.3流程：IRQ开消费至多一个slot；IRQ-save、rq锁检查need/runqueue/slot READY；有队列或need则解锁后schedule并恢复IRQ，有READY恢复IRQ后下轮消费；无工作解锁但保持DAIF.I=1，dsb sy/wfi后enable IRQ+ISB。不可WFI前开IRQ，不保留任何锁，不用tick兜底。
- [ ] boot handoff首次idle扫描仍强制执行；ap_work无条件kick消除跨scheduler-ready漏通知。BSP也用同一WFI协议，BSP timer仅负责时间/scan fallback；AP CNTP保持关闭。不改变ap_work单生产者/未知cmd保留READY/seq去重/timeout终止语义。
- [ ] 运行ap_work/idle host tests、ARM MODE=sched-ap/sched/m3-probe/no-ack。先提交带真实现有suite回归的 `feat(aarch64): sleep idle CPUs without losing remote work`；新增确定性交错矩阵由T4提供，提交说明不得将它提前记PASS。

### Task 4: wake独立变体、确定性竞争证据与最终验收

**Files:** 文件表T4；新增reports/2026-10-31-aarch64-ap-sched-p2-validation.md；改docs/{roadmap.md,arch.md,build/build.md,changelog.md}。

**Interfaces:** 增加 `AARCH64_AP_SCHED_TEST=wake` 与 `MODE=sched-wake`，kernel/image独立sched-wake目录；沿用P1 before_release/run测试入口，runner `--phase wake`（其他参数同P1），phase=ap仍支持。wake正向START/END同P1但phase=wake，accepted_mask仍必须全在线。以下新增case固定schema，所有字段十进制非负整数、必须全部出现。

| case | CPU集合 | 字段与通过条件 |
|---|---|---|
| remote-wake | 每AP；UP为cpu0 NA | rounds=100 completed=100 duplicate=0 explicit=100 scan=0 na=0 |
| wake-switch | 每AP；UP为cpu0 NA | rounds=100 completed=100 duplicate=0 on_cpu_wakes=100 finish_enqueues=100 na=0 |
| idle-race | 每AP；UP为cpu0 NA | before_check=100 after_check=100 at_wait=100 completed=300 lost=0 wfi_enter>=100 wfi_return>=100 na=0 |
| sgi | 每CPU | handler>=1 eoi=handler bad_iar=0 irq_switches=0 tlb_ack_ok=1；UP通过一次self-targeted SGI4硬件探针保证handler>=1 |
| work-sleep | 每AP；UP为cpu0 NA | ready_overlap=1 pending_before=100 after_wait=100 completed=201 bad_seq=0 na=0 |
| scan-fallback | 每AP；UP为cpu0本地 | rounds=100 completed=100 explicit=0 scan=100 na=0 |

UP的NA行依然每case恰一行、cpu=0、na=1，其余上表数值字段全为0（包括wfi计数）；sgi和scan-fallback不NA。继承P1全部case/schema，但phase=wake。2/4/8核新增每AP行，不允许缺AP降级成功。P1 fatal-owner仍独立验证，不强行套正向case。

- [ ] 先写完整正向fixture与负向变体：少CPU/缺case、重复、wrongphase、NA伪成功、explicit被scan兜底、EOI重复或丢失、work seq错、completed少1都失败；mock runner timeout/异常清理仍满足P1合同。`python3 qemutests/aarch64_ap_scheduler.py --phase wake --self-test` 在新增schema未接前应失败，完成后PASS。
- [ ] 加编译期测试hook，仅wake variant包含。idle阶段BSS握手状态为CHECK_ARMED/CHECK_DONE/WAIT_ARMED/RETURNED，按(cpu,round)序号release/acquire；BSP可控制目标停在指定阶段。所有屏障2秒期限，AP不通过本地timer退出；操作最终走真实constructor/task_wake/ap_work/kick，不直接改rq或伪造IRQ。
- [ ] 冻结hook头为 `kernel/include/selftest/aarch64_ap_scheduler.h`，kernel/Makefile仅selector=wake定义 `AARCH64_AP_SCHED_WAKE_TEST=1`；所有声明/调用/实现由该宏包围，普通与ap/fatal-owner/旧sched变体没有这些时序符号。接口及精确调用点如下表，host测试显式开宏并提供mock。

| Hook精确签名 | 调用点及约束 |
|---|---|
| `void aarch64_ap_sched_idle_hook(unsigned cpu, unsigned stage)` | idle.c；stage=1在消费ap_work后/IRQ-save前，IRQ开；stage=2在最终无工作检查并释放rq_lock后，IRQ masked；stage=3紧邻wait helper调用前，IRQ masked，publish WAIT_ARMED后立即返回；stage=4在wait helper返回并enable IRQ+ISB后，IRQ开。stage整数1/2/3/4在头中用具名enum固定；均不持锁。仅stage=1/2可等BSP许可，最多2秒 |
| `bool aarch64_ap_sched_skip_scan(task_t *task)` | core.c scanner已持task_list_lock、IRQ masked，且尚未读取测试目标blocker时；仅atomic读取目标身份/代次并返回，无等待/新锁/日志，true跳过指定explicit测试对象 |
| `void aarch64_ap_sched_after_dequeue(task_t *task)` | core.c schedule在current dequeue/条件reenqueue及释放rq_lock后、scan前，与旧observe_block_enqueue相邻；IRQ masked，不持锁，最多2秒测试握手 |
| `void aarch64_ap_sched_before_finish(task_t *prev)` | core.c task_finish_switch最开始、获取rq_lock与清on_cpu前；在incoming task栈，IRQ masked，不持锁，最多2秒握手。读取prev必要身份后仅等待BSS控制字，prev在此点仍有效 |
| `void test_aarch64_ap_sched_before_ready(unsigned cpu)` | ap_sched.c AP已装合法idle栈、enable IRQ/ready发表前，无锁IRQ masked，最多2秒 |
| `void test_aarch64_ap_sched_poll_ready(void)` | BSP ready等待循环，无锁IRQ开；处理CHECKPOINT并投递work，不睡眠 |
| `void aarch64_ap_sched_sgi_trace(unsigned cpu, uint32_t raw_iar, bool after_eoi)` | gic_driver.c仅intid=4的dispatch前与真实EOIR写完成后各调用一次；IRQ masked、不持rq/task-list锁，不等待，不自行EOI。记录完整raw IAR和实际进入/完成计数到perCPU槽；observer据此记SGI IRQ active而非timer-only |

所有等待超时只设置BSS failure并释放其他测试等待者，BSP聚合输出FAIL；无法恢复的目标由runner硬超时判FAIL，不能把坏同步当SKIP。观测锁不得跨hook等待，普通生产锁次序不因测试改变。
- [ ] ready_overlap每AP每boot只测一次真实one-shot ready交接，不清零scheduler_ok或重做handoff。新增测试入口 `void test_aarch64_ap_sched_before_ready(unsigned cpu)`（AP真实idle栈、IRQ masked、无锁、enable IRQ/发表ready前），release发布CHECKPOINT并等BSP许可；`void test_aarch64_ap_sched_poll_ready(void)` 在BSP release后的ready等待循环调用（IRQ开、无锁），看到CHECKPOINT后用真实ap_work_submit提交WORK_BARRIER，再允许AP继续。AP就绪后消费并ACK，记录submitted_before_ready=1和consumed_after_ready=1，汇总ready_overlap=1。before_release入口只配置静态测试状态；不承担重复ready发布。每AP另100轮pending-before及100轮after-wait，故completed=201。
- [ ] 构造idle-race三个时序：入队前检查可见；CHECK_DONE后IRQ masked时入队，SGI pending后允许目标执行WFI；WAIT_ARMED后由发送方延迟至目标可执行WFI再发。最后一项只证明WFI路径返回，不宣称PE实际低功耗；真实QEMU采用AP无timer，检查任务确实完成而非只看handler计数。每种100轮。
- [ ] 显式blocker竞争用static BSS控制对象，条件atomic发布；wake-switch通过after-dequeue/before-finish的现有selftest hook屏障令on_cpu=1时wake。新增test-only scan过滤标记使explicit目标完全跳过BSP fallback scan（只跳过指定测试对象，不暂停BSP timer），记录scan=0；scan-fallback独立关闭explicit发送、保留BSP timer发现条件后通知AP。
- [ ] SGI测试沿用真实GIC dispatch trace/host EOI spy；runtime累计handler/EOI在dispatch对应阶段采样，完整raw IAR单写对应记录，不以expected计数冒充observed；支持同一SGI合并。TLB共存不调用固定seq的late ap_work probe：在所有目标无测试屏障且能处理IRQ的时刻，BSP先快照各核tlb_ack_gen，发SGI4后直接调用 `void tlb_shootdown(void)`（kernel/include/memory/vmm.h），验证各目标generation恰好增1，记录tlb_ack_ok=1。全局shootdown锁确保串行，测试禁止其他并发shootdown源。UP也验证本核ACK。既有独立MODE=m3-probe继续回归；不改变旧probe序号或其历史证据。测试结构观测每CPUIRQ active覆盖SGI4，证明IRQ内context switch计数为0。
- [ ] pre/post-handoff work及work-sleep使用P1 slot IDLE→继承seq+1单生产者规则，unknown cmd保留READY也要测试不丢，不能引入多生产者。记录和资源汇总由BSP单写，所有目标已释放后才END。
- [ ] 接profile/kernel宏/受控submake/显式路径/prepare-run/mode whitelist/help/catalog/test-harness。新增aarch64-sched-wake catalog，保持MODE=sched-ap和fatal-owner可运行；正向参数/互斥组合外层make就拒绝。普通ELF不含时序注入hook。
- [ ] 跑 `python3 -m unittest qemutests.test_aarch64_ap_scheduler qemutests.test_aarch64_ap_boot_contract qemutests.test_aarch64_harness qemutests.test_scenario_catalog qemutests.test_suite_catalog qemutests.test_inventory`，新host targets和全test-host通过。
- [ ] clean两profile后执行[P1最终门禁表](2026-10-31-aarch64-ap-sched-p1-cooperative.md#task-5-独立变体严格runner与最终交付)全部项目，另 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake`（1/2/4/8各2次）。记录逐boot命令、image/ELF/firmware SHA256、revision/dirty、stdout/退出原因；失败或未跑不能算PASS。
- [ ] 更新文档：P2完成表示跨核协作式唤醒和idle WFI，不表示抢占；AP timer/自动负载均衡/EL0仍待办。保存P1实际基线、改动及最终测试报告。提交 `test(aarch64): verify SGI wakeup and idle race boundaries`。
- [ ] 最终diff独立评审明确APPROVE后停止，不自动推进每核timer或IRQ抢占。

## 作者自审与本轮状态

spec §5.1→T1；§5.2/§5.2a→T2；ap_work交接与§5.3→T3/T4；§6观测→T4；§7独立variant及旧suite回归→T4。P1接口漂移在执行前阻断，不能由Luna临场重新设计。

本轮只编写文档，未实施或运行以上产品测试。P2独立评审已明确 **APPROVE**，记录见对应评审报告；批准不自动授权实施。
