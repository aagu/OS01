# BOOT-4 Stage Contracts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 双架构公共启动边界可核验，注册阶段和同阶段依赖明确，CPU准备API准确表达执行位置。

**Architecture:** 拆开controller/context/device/SMP/runtime边界，保留架构内部IRQ与admission协议；固定表通过有界preflight和稳定扫描满足依赖。checker读真实对象，不另建ready位；删除LAPIC空callback，旧percpu API兼容映射为BSP CPU准备。

**Tech Stack:** freestanding C、Clang/LLVM、x86_64/AArch64、固定注册表、C hosttests、Python unittest、QEMU/UEFI及现有make profile/variant harness。

**Spec:** [BOOT-4设计 R2](../specs/2026-11-08-boot-4-stage-contracts-design.md)，`boot4_spec_review` 已 APPROVE。基线 `eebecfbf`，实施前重核对HEAD。本文R2，独立 reviewer `boot4_plan_review` 已 APPROVE；见[评审记录](../reports/2026-11-08-boot-4-doc-review.md)。本轮仅文档，checkbox均为未来工作。

## Global Constraints

- BSP表64、CPU prepare表16、每项依赖最多8；零依赖API兼容，元数据错误即使OPTIONAL也fatal。
- 命名phase值CONTROLLERS=3/TIME=4/DEVICE_IRQ=5/STORAGE=6；数字别名保留，整体boot步骤不复用这些编号。
- 不改boot_context/UAPI/percpu/task/pt_regs布局、CPU admission/资格发布、BOOT-2 RTC/PIT/LAPIC硬件偏序。
- x861/1与ARM0/0为本项产品配置；保留USERSPACE⇒VFS编译约束，不增加ARM用户态支持。
- 不在checker修复状态/放IRQ/启动timer；ARM软件phase提前但CNTP/IRQ/IPI/shootdown仍在AP收束后；softirq_init先于timer_init且各一次。
- root/TTY/PTY/stdio/PID1必需失败仍按BOOT-3停止，task_init/服务拆分与VFS bootstrap退出留BOOT-5。
- error沿用BOOT-3五参数stop/warn，状态0/1/-1/未知-999，环码-ELOOP=40，不扩张errno镜像。
- operational日志用log_*；ARM不使用不支持的signed formatter，fatal交BOOT-3处理。
- 新增源/头位置对称；新增架构source纳入各profile；删除文件检查显式与自动发现两条构建路径。
- KERNEL_SELFTEST关闭传空；systest与kernel-selftest分开；所有测试构建从根make入口，产物隔离。

## Review Focus

1. PIC读取尚未初始化的IOAPIC、fault dump先于设备probe（Task1逆序生产wrapper测试）。
2. OPTIONAL生产者失败使required消费者误跑；元数据环上游被误报环成员（Task1失败与实际环成员测试）。
3. BSP GS/TPIDR无效时checker先解引用this_cpu，或把inactive clocksource当失败（Task3真实checker负例）。
4. ARM softirq重置抹掉timer action；提早BSP IRQ或修改smp_starting/admission（Task3实际timer对象与双架构轨迹）。
5. normal混入fixture、fatal靠超时通过、ARMrunner错用x86镜像（Task4 artifact审计与parser负例）。

## 文件及接口归属

