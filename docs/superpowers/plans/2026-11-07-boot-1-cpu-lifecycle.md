# BOOT-1 CPU Lifecycle and SMP Handshake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 CPU online 表示目标 CPU 已完成本地初始化，修复 x86 AP 启动握手和迟到资源风险，同时保留 aarch64 ACK/admission/IPI 三层资格。

**Architecture:** 保留 percpu_t 布局和独立 online/ipi_ready/scheduler_ok 字段，显式发布、acquire 消费。x86 BSP 等 online、执行 TSC 握手，再等 scheduler_ok 后才复用 trampoline；超时停止 BSP 并保留暴露资源。aarch64 仅补显式 runtime online 发布及验证，不改变 PSCI/admission。

**Tech Stack:** freestanding C、x86_64/AArch64、Clang/LLVM、QEMU/UEFI、C hosttests、Python unittest、现有 make profile 和 ProcessSession/RunArchive。

**Spec:** [BOOT-1 R2 设计](../specs/2026-11-07-boot-1-cpu-lifecycle-design.md)，基准 `64d23013`；spec reviewer `boot1_spec_review` 已 APPROVE。本文 R2，独立 reviewer `boot1_plan_review` 已 APPROVE；见[评审记录](../reports/2026-11-07-boot-1-doc-review.md)。全部 checkbox 为未来实施步骤，本轮只写/审文档。

## Global Constraints

- 不改变 percpu_t=144 字节、GS/汇编偏移、ipi_ready/tlb_ack_gen 末尾布局、UAPI 或 boot_context。
- 不重排早期 IRQ、校准、挂载、PID 1 创建和系统服务；BOOT-2～5 另行实施。
- num_cpus 是发现 CPU 的遍历上界，不是 online 计数；保持 CPU 编号和拓扑映射。
- online 发布为 release，跨 CPU 读取为 acquire；IPI/调度保留原有各自门控。
- aarch64 boot slot ACK、冻结 accepted mask、runtime online 独立；未接纳但 online/IPI ready 的 AP 继续参与 shootdown，不获得调度资格。
- x86 必需等待预算 5000000 次、TSC 预算 1000000 次；不是墙钟秒数；任何暴露给 AP 的资源不得在超时后释放/复用。
- 所有新/修改内核运行日志用 log_* / debug_channel；纯 selftest 协议标记按仓库例外。
- 新 kernel 源与公开头成对；仅从仓库根 make 入口构建/运行，使用 profile 私有产物。
- syscall E2E 必须 `make OS01_SYSTEST=1 test-syscall`，不得混用 KERNEL_SELFTEST=1。

## Review Focus

1. percpu 构造后误报 online；ARM 显式发布遗漏导致 shootdown 目标为空（Task 1 的生产对象测试、Task 4 的真实 IPI/ACK 证据）。
2. online 已发布但 scheduler/IPI 尚未就绪，BSP 提前覆盖 trampoline 或释放栈（Task 1 等待与集成契约、Task 3 fatal 场景）。
3. TSC 互等、publication 顺序和最后一次检查边界错误（Task 1 边界用例及 TSC 源码契约；Task 3 多核启动）。
4. ARM NO_ACK 被误当 offline 或被调度接纳；未来移除 probe 时丢掉初始化排他屏障（Task 4 负例与 gate 契约）。
5. 错镜像、缺/重复/截断证据、fatal 后继续、QEMU 自行退出或 timeout 被误判 PASS（Task 3 parser/process/build contract 负例）。

## 文件与责任

