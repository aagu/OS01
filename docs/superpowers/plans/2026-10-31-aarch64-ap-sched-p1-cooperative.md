# aarch64 AP Cooperative Scheduler P1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 用户指定后续实施者为 Luna，每次执行一个完整 plan。本轮只写文档及评审，禁止派实施 agent。

**Goal:** 在1/2/4/8核aarch64上，以忙等idle运行显式pinned内核线程，完成创建、yield、退出与异栈回收。

**Architecture:** 保留现有SMP ACK、TLB和ap_work路径；BSP完成原启动探针及自身task-stack handoff后释放AP。AP在永久独立idle栈上发布scheduler_ok，使用公共EEVDF调度器。普通线程默认CPU0，所有aarch64线程单bit affinity。

**Tech Stack:** freestanding C、AArch64汇编、Clang、GNU make、QEMU virt/GICv2、Python unittest、现有hosttests。

**Spec:** [总设计](../specs/2026-10-31-aarch64-ap-cooperative-scheduler-design.md)，spec R2已独立APPROVE，详见[评审记录](../reports/2026-10-31-aarch64-ap-sched-document-review.md)。

## Global Constraints

- 不改变UAPI、boot_context、thread_t、percpu_t布局；STACK_SIZE=32 KiB，SP 16-byte对齐，current仅在task栈上使用。
- 不实现EL0、AP timer、resched SGI、IRQ抢占、迁移或FP/SIMD；继续-mgeneral-regs-only。
- P1所有aarch64 scheduler idle忙等；普通kernel_thread/create_kthread默认mask=1，pinned任务mask=1ULL<<cpu。
- AP admission来自冻结的boot ACK mask；online/ipi_ready与scheduler_ok分开，失败或迟到AP不被录用。
- P1仅BSP扫描blocker，扫描先跳过AP再读state；AP blocker_wait返回-EOPNOTSUPP。
- 新公开头放kernel/include/对应目录，公共调度代码不include per-arch头；日志用log_*。
- 所有条件等待2秒硬件计数期限；矩阵1/2/4/8核各2次、512 MiB、每boot120秒。
- 每个任务先建立能失败的实际行为测试，再实现，再跑对应检查；只在相关检查通过后提交该任务。最终全门禁不以局部测试替代。

## Review Focus

1. ACK mask有洞、迟到/未ready AP：绝不投递到这些CPU（T3 admission故障与T5 parser）。
2. AP孩子在构造器返回前退出：无解锁后child指针解引用，资源全回收（T1生产构造器host test、T4回收）。
3. 多核观测并发与地址重用：不静默丢资源记录、不串CPU/代次（T2 observer test、T4压力）。
4. AP启动切栈与双CPU fatal：无boot栈current、无共享emergency栈并发使用（T3普通启动和独立fatal变体）。
5. BSP blocker与AP schedule同时发生：AP不扫描/等待，BSP旧blocker语义保留（T1 scan策略及T4旧suite）。

## 文件与任务边界

| 任务 | 创建/修改 | 职责 |
|---|---|---|
| T1 | 新include/arch/sched.h；新arch/{aarch64,x86_64}/cpu/sched_notify.c；改sched/{core,kthread,balance,boot,fair}.c、time/tick.c、arch/x86_64/smp/smp.c | mask、notify、scan facade和atomic配对 |
| T2 | 改sched/kthread.c、selftest/test_aarch64_scheduler.c、include/selftest/aarch64_scheduler.h、arch/aarch64/cpu/thread_entry.S；新selftest/aarch64_sched_observer.c及对应头 | 并发观测，共享资源记录与每CPU入口槽 |
| T3 | 新arch/aarch64/cpu/ap_sched.c与include/arch/aarch64/ap_sched.h；改aarch64 cpu/{task_arch.c,thread_entry.S,idle.c}、smp/smp.c、include/arch/aarch64/{smp,task_arch}.h | AP release/handoff、轮询idle、fatal owner |
| T4 | 新selftest/test_aarch64_ap_scheduler.c与include/selftest/aarch64_ap_scheduler.h；改旧scheduler生产者/parser | 新行为用例和旧断言升级 |
| T5 | 新qemutests/aarch64_ap_scheduler.py、test_aarch64_ap_scheduler.py；改kernel/Makefile、mk/{project.mk,profiles/aarch64-clang.mk,targets/aarch64.mk,components/run.mk,tests/catalog.mk}及catalog/inventory测试 | variant/runner/门禁/交付 |

