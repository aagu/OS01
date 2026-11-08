# BOOT-4 阶段契约与跨架构编排设计

日期：2026-11-08；版本 R2；独立子 agent `boot4_spec_review` 已 APPROVE。仅设计，未实施。见[评审记录](../reports/2026-11-08-boot-4-doc-review.md)。

## 1. 目的、基线与边界

基线 `eebecfbf`，BOOT-1～3 已有实现。以当前源码为准：BOOT-3 验证报告中的若干简写（例如 `subsys_status` 未知值、fatal 函数参数）并不等同真实接口，本文保留头文件中的未知值 `-999` 和五参数 fatal API（rc 为 int64_t）。

目标是让公共 boot 边界具有双架构一致的后置能力，让现有同阶段依赖不受 initcall 链接顺序影响，并准确区分 BSP 为 CPU 准备数据与目标 CPU 初始化硬件。BOOT-5 仍负责拆分 task_init、服务启动和 VFS bootstrap 退出。

不改 boot_context/UAPI、percpu_t/task/pt_regs 布局、CPU admission、online/ipi_ready/scheduler_ok 发布者、IRQ 入口和 BOOT-2 RTC/PIT/LAPIC 时序。不并行启动，不增加设备重试，不把 kernel_main 全部装入子系统注册表，不做跨架构 AP 底层 API 统一。

## 2. 现状及方案裁决

- x86 `arch_boot_subsystems()` 执行注册、phase 3、BSP context、phase 4、校准、runtime-open、phase 5/6；其后的 `arch_boot_percpu()` 通常仅检查。ARM 同名 subsystems 只做 DTB/GIC，percpu 真正构造 BSP，phase 4/softirq/timer/IRQ 则藏在 aps 内。
- ARM 当前先 timer_init 注册 TIMER_SIRQ，再 softirq_init 清空向量。移动软件设施时必须一起修正这个顺序，不能把“阶段完成”标志建立在被清空的 handler 上。
- PIC 与 APIC 同 phase；PIC wrapper 使用 IOAPIC 结果，必须有 `apic → pic`。phase-6 故障 dump 必须在 `device-boot` 后，不能靠链接顺序。
- 唯一生产 per-CPU 子系统 `lapic-timer-start` 在 BSP 上返回 0，硬件及 accounting 实际由 BSP tick_start/AP 入口完成，成功日志没有对应工作。

采用“显式公共边界 + 固定表内有界依赖调度”。仅写注释/require_ready 无法解决逆序注册的同阶段生产者；完整通用 DAG、动态分配注册表和远程回调 RPC 超出现有需求。保留 BSP 64、CPU prepare 16 容量：当前远低于上限，BOOT-3 已有容量失败策略，暂无取消固定表的依据。依赖调度只作用于四个子系统阶段，不扩张为全系统 DAG。

## 3. 公共启动序列与钩子契约

替换 public `arch_boot_subsystems` 为 `arch_boot_controllers(bootctx)` 与 `arch_boot_devices(bootctx)`；保留 `arch_boot_percpu()` 但现在双架构都在 controllers 之后立即构造一次；增加 `arch_boot_smp_prepare()`、`arch_boot_runtime()`。均为 strong override，缺失由链接器报错。无 weak 空成功。

公共顺序固定为：early → memory → pre-subsys-selftest → controllers → percpu → devices → random → gated FS/TTY → smp-prepare → aps → runtime → post-smp-selftest → gated task_init / kernel-idle。栈 canary/handoff 校验仍最先；既有 x86 intr fault probe 相对 random/FS 的位置不变。

| 公共边界 | 前置 | 返回后保证 | x86 行为 | ARM 行为 |
|---|---|---|---|---|
| controllers | memory/pre-selftest 完成，BSP IRQ masked | 拓扑来源及 IRQ controller 已准备；所有 BSP initcall 注册及依赖元数据已验证；CONTROLLERS phase 完成 | 设置 RSDP、BOOT-3 fixture、register、phase 3 | DTB、GIC、既有 handoff marker、register、phase 3（空阶段合法） |
| percpu | controllers 完成 | BSP this_cpu/自指针/online 有效、拓扑数量确定；不表示 AP 已启动或有调度资格 | 原 phase3 后的 constructor 与 context_prepare 搬到这里，late facade 删除二次构造 fallback | 原构造、online、num_cpus、smp_starting_enter 原样 |
| devices | BSP context 已有效 | TIME、DEVICE_IRQ、STORAGE phases 全部完成；软件 timer/softirq 可用；硬件结果按 OPTIONAL 可降级 | phase4 → 校准一次 → BOOT-2 runtime_open → phase5/6 → device_result | softirq_init → phase4/5/6；不启动 CNTP，不放行 BSP IRQ |
| smp-prepare | FS/TTY（如启用）完成 | 本平台 AP 启动所需的 BSP 本地准备完成；不表示 AP 已响应 | accounting_init → tick_start，保留原先的 pre-AP 顺序 | 明确无额外准备；实现注明 N/A，不发布 timer-ready |
| aps | smp-prepare 完成 | AP 启动尝试已收束；资格/降级按 BOOT-1；不保证所有发现 CPU 均已录用 | smp_boot_aps；CPU prepare 若有实际注册项则在 BSP dispatch | smp_boot_aps + 原 spinlock 测试，保留 BSP IRQ masked |
| runtime | AP 启动尝试收束 | BSP tick 路径已启动、IRQ 可递送、BSP IPI 资格满足本架构协议；不表示 BSP scheduler_ok | 只核验既有硬件/资格状态，不重复 tick_start 或发布 | 原 clocksource marker → arch_tick_start → selftest SPI → IRQ enable/ISB → BSP IPI publish → shootdown/no-ACK observation |

