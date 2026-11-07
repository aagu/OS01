# 系统启动顺序、阶段抽象与优化分析

日期：2026-11-07  
源码基准：`b3a4f3bc`，分析覆盖 x86_64 与 aarch64 的公共入口、架构启动钩子、子系统框架及 x86 用户态启动路径。

## 1. 结论与证据边界

启动主干的依赖方向基本合理：先建立机器环境和内存，再探测设备、挂载文件系统、建立终端，最后启动 AP 和用户运行环境。统一 `kernel_main` 已消除两套全局入口，但同名架构钩子的职责、CPU 就绪状态以及失败传播尚未形成一致契约。

本报告为源码静态分析，没有运行 QEMU、故障注入或性能测量。下文区分“源码已确认的行为”“由调用关系推导的风险”和“结构优化建议”；不将潜在时序风险描述为已复现的崩溃，也不声称已修复问题。行号对应上述基准，后续以符号名定位为准。

优先顺序：CPU 生命周期与 SMP 握手 → 早期 IRQ 与时间设施 → 失败传播 → 阶段契约 → 调度启动与系统服务解耦。前三项建立正确性基础，后两项整理抽象边界。

## 2. 实际启动路径与抽象粒度

| 阶段 | 实际职责与入口 | 粒度评价 |
|------|----------------|----------|
| UEFI loader | `boot/uefi/main.c:139`：构造 handoff、读取和加载内核、收集图形/固件/熵、获取最终内存图、ExitBootServices、进入内核 | 生命周期完整；公共流程与架构加载机制分离合理 |
| 汇编引导 | per-arch `head.S`：初始映射、栈、机器状态与异常基础设施，进入高半区 `kernel_main` | 必要的架构边界，不应强求实现统一 |
| 交接与 early init | `kernel/core/main.c:61`：canary、boot_context 校验、早期输出和异常入口 | 基本合理；需要明确 early 阶段哪些能力仍不可用 |
| 内存启动 | `arch_boot_memory()`：PMM/slab、direct map、架构页表及 framebuffer 接线 | 内存和显示职责交织，但映射先后存在实际依赖，不宜机械拆分 |
| 子系统启动 | `arch_boot_subsystems()`：x86 执行 phase 3–6；aarch64 做 DTB/GIC | 同名钩子的完成能力不一致，粒度偏粗 |
| BSP percpu | `arch_boot_percpu()`：CPU 拓扑、percpu 数据、当前 CPU 指针及部分状态发布 | 是必要能力边界；x86 排在整个 phase 3–6 后，需核查早期 IRQ 依赖 |
| 随机数 | `random_init(bootctx)` | 位于随机设备使用和用户态之前合理；仍需保持熵质量与失败语义 |
| VFS 与终端 | `fs_boot_prepare()` → PCI/USB 节点 → PTY → 架构节点 → 根挂载/tmpfs/procfs → TTY → devfs probe | 大方向合理；公共 main 暴露了较多设备类别和诊断细节 |
| SMP 与时间启动 | `arch_boot_aps()`：x86 做 accounting/tick/AP/percpu callbacks；aarch64 还做 phase 4、softirq、BSP tick、IRQ 放行和 shootdown probe | 多个能力边界混在一个钩子里 |
| 调度与用户运行环境 | creation window → futex → console → x86 `task_init()`；aarch64 走 kernel idle/handoff | 需要区分架构机制、通用调度机制和系统服务策略 |
| PID 1 | `user/init.c:489`：信号处理、inittab、SYSINIT → WAIT → ONCE → 服务监督 | 当前规模适合四阶段模型；失败策略与监督阻塞需要后续补强 |

x86 的 `task_init()` 实现在 `kernel/arch/x86_64/cpu/sched_boot.c:52`，实际创建 `/bin/init`。默认 `config/inittab` 启动 `/bin/terminal`，terminal 经 PTY 启动 ash。部分注释中的 `/init.elf` 已与实际运行路径不一致。

## 3. 应保留的顺序与不变量

- 内存分配能力先于设备探测、驱动绑定及文件系统构建。
- `device_boot_init()` 先建立 device/block/net registry，再枚举 PCI 并绑定驱动；NIC probe 的注册前置条件明确。
- VFS/devfs 准备先于设备节点注册；块设备探测先于根文件系统挂载。
- TTY 和标准 fd 准备先于首个用户进程；默认 terminal 的运行路径还依赖 PTY。
- 主要挂载工作在 AP 启动之前完成，以约束当前 VFS bootstrap 锁豁免期间的并发。
- 用户 init 先取得 PID 1，再创建消耗普通 PID 的服务线程；AP idle 使用 PID 0，不应破坏此约束。
- tick、IPI 和调度分别需要自己的就绪条件；“CPU 已发现”不能替代“可接收 IPI”或“可运行任务”。

不建议在这些不变量明确之前提前 SMP、并行探测或整体重排。启动速度尚无测量证据，应先建立阶段耗时和能力观测再做性能取舍。

