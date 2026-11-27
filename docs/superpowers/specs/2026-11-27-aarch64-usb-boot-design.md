# aarch64 U 盘启动设计

2026-11-27

## 目标与交付范围

以 GPT U 盘上的 FAT32 ESP 启动 `EFI/BOOT/BOOTAA64.EFI`，加载同盘的 `kernel.elf`。离开 UEFI Boot Services 后，OS01 用自己的 xHCI 与 USB Mass Storage 驱动重新枚举 U 盘，按 loader 交接的 ESP 分区身份找到物理盘，把同盘 ext2 分区挂为 `/`，把对应 FAT32 ESP 挂为 `/boot`，启动至串口 shell 并读回这两个分区各自的文件。

本期设两个验收层级。QEMU `virt` 是自动化验收环境，Raspberry Pi 5 Model B 是首个真机，使用已有 bring-up 的 WoR UEFI v0.3、Device Tree 模式和 RP1 USB 口。真机验收允许由 TF 卡承载平台固件和 VideoCore 配置，OS01 loader、内核、ESP 和根分区均由 U 盘提供。USB 设备限定为启动前直插、单 LUN、512 字节逻辑扇区、SCSI transparent/Bulk-Only Transport (BOT) 的 USB 2.0 High-Speed U 盘。QEMU 验收串口交互，RPi5 验收串口输出与实际用户态读盘。

Dragon Q8B、其他固件版本、完整 BCM2712 PCIe RC 接管、通用 DWC3 bring-up、aarch64 USB HID、Hub 启动、UAS、SuperSpeed 专项、运行期热拔插和掉电持久化资格均留待后续。已具备的 USB MSC 读、写和 flush 语义继续通过共用路径提供，本期验收聚焦启动与读取。x86_64 已有路径继续承担其原有写入、Hub、HID 和协商速度回归。

## 现状与方案

| 方案 | 成果与代价 |
| --- | --- |
| 原生 xHCI + BOT | 固件加载内核后，内核能继续从实际 U 盘读写，复用 x86_64 的 xHCI、MSC 与分区体系。QEMU 可先验证 PCI 路径，RPi5 另需 RP1 平台接入、设备地址转换与 DMA 同步。采用此方案。 |
| UEFI 预读 ext2 镜像到 RAM | 可较早进入用户态，同时增加镜像内存开销、保留区和 ABI 设计，运行时访问的是内存副本。 |
| 板载存储驱动 | SDHCI 或 NVMe 可以解决相应板卡的一部分根盘需求，需要各自驱动，适合单独推进。 |

共用 loader 已通过 Loaded Image 的 DeviceHandle 读取 GPT HD device-path 节点，填充 `boot_context.boot_device`。`fs_boot_mounts()` 已按 ESP GUID 选盘，并比较分区编号、起始 LBA 和长度。同一数据路径和双分区格式在现有镜像中可用。当前 aarch64 的 PCIe ECAM 支持 QEMU `virt`，xHCI 仍位于 x86_64 源目录，RPi5 已能进入 boot-3，缺少内核运行时根块设备。使用本仓库现有 QEMU 固件、镜像、`acpi=off` 和匹配的 DTB 实测，USB-only 已有 UEFI USB 启动项、有效 ESP GUID 与内核 handoff 日志，随后在 boot-3 根盘阶段报 `-ENODEV`。参见 [既有 USB 启动设计](2026-11-14-usb-boot-design.md)、[aarch64 PCIe 一期](2026-11-27-aarch64-pcie-bus-design.md)、[RPi5 bring-up 记录](../../aarch64-rpi5-bringup-handoff-2026-11-25.md) 和 [路线图](../../roadmap.md)。

## 固件、磁盘与启动顺序