ARM 的 phase4 软件初始化提前到 AP 启动前，但 CNTP 硬件、BSP IRQ 开放、IPI 发布及 shootdown 仍在 AP 启动后，AP 的 timer 仍在自身调度 handoff 启动。softirq_init 只调用一次且先于 timer_init，之后不得重置。x86 的 phase3→context→phase4→校准→runtime_open→phase5/6 的硬件时序逐项保持。common main 的 random_init 仍在 devices 后。

`arch_boot_device_nodes` 声明和 ARM 空实现受 `#if OS01_HAS_VFS` 一致约束；当前 ARM 默认 VFS=0 时不编译也不宣称完成节点能力。只承诺现有 x86 VFS=1/USERSPACE=1 与 ARM 0/0 产品配置；不新增 ARM VFS/userspace 支持。现有特性门的 0/1 与 USERSPACE⇒VFS 编译约束保留。

## 4. 可检查的能力边界

新增 `enum arch_boot_checkpoint { ARCH_BOOT_CHECK_DEVICES, ARCH_BOOT_CHECK_PRE_SMP, ARCH_BOOT_CHECK_RUNTIME };` 与 `int arch_boot_validate(enum arch_boot_checkpoint point, const char **component)`，双架构 strong 实现。在设备阶段之后、smp_prepare 之后、runtime 之后由 main 调用。返回 0 或负 errno；失败 component 为静态字符串（调用方默认 `boot-contract`，NULL 安全）。失败走 `boot_failure_stop("subsys", component, rc, -1, 0)`，不增加 BOOT-3 stage 枚举。

检查必须读取真实对象，不增加一套可漂移的 ready 位，不以成功日志作为输入：

- DEVICES：BSP 自指针/cpu_id=0/online acquire、1≤num_cpus≤NR_CPUS；`timer`、`clocksource` 执行成功；TIMER_SIRQ action 等于 do_timer。x86 额外 `apic` 成功、BOOT-2 runtime 状态，device_boot_result=0。clocksource init 成功不等于 active=true，RTC fallback 合法。
- PRE_SMP：延续 DEVICES；x86 核验 accounting_ready 和真实启用的 tick source（LAPIC 或 PIT fallback），ARM 不要求尚未启动的 CNTP。失败不得启动任一 AP。
- RUNTIME：BSP online/ipi_ready、IRQ enable 和 tick source 可用；ARM CNTP running + 已发布 timer config，x86 LAPIC/PIT 真实状态。ARM smp_starting gate 的结束仍由原调度 handoff负责；不得为检查而提前改 gate 或 scheduler_ok。

底层若无只读 getter，允许在其源/头对称位置增加窄 getter，不得为了本项公开内部可写状态。checker 本身不得开关 IRQ、重启 timer、发布资格或修复对象。x86 读取 IRQ flags 用 save/restore 保留状态。CPU-local checker 只在 BSP 调用，必须在读取 this_cpu 前有平台安全前置验证。

每个检查成功后输出一条 `[boot-4] checkpoint=<devices|pre-smp|runtime> status=ok`，用 log_info。仅表示该边界列明的能力；不包含 AP 数或调度保证。ARM 使用现有 formatter 支持的 %s/%u/%lu，错误码由 BOOT-3 helper 格式化。

## 5. 命名阶段与有限依赖

新增命名常量 `SUBSYS_PHASE_CONTROLLERS=3`、`SUBSYS_PHASE_TIME=4`、`SUBSYS_PHASE_DEVICE_IRQ=5`、`SUBSYS_PHASE_STORAGE=6`；旧 `SUBSYS_PHASE_3..6` 为值相同的兼容别名。所有生产注册/dispatch改用命名常量，测试可断言数字兼容。main 的整体步骤不与子系统四阶段共用数字枚举。