## 4. 正确性问题与风险

### F1：CPU online 的生产者与消费者语义冲突（优先）

源码行为：`kernel/percpu/percpu.c:21` 的 `percpu_init()` 在构造数据时将 `online` release-store 为 1。x86 `kernel/arch/x86_64/smp/boot.c:81` 在启动 AP 前遍历并初始化所有 enabled CPU；随后 `kernel/arch/x86_64/smp/smp.c:275` 通过 `online` 等待 AP 响应。

推导：该等待条件可在 AP 执行前就满足，不能证明目标 CPU 已完成启动。AP 入口后续也会写 online，TSC 采样另有握手，但这些不能恢复先前等待条件的语义。尚未通过延迟/禁止 AP 响应验证实际影响。

优化：percpu 构造、CPU 启动响应、IPI 就绪和调度就绪分别定义发布方及消费条件；核查跨架构所有使用点。不能只删除通用 online 写入而遗漏 aarch64 的 shootdown 目标资格。状态可用独立能力位表达，不要求强行做成单一线性枚举。

### F2：x86 IRQ 放行早于运行期 percpu 建立（优先）

源码行为：`kernel/arch/x86_64/intr/pic_8259a.c:116` 的 phase-3 wrapper 在 PIC fallback 和 IOAPIC 分支均放行本地 IRQ。phase 4 的 `driver/pit.c:20` 注册 PIT IRQ 并设置周期；BSP GS/percpu 在整个 phase 3–6 结束后才由 `arch_boot_percpu()` 建立。`kernel/time/tick.c:32` 直接使用 this_cpu，`kernel/arch/x86_64/intr/entry.S:82` 直接通过 GS 访问 resched 状态。

推导：存在中断路径可达而运行期 percpu 前置条件未满足的窗口；实际是否投递及触发后果需要运行跟踪验证。仅有 accounting_ready 等局部门控不能证明整个中断入口/返回路径安全。

优化：分开控制器配置、handler 安装、中断源启动和 CPU IRQ 放行。RTC/时间校准可能需要早期中断，应先确认完整依赖，再决定提前建立 BSP percpu 或提供受限 early IRQ 路径；不能简单把所有 IRQ 延后而破坏校准和超时设施。

### F3：必需能力的失败策略分散

源码行为：`kernel/subsys/subsys.c:52` 对非 OPTIONAL 失败只记录 FAIL 并继续；APIC wrapper 和 device boot 的外围代码另行 panic。`arch_register_subsys()` 忽略 initcall 返回值，多处注册 wrapper 未传播注册错误。`kernel/arch/x86_64/cpu/sched_boot.c:126` 丢弃创建 init 的返回值，随后继续网络和调度启动。

TTY/PTY 同样需要核对必需性：main 对初始化失败仅日志记录；`kernel/tty/boot.c:45` 在 tty_alloc 失败时仍继续注册节点，节点存在不能证明终端可用。

优化：统一报告失败阶段、组件、错误码及策略；必需能力失败停止相应启动路径，可选失败有明确降级结果，依赖缺失阻止消费者运行。必需性应由目标启动配置决定，不能将所有硬件都提升为必需设备。

## 5. 抽象与维护性问题

### F4：公共阶段名没有一致的返回契约

x86 `arch_boot_subsystems()` 执行 phase 3–6，aarch64 同名函数只做 DTB/GIC；aarch64 把时间设施和 IRQ 放行放进 `arch_boot_aps()`。应统一公共边界的前置/后置能力，允许架构内部保留必要差异。只统一函数名或只增加数字 phase 都不足以解决问题。

### F5：子系统框架只表达大阶段

`subsys_init_phase()` 按注册顺序遍历，同 phase 没有显式依赖表达；linker section 顺序成为隐含编排输入。框架还缺少重复名称、非法 phase、注册容量失败等统一检查。

先为现有实际依赖建立命名阶段、依赖检查或局部顺序约束；完整 DAG 与取消固定表应根据需求单独裁决，不作为本轮正确性修复的默认前提。框架当前已经使用 log_*，旧 roadmap 的 serial_printk 描述过时。

### F6：per-CPU 接口的执行位置有歧义

`kernel/subsys/subsys.c:80` 在 BSP 上遍历 CPU 编号执行回调，并不把代码送到对应 CPU 执行。`kernel/arch/x86_64/smp/subsys_percpu.c:6` 的 LAPIC timer 回调已明确为空操作，原因是从 BSP 调用只会重复编程 BSP 的本地 LAPIC。

应区分“为目标 CPU 准备数据”和“在目标 CPU 初始化硬件”，避免成功日志掩盖实际无操作。

### F7：task_init 汇集架构机制与系统策略

x86 task_init 同时做 TSS/IST、init_mm、idle 发布、标准 fd、PID 1、lwIP、调度启用、VFS bootstrap 结束、thermal worker、自测和永久 idle loop。建议按职责拆解，让架构代码负责机器状态，让公共启动策略负责初始进程和服务编排。

