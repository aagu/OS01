# USB Boot HID 键盘与鼠标实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 在 OS01 x86_64 上让开机接入的 USB Boot 键鼠驱动现有 TTY 和 GUI，并能与同一 Hub、同一 xHCI 上的 USB 启动盘同时工作。

**Architecture:** 利用现有 xHCI 筛选和配置 Boot HID；一个运行时工作线程经共享 event ring 完成 Interrupt IN，并通过既有 input route 将完整 Set 1 帧和鼠标快照交给 TTY/GUI。USB 2 Hub 状态端点也走同一个异步传输通道；失联后合成释放并保留仍可能被 DMA 使用的内存。

**Tech Stack:** C、现有 xHCI/PCI/TT/scheduler/DevFS/gfx、宿主 C 框架、Python unittest、QEMU q35/OVMF/QMP。

**Spec:** [2026-11-15-usb-hid-input-design.md](../specs/2026-11-15-usb-hid-input-design.md)。实施前读全文。

## 全局约束

- x86_64；首版全机一键盘、一鼠标；LS/FS/HS 单事务 Interrupt IN。直连或一层非 SuperSpeed USB 总线 Hub；支持复合 Boot 键鼠，同一 MSC BOT slot 只绑定存储。
- 仅 Boot Protocol；无 USB repeat、LED、滚轮、附加设备热枚举或重新绑定。保留 PS/2、串口、MSC 与现有用户态 ABI。代码遵守 AGENTS.md 目录和日志规则；新头为内核私有。
- 启动阶段不发常驻 HID TD；运行时一个 BSP kthread，10 ms 请求周期，控制器忙时跳过，每控制器每轮最多 drain 64 个事件。一个端点最多一个未完成 TD / 一个完成记录，DMA 释放须有硬件所有权证据。
- 宿主新增测试标注 @os01-test，按子系统加 link 规则；每次修改清单执行 make regen-test-catalog，把生成文件随对应任务提交。所有命令从仓库根执行。每任务先 RED 后 GREEN；仅提交该任务列出的文件及其生成项。

## Review Focus

1. USB E0 帧夹在 PS/2 两个字节间，不能污染 PS/2 framer 或跨租约错投（任务 2）。
2. MSC 在同一复合配置中遇到无效 HID wMaxPacketSize 或重复地址，仍有正确的 bulk IN/OUT（任务 3）。
3. 两处 STOP 等待与 HID 短包撞在一起，双方都收到自己的事件且 ERDP 不重走（任务 4）。
4. Hub 位图短、Hub 自身失效或 child 已拔出但旧报告仍在缓存，不能留下修饰键或鼠标按钮（任务 5）。
5. QMP 接受注入但 guest 没读到 USB；串口回显、陈旧 marker 或备用磁盘不能给测试造假（任务 6）。

---

### 任务 1：纯 Boot 报告差分和 Set 1 编码

**Files:** 新建 kernel/driver/usb_hid_report.c、kernel/include/driver/usb_hid_report.h、hosttests/cases/test_usb_hid_report.c；修改 hosttests/mk/drivers.mk、kernel/Makefile、mk/components/run.mk。

**Produces:** usb_hid_keyboard_t（零初始化，8 个 modifier 和六键集合）；宏 USB_HID_MAX_FRAMES=20；struct input_ps2_frame 等效类型沿用 input_ps2_frame_t。
- int usb_hid_keyboard_update(usb_hid_keyboard_t *state, const uint8_t *report, size_t len, input_ps2_frame_t out[USB_HID_MAX_FRAMES]); 返回帧数或 -EINVAL。
- size_t usb_hid_keyboard_release(usb_hid_keyboard_t *state, input_ps2_frame_t out[USB_HID_MAX_FRAMES]); 输出当前状态的释放并清空。
- int usb_hid_mouse_decode(const uint8_t *report, size_t len, uint16_t mps, mouse_event_t *out); 成功返回 0，无效返回 -EINVAL 且不修改 out。