| 单元 | 文件 | 责任 |
|------|------|------|
| 公共构造 | 修改 `kernel/percpu/percpu.c`、`kernel/include/percpu/percpu.h` | 构造不发布 online，记录调用前提和能力含义 |
| x86 启动 | 修改 `kernel/arch/x86_64/smp/smp.c`、`smp/boot.c`；新增 `kernel/arch/x86_64/smp/boot_sync.c` 与 `kernel/include/arch/x86_64/smp/boot_sync.h` | 有界等待、两阶段握手、TSC 原子协议、fatal 资源保留 |
| ARM 发布 | 修改 `kernel/arch/aarch64/smp/smp.c`、`kernel/arch/aarch64/boot/main.c` 的契约注释 | AP 自构造后显式 online，保留现有 BSP/gate/PSCI 机制 |
| 并发消费者 | 修改 `kernel/sched/balance.c`、`kthread.c`、`diag.c`、`kernel/memory/tlb.c`、`kernel/arch/x86_64/intr/apic/ipi.c`；检索其它生产/selftest online 读取 | acquire 状态快照，保持既有能力/admission 条件 |
| Host | 新增 `hosttests/cases/test_boot_cpu_lifecycle.c`、`test_x86_smp_boot_sync.c`、`hosttests/mock/boot_cpu_runtime.h`、`x86_smp_boot_sync_runtime.h`；修改 `hosttests/mk/sched.mk`、现有 TLB mock/test 与发布顺序测试 | 编译真实构造/等待对象；原子及目标资格验证 |
| x86 E2E | 新增 `qemutests/boot_lifecycle.py`、`qemutests/test_boot_lifecycle.py` | 严格 parser、隔离矩阵、进程归档及 build contract |
| ARM E2E | 修改 `qemutests/aarch64_uefi_smp.py`、`test_aarch64_harness.py`、`test_aarch64_ap_boot_contract.py`；必要的 NO_ACK 观测接线放已有 ARM smp/probe 文件 | 实际 runtime online/IPI/accepted/scheduler 区分 |
| 构建注册 | 修改 `mk/project.mk`、双架构 profile、`kernel/Makefile`、`mk/components/run.mk`、`mk/tests/catalog.mk`、`config/test-scenarios.json`、`.github/workflows/ci.yml` | selector/路径隔离、子 make 旗传播、host/QEMU/harness/CI 注册 |
| 文档证据 | 修改 `docs/smp/smp.md`、`docs/arch.md`、`docs/subsys/subsys.md`、`docs/build/build.md`、`docs/roadmap.md`；新增 `docs/superpowers/reports/2026-11-07-boot-1-validation.md` | 契约与实际执行证据，完成后才关闭 BOOT-1 |

现有 `arch/x86_64/smp/*.c` 自动发现可收集 boot_sync.c；实施时核对真实 build inventory。新增公开头的目录和源目录对称；host mock 只替换机器边界，不复制状态机。开始实施前重新核对 HEAD/AGENTS.md；不覆盖工作区已有改动。

## Task 1: 修复构造/发布与 x86 两阶段握手

**Files:** 上表公共构造、双架构发布、x86 启动文件；新增 host 源/mock、`hosttests/mk/sched.mk`、`mk/components/run.mk`；修改 `hosttests/cases/test_double_state_publish.c`。

**Interfaces:** 消费现有 `void percpu_init(uint32_t cpu, uint32_t apic_id)`、`void smp_boot_aps(void)`（x86）、现有 IPI 发布函数和 TSC 字段。新增 `bool x86_smp_wait_u32_acquire(const uint32_t *flag, uint32_t budget)`：NULL/零预算返回 false；每轮先 acquire 读非零则 true，否则 pause，最多 budget 次，最后 pause 后不追加读取。常量 `X86_SMP_READY_SPIN_LIMIT=5000000u` 定义在配对头。该函数不读取 current、不分配、不关 IRQ。根 focused 入口为 `test-boot-lifecycle-host CASE=construction|wait|all`，默认 all；construction 只构建/运行真实构造测试及现有 layout/publish 测试，不依赖 wait 对象；wait 只依赖等待测试；all 才聚合两者。

