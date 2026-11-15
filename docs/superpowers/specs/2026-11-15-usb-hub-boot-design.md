# xHCI Hub 下游枚举与真机 USB 冷启动

状态：提案，待评审。本文补充 [USB 启动设计](2026-11-14-usb-boot-design.md)；将原设计排除的 Hub 拓扑纳入冷启动范围。

## 现象和目标

照片显示至少三个 xHCI 控制器已经运行，合计注册四个 USB 设备：两个 `1a40:0101`、一个 `2109:2817`（日志速度 3）和一个 `2109:0817`（速度 4）。四者都是 Hub（类 `09`），可见日志中没有存储接口或 `usb_msc: usba: ...` 容量行。最后停在 `stage=fs component=root rc=-19`。可选的键盘、鼠标初始化失败不解释这条根挂载错误。照片作为现场观测，不足以确定 U 盘所在的控制器、下游端口、速度或存储协议；也不能单凭照片证明 UEFI 是从哪块介质加载的。

当前 [xHCI 驱动](../../../kernel/arch/x86_64/driver/xhci.c)的 `xhci_probe_ports()` 只扫描控制器根端口；设完 Hub 的配置就返回。后续 [xhci_msc_init()](../../../kernel/arch/x86_64/driver/xhci_msc.c) 只接收已配置的 BOT 接口，没有候选也可以返回成功。因此 `usb-msc ... ok` 不是发现磁盘的证据。[fs_boot_mounts()](../../../kernel/fs/boot.c) 在没有可用磁盘候选时返回 `-ENODEV`；在 GUID 模式下，有候选且扫描完成但均不匹配是 `-ENOENT`。照片和代码指向一个明确缺口：如果启动盘在 Hub 后，内核无法发现它。最终要用同一 U 盘和实际拓扑验证这就是本次失败原因。

目标：x86_64 UEFI 从冷启动时已插入的 U 盘加载内核后，内核能通过 USB 2.0 或 SuperSpeed Gen1×1（5 Gb/s）Hub（含级联）重新发现 512 字节逻辑扇区的 BOT/SCSI 盘，按现有 EFI ESP 唯一分区 GUID 选择该盘，挂载同盘 ext2 根并运行用户态。保留 USB 直连和 AHCI；不引入热插拔、根盘拔出恢复、UAS、4Kn、新的 HID 驱动或 boot_context ABI。带内置盘的验收要求启动身份有效；当前无身份时的首盘兼容路径不作为真机双盘选对根的保证。

## 路线

- **推荐：启动阶段同步、有界地遍历 USB 树。** 延续现有 PCI probe 的轮询模型，存储注册一定先于根挂载。Hub 类请求与 xHCI 设备枚举分层；两个协议使用同一拓扑及 slot 生命周期。
- **先只做 USB 2.0。** 可作为交付检查点，也是用真机 USB 2.0 HS Hub 隔离问题的办法。照片同时出现 SuperSpeed Hub，不能把 USB 2.0 验收当成覆盖原接法。
- **异步 Hub 状态中断与热插拔。** 需要常驻事件处理、拓扑并发和块设备断开语义。对已经插好的启动盘无必要，留给独立设计。

## 启动集成与拓扑

```text
device-boot / PCI xHCI probe
  → 根端口检测与复位（PORTSC）
  → 公共枚举入口（Enable Slot → Address Device → 描述符 → 配置）
  → 完成本层所有 Hub/TT → 扫描下游端口 → 子设备走同一枚举入口
  → 所有控制器扫描有界完成
usb-msc（依赖 device-boot）
  → 按已配置 slot 绑定 BOT → SCSI 就绪/容量 → 注册磁盘
fs_boot_mounts
  → GPT / EFI ESP GUID → 同盘 ext2 根 → /bin/init
```