- [ ] 写生产解码测试：test_keyboard_boot_diff 用 {0x10,0,0x04,0x04,0,0,0,0} 得到 E0 1D、1E；重复输入为 0 帧；再发数组 {0x01,0,0,0,0,0} 且 modifier=0，只得 E0 9D；全零恢复时得到 9E。7/9 字节键盘不改变状态。断连释放七个已持有输入（六普通键和一个 modifier），Caps/PrintScreen 各不重复，Pause 上升发一次 E1 且 release 无帧。固定对照 HID Usage Tables 与现有 PS/2 Set 1 表，至少断言右 Alt、方向键、数字区 Enter、F12 和完整 PrintScreen/ Pause。test_mouse_boot_prefix 用 {5,0xfe,0x7f,0x55}、MPS=4 得 buttons=5、dx=-2、dy=127、wheel=reserved=0；长度 2 或长于 MPS 拒绝。
- [ ] 注册宿主用例与驱动 link rule，根 make 加能力门和 target test-usb-hid-report，通过 os01_submake 调用 hosttests 目标。运行 make PROFILE=x86_64-clang test-usb-hid-report，确认仅因尚无解码实现失败。
- [ ] 实现解码。非修饰 usage 满足映射才加入集合；按 spec 的四组顺序升序输出，整个状态计算后一次提交。帧输出完整 canonical 字节、is_make、is_special 和 physical_key；start_epoch 由 route 决定。PrintScreen 的物理身份仅由后续 route 私有识别；保持 special frame 的 physical_key=0。x86_64 才纳入新实现；从 sysroot header manifest 排除新私有头。
- [ ] 原命令重跑，期望宿主测试 0 失败。生成清单并提交 feat: decode USB boot keyboard and mouse reports。

### 任务 2：不依赖 i8042 的双源输入路由

**Files:** 修改 kernel/driver/input_route.c、keyboard.c、mouse.c；kernel/include/driver/input_route.h、keyboard.h、mouse.h；hosttests/cases/test_input_route.c、test_mouse_init.c、test_i8042_demux.c 及对应 hosttests/mk；需要时同步 kernel/include/uapi/gfx.h 的过期注释（无布局、数值变动）。

**Consumes:** input_ps2_frame_t、mouse_event_t。**Produces:**
- void input_route_keyboard_usb_frames(const input_ps2_frame_t *frames, size_t count);
- void input_route_mouse_usb_event(const mouse_event_t *event);
- void keyboard_input_init(void); void mouse_input_init(void); 只初始化 route / NORMAL sink / 队列，不访问 i8042。原 keyboard_init()/mouse_init() 先调用对应公共初始化，保留原探测/回滚契约。

