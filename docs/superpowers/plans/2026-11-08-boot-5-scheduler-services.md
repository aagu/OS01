# BOOT-5 Scheduler and Services Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 首次用户任务发布前结束VFS bootstrap，公共用户态启动策略与架构机制/服务/自测分离，保持PID1及CPU发布契约。

**Architecture:** 先修正现有task_init里的VFS边界并为spawn加入口屏障，再完善目标资格和原子publication；之后把已验证的流程移到init/userspace及services，对x86仅保留TSS/idle机制。ARM沿用自身EL1handoff；调度boot机制不依赖VFS或服务。

**Tech Stack:** freestanding C、Clang/LLVM、现有mutex/runqueue/refcount、x86_64/AArch64、模块化hosttests make、Python unittest、QEMU/UEFI。

**Spec:** [BOOT-5 R2设计](../specs/2026-11-08-boot-5-scheduler-services-design.md)，`boot5_spec_review` 已 APPROVE。基线 `f610e665`；实施前重核对HEAD/AGENTS。本文R2，独立 reviewer `boot5_plan_review` 已 APPROVE（[评审记录](../reports/2026-11-08-boot-5-doc-review.md)）；全部checkbox为未来实施，不自动执行或提交。

## Global Constraints

- 不改UAPI/boot_context/percpu/task/thread/pt_regs布局，保留BOOT-1 admission/online/IPI、BOOT-2 IRQ/校准、BOOT-4阶段/检查点。
- x86 USERSPACE=1/VFS=1；ARM0/0继续EL1handoff，不链接init/userspace/services或x86late，不新增空成功机制hook。
- 创建策略新目录 `kernel/init/` ↔ `kernel/include/init/`，源/头成对；kernel/core仍保持薄编排。
- VFS关闭在stdio成功之后、spawn之前，无VFS/FS锁持有、无正在进行的豁免namespace操作；release结束、acquire读取，生产不可reopen。
- pid0 idle仍不进入可睡眠FS mutex；runtime idle不得做namespace操作。AP可在BSP scheduler_ok之前运行PID1，但已满足VFSruntime和AP自身资格。
- PID1先于probe/net服务；网络保留creation window内时机，thermal/late在BSP调度发布后；网络无卡/thermal失败不fatal。
- creation window先永久close再release scheduler_ok；共享sched/boot机制不增加VFS或USERSPACE条件，ARM0→2路径不变。
- spawn在task_list_lock→目标rq_lock保护下重检且完整发布；负面路径锁外drop refs，已发布任务不unpublish，已消费PID不回滚。
- stdio/spawn沿用BOOT-3故障窗口及五参数fatal字段、旧成功行；selftest九case顺序/单END保持；operational日志用log_*。
- 构建从根make入口，selector进入mk/project.mk受控白名单；KERNEL_SELFTEST关闭传空，systest与kernel-selftest分开。

## Review Focus

1. AP PID1在BSP网络准备期间做FS操作，仍绕过bootstrap锁（Task1真实gate/发布轨迹，Task4实际mutex overlap）。
2. initialplacement无资格仍fallback CPU0，或locked recheck失败留task-list残影/泄漏stack page（Task2真实spawn/清理账本）。
3. 拆分时服务抢占PID1、网络窗口关闭太早、UP netif配置时机变化（Task3真实pipeline状态及UP回归）。
4. ARM链接进VFS/服务或共享sched_boot_start依赖USERSPACE（Task3双profile符号与ARMhandoff合同）。
5. observer只有gate日志却无实际mutex、normal含fixture、命令回显冒充Ctrl-C后shell成功（Task4负向parser与Task5交互证据）。

## 文件与接口归属

