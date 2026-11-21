# GICv2 Phase 2 设计

[路线图](../../roadmap.md) · [Phase 1](2026-09-17-aarch64-gic-phase1-design.md) · [EL1 同步异常诊断](2026-09-30-aarch64-el1-sync-diagnostics-design.md)

## 目标与范围

aarch64 在正式 EL1 向量表启用后，收到 SError 的核应保存现场、尝试输出可定位的诊断并停驻。GICv2 继续服务 QEMU virt，同时明确 Non-secure EL1 与 GICv2 固件的交接契约，并验证合法而尚未分配的 INTID 可供未来驱动使用。

范围限于 AArch64 EL1 的正式向量表、GICv2 驱动及其现有消费者和验收。初始引导向量、AArch32、FIQ、GICv3、通用中断接口、RAS 恢复、SError 进程归因和其他真机启动阻断由各自任务处理。

## 方案选择

SError 采用三个向量槽直达共用致命路径，并复用同步致命诊断的无锁早期输出和首错仲裁。将 SError 转成普通 GIC IRQ 会引入无对应 IAR 的确认操作。恢复式处理需要平台 RAS 错误源、严重性与可恢复性的契约，当前平台没有这套能力。

评估了复用 [`kernel/core/trace.c`](../../../kernel/core/trace.c)。当前 `backtrace()` 读取 x86_64 的 RBP/RSP/RIP 并沿栈解引用，`lookup_kallsyms()` 使用可能持锁的 `log_err`，aarch64 构建也未编入该文件。可另行拆出纯地址符号查找。Phase 2 沿用 aarch64 同步致命诊断的无锁输出方式，以原始 ELR 为诊断接口。验收归档保留匹配的 ELF 或可检索的构建 ID，事后用匹配内核 ELF 解析内核 ELR。EL0 记录仅在另有进程映射与匹配用户 ELF 时可符号化，否则保留原始地址。解析结果描述中断接收点。

GICv2 按控制器公开能力和固件交接契约配置。按板名或 IIDR 猜安全状态会将同一控制器的不同启动配置混为一谈。继续共享按 INTID 索引的表，已有 1020 槽覆盖完整架构范围。原路线图中的“handler 表扩容”落为未用槽位覆盖和驱动接入契约，保留将来的真实消费者分配。

## SError

- 正式 VBAR 的 `+0x180`（EL1t）、`+0x380`（EL1h）、`+0x580`（AArch64 EL0）各分支到汇编入口。EL1t 在首个栈访问前选择 SP_EL1，保留硬件保存的来源模式和 SP_EL0。EL1h 与 EL0 使用各自进入异常后的 EL1 栈。三条路径屏蔽 DAIF，保存现有 272 字节 `pt_regs` 全帧，随后取 ESR_EL1。C 根据保存的 SPSR 模式输出来源。致命 C 路径及其意外返回都进入专用汇编停驻符号 `aarch64_serror_park`，该符号覆盖屏蔽及 WFI 循环。
- 第一条致命记录使用 `[aarch64-serror] FATAL origin=<el1t|el1h|el0t> mpidr=0x… ec=0x… esr=0x… elr=0x… spsr=0x… far=n/a`。MPIDR、ESR、ELR、SPSR 用 16 位小写十六进制，EC 用两位并由 ESR[31:26] 提取。真实 SError 的 EC 为 `0x2f`。ELR 是异常接收点，FAR 不提供故障地址，ISS 原样保留。SError 与已有 EL1h sync fatal 共享首错 claim，胜者尝试写一行，其他故障核直接停驻。同步致命记录格式保持原样。
- 致命路径只依赖有效的 EL1 栈、内核映射、首错原子操作和已初始化的早期 UART。它绕过常规日志锁、调度、信号和 GIC；故障核全屏蔽停驻。此处没有全核 rendezvous，另一核的普通 UART 输出可能交错，严重硬件故障也可能阻止输出。
- BSP 在 `arch_boot_controllers()` 解析 DTB、按运行时基址重新初始化 UART 后开启 SError，即执行 `msr daifclr, #4; isb`。正式 VBAR 在更早的 `arch_early_init()` 安装。AP 在 `arch_smp_ap_cpu_init()` 开始时开启，届时 EL1 栈、映射和正式 VBAR 已生效，UART 由 BSP 建立。正式 EL0 初始帧（含 exec）与信号返回校验统一使用 `0x240`，保留 D/F 屏蔽与 I 开启。探针或 trampoline 中专用的全屏蔽状态按现有用途处理，恢复正式执行的路径应继承或恢复已清 A 的状态。IRQ 屏蔽 API 仍只控制 I。物理 SError 到达 EL1 以启动固件的 EL2/EL3 路由为前提，内核侧验收记录此条件。

## GICv2 和 handler

