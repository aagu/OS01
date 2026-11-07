# BOOT-2 Early IRQ and Time Startup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 x86 首次可屏蔽 IRQ 到达时 GS/percpu/timer/return-work 尚未满足前置条件的窗口，保留 RTC 校准与 BSP PIT/LAPIC fallback。

**Architecture:** 控制器与 handler 先配置不放行；phase 3 后构造 BSP percpu，phase 4 内仅允许受限 RTC 窗口，phase 4 后启 bootstrap PIT 再开普通 IRQ。BSP handoff 在 IRQ-off 区完成并恢复来时IF，ARM原有启动/调度/IPI编排不变。

**Tech Stack:** freestanding C、x86_64 assembly、Clang/LLVM、PIC/IOAPIC/LAPIC/CMOS RTC/PIT、QEMU/UEFI、C hosttests、Python unittest、既有 profile/harness。

**Spec:** [BOOT-2 R2 设计](../specs/2026-11-08-boot-2-early-irq-time-design.md)，reviewer `boot2_spec_review` 已 APPROVE。基准 `87ceffa7`（编写期间完成的 BOOT-1 收尾提交）；实际实施重新核对 HEAD。本文 R2，独立 reviewer `boot2_plan_review` 已 APPROVE；见[评审记录](../reports/2026-11-08-boot-2-doc-review.md)。本轮仅文档，全部 checkbox 为未来步骤。

## Global Constraints

- 不改变 UAPI、boot_context、percpu/task/thread/pt_regs 布局；不改变 BOOT-1 online/IPI/scheduler/ACK/admission 契约。
- 不实现 BOOT-3 通用失败传播、BOOT-4 阶段枚举/DAG、BOOT-5 调度/服务拆分；不改变ARM编排。
- 本期只覆盖可屏蔽IRQ，不将NMI/异常或全局softirq SMP重构纳入结论。
- `_start` 首条指令cli；PIC配置保持来时IF；GS安装/构造一次，后续facade只验证。
- IRQF_NO_AUTOEN=0x04：从安装起masked，绝不瞬时enable；无flag调用者保持原有auto-enable。
- RTC窗口只有PF(bit6)计数，256个样本、255个间隔；DIV=0按÷2后的decrement rate解释；所有路径恢复/清理IF、拥有的route与采样硬件。
- RTC/TSC-window额外预算 `BOOT2_CALIBRATION_SPIN_LIMIT=50000000u`，不是墙钟时间；不依赖jiffies/schedule完成校准。
- 关闭KERNEL_SELFTEST传空值，不能传0（当前ifdef语义）；syscall顶层用OS01_SYSTEST=1并与kernel-selftest分开。
- 新/修改内核运行日志用log_*；新增kernel源/公开头按子系统和架构路径成对；所有构建从仓库根make入口执行。
- 本文只规划未来实施，不回改其它任务完成的BOOT-1收尾；开始实施时重核对AGENTS.md和工作树。

## Review Focus

1. IF=0并不阻止controller enable把边沿送进IRR：masked registration必须由真实registry/controller trace验证（Task1）。
2. RTC即时pending、AF/UF、注册失败/timeout导致错误采样或未清理硬件（Task2）。
3. clocksource与LAPIC同phase顺序变化、晚期facade二次memset/GS安装（Task2 trace与Task4真实IRQ）。
4. LAPIC失败却虚报accounting有效，或handoff两个周期源同时unmasked、丢失入口IF（Task3）。
5. QEMU错误变体、熵不足、静默/timeout/自行退出或伪造C回调证据被判PASS（Task4严格parser/process/ELF与真实IRQ返回记录）。

## 文件与责任

