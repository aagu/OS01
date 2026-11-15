# USB Boot HID 键盘和鼠标设计

## 目标与范围

在 x86_64 上，开机已连接的 USB 键盘能操作终端和 GUI，USB 鼠标能移动指针并使用左、右、中键。PS/2 和串口继续工作；USB 存储可与 HID 共用一个 xHCI 控制器，包括从该控制器上的 U 盘启动。保持 /dev/keyboard 的 Set 1 流、TTY 行规程、8 字节 mouse_event_t 和 gfx 输入租约的现有契约；不加系统调用或设备节点。

首版支持一个 USB Boot 键盘接口和一个 USB Boot 相对鼠标接口（全机各一，按控制器、slot、接口号升序选择第一个成功绑定者）；允许同一复合设备的两个接口；含 alt 0 MSC BOT 接口的设备只绑定存储。支持直连或一个已由现有枚举器配置的外置 USB 2 总线 Hub（包括 FS USB 1.1 Hub）、LS/FS/HS 的 USB 2 HID、单事务 Interrupt IN。未绑定任何接口不启动工作线程。拔出或失联释放该输入源持有的键和按钮；插回须重启。USB 3 HID、更多 Hub 层级、热添加、运行时重新绑定、PCI 控制器热移除、Report Protocol、无线接收器专有协议、滚轮扩展、灯光同步、USB 自动重复和 NKRO 留到以后。PS/2 原有滚轮与重复不变。Caps Lock 仍按原有键入语义生效。

## 方案选择

- 复用 xHCI、USB 枚举和输入路由，使用 Boot Protocol。键鼠与 USB 启动链及 TTY/GUI 共用基础设施；代价是事件环必须同时服务 HID 和原有同步传输。采用此方案。
- 通用 HID Report 描述符解释器能支持平板、滚轮和更多设备，但不能省去 xHCI 并发与输入路由工作；留给后续。
- 固件服务或 QEMU PS/2 兼容模拟不能覆盖 ExitBootServices 后的原生 USB 输入。

## 枚举与接口绑定

由 x86 的可选 usb-hid 子系统消费 device-boot 已配置的 slots。复用 Hub 枚举及 endpoint ring，不重置或重新枚举设备。扫描实际完整配置描述符、当前配置的 alt 0，仅匹配接口 03:01:01（键盘）或 03:01:02（鼠标）。按接口归属寻找唯一合格的 Interrupt IN；拒绝重复地址、与其他 alt 0 接口碰撞、端点 0、非单事务、非法 MPS/interval 和畸形配置。核对已配置 ring 的 DCI、类型、MPS、Interval；键盘 MPS 至少 8，鼠标至少 3。为 USB 2 interrupt endpoint context 正确设置 Interval 和 Max ESIT Payload，保持 Hub 与存储语义。接口发现使用原始配置缓冲，在建立和提交 endpoint context 前筛掉 HID 接口不合格或冲突的端点，保存候选元数据，独立于 lsusb 的可选描述符副本和 slot->iface 的 interface 0 记录。与非 HID 端点碰撞时保留非 HID 端点；含 MSC BOT 的 slot 不配置 HID 端点。若配置边界已损坏到无法隔离接口，允许按原有枚举失败路径拒绝该配置。

SET_CONFIGURATION 成功后执行接口请求 SET_PROTOCOL(0)（0x21/0x0b，wIndex=bInterfaceNumber，无数据）；不成功则不绑定。首版只发 SET_PROTOCOL；设备默认 Idle 下的重复报告由差分消除。失败后不在该 slot 继续发 EP0 或绑定 HID：非 split STALL 也将 slot 标记不可用于传输，保留相关 DMA；同 slot 先前协议成功的候选也不发布。沿用 split EP0 收尾及控制器隔离，HID 不主动 disable 整个 slot 或重置控制器。仅在同一控制器或 TT 本身不安全时才可牵连其他 slot。绑定按 BDF、root、route、slot、interface、DCI、kind 记录一次日志。

## 运行时传输与回收