| 任务 | 文件 |
|---|---|
| 1 | 修改 `kernel/fs/vfs.c` ↔ `kernel/include/fs/vfs.h`、`kernel/sched/exec.c`、现有 `kernel/arch/x86_64/cpu/sched_boot.c`；新增 `hosttests/cases/test_boot_services_vfs.c`，修改 `hosttests/cases/test_boot_{task_init,spawn_failure}.c`；hosttests/mk/{fs,sched}.mk、mk/components/run.mk |
| 2 | 修改sched/exec.c及其host边界、必要的既有sched/boot.h注释；新增 `hosttests/cases/test_boot_services_spawn.c`；hosttests/mk/sched.mk；不重写balance.c的legacy sched_pick_cpu API |
| 3 | 新增 `kernel/init/{userspace,services}.c` ↔ `kernel/include/init/{userspace,services}.h`；新增 `kernel/selftest/boot_late.c` ↔ `kernel/include/selftest/boot_late.h`；修改core/main.c、x86 cpu/sched_boot.c、include/{arch/boot.h,sched/task.h,sched/boot.h}、sched/boot.c注释、net/net.c旧caller注释、selftest/test_sched_boot_probe.c注释；kernel/Makefile、hosttests/mk/sched.mk；新增hosttests/cases/test_boot_services_{sequence,services,late,mechanisms}.c及 `hosttests/mock/boot_services_mechanisms_runtime.h`，迁移原task-init与BOOT-4 sequence测试 |
| 4 | 新增 `kernel/selftest/boot_services_fixture.c` ↔ `kernel/include/selftest/boot_services_fixture.h`、`qemutests/{boot_services,test_boot_services}.py`；接线init/userspace.c、fs/{vfs,fat,ext2}.c；修改kernel/Makefile、mk/project.mk、mk/profiles/{x86_64-clang,aarch64-clang}.mk、mk/components/{kernel,image,run}.mk、mk/tests/catalog.mk、qemutests/scenario_catalog.py及catalog/dryrun/isolation测试 |
| 5 | docs/{architecture,arch,roadmap}.md、docs/sched/scheduler.md、docs/fs/filesystem.md、docs/build/build.md、AGENTS.md；新增未来 `docs/superpowers/reports/2026-11-08-boot-5-validation.md` |

focused入口 `make test-boot-services-host CASE=vfs|spawn|sequence|services|late|mechanisms|all` 默认all。六binary独立依赖、按hosttests/mk/sched.mk与fs.mk的register_host_test模式纳入test-host；不要求改仅负责include的hosttests/Makefile。Python测试加入test-harness。未实施后续CASE不阻塞早期RED。

## Task 1: 首任务发布前退出VFS bootstrap

**Interfaces:** 保留 `void vfs_end_bootstrap(void)`；新增 `bool vfs_bootstrap_active(void)`（fs/vfs.h）。vfs_sched_ready签名与pid0豁免不变。spawn在bootstrap active时return-EAGAIN，lookup/allocator/alloc_pid均不调用。

- [ ] **Step 1 — 行为RED。** 真实VFS对象链接host mutex/current边界，验证初始化bootstrap active、pid>0时未结束不取得namespace/FS mutex、end后实际进入namespace调用会锁/解锁、idle仍不睡眠、end幂等。旧真实spawn在active时仍lookup，断言应在任何lookup/alloc/PID前拒绝，记录行为RED；BOOT-3 files_dup测试给明确runtime gate初态，防止被新入口屏障提前挡住而假绿。
- [ ] **Step 2 — 边界RED。** `make test-boot-services-host CASE=vfs`，另扩展现有test_boot_task_init对旧真实task_init的轨迹：stdio成功返回→end→spawn，失败stdio不能end/spawn，负spawn之前已end但不能net/sched/thermal。旧end位于sched_start后应明确FAIL。真实VFS测试用实际namespace函数路径触发mutex，不只测复制的ready表达式；多线程acquire可见性测试不在lock pair中间翻门。
- [ ] **Step 3 — 实现。** vfs_end_bootstrap release-store0；getter acquire-load，vfs_sched_ready使用getter；vfs_init早期置1和host测试reset也用atomic。现有x86task_init将end调用移到stdio成功返回/BOOT-3 sentinel之后、spawn之前，删除原sched_start后的调用。spawn首部guardreturn-EAGAIN；不修改其它spawn失败返回码。public声明/注释标明锁已返回且无普通任务发布的前置；产品只有此处一次end，不新增可reopen接口。
- [ ] **Step 4 — GREEN。** focused vfs、`make test-boot-failure-host CASE=spawn` / CASE=task-init、test-host，x86phase-0的1/2核与BOOT-3stdio/init/child-fds回归。审计所有vfs_sched_ready依赖（namespace/FAT/ext2/tmpfs）以及boot idle后续调用：BSP spawn在publish前已put ELF node，之后net无namespace操作，late文件引用并非idle namespace操作。确认无持锁跳转，无全局sleep锁扩张。
- [ ] **Step 5 — Commit。** `fix(boot): end VFS bootstrap before the first user task is published`。

## Task 2: 初始spawn资格检查与完整publication