- 在 `kernel/arch/x86_64/driver/xhci.c` 提取接收位置和速度的公共枚举入口；根端口仍由 `xhci_port_reset()` 管理。把 Hub 描述符解析、请求和端口状态机放入 `kernel/arch/x86_64/driver/xhci_hub.c`，对应私有头放 `kernel/include/arch/x86_64/driver/xhci_hub.h`。沿用 xHCI PCI 注册和已有 `usb-msc` 子系统依赖，不改 `kernel_main`。每控制器使用显式有界的逐层队列（仅排入已就绪 Hub 的 slot，容量不超过已启用 slot 数）。按根端口及下游端口号递增处理；在该层全部 Hub 和 TT 完成初始化或明确失败后才处理下一层，符合 xHCI §4.3。同一控制器不并行复位、寻址或轮询请求；同一时刻只允许一个已经复位、等待寻址的目标；由下文的默认地址门禁约束整个控制器上的下一次寻址。
- 每个 slot 记录控制器、原始根端口、父 Hub slot、实际父端口及深度、20 位 Route String、速度类别和控制器所需 Speed ID。根设备 `depth=0, route=0`；子设备继承根端口，`route = parent.route | (port << (4 * parent.depth))`。例如根端口 4 下 Hub 的 2 号口再接 Hub，后者 3 号口上的盘为 `root=4, route=0x32`。最多写五个 nibble，深度 5 只接受非 Hub 叶子，外部 Hub 的深度限 0–4，越界不发 SET_HUB_DEPTH 或扫描下游；首版只遍历端口 1–15，超过范围明确跳过并保留描述符的真实端口数，禁止把多个端口别名成同一路径。超过深度、slot 或控制器预算都应可诊断。USB 2.0 和 SuperSpeed Hub 作为各自总线上的设备处理，不按 VID/PID 合并。
- Address Device 写 Route String、Root Hub Port、Speed ID；Configure Endpoint 保持拓扑与 TT 字段，按命令语义填写 Hub 位、端口数和必需的 TTT。LS/FS 经过 HS 时，TT 父 slot 指向最近的 HS Hub，TT 端口是该 HS Hub 通往下游路径的端口（可能先接 FS Hub），不能填当前设备的直接父端口。直接 FS/LS 或 HS/SS 设备不填上游 TT；首版 SS 只走同速 Gen1×1 Hub，故也不使用 xHCI Table 6-6 为跨速率等级定义的 SS/SSP Parent Hub/Port。首版选默认 single-TT alt 0，不宣称已启用 multi-TT；缺少可用 single-TT 配置则诊断并跳过。Hub 自身的 TTT 仅对 HS 有意义。
- USB 2.0 Hub 端口状态解码出 LS/FS/HS。首版 SS 分支只支持经核验的 Gen1×1 拓扑：根 Hub 的根端口 PSI 必须确认为单通道 5 Gb/s SuperSpeed（显式 PSI 核对链路协议和对称速率；默认表按该段协议版本解码）；每个 SS Hub 读并验证标准 BOS，必须有 SuperSpeed Capability 且不宣告 SuperSpeedPlus Capability。无法验证或支持 SSP 的 Hub 记录并跳过其 SS 分支；即使它当前在低速率运行，也不以 4 字节端口状态猜测子端口速率。USB 2.0 同伴 Hub 独立扫描。USB Hub 状态速度位不等同于 xHCI Slot Speed ID；按所在根端口的 Supported Protocol 映射 LS/FS/HS/Gen1×1，无唯一合法匹配即跳过。当前 PSI 按协议大版本共用的存储需保留每段 Supported Protocol 的归属；无显式 PSI 时用规范默认表。SSP Hub 的 8 字节 EXT_PORT_STATUS、BOS Sublink Speed Attribute、lane count 和跨速率等级路由留给后续。USB 2.0 Address Device 成功后等待至少 2 ms SetAddress recovery，再发第一个 EP0 请求（含直连公共路径）。FS 的 EP0 从 8 字节起步，先读设备描述符前 8 字节，校验 `bMaxPacketSize0`；必要时用 Evaluate Context 更新 EP0 MPS，再取完整描述符。HS/SS 的固定初值仍校验，SS 的字段为指数。

## Hub 建立与端口状态机