路径以上均相对仓库根；host新增测试统一放hosttests/cases/，mock放hosttests/mock/，按所属模块注册hosttests/mk/{sched,intr}.mk。kernel/Makefile显式aarch64白名单必须纳入新增C文件；arch cpu目录汇编仍由既有收集规则处理。x86 wildcard是否收集arch文件，执行前以真实Makefile核对，不添加重复目标。

### Task 1: 冻结共同接口和P1并发策略

**Files:** 文件表T1；hosttests/cases/test_kthread_ctor.c、test_switch_complete.c；新增test_sched_notify.c；mock/switch_complete_test_runtime.h；hosttests/mk/sched.mk；include/sched/{boot,internal}.h按声明需要更新。

**Interfaces:** 消费现有sched_notify_remote_cpu(unsigned)、sched_boot_start(void)、kernel_thread_on_cpu(fn,arg,cpu)。产出arch/sched.h中的 `uint64_t arch_kthread_default_mask(void)`、`void arch_sched_notify_remote(unsigned cpu)`、`bool arch_sched_scan_blockers(void)`；现有公共API签名不变。

- [ ] 扩展生产构造器host测试：mock arch mask=1，即使CPU1更空，默认创建仍cpu0/mask1；显式cpu1创建成功且mask2；invalid/online-only/ready-only目标返回-EINVAL、无task-list/rq发表、无分配泄漏。模拟enqueue解锁后子对象失效，通知只使用已捕获CPU/PID，不读对象。
- [ ] 新生产notify host测试：local不调arch hook；远端先need_resched=1后hook，invalid/not-ready不调用；x86后端目标采用arch_processor_id及0x41。真实balance.c链接mock hook，禁止把复制算法当测试。运行 `make -C hosttests test-kthread-ctor` 及新增 `make -C hosttests test-sched-notify`，记录新增断言RED。
- [ ] 建立facade和两arch强后端。共同构造器用arch默认mask；aarch64 prepare在T3前仍可拒绝AP，此任务host隔离证明common能力。P1 ARM notify hook仅返回，含明确轮询契约；common负责atomic置need_resched。删除ARCH_HAS_REMOTE_RESCHED及全部消费者，x86硬件发送留在x86后端。
- [ ] common schedule按arch_sched_scan_blockers决定是否scan；ARM scanner在task_list_lock内先按cpu跳过AP；blocker_wait对!arch_sched_scan_blockers返回-EOPNOTSUPP（x86所有CPU true）。在现有selftest加AP策略测试前先以host mock区分策略true/false，验证false不读callback、不调用schedule。
- [ ] P1就完成atomic审计：need_resched读acquire、写release、调度清零relaxed且位于rq_lock保护的pick决策内，删除解锁后的两处清零。涉及core、fair、balance、tick、ARM idle；x86也使用兼容atomic访问，不变偏移/硬件流程。scheduler_ok所有写者（BSP boot、x86 AP、ARM handoff）release，构造器/placement/balance/schedule读acquire；初始化时并发尚未开始的清零保留初始化语义。t->cpu运行期重复赋同值也转换为atomic或删除，所有锁外读取配对，特别是ARM scanner；禁止写plain而读atomic。
- [ ] 为新增源码接kernel构建/host mock，运行以上host tests和 `make PROFILE=aarch64-clang test-static CASE=sched-common-compile`、两个普通kernel build。通过后提交 `refactor(sched): define AP cooperative scheduling contracts`。

### Task 2: 在启用AP前使观测支持并发

**Files:** 文件表T2；新增hosttests/cases/test_aarch64_sched_observer.c及mock/aarch64_sched_observer_runtime.h，注册sched.mk；kernel/Makefile。

**Interfaces:** 保留既有非资源aarch64_sched_observe_*函数；新observer头声明 `void aarch64_sched_observer_begin(unsigned cpu, unsigned generation)`、`int aarch64_sched_observer_end(unsigned cpu, unsigned generation)`、`uint64_t aarch64_sched_observe_free_begin(const void *ptr, unsigned kind)`、`void aarch64_sched_observe_free_complete(uint64_t token)`。end仅在目标已退出并收到真实释放记录后调用，返回0代表本代零live/零重复/零溢出。C/汇编共用每CPU `struct aarch64_sched_entry_record { uint64_t sp, base, ok; }`，数组stride=24、offset=0/8/16用_Static_assert钉死。

