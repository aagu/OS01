# aarch64 Per-CPU Tick and Accounting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给已录用的 aarch64 CPU 接入本地 100 Hz tick、CPU/task 采样记账与 tick 驱动的 vruntime，同时保持 EL1 协作调度。

**Architecture:** BSP 独占全局时间，AP 只做本核采样。新增 arch 后端管理本地启动和 gating，复用公共 accounting/fair；aarch64 schedule 不再按调用次数计费。ISR 与 IRQ 返回都不切换线程。

**Tech Stack:** freestanding C、AArch64 assembly、CNTP/GICv2、QEMU virt/UEFI、现有 make profile、C hosttests、Python unittest/协议 runner。

**Spec:** [已批准的 R3 设计](../specs/2026-11-06-aarch64-percpu-tick-accounting-design.md)。基线 `ca0e43ad`。本文版本：R2。独立 reviewer `plan_review` 已于本日明确 **APPROVE（R2）**；[评审记录](../reports/2026-11-06-aarch64-percpu-tick-accounting-doc-review.md)。

## Global Constraints

- 本期仍是 EL1 协作式调度：timer/SGI handler 和 IRQ 返回路径均不得调用 schedule。
- BSP 继续独占 jiffies、poll timeout、timer-list/TIMER_SIRQ 和 blocker 扫描。
- 不改变 UAPI、boot_context、task_t、thread_t、percpu_t 布局；继续 `-mgeneral-regs-only`。x86 调度与时间行为保持现状。
- 每次真实递送的 CNTP IRQ计一个样本；100 Hz 为名义频率，不按墙钟补算丢失 tick。
- 每个有效 BSP 本地 tick 均 release-store need_resched=1，包括 idle；AP idle tick 不无条件请求 schedule。
- rq_lock 和 usage lock 不嵌套；tick 不获取 task_list_lock。
- 本次只产出和评审文档；本计划以下 checkbox 全为未来实施步骤，plan approve 后停止。

## Review Focus

1. boot/work stack、init 未完成或 scheduler_ok=0 时，不得读取 current 或碰未初始化锁（Task 1/2 poison-current host tests）。
2. AP IRQ 不能推进全局时间，BSP 不能双记账；忙/闲边界不能错计 task（Task 2精确增量；Task 4每核证据）。
3. 零 tick yield 和 IRQ-masked switch 不能依赖旧 schedule 计费；仍须验证四种 IRQ 状态路径（Task 2 fixture 与 Task 4 QEMU）。
4. AP 周期 tick 不得救活漏 SGI 的 WFI 测试，旧 observer 不能跨核清零活跃槽（Task 2 target-owned mask/reset；Task 4带timer的SGI测试）。
5. 错误 selector/ELF、缺 AP record、未终结 run 与不断输出的 hung QEMU 必须失败（Task 3 parser/timeout负例；Task 4 build contract/CI）。

## 文件与责任

| 单元 | 文件 | 责任 |
|---|---|---|
| Timer 后端 | 修改 `kernel/arch/aarch64/platform/time.c`；新增 `kernel/include/arch/aarch64/timer.h` | 单次注册、只读配置发表、本核 arm、BSP/AP ISR 分工 |
| 调度 tick | 新增 `kernel/arch/aarch64/cpu/sched_tick.c`、`kernel/include/arch/aarch64/sched_tick.h` | init/start/tick gate、accounting、fair、watchdog |
| 集成 | `kernel/time/tick.c`、`kernel/include/time/clockevent.h`、`kernel/sched/core.c`、`kernel/sched/accounting.c`、`kernel/include/sched/accounting.h`、`kernel/Makefile` | aarch64 link accounting、限定双架构计费分支、修正契约注释 |
| Handoff | `kernel/arch/aarch64/cpu/task_arch.c`、`cpu/ap_sched.c` | 初始化/启动 tick，失败不得发布 scheduler_ok |
| 旧测试与注释 | `kernel/selftest/test_aarch64_scheduler.c`、`test_aarch64_ap_wake.c`、`kernel/arch/aarch64/cpu/idle.c`、`cpu/sched_notify.c`、`smp/smp.c` | fixture/observer/屏蔽握手；清理AP timer永久关闭的过时注释 |
| Host | 新增 `hosttests/cases/test_aarch64_sched_tick.c`、`hosttests/mock/aarch64_sched_tick_runtime.h`；修改 `hosttests/mk/sched.mk` | 真实生产对象的精确计费与寄存器行为测试 |
| 产品自测 | 新增 `kernel/selftest/test_aarch64_sched_tick.c`、`kernel/include/selftest/aarch64_sched_tick.h`；修改 `kernel/arch/aarch64/cpu/probe.S`、`kernel/selftest/selftest.c`、`kernel/arch/aarch64/boot/main.c` | 分核证据、实际寄存器 probe、7项late协议 |
| Runner | 新增 `qemutests/aarch64_sched_tick.py`、`qemutests/test_aarch64_sched_tick.py` | 严格解析、ELF audit、矩阵及失败收敛 |
| 构建/CI | `mk/profiles/aarch64-clang.mk`、`mk/project.mk`、`mk/targets/aarch64.mk`、`mk/components/run.mk`、`mk/tests/catalog.mk`、`config/test-scenarios.json`、`.github/workflows/ci.yml`、`qemutests/test_ci_workflow.py`、`test_kernel_selftest_result.py` | selector/路径/注册/真实CI调用/协议回归 |
| 收尾 | `docs/build/build.md`、`docs/arch.md`、`docs/roadmap.md`；新增 `docs/superpowers/reports/2026-11-06-aarch64-percpu-tick-accounting-validation.md` | 实施证据与范围说明 |