- 用 `GICD_TYPER.SecurityExtn`（bit 10）选择访问策略。无 Security Extensions 时沿用 QEMU 的 Group 0 配置。有 Security Extensions 时只支持 Non-secure EL1，固件为 OS 持有的 SPI 以及每个目标核的 SGI/PPI 配置 Group 1，并把 PMR 初始化为 Non-secure 可写；EL1 跳过 IGROUPR 访问。在该视图 `GICD_CTLR` 和 `GICC_CTLR` 均以 bit 0 启用 Group 1，CPU 接口用 EOImodeNS=0，SGIR 保留 NSATT=0 的写值。能力位描述控制器功能，访问视图来自 Non-secure 启动契约。
- Non-secure 配置返回成功表示完成 EL1 可见配置，不能用 IGROUPR 读回证明固件交接。检查 GICC_PMR 写入后的可见值非零，固件将 PMR 锁在安全优先级范围时，诊断并拒绝该核进入可收中断状态。BSP 失败终止 IRQ bring-up，AP 在 runtime online、启动 ACK 和 IPI-ready 之前停驻，由 BSP 现有的 AP 启动超时报出失败。实际 SGI、CNTP 和 SPI 递送才是交接成功证据。参考 Arm IHI 0048B §3.5–3.6、§4.2、§4.3 的 SGIR 和 §4.4 的 CTLR/IAR/EOIR。[Arm GICv2 规范](https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe)
- 软件表上限 1020，硬件上限取 `min((ITLinesNumber + 1) * 32, 1020)`。SGI 0–15、PPI 16–31、SPI 32–1019 均使用现有注册接口，重复注册和注销返回码沿用。新增用例覆盖未占用的 SGI 5/15、PPI 16/29/31 和最高 SPI 1019，含真实边界较低的 TYPER。SGI 0–2 的现有探针和 SGI 3/4 的生产用途保留。其他槽按真实消费者逐个注册，无占位 handler 或全量 unmask。
- 注册和硬件递送分别由调用者控制。调用者先注册并在允许跨核递送前完成现有发布屏障；运行中变更须在中断源关闭且目标上没有在飞的 dispatch 时进行。SPI 应清设备源后返回，驱动回写完整 IAR。特殊 INTID 1020–1023 不查表、不触发回调且不写 EOIR。合法 INTID 无 handler 仍执行 unexpected 后回写，既有 trace 保留。
- 路由沿用已经发布的 banked GIC target 缓存。从高半 GICD 的 banked ITARGETSR0–7 折叠 CPU target 字段，允许 Group 0 对 Non-secure 读取为零。非零字段必须一致且 one-hot，允许非 identity 映射。全部为零时，仅当 `GICD_TYPER.CPUNumber[7:5] == 0` 且 DT 启用一个 CPU 时使用 target 1。每核在一次启动内原子 claim 唯一 target，拒绝重复位、空/非法多接口目标与越界逻辑核号。BSP 在首个 SPI 路由和启动 AP 前发布自己的 target，AP 在 `arch_smp_ap_cpu_init()` 内、runtime online 和启动 ACK 前校验并发布，失败时走上述超时语义。生产 PL011 RX 以及 QEMU 探针中的 BSP 定向 SPI 和 SGI 回发均使用 BSP 实际 target。QEMU 源 CPUID 专属断言留在 QEMU 验收中。

## 验收

| 层 | 可观察证据 |
|---|---|
| 宿主 | 模拟两种 SecurityExtn 的写入顺序和 PMR 读回，验证安全扩展模式不访问 IGROUPR，控制寄存器 bit 0 与 EOImodeNS 值正确。覆盖合法边界、重复注册、非 identity/重复 target、DT 单核但控制器多接口、Group 0 可见字段为零、完整 IAR 写回、合法未注册与四个特殊 INTID。 |
| 链接产物 | 三个固定向量偏移均跳往 SError 保存入口，验证 EL1t 先换栈，保存沿用 `PT_REGS_*`，三槽到停驻的路径没有 ERET，boot vectors 无变化。 |
| QEMU | 独立探针变体分别从三个槽执行，每个来源单独启动。预置 ESR/ELR/SPSR 并记录 armed，按来源与字段精确验收一次 fatal，暂停后核对 PC 落在专用停驻符号范围，拒绝任何续跑、其他 fatal 或提前退出。EL1t 探针以 SPSel=0、与 SP_EL1 不同的 SP_EL0 进入槽并检查 SP_EL0 未被用作保存栈。证据记录为合成入口。同时在 BSP/AP 开启点用探针读 DAIF，验证 A 清零和 I 仍屏蔽。运行现有 EL1 sync、GIC SPI、SMP SGI/CNTP 和正式 EL0/信号用例。 |
| 真机 | 为符合现有 UEFI、DTB、GICv2、PSCI 和首次早期 PL011 C 初始化条件的板记录板型、镜像、固件版本/安全分组与路由、DTB、SMP 数、每核 target。使用生产镜像观察各核 CNTP handler 的进展和实际 PL011 RX 到 TTY 的重复递送，以现有 IPI 机制在 online BSP/AP 间双向定向触发并记录相应核上的实际 handler/EOI。可用调试器观察或用独立板级诊断镜像采样；记录采用的方法。禁用 QEMU 的固定 SPI40 注入和 SGI 广播探针，避免触碰板上未归 OS 的源。板卡缺席时记录待验证及已知阻断，提交可照做的操作步骤。平台具备文档化 SError 注入能力时补采硬件入口记录，缺少该能力时单列待验证。 |

软件验收可以先凭宿主、链接产物和 QEMU 结果通过。真机 IRQ 覆盖和真实 SError 递送各自只有在相应证据归档后才可填已通过。路线图应分别记录实现进度和物理验证状态。

SError 的来源、路由与保存寄存器参考 [Arm AArch64 异常模型](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Exception%20model.pdf)。