其中 `sched_boot_start()` 与 `vfs_end_bootstrap()` 的先后、AP 已具调度能力时初始任务可放置的位置，需要明确发布协议后再调整；本报告没有证明两者之间已发生竞态。现有 boot creation window、PID 1 优先分配和 late selftest 时机均应保留并回归。

## 6. 五个 spec 的实施规划

以下 BOOT-1～BOOT-5 为路线图标识，尚未编写独立设计 spec 或逐步实施 plan。正式实施前，每项应先冻结行为契约、文件边界和失败路径验收。

| 标识 | 范围 / 主要触点 | 核心验收 | 前置 |
|------|----------------|----------|------|
| BOOT-1 CPU 生命周期与 SMP 握手 | `percpu/percpu.c`、percpu 头、双架构 SMP 与 boot 后端；澄清 prepared/online/ipi_ready/scheduler_ok，修复启动响应判定与发布顺序 | AP 延迟/不响应不会误判成功；不可对未就绪 CPU 发起 shootdown/任务放置；双架构 SMP 与现有调度/shootdown 回归 | 统一 kernel_main 已完成 |
| BOOT-2 早期中断与时间设施启动顺序 | x86 PIC/APIC/PIT、tick、IRQ 入口返回、BSP percpu、校准路径；对照 aarch64 的门控 | 首次 IRQ 到达前所有可达路径满足前置条件；覆盖早期投递、PIT/LAPIC handoff、校准失败与 fallback | BOOT-1 |
| BOOT-3 启动失败传播与必需能力检查 | `subsys/subsys.c`、注册 wrapper、fs/tty/pty boot、PID 1 创建；统一错误传播 | 必需步骤失败停止且定位明确；可选设备缺失仍可启动；覆盖注册失败、根挂载失败、TTY/PTY 与 init 创建失败 | 建议在 BOOT-2 后验收 |
| BOOT-4 阶段契约与跨架构编排 | `core/main.c`、`arch/boot.h`、双架构 boot 后端、subsys 头/执行器；命名阶段、同阶段依赖、per-CPU 执行位置 | 公共钩子前置/后置能力可检查；双架构满足各自特性门契约；必要依赖不依靠偶然链接顺序；删除误导注释/空成功语义 | BOOT-1～3 |
| BOOT-5 调度启动与系统服务解耦 | x86 `cpu/sched_boot.c`、公共 `sched/boot.c`、VFS bootstrap、网络/thermal/selftest 启动接线 | init 保持 PID 1；用户任务不能绕过运行态发布契约；网络、thermal、late selftest、终端和 Ctrl-C 回归 | BOOT-4 |

建议逐项合入：`BOOT-1 → BOOT-2 → BOOT-3 → BOOT-4 → BOOT-5`。BOOT-3 的失败策略设计可提前进行，此链主要规定整体验收与集成次序。BOOT-1/2 先补针对性观测或故障注入，确认静态风险的实际路径，再冻结修复方案。

ARCH-8 作为子系统框架治理总项，由 BOOT-3 承接失败传播、BOOT-4 承接阶段/依赖及 per-CPU 语义；不另排同范围的重复重构。固定表改造是否纳入 BOOT-4，由容量与生命周期需求裁决。低层 AP 启动 API 统一仍保留为独立后续，BOOT-1 不承诺完成整个统一 SMP 工程。

## 7. 验证要求与范围控制

正式 spec 应绑定当前 build harness 的 profile、测试目标和证据路径，覆盖双架构支持范围内的单核/双核/四核场景；8 核边界按 CPU 状态与资源变更补测。至少包含正常启动和对应失败/延迟路径，不能只靠正常启动日志证明时序安全。

- CPU/布局变更：静态 layout 检查、状态生产者/消费者审计及 SMP/shootdown 场景。
- 中断/时间变更：首次中断时点、校准、tick 切换与 fallback 场景；延迟故障不能造成无界等待。
- 用户启动变更：phase-0、inittab-phase、网络、PID 1/终端交互；syscall 与内核 selftest 分开运行。
- syscall E2E 必须使用顶层 `make OS01_SYSTEST=1 test-syscall`，不得同时启用 KERNEL_SELFTEST。
- 纯文档阶段只检查差异、链接与计划一致性，不将未运行的构建或 QEMU 测试列为通过。

后续独立范围：显式 root 标识与根盘选择、respawn 退避、ASKFIRST 非阻塞监督、启动耗时观测及诊断 probe 分离。主干保持主要挂载先于 SMP；暂不引入并行启动、EL0 用户态适配、新硬件或 UAPI 扩展。

## 8. 关联文档

- [路线图：启动编排治理](../../roadmap.md#启动编排治理五个-spec2026-11-07)
- [启动与 boot_context](../../boot/boot.md)
- [架构与 facade](../../arch.md)
- [子系统注册框架](../../subsys/subsys.md)
- [SMP](../../smp/smp.md)
- [调度器](../../sched/scheduler.md)
- [构建与测试目标](../../build/build.md)