| 单元 | 修改 / 新建文件 | 责任 |
|------|------------------|------|
| IRQ registry/PIC | 修改 `kernel/include/intr/interrupt.h`、`kernel/intr/irq.c`、`kernel/arch/x86_64/intr/pic_8259a.c` | NO_AUTOEN、PIC cascade、IF保持 |
| 启动编排 | 修改 `kernel/arch/x86_64/head.S`、`platform/boot.c`、`smp/boot.c`、`kernel/include/arch/x86_64/boot.h`、`kernel/include/arch/x86_64/smp_boot.h` | cli；phase3→percpu→phase4→runtime IRQ→phase5/6；晚期facade不重构造 |
| 私有协调 | 新增 `kernel/arch/x86_64/platform/boot_irq.c` 与 `kernel/include/arch/x86_64/platform/boot_irq.h` | 一次性context/calibration/runtime状态与前置检查 |
| 时间设备 | 修改 `kernel/driver/pit.c`、`kernel/include/driver/pit.h`、`kernel/arch/x86_64/platform/rtc_pie.c`、`platform/time.c`、`intr/apic/lapic_timer.c`、`kernel/include/arch/x86_64/rtc.h`、`kernel/include/intr/apic.h` | prepare/start、RTC事务、频率cache、calibration/handoff |
| Host | 新增 `hosttests/cases/test_boot_irq_registration.c`、`test_boot_irq_context.c`、`test_rtc_pie_calibration.c`、`test_boot_tick_handoff.c`、`hosttests/mock/boot_irq_time/`下隔离寄存器/CPU头和runtime；修改 `hosttests/mk/intr.mk`、`sched.mk`、已有IRQ mock/tests | 真实对象与硬件投递模型，不复制状态机 |
| 观测fixture | 新增 `kernel/selftest/boot_irq_time_fixture.c`、`kernel/include/selftest/boot_irq_time_fixture.h`；修改 `intr/entry.S`、RTC/PIT/LAPIC/boot接线 | 隔离变体真实handler/return-work计数，BSP输出快照 |
| Runner | 新增 `qemutests/boot_irq_time.py`、`qemutests/test_boot_irq_time.py` | 10组矩阵、严格判定、进程/归档/身份审计 |
| 构建/CI | 修改 `mk/project.mk`、双架构profile、`kernel/Makefile`、`mk/components/run.mk`、`mk/tests/catalog.mk`、`config/test-scenarios.json`、`.github/workflows/ci.yml`；更新相关suite/CI/isolation Python约束 | selector、变体、受控子make、inventory/catalog/scenario/harness/CI |
| 文档 | 修改 `docs/boot/boot.md`、`docs/time/timer.md`、`docs/interrupt/interrupt.md`、`docs/arch.md`、`docs/build/build.md`、`docs/roadmap.md`；新增 `docs/superpowers/reports/2026-11-08-boot-2-validation.md` | 契约、门禁结果和闭环状态 |

boot_irq源/头目录对称，x86 platform源自动发现；fixture从普通selftest wildcard排除，仅测试宏选择时加入。已有BOOT-1文档不顺带重写；实施阶段再按最终代码更新上述相关文档。

## Task 1: 实现真正的 masked registration 与 PIC cascade

**Files:** registry/PIC行、`hosttests/cases/test_boot_irq_registration.c`、对应mock、`hosttests/mk/intr.mk`、根run.mk。

**Interfaces:** 保留register_irq签名/返回1成功0失败；新增IRQF_NO_AUTOEN=0x04；PIT/RTC尚不改调用。PIC enable slave同时开master IRQ2，disable最后slave才关cascade；保护IMR读改写并恢复入口IF。不在本任务删除pic_init/wrapper的sti（需Task2同步接入校准窗口）。根focused入口 `make PROFILE=x86_64-clang test-boot-irq-time-host CASE=registration|context|rtc|handoff|all` 默认all；每CASE只依赖本组binary，后续组不存在不能阻断registration行为RED。