- 每个受管 Interrupt IN 端点（HID 或 Hub 状态端点）最多一个未完成 Normal TRB（IOC=1、CH=0，不使用 Event Data）；独立 DMA 页，遵守既有 AC64、边界及内存屏障。请求长度为合法 MPS；先建立 pending 身份，再发布 cycle 和 doorbell。NAK/无输入是正常状态，不设键鼠 TD 空闲超时。
- 每个 controller event ring 只有一个受同一 io_lock 保护的消费入口。现有命令、EP0、bulk 和两处 Stop Endpoint 恢复循环都从该入口取事件；在等待方筛选前分流已登记的 HID / Hub 状态端点；仅 slot + DCI + TRB 地址匹配且 ED=0 的 pending 事件可形成报告。未知/陈旧事件不算其他完成；停止事件仍由相应恢复等待方匹配。每端点保留至多一个已完成记录，只有工作线程取走后才排下一个 TD。读取 cycle 后读取事件/报告，推进 ERDP 含 EHB。
- 单个 BSP kthread 服务已绑定端点，在 TTY 安装、VFS 结束 bootstrap 且 scheduler runtime 已就绪后启动；非 idle 工作线程确认 vfs_sched_ready()。io_lock 在 controller probe 时初始化，避免运行时首用竞争。用 scheduler blocker 每 10 ms 请求一次轮询（实际由调度 tick 唤醒，无追赶）。普通服务用已初始化的锁 trylock，繁忙时下轮重试；每控制器每轮最多 64 个事件。锁内复制完成报告和更新状态，锁外解码及发布。不开新 IRQ，不做同步 HID IN 等待。
- 成功/Short Packet 用 residual 算实际长度；越界或过短报告不产生输入，长度异常不影响下一个正常报告。终止性端点错误退役接口并释放输入源；不对周期端点执行 CLEAR_TT_BUFFER（USB 2.0 §11.24.2.3 仅适用于非周期端点）。出队和提交前检查 controller running/poison/halt、xhci_slot_transport_usable()（含祖先及 TT）、根口 CCS/PED/CSC；失效先丢弃已缓存报告，再释放受影响接口。USB 2 根端口在现有枚举 reset 前用中性的 PORTSC 写入清除初始 CSC（不写 PED/其他 W1C 位），之后不清新 CSC；绑定前也复查，避免把枚举后的重插当成原设备。
- 一个直连 USB 2 Hub 共用一个已配置的 Interrupt IN 状态端点，最多两个 Hub 监视器；持续接收 Hub 状态位图，运行时不做 Hub EP0 轮询。基线使用现有 Hub 准备/复位阶段对初始 C_CONNECTION 的清理、以及复位后 CONNECTION+ENABLE 的确认；修正现有复位检查，拒绝确认时已再次置位 C_CONNECTION 的子口。HID 绑定不加额外 GET_STATUS 或清除动作，预备监视器后才挂子口 HID TD，枚举基线之后锁存的变化由状态端点处理。监视器按 ceil((nports+1)/8) 校验 MPS 与最小实际位图长度；位图缺失、过短或传输错误按监视器失效隔离。运行时收到受管子口的任何变化位（包括重新接上），保守地退役该子口的两类 HID；bit 0（Hub 自身变化）或监视器失效则退役所有受其保护的 HID。只含无关口位的报告延后 100 ms 重挂以限制持续变化包，其他正常完成下轮重挂。先处理监视器变化，再交付该轮缓存的子口报告；运行时不清 Hub 变化位，不影响存储端口。
- 退役保留 ring、DMA 页和 tombstone，直到确认 Disable Slot 或控制器 halt；同一 session 不复用退役端点/TRB 内存。PCI 热移除不在首版范围内，现有 teardown 先用 HID 服务生命周期锁与工作线程的锁外路由阶段同步，摘除 controller 并释放对应源，再执行既有 halt/保留 DMA 路径。服务锁在 io_lock 外层，输入路由不在 io_lock 内。无 USB HID 时原有启动、日志和节点行为保持不变。

## 报告与输入路由