1. **控制传输。** 从现有 EP0 descriptor-in 与 no-data 路径提取通用请求接口，输入 setup 各字段、缓冲区容量、deadline；输出实际长度或错误。保留按 slot/DCI/TRB 指针匹配事件、成功等 Status Stage 和超时 DMA 所有权语义。锁内完成读回拷贝，不把 slot DMA 指针交给 Hub 扫描层。保存配置描述符原始字节后再发 Hub 请求，以免覆盖共享 DMA。新增请求复用同一 EP0 ring；一次只发一个请求。
2. **建立 Hub。** 识别 class `09` 的 Hub 设备及其有效 Hub 接口，按现有流程配置端点并 `SET_CONFIGURATION`，然后 class/device IN 取 Hub 描述符（USB 2.0 `0x29`，SuperSpeed `0x2a`）。校验类型、实际字节数、描述符长度、端口数、供电特征和可变 bitmap 边界。用只更新 Slot Context 的 Configure Endpoint 写 Hub 属性并检查 Output Slot Context 回读；这是 Linux xHCI 使用的顺序。USB 3.2 不保证配置前的 Hub 描述符请求，xHCI §4.5.2 又要求严格实现于首次 Addressed→Configured 转换时设置 Hub 属性；处理规则见下一段。两侧配置成功后，SuperSpeed Hub 发 `SET_HUB_DEPTH`（直接挂根端口的外部 Hub 值为 0）。状态变化中断 IN 端点即使没有 TD，也必须提供合法周期端点上下文：验证活动 Hub 接口的端点方向、类型、MPS 与速度相关的 bInterval；FS 用 Max Burst=0，HS 由 wMaxPacketSize 的高带宽事务数编码换算 Max Burst，USB 2.0 的 Max ESIT Payload=MPS×(Max Burst+1)；SS 必须有合法紧随其后的伴随描述符，Max Burst 取 bMaxBurst，Max ESIT Payload 取并校验 wBytesPerInterval。验证该端点能传输 Hub 的状态位图。现有生产代码只为 SS Bulk 读 companion 且未填写 Max ESIT Payload，这里要补齐，沿用 Bulk 原有行为。启动遍历只用 EP0，不提交中断 IN TD。
3. **供电和等待。** 对每个待扫描端口读取逻辑电源状态；未逻辑上电则逐口发 class/other `SET_FEATURE(PORT_POWER)`，包括成组供电与无物理电源开关模式，不能因为一组已有 VBUS 就假定其他端口已退出 Powered-off。遇到 per-port 过流跳过该端口；对按 Hub 全局报告过流的设备，供电前、供电等待后及复位候选端口前，先用 class/device IN `GET_STATUS` 读取四字节 Hub 状态与变化位；全局过流或全局状态无法确认则停止该分支，不反复上电。完成供电遍历后共用一次稳定等待，至少 `2 × bPwrOn2PwrGood` ms，扫描时重新确认相应端口已逻辑上电。x86 在 `device-boot` 前已初始化时钟并开放 PIT IRQ。使用 `arch_clocksource_read_ns()`（校准后的 TSC，未启用时退到 10 ms jiffies）；依赖 jiffies 时为最小等待加一个 tick 的量化余量。固定 CPU 循环数不能代替最小供电、去抖或复位恢复时间。启动 Hub 路径前确认时钟推进；不可用则放弃该分支并记录原因。
4. **探测与复位。** 以 class/other IN `GET_STATUS` 读取恰好 4 字节状态与变化位（小端），正确区分两代协议的状态布局。供电等待后先扫描全部端口；空端口共用一次有界重扫窗口，不为每个空端口单独等待。对已连接或正在变化的端口有界采样/去抖，连接至少稳定 100 ms。清除已观察到的 `C_PORT_*` 变化而非清除当前状态，发复位前先清对应的旧完成位，避免把固件留下的变化位当成本次结果。USB 2.0 发 `PORT_RESET`，等复位完成、仍连接且已使能，清 `C_PORT_RESET`，等待规范要求的复位恢复时间。SuperSpeed U0 只说明链路可用；UEFI 可能已为设备分配地址，不能由 U0 推断设备处于 Default。首次扫外部 Hub 的 SS 下游也要确保设备先复位再寻址：健康 U0 发 `PORT_RESET`（热复位），需重新训练的链路错误按规范发 `BH_PORT_RESET`（暖复位）；核对本次完成位：热复位需要新的 C_PORT_RESET；显式暖复位还需新的 C_PORT_BH_RESET（两位都预清并在完成后确认清除）。最终要求连接、使能、U0。对过流、掉线、持续训练或重置失败记录原因并跳过端口。外部 Hub 的端口操作始终通过父 Hub 的 EP0，绝不写根端口 PORTSC 来代替。
5. **子设备。** 只在端口稳定并通过验证后创建路径和子 slot。Hub 就绪后才扫描其端口；子盘完成配置后由现有 MSC 阶段绑定 `08/06/50`，不在 xHCI 枚举层注册块设备。任何失败分支均不进入 MSC 候选集合。