- [ ] **Step 1 — 写RED。** 先仅在生产interrupt.h声明IRQF_NO_AUTOEN=0x04，保留register_irq旧实现，不实现flag分支；这是让测试可编译的声明准备，不算行为修复。然后编译真实irq.c/PIC和现有x86 route/dispatch边界（沿用IRQ_CHAIN模式）。`no_autoen_never_enables`：mask从初态开始，controller enable若被调用立刻锁存pending、即使IF=0；断言NO_AUTOEN路径enable次数0、handler已注册、IF不变；`legacy_registration_autoen`保留默认1次。`pic_slave_requires_cascade`用真实PIC端口I/O模型，只有slave bit0与master bit2同时为0才可投递IRQ8；`pic_disable_keeps_other_slave`保留其它slave时IRQ2仍开，最后一个关闭才可mask；`install_failure_does_not_enable`验证回滚。
- [ ] **Step 2 — 注册并确认RED。** intr.mk注册新binary（intr、protocol-or-legacy）且加入TEST_BINS；用私有`boot2-registration/`对象，真实irq结构/flag声明从生产头取，不能mock重定义遗漏新flag。根入口校验CASE并经os01_submake显式选择host子目标，未知CASE非零。先只执行 `make PROFILE=x86_64-clang test-boot-irq-time-host CASE=registration`，记录enable计数/cascade投递断言失败；不得让尚不存在context/rtc/handoff编译阻断它。
- [ ] **Step 3 — 实现。** 使用Step1已声明的flag，register_irq在NO_AUTOEN时确保install维持masked并跳过enable；不得先enable再disable，也不得改变普通调用者默认行为/flag触发位。PIC slave enable/disable按spec管理cascade、IRQ-save/restore保护IMR更新；保持非目标mask位。模拟install失败不注销其它owner。现阶段不接入生产PIT/RTC调用，避免引入未有开窗路径的中间态。
- [ ] **Step 4 — GREEN。** 跑registration CASE、`make PROFILE=x86_64-clang test-intr-irq-x86`、`make PROFILE=x86_64-clang test-host`；检查普通驱动registration默认auto-enable未变，模拟pending在后续sti才递送且NO_AUTOEN未生成pending。共享header变化还须 `make PROFILE=aarch64-clang aarch64-uefi`。
- [ ] **Step 5 — Commit。** `feat(irq): support masked registration and PIC slave cascade`。

## Task 2: 建立 BSP context 和受控 RTC 窗口

**Files:** 启动编排行、私有协调行、PIT/RTC/clocksource相关行、LAPIC init准备部分；context/rtc host、对应mock、registry/phase契约测试、host mk/root focused接线。

**Interfaces:** 新增 `void x86_boot_irq_context_prepare(void)`、`bool x86_boot_irq_calibration_allowed(void)`、`void x86_boot_irq_calibration_begin(void)`、`void x86_boot_irq_calibration_end(void)`、`void x86_boot_irq_runtime_open(void)`、`bool x86_boot_irq_runtime_opened(void)`。私有一次状态cold→context↔calibration→runtime；非法begin/runtime-open先fatal，不在cold时读GS。allowed仅检查state/入口IF且不改硬件，错误RTC参数返回-1。PIT改 `int pit_init(void)`（prepare-only，0成功/-1失败），新增 `int pit_start(void)`、`bool pit_prepared(void)`；start一次，handoff的mask/unmask不算第二次start。RTC签名不变，只有成功写两个输出。`arch_cycle_freq()`首次合法boot调用缓存结果和0失败，后续纯查询，不再调用RTC。