- [ ] 在 GUI host 测试中加入 test_usb_batch_ps2_partial：PS/2 收 E0、USB 提交完整 1E、PS/2 收 1D；观察完整 1E 与 E0 1D，且 lease 切换时 PS/2 既有 DESYNC 不被 USB 清除。test_dual_owners 断言 PS/2 Ctrl + USB Ctrl 只产生一次 make，任一方单独释放不产生 break，最后释放产生一次；PS/2 typematic 连发仍进 NORMAL；Caps 只在 aggregate 0→1 翻转。bind 时持有的键、PrintScreen 和按钮不能通过另一源来一次 make 绕开隔离；USB release batch 保留 PS/2 持有的 Ctrl/按钮，对独占 USB Shift/按钮看到 break/释放。跨 epoch 的 PS/2 break 到达时，若 USB 仍持有相同键，保持隔离且 ACTIVE 依旧 DESYNC；随后 PS/2 再按或 repeat 不泄漏。鼠标 PS/2 wheel 不随 USB 运动重发。测试 PS/2 init 失败后仍能通过新入口正常读键鼠事件。
- [ ] 运行 make PROFILE=x86_64-clang test-gui-host 做 RED，预期新的路由接口缺失。i8042 实际初始化 fixture 仍须验证失败时不留下已安装的硬件回调。
- [ ] route 上锁后，键盘 ordinary owner mask 按 Set 1 identity 存两位，单独给 4 字节 PrintScreen 序列保留内部 identity，例如 0x1ff，Pause 继续不跟踪。PS/2 跨 epoch 路径先更新该源与 aggregate 的物理状态，再沿用隔离和 DESYNC，不能因聚合边沿被过滤而静默吞掉跨 epoch；只在 aggregate 1→0 清该键的 isolation，跨 epoch make 只在新 aggregate 边沿时设置 isolation，不改变另一源的既有 ownership；USB 批次借用当前 epoch，每帧通过同一 publish helper。鼠标每源存按钮，发布当前事件位移与 OR 后的按钮。幂等 sink 初始化不得重新初始化已经被 IRQ、lease 或 poll 使用的对象；host fixture 显式重置测试状态。保留原 input_route_keyboard_byte 和 input_route_mouse_event 作为 PS/2 入口。
- [ ] 重跑 test-gui-host 与 make PROFILE=x86_64-clang test-host；现有和新测试都通过。提交 feat: route USB and PS/2 input through the existing lease。

### 任务 3：先筛 Interface，再建立 xHCI endpoint contexts

**Files:** 新建 kernel/include/driver/xhci_hid.h、kernel/arch/x86_64/driver/xhci_hid_desc.c、hosttests/cases/test_xhci_hid_desc.c；修改 kernel/arch/x86_64/driver/xhci.c、xhci_hub.c，kernel/include/driver/xhci.h、xhci_hub.h，kernel/Makefile、hosttests/mk/drivers.mk、mk/components/run.mk。

**Produces:** enum xhci_hid_kind { XHCI_HID_KEYBOARD, XHCI_HID_MOUSE, XHCI_HID_KINDS }; struct xhci_hid_candidate { bool valid; uint8_t iface, dci, ep_addr, interval; uint16_t mps; }; slot 存 hid_candidates[XHCI_HID_KINDS] 和 uint8_t hub_status_dci（0 未识别）。生产筛选入口 int xhci_hid_scan_config(const uint8_t *cfg, uint16_t len, uint8_t speed_class, struct xhci_hid_candidate out[XHCI_HID_KINDS], uint32_t *hid_skip_dcis); skip DCI 位图包括所有未选 HID DCI，但不标记非 HID 占用的 DCI；第二遍建 ring 还须依据当前描述符所属的接口（避免被同号的坏 HID 抢占）。