- [ ] **Step 1 — 建立基准和 RED。** 记录原始 HEAD、x86 phase-0 单/双核与 ARM smp/no-ack 现有结果（失败也保留）；新 `test_boot_cpu_lifecycle.c` 链接真实 percpu.c 和真实 percpu_t，替换 arch_set_percpu_base 等机器边界。用 `construct_cpu_stays_offline` 断言 CPU1 构造后 online=0、scheduler_ok=0、self/identity/rbtree/rq_lock 有效；`construct_preserves_tail_layout` 检查末尾机制和布局，测试不构造已发布对象。`test_x86_smp_boot_sync.c` 定义 initial-ready、publish-on-first-pause、never-ready、zero-budget、last-check-ready、publish-after-last-pause、NULL 七类测试，mock pause 递增计数并按配置 release 写 flag。扩展发布契约：ARM AP 初始化/页表验证 → online → boot ACK → IRQ/ISB → IPI；x86 online → go/sample → IPI → scheduler；等待 scheduler 后才有下一次 trampoline 填写。
- [ ] **Step 2 — 确认 RED 与 root 接线。** 新增根 `make PROFILE=x86_64-clang test-boot-lifecycle-host CASE=construction`，根校验 CASE 后通过 os01_submake 显式选择 hosttests `test_boot_lifecycle_construction|wait|all` 目标；不要把未来 wait binary 作为 construction 目标前置项。先只运行 construction 保存真实 online=1 的行为断言 RED，再运行 CASE=wait 单列接口缺失的编译/链接失败；后者不代替前述行为 RED。新 binary 注册 `register_host_test(...,sched,protocol-or-legacy)` 并加入 TEST_BINS；root focused dispatch 的未知CASE必须非零失败。host mock 使用真实 percpu 头，preinclude 仅替换架构寄存器/机器函数；对象名 `boot_cpu_percpu_production.o`、`x86_boot_sync_production.o` 避免污染别的 suite。默认 CASE=all 对应两个新 binary 和 existing test_percpu_layout/test_double_state_publish。
- [ ] **Step 3 — 实现协议。** percpu_init 移除 online 发布，保留 memset 边界；x86 BSP/ARM BSP 按 spec 发布 online，ARM AP 在 page verify 成功后 boot ACK 前 release 发布 online。x86 AP release online，TSC go/sample 改 release/acquire、tsc_boot 在 sampled 之前写；随后 IRQ→IPI→scheduler_ok。BSP 对每个 CPU 先检查初始 online/scheduler_ok 都为 0，准备资源/发 SIPI，调用真实 wait helper 等 online，完成原有有界 TSC 采样后等 scheduler_ok，再 acquire 验证 online/IPI。不满足则通过局部 noreturn fatal helper 输出 spec 标记并 kpanic；删掉启动失败后的 list_del/free/继续启动分支。分配未暴露资源失败可清理后 fatal。保持 hardware delay 和 tick/accounting 顺序。
- [ ] **Step 4 — GREEN 和最小回归。** 运行根 focused host 目标；检查 pause 次数与所有边界断言。运行 `make PROFILE=x86_64-clang image`、`QEMU_SMP=2 make PROFILE=x86_64-clang test-qemu SUITE=phase-0`、`make PROFILE=aarch64-clang aarch64-uefi` 和 `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe`；新增 `ipi_ready_publish_ap/bsp` online 断言。ready 标记在 BSP 确认第二阶段后输出，complete 包括 BSP，严格匹配 spec §6/8；现有协议日志不改名。检查构造与 AP 并发前置条件，没有新增在线后 memset。
- [ ] **Step 5 — Commit。** 仅提交本任务文件：`fix(smp): separate percpu construction from CPU readiness publication`。此时协议修复可正常运行，后续任务补全消费者与动态故障证据；尚不宣称 BOOT-1 验收完成。

## Task 2: 审计所有 runtime online 消费者和 shootdown 资格

**Files:** 消费者行、`kernel/selftest/test_x86_intr.c`、其它检索到的 runtime online selftest 读取；`hosttests/cases/test_tlb_serial_protocol.c`、`hosttests/mock/tlb_test_runtime.h`、`hosttests/mk/sched.mk`；已有 scheduler host 测试随真实改动更新。

**Interfaces:** 消费 Task 1 的 online release；不增加公共 API。TLB 条件保持 online ∧ ipi_ready ∧ 非自身；调度条件保持 online ∧ scheduler_ok ∧ allowed/admission。mock TLB percpu 类型补 scheduler_ok 字段以表示 IPI-only 状态，仍明确它是依赖隔离 mock，布局由真实类型测试验证。