分配入口明确保留 `void aarch64_sched_observe_resource(const void *ptr, unsigned kind, bool alloc)`，仅允许alloc=true登记新实例；所有旧false调用都替换成free_begin/实际kfree/free_complete。observer收到false必须记测试失败，防止遗留调用伪造完成。

- [ ] 对真实observer模块写pthread host测试：两个CPU并发alloc/free，重复释放失败，复用同一地址的新generation不混淆；512条活跃记录容量耗尽必须报错，end在live!=0或FREEING!=0时失败；free_begin后、free_complete前同地址再alloc，旧token不能释放新记录。运行新增 `make -C hosttests test-aarch64-sched-observer`，确认RED。
- [ ] 从旧selftest抽出资源登记到新observer模块，IRQ-safe专用锁保护，锁内不分配/打印/取rq锁。全局仅一个活动批次，begin的cpu是待测目标标签，不按hook执行CPU归属；全部参与测试的构造/释放在批次结束前静止，其余未跟踪工作不创建动态任务。每次alloc登记全局单调非零instance token、ptr/kind/generation，state=ALLOCATED；同地址旧FREEING与新ALLOCATED可共存。free_begin锁内将最新匹配ALLOCATED转FREEING并返回token（未跟踪返回0）；free_complete(token)在实际kfree之后转FREED并增加已释放计数，只用token不读对象。固定至少512条活跃记录槽，可重用FREED槽，但token不复用；满表、未知非零token或重复完成都失败。end停用前确保零ALLOCATED/FREEING，late token不得触碰新代记录。
- [ ] 修改kthread.c所有rollback和switch-tail的释放点：先捕获ptr/token及必要元数据，kfree(ptr)，再free_complete(token)。observe_reap仍在prev有效且尚未task_finish_switch之前读取prev，不把它计作free完成；之后不读prev。用host kfree mock阻塞释放，证明end在实际释放前失败，返回后才成功。
- [ ] 迁移旧case_reclaim的1000任务至同一observer：分4批250任务，代号独立，累计created/stack_free/context_free均1000，保留原parser created>=1000及异栈/零泄漏断言；不得以新256任务用例替代旧1000验收。删除旧1000容量表和allocation_active等双重真值源。
- [ ] 入口汇编以TPIDR_EL1读取PERCPU_CPU_ID_OFFSET定位24-byte槽，不能通过current取CPU；原C消费者同步按cpu读取。x19/x20/x21首次入口参数保存不受新增观测破坏。
- [ ] switch/nested/IRQ active与checksum观测改每CPU；旧BSP病例只聚合CPU0目标，不把AP idle或AP switch计入其失败。旧fault目标、switch/entry计数仍只认选定victim。
- [ ] 跑host test、`python3 -m unittest qemutests.test_aarch64_harness`、`make PROFILE=aarch64-clang test-aarch64 MODE=sched`，该步AP仍未启用。通过后提交 `test(aarch64): make scheduler observations safe across CPUs`。

### Task 3: AP单向task-stack handoff与生产轮询idle

**Files:** 文件表T3；更新旧selftest case_ap及qemutests/aarch64_scheduler.py/test_aarch64_harness.py（随行为变更同步）；kernel/Makefile；新增qemutests/test_aarch64_ap_boot_contract.py（源级和模拟工具输出unittest，注册test-harness），新增qemutests/aarch64_ap_scheduler.py的fatal分支与ELF audit；提前修改mk/{project.mk,profiles/aarch64-clang.mk,targets/aarch64.mk,components/run.mk}接fatal-owner最小路径。

**Interfaces:** 按spec §3.2精确产出prepare/release/released/enter/idle_handoff/stack_enter与smp_accepted_mask；已有arch_aarch64_prepare_bsp_handoff/bsp_handoff调用顺序扩展，不改变原签名。`aarch64_ap_sched_released(cpu)`同时检查冻结admission bit。新增内部static gate、idle unions/contexts与fatal-owner。