- [ ] test_hid_scan_config：配置含非 Boot interface 0、键盘 interface 3 alt 0 endpoint 0x82、其 alt 1 endpoint 0x84、鼠标 interface 8 endpoint 0x83，断言仅选接口 3/DCI 5 和接口 8/DCI 7。乱序接口选各类编号最小的有效者；低编号键盘 MPS=7 或鼠标 MPS=2、高编号分别为 8/3 时选择高编号。HID 使用 endpoint 0/地址保留位、重复 DCI、端点数不符、错误类型/MPS/interval、越界长度的对照配置分别无候选或报 -EINVAL。test_mixed_msc_invalid_hid 将非法 interrupt MPS 和合法 BOT 端点构造在同一 cfg，生产 ring setup/Configure Endpoint 仍仅得到 bulk，MSC 候选值不变；HID 与 bulk DCI 碰撞始终由 bulk 占有。用更高的无效 HID DCI 验证 Configure Endpoint 的 Add Flags 不含该位，Context Entries 等于最高 active DCI；全部非 EP0 被过滤时为 1。
- [ ] test_interrupt_context_payload 在生产 context 中检查 LS MPS 8、bInterval=10 → Interval=6 / ESIT=8；FS MPS 64、bInterval=8 → 6 / 64；HS MPS 64、bInterval=4 → 3 / 64；原 Hub HS 多事务、SS companion 和 bulk 的 context 编码保留（SS 取 companion，bulk Max ESIT=0）。Hub 状态 DCI 取 Hub alt 0 验证过的唯一 Interrupt IN，未猜 0x81；测试 HS Hub + LS/FS 子口的既有 speed class、TT slot/port 保持。
- [ ] 加根目标 test-xhci-hid-desc 和对应 link rule，运行 make PROFILE=x86_64-clang test-xhci-hid-desc 做 RED。
- [ ] 在 xhci_setup_endpoint_rings 同一原始 cfg 缓冲调用筛选，合法性按 LS MPS≤8、FS≤64、HS≤1024；键盘 MPS≥8、鼠标 MPS≥3，先验证尺寸再选接口。LS bInterval 10..255、FS 1..255、HS 1..16；HID wMaxPacketSize 的事务/保留位必须为零。SET_CONFIGURATION 仍复用既有顺序。MSC BOT alt 0 存在即过滤所有 HID（即使其自身端点资料有问题）。Hub 路径复用现有唯一状态 endpoint 校验，缓存实际 DCI 移交任务 5；刷新配置/释放 ring 清理候选。构造 context 前以已成功创建的 active ring 重算最高 DCI（无则 max_dci=0、Context Entries=1），而不是使用筛选前描述符的最大值。配置长度或接口结构损坏无法归属时拒绝，保证配置边界和 buf 长度一致。
- [ ] 重跑专用 host 和 make PROFILE=x86_64-clang test-host，现有 phase2/3、MSC 与 Hub 案例不回归。重新生成 catalog，提交 feat: select boot HID endpoints during xHCI enumeration。

### 任务 4：共享完成事件与异步 Interrupt IN

**Files:** 修改 kernel/arch/x86_64/driver/xhci.c、kernel/include/driver/xhci.h、kernel/include/driver/xhci_hid.h；新建 hosttests/cases/test_xhci_intr_in.c；修改 hosttests/mk/drivers.mk、hosttests/mock/arch9/xhci_runtime.h、mk/components/run.mk。

**Consumes:** slot/endpoint ring 和 DMA 资源管理。**Produces:** struct xhci_intr_in_result { uint32_t cc; uint16_t actual; bool bad_length; }; 以下函数的 _locked 调用者在运行时已持 ctrl->io_lock，boot 单 BSP 例外：
- int xhci_intr_in_prepare_locked(struct xhci_controller *ctrl, uint8_t slot, uint8_t dci); 校验并初始化绑定状态及 DMA，无 doorbell。
- int xhci_intr_in_submit_locked(struct xhci_controller *ctrl, uint8_t slot, uint8_t dci);
- int xhci_intr_in_take_locked(struct xhci_controller *ctrl, uint8_t slot, uint8_t dci, uint8_t *dst, size_t cap, struct xhci_intr_in_result *out); 返回 1 取出、0 未完成、负值调用无效；cap 不足不消费。只为成功/Short 且长度合法的记录复制数据。
- void xhci_intr_in_retire_locked(struct xhci_controller *ctrl, uint8_t slot, uint8_t dci);
- unsigned xhci_intr_in_pump_locked(struct xhci_controller *ctrl, unsigned budget); 仅从无同步 owner 的 worker 中调用。
- static bool xhci_event_pop_locked(struct xhci_controller *ctrl, struct xhci_trb *out); raw cycle + acquire + ERDP；static bool xhci_intr_in_dispatch_locked(struct xhci_controller *ctrl, const struct xhci_trb *event); true 表示已归该异步端点（包括应丢弃的异步 stale event）。