`arch/aarch64/timer.h` 对应已有 platform/time.c；其余新增 kernel 源/公开头严格成对。实施前重新核对工作树中的 AGENTS.md、基线和签名；如基线变化，以真实实现复核而不是覆盖用户改动。

## Task 1: 冻结 CNTP 配置与本核启动后端

**Files:** time.c、timer.h、hosttests 新文件、hosttests/mk/sched.mk（见上表）。

**Interfaces:** 消费现有 `bool arch_tick_start(void)`、GIC registration/configuration 和 CNTP helpers；产出 `bool aarch64_timer_start_local(void)`。BSP boot start保留全局handler所有权；AP接口不注册handler。这一任务尚不在 AP handoff 调用它，不启用 AP timer。

- [ ] **Step 1 — 写 RED tests。** 在 test_aarch64_sched_tick.c 添加 `timer_register_once`、`timer_bad_config`、`timer_local_arm`、`timer_failed_arm_disables`，用真实 time.c 和 mock register trace。断言注册只发生在BSP；-2冲突失败；PPI=15/32失败、16/31可用；freq=0/99失败，freq=100和QEMU 62500000成功；freq导致period>INT32_MAX失败；AP freq不一致失败。未发表配置时 AP 返回false且不arm。失败后CTL ENABLE清除；有效启动顺序 TVAL→CTL→ISB→readback，ENABLE=1且IMASK=0。
- [ ] **Step 2 — 确认 RED。** 新增 root `make PROFILE=x86_64-clang test-aarch64-sched-tick-host` 转发到 hosttests 的同名目标（采用 os01_submake/whitelist 模式），register_host_test 归 sched，并加入 TEST_BINS。运行目标，预期缺接口或上述行为断言失败；不能以编译参数错误代替有效RED。
- [ ] **Step 3 — 实现 timer 后端。** time.c 保持既有 ISR暂时只在BSP递送；BSP注册后release发表 config_ready、period、freq、PPI（字段先写、ready最后）。AP acquire校验并只arm本核。CPU频率读取/CTL读写/ISB使用可替换机器边界，不在mock复制算法。BSP boot start遇异常关闭timer返回false；不接受未知重复注册。运行日志改log_*；普通产品与selftest均精确保留原有BSP `[cntp]`/每秒`[tick]`协议，经log_info（支持%lu、无额外前缀）输出，AP不加周期输出；原aarch64_uefi_smp.py的存活断言保持不变。
- [ ] **Step 4 — GREEN 和兼容性。** 运行上述host目标、`make PROFILE=aarch64-clang aarch64-uefi-kernel`、`make PROFILE=aarch64-clang test-aarch64-audit`；产品原BSP tick仍可工作，AP未启用。host对象使用私有名避免覆盖已有monitor accounting对象；mock preinclude只替换寄存器、CPU/IRQ/GIC边界。
- [ ] **Step 5 — Commit。** 仅暂存本任务文件，提交 `feat(aarch64): prepare shared CNTP configuration and local timer start`。