**Interfaces:** spawn_user_task原 `int64_t spawn_user_task(const char *path, const char *const *argv)` 保留。exec.c新增私有 `static bool spawn_boot_target_allowed(unsigned cpu)`：拓扑内且online，scheduler_ok或caller=BSP/target0/windowopen例外；不产生可写全局资格状态。初选sched_pick_cpu_mask(all topology)，无合格目标且例外合法选0，否则return-EAGAIN。最终锁内复用同一判定；getter/eligibility helper不向别模块公开。

- [ ] **Step 1 — RED资格。** 新binary编译真实exec.c、balance.c及boot.c，外部ELF/MM/hardware以可成功边界替身；断言AP online但scheduler_ok=0不可选、只合格AP可选、UPwindowopen允许CPU0排队、closed且CPU0未ready拒绝、target越界/num_cpus=0拒绝且不碰rq。无目标必须在私有资源/PID消耗前拒绝；activegate仍先于lookup，legacy sched_pick_cpu测试不被重写。
- [ ] **Step 2 — RED重检/账本。** `make test-boot-services-host CASE=spawn`；在私有ELF/栈准备完成后的受控host边界将所选AP资格清除，让locked recheck失败；使用真实publish链，捕获list/enqueue/notify/user_init记录。要求无任何发布，files_unpin、FPU/free、mm/VMA/页表/stack-page teardown、thread/raw各恰一次，锁顺序task_list→rq、释放后再cleanup；不能将enqueue整体stubbed success作为atomicity证明。BOOT-3子fd失败必须仍发生在真实files_dup点。
- [ ] **Step 3 — 实现。** activegate后初选并验证目标，替换tsk->cpu的legacyfallback；最终先持task_list_lock再rq_lock，重检VFSruntime/目标资格，再list+enqueue，无fallible操作跟随，反序unlock后notify。失败-EAGAIN，全部私有资源锁外释放；复用现有destroy_unpublished_user_mm，stack page只由mm teardown释放。PID可能已消耗不回滚；不要先list再验证rq。保留user_init_task/pid的既有用途、首次PID1归属和原返回成功语义；不要把此任务扩大为exec/FPU全路径审计。
- [ ] **Step 4 — GREEN。** focusedspawn、BOOT-3spawn/stdio、kthread ctor与placement host、test-host、x861/2/4核phase-0和boot-lifecycle。检查普通UP init可成功入CPU0等待BSP发布；AP已ready时PID1允许提前运行但VFS gate已关闭。所有新重检失败分支通过账本断言，失败无notify/list残影，成功无重复释放，不改变fork/API/allowed mask/CPU发布者。
- [ ] **Step 5 — Commit。** `fix(sched): validate initial user-task placement before atomic publication`。

## Task 3: 公共策略、服务、自测和架构机制分离

**Interfaces:** `kernel_start_userspace(void)` noreturn；`boot_services_prepare/start(void)`；`selftest_boot_creation_probe/run_late(void)`；USERSPACE门内strong `arch_sched_boot_prepare/idle(void)`（后者noreturn）。旧task_init定义/声明/引用删除；共享sched_boot_*四API行为不变。