- [ ] 先写新行为证据fixture：每个accepted AP必须有一次idle-ready，not-ACK AP不可有。unittest仅用源码与模拟nm/readelf输出，不要求预构建ELF；真实ELF审计由runner在prepare后必跑，缺ELF失败，验证AP unions各32KiB对齐、各ctx唯一、静态对象在内核保留范围。旧AP-case fixture升级为default_cpu0=1、pinned_ap_ok=1、每录用AP scheduler_ok=1、静止时nr_running=0、work_ok=1；保留非AP八项证据断言。现树应因无AP ready失败。
- [ ] smp_accepted_mask返回BSP完成smp_boot_run后冻结的online_mask；处理UP和PSCI unavailable分支，不能返回未初始化static result。AP只经acquire release-gate获得mask，不在BSP写result时读取；无ACK、CPU_ON失败、迟到AP永久留旧work循环。
- [ ] prepare在原启动探针结束、BSP切栈前构造永久AP idle/task_union与thread_t，thread->x30指向idle_resume，sp=top-16、usable_low=align16(base+sizeof(task_t)+256)。task/list/rb独立初始化，持task_list_lock加入一次；不分配PID、不复制BSP指针、不重置runqueue。prepare不能使用current。
- [ ] 新AP trampoline保存cpu参数、逐项验证spec栈边界后安装SP并tail-branch到handoff；保存旧AP work栈无需回收。handoff核对current与static idle，rq->idle和metadata完整后enable IRQ+ISB、release scheduler_ok。不重复percpu_init、不改TPIDR/TTBR/VBAR。
- [ ] secondary_idle现有work循环每轮处理slot后检查gate，gate开放才进入新栈。BSP在自身sched_boot_start后release gate，再等待accepted AP ready，counter2秒期限，IRQ开、不持锁；缺失fail-stop。正常、旧自测、no-ack均走同一生产逻辑，不用测试宏启用AP。
- [ ] arch_kthread_prepare允许已验证目标，保留单bit affinity。ARM idle每次在IRQ开时处理至多一个ap_work，再IRQ-save+rq_lock检查队列/need，释放锁后schedule，恢复IRQ；无工作pause，BSP也改轮询。AP blocker_wait拒绝测试在AP任务栈执行，确认-EOPNOTSUPP且不改状态。
- [ ] fatal ASM安装共享emergency SP之前用ldaxr/stxr无栈争抢单owner，loser直接IRQ屏蔽park。此任务就接独立 `AARCH64_AP_SCHED_TEST=fatal-owner` fault变体及 `MODE=sched-ap AP_SCHED_CASE=fatal-owner` 最小runner，经CPU0/1屏障同时进入非法SP路径。测试宏下无栈仲裁前atomic OR arrivals_mask、失败者atomic增加losers并park；owner在安全emergency栈等arrivals_mask=3及losers=1后单写 `[ap-fatal] owner=<0|1> arrivals_mask=3 entrants=1 losers=1`，期限2秒，禁止成功END。runner收到该完整证据才停止QEMU；缺失/重复/值错均失败。普通ELF无注入符号。
- [ ] 更新旧AP-case为默认CPU0成功、显式ready AP成功、work探针仍通过；旧global cpu0观测仅观察CPU0目标。以硬件计数期限等任务完成与回收；不得通过保存自回收task裸指针等待。
- [ ] 清两个profile构建。普通ARM idle smoke覆盖1/2/4/8（记录实际QEMU命令和路径）；运行旧MODE=sched、MODE=no-ack、MODE=m3-probe，以及 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap AP_SCHED_CASE=fatal-owner`（真实ELF审计+2核故障boot）。本任务完整可验证后提交 `feat(aarch64): enter cooperative scheduling on accepted APs`。

### Task 4: AP生产路径测试主体

**Files:** 文件表T4；kernel/arch/aarch64/cpu/task_arch.c测试driver调用、boot/main.c late selftest ID声明；kernel/Makefile；提前接T5所列profile/targets/run.mk的positive variant和新runner positive parser/执行路径，新增test_aarch64_ap_scheduler.py。

**Interfaces:** 新头产出 `void test_aarch64_ap_scheduler_before_release(void)` 和 `void test_aarch64_ap_scheduler_run(void)`；仅AARCH64_AP_SCHED_TEST构建调用，普通旧scheduler自测调用保持。前者在BSP task栈且自身ready之后、release AP gate之前验证未ready AP创建失败并提交pre-handoff work；后者在全部AP ready后运行，结束前确认所有测试对象已回收。测试证据固定如下。

| case | CPU集合 | 必须满足的字段 |
|---|---|---|
| ready | 所有录用CPU | scheduler_ok=1 idle_on_cpu=1 idle_on_rq=0 unique_stack=1 |
| entry | 所有录用CPU | entries=2 wrong_cpu=0 bad_mask=0 bad_sp=0 bad_regs=0 |
| yield | 所有录用CPU | tasks=2 yields=64 bad_canary=0 irq_switches=0 |
| reap | 所有录用CPU | tasks=256 stack_alloc=256 stack_free=256 ctx_alloc=256 ctx_free=256 bad_stack=0 duplicate=0 overflow=0 idle_freed=0 |
| work | 所有录用CPU | pre_handoff=1 post_handoff=1 seq_ok=1（CPU0本地消费验证，非AP握手） |
| admission | cpu=0 | invalid_rejected=1 zero_publish=1；UP: unready_rejected=0 unready_na=1，2/4/8核: unready_rejected=1 unready_na=0；全部四字段必须存在 |
| default-mask | cpu=0 | default_cpu0=1；UP: ap_to_bsp=0 bsp_to_ap=0 ap_to_ap=0 na=1，2/4/8核: ap_to_bsp=1 bsp_to_ap=1 ap_to_ap=1 na=0；全部五字段必须存在。2核ap_to_ap为本AP本地创建，4/8核必须跨不同AP |

- [ ] 写对应fixture和反例（缺CPU、重复CPU、只完成函数未释放stack、mask错误、资源超额等）供T5 parser测试，先确认真实现有镜像不满足新证据。
- [ ] 每CPU两个固定目标线程调用现有reg_probe并各32次yield；若reg_probe自带16次yield，额外16次并把观测总数固定32，不把probe内部yield漏计。独立BSS保存线程结果，BSP单写完整协议行。
- [ ] 每CPU一批256短命任务，先开启代次观测、构造任务，再等真实异栈reap和free记录；函数返回的done不等于回收完成。所有API失败当场失败并退出等待，不继续等不存在任务。显式AP任务及AP→BSP/AP创建方都保持目标单bit。
- [ ] before_release在真实WORK_BARRIER提交/完成后记录pre-handoff；run在AP idle消费后记录post-handoff；CPU0本地slot同步消费验证。两者均在既有M3/启动探针结束之后，且新variant不再运行旧suite的late probe。测试是剩余唯一BSP生产者：确认slot IDLE后读取保留的slot.seq，使用seq+1（溢出失败），wait完成后再递增；不重置slot或旧模块seq。无生产协议API扩展。
- [ ] admission的运行期无效CPU用NR_CPUS；before_release在IPI就绪但scheduler_ok=0的真实AP上调用constructor，捕获-EINVAL且task-list/rq零变化后release。UP的unready_rejected=0、unready_na=1，其他矩阵unready_rejected=1、unready_na=0；parser必须按CPU数判定。no-ack另证明确实失败AP也不被录用；不得清零活跃CPUready，也不引用host结果冒充本boot证据。
- [ ] 此任务就完成positive最小selector/路径/宏/producer/parser/runner接线，正向START要求accepted_mask=(1<<cpus)-1，缺失AP不得降级PASS。在新variant启动协议START前注册准确late IDs/数量，结束与旧selftest_end_run只有一个拥有者；失败也输出带原因的FAIL，不借全局框架END掩盖未完成case。运行 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap` 的1/2/4/8核各2次矩阵，提交 `test(aarch64): cover AP cooperative task lifecycle`。