- [ ] **Step 1 — 补资格测试。** 扩展真实 tlb.c host 用例 `offline_ready_excluded`、`online_ipi_missing_excluded`、`online_ipi_without_scheduler_included`：检查 IPI mask、目标 ack 增量和本地 flush，第三例 scheduler_ok=0 仍必须入目标。保留 existing self/代数回绕/timeout 测试。对既有 balance/kthread host 用例增加 offline、online但未调度、ready且allowed三态，明确 boot creation window 仅允许 CPU0 的原有例外。对当前已正确行为允许测试直接 GREEN，不伪造 RED。
- [ ] **Step 2 — 执行资格基线。** 将 test_tlb_serial_protocol 加入 focused 聚合目标，运行 `make PROFILE=x86_64-clang test-boot-lifecycle-host`；对失败项记录行为 RED，对已通过项记录兼容性基线。保留调用链而非仅源码字符串包含测试。
- [ ] **Step 3 — 统一并发访问。** 在生产/selftest 状态选择及诊断读取中把 plain online load 改 acquire；保留或增强已有 IPI/scheduler 条件，不把 generic ipi_broadcast 的协议责任扩成所有向量统一检查 ipi_ready。修改 sched/diag.c 时同步用 log_*，不得新增 bare serial_printk。检索 go/sampled、online 写入并列出生产者表，区分 ARM boot slot 同名字段；单线程初始化/mock赋值不冒充运行期并发发布。
- [ ] **Step 4 — 验证。** 跑 focused host、`make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-static`；核对真实 percpu 144 字节和偏移不变。对 online 访问逐个语义审查，无遗留并发 plain 读写；ARM probe gate 前各路径无远端 online 读取，记录已验证范围。
- [ ] **Step 5 — Commit。** `fix(smp): acquire CPU readiness in scheduling and IPI consumers`。

## Task 3: x86 隔离故障矩阵与严格证据 runner

**Files:** 新 runner/tests；`mk/project.mk`、双架构 profile、`kernel/Makefile`、`mk/components/run.mk`、catalog/scenarios/CI；x86 smp.c 中受编译旗控制的注入；`qemutests/test_qemu_isolation.py`、`test_suite_catalog.py` 按既有枚举约束更新。

**Interfaces:** 新选择器 `X86_SMP_BOOT_TEST=none|delay-online|no-online|no-scheduler`（默认空/none）；映射 `boot-lifecycle-<case>` kernel/image variant 与 `X86_SMP_BOOT_TEST_DELAY_ONLINE/NO_ONLINE/NO_SCHEDULER` 宏。新增 suite `make PROFILE=x86_64-clang test-qemu SUITE=boot-lifecycle`，目标自建/运行矩阵。runner CLI `--case normal|delay-online|no-online|no-scheduler --cpus N --firmware PATH --image PATH --kernel-elf PATH --build-dir PATH --profile x86_64-clang --qemu PATH --timeout 90`；内部 `check_evidence(text: str, case: str, cpus: int) -> tuple[bool, list[str]]` 是纯 parser，process 边界可注入。