## Task 2: 接入每核调度记账并修复旧证据 fixture

**Files:** sched_tick源/头、集成和handoff行、旧测试与注释行、hosttests新文件；需要时修改 `hosttests/mock/blocker_runtime.h` 与 `hosttests/cases/test_blocker_remote_wake.c` 以链接真实 core/fair 分支。

**Interfaces:** 消费Task1本地arm；产出 spec 的 `aarch64_sched_tick_init/start/tick`、`AARCH64_SCHED_TICK_HZ=100U`。init只在BSP boot-stack一次执行；start只在本核合法idle stack/IRQ masked调用；tick只从CNTP ISR调用。

- [ ] **Step 1 — RED 精确计费/顺序。** test_aarch64_sched_tick.c 编译真实 sched_tick.c、time.c、fair.c、accounting.c；必要的其它fair符号以function-sections/gc-sections消除，不能stub update_curr/sched_account_tick。增加 `early_tick_never_reads_current`（current访问器poison），`start_requires_admission_idle_and_init`，`busy7_idle5`（busy/runtime/vruntime各+7，idle+5、idle task runtime/vruntime=0），`deadline_and_pending`（deadline前不置位、到达置位、不清旧pending），`bsp_idle_requests_scan_ap_idle_does_not`，`lifecycle_idempotent`（重复start无寄存器写/清零/epoch变化），`global_tick_cpu0_only`（BSP1/AP0、watchdog每有效tick+1）。init前start直接失败且无accounting锁操作。
- [ ] **Step 2 — RED schedule收费回归。** 复用现有真实 core.c host测试缝隙，在同CPU唯一runnable worker上执行 schedule 自选路径；零tick的16次schedule不得增加aarch64 vruntime/runtime。双编译分支：host aarch64视图preinclude屏蔽机器头，`-D__aarch64__`，保留host系统ABI宏；产品分支用 `#if defined(__aarch64__) ... #else ...`，不 `-U__x86_64__` 破坏宿主libc。独立x86视图断言原schedule收费仍发生。运行 `make PROFILE=x86_64-clang test-aarch64-sched-tick-host`，记录行为RED。
- [ ] **Step 3 — 最小集成。** sched_tick静态ready/started数组；gate次序init→cpu/admission→started→scheduler_ok→idle→current。start校验本核idle、BSP验证已运行CNTP，AP调用Task1接口；accounting_cpu_start在init之后。BSP在prepare handoff中init、publish_idle之后/start之前激活；AP idle metadata安装后、IRQ enable和scheduler_ok之前激活，失败log_err/fail-stop。ISR rearm first→CPU0 global tick/协议→local sched tick，EOI保持GIC所有。usage锁释放后取rq_lock update_curr/watchdog，BSP置need_resched。tick.c的aarch64全局路径不再重复本地记账/置位/watchdog；core.c仅aarch64移除schedule的update_curr。kernel/Makefile链接sched/accounting.c并加入SCHED_COMMON_COMPILE_TUS。
- [ ] **Step 4 — 旧observer改为owner操作。** aarch64_sched_timer_tick_count只由BSP计数并以atomic store/load发表（probe.S已有ldar）；case_timer只在CPU0 IRQ masked时清CPU0的frame/active/irq-switch槽，不再循环清AP活跃槽。AP槽由AP自己初始化/观察，远端读用atomic或目标核IRQ masked快照发布。snapshot/reset请求不能从BSP memset活跃AP数组。
- [ ] **Step 5 — 确定性IRQ四态fixture。** 修改irq_worker/irq_peer：保留存活peer指针直到peer退出的握手，worker在IRQ masked+rqlock下对queued peer先dequeue，设置peer.vruntime=F，再enqueue；设置running worker.vruntime=F+2*EEVDF_MIN_SLICE，释放锁后yield使peer必被选中。peer用对称的锁内操作安排worker低于自己，再yield返回worker；F取当次rq min_vruntime，禁止原地改rbtree key。分别在worker IRQ on/off时验证真实switch与恢复；peer终止并完成reap后，唯一worker自选验证on/off no-switch；退出握手后不再解引用peer指针，reap仅通过独立资源observer/barrier确认。四bit仍必须15，case时限保留，禁止只增加yield次数碰运气。该测试人为布置vruntime仅用于fixture，不能成为生产收费路径。
- [ ] **Step 6 — 防止timer救场。** 在 test_aarch64_ap_wake.c 的 target idle hook加入OS01_SELFTEST专用本核 `uint64_t aarch64_sched_tick_test_mask_local(void)` / `void aarch64_sched_tick_test_restore_local(uint64_t saved_ctl)`（实现在time.c，声明timer.h selftest guard）。保存CTL、设IMASK、ISB并读回后，目标才release ACK。关键窗口请求与ACK走已有gate的release/acquire；target一直保持IMASK直到本轮marker/work完成，由target在idle hook恢复CTL并rearm、ISB，然后发restore ACK，BSP才开始下一轮。覆盖idle_case三时机及work_case两时机；不屏蔽BSP的时间源。失败路径由目标restore握手清理，无法清理则明确FAIL/fail-stop，不继续伪通过。新增mask/restore计数证据并调整原runner严格验证每个窗口确实受保护；其它P2 case和专用Task4测试保持AP timer开启。
- [ ] **Step 7 — GREEN 与审计。** 运行host目标、`make PROFILE=x86_64-clang test-monitor-host CASE=accounting`、aarch64 kernel/common compile/audit。回归 `MODE=sched`、`MODE=sched-ap`、`MODE=sched-wake`，保留原断言；特别case_idle<=10jiffies、IRQ四态、yield10000与reap1000。生产GIC MMIO spy新增PPI完整EOI1次/spurious0次；做重复EOI/重复计费/AP全局tick mutation，逐个确认失败后恢复源。更新AP timer关闭的旧注释，secondary_idle保留实际禁用代码。
- [ ] **Step 8 — Commit。** 提交 `feat(aarch64): account local scheduler ticks on admitted CPUs`，包含不可分割的启动/计费/旧测试fixture修改。