增加 `int register_subsys_deps(const char *name, int (*init)(void), int phase, uint32_t flags, const char *const *requires, unsigned require_count)`；旧 register_subsys 包装为零依赖。最多每项 8 个依赖，名字及数组须静态生命周期。0 count 允许NULL；非0必须非NULL；空名、重复依赖、自依赖、超过8均 -EINVAL，注册失败走 BOOT-3 首错 latch，不插入条目。

首次 phase dispatch 前 seal 并一次性验证完整 BSP 表：所有依赖必须存在、只能指向同/更早 phase，缺失 -ENOENT，跨向未来 -EINVAL；同阶段环 -ELOOP。完整验证失败（即使 entry OPTIONAL）都是元数据错误，`stage=register component=<consumer> rc=... phase=<consumer phase> cpu=-1`，不得执行任何子系统回调。不存在条件依赖；条件编译生产者时消费者的声明也必须对应裁剪。

执行每个 phase 用有界稳定扫描：就绪项按注册序；等待同阶段 pending 的项延后到下一次扫描；一个已就绪项不得为等待另一个未来就绪项而被阻塞。每项最多执行一次，最多64轮/4096次就绪判断；独立项顺序保持注册序，不承诺独立项的硬件顺序。preflight 已拒绝环；运行时无进展视为 -ELOOP 元数据致命错误。

依赖成功的定义为 producer initialized=1，不能用 raw rc=0 或存在性替代。更早 phase 尚未执行为调用顺序错误，消费者不执行、立即 fatal (`stage=subsys`, -EAGAIN)；不隐式运行较早 phase。依赖已失败时 consumer 不执行，required consumer fatal -ENODEV，OPTIONAL consumer initialized=-1/raw=-ENODEV 并 WARN，随后独立项继续。错误必须另有结构化日志 `[boot-4] blocked component=<consumer> dependency=<producer>`。被依赖项 OPTIONAL 并不让 required consumer 自动可选。

phase dispatch 只接受3..6；非法值 fatal -EINVAL，不得悄悄 seal 空阶段。重复合法 dispatch 幂等，不重跑失败或成功回调。subsys_init_all 按四个命名 phase 执行，不得由架构用它绕过 context/校准边界。BOOT-3 first-error、容量、状态/-999/raw 查询及必需失败策略不变；依赖元数据内部追加，不改任何跨边界 ABI。

冻结生产边：`pic requires apic`；`pit requires timer`；`lapic-timer requires timer`；`thermal requires clocksource`；ARCH-9 测试 `arch9-fault-dump requires device-boot`。其余跨phase消费者继续由命名阶段保障，不凭空声明硬件不存在时无法满足的依赖。LAPIC校准仍在整个 TIME phase结束后，不改成 callback 的同阶段依赖。

## 6. CPU 准备与本核执行

新增明确接口 `register_subsys_cpu_prepare(name, prepare_for_cpu, flags)`、`subsys_prepare_cpus()`、`subsys_cpu_prepare_result(name, failed_mask)`，语义为 BSP 串行遍历配置范围 `[0,num_cpus)`，为编号对应的 CPU 准备共享数据；不承诺该 CPU online/accepted，不访问目标 CPU 本地寄存器。参数、容量16、seal 时机、required/optional错误和 failed_mask 沿用 BOOT-3。回调执行点仍在 AP boot 后，不能用于 AP 启动前资源准备。

旧 `register_subsys_percpu`、`subsys_init_percpu`、`subsys_percpu_result` 保留为兼容 wrapper，并在头文件明确 deprecated/同一张表；不得复制两张表或跑两次。内部类型/字段改名为 cpu_prepare；status 查询仍 BSP名称优先，两个 namespace 可同名的旧行为保留。日志改为 `cpu-prepare <name> target=<id> executor=bsp`，不能声称本核硬件初始化成功。

删除 x86 `_lapic_timer_start_percpu` 空 callback、其注册项和只为它服务的 `arch_register_subsys_percpu`/文件/声明/调用；实际 LAPIC/PIT/CNTP启动仍归 BSP/AP原入口。保留generic prepare执行器供真实调用者和host测试，当前产品无项目则不输出虚假成功。不得顺便为目标核新增 RPC/跨架构 on_cpu init 框架。

## 7. 文件边界与兼容性