键盘要求恰好 8 字节。按集合比较六个 usage（忽略零、重复和未映射 usage）；重复报告不生成事件。数组含 0x01..0x03 时保留上次非修饰键集合，只更新 modifier 字节；恢复后与保留集合求差。正常报告顺序是普通键 break、修饰键 break、修饰键 make、普通键 make，组内按 usage 升序。映射标准 US 键区、Enter/Backspace/Tab/Esc、Caps Lock、F1–F12、导航/方向、数字小键盘及左右修饰键。扩展键用 E0（如右 Ctrl 为 E0 1D/E0 9D），PrintScreen 使用现有完整 make/break 特殊序列，其聚合与隔离使用独立内部身份，不改变帧的公开语义；Pause 只在报告上升沿发一次 E1 特殊 make，释放/退役无 break。USB 一个报告的完整帧批次在同一 route 锁和 epoch 下发布，绕过 PS/2 字节 framer。

普通键物理拥有者按 PS/2、USB 两源存储；首个持有者产生 make、最后释放者产生 break；已由 PS/2 持有的重复 make 保留原 typematic。USB 拔出清除本源持有状态，其他源仍持有的项保留。PS/2 跨 epoch 序列继续使用现有 quarantine/DESYNC 规则。输入都走原租约判断，NORMAL 继续送 TTY/设备队列，ACTIVE 继续原 ABI、隔离、授权、溢出和唤醒规则；另一设备也不能绕过租约对已隔离键的屏蔽。

鼠标接受 3..MPS 字节：buttons 取低三位，dx/dy 为第二、第三字节的符号扩展，正 Y 向下；wheel=0、reserved=0，额外字节忽略。路由将各源按钮快照按位 OR 后做现有租约隔离；位移和滚轮只来自当前事件。USB 拔出时发布零位移/滚轮和剩余源按钮快照。原 NORMAL sink 的初始化与硬件探测拆开；/dev/mouse 在 PS/2 或 Boot HID 鼠标可用时注册；USB-only 键盘仍接同一 console TTY。

## 验收

| 层级 | 证据 |
|---|---|
| Host | 用生产实现覆盖复合接口/alt/畸形描述符/非 Boot、坏 HID 与 MSC 复合设备保住存储端点、MPS/interval/Max ESIT、普通同步等待与两处恢复等待间插入 HID 事件、环 wrap/cycle、NAK 空闲、残量、STALL/拔出/祖先或 TT 故障后不再投递、Hub 状态位图与无关端口隔离、未确认 DMA 停止时保留。覆盖键盘顺序/rollover/E0、双源同键/同按钮、租约隔离/跨 epoch、断连释放与 PS/2 继续工作。 |
| QEMU，USB 直连 | q35 i8042=off，qemu-xhci + usb-kbd + usb-mouse，无 usb-tablet。用 QMP 键入 nonce shell 命令、Shift、Backspace、Enter，并证实 Ctrl-C 终止前台任务、shell 可继续接受 USB 输入。用实际 input 租约读出 USB 键帧、鼠标有符号位移和三键按下/释放；释放租约后 TTY 恢复。按住修饰键和鼠标键拔除，观察合成释放。 |
| QEMU，共享存储/Hub | 一个外置 usb-hub 后放 HID 及 USB-only 引导盘，共用一个 xHCI；运行时交叠 USB 盘 I/O 和输入，验证启动盘来源、读写校验与输入完整；直连与共享覆盖 SMP=1、2，并验证 Hub 子口拔出。直连显式 usb_version=2 并核对 HS；Hub 后显式 usb_version=1 并核对 FS，模拟 FS Hub 不能证明 HS Hub 的 Split/TT。 |
| 回归/实机 | USB boot、PS/2 GUI、phase-0、host、static 检查继续通过；手动记录至少一台 xHCI 实机直连及经 HS Hub 的 LS/FS 键盘鼠标身份、速度和结果；无实机明确标注未验证。 |

参考：[USB-IF HID 1.11 §7.2、附录 B/F](https://www.usb.org/sites/default/files/hid1_11.pdf)、[Intel xHCI 1.2b §4–7](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf)、[QEMU USB 设备](https://www.qemu.org/docs/master/system/devices/usb.html)、[QMP 参考](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html)。