### Task 5: 独立变体、严格runner与最终交付

**Files:** 文件表T5，qemutests/test_{scenario_catalog,suite_catalog,inventory}.py；新增前述boot contract模块；docs/{roadmap.md,arch.md,build/build.md,changelog.md}；新reports/2026-10-31-aarch64-ap-sched-p1-validation.md。

**Interfaces:** T3/T4已接 `MODE=sched-ap`，`AP_SCHED_CASE=positive|fatal-owner`默认positive；选择器 `AARCH64_AP_SCHED_TEST=ap|fatal-owner`（fatal-owner仅故障用）；positive输出kernel/sched-ap和image/sched-ap，fault输出sched-ap-fatal-owner。runner参数 `--phase ap --image PATH --firmware PATH --elf PATH --cpus 1 2 4 8 --repeat 2 --timeout 120 --log-dir PATH`；`--case fatal-owner`固定2核独立boot，拒绝正向END且owner行恰一次。正向包络为 `[ap-sched] START phase=ap cpus=N accepted_mask=M`、`[ap-sched] END phase=ap failed=0`，二者必须全行匹配，夹住全部case行；N/M为十进制整数且M=(1<<N)-1。T4证据不使用host结果冒充runtime结果。

- [ ] 扩展T3/T4已有Python unittest：固定case与字段表严格解析、START/END各一次、错误phase/CPU/数字/未知字段/重复/少行/FAIL/timeout拒绝；fatal-owner测试缺参与CPU、双owner、成功END均失败。runner用mock QemuSession证明超时/异常也停止并join进程、保存报告；禁止自写裸Popen替代现有harness。
- [ ] 审计T3/T4构建接线：profile selector验证、KERNEL_VARIANT、project.mk受控submake白名单、kernel源/宏、targets显式镜像/firmware/ELF路径、run.mk prepare/run/mode白名单/help；MODE=sched-ap外层拒绝全部冲突，不能只依赖子make。此任务补齐负向组合测试和catalog，不推迟前两任务可运行的必要构建路径。不复用旧sched变体路径，不隐式覆盖普通镜像。
- [ ] 主新suite仅ap正向8boots；fatal-owner独立1boot（2核）。catalog新增aarch64-sched-ap及aarch64-sched-ap-fatal，variant/cpus/timeout与真实命令一致；test-harness显式注册模块，不靠扫描。inventory数量从真实注册表推导，不手填旧153。
- [ ] `python3 qemutests/aarch64_ap_scheduler.py --phase ap --self-test`、`python3 -m unittest qemutests.test_aarch64_ap_scheduler qemutests.test_aarch64_ap_boot_contract qemutests.test_aarch64_harness qemutests.test_scenario_catalog qemutests.test_suite_catalog qemutests.test_inventory`，预期全PASS。
- [ ] 对两profile clean，然后执行下表；每项保存命令/退出码/log，不把普通boot、host、符号审计合并为一个无证据PASS。正向新suite必须8boots；fatal-owner仅1boot且普通ELF无fault注入符号。