- [ ] **Step 1 — RED策略轨迹。** 新sequence编译真实init/userspace.c（先允许最小加载stub记录接口缺失，再测行为），边界stub记录调用及BSPwindow/scheduler状态。断言creation_begin→futex→console→archprepare→init_mm→publishidle→stdio→end→spawn(pid1)→probe→net→schedstart→thermal→late→archidle；stdio失败不end/spawn，spawn负失败不下游，正!=1fatal不unpublish。服务before-spawn不得调用；net时open/scheduler0，thermal时closed/scheduler1。trap代替永久idle末端，不mock被测orchestrator本身。
- [ ] **Step 2 — RED服务/自测/结构。** focusedservices编译真实init/services.c：net调用一次、无卡由原net层skip且不fatal、thermal返回-EIO只warn。late binary编译真实boot_late.c，用case边界计数九case顺序、END唯一、probe先spawn后且window内；非SELFTEST头不引入对象符号。结构/跨target编译测试旧x86sched_boot.c对fs/net/selftest依赖应RED，ARM不引用USERSPACE hooks、main仅gated调用入口；原BOOT-4三checkpoint测试保留而非删掉迁移失败断言。另建mechanisms独立binary：编译真实x86 cpu/sched_boot.c的arch_sched_boot_prepare/idle以及真实cpu/task_arch.c的arch_task_init_platform（ffunction-sections/gc-sections剔除无关constructor依赖），仅在mock runtime替换set_tss64、硬件halt/IRQ与schedule边界，不替换被测两个hook或arch_task_init_platform。断言prepare准确调用一次set_tss64，目标为TSS64_Table、十个参数分别来自init_thread.rsp0及init_tss[0].rsp1/rsp2/ist1..7，且更新init_tss[0].rsp0；idle在need_resched=0时halt但不schedule，=1时halt→schedule→IRQ-enable→再次halt，模拟schedule返回IRQ关闭，验证再次halt前已恢复；用第二次halt的longjmp结束noreturn测试循环。运行 `make test-boot-services-host CASE=mechanisms`，旧缺失hook的接口RED与TSS/IRQ行为RED分开记录。
- [ ] **Step 3 — 实现。** 创建init源/头对称目录，逐段移动已有代码而非重写：main旧三句移到pipeline；init_mm metadata helper只移segment/VMA初始化，早期CR3保存在原arch_task_init_early。archprepare调用原TSS平台hook；原hlt循环留archidle，用现有arch_cpu_halt()表达hlt硬件边界以便host替换，保持原循环/need_resched/schedule/IRQ-enable顺序。stdio、BOOT-3spawn窗、init日志和Task1end位置精确保留；新三marker按spec固定。net/thermal放services两个不同阶段；probe/late helper移九case原顺序和单END，非selftest inline空函数，不输出成功。kernel/Makefile新增x86 init/*.c收集（当前wildcard不含init），boot_late.c仅x86SELFTEST实现有效；ARM白名单不增加策略/late文件。删除task_init声明，更新common boot注释但不加VFS/USERSPACE条件。
- [ ] **Step 4 — GREEN。** 六focusedCASE、test-host/static/harness；mechanisms focused独立binary注册于hosttests/mk/sched.mk并纳入test-host；TSS十参数/单次调用与idle两分支/恢复IRQ断言必须PASS，不能由策略stub或结构扫描替代。原BOOT-3test_boot_task_init保留测试入口但改测真实kernel_start_userspace及新源，修订文件说明；BOOT-4sequence mock入口换名，继续验证ARM0/0分支。x86/ARM默认产品链接审计：task_init消失、commonboot无service引用、x86mechanism只TSS/idle；ARM无init/services/late新依赖且handoffcaller不变。x86UPselftest验证PID1/probe/net顺序，ARMsmp/no-ack冒烟；失败修复后再进入矩阵。
- [ ] **Step 5 — Commit。** `refactor(boot): separate userspace policy services and architecture startup`。

## Task 4: 真正锁观察与8组启动服务矩阵

**Interfaces:** `BOOT_SERVICES_TEST=none|overlap`（默认空）；variant=boot-services-overlap，x86-only。fixture `void boot5_test_arm(void)`、`void boot5_test_fs_locked(void)`、`void boot5_test_after_init(void)`；普通headerinline空函数、fixture源完全排除。arm在end后/初spawn前，fs_locked仅在namespace/FAT/ext2实际mutex_lock成功后调用，after_init在spawn成功及两init日志之后/probe之前。runner CLI `--case normal|overlap|no-net --cpus N --image PATH --firmware PATH --kernel-elf PATH --log-dir PATH`，Python `parse_serial(text, case, cpus)`独立纯解析；8组串行。

- [ ] **Step 1 — RED parser。** `python3 -m unittest qemutests.test_boot_services`：normal/no-net三marker各一次且BSP顺序，PID1用户输出可早于内核init-created但不能早于vfs-runtime；overlap唯一variant/观察cpu=1..N-1、pid1/bootstrap0/locked1/BSPscheduler0/open，最终runtime-ready；no-net须旧NET_OFF skip及shell。拒绝只有gate而无lock证据、错误CPU/窗口/身份、marker缺失重复乱序、fatal/崩溃/静默/提前自退、期限滑动、排空窗口迟到fatal。普通产物有fixture身份即FAIL。
- [ ] **Step 2 — RED fixture/build边界。** host fixture测试只有arm后、PID1/AP、bootstrap关闭/真实locked分支才release发布观察；错值不产生成功记录，after_init在jiffies+200超时以明确fixtureFAIL停止（不能只缺record后任意继续），从不持FS锁等BSP。dryrun/catalog/scenario/isolation测试必须验证normal1/2/4/8+overlap2/4+no-net1/2=8；顶层none/空无variant，未知值与CFLAGS冲突解析报错，ARM任何非none拒绝，BOOT1/2/3/4fault/selftest/systest/nettest等互斥。
- [ ] **Step 3 — 实现。** observer仅在真实mutex取得的条件分支回调，不在gate query或unlock伪造；固定静态snapshot及release/acquire单次发布，无分配/PID消费，BSP用globaljiffies两秒期限观察，保留原IRQ状态。身份 `[boot-5-test] variant=boot-services-overlap`；观察行严格按spec。mk/project.mk补selector归一化/OS01_SUBMAKE_ALLOWED/ARGS/IMAGE_VARIANT，x86profile推导隔离kernel/image，kernelMakefile翻译固定 `BOOT_SERVICES_TEST_OVERLAP` 宏并只对variant链接fixture；ARMprofile拒绝顶层和直接flag绕过。run.mk及suite/catalog/scenario显式传路径；no-net用普通产品但QEMU `-nic none`且不附加其它NIC，normal/overlap沿既有virtio-rng和网络参数，不由runner隐式build。只读snapshot，normalimagehash前后校验。
- [ ] **Step 4 — GREEN。** test-harness/catalog/scenario、selector跨实际kernel/image子make宏检查后执行 `make PROFILE=x86_64-clang test-qemu SUITE=boot-services`，8组必须全部PASS。overlap日志必须来自实际AP PID1的FSmutex，不以主机日志或人为sleep代替。每QEMU120秒绝对期限，成功链完整后至少1秒排空，runner主动结束；QEMU自退不是成功。归档命令/CPU/ELF/image/firmware哈希、原串口与JSON；parser允许合法用户输出穿插，不把BSP三marker扩成所有CPU串口全序。
- [ ] **Step 5 — Commit。** `test(boot): prove VFS runtime handoff and service startup ordering`。

## Task 5: 整体验收与公开文档

**Interfaces:** 无新运行接口；validation报告区分8组新矩阵、旧回归、交互证据及未执行门禁。roadmap只在取得真实证据后标完成。

- [ ] **Step 1 — 对照清单。** spec§3职责→Task3，§4顺序→Task1/3，§5gate→Task1，§6placement→Task2，§7兼容→Task3/5，§8矩阵→Task4/5；确认无生产新ready bit/延迟AP/pininit等范围变化，已发布资源不因fatal清理。记录实现基线和最终HEAD，不能引用BOOT4报告冒充本项验收。
- [ ] **Step 2 — 通用/x86回归。** `make test-host`、`make test-static`、`make test-harness`；`make test-qemu SUITE=boot-lifecycle`（8）、SUITE=boot-irq-time（10）、SUITE=boot-failure（17）、SUITE=boot-contract（x867）；phase-0/inittab-phase/network；`make OS01_SYSTEST=1 test-qemu SUITE=systest`；另 `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`。正常1/2/4/8产品PID1与latecase/END不回退，net/thermal缺失不升级fatal。
- [ ] **Step 3 — ARM与交互。** `make PROFILE=aarch64-clang test-aarch64 MODE=boot-contract`（ARM7），MODE=smp/no-ack/m3-probe/sched-ap/sched-wake/sched-tick各自执行，及test-aarch64-boot-logo；维持原矩阵与selector，不混BOOT5fixture。x86正常产品用print-run-paths解析真实固件/image，serial stdio管道待shell就绪后发送 `cat\n`、等待、字节0x03、再 `echo BOOT5_SHELL_ALIVE\n`；要求cat默认signal2死亡、随后独立输出行BOOT5_SHELL_ALIVE和可用shell，命令回显本身不算，通过terminal/PTY路径且无terminal被杀重启。保留串口/命令/CPU/时序证据；这是额外交互回归，不计入8组矩阵。
- [ ] **Step 4 — 文档与自审。** architecture/arch/scheduler/filesystem记录公共策略/服务/机制边界、VFSruntime早于首次publish而不等同BSPscheduler、idle豁免限制、UP例外与ARM不适用项；build登记focused/new suite/selector/8组及取证规则；AGENTS Init/Facade/Keyfiles更新；roadmap链接实际验证报告。检查所有本地链接、git diff --check、public声明定义/caller一致、生产fixture符号排除；缺失门禁如实列阻塞，不用静态扫描代替并发证据。
- [ ] **Step 5 — Commit。** `docs(boot): record scheduler service boundaries and BOOT-5 validation`。

## 计划自审

5个任务、25个步骤；Task1完成即修复VFS时序并保持旧入口可启动，Task2完成即加固publish，Task3纯迁移已验证顺序，Task4提供真实并发证据，Task5完整回归。接口缺失编译失败与行为RED分开，测试链接迁移后真实源码，未执行任何实现或QEMU。
