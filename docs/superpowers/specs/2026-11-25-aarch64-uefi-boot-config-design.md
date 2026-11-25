# AArch64 UEFI 启动配置设计

## 目标

从当前已在真机和 QEMU 启动过的 bring-up 工作树推进。`PROFILE=aarch64-clang` 产出一份 `BOOTAA64.EFI` 和一份 `kernel.elf`。QEMU virt 与 Raspberry Pi 5 使用各自的磁盘镜像和 ESP 根目录 `config.txt`。平台差异由同一个加载器在启动时解释。修改 ESP 配置后重启即可应用。真机基线为交接记录中的 Pi 5 Model B Rev 1.0 和 Worproject RPi5 UEFI v0.3。

验收范围到平台启动和设备初始化。RPi5 当前根挂载停在 `-ENODEV`。根文件系统后端、RP1 RX 中断、真机多核用户态和其他机型支持各自另行实现。[Bring-up 证据](../../aarch64-rpi5-bringup-handoff-2026-11-25.md#更新-2026-11-25-深夜真机首次跑通内核craton-logo-on-hdmi)

## 方案取舍

| 方案 | 取舍 |
| --- | --- |
| 编译期平台宏 | 可以直接复用上板变体，平台选择会进入 EFI 或内核对象和重建戳。 |
| 固件全量自动探测 | 便于交付一个镜像。当前板固件缺少 EFI DTB 项，且提供的 `/chosen/stdout-path` 指向三针调试座，无法表达用户选择 40 针排针。 |
| ESP 配置，加载器解释 | 采用。构建参数只选镜像，设备发现仍来自 DTB，加载器把固件个例和控制台策略转换成内核可读的早期数据。 |

## 构建入口与配置

- 顶层 `make PROFILE=aarch64-clang aarch64-uefi` 默认 `AARCH64_PLATFORM=qemu-virt`，继续生成 `$(BUILD_DIR)/image/aarch64-uefi.img`，`run-aarch64-uefi` 和现有 QEMU 验收沿用这个入口。`AARCH64_PLATFORM=rpi5` 生成 `$(BUILD_DIR)/image/rpi5/aarch64-uefi.img`，板镜像的依赖链不获取或复制 `QEMU_EFI.fd`。非法值在解析期报错。QEMU runner 和验收目标遇到 `rpi5` 应报用途错误，固件获取和镜像构建前退出。平台名不进入编译参数、对象目录或 ELF 链接布局。沿用 `AARCH64_BUILD_ROOT` 及现有镜像测试变体的隔离机制。首版 RPi5 入口只提供普通镜像，要求测试变体时给出构建错误。
- 两个平台各有一份受版本控制的配置。镜像规则把选中的配置放入对应 ESP 的 `/config.txt`，作为镜像直接依赖。EFI、内核及用户态产物来自同一套构建。两个镜像的目标、临时目录、manifest 路径各自独立。修改配置只触发对应镜像重打包。
- 加载器从 `LoadedImage.DeviceHandle` 所属卷读取 `config.txt`，和读取内核用同一个卷。RPi5 那张 TF 卡上的 VideoCore `config.txt` 仍负责 `enable_rp1_uart=1` 及 RPI_EFI 链载。OS01 的文件在装载 `BOOTAA64.EFI` 的 ESP 上。

| 镜像 | ESP 配置第一行 | ESP 配置第二行 |
| --- | --- | --- |
| QEMU virt | `platform qemu-virt` | `console dtb` |
| RPi5 | `platform rpi5` | `console rp1-header` |

## 配置解析与错误语义

- 公共 UEFI 入口只在 Boot Services 可用时读取一次，用有界的字节解析得到类型化配置，再交给图形和固件获取路径。文件最大 4096 字节，允许 LF、CRLF、末行无换行、空行和以 `#` 开头的整行注释。去除行首尾 ASCII 空格和制表符，在第一段空格或制表符处切键和值。文件内 NUL、裸 CR 和超长内容作为配置错误。未知键忽略，不赋予未知键启动语义。诊断记录已识别项的失败原因和行号。
- 缺文件或空文件在 aarch64 上等价于 `platform auto`、`console dtb`。缺单项时分别应用这两个默认值。显式 `platform` 支持 `auto`、`qemu-virt` 和 `rpi5`。显式 `console` 支持 `dtb` 和 `rp1-header`。`rp1-header` 要求显式 `platform rpi5`。aarch64 上重复或错误的已识别平台／控制台项、已打开文件的读取失败均在加载器阶段报错。显式平台和 DT 根节点冲突时停止。
- x86_64 兼容 `resolution WxH` 和 `1440x900` 的默认目标。按现有习惯使用最后一条有效设置。GOP 按固件列出的顺序遍历模式，候选宽高都不超过目标值，且都严格大于当前候选时才替换。例如目标为 `1440x900` 时，依次看到 `1024x768` 和 `1280x720` 会选 `1024x768`。无效分辨率行按未设置处理，文件读取失败仍可用默认分辨率启动。x86_64 忽略 aarch64 平台和控制台项。aarch64 以 GOP 当前模式为准。

## DTB 来源和控制台选择

1. 优先使用有效 EFI DTB 配置表。显式 `qemu-virt` 匹配根 `linux,dummy-virt`，显式 `rpi5` 匹配根 `raspberrypi,5-model-b`。`auto` 只使用合法 EFI DTB 和通用设备发现。QEMU runner 按现有方式提供匹配 vCPU 数的 DTB，保持其 DTB 注入与 `acpi=off` 协同。存在但损坏或与显式平台冲突的 EFI DTB 视为错误。
2. 只有显式 RPi5 且 EFI DTB 项缺失时才允许读取 `[0x1f0000, 0x210000)` 中的 VideoCore DTB，最大 128 KiB。Worproject v0.3 把这段保留 RAM 建为 Normal Write-Back 恒等映射，分配类型为 `EfiReservedMemoryType`。加载器在预备内存图中仅对这一固定范围接受带 `EFI_MEMORY_WB` 且无 `EFI_MEMORY_RP` 的 `EfiReservedMemoryType` 或 `EfiConventionalMemory` 覆盖。读 header 前校验 header 覆盖，复制前校验完整区间覆盖，拒绝缺洞、溢出、MMIO 或读保护。校验 FDT 结构和根机型后再采用。各来源均复制到加载器管理的 handoff 页。EFI 表来源的 DTB 大小上限为 2 MiB。缺失或错误时使用 UEFI 诊断并返回错误。板测记录实际覆盖的 EFI 类型和属性。 [固件预留大小](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Platform/RaspberryPi/RPi5/RPi5.dsc#L414-L422) · [固件映射](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Platform/RaspberryPi/RPi5/Library/PlatformLib/RaspberryPiMem.c#L109-L115) · [EFI 分配类型](https://github.com/worproject/edk2-platforms/blob/8e1779b538bcc1e6dc68d7df625394f933651d7a/Platform/RaspberryPi/Library/MemoryInitPeiLib/MemoryInitPeiLib.c#L84-L96)
3. `console dtb` 选择 `/chosen/stdout-path` 指向的可用 PL011，支持绝对路径和 `/aliases` 别名，按第一个冒号截取节点标识。首版继续使用驱动已有的波特率策略。缺 `stdout-path` 时取首个启用且可翻译的 PL011。对于明确指向错误、不可访问或已禁用设备的 `stdout-path`，从加载器报告错误。加载器解析寄存器 CPU PA 后决定映射。内核用 handoff 的 CPU PA 匹配唯一启用的 PL011，查得时钟及可用的 GIC RX IRQ，找不到或有多个匹配时报 DTB 错误。按现有 DTB 解析测试入口调用而未给 handoff 的宿主调用保留普通 DT 解析契约。
4. `console rp1-header` 使用 RPi5 已实测的 CPU PA `0x1f00030000`。使用者须在固件启用 `enable_rp1_uart=1`。加载器依据已验证的 RPi5 DTB 选择这个板级固件例外，内核沿用固件串口配置，以轮询方式发送日志。固件控制台模式允许 DTB 没有可用的普通 PL011，运行期控制台的时钟与 RX GIC IRQ 都置零。整个路径不接触 `0x1c00030000`。本期无需实现 PCI 地址空间解析。
5. 加载器和内核对普通总线使用一致的地址翻译和设备启用判断。空 `ranges` 表示恒等映射，缺少 `ranges` 的中间总线按无映射处理。用于 MMIO 的整个 `reg` 范围必须可翻译，拒绝溢出和跨越翻译区间。支持本期所需的普通一、二 cell 编码和嵌套总线。[Devicetree 规范](https://devicetree-specification.readthedocs.io/en/stable/devicetree-basics.html#ranges)

## 交接、页表与设备初始化

- 使用 200 字节的 `boot_context` v3。早期 MMIO 使用 `BOOT_CONTEXT_HAS_EARLY_MMIO` 与 `early_mmio`，新增 `BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED` 位 `1u << 8`，仅在早期 MMIO 有效且选择固件控制台时置位。先检查 v3 magic、version、size、flag 与窗口及 console MMIO 的表示范围和对齐，再发布可信 PA。旧 v3 handoff 携带合法 early MMIO 时按普通控制台方式解读。实际内核入口遇到缺失或无效的 early MMIO 时停机。汇编窗口拒绝路径、早期异常向量和 C panic/PL011 路径在可信 PA 尚未发布时均不访问 UART。宿主页表测试可继续用内部默认窗口模拟 handoff。
- M0 和 M1 采用规范化后的同一组 Device 窗口。主窗口覆盖早期 GIC 与普通 DT 控制台所需寄存器，额外的固件控制台在主窗口外使用自己的 2 MiB 块。块按 PA 排序，去重和验证范围。窗口不覆盖 RAM 或启动块 B，不跨越 2 GiB 恒等映射边界。2 GiB 以上的每段须在一个 1 GiB L1 槽内，最高地址小于 512 GiB。加载器检查形状，并在最终 EFI 内存图可用时检查映射与 RAM／handoff 页冲突；这一步与 `ExitBootServices()` 之间不调用会改变 map key 的服务。内核保留对收到的数据和 M1 内存类型的防御性验证。
- 两平台均走正常 `ExitBootServices()`，保留成功后的 AArch64 cache clean 以及现有屏障次序。每次拿到最终内存图快照都重新校验窗口。首次 EBS 调用后的重试只用现有内存图缓冲区和必要的 Memory Allocation Services，若无法重试则本地停机，避免调用 SerialIo、ConOut 或重新查协议。删除 bring-up 的 `AARCH64_SKIP_EBS`、`AARCH64_FIRMWARE_CONSOLE_PA`、`AARCH64_FW_CONSOLE_BEACON`、`AARCH64_VIRTIO_MMIO` 编译开关和 `P/Q/H/K`、HDMI 阶段写入。启动阶段通过 EBS 前的 UEFI 文本输出及交接后的内核日志定位。 [UEFI ExitBootServices](https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html#efi-boot-services-exitbootservices)
- `arch_boot_controllers()` 在 `dtb_init()` 成功后、`arch_boot_percpu()` 进入 SMP 门禁前映射 virtio-mmio。DTB 解析校验启用节点的完整 `reg` 范围，映射覆盖驱动使用的 `[base, base + 0x200)`，跨 2 MiB 边界时映射两块，同块合并。缺少节点返回成功。已存在的同 PA、Device、内核可读写且不可执行的 2 MiB 块视为已覆盖。与 RAM 或不兼容的已建映射冲突时停止启动，禁止覆盖。现有存储初始化会再次调用 mapper，完成后该调用直接返回成功，避免 SMP 启动后重复碰页表。装载使用 DTB 列出的 slot，QEMU 的 `0x0a000000` 不再出现在架构映射策略里。

## 通用启动参数的扩展点

首版以类型化配置和现有 handoff 完成平台支持。后续参数入口可采用可重复的 `kernel.arg <值>` 与 `init.arg <值>`，每行一个参数，保持顺序和值内空格。届时附加一个带边界的版本化参数区。内核在释放 handoff 内存前取得自己的副本，内核消费者解释 `kernel.arg`，PID 1 启动路径把 `init.arg` 作为 `argv[1..]` 交给现有 `spawn_user_task(path, argv)`。首版未知项规则允许旧加载器忽略未来增加的键。

## 验收标准

1. 两个普通镜像可在同一个构建根按任意顺序生成，ESP 中的 EFI 和内核逐字节一致，配置内容符合上表。改变 RPi5 配置只重做 RPi5 镜像。RPi5 构建在未准备 QEMU 固件时仍成功。
2. x86_64 通过原有分辨率配置及无配置启动。`make test-host` 覆盖共享 `boot_context` ABI，现有 DTB 解析用例和页表检查通过。验证错误配置、无可信 UART、DTB 保留内存边界、virtio 跨块以及已有 Normal/Device 映射的失败或成功语义。
3. QEMU 默认单核进入可交互 BusyBox，四核通过现有 SMP 验收。向 QEMU 提供 RPi5 镜像时看到平台不匹配或受控的 DTB 缺失诊断，进入内核前退出，不访问 RPi5 UART。
4. RPi5 真机使用配置中的排针控制台，正常经过 `ExitBootServices()`、M0、M1 双高地址窗口、GIC、计时器和设备阶段。现有根设备缺失时允许明确的 `FATAL stage=fs component=root rc=-19`。没有板测记录时，验收需标明真机待测。