## Task 3: 严格证据 runner 与负例

**Files:** 新增 qemutests/aarch64_sched_tick.py、test_aarch64_sched_tick.py；修改 mk/components/run.mk 的append-only TEST_HARNESS_MODULES（本任务尚不接MODE）。

**Interfaces:** 产出CLI `--image --firmware --elf --qemu --llvm-nm --build-dir --profile --log-dir [--self-test]`。默认矩阵SMP=1/2/4/8，各repeat=1/2、512MiB、单boot固定120s，复用现有harness/process、result、DTB注入与QEMU隔离。Task4按下述协议生产证据。

- [ ] **Step 1 — RED parser测试。** 使用现有kernel-selftest v1 parser验证早期case与7个late必选ID；新增 `[sched-tick] START cpus=N accepted_mask=M` / `[sched-tick] case=ID cpu=C ...` / `[sched-tick] END failed=0`。七个ID固定为 `tick-ready`、`tick-idle`、`tick-busy`、`tick-yield`、`tick-deadline`、`tick-global`、`tick-remote`；每ID perCPU 0..N-1恰好一条（remote在SMP1 cpu0 na=1），整批证据在对应selftest_record_late前发表，END只出现一次且在selftest END后。
- [ ] **Step 2 — 冻结字段和断言。** ready=`started=1 valid=1 hz=100`；idle=`samples>=3 idle=samples busy=0 runtime=0 vruntime=0`；busy=`samples>=7 busy=samples idle=0 runtime=samples vruntime=samples`；yield=`calls=16 samples=0 runtime=0 vruntime=0 self_switches=0`（self_switches意为错误调用arch_switch的自切换数）；deadline=`need=1 irq_switches=0 nested=0 regs_ok=1`；global=`local_samples=busy+idle global_ticks`且AP global_ticks=0，CPU0额外`jiffies_delta=global_ticks`；remote=`sgi_ok=1 tlb_ok=1 wfi_enter>=1 timer_live=1 na=0`，SMP1 remote使用全零值及na=1。除明确NA，缺字段/负值/未知字段/重复CPU/越界均拒绝，accepted_mask必须等于(1<<N)-1，不使用online数量冒充录用。
- [ ] **Step 3 — 失败行为测试。** 覆盖删除AP、CPU重复/混轮、AP global_ticks>0、busy与runtime不等、irq_switches>0、缺任一END/未知ID/重复case、错误variant/SMP、FATAL/panic、超时、QEMU非预期退出。fake QEMU持续输出tick也不能延长120s总期限；用mock clock缩短单测，不实等120s。产物须核对kernel ELF machine=AArch64、专用selftest符号、同目录variant和profile；复用已有runner的镜像/ELF来源核验，不仅看文件名。
- [ ] **Step 4 — GREEN。** 实现解析/矩阵/日志（每SMP/repeat独立文件夹，包含stdout/命令/JSON verdict），`python3 -m unittest qemutests.test_aarch64_sched_tick -v`、`python3 qemutests/aarch64_sched_tick.py --self-test`通过。复用parse_v1，避免再造一套宽松的PASS解析。
- [ ] **Step 5 — Commit。** 提交 `test(aarch64): validate per-CPU tick evidence and runner failures`。