- [ ] **Step 1 — RED context/RTC。** context组明确链接真实 `kernel/driver/pit.c`、`kernel/time/tick.c`、`kernel/time/timer.c`、`kernel/intr/softirq.c`、`kernel/intr/irq.c`、`kernel/arch/x86_64/intr/irq_entry.c`、`irq_hooks.c`、`irq_route.c`、`kernel/intr/vector_dispatch.c` 及已有真实decoder/dispatcher对象；不得沿用test_intr_irq_x86.c中stub tick_handler的部分。只替换机器/控制器I/O、poll/accounting外部边界及host不可执行的GS寄存器读，poison accessor在percpu前访问即失败。模拟controller pending经真实registry/dispatch→PIT→tick处理：prepare/RTC阶段jiffies/need_resched/watchdog/softirq均不变化，runtime首次投递后精确jiffies+1/watchdog+1/need_resched置位，timer_list已由真实timer_init建立，TIMER_SIRQ只在合法到期时设置，真实do_softirq执行已注册action。host不执行iret，汇编返回由ELF和Task4补证。context trace另记录真实boot orchestrator依次phase3→percpu构造/GS→phase4→PITstart/IFon→phase5→6；测试pre-GS poison访问、late facade不二次构造、同phase LAPIC stub先/后clocksource两种顺序。RTC真实对象mock每次硬件enable可立即pending，在STI后通过注册handler投递，C.PF=0x40采256次；C=0/AF/UF先到不能计数，257th不得改样本。测试registration失败无硬件副作用/不unregister他人、参数/IF错误不写输出、success/timeout/stuck-TSC budget都清理A/B/route/IF/active及maskedLAPIC。测试只用真实事务函数，mock不实现事务步骤；生产对象未具接口时的链接失败与行为失败分别记录。
- [ ] **Step 2 — 独立确认RED。** 注册context/rtc两个binary（intr/sched按职责固定，均protocol-or-legacy），各自CASE无另组前置；先运行CASE=rtc针对旧真实rtc_pie.c拿到非PF计数/状态重置时机的行为RED，再接入新context接口并单列未实现错误。需要旧对象编译时，仅mock其已有架构/IRQ/CMOS接口，不依赖新头。`CASE=context`以真实现有boot helper trace证明首次phase4前无GS，不能仅凭“文件不存在”声称时序RED。
- [ ] **Step 3 — 同步实现可启动的顺序变更。** `_start`第一指令cli；pic_init/wrapper移除sti、保持输入IF。x86_boot_subsystems分段3→x86_64_boot_percpu→context_prepare→4→lapic_timer_calibrate→runtime_open→5/6；BOOT-1 device_boot_result检查仍在phase6后。x86私有percpu构造入口拒绝重复，晚期arch_boot_percpu只验证num_cpus/self/CPU0 online，不写GS。context准备检查IF0、BOOT-1前置、BSP init_task_union栈；IDT/TSS/softirq由真实early调用链先建立，host trace/ELF和后续QEMU证明，而非手设状态替代证据。PIT prepare用NO_AUTOEN、设置100Hz，initcall wrapper直接返回pit_init结果；runtime_open验证subsys timer/pit/lapic-timer已成功（clocksource active=0允许）、PITstart成功后state=runtime再sti。LAPIC init只装stub，calibration末尾执行，不能phase4里初始化两次timer。RTC按spec完整事务、PF饱和、cleanup与50M预算；缓存freq包括0，禁止在SMP/已运行timer时重校准。输出失败不覆盖原值，CPUID非零无RTC副作用。
- [ ] **Step 4 — GREEN与正常回归。** 跑context/rtc/registration CASE以及 `make PROFILE=x86_64-clang test-boot-lifecycle-host CASE=all`；更新已有BOOT-1/source契约对提前percpu/late facade的预期而不放松生命周期断言。执行x86 image、`QEMU_SMP=1`与2的phase-0、ARM image/m3-probe；检查first cli、BSP构造一次、RTC清理后IF0、phase5仍有PIT时基。记录BOOT-2生产context/runtime-open标记；不把正常boot当作故障矩阵完成。
- [ ] **Step 5 — Commit。** `fix(boot): prepare BSP context before controlled RTC interrupts`。

## Task 3: 有界 LAPIC calibration 与原子 BSP handoff

**Files:** platform/time.c、intr/apic/lapic_timer.c、boot_irq头/源、intr/apic.h；`test_boot_tick_handoff.c`、RTC math/cache测试和mock；host mk与focused handoff/all接线。

**Interfaces:** 保留 `void lapic_timer_calibrate(void)`（失败清频率/保持mask）、`bool lapic_timer_start(uint32_t freq_hz)`、`bool arch_tick_start(void)`。校准统一额外pause上限50M；RTC保留raw TSC >2^32超时；宽中间算术、反向/零/不可表示样本拒绝。handoff只在BSP runtime-open且PIT prepared后调用，IRQ保存/屏蔽PIT→尝试LAPIC→发布accounting→恢复IF；false指PIT有效fallback，双源不可用fatal。