- `kernel/core/main.c`：只改编排序列/checkpoint，保留 canary、fault probes、特性门及 BOOT-3 FS/TTY/stdio/PID1路径。
- `kernel/include/arch/boot.h`、x86 `platform/boot.c`/`smp/boot.c`、ARM `boot/main.c`：新增/拆分strong hooks和验证；更新对应 per-arch boot头。ARM无需重写大段内存、自测或调度代码。
- `kernel/subsys/subsys.c` ↔ `kernel/include/subsys/subsys.h`：命名 phase、依赖验证/调度、cpu_prepare兼容API。优先保留现有文件；无新动态注册器。
- 注册wrapper文件按§5改依赖；删除 x86 `smp/subsys_percpu.c` 及 arch/subsys 相关孤立声明；`kernel/Makefile`清除已删除文件显式条目（若为自动发现则不加替代文件）。
- 只读getter放原模块源/头，例如 `sched/accounting.c` ↔ `include/sched/accounting.h`、`platform/boot_irq.c` ↔ 对称头；具体接口由plan冻结。
- docs/architecture.md、docs/arch.md、docs/subsys/subsys.md、docs/smp/smp.md、AGENTS.md架构简介同步。先检索实际文件内历史描述再局部修订，不改历史分析报告来假装它描述新实现。

## 8. 验证契约

### 8.1 Host 与编译

真实 subsys.c host 测试：逆序注册、同阶段链、跨阶段前置、独立项、缺依赖/未来phase/环/自环/重复/上限8与9、OPTIONAL生产者失败传播、optional consumer跳过、required consumer停止、重复dispatch、非法phase、BOOT-3 latch不被覆盖和不运行回调。CPU prepare测试断言executor永远BSP、离线target仍可被准备、failed_mask、旧新API互通及不重跑。名字空间和容量64/16回归。

真实 main 编译/调用轨迹host测试按x861/1、ARM0/0配置验证顺序，边界getter mock但checker生产实现另测，不能只测一份复制的序列。新旧hook符号链接审计：产品ELF只有公共hook的强实现、无arch_boot_subsystems/空LAPICprepare项。使用两个profile构建验证，USERSPACE=1/VFS=0编译必须失败；不强行构建不支持的ARMuserspace。

真实softirq+timer_init host 测试验证 ARM新顺序留下 TIMER_SIRQ=do_timer，并在第二次初始化探测时拒绝回归清空。可用源码合同测试固定callsite各一次，但不能代替对象状态断言。

### 8.2 QEMU与防假阳性

新增x86 `test-qemu SUITE=boot-contract` 与ARM `test-aarch64 MODE=boot-contract`，共享runner解析器，分别normal1/2/4核、reverse1/2核、missing1核、cycle1核，共每架构7组、合计14组。普通产品normal无注入，读取三条生产checkpoint及原生产成功链（x86 init-created/PID1/shell，ARM原SMP/持续tick/内核idle）。

隔离 `BOOT_CONTRACT_TEST=none|reverse|missing|cycle`，双方profile都支持，非none产物位于 `boot-contract-<case>` 独立variant；拒绝与BOOT1/2/3 fault、systest/nettest、其它fault、自测选择器混用。reverse测试仅注册两个必需phase5 fixture：先consumer后producer，consumer requires producer；回调输出顺序证据，consumer真实查询producer状态1才成功。missing/cycle在真实注册表加错元数据，走preflight真失败，禁止直接调用fatal模拟。每个fixture有variant与case身份，普通ELF不得含fixture符号/身份字符串。

normal/reverse三条checkpoint唯一且 devices→pre-smp→runtime；reverse另须producer→consumer且consumer查询成功，最终成功链完整。missing/cycle应唯一完整 BOOT-3 `stage=register component=<fixture consumer> rc=<-ENOENT|-ELOOP> phase=5 cpu=-1`，不得出现phase3callback、BSP context、checkpoint/AP启动/PID1证据。环诊断固定选择注册序中首个环成员。

每次QEMU120秒绝对期限；成功或fatal完整证据后至少1秒排空观察再由runner结束QEMU；timeout、静默、崩溃、自退、重复/乱序/未知身份、成功失败混合一律FAIL。归档命令、profile/CPU、ELF/image/firmware hash、原始串口和解析结果；只用显式make传入路径，normal只读snapshot，前后hash相同。parser负向host测试纳入test-harness，新增targets/catalog/scenario同步现有build harness规范。

### 8.3 回归门禁

执行test-host、test-static、test-harness，BOOT-1/2/3既有矩阵，x86 phase-0/inittab-phase/network/systest与8核kernel-selftest分开；ARM smp/no-ack/m3-probe/sched-ap/sched-wake/sched-tick及logo。保留所有旧协议marker及其因果偏序；ARMclocksource marker仍在AP收束后，不能因提前软件初始化破坏现有runner。KERNEL_SELFTEST关闭传空，不能传0；systest用现行bucket命令 `make OS01_SYSTEST=1 test-qemu SUITE=systest`。

## 9. 完成定义

spec与plan经独立评审批准只表示文档可执行。实施完成必须有14组新矩阵、上述回归及双架构真实产物证据；任何不可运行门禁明确列出阻塞，不以静态审计代替运行验收。BOOT-5的调度/系统服务解耦仍待单独设计。