- [ ] **Step 1 — RED parser/process/build tests。** 新 Python module 覆盖 normal N=1 无 AP行，N>1 AP集合精确 1..N-1，各 ready 唯一且按启动序，complete 唯一且在 ready 后；delay身份/注入记录；no-online 要 stage=online，no-scheduler 要 stage=scheduler；缺少/重复/错 CPU/截断标记、错误 case、fatal 后 ready/complete/PID1/后续 CPU启动、异常/无关 panic、timeout、silent、进程自行退出全失败。FakeSession 验证 complete fatal 后 observation≥1s、runner 主动停止、原始输出/result归档。build tests 覆盖错误 selector、aarch64拒绝、与 selftest/systest/nettest/canary/capacity/driver/resolution/ARM fault 混用拒绝、隔离路径与子 make 传播；normal ELF 无三种注入 marker/宏选择符符号。
- [ ] **Step 2 — 确认 RED。** `python3 -m unittest qemutests.test_boot_lifecycle -v`；先让模块/接线可载入，再保留具体 parser 行为与 make-contract 失败，不能只记录 ImportError。将测试模块加入 TEST_HARNESS_MODULES；QEMU suite 未实现时不得靠 timeout PASS。
- [ ] **Step 3 — 实现接线与注入。** project.mk 校验 selector，加入受控子 make 允许列表并生成宏；双 profile 从有效宏解析 selector（包括直接 KERNEL_EXTRA_CFLAGS），x86 选择独立 KERNEL_VARIANT/IMAGE_VARIANT，aarch64拒绝。kernel/Makefile 正常构建不得编入注入逻辑，故障镜像强制 KERNEL_SELFTEST=0且拒绝其他变体。CPU1：delay 在 online 前 log 一次并 pause 100000；no-online 在发布前一直 pause；no-scheduler 在 IPI 发布后、scheduler 发布前一直 pause。fault identity 行 `[boot-1] variant=boot-lifecycle-<case>`，delay 行 `[boot-1] delay cpu=1 spins=100000`；normal 不输出这些行。保留 production ready/complete/fatal 格式。normal 用普通镜像只读/snapshot，fault用隔离镜像；runner依照 spec观察窗口和归档，不依据 QMP与串口跨通道先后推论时序。
- [ ] **Step 4 — GREEN 与矩阵。** 根 suite 一次串行跑 normal 1/2/4/8、delay 2/4、no-online 2、no-scheduler 2（8组）；每组 timeout=90s，fatal阶段观察1s，正常须 complete 后见 PID1 和 shell提示符。注入变体构建前后普通镜像 hash 不变（先构建 normal 再取 hash）；ELF审计 fault身份与普通构建隔离。运行 Python module、`make PROFILE=x86_64-clang test-harness`、`python3 qemutests/test_catalog.py --check`、`python3 qemutests/scenario_catalog.py --check`。catalog 新条目 boot-lifecycle，nightly/rootfs/isolated/exclusive/1,2,4,8/512MiB/单case90s；CI nightly直接调用整个8组目标，不能把整个矩阵误限为90s，job预算至少20分钟。场景目录每个normal/delay/fatal行为有primary证据，更新suite白名单、help、prepare/run元数据。
- [ ] **Step 5 — Commit。** `test(smp): validate x86 CPU lifecycle with isolated boot faults`。

## Task 4: 固化 ARM runtime online / ACK / admission 区别

**Files:** ARM smp/probe观测、`qemutests/aarch64_uefi_smp.py`、`test_aarch64_harness.py`、`test_aarch64_ap_boot_contract.py`、已有 `test_double_state_publish.c`；必要的 `hosttests/mk/sched.mk` 聚合和 scenarios。

**Interfaces:** 不改变已有 `boot_online_set/get`、`ipi_ready_publish_and_count()`、`aarch64_smp_accepted_mask()`、`aarch64_ap_sched_released()`。NO_ACK 模式在 BSP probe 已成功、accepted 集合冻结后输出一次 `[boot-1] a64-no-ack cpu=1 online=1 ipi_ready=1 scheduler_ok=0 accepted=0`，所有值由 acquire 或现有 accepted API 读取，并在输出前按此断言；只在已有 CPU1 NO_ACK 注入编译条件生效。输出点为 ARM `arch_boot_aps()` 的 shootdown probe 之后，尚未进入 scheduler handoff，不能从 AP 早期串口输出推导最终 admission。