- [ ] test_async_interrupt_interleavings 通过生产等待路径，分别将 HID 0x82/DCI 5 的 SHORT_TX 事件放在 Command Completion、EP0 Status、bulk、xhci_stop_ep_and_wait_locked 与 xhci_recover_bulk_timeout 的各自期待事件之前；期待事件仍匹配，HID take 恰好一次。ED=1、错误 pointer、错误 slot/DCI、同一完成重复、退役后迟到均不被当作有效报告。跨 event 255→0 与 transfer 254→Link→0 验证 cycle 和 ERDP。
- [ ] test_interrupt_ownership：没有 event 反复 pump，空闲 TD 保持 pending，不再响门铃；正常接收 MPS=8、residual=5 得实际 3，residual=9 为 bad_length。Take 前不能 submit；retire 后不可提交且跳过已缓存结果。模拟 Disable Slot 不确认或 halt 失败，DMA page 保留；确认后释放恰好一次。非 AC64 地址超 32 位时从未响门铃。
- [ ] 注册 test-xhci-intr-in，运行 make PROFILE=x86_64-clang test-xhci-intr-in 做 RED。
- [ ] 三处消费循环用唯一 raw pop，再调用异步分流，再做原同步匹配。实现一个 MPS 的独立映射 DMA 页（单事务 MPS ≤1024 及页边界）；单 Normal TRB 的 IOC=1、CH=0，不使用 Event Data，TD Size=0。pending pointer 需含 wrap 归一化结果，在 cycle 位可见前写好；Event cycle 用 acquire，正常环更新沿用现有 barrier/ERDP 规则。STOP 完成和 STOPPED 类型不作为输入；测试路径保持原 STOP 语义。普通同步等待找下一目的事件，worker 无 owner 时可以丢弃过期同步事件，保留既有端口状态观测。probe 早期初始化 io_lock，移除懒初始化；生命周期 DMA 收尾接入现有 slot/endpoint 释放链，不能从软件 retire 推定 DMA 已停止。
- [ ] 专用 host 重跑，再跑 make PROFILE=x86_64-clang test-host；通过后生成 catalog，提交 feat: multiplex xHCI synchronous and interrupt completions。

### 任务 5：启动绑定、运行时与断连

**Files:** 新建 kernel/arch/x86_64/driver/xhci_hid.c、hosttests/cases/test_xhci_hid_service.c；修改 kernel/include/driver/xhci_hid.h、kernel/arch/x86_64/driver/xhci.c、xhci_hub.c、kernel/arch/x86_64/platform/boot.c、kernel/init/services.c、kernel/include/arch/x86_64/boot.h、hosttests/mk/drivers.mk、mk/components/run.mk。

**Consumes:** 任务 1–4 的公开接口。**Produces:** bool xhci_hid_mouse_bound(void); int arch_usb_hid_start_worker(void); void xhci_hid_detach_controller(struct xhci_controller *ctrl); 测试复用的生产步骤 void xhci_hid_service_once(uint64_t now_ns)。引导入口 static int xhci_hid_init(void)，通过 register_subsys_deps 注册可选 STORAGE 子系统 usb-hid，依赖 device-boot。