- [ ] **Step 1 — RED handoff/math。** 真实time.c/lapic_timer.c对象配寄存器/计数器stubs，断言LAPIC成功前后周期源mask trace、失败保证LAPIC stopped/masked后PITunmask、accounting只正确发布一次；入口IF0/1均恢复。PIT未prepared时失败fatal、不发布valid；正常源已准备时重复runtime_open/PITstart被拒绝。TSC不前进、elapsed=0/反向/计数耗尽、50M边界及频率乘法溢出均有限失败；CPUID得到频率/RTC成功/RTC失败0三条cache路径第二调用均不触碰RTC。mock budget可用生产编译常量受控缩小来加速，但完整默认值的定义需单独审计，不能把测试预算带入产品。
- [ ] **Step 2 — 确认RED。** `make PROFILE=x86_64-clang test-boot-irq-time-host CASE=handoff`，以真实hand-off trace和无界TSC场景的host执行超时/受控pause上界捕获记录RED；已有正确的÷2频率语义仅记录兼容性GREEN，不人为破坏。cache/math补充用例归CASE=rtc，独立运行。
- [ ] **Step 3 — 实现。** 校准在phase4末、IF0、LVT masked下执行，新增有限loop和样本检查；保持返回void以现有0Hz不可用表达失败。lapic_timer_start参数拒绝freq=0/溢出并保证失败masked/stopped。arch_tick_start保存IF，在同一区域完成mask/选择/accounting，若失败恢复bootstrap PIT；不要调用已执行过的一次性pit_start。保留单次在途PIT IRQ允许，不添加虚假的pending取消保证。AP start不采用“BSP强制失败”注入；AP与BSP tick/accounting归属不改。handoff记录spec tick-source日志。
- [ ] **Step 4 — GREEN。** focused全部CASE、test-intr-irq-x86、test-host、test-static，复核已存在clocksource测试及÷2计费/频率断言。x86 phase-0/network、BOOT-1 boot-lifecycle矩阵；确认early accounting_ready=0继续安全、后续BSP只记一次。源选择与mask寄存器结果不能仅由日志文案自证。
- [ ] **Step 5 — Commit。** `fix(time): bound calibration and serialize BSP tick handoff`。

## Task 4: 真实 IRQ 返回证据与隔离 QEMU 矩阵

**Files:** fixture源/头、entry.S与RTC/PIT/LAPIC/boot测试接线；runner/tests；构建/CI/登记行；复用既有ProcessSession/RunArchive/RunReport，不重写harness。

**Interfaces:** selector `X86_BOOT_IRQ_TIME_TEST=none|force-rtc|rtc-timeout|bsp-lapic-fail|unsafe-open`，默认空/none；有效宏派生 `boot-irq-time-<case>` kernel/image变体，aarch64拒绝，禁止与BOOT-1/X86_INTR/其它fault或KERNEL_SELFTEST/OS01_SYSTEST/NETTEST/canary/resolution/capacity等混用。runner CLI `--case normal|force-rtc|rtc-timeout|bsp-lapic-fail|unsafe-open --cpus N --firmware PATH --image PATH --kernel-elf PATH --qemu PATH --build-dir PATH --profile x86_64-clang --timeout 90`；纯判定 `check_evidence(text: str, case: str, cpus: int) -> tuple[bool, list[str]]`，进程边界可注入。