**严格 xHC 的兼容路径：** 若只更新 Slot Context 返回明确的命令拒绝，或命令完成但 Output Slot Context 未保存所需字段，允许一次有界重建。保留已验证的 Hub 描述符和身份数据，安全 Disable Slot，强制重置该 Hub 的上游端口并重新寻址（根端口的既有 SS/U0 快速路径也必须绕过）；复读标准 Device/Configuration 描述符并与缓存核对。由已缓存的 Hub 信息在新 slot 的首个 Configure Endpoint 一次写齐 Hub 与端点字段；SET_CONFIGURATION 后复读并核对 Hub 属性，匹配才继续。首次尝试期间未供电扫描且无已创建子 slot；控制器/命令超时等所有权不明的情况不走重建。该路径的触发、回读和失败要可测试；不使用按 VID/PID 推测 Hub 属性的表。

## USB 2.0 TT 错误恢复

- 把共享 TT 的状态挂在实际 HS Hub slot。针对 TT 后的 Address Device `USB Transaction Error`，保留 completion code 与拓扑，在允许下一个分裂事务前发 USB 2.0 Hub class/other OUT `CLEAR_TT_BUFFER`（bRequest=8；不是标准 CLEAR_FEATURE）。目标为实际 HS Hub 的 EP0，single-TT 的 wIndex=1，即使使用的 HS 下游端口不是 1；wValue 编码端点号 [3:0]、USB 设备地址 [10:4]、端点类型 [12:11]、方向 [15]，保留位为 0。Address Device 失败留下 Default，清地址 0 的 EP0，Control 双向均清；已寻址设备用 xHC Output Slot Context 的 USB Device Address，不用 slot ID；Bulk 清理使用失败端点的真实端点号与方向。清理后才允许继续扫描；失败目标仍走安全回收，不自动重复寻址。
- **在 xHCI 层同步结清。** 对已提交到 TT 后 Control/Bulk pipe 的 STALL、事务错误、超时或完成状态不明，在当前控制器序列化域内、返回原始错误给 BOT 或释放 I/O 锁前，将 TT 标记为清理中，停稳失败的 pipe 并完成 CLEAR_TT_BUFFER，或标记为不可用。确认没有提交 TRB 的本地错误、已正常完成的短包不触发 TT 清理。所有依赖该 TT 的入口都拒绝在清理中或不可用时提交新 TRB（包含其他 slot 和 BOT 类 Mass Storage Reset）；只有直接发往 HS Hub EP0 的清理请求可走锁内 helper，无递归加锁。结清不依赖 BOT 是否会调用回调或重试；第二次超时、写入不可重试等终止路径也必须执行。
- **分开 TT 结清和 BOT 协议恢复。** Halted Bulk 执行 Reset Endpoint → 清 TT，记录本次 pipe 已完成 xHC/TT 前半段，但在设备 ENDPOINT_HALT 与 dequeue 完成前不向该 Bulk pipe 再提交；BOT 仍按原顺序做类 Mass Storage Reset（若其状态机要求）及设备 clear_halt，clear_halt 回调接续后半段，不重复 Reset Endpoint 或清 TT。超时在锁内复用 Stop Endpoint 与匹配 Stop/Transfer Event 的 DMA 证明，必要时处理 Halted，再清 TT、恢复到可由 BOT 接管的状态；原 `recover_timeout` 回调读取该次结清结果，不重复停止。复用现有 BOT 的重试与写入策略；不能证明恢复条件时只允许失败退出。废弃的枚举/运行期 EP0 如不重启，先证明 Stop 或 Disable，再清 TT 并禁止该坏 pipe 继续使用。清理用独立有界预算，保留原始 I/O 错误；非 TT 路径保留现有协议顺序。
- 不能证明停稳、无法取得有效清理键或清 TT 失败时，将整个 single-TT 标成不可用（若是控制器层命令超时则停止整个控制器）。拒绝依赖该 TT 的后续枚举、MSC probe 和控制/数据传输；已发布盘通过现有 I/O 失败路径返回错误，保持引用和 DMA 所有权规则。TT 隔离不影响 HS Hub 上不经过该 TT 的 HS 设备或其他控制器，不以重新上电或 RESET_TT 扰动健康子树。