- [ ] test_bind_and_fail_closed：对 03:01:01 的 iface=3 只观察 setup {0x21,0x0b,0,3,0}，参数中无 SET_IDLE；接口 3 成功而同 slot 接口 8 STALL 时，这两个接口均不发布、slot 隔离，其他 slot 仍能尝试。分配失败、不匹配 ring、未 configured、MSC 混合 slot、拓扑超过一层均不 arm。验证 i8042 失败时 USB-only 仍初始化 sinks 并有 /dev/mouse 注册资格。到 scheduler runtime 才开始 submit，没绑定时不创建 kthread。
- [ ] test_monitor_precedes_payload 与 test_transport_loss：根口初始 CSC 只在 reset 前清且不写 PED；枚举后 CSC 置位、CCS/PED 任一清零、slot/TT/祖先不可用均使已缓存 HID report 无法发布。监视端点位图最短 ceil((nports+1)/8)，短/错误和 bit0 带来只在该 Hub 下的 fail closed；1<<port 仅释放对应子口，复合键鼠一起释放；无关位不释放、不清除，下次提交不得早于收到位图后 100ms。缓存同时存在断连和 HID 完成时先 retire；模拟 64 个事件之后才到 Hub change，首轮 HID 完成留存且不投递，后续先处理 Hub change 再丢弃旧报告；模拟并发 ctl teardown，摘链、release 和 worker 发布互斥且随后无 UAF；退役不写 Hub EP0、不发 CLEAR_TT_BUFFER。
- [ ] 注册 test-xhci-hid-service，请宿主 mock 的锁回调断言 route 被调用时 controller io_lock 未持有。运行 make PROFILE=x86_64-clang test-xhci-hid-service 做 RED。
- [ ] 选取配置完成且 transport usable 的 slot，按 spec 顺序；依赖 binding 时的有效端点和已配置 Hub monitor（登记最多两个，复用 slot 对应 monitor）。SET_PROTOCOL 以每请求 500 ms deadline 调用已有 EP0 API；在同 slot 的候选全部决策后一次提交软件绑定条目。只有协议及 DMA/monitor 准备成功才对 DevFS 声明可用。根口 reset 前清初始 CSC；Hub reset 现有 LS/FS/HS 最终 GET_STATUS 分支统一拒绝重新置位 C_CONNECTION。HID 不向父 Hub 额外发请求。绑定日志使用 log_info 并包含 BDF/root/route/slot/iface/dci/kind；每条退休原因只记一次。
- [ ] 在 x86_64_boot_device_nodes 使用 keyboard_input_init()/mouse_input_init() 做独立幂等兜底并放宽 mouse 节点条件。kernel/init/services.c 提供 no-op weak arch_usb_hid_start_worker，由 x86 覆盖；在 boot_services_start 内调用并把失败记为可选服务失败。worker 用 kernel_thread_on_cpu(fn, 0, 0)，check vfs_sched_ready；每轮释放全部锁后用 current->wakeup_ns + blocker_wait 的到期检查睡眠，不忙等。先挂所有所需 monitor，再挂 HID TD；每轮先验证拓扑/控制器，drain，处理 monitor、重新验证，再复制有效 HID；每次锁内工作限制为 64 个 event。若 pump 返回 64，保留缓存、不 take/发布或重挂 HID，直到后续轮确认本轮 drain 遇到空环，再执行 monitor → HID。scratch 拷贝放在 worker 私有状态或有严格尺寸限制的栈；不在 io_lock 下进入 route。
- [ ] 服务生命周期 mutex 序列化 registry 遍历、锁外 route 交付和 controller 摘除；lock order 为服务锁 → controller io_lock，释放 io_lock 才进 route。xhci_remove 摘除并解按后，才进入原 halt/slot/controller teardown；无 worker 的 boot 情形保持单 BSP，不从 idle 的 PID 0 获取 runtime mutex。对已经退休的端点保留 DMA，直到确认停止。在 host 断言每源结束只产生一次 release。
- [ ] 专用 host、make PROFILE=x86_64-clang test-host、make PROFILE=x86_64-clang test-static；0 失败。重新生成 catalog，提交 feat: start USB boot input and release it on disconnect。

### 任务 6：USB-only 输入和同 Hub 启动盘的 QEMU 验收

**Files:** 新建 qemutests/usb_hid.py、qemutests/test_usb_hid_runner.py、user/usb_hid_probe/{app.mk,main.c}；修改 qemutests/harness/qemu_cmd.py、qemutests/harness/qmp.py、qemutests/usb_boot.py、mk/components/run.mk、config/rootfs.mk、config/test-bridge.json、config/test-scenarios.json、.github/workflows/ci.yml、docs/build/build.md、docs/driver/driver.md。