fixture API 固定为 `void boot2_test_irq_return(const pt_regs_t *regs)`、`void boot2_test_rtc_pf(const pt_regs_t *regs)`、`void boot2_test_timer_irq(unsigned source, const pt_regs_t *regs)`（source=0 PIT、1 BSP LAPIC）、`void boot2_test_after_rtc(int result)`、`void boot2_test_after_runtime_open(void)`、`void boot2_test_after_handoff(bool lapic)`；仅隔离变体链接。观测仅在BSP且AP启动前的RTC/bootstrap/handoff指定阶段激活，结束后关闭；各handler按实际regs记录(cpu=0, source, frame地址, phase) token，return hook只消费同cpu/phase/frame的token，增加该source返回计数，未配对的syscall/#PF/其它IRQ不计。所有统计是各phase内计数器差值，AP不得贡献；任何非预期重叠token应FAIL，不能覆盖后继续。入口/handler只累计状态，BSP关闭IRQ取快照后输出，不清live计数；helper不得递归校准、schedule或在IRQ中打印。

- [ ] **Step 1 — RED fixtures/parser/process。** 合成完整串口；加入返回来自syscall/#PF/不同frame/AP/上一phase、目标handler有计数但无配对return、缺bootstrap证据/在devices-begin后出现等负例，不能由累计return补齐目标。另覆盖缺失/重复/截断/错误CPU/case/错源/错IF/RTC非PF计数/不足returns/fatal后继续等负例；QEMU silent/timeout/自行退出非PASS，fatal完整后≥1s观察且runner自停才可PASS。FakeSession分块覆盖不带换行shell prompt和split标记。ELF/路径tests验证flag绕过普通selector也不能污染normal；有效宏映射一套，root子make正确传播；unknown/mixed/aarch64 selectors非零拒绝。正常/故障镜像需virtio-rng，不能把熵不足归为boot失败修复证据。
- [ ] **Step 2 — 确认RED。** `python3 -m unittest qemutests.test_boot_irq_time -v`，先使模块与纯stub可加载，再记录行为断言/NotImplementedError和build-contract失败，不只记录ImportError。新增模块到TEST_HARNESS_MODULES；新suite whitelist/help/catalog/scenarios有明确断言，未实现QEMU目标不能靠超时通过。
- [ ] **Step 3 — 实现fixture与身份。** 在entry.S的ret_from_intr/check_signal的两个RESTORE_ALL出口改跳测试私有收尾标签（正常宏关闭时保持原路径），传入原始rsp即pt_regs地址；用ABI callee-saved临时寄存器保存原rsp，向下对齐rsp至16字节后call，再恢复原rsp并jmp RESTORE_ALL，全部GPR仍由原保存帧恢复。异常restore-only不接线；ret_from_intr中的syscall/#PF也可能经过该标签，必须依照frame token不计入目标IRQ。ELF反汇编测试检查参数、栈恢复和仅测试变体接线。force-rtc跳过CPUID nominal；rtc-timeout还保持RTC route masked；bsp-lapic-fail只影响BSP handoff、不影响AP正常start；unsafe-open在context前调用begin并触发context FATAL。变体起始输出 `[boot-2-test] variant=boot-irq-time-<case>`。RTC结束输出 `[boot-2-test] rtc result=<ok|timeout> pf=<n> returns=<n> if=0 jiffies_delta=0`，force-rtc要求pf=256、returns>=256，timeout要求pf=0且返回有限失败；计数来自真实handler和同帧IRQ返回。runtime_open返回后、调用phase5之前调用after_runtime_open：IF-save下启用bootstrap phase并取基线，恢复IF后用rawcounter/finitebudget等至少3次真实PIT及配对return；再次关IRQ取快照，输出 `[boot-2-test] bootstrap source=pit count=<n> returns=<n> jiffies_delta=<n>`（三者均>=3），关闭phase再恢复IF。在下一条phase5调用前输出 `[boot-2-test] devices-begin`。parser必须验证bootstrap在devices-begin前且在handoff tick记录之前，不能拿handoff后PIT替代。handoff后另取基线，等实际选中源至少3次IRQ及同source/frame配对return，输出 `[boot-2-test] tick source=<pit|lapic> count=<n> returns=<n> jiffies_delta=<n>`，均>=3；rawcounter/finitebudget保护等待，失败显式FAIL而非无限等。normal不编fixture，仅要求生产阶段日志与BOOT-1/PID1/shell；normal ELF无fixture符号/marker。执行sample只读快照不得清live ISR counter。
- [ ] **Step 4 — GREEN与10组。** 根 `make PROFILE=x86_64-clang test-qemu SUITE=boot-irq-time`自建普通镜像后取hash：normal1/2/4/8、force-rtc2、rtc-timeout1、bsp-lapic-fail1/2/4、unsafe-open1。从独立bootstrap记录及其devices-begin次序证明phase5前PIT工作、RTC内jiffies不增、RTC完成/清理后真实IRQ返回继续执行；成功组有BOOT-1 complete、PID1和shell，unsafe组有唯一context fatal且无runtime-open/BOOT-1 complete/PID1。用-snapshot/私有镜像运行，结束核对normal hash不变。catalog条目nightly/rootfs/isolated/exclusive/512MiB/单case90s，整个10组CI job至少25分钟；准备/运行target、suite/CASE枚举、scenario primary证据同步。运行该Pythonmodule、test-harness、catalog/scenario --check；真实QEMU组全有独立RunReport，不把host handler调用当硬件IRQ证据。
- [ ] **Step 5 — Commit。** `test(boot): validate early IRQ windows and timer fallback in QEMU`。