## 失败隔离与诊断

- 为 Hub 扫描设置控制器级总时间预算（建议默认 10 秒，从进入第一个 Hub 扫描阶段计）并下传绝对 deadline，单端口再设局部上限（去抖窗口 500 ms、复位完成 1 s，每类复位最多一次，均裁剪到剩余总预算）。限制遍历深度、端口数和 `MaxSlotsEn`。同步扫描不持有跨等待的控制器锁；单个 TRB 请求按既有方式序列化；轮询即使持续收到非目标事件也应检查 deadline，且在 jiffies 回退时不得因同一 tick 内达到旧循环上限而提前判超时。预算用完不创建新子 slot，可继续其他控制器及根挂载；DMA 清理允许独立的有界安全预算。
- **默认地址门禁。** 从发出端口复位到 Address Device 成功，只能有一个在途目标。发出复位后若失败或中止（含复位结果不明、速度不支持、预算耗尽、Enable Slot 无资源/软件状态分配失败、Address Device 失败），先完成该目标的 TT 清理（适用时）和两类隔离，再继续下一端口：(1) 确认未获 xHC slot，或对已获 slot 确认成功 Disable Slot；(2) 对 USB 2.0 下游端口，以父 Hub 的 `CLEAR_FEATURE(PORT_ENABLE)` 禁止转发并回读确认；USB 2.0 根端口用保留 PORTSC 其他写位语义的 PED 禁用并回读。已确认断开或失能的端口无需再发禁用请求。Disable Slot 只回收控制器资源，不能代替 USB 2.0 端口隔离。对于 SS，路由限定目标端口，但同样必须确认失败的 Default slot 已释放；若不能确认 SS 地址仍只会在该独立路由上被访问，则停止新寻址。任何必须的隔离无法确认时，禁止该控制器继续复位或进入地址零流程，保留仍可能由硬件访问的 DMA；其他控制器可继续。已有设备仅在其控制器及路径仍可信时继续供 MSC 使用。
- 管理上限与 `MaxSlotsEn` 统一为硬件能力和软件 slot 数的较小值；不要把日志的硬件上限当作实际启用数量。现有软件表能存 32，但启动只启用 16。补齐 Enable Slot 成功但随后分配失败的回滚。回收以确认 Disable Slot 或控制器停止为前提；控制器/slot 中毒则沿用保留 DMA 的语义，不清 DCBAA、不复用仍可能被控制器访问的 slot/内存。从叶到父清理；如果子 slot 无法安全停止，保留它依赖的父资源。端口错误跳过同层端口；Hub EP0 已停且不能恢复时结束该 Hub 分支；控制器命令超时结束该控制器扫描。
- 用 `log_info/warn/err/debug` 记录 PCI BDF、根端口和实际端口路径、父/子 slot、route、速度类和 Speed ID、Hub 协议/端口数/TT、阶段和错误。详细状态变化进 debug；空端口不刷 info。成功链应能关联 Hub 路径 → 子设备 `08/06/50` → MSC 块盘和容量 → GPT/ESP GUID 选盘 → 根挂载 → 用户态 ready。分开报告无 MSC 接口、BOT 不就绪/容量失败、GPT 不匹配和根挂载失败。