**Produces:** QmpClient.wait_event(name, match, *, deadline) 返回匹配的 event 对象（match 接收 event 对象）；input_events(events, *, deadline=None) 保留旧调用。usb_boot.py 提取 inject_root_probe(image: Path, probe_elf: Path, guest_basename: str) -> None。build_qemu_x86(..., usb_input: str = "off") 接受 off/direct/hub，保持旧调用 token 不变；runner CLI: python3 qemutests/usb_hid.py --case direct|hub --qemu PATH --firmware FILE --disk FILE --probe-elf FILE --smp N --timeout 180 --log-dir DIR；根目标 make PROFILE=x86_64-clang test-qemu SUITE=usb-hid，用 direct/hub × SMP=1/2 单独目录运行。guest 命令 /bin/usb-hid-probe input NONCE 和 /bin/usb-hid-probe io NONCE。

- [ ] qemutests.test_usb_hid_runner 先写 topology 和 evidence 的正反测试。两项 argv 都必须包含 q35,i8042=off，只有显式 id=kbd/mouse 的 USB 相对键鼠，无 tablet；hub 用一个 xHCI 和一个 Hub，storage=usb，盘在 1.2、键鼠在 1.3/1.4，无 AHCI/IDE/virtio 块设备。测试缺失租约标记、原始事件丢边沿、串口仅回显 nonce、盘 I/O 没有在注入窗口活动、没有设备移除事件、早退或镜像改动都失败。用假 socket 测 DEVICE_DELETED 在命令响应前和之后到达，两者均保留并按 data.device=kbd/mouse 精确匹配；错 id 不满足等待。运行 python3 -m unittest qemutests.test_usb_hid_runner 做 RED。
- [ ] 实现 argv（direct 使用正常 AHCI 盘与 xHCI root 2/3 usb_version=2；hub 使用既有 usb_hub=True、usb_version=1、可写独占副本）。复用 ProcessSession/QmpClient、usb_boot.py 的 GPT/ESP GUID、拷贝与离线 ext2 注入路径；抽取有边界/分区大小校验的通用 probe 注入 helper，写为 /bin/<basename>、mode 0100755，并逐字节回读验证。沿用原 USB boot probe 路径和行为。所有 rootfs 变体均排除 usb_hid_probe；专用目标依赖 $(USER_ARTIFACT_DIR)/usb_hid_probe.elf，仅注入 job 私有镜像。用 QMP info usb 的端口/id 和实际 480/12 Mb/s 及 guest 的绑定拓扑联合校验。QMP 客户端保留有界 64 项 async 队列，等待单一全局 deadline，匹配既已缓存或随后到达的事件；归档设备移除命令和匹配事件。记录 source hash 前后对比、topology/QMP transcript、firmware identity、serial、JSON verdict；所有失败路径清理 QEMU。输入命令使用 QMP input-send-event 单步 down/up，由独立的串口窗口判断 shell 真正输出；串口只用来启动早期测试夹具、运行 guest probe/同步阶段，不能输入被计分的 keyboard canary。
- [ ] 实现 input probe：app.mk 设置 APP_SOURCES := main.c、APP_LDLIBS := -lgfx；用现有 gfx 客户端/ABI 获取 display owner、配置 view、绑定已打开的 /dev/keyboard 和 /dev/mouse；轮询并读取完整原始键帧、mouse_event_t 输出带 nonce 的可机器断言结果。probe 发 READY 后才注入；持租约时校验 E0 左方向键 make/break、x=+12/y=-3、左/右/中各 press/release。额外注入可打印 nonce 加 Enter：input probe 在 leased 键盘确认完整键帧，继承的 PTY slave fd0 临时设 O_NONBLOCK，持续观察至 lease 结束前确认无对应输入字节，复原 fd 标志；不以无串口回显代替此断言。基于输入回执节流来保证同步测试可重跑。先发送 Shift/左鼠标键按下、等 probe 收到后 device_del 对应 QEMU id，等待 DEVICE_DELETED 及相应合成释放；Hub 案例明确验证子口移除，保留挂同 Hub 的存储。为了同时证明正常 unbind 后 TTY 恢复，先完成一轮正常关闭租约的输入，再另起一轮做 detach。
- [ ] 直接案例 shell 验证：QMP 键入 echo nonce，含 Shift、故意键入后 Backspace、Enter；nonce 必须是新的独立输出行。QMP 启动 cat /dev/urandom >/dev/null 并发送 Ctrl-C，等待 SIGINT 证据与新 prompt，再用另一 nonce 证实 shell 存活；不要凭 QMP 应答或命令回显判 PASS。
- [ ] Hub 案例从串口验证启动盘 BDF/slot/route、ESP GUID 和 root 装载；用 io probe 后台反复写入确定内容到私有 USB root 的文件，SYS_sync 后回读校验并记录 IO_BEGIN/IO_PROGRESS/IO_END。由 host 在 IO_BEGIN/END 之间注入键鼠并要求若干 IO_PROGRESS 夹住实际 guest 输入回执，同时用既有 USB BOT trace 形状证明至少 WRITE(10) 和 SYNCHRONIZE CACHE(10) 成功，不能只检查缓存读写。trace 以独立字节游标和完整行分段：并发阶段起始于启动 I/O probe 前，等待该 probe 退出且观察到本阶段成功的 WRITE(10)→SYNCHRONIZE CACHE(10) CSW；结束游标也是后续阶段的基线。两个设备均移除后再启动单次写、sync、回读，只从该游标之后重新要求成功的 WRITE(10)→SYNCHRONIZE CACHE(10)，启动或前阶段 trace 不可复用，并验证同 Hub 的存储仍在线。
- [ ] 登记 x86_64/rootfs、nightly、exclusive、isolated、严格超时的 suite 和 scenario；root dispatcher 调用专用 runner，确保不会走通用 AHCI 注入、也不会在并行时复用同一镜像。用模块内 TEST_MANIFEST 登记 host-only unittest（不加 bridge harness_module）；同步 CI schedule 和运行文档，执行 make regen-test-catalog。
- [ ] 执行 python3 -m unittest qemutests.test_usb_hid_runner、make check-test-catalog check-ci-schedule、make PROFILE=x86_64-clang test-qemu SUITE=usb-hid。期望 4 个组合独立 PASS，source image 不变；仅有测试拓扑枚举不能视为通过。提交 test: verify USB boot input and shared Hub storage。