| 任务 | 创建/修改/删除 |
|---|---|
| 1 | 修改 `kernel/subsys/subsys.c`、`kernel/include/subsys/subsys.h`；依赖wrapper：`kernel/arch/x86_64/intr/pic_8259a.c`、`kernel/driver/pit.c`、`kernel/arch/x86_64/intr/apic/lapic_timer.c`、`kernel/arch/thermal.c`、`kernel/device/test_fault.c`；新增 `hosttests/cases/test_boot_contract_subsys.c`；hosttests/Makefile、mk/components/run.mk |
| 2 | 修改subsys源/头、`kernel/arch/x86_64/smp/boot.c`、`kernel/include/arch/subsys.h`；删除 `kernel/arch/x86_64/smp/subsys_percpu.c`；新增 `hosttests/cases/test_boot_contract_cpu_prepare.c`；清理kernel/Makefile（若显式列出）及测试mock |
| 3 | 修改 `kernel/core/main.c`、`kernel/include/arch/boot.h`、`kernel/arch/x86_64/platform/boot.c`、x86 smp/boot.c及两个boot头、`kernel/arch/aarch64/boot/main.c`；新增 `kernel/arch/{x86_64,aarch64}/platform/boot_contract.c`（接口统一声明于已有 `kernel/include/arch/boot.h`）；getter修改原模块及对称公开头；新增 `hosttests/cases/test_boot_contract_{sequence,checks,softirq}.c`、硬件边界mock；kernel/Makefile、hosttests/Makefile、qemutests/test_boot_facade_headers.py |
| 4 | 新增 `kernel/selftest/boot_contract_fixture.c` ↔ `kernel/include/selftest/boot_contract_fixture.h`、`qemutests/boot_contract.py`、`qemutests/test_boot_contract.py`；修改双架构controller注册接线、kernel/Makefile、mk/profiles/{x86_64-clang,aarch64-clang}.mk、mk/project.mk、mk/components/{kernel,image,run}.mk、mk/tests/catalog.mk、qemutests/scenario_catalog.py及对应catalog/dryrun/隔离测试 |
| 5 | 修改 `docs/architecture.md`、`docs/arch.md`、`docs/subsys/subsys.md`、`docs/smp/smp.md`、`docs/build/build.md`、`docs/roadmap.md`、`AGENTS.md`；新增未来验收报告 `docs/superpowers/reports/2026-11-08-boot-4-validation.md` |

新增focused入口 `make test-boot-contract-host CASE=subsys|cpu-prepare|sequence|checks|softirq|all`（默认all）。五个binary独立依赖，不因后续尚不存在的文件而阻断前项RED。统一加入test-host；Python负向测试加入test-harness。错误CASE解析失败。

## Task 1: 命名阶段与固定表依赖执行

**Interfaces:** 新增 `int register_subsys_deps(const char *name, int (*init)(void), int phase, uint32_t flags, const char *const *requires, unsigned require_count)`；旧register_subsys→零依赖wrapper；subsys_entry内部追加静态生命周期requires/count。旧dispatch/query签名不变；新增phase命名及旧值别名。

- [ ] **Step 1 — 行为RED。** 新host binary编译真实subsys.c与生产wrapper（硬件init替身，注册宏可导出wrapper），以`pic,apic`逆序记录旧错误顺序；旧零依赖phase重复调用/BOOT-3首错保持GREEN。新增API先用可编译声明/最小adapter记录行为RED，不能把缺符号当依赖排序的证明。断言包括：consumer先注册producer后注册实际producer先执行；chain长度64；独立ready项不被前项等待阻塞；earlier phase未跑即-EAGAIN不执行consumer；OPTIONAL producer=-EIO，required consumer不跑并fatal-ENODEV，optional consumer=-1/raw=-ENODEV且独立项继续。
- [ ] **Step 2 — preflight RED。** 运行 `make test-boot-contract-host CASE=subsys`；分别新进程或subsys_test_reset隔离缺依赖(-ENOENT)、future phase(-EINVAL)、自依赖/重复依赖/count9/NULL与空名(-EINVAL)、count8合法、环(-ELOOP)、非法dispatch(-EINVAL)。验证所有注册元数据失败在任一真实callback前停止、first latch优先、64/65容量界、旧status=-999不变。专门注册A→B、B→C、C→B，诊断B而不是A；再加多个环验证注册序最早的实际环成员。
- [ ] **Step 3 — 实现。** 注册先完整验证参数再写表，坏输入latch；首次合法dispatch seal后preflight索引解析所有依赖，缺失/future确定按consumer注册序首错。用固定64节点有界图可达性检测：为每个节点判断从自身出发能否回到自身（静态/栈有界visited[64]与worklist[64]，无递归栈、无callback），首个满足者就是最早实际环成员。按phase执行稳定多轮ready扫描，failed依赖先于pending判断，最多64轮；存raw/status并保留BOOT-3callback后latch屏障。五条生产边精确按spec§5声明为static const数组，旧API无依赖不改变语义；blocked日志含consumer及首次失败dependency。
- [ ] **Step 4 — GREEN。** focused subsys、`make test-boot-failure-host CASE=subsys`、test-host和双profile构建，逆序wrapper测试必须观察PIC在APIC成功后查询IOAPIC，fault dump在device-boot成功后执行。区别preflight环的图检查成本（≤64次节点搜索，8边/节点）与runtime就绪扫描≤4096次；不声称整个算法只有4096步。重复phase不重跑任何条目；no-progress为注册完整性fatal。确认 .subsys_init KEEP/link段只负责收集，非顺序依据。
- [ ] **Step 5 — Commit。** `feat(subsys): name boot phases and enforce bounded dependencies`。