## Task 5: 双架构门禁与文档闭环

**Files:** 文档行、新验证报告、实际CI/测试登记需要的最后调整。不把其它任务既有BOOT-1收尾算本期产物。

**Interfaces:** 消费前四任务；输出验证报告，记录原始基准/最终提交、测试命令/返回码、原始serial/ELF/image/结果路径、原始RED及剩余边界。

- [ ] **Step 1 — 覆盖复核。** 将spec §3不变量、§4 RTC各出口、§5注册/PIC/handoff、§7矩阵逐项映射到实现与证据。确认旧registry enable IRR锁存RED、PIC cascade RED、非PF采样RED有效；无第二次timer/percpu/GS初始化；所有调用clocksource/arch_cycle_freq的地点符合缓存和boot-only校准。新增源/头目录对称，修改内核日志符合规范。
- [ ] **Step 2 — 最终门禁。** `make PROFILE=x86_64-clang test-host`、`test-static`、`test-harness`、`test-qemu SUITE=boot-irq-time`、`test-qemu SUITE=boot-lifecycle`、`test-qemu SUITE=intr-x86`，以及 `test-intr-irq-x86`；`QEMU_SMP=1/2`分别phase-0、network；`make PROFILE=x86_64-clang OS01_SYSTEST=1 KERNEL_SELFTEST= test-syscall`；单独 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`。ARM `test-aarch64 MODE=smp`，按现有NO_ACK=1显式build后跑no-ack，再NO_ACK=0恢复正常，运行m3-probe/sched-ap/sched-wake/sched-tick；每条从根完整运行，记录return code。layout/static和双架构编译保持通过。
- [ ] **Step 3 — 写验证报告与契约。** 更新boot/time/interrupt/arch/build对应阶段、私有facade提前准备含义、NO_AUTOEN/PIC cascade、RTC清理、bootstrap PIT/accounting起点、handoff在途IRQ允许。记录QEMU10组和ARM/BOOT1回归，不声称解决NMI、全局softirq SMP、AP无本地timer或BOOT-3～5；受环境阻塞则保持未完成并说明命令/证据缺口。
- [ ] **Step 4 — 文档/状态检查。** `git diff --check`、本地链接与catalog/scenario检查；核对证据对应最终文件和提交。只有真实验收完成才能在roadmap关闭BOOT-2并移入changelog；文档审批阶段只写“spec/plan获批、待实施”。不重写其它任务的BOOT-1进度。
- [ ] **Step 5 — Commit。** `docs(boot): record BOOT-2 IRQ and time startup validation`。不自动开始BOOT-3或合入master。

## 本轮交付边界

本次只编写spec/plan并由独立子agent多轮评审；获批后停止。实际执行须重新确认工作树与BOOT-1最终基准，按选定实施方式执行上述步骤。本轮没有任何生产代码或QEMU通过声明。