| Gate | 命令 |
|---|---|
| x86 host/static | `make PROFILE=x86_64-clang test-host`；`make PROFILE=x86_64-clang test-static` |
| x86 boot/syscall | `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`；`make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` |
| x86 kernel | 分别 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` 与 SMP=8；不得与systest同次启用 |
| ARM compile/audit | `make PROFILE=aarch64-clang test-static CASE=sched-common-compile`；`make PROFILE=aarch64-clang test-aarch64-audit` |
| ARM旧suite | 分别 `make PROFILE=aarch64-clang test-aarch64 MODE=<sched/smp/no-ack/m3-probe/gic-spi/sync-fault>`，尖括号是逐项执行的简写 |
| ARM新suite | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap`；新增 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap AP_SCHED_CASE=fatal-owner` |
| harness | `make test-harness` |

- [ ] 更新文档标记P1“轮询协作式AP调度完成”，P2、抢占、M4仍待办；报告普通1/2/4/8 idle smoke与两profile普通/自测ELF构建结果，记录P1真实commit供P2核对。提交 `build(aarch64): register AP scheduler acceptance suite`。
- [ ] 对最终diff自审并由独立reviewer明确APPROVE；任何修订重跑受影响检查。停止，不开始P2，除非后续明确授权。

## 作者自审与执行状态

覆盖：spec §3→T3；§4/§6 atomic与scan→T1；observer→T2；生命周期→T4；§7变体/证据/回归→T5。P2 blocker锁、SGI、WFI不在本plan实施范围。

本轮只有文档。上述命令均为未来实施步骤，未声称已执行。P1独立评审已明确 **APPROVE**，记录见对应评审报告；批准不自动授权实施。