## Task 2: BSP CPU数据准备语义与空callback删除

**Interfaces:** 新 `int register_subsys_cpu_prepare(const char *name, int (*prepare_for_cpu)(int target_cpu), uint32_t flags)`、`void subsys_prepare_cpus(void)`、`int subsys_cpu_prepare_result(const char *name, uint64_t *failed_mask)`；旧三API为同表wrapper。内部entry/type改cpu_prepare；本项无on_cpu硬件执行API。

- [ ] **Step 1 — RED。** 真实subsys对象链接cpu_id/num_cpus边界，回调记录executor=0/target=0..3，令target2 offline仍应被调用；required失败立即停止，optional失败继续并保留首rc/failed_mask。原日志不得包含executor=target或lapic启动成功伪证据。注册旧API、执行新API、查询旧API及反向组合；再调旧dispatch不重跑。16/17容量、late-register seal、同BSP表名字namespace、failed_mask NULL和unknown/pending mask=0测试。
- [ ] **Step 2 — 验证RED。** `make test-boot-contract-host CASE=cpu-prepare`，新API缺符号与实质executor/log测试分开。加ELF/源码合同检查产品不得含`_lapic_timer_start_percpu`、`lapic-timer-start`或`arch_register_subsys_percpu`，旧产品应因此RED。核对所有caller和host mocks，不能仅删除源文件使自动发现构建侥幸通过。
- [ ] **Step 3 — 实现。** 新旧接口同一表/一套seal/latch/结果，BSP唯一dispatch调用点留在AP尝试收束后；范围按num_cpus而不是online/accepted筛选，禁止回调承诺本核硬件执行。删除空callback文件及其声明/注册调用；x86 aps保留generic `subsys_prepare_cpus()`。头文件明确deprecated wrapper和BSP executor；log格式 `cpu-prepare %s target=%u executor=bsp`。不改BSP/AP计时硬件与accounting发布。
- [ ] **Step 4 — GREEN。** focused cpu-prepare、BOOT-3 subsys host、test-host，x86/ARM链接及符号审计。确认table容量16、name查询BSP优先、optional失败mask等旧契约保留；generic表为空时无虚假成功日志，不因无产品注册项而删除错误检查或测试。
- [ ] **Step 5 — Commit。** `refactor(subsys): distinguish BSP CPU preparation from local init`。

## Task 3: 公共钩子重排与真实能力核验

**Interfaces:** 在arch/boot.h定义 `enum arch_boot_checkpoint` 三值DEVICES/PRE_SMP/RUNTIME（前缀ARCH_BOOT_CHECK_）；新 `void arch_boot_controllers(const struct boot_context *)`、`void arch_boot_devices(const struct boot_context *)`、`void arch_boot_smp_prepare(void)`、`void arch_boot_runtime(void)`、`int arch_boot_validate(enum arch_boot_checkpoint, const char **component)`。删除旧public/private subsystems入口，拆分x86 helper命名为 `x86_64_boot_controllers/devices`，保留percpu与aps public。