### 任务 7：集成复验与实机记录

**Files:** docs/driver/driver.md，必要时新增 docs/driver/usb-hid-qualification.md。

- [ ] 运行 make PROFILE=x86_64-clang test-host test-static test-harness；再运行 make PROFILE=x86_64-clang test-qemu SUITE=phase-0、SUITE=gui 和 SUITE=usb-boot（独立命令），验证默认 PS/2/GUI 和根启动。usb-boot 总 suite 按 mk/components/run.mk 的分支循环 readonly、dual、ahci、persistence、superspeed、hub；核对六个子结果（独立排查用 make PROFILE=x86_64-clang test-qemu-usb-boot USB_BOOT_CASE=hub 或 USB_BOOT_CASE=persistence）。用 make PROFILE=aarch64-clang test-aarch64 MODE=smp 验证新增私有代码未污染 ARM 构建。归档实际退出码和产物路径。
- [ ] 若有实机，记录控制器 BDF、固件、键鼠 VID:PID、速度和拓扑。直连以及 HS Hub + LS/FS HID 各测终端、GUI、设备拔出与 PS/2 回退，并验证同 Hub USB 盘读写。如果仅有 QEMU，标注实机未验证（尤其 Split/TT）；不以 QEMU FS Hub 结果替代实机。更新文档并提交 docs: record USB boot HID qualification。