- [ ] **Step 1 — 补负例。** NO_ACK parser 现有 DEGRADED证据之外，要求恰好一条上述runtime资格记录，且在唯一完整 `SHOOTDOWN-PROBE: OK` 之后；缺失、online=0、ipi_ready=0、scheduler_ok=1、accepted=1、重复、错误CPU、对应字段截断均拒绝。正常模式不要求该行，出现则拒绝错镜像。源码契约验证 ARM AP 显式发布点、构造-before-runtime-reader 的 probe gate、scheduler release只用冻结accepted集合；TLB runtime资格行为由Task2真实对象测试负责。
- [ ] **Step 2 — 确认 RED。** 运行 `python3 -m unittest qemutests.test_aarch64_harness qemutests.test_aarch64_ap_boot_contract -v`、focused host；新增NO_ACK缺证据用例失败必须来自判定不符合，而不是修改已有未相关用例。
- [ ] **Step 3 — 加最小观测。** ARM BSP 在多核probe返回后读取CPU1 runtime/admission状态，符合注入条件才通过log_info输出记录；其余模式零额外路径。维持现有生产probe和IPI count gate，不将gate替换为accepted count；保留AP work loop和NO_ACK永久不接纳。更新selftest/source contracts对在线显式发布的预期，不修改PSCI/cache clean/boot slot布局。
- [ ] **Step 4 — GREEN 与真实运行。** 跑 `make PROFILE=aarch64-clang test-aarch64 MODE=smp`（现有1/2/4×3）、`make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 KERNEL_SELFTEST=0 aarch64-uefi` 后 `make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 KERNEL_SELFTEST=0 test-aarch64 MODE=no-ack`；再以 NO_ACK=0 明确恢复正常构建，跑 m3-probe/sched-ap/sched-wake/sched-tick。记录 NO_ACK AP 的真实shootdown参与及未接纳证据；正常/故障固件DTB遵循现有harness，不手工替换普通产物路径。重跑两Python模块及focused host。
- [ ] **Step 5 — Commit。** `test(aarch64): pin runtime IPI readiness outside boot admission`。

## Task 5: 完整验收、文档与关闭条件

**Files:** 文档证据行、roadmap、测试登记和CI结果。仅调整实现实际影响到的文档；不顺带改其它架构规划。

**Interfaces:** 消费前四任务生产接口/runner；产出验证报告，记录原始基准、最终commit、每条命令/返回码、result/serial路径及未覆盖范围。

- [ ] **Step 1 — 检查覆盖清单。** 逐项映射spec §4不变量、§5发布点、§6失败策略、§8矩阵到生产代码和证据。确认原始构造RED已保存，超时不回收暴露资源，TSC可选超时不冒充启动完成，所有新目标进catalog/scenarios/CI；缺项补入所属任务而非在报告中略过。
- [ ] **Step 2 — 执行最终门禁。** 在最终文件状态运行 `make PROFILE=x86_64-clang test-host`、`test-static`、`test-harness`、`test-qemu SUITE=boot-lifecycle`；`QEMU_SMP=1`与`2`分别跑phase-0，`make PROFILE=x86_64-clang test-qemu SUITE=network`；`make PROFILE=x86_64-clang OS01_SYSTEST=1 KERNEL_SELFTEST=0 test-syscall`；单独 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`。ARM重复Task4全部模式。test-static包括x86 layout，ARM构建保留共同layout静态断言。任何阻塞记录具体环境，不以缺工具/超时作为PASS。
- [ ] **Step 3 — 写验收报告和契约文档。** 记录CPU身份/字段发布点、ARM probe gate依赖、x86超时停止策略和资源保留、num_cpus含义、故障镜像隔离及8组矩阵。更新SMP/arch/build文档和subsys perCPU说明的相关交叉引用；BOOT-2 IRQ问题仍未解决，不能写成整体启动安全已证明。
- [ ] **Step 4 — 检查差异与文档链接。** `git diff --check`，核对新增文件均被追踪/登记、链接存在和验证证据与最终实现一致。仅当以上真实门禁完成才把roadmap BOOT-1移入changelog并标明BOOT-2仍待实施；部分通过则保持进行中，明确剩余工作。
- [ ] **Step 5 — Commit。** `docs(smp): record BOOT-1 lifecycle contracts and validation`。执行者交付验证报告与各任务commit，不自动合入master或开始BOOT-2。

## 文档评审与实施边界

本次请求只要求编写并通过子 agent 评审 spec/plan。获批后停止；不执行上述checkbox、不修改生产代码、不声称QEMU通过。实际实施时再读取已批准版本和review记录，并由用户选择/授权实施方式。