新增只读getter精确冻结如下（在原模块实现，对称头声明，不写新ready状态）：

| 签名 | 源/头 | 真实状态与边界 |
|---|---|---|
| `bool sched_accounting_is_ready(void)` | sched/accounting.c ↔ include/sched/accounting.h | acquire既有accounting_ready，不以scheduler_ok推导 |
| `bool pit_started(void)` | driver/pit.c ↔ include/driver/pit.h | 读取既有started flag，只证明曾启动，单独不能证明route enabled |
| `bool ioapic_irq_unmasked(uint32_t gsi)` | arch/x86_64/intr/apic/ioapic.c ↔ include/intr/apic.h | published ready、已分配route、idx/pin合法后在既有controller锁下读RTE MASK=0；未知false，不写RTE |
| `bool pic_irq_unmasked(unsigned isa_irq)` | arch/x86_64/intr/pic_8259a.c ↔ include/intr/pic.h | <16、IMR对应位=0；slave还检查master IRQ2=0；只读IMR |
| `bool lapic_timer_local_running(void)` | arch/x86_64/intr/apic/lapic_timer.c ↔ include/intr/apic.h | apic_available先于MMIO；LVT未mask、periodic、正确vector、INIT>0；不读瞬时CURRENT来判停 |
| `bool aarch64_timer_config_ready(void)` | arch/aarch64/platform/time.c ↔ include/arch/aarch64/timer.h | acquire既有g_timer_config.ready，配合现有local_running使用 |

x86 checker组合PITstarted+实际selected controller的ISA0 route与LAPIC getter，PRE_SMP/RUNTIME要求两者恰一可递送，不将PIT曾启动flag当作仍unmasked。DEVICES只要求BOOT-2 runtime_opened，不要求尚未进行的LAPIC handoff。新getter及checker的硬件read/privileged primitive为host mock边界，算法和条件编译真实源码。

- [ ] **Step 1 — RED轨迹/对象。** sequence binary编译真实main.c（重命名kernel_main为受测入口，关闭canary fault，外部hooks/FS/task/idle为追踪stub），分别feature1/1和0/0，断言spec§3全序及FS/PTTY失败不进入SMP；末端用noreturn测试陷阱退出。softirq binary链接真实intr/softirq.c+time/timer.c，初始先timer_init再softirq_init复现action丢失；GREEN目标为init顺序正确且只执行一次。header跨target测试新hook声明/旧hook移除，invalid1/0编译FAIL，正常profile头编译PASS。
- [ ] **Step 2 — RED checker与getter。** `make test-boot-contract-host CASE=checks`，双架构boot_contract.c编为独立host对象，模拟base/IF或DAIF/MMIO/port/acquire边界。测试base=0/错误地址时不得调用this_cpu；offline/self/cpu_id/num_cpus失效，timer status=0/-1、TIMER_SIRQ为空、device_result错误、accounting未ready、两tick都停/都开、PITonly、LAPIConly、RTCinactive合法；ARM config未发布/local未运行/IRQmasked/IPI未ready失败且不得修改硬件/资格。getter本身另链接真实实现、mock最低层MMIO读（含IOAPIC selector/data与锁）、IMR和CNTP寄存器，不把getter整体stub视为硬件逻辑覆盖。
- [ ] **Step 3 — 实现。** 按spec表逐段搬移：controller注册/phase3；percpu constructor+context_prepare；devices phase4/校准/runtime_open/phase5/6；smp_prepare accounting+tick；aps仅AP与CPUprepare；ARM runtime承接原marker/tick/IRQ/IPI/probe。原x86unsafe-open注入调用仍在context_prepare前（移到controller尾端），BOOT-2设备begin仍在phase5前；所有旧fixture相对失败边界不变。ARM softirq_init移动到devices首部且删旧post-phase调用。main每个checker非0调用BOOT-3 stop，0输出checkpoint。所有实际checker独立放各arch platform/boot_contract.c，公共声明用现有arch/boot.h，无旁置公开头；ARM显式源码白名单加入新文件。安全base验证x86先rdmsr(IA32_GS_BASE)==&percpu_data[0]，ARM先读TPIDR_EL1比地址，然后才this_cpu；这些privileged读通过mock header/窄OS01_HOST_TEST seam替代，产品无可写状态注入。所有返回error component为静态串，未知checkpoint=-EINVAL/component=checkpoint。
- [ ] **Step 4 — GREEN。** 五CASE与test-host、test-static、test-harness；构建默认x86/ARM，nm证明每个public hook恰一strong实现、旧subsystems符号不在产品、ARM0/0无arch_boot_device_nodes。审查x86phase3→context→phase4→calibrate一次→runtime_open→phase5/6及tick_start→AP原偏序，ARM软件提前而硬件/IRQ未提前。ARM TIMER_SIRQ真实do_timer仍在，主序列不重置softirq。检查所有probe/selftest旧marker保留，首次正常QEMU x861/2及ARM1/2冒烟，再进入完整矩阵。
- [ ] **Step 5 — Commit。** `refactor(boot): enforce shared stage contracts across architectures`。

