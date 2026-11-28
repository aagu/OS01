# AArch64 BCM2712 MIP 与 RP1 中断设计

日期 2026-11-28

## 目标与范围

RPi 5 的 40 针排针 UART0 通过可复用的 RP1 中断服务向控制台 TTY 提供 RX。服务允许多个不同 RP1 中断源各自注册回调，UART0 是首个生产调用方。第一版在单一已由固件启动的 RP1 endpoint 上支持启动期注册、启用和失败回滚，成功的注册由内核持有到关机。通用 PCI 枚举、链路重训、RP1 复位、运行期热插拔及其他外设迁移留给各自的工作。

`console rp1-header` 在完整路由验证通过时启用 RX IRQ。缺少描述、资源不可用或无法安全接管时，开启现有 100 Hz BSP 轮询，串口继续提供启动日志和交互。普通 `console dtb` 仍使用其 PL011 的直接 GIC SPI，QEMU virt 的 GICv2m 与现有 x86 PCI 路径沿用现有接口。PL011 TX 继续轮询。

## 基线与方案选择

固件控制台路径显式将 `pl011_spi` 置零。普通 PL011 路径从三单元 GIC specifier 解析 SPI。RP1 UART0 的中断描述属于 RP1 控制器。2026-11-28 反编译本机 RPi5 UEFI v0.3 的 76,038 字节 DTB，在 `/axi/pcie@120000` 下看到 RP1 UART0 `interrupts = <0x19 0x04>`，即 RP1 源 25、level-high。该 RC 的 `msi-parent` 指向 `/axi/msi-controller@130000`，其兼容串为 `brcm,bcm2712-mip-intc`，CPU PA 为 `0x10_00130000`，PCI MSI 写入地址为 `0xff_fffff000`，基础 SPI 为 128，数量 64，偏移 0。该固件 FDT 中 UART 节点为 disabled，`rp1-header` 仍可使用已由固件启用的同一 UART。上述数字用于实测对照和测试夹具，运行时从经验证的描述及资源取得。这个离线 DTB 说明拓扑，实际启动时交付的 DTB 和寄存器状态以真机证据为准。 [固件 DTB 取证背景](../../aarch64-rpi5-bringup-handoff-2026-11-25.md#关键地址rp1-uart-的-cpu-别名是-0x1f00030000)

采用基于固件资源的 RP1 中断层和独立的 BCM2712 MIP 层。现有 RP1 xHCI 路径已经能读取间接配置空间并校验 endpoint 与 BAR1，沿用其资源发现方式可减少对固件状态的写入。仅为 UART 硬编码一条路由无法给第二个 RP1 外设分配向量。先建设完整 BCM2712 通用 PCI 后端会额外引入桥枚举和统一资源接管，两者都超出这次首次接入所需的平台能力。 [RP1 资源发现](../../../kernel/arch/aarch64/bus/rp1_pcie.c) · [现有 AArch64 MSI-X 后端](../../../kernel/arch/aarch64/bus/pci.c#L680)

## 拓扑与平台资源

- 在现有 BCM2712/RP1 DT 扫描旁发现可选的 IRQ 描述，沿 RP1 的实际 RC 解析 `msi-parent`，从启用的 MIP 节点读取 `reg`、`brcm,msi-pci-addr`、`brcm,msi-base-spi`、`brcm,msi-num-spis` 和可选的 `brcm,msi-offset`（缺省 0）。验证唯一匹配、完整单元宽度、地址加法及窗口边界、槽号落在 64 个 MIP 输入内、整段 GIC SPI 范围合法且可用。可选 IRQ 描述的错误单独记录，xHCI 资源仍可使用。无 MIP 描述的启动保有原行为。
- 解析 UART specifier 时确认父控制器是所选 RP1，类型为 level-high，`reg` 对应 RP1 BAR1 内的 UART0，并与固件移交的控制台 CPU PA 一致。允许 `rp1-header` 下这个精确匹配的 disabled UART 描述，其他模式按现有设备启用规则行事。硬件源仍由描述给出，源 25 专用于测试对照。
- MIP 的显式或继承 `interrupt-parent` 必须解析为内核选中的同一 GICv2，运行时用 `nr_intids` 验证整个上游范围。UART 的父控制器也按继承规则解析，验证与所选 RP1 的 phandle 相同。
- UEFI loader 在显式 RPi 5 `rp1-header` 路径、最终 memory map 获取前查询唯一的 RP1 C0 PCI I/O handle（`1de4:0001`, revision 2）。只读取得 BDF、MSI-X capability 和 `GetBarAttributes(0)` 的唯一内存资源描述，释放协议返回的缓冲区。首版支持 table 与 PBA 均位于 BAR0 的 RP1 形状。以固件描述的实际长度为边界，同时校验 PCI BAR 原值、资源 CPU 基址及内核翻译一致。Table 长度为 `entries * 16`，PBA 长度为 `ceil(entries / 64) * 8`，验证两个区间完整且互不重叠。内核读回 endpoint 身份、BDF、BAR 和 capability 的 BIR、表项数及偏移后才访问。协议或有效资源缺失时保留轮询。全程不发 sizing 探针。PCI capability 链有界且无环。 [WoR BAR 查询](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Silicon/RaspberryPi/RpiSiliconPkg/Drivers/Rp1BusDxe/Rp1BusDxe.c#L236-L248)
- `boot_context` 用追加的可选 v4 记录传递固定宽度的 BAR0 PCI 基址、CPU 基址、长度、BDF 与 MSI-X table/PBA 描述。保留 v3 的 200 字节 wire 布局及汇编偏移。新内核接受旧 loader 的 v3，缺记录时 UART 走轮询；x86、QEMU 及未取得记录的 loader 仍发布 v3。含 v4 RP1 记录的 loader 用本次新内核配套启动。严格检查 version、size、flag，只有 v4 验证成功后读取追加字段，兼容旧 v3 的检查以原 200 字节大小为准。查询结果跨 ExitBootServices 重试保留。宿主 ABI 检查固定两个版本的布局。
- Stage 3 在 VMM 门禁关闭前验证并预映射 RC、MSI-X table/PBA、RP1 PCIe APB 与 MIP MMIO，使用 Device-nGnRnE，检查 Boot Block、UEFI RAM 重叠及窗口边界。共享 RC 索引访问要串行、保存并还原选择器。共享 endpoint 和 BAR 发现独立于 xHCI 描述、设备数量及 DMA 校验，后者仍由 xHCI 分支检查。RP1 APB 取经验证 BAR1 的 `0x108000`，映射包括 RW/SET/CLR 别名的整段 4 KiB。保有固件分配的 endpoint BAR、既有出站窗、xHCI DMA 入站窗与 RP1 控制台 pinmux/波特率。
- 64 位 MSI-X PCI 写入目标与 CPU 的 MIP MMIO PA 分开计算。先检查启用的 RC 入站窗及 RC 内部 MSI 目标译码，沿用唯一匹配、无冲突的已有 MIP 路由。首版不写参考驱动使用的 RC_BAR1 寄存器，因为 OS01 USB 资源校验依赖 `0x40ac/0x40b0` 读值。无现成路由时，从 RC_BAR4–10 选配置尺寸与 remap enable 均为关闭、且无重叠的额外窗。保存寄存器，使用 BCM2712 布局及 4 KiB 尺寸编码，最后打开译码并读回。遇启用映射冲突、内部 MSI 目标冲突或不能判明时回退。额外窗能否转发 MIP 是待真机验证的假设，只有实际递送探测通过才提交。第一次申请才编程，失败时撤销本次新窗，保有已有译码和 PCIe 链路。 [BCM2712 PCIe 参考实现](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/pci/controller/pcie-brcmstb.c)

## 中断管理与生命周期

1. **MIP 层**为每个请求分配唯一空闲槽 `slot`。MSI 数据为 `offset + slot`，GIC INTID 为 `32 + base_spi + offset + slot`。先取得 GIC handler 槽的独占权并保存 GIC trigger、priority、target 和可访问的组位。候选 SPI 无已注册 handler、无 enable/active/pending，MIP 对应 host/VPU 状态位也无 pending。只调整当前槽的 MIP edge、host 接收和 VPU 屏蔽位。禁用时把 SPI 配为 edge-rising 并检查读回，使用 BSP 目标和现有 GIC 中断组规则。 [MIP 槽号、消息与 GIC 分配](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/irqchip/irq-bcm2712-mip.c)
2. **RP1 层**以外设源号索引 MSI-X 表项和 APB 配置，MIP 槽另行分配。请求包含源号、触发类型、回调和上下文，返回内核持有的不透明句柄。源号为 0–60，且必须落在 PCI capability 表项范围内。USBHOST0/1 的 30–39 是 edge，其余为 level-high。调用方在提交前使设备自身 IRQ 源屏蔽且静默，目标源的 TEST、ENABLE 或 PBA 已置位时拒绝请求，重复请求同一源失败。 [Raspberry Pi RP1 Peripherals，第 6 章](https://pip-assets.raspberrypi.com/categories/892-raspberry-pi-5/documents/RP-008370-DS-1-rp1-peripherals.pdf#page=80)
3. **功能级接管**区分三种状态。固件留下 MSI-X Enable=0 时，保存 Message Control 和各向量原掩码，保持 Enable=0 并置 Function Mask，逐项设置并读回 Vector Mask，再允许 Enable=1 并清 Function Mask。已有 OS01 向量时只改当前源和表项，保留全局控制位。MSI-X Enable=1 且内核没有功能所有权时拒绝接管。WoR 会开启两个 USBHOST APB 源，保留未拥有源的 APB 配置与 PBA，USB APB enable 本身允许原先禁用的 MSI-X 功能进入接管流程。Message Control 写入限该半字。 [WoR 的 USB 源配置](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Silicon/RaspberryPi/RpiSiliconPkg/Drivers/Rp1BusDxe/Rp1BusDxe.c#L90-L105)
4. **提交前递送探测**在启动 BSP、本地 IRQ 关闭且其它 CPU 未 online 时执行。先锁定资源并配置 GIC、MIP 和屏蔽态表项的消息地址低/高与数据，再开启 GIC、该表项及 RP1 源。确认 GIC 初始 pending=0 后，用该源的 RP1 APB TEST 位发消息，使用 architected counter 在 10 ms 内等候对应的 GIC pending。清 TEST，使用 endpoint 读回冲刷写入，level 源执行 IACK 并再次冲刷，清本次 GIC pending 并检查源、PBA 与 GIC 静默。该事件绕过设备回调，退出时恢复原本地 IRQ 状态。超时、探测前已有 pending 或探测后无法静默都阻止提交。CPU 写 MIP 或 GIC 软件 pending 可用于诊断。
5. **失败回滚**先屏蔽当前 RP1 源及表项，关闭已取得的 GIC SPI，endpoint 读回确认阻断，再处理 TEST/IACK 与挂起。PBA 是只读状态。确认静默且无迟到递送才恢复本次向量及 GIC/MIP 快照并释放预留。否则维持源、表项、GIC 与必要入站资源的隔离与占用，关闭该 SPI，拒绝重用相应资源。若 GIC 禁用读回失败，须确认 MIP Host mask 或 RP1 当前向量/源的屏蔽才进入轮询。发生硬件写入后所有隔离屏障均无法验证时，使用现有 unsafe 设备故障路径。首次接管失败且确认静默时，在功能禁用、全局屏蔽的状态恢复改过的向量掩码，最后恢复原 Message Control。没有成功消费者且能够安全撤销时还原本次写入的入站窗。既有消费者继续工作。
6. **派发与确认**由 GIC 调用 RP1 trampoline，仅已提交的注册允许进入设备回调。准备中、撤销或隔离的源忽略回调并返回 GIC EOI，以防迟到中断和轮询同时消费 FIFO。已提交的设备回调清除设备源后返回。RP1 level-high 源写 IACK，edge 源关闭 IACK 模式。通过同一 endpoint 安全寄存器的读回冲刷设备与 RP1 posted writes，再由现有 GIC dispatch 执行 EOI。level 源在 IACK 时仍 asserted 会再发 MSI-X。 [RP1 MSI-X 与 IACK](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/mfd/rp1.c)

准备、提交、放弃由启动 BSP 串行执行。提交后的回调运行在 BSP IRQ 上下文，禁止睡眠。第一版支持多个同时存活的源和失败回滚，成功注册由内核持有到关机。调用者无需知道 GIC INTID、MSI 地址或表项索引。现有通用 PCI MSI-X API 面向表项 0 与 32 位消息地址，保留其调用行为。RP1 复用可抽出的表描述解码或带索引表项操作，并采用独立的 64 位消息路径。

## 控制台行为

`arch_console_input_attach()` 建立 TTY 后判断 UART 路径。RP1 模式用已准备的服务注册 RX。普通 DTB 模式沿现有 SPI 路径，注册后配置失败时撤销 handler。RP1 模式用 RXIM 与 RTIM 两种 UART 原因驱动同一 RP1 源，以覆盖稀疏单字节。探测期间 UART IMSC 关闭且保留 FIFO 字节。提交时在本地 IRQ 关闭且 TTY 指针已发布的条件下打开 IMSC，由 IRQ 消费已到及新到的数据。RP1 handler 复用 FIFO drain 与 `tty_push_input()`，读 DR 清除接收条件，避免在最后一次 FIFO-empty 检查后用全量 ICR 清掉刚跨阈值的新事件。始终仅一个路径消费 FIFO。成功启用 IRQ 时轮询关闭，回退时主动屏蔽 IMSC 并验证，设备屏蔽读回失败时按上述规则确认上游隔离，由 BSP 每 10 ms 读取。保留 `OS01_SELFTEST` 对直接 PL011 SPI 的所有权。

日志用 `log_info`／`log_warn`。成功记录 `PL011 console RX armed via RP1` 与源、INTID，回退记录 `using 100 Hz poll fallback` 和可定位的失败阶段。模式日志各一次。验证构建提供递送计数、IRQ RX 字节数与 poll RX 字节数，由 BSP 在低频验证日志中读取快照。生产构建保留模式日志。

## 验收

- 宿主覆盖 v3/v4 ABI、UEFI 资源缺失及越界、实际形状的合成 DTB、缺失及异常 phandle/单元、错误 GIC/UART 父控制器及 PA、合法 disabled 固件 UART、64 位地址和带偏移的 MIP 槽。验证 table/PBA 的边界、入站窗沿用及新窗、RC 内部 MSI 冲突、两个源独立注册、重复源、槽耗尽、固件所有权、原有及晚到 PBA、递送探测模型成功或超时、设备 flush/IACK/EOI 顺序及逐阶段回滚。测试以 `@os01-test` 元数据登记。
- 回归现有 aarch64 QEMU virt 串口交互、PL011 GIC SPI 自测和 PCIe/MSI-X。编译 RPi 5 与普通 QEMU 镜像。RP1 硬件递送用真机验证。
- RPi 5 UEFI v0.3 冷启动记录 DTB 拓扑、BAR0 合法范围、入站窗读回、RP1 源、MSI 地址/数据和 GIC INTID，区分探测成功和运行期 UART IRQ。用外部串流脚本验证单字节、连续发送已知字节串、跨越 arming 的输入以及空闲稳定。在 UART/TTY 容量内测试，证明 IRQ 计数随输入增长，poll RX 字节数为零，应用读得的字节完整且无重复。
- 在 arming 后从实际 RP1 USB 根设备直接读取已知只读块并比对哈希，补足启动阶段已经完成的根挂载。用移除 MIP 描述的测试 DTB 或等效验证构建验证 polling，保有串口和 RP1 USB 根盘。文档评审与真机验收分别记录.