1. 继续使用 `make PROFILE=aarch64-clang AARCH64_PLATFORM=<qemu-virt|rpi5> aarch64-uefi` 的双分区镜像。固件按 removable-media 约定启动 AArch64 loader，loader 从其 Loaded Image 对应的 ESP 读取 `kernel.elf` 和 OS01 `config.txt`。在调用 `ExitBootServices()` 前记录 ESP GUID 并传递现有 `boot_context` v3，退出后所有 USB I/O 都由内核发起。参见 [UEFI Boot Manager](https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html#removable-media-boot-behavior) 和 [ExitBootServices](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html#efi-boot-services-exitbootservices)。
2. QEMU PCI xHCI 经 `device-boot` 绑定。aarch64 始终注册存储阶段 provider `rp1-xhci`，依赖 `device-boot`，非目标板及安全拒绝均记录相应状态并正常返回。aarch64 的共用 `usb-msc` 依赖 `rp1-xhci`，x86_64 沿用对 `device-boot` 的依赖。provider 在成功发布控制器后才返回，`usb-msc` 在同一阶段完成块设备发布，随后才进入 `fs_boot_mounts()`。aarch64 启动轮询使用已就绪的单调时钟限制等待并保证 USB reset recovery 的最短延时，独立于 BSP 指令执行速度。复用 `usba` 至 `usbz` 命名。
3. 延续现有 GPT 选择语义。有效交接只允许匹配的磁盘，重复匹配报 `-EINVAL`，存在可扫描盘但没有匹配时报 `-ENOENT`，几何不一致沿用告警。旗标缺失时既有普通启动兼容策略继续有效，任何使用首盘回退的 USB 运行均无法通过本期验收。`/` 和 `/boot` 都必须挂载并读回各自标记。`boot_context` 及 GPT/FAT32/ext2 磁盘格式沿用现状。

## 共用 xHCI 与 DMA 边界

- 将 xHCI 控制器处理、轮询传输、Hub 协议依赖和 MSC 适配抽到共享驱动目录，PCI 侧集中处理 BAR、PCI command、legacy ownership 和设备框架回滚。aarch64 PCI 绑定限定 QEMU `qemu-xhci` 的 `1b36:000d` 与 xHCI class，使用该设备的 BAR0 资源契约 `0x4000` 与 `pci_iomap_range()`，并在能力、端口、doorbell、interrupter 和扩展能力访问前验证寄存器跨度。QMP 已确认本机 QEMU 11.1.3 报告该 64 位 BAR0 的尺寸为 16384；x86_64 保留既有 class 绑定及相容的 MMIO 接入。RP1 调用同一控制器启动和端口枚举流程。架构相关的空转、屏障和非一致性 cache 操作分别由架构实现承载。aarch64 的 `OS01_HAS_USB` 继续表示 USB devfs 展示能力，MSC 磁盘经通用 block/devfs 路径发布。参见 [QEMU xHCI PCI 实现](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-xhci-pci.c)。
- 区分 CPU 虚址、用于分配和释放的 CPU PA、写进 xHCI 寄存器和 TRB 的设备地址。覆盖 DCBAA、scratchpads、ERST/event ring、command/transfer rings、input/output context、EP0 和 bulk staging，以及完成事件中回显的 TRB 地址。范围、溢出、AC64、对齐和 64 KiB 约束在相应的地址域验证。不能映射整个对象时，在控制器看见该对象前失败。x86_64 与 QEMU virt 基线采用受支持的直通地址，RP1 按 §RP1 固件交接与平台接入确定 DMA aperture。
- coherent 路径保留发布和消费顺序。RP1 按 non-coherent 处理。CPU 写对象与链 TRB 先在发布前 clean 到 PoC，再发布 CYCLE 并同步其 cache line，完成屏障后才允许 doorbell。Event ring 在控制器启动前、output context 和 IN staging 在每次交给设备前清除脏 cache line；设备可写期间 CPU 不对其做写回。CPU 轮询 event ring 时先 invalidate、完成屏障，再看 CYCLE 和内容。读取 output context 或 IN 数据前再次 invalidate 并同步；环回绕、复用和恢复遵守同一顺序。DMA 对象隔离 cache line，普通调用者缓冲区继续经私有 staging 复制。
- 活动 DMA 必须以确认 halt、已完成的 endpoint/slot 停止或等效的控制器确认结束所有权。aarch64 PCI 探测先关闭 bus master，仅开 MMIO decode 以确认固件已停机并 reset，再编程内核地址并开启 bus master。初次读回仍运行时尝试停止并报告不安全启动。后续暂时无法证明停止时按所有权粒度隔离并保留 DMA，相关对象不可重新使用。aarch64 的控制器级不安全状态覆盖启动、端口枚举和 MSC 启动探测。PCI bind 阶段保留可追踪的 owner 并返回 `DEVICE_UNSAFE` 给 `device-boot`，独立 RP1 provider 或随后 `usb-msc` 探测通过 `device_fail_unsafe()` 直接停机，避免 optional init 把该状态降为告警。隔离在单个 slot 且控制器仍安全时，其他合格盘可以继续启动。

## RP1 固件交接与平台接入

目标为使用 WoR v0.3 正常从板载 RP1 USB 端口启动 OS01 的设备。TF 卡保存 `RPI_EFI.fd`、固件 DTB、VideoCore `config.txt` 和已有串口配置，U 盘 ESP 的 OS01 `config.txt` 继续使用 `platform rpi5`。在 DTB 模式下识别受支持的根板型与 `brcm,bcm2712-pcie` 下的 RP1 路径，并按 `compatible = "snps,dwc3"`、有效 `status` 和 `dr_mode = "host"` 筛选两个已知 RP1 USB 控制器。v0.3 随包 DTB 离线核对含这两个节点且均启用，随包 DTB 只用于样例，运行以实际 handoff DTB 为准。参见 [WoR v0.3 发布说明](https://github.com/worproject/rpi5-uefi/releases/tag/v0.3) 和 [Raspberry Pi 5 固件设置](https://github.com/worproject/rpi5-uefi/tree/v0.3#configuration-settings)。

1. DTB 提供板型、RC 的 `reg`、RP1 子设备身份及相对外设窗口的偏移。普通祖先总线负责把 RC 寄存器地址转换到 CPU 侧。RP1 子节点偏移使用 RP1 `ranges` 中的子总线基址计算，显式识别 PCI 父总线的三 cell memory address 和窗口类型。链路已起且总线编号有效时读 root port 和下游 RP1 endpoint 的 PCI 配置。仅允许为配置读取选定并恢复间接访问 selector，保留 BAR、command、桥窗口和链路配置。根据运行时 RP1 外设 BAR1、root port 桥窗口和 RC outbound 读回值得到实际 CPU 窗口，确认 RP1 身份、Memory Space 和 Bus Master 已启用。随包 DTB 和固件硬件窗口记录可能不同，运行时读回决定实际 CPU 地址。DTB 的静态 PCI bus 地址和 RAM DMA 别名用作诊断。WoR 自身同样从枚举后的 BAR1 取得 RP1 外设 CPU 基址。[WoR RP1 总线驱动](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Silicon/RaspberryPi/RpiSiliconPkg/Drivers/Rp1BusDxe/Rp1BusDxe.c#L219-L270)
2. DWC3 节点的偏移应为 RP1 外设窗口内的 `0x00200000` 和 `0x00300000`，每处资源为 1 MiB。M1 完成后先验证并映射 RC，再验证并映射 RP1 控制器。每个实际映射单元检查内存图、Boot Block、已有页表属性及地址溢出。限界 xHCI 到各 DWC3 的 `[0, 0x8000)`，在单独的范围校验后读全局状态并确认 DWC3 核 ID 与 host 状态（host-only 硬件或双角色硬件的当前 host 模式）。依赖固件供电、时钟、PHY、总线配置和 EBS 时停机，内核首次访问时确认 HCH，随后仅执行 xHCI reset 并编程自己的 rings。遇到固件遗留控制器仍在运行，尝试停止并报告不安全启动。若平台前置条件不满足，真机验收保持待完成，控制器供电/PHY/RC 的初始化需另行进入设计。参见 [DWC3 寄存器窗口](https://github.com/torvalds/linux/blob/master/drivers/usb/dwc3/core.h#L77-L96) 和 [WoR EDK2 的 xHCI EBS 回调](https://github.com/worproject/edk2/blob/eca8aae/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c#L1901-L1926)。
3. 支持的 v0.3 DMA 策略为设备地址等于 CPU PA、整个分配位于 `[0, 0xc0000000)` 的可分配 RAM 中，并处于 RC 实际启用的 identity inbound 窗口内。还需以 root port 的 outbound decode 窗口为排除区，避免固件源码所述的地址碰撞。先读回并验证 RC BAR2/UBUS inbound 和 outbound 配置；如果硬件读回与该策略冲突，记录原因且不发布 DMA 对象。不得从随包 DTB 的 `0x10_00000000` 别名推定有效地址。使用受上下界约束的 PMM 分配为所有 RP1 DMA 对象取得落在合格区间内的页，包含对齐的连续 bulk staging。分配失败时按安全路径回滚。隔离时继续占用底层页。参见 [v0.3 PCIe 资源策略](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Silicon/Broadcom/Bcm27xx/Library/Bcm2712PciHostBridgeLib/Bcm2712PciHostBridgeLib.c#L5-L16) 和 [固件 DMA 低 3 GiB 约束](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Platform/RaspberryPi/RPi5/RPi5.dsc#L675-L686)。

## 验收与证据

| 场景 | 通过条件 |
| --- | --- |
| QEMU USB-only | AArch64 UEFI 从 `qemu-xhci` + `usb-storage` 引导。使用现有 `QEMU_EFI.fd`、`virt,gic-version=2,acpi=off`、匹配 CPU 和内存的 QEMU DTB、`qemu-xhci,p2=4,p3=0` 和 USB `bootindex=0`。拓扑保留现有 RNG，整个运行只有一个 USB 块盘。镜像 ESP GUID → 内核接管并配置 BOT → 发布 USB 块盘 → 按 GUID 选中 USB 的根分区及 ESP → 串口提示符 → 分别读回两个唯一标记 → 再现提示符。起止校验源镜像哈希。 |
| QEMU 双盘 | USB 固件引导优先。预先注册的 virtio-mmio 盘拥有独立 GPT 磁盘 GUID、ESP GUID 和不同的分区标记。必须证明首盘是非 USB 盘且最终两处挂载及标记均来自 USB。 |
| QEMU 错误身份及非 USB 回归 | 私有 USB 镜像内把首分区 type 改为 Microsoft Basic Data，保留分区 GUID、FAT 字节和 loader，另一条可读 virtio 盘具有不同 ESP GUID。已实测当前 QEMU 固件仍从该 USB FAT 分区启动并交接 GUID。实现后必须额外看见 USB MSC 发布以及 virtio 首盘，随后 `fs: no disk matched firmware boot ESP GUID`、根盘 `rc=-2`，且无 shell。另跑既有 virtio 根盘与 PCIe/AHCI 回归。 |
| 宿主及 x86_64 回归 | 测试 DMA 的非恒等转换、对象跨窗/溢出/AC64、发布和消费顺序、无法证明停止时保留资源，并以 DTB 样例验证 RP1 资源发现及缺失/禁用/歧义。x86_64 的 `usb-boot` 与 `usb-hid` 套件通过。 |
| RPi5 真机 | 使用已记录板型、WoR v0.3、EEPROM 版本和 RP1 排针串口。私有验收盘的 `/etc/inittab` 启动用户态脚本，将标准流接到 `/dev/tty`，依次读回 `/usb-root.txt` 与 `/boot/USBESP.TXT`，成功后 `exec /bin/sh -i`，在已知只有 TX 的 RP1 控制台采集真实 ash 提示符。连续两次冷启动取得 UEFI GUID、内核接管、MSC 发布、同盘挂载、两个标记及提示符，并记录端口、VID:PID、协商速度、DWC3/xHCI 标识与 DMA aperture。TF 卡只含平台固件。拔掉 U 盘再冷启动时，无法进入 OS01 shell。产品默认 inittab 和 RP1 串口输入策略沿用现状。 |

QEMU 保存每次的命令、串口日志和结构化测试结果。自动化与真机各自记录通过状态。只有真机证据取得后，才把 RPi5 的 USB 根盘阻断标为完成。