## Task 4: 双架构14组编排矩阵

**Interfaces:** `BOOT_CONTRACT_TEST=none|reverse|missing|cycle`；`void boot4_test_register(void)`仅variant编译，双架构controller内arch_register_subsys之后、首phase dispatch之前调用（BOOT-3 registration fixture仍保留原位置）。普通header inline空宏不发日志、不链接fixture。runner CLI `--arch x86_64|aarch64 --case normal|reverse|missing|cycle --cpus N --image PATH --firmware PATH --kernel-elf PATH --log-dir PATH`；纯函数 `parse_serial(text, arch, case, cpus)`返回结构化结果，QEMU启动/归档与解析分离。

- [ ] **Step 1 — RED parser。** 新 `qemutests/test_boot_contract.py` 构造两架构完整成功串/错误串，断言normal无fixture、reverse producer先consumer且status1、checkpoint各一次全序；missing唯一register/-ENOENT、cycle唯一register/-ELOOP(40)且component首环成员，禁止任一phase3 callback/context/checkpoint/AP/PID1。负例覆盖wrong arch/variant/ELF machine、缺字段/重复/乱序/未知case、fatal后成功marker、120秒期限、成功前QEMU自退、排空窗口迟到fatal、空串。运行 `python3 -m unittest qemutests.test_boot_contract`，先记录接口stub可加载后的行为失败。
- [ ] **Step 2 — RED build契约。** 扩展catalog/scenario/dryrun/variant/header测试：两入口恰7组（normal1/2/4、reverse1/2、missing1、cycle1），make只传显式路径；根make清空MAKEOVERRIDES，测试必须检查OS01_SUBMAKE_ARGS白名单通道最终传到双架构kernel/image子make并生成正确编译宏，reject交叉profile/BOOT1/2/3/其它fault/systest/nettest/selftest混用，普通ELF不得含`boot4_test_register`和fixture身份；variant必须含。none正常，未知selector解析期失败。独立focused入口应在其它CASE尚不存在时仍能运行。不要给normal强加variant注入身份。
- [ ] **Step 3 — 实现。** fixture静态注册consumer(`boot4-consumer`)再producer(`boot4-producer`)，reverse为phase5 required，consumer依赖producer并在真实callback检查subsys_require_ready==0；打印 `[boot-4-test] callback=producer`、`callback=consumer dependency_status=1`。missing为consumer requires `boot4-absent`；cycle为consumer↔producer。共同先打印 `[boot-4-test] variant=boot-contract-<case>`，registration任一返回错误交真实latch。普通入口完全排除fixture。在mk/project.mk归一化BOOT_CONTRACT_TEST、none为空variant、未知值/顶层selector与直传CFLAGS冲突解析期拒绝，补入OS01_SUBMAKE_ALLOWED/ARGS白名单并生成IMAGE_VARIANT；同步互斥选择器与两profile的variant推导，ARM image.mk/两arch kernel.mk路径均走显式子make通道。两profile新增三种隔离kernel/image variants，kernel/Makefile将规范case翻译成固定宏（例如BOOT_CONTRACT_CASE_REVERSE），复用现有UEFI/image构建，不让Python隐式build。normal snapshot/hash守恒；故障与reverse显式case/hash身份。更新SUITE/MODE允许表、help/catalog/scenario与CI选择器合同；14组内串行执行，跨suite按现有exclusive规则锁镜像。
- [ ] **Step 4 — GREEN。** `make test-harness`、catalog/scenario与选择器dryrun全通过后运行 `make PROFILE=x86_64-clang test-qemu SUITE=boot-contract` 和 `make PROFILE=aarch64-clang test-aarch64 MODE=boot-contract`，各7组。runner在完整成功链或fatal后继续读≥1秒，120秒绝对期限并在结束前主动终止QEMU；超时/崩溃/自退均FAIL。缺依赖/环必须经真实preflight，不直接调用fatal；正常ARM需持续生产tick与idle，x86需PID1及shell。归档原串口、JSON、命令、CPU、artifact哈希；普通image前后hash一致。
- [ ] **Step 5 — Commit。** `test(boot): validate cross-architecture stage and dependency contracts`。