## Task 4: 实际产品自测、selector与CI接线

**Files:** 产品自测行、handoff、timer/sched_tick selftest observer、runner（如确需同步字段）、构建/CI行（见文件表）。

**Interfaces:** 新增 `void test_aarch64_sched_tick_run(void)`；`AARCH64_SCHED_TICK_TEST=1`→`-DAARCH64_SCHED_TICK_TEST=1`→variant `sched-tick`；MODE=sched-tick自己设KERNEL_SELFTEST，外层冲突必须parse-time拒绝，直接selector也须独立验证。产出Task3七ID的产品证据。

- [ ] **Step 1 — RED build/protocol合约。** test_aarch64_sched_tick.py增加make -n的成功路径/冲突矩阵：新selector与旧sched/AP/M1/no-ack/sync-fault/canary/weak/OS01_SYSTEST对称互斥，非法值与无KERNEL_SELFTEST拒绝；x86 profile拒绝新aarch64 selector。kernel selftest late注册/expected_late=7与handoff执行分支必须一致；test_kernel_selftest_result.py维持其它variants原manifest。CI测试先要求真实 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick` step，不能只有catalog占位。
- [ ] **Step 2 — selector/path接线。** project.mk传递白名单；aarch64 profile选择隔离kernel/image/sched-tick目录；targets新增 `AARCH64_UEFI_SCHED_TICK_DISK/FIRMWARE` 与 `AARCH64_KERNEL_SCHED_TICK_ELF`；run.mk新增prep/run helper、MODE白名单/help/互斥，root `test-aarch64-sched-tick-host`已由Task1提供。kernel Makefile添加测试source（非选中variant为空TU/不含driver），selftest.c只为tick variant注册七ID，boot/main.c调用selftest_begin_run(7)，task_arch.c在tick variant调用新run且恰好一次selftest_end_run，普通manifest不变。
- [ ] **Step 3 — 本核计数和probe。** 仅selftest构建的 `void aarch64_sched_tick_observe(unsigned cpu, bool global)`：CPU0实际global tick完成后global=true；每次成功调度采样后global=false。使用分核atomic计数，无共享自增；snapshot由目标IRQ masked获取并release发表到持久slot，BSP acquire读取，只由BSP打印完整证据行。新增assembly `int aarch64_sched_tick_irq_probe(const uint64_t *local_count, uint64_t target, uint64_t deadline_cycles)`，借鉴旧probe的寄存器sentinel，以传入AP本核计数ldar等待真实IRQ，不等待BSP计数。保持旧probe ABI；以ELF反汇编核对本地计数地址和保存/恢复寄存器。
- [ ] **Step 4 — 每核七case。** 按Task3字段运行：idle通过本核idle stack上的有界ap_work（CPU0直接调用）采样>=3；busy用kernel_thread_on_cpu创建pinned worker，本核IRQ-masked前后快照，IRQ-on不yield等待>=7本核样本，硬件cycle deadline=2秒，完成后IRQ-masked取差并发表，不读被回收task；yield在唯一runnable worker中IRQ关闭做16次真实kthread_yield，自选不能调用arch switch；deadline让worker运行至少EEVDF_MIN_SLICE=10有效tick检查pending与无IRQ switch、寄存器probe；global先后目标核snapshot，CPU0在同一IRQ-masked窗口读jiffies和global计数。每个CPU的case及worker批次严格串行；上一批资源observer/reap barrier完成后才创建下一批，确保yield窗口只有该worker可运行。每核worker仅在结果已复制到静态slot后允许退出，资源observer批次结束后清slot，复用既有reap barrier，不持task指针跨回收。global case的local observer/accounting均取同一前后IRQ-masked窗口的增量，local observer仅成功gate记账后累加，禁止把历史累计量与本窗口增量混比。
- [ ] **Step 5 — SGI/WFI与blocker证明。** timer开启时给每个AP发SGI4并校验真实handler/EOI增量及SGI3 TLB ack，周期tick计数在测试窗口前后增长；目标在WFI observer记录进入/返回，不能以periodic timer返回当SGI ACK。SMP1 remote显式NA。另保留MODE=sched的空rq blocker<=10jiffies验收（在全量gate必跑）；不能从tick自测的伪扫描替代。所有等待使用硬件counter或现有带deadline helper，超时明确FAIL。
- [ ] **Step 6 — catalog/CI与GREEN。** mk/tests/catalog.mk新增 `aarch64-sched-tick`（1,2,4,8/512/120/isolated/shared/nightly）；config/test-scenarios.json把host、parser、产品case分别映射primary/supporting与distinct_risk。CI aarch64 full-matrix job新增实际MODE调用；TEST_HARNESS_MODULES加入新模块，相关inventory/catalog/workflow测试全部通过。运行Task3单测/runner self-test、make test-harness、aarch64 MODE=sched-tick完整8boot；所有perCPU证据符合Task3，variant混用负例被拒绝。
- [ ] **Step 7 — Commit。** 提交 `test(aarch64): exercise local tick accounting across SMP CPUs`。

## Task 5: 最终门禁与完成报告

**Files:** docs收尾行（见上表）；仅在发现真实缺陷时回到对应Task修复并重跑受影响门禁。

**Interfaces:** 消费Task1–4的已验证实现；产出带命令/退出码/路径的validation报告，不产出下一期IRQ抢占代码。

- [ ] **Step 1 — 普通产品与布局。** 分别构建 `make PROFILE=x86_64-clang kernel.bin`、`make PROFILE=aarch64-clang aarch64-uefi-kernel`，按 `print-run-paths`/profile解析实际ELF，nm核查专用test入口/observer/mask helper未泄漏进非selftest产品。运行aarch64 common-compile和audit、x86 test-static；布局不得变化。若变了先修订spec，不能顺手改ABI。
- [ ] **Step 2 — Host与x86。** `make PROFILE=x86_64-clang test-host`；`make PROFILE=x86_64-clang test-qemu SUITE=phase-0`；分别 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` 和 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`；最后单独 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`，不得与KERNEL_SELFTEST=1混用。无需无差别clean；头依赖已跟踪，若接口/配置使证据可疑再做clean验证。
- [ ] **Step 3 — aarch64全回归。** 普通产品 MODE=smp/m3-probe/gic-spi 按各runner规定prep；sync-fault/sched/sched-ap/sched-wake/sched-tick使用专用variant。no-ack用其已有要求：profile clean→AARCH64_SMP_TEST_NO_ACK_CPU=1构建并 `test-aarch64 MODE=no-ack`→clean→普通recovery SMP验证，拒绝把注入镜像复用作正常产品。MODE=sched-tick必须完整1/2/4/8各两次；kernel-selftest模式与systest隔离。
- [ ] **Step 4 — harness与mutation收敛。** `make test-harness`通过；Task2三个mutation和Task3 parser负例都有失败记录，源已恢复、恢复后对应测试通过。只在新修复或未解决风险要求时重跑，不反复全量门禁。
- [ ] **Step 5 — 独立实现review与文档。** 实施者将代码和证据交独立review；Critical/Important清零后明确APPROVE。报告记录精确频率/采样语义、每核结果、未跑项（如有不得写PASS）和局限“仍无IRQ返回抢占”。更新build/arch/roadmap，移除已完成项而不提前标记EL0/统一kernel_main完成，提交 `docs: record aarch64 per-CPU tick accounting validation`。

## 本次文档工作停止点

独立子agent先评审spec并由主agent修订至APPROVE，再独立评审本文并由主agent修订至APPROVE。把轮次和结论写入 [文档评审记录](../reports/2026-11-06-aarch64-percpu-tick-accounting-doc-review.md)，随后停止；本次不得勾选上述任务、修改产品源码、运行实施步骤或自动进入执行。