## 验收顺序

| 层级 | 验收 |
| --- | --- |
| 宿主测试 | 覆盖 1/2 层 route（含第五 nibble 和拒绝越界）、TT 经过 FS 子 Hub 的转接、无 TT 情况、PSI 非默认映射、BOS/SSP 分支拒绝、USB 2.0 与 SS 周期端点 Context 编码及非法 companion、FS EP0 MPS 更新、USB2/SS setup 请求与状态解析、成组供电的非首端口、全局过流、USB2 地址恢复等待、短/坏描述符、陈旧复位完成位、持续非目标事件、复位不完成和端口掉线、slot 耗尽及失败回滚、FS Hub 的同层兄弟在 Enable Slot 失败和 SetAddress 错误后只能在两类隔离确认后继续，并覆盖隔离失败阻止新寻址、Hub Context 两种建立顺序、同层两个 Hub 均就绪才访问下一层、TT 分裂失败后健康兄弟可枚举及清理失败隔离、BOT 分裂管道恢复顺序（同 TT 兄弟和类 Reset 在 pending 时均不得提交；第二次超时/不重试也结清或隔离）和 DMA 所有权不明。优先扩展现有 xHCI hosttests；若新增测试按目录规则生成 catalog。 |
| QEMU | 新增 `USB_BOOT_CASE=hub`：同现有 USB-only 隔离镜像，`qemu-xhci,p2=4,p3=0` + `usb-hub,bus=xhci.0,port=1` + `usb-storage,bus=xhci.0,port=1.2,bootindex=0`。禁止备用根盘；用串口核对子路径、BOT 块盘、正确 ESP GUID、根与用户态。可加双层作为第二个拓扑用例。既有 `make test-qemu SUITE=usb-boot` 的 readonly/dual/ahci/persistence/superspeed 继续通过。若 OVMF 不能从模拟 Hub 加载内核，另设隔离用例只从辅助盘加载 EFI 与 kernel，并明确提供 USB 目标 ESP 身份；该用例只验证内核链，不能标记为 Hub 上的 UEFI 启动通过。 |
| 真机 | 同一已知为 BOT/512B 的 GPT 启动盘先做直连对照；再用 HS Hub 或 USB 2.0 链路、Gen1×1 SuperSpeed Hub 链路分别冷启动，最后复现照片的接法。最好先从能正常运行的系统取得完整 USB 拓扑及存储接口协议。至少验证一次有不同 ESP GUID 的内置盘的情形，确认 OS01 挂载 USB 上的根。保留完整日志、物理接法、协商速度；照片的原接法启动进入用户态才算现场问题闭环。 |

已用本机 QEMU 11.1.3 与现成 x86_64-clang profile 镜像复现基线：monitor 显示 Hub `1` 和 MSD `1.2` 均为 12 Mb/s；USB-only 启动时 OVMF 从 `1.2` 加载，现有内核只配置 `0409:55aa` Hub，随后报 `fs/root rc=-19`。镜像以只读方式挂接。这证明现有镜像在模拟 Hub 后能通过固件阶段，并提供了可用于回归的内核失败基线。QEMU 这个 Hub 模型仅提供 LS/FS 下游，对 HS TT、真实电源和 SuperSpeed Hub 的训练不构成验收。顺序：先完成公共路径与 USB 2.0 门禁，再做 SuperSpeed 与真机验收；不以 USB 3 直连结果替代 SuperSpeed Hub。

规范及实现参考：[Intel xHCI Rev. 1.2b，§4.3、§4.5、§6.2.2](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf)、[USB-IF USB 2.0 §9、§11 与 USB 3.2 Rev. 1.1 §10](https://www.usb.org/documents?search=USB)、[USB 3.2 §10 PDF](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/138/USB-3.2-Revision-1.1.pdf)、[Linux xHCI Hub Context 更新](https://github.com/torvalds/linux/blob/master/drivers/usb/host/xhci.c)、[QEMU USB Hub 模型](https://github.com/qemu/qemu/blob/master/hw/usb/dev-hub.c)。