## Task 5: 整体回归与公开契约同步

**Interfaces:** 不新增运行API；交付验证报告逐项区分新矩阵、旧回归、未运行项/阻塞，roadmap只有取得实际证据后才标已验收。

- [ ] **Step 1 — 建立验收清单。** 从spec§3～8逐项对应Task1～4测试；用HEAD与构建profile记录基准，保留现有BOOT-3验证记录，不擅自改历史报告事实。确认当前test bucket规范，禁止用已删除alias替代bucket。
- [ ] **Step 2 — 执行通用/x86门禁。** `make test-host`、`make test-static`、`make test-harness`；`make test-qemu SUITE=boot-lifecycle`（8组）、`SUITE=boot-irq-time`（10组）、`SUITE=boot-failure`（17组）；`SUITE=phase-0`、`SUITE=inittab-phase`、`SUITE=network`；`make OS01_SYSTEST=1 test-qemu SUITE=systest`；另用 `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` 运行8核。每项rc=0/日志符合现有runner才PASS；失败先修再重跑受影响项。
- [ ] **Step 3 — 执行ARM门禁。** `make PROFILE=aarch64-clang test-aarch64 MODE=smp`、MODE=no-ack/m3-probe/sched-ap/sched-wake/sched-tick，及 `make PROFILE=aarch64-clang test-aarch64-boot-logo`。各MODE沿用原矩阵和selector，分开调用不混BOOT4fixture；检查no-ack online/ipi_ready与accepted分离、BSP gate按原时刻退出、每核CNTP不重启、旧tick/clocksource/GIC协议不因搬移破坏。
- [ ] **Step 4 — 更新文档并自审。** architecture/arch/subsys/smp写新公共顺序、四命名phase/有限requires/failed依赖策略、prepare executor=BSP与本核硬件归属、feature门不适用能力；build文档登记新focused入口/14组矩阵/隔离/证据规则；AGENTS keyfiles/Init/Facade同步，roadmap记录实际结果及BOOT5边界。validation报告附命令/rc/产物身份与限制。运行git diff --check、本地Markdown链接检查；复核无ABI/资格发布变化且无无关文件。
- [ ] **Step 5 — Commit。** `docs(boot): record BOOT-4 stage contracts and validation`。

## 计划自审

覆盖：spec§3公共边界与特性门→Task3；§4能力检查→Task3；§5依赖/阶段→Task1；§6执行位置→Task2；§7文件/文档→各任务及Task5；§8 host/矩阵/回归→Task1～5。5个任务、25个步骤；先行为RED再实现，首次无接口的编译失败独立记录。文档批准不表示以上代码、测试或验收已完成。
