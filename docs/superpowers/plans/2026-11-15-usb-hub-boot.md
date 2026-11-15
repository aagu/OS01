# xHCI USB Hub Boot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 冷启动时找到 xHCI 外部 Hub 下的 BOT U 盘，按固件 ESP GUID 挂载同盘 ext2 根并运行用户态。

**Architecture:** 在现有 `device-boot` 同步轮询阶段逐层枚举；根端口用 PORTSC，外部端口用父 Hub EP0。复用现有 MSC/BOT 及 GUID 选盘；脏 TT 和地址零的异常在进入下一个事务前处理。

**Tech Stack:** x86_64 UEFI、xHCI C 驱动、USB 2.0/USB 3.2、C hosttests、OVMF/QEMU Python harness。

**Spec:** [2026-11-15 USB Hub Boot Design](../specs/2026-11-15-usb-hub-boot-design.md)。执行者同时阅读 spec；规范章节以该文档链接为准。

## Global Constraints

- x86_64 UEFI、冷插入、USB 2.0 与 verified Gen1×1（5 Gb/s）Hub、512 字节逻辑扇区、BOT `08/06/50`。保留 USB 直连/AHCI。无 hotplug、UAS、4Kn、HID、boot_context ABI 或 GUID 选盘策略修改。
- 以 controller 为所有权边界。每层所有 Hub/TT 初始化或明确失败后再往下一层；Hub 深度 0–4，叶子最多 5；每级扫描端口 1–15，原样保存真实端口数。USB2/SS 同伴不合并。HS TT 只做 alt 0 single-TT。
- 默认 controller Hub 扫描预算 10 s，端口去抖预算 500 ms 且连接稳定至少 100 ms，复位完成预算 1 s；每类复位最多一次。锁不能跨长延时，等待使用单调时间；清理有独立有界预算。
- 遵守项目源码/头文件分层与日志规则；新的 boot 逻辑留在现有 PCI probe / 子系统依赖内。只有确认停止或成功 Disable Slot 才释放相关 DMA。
- 从仓库根运行命令。计划执行时以隔离 worktree 开始；若 spec 尚未跟踪，先将当前已评审的 spec 和计划一起纳入执行分支，确保任务与 reviewer 可读，不改 spec 内容。每个实现任务红→绿→独立评审→提交；不要把测试失败预期改成接受旧行为。
- 下文 **H** = `make PROFILE=x86_64-clang test-host`；**K** = `make PROFILE=x86_64-clang test-kernel-layout`；**P** = `python3 -m unittest qemutests.test_harness_qemu_cmd qemutests.test_usb_boot_runner`；**Q(c)** = `make PROFILE=x86_64-clang test-qemu-usb-boot USB_BOOT_CASE=c`（替换 c）。红阶段记录点名的新失败，绿阶段确认命令退出 0。对于新增内部函数/TU，先用测试阶段的声明和仅返回 `-ENOSYS`/拒绝的占位定义让 fixtures 链接，或从已有生产入口触发；内部分支可经 `OS01_HOST_TEST` 薄包装访问。红阶段须到达断言，不能以缺符号或构建失败作红；占位定义不得进入绿提交。

## Review Focus

- 旧的复位完成位、持续无关事件、10 ms jiffies 回退可能提前成功/超时 → Tasks 1、4。
- 同控制器另一段 Supported Protocol 重用 PSIV，或同 BDF slot ID 之外的其他控制器复用 slot ID → Tasks 2、5、6。
- 已复位但未寻址的 USB2 设备仍能转发地址零 → Tasks 2、4。
- BOT 进入类 Reset 或直接判死时，脏 TT 不得影响共享 TT 的其他盘 → Task 3。
- 4 字节 SS 端口状态无法证明 Gen1×1；已是 U0 也不能证明设备未被固件寻址 → Task 6。

## File map

| 路径 | 职责 |
| --- | --- |
| `kernel/arch/x86_64/driver/xhci.c`、`kernel/include/driver/xhci.h` | 控制器/slot、事件/EP0、拓扑/逐层扫描、上下文和 TT 恢复；保持已有外部使用者接口。 |
| `kernel/arch/x86_64/driver/xhci_hub.c`、`kernel/include/arch/x86_64/driver/xhci_hub.h`（新） | Hub 描述符与 BOS 验证、Hub 类请求与端口状态；xHCI 架构源码由现有 wildcard 收录。 |
| `kernel/arch/x86_64/driver/xhci_msc.c` | 继续注册已配置 BOT slot；增加拓扑关联日志。TT 接在 xHCI 传输层，`kernel/driver/usb_msc_bot.c` 与 `kernel/fs/boot.c` 的协议/行为不改。 |
| `hosttests/cases/test_xhci_{init,phase2,phase3}.c`、`hosttests/mock/arch9/xhci_runtime.h`、`hosttests/mk/drivers.mk` | 现有 fake DMA/MMIO 扩展，脚本化 clock/事件/请求；新的 Hub TU 用 `XHCI_HOST_CFLAGS` 编译，链接到全部四个现有消费者 `test_xhci_init.elf`、`test_xhci_phase2.elf`、`test_xhci_phase3.elf`、`test_usb_devfs.elf`，不新增测试 catalog 条目。 |
| `qemutests/{usb_boot.py,test_usb_boot_runner.py,test_harness_qemu_cmd.py,harness/qemu_cmd.py}`、`mk/components/run.mk`、`docs/build/build.md` | USB-only Hub gate、验收日志、现有 suite 接线与操作文档。 |

## Task 1: 可控 EP0、事件超时和 EP0 MPS

**Files:** `xhci.c`、`xhci.h`、`test_xhci_phase2.c`、`test_xhci_phase3.c`、`xhci_runtime.h`（路径见表）。

**Interfaces:** 在 `xhci.h` 添加 `struct xhci_setup { uint8_t type, request; uint16_t value, index, length; }`，`int xhci_control_in(struct xhci_controller *, uint8_t slot_id, const struct xhci_setup *, void *dst, size_t capacity, uint64_t deadline_ns)` 和 `int xhci_control_no_data_until(struct xhci_controller *, uint8_t slot_id, const struct xhci_setup *, uint64_t deadline_ns)`；前者返回已复制字节或负 errno，后者返回 0 或负 errno。两者对应内部 `*_locked` 形式供 Task 3 使用，签名相同。既有 no-data/descriptor 接口继续可调用。内部 `int xhci_wait_min_ns(uint64_t duration_ns, uint64_t deadline_ns)`；Task 4 需要时在私有头声明。以 0 表示旧包装器的默认预算，Hub 调用一律传实际绝对值；现有事件函数保留包装器。命令内部 `enum xhci_cmd_state { XHCI_CMD_NOT_SUBMITTED, XHCI_CMD_COMPLETED, XHCI_CMD_IN_FLIGHT_UNKNOWN }`、`struct xhci_cmd_result { enum xhci_cmd_state state; struct xhci_trb event; }` 和 `int xhci_submit_cmd_until_locked(struct xhci_controller *, const struct xhci_trb *, uint64_t deadline_ns, struct xhci_cmd_result *)`：0 表示已收到匹配的 completion，调用者判 completion code；负 errno 保留提交状态，event 仅 COMPLETED 有效。

- [ ] **写失败测试。** `test_ep0_class_in_until`：`{0xa3,0,0,3,4}` 产生相符的 Setup 和 IN Data/OUT Status，返回 4 并在 Status 成功后拷贝；短读 2 返回 2，多余 residual、错误 slot/DCI/TRB 或丢 Status 不算成功，timeout 保留 DMA。加 `test_xhci_deadline_on_noisy_ring`：无关事件不断推进时仍返回 `-ETIMEDOUT`，jiffies 同一 tick 不触发旧 loop 上限。加 `test_xhci_command_deadline_ownership`：已过期不发布 command、不敲门铃、不 poison；敲门后超时则中毒且保 DMA；匹配拒绝仍为 COMPLETED 并保留 completion code；Stop 等待跨无关事件只接受本 pipe 的匹配事件。加 `test_xhci_fs_mps_and_address_recovery`：初始 8，首读 8，MPS=32 时 Evaluate Context 只加 A1 并更新 MPS，随后取完整 18；非法 MPS/短于 8 不继续；从 USB2 Address 成功到首个请求至少 2 ms。
- [ ] **跑红：H。** Fake clock 可单步推进，脚本化生产 ring 的 completion；原无类请求/无绝对 deadline/初始 FS MPS 64，至少分别观测一个失败断言。
- [ ] **实现共用路径。** 统一 EP0 IN 与 no-data 的 setup、匹配和拷贝；使用逐字段 TRB 编码，不依赖 C struct padding。deadline 在每轮（含消费无关事件）和发布新 TD 前检查；校准 TSC 或已推进的 PIT/jiffies，量化最小等待加一 tick；冻结时钟用有限取样判失败而非模拟时间。共享设备描述符路径用 LS/FS=8、HS=64、SS=512 初始化，FS 只允许 8/16/32/64，HS/LS 校验固定值，SS 编码 9；用 Evaluate Context 更新 EP0，不重置 dequeue。要求完整 18 字节后才信任身份；保存 Config descriptor bytes 后方可发覆盖 staging 的请求。保留既有 DMA 检查。Enable/Address/Evaluate/Configure/Disable/Stop 的 Hub 路径都下传同一绝对截止时间至命令发布和完成轮询；直接 enqueue 的 Address、Stop 路径也遵守提交状态合同（Stop 扩展匹配 stopped Transfer Event）。预提交检查最晚在第一次写 command TRB 前；已经发布则必须完成/进入所有权不明，不能因预算过期跳过门铃后直接释放。回滚命令另取有界 cleanup deadline；所有权不明时不借清理预算重用同一 cmd ring。
- [ ] **跑绿：H、K、Q(readonly)。** 现有 USB 直连和 ring wrap/错误匹配用例通过。
- [ ] **提交：** `xhci: unify deadline-aware EP0 requests and validate EP0 MPS`。

## Task 2: 拓扑、Slot Context 与默认地址回滚

**Files:** `xhci.c`、`xhci.h`、`test_xhci_init.c`、`test_xhci_phase2.c`、`test_xhci_phase3.c`。

**Interfaces:** `struct xhci_location { uint32_t route; uint8_t root_port, parent_slot, parent_port, depth, speed_id, speed_class, tt_hub_slot, tt_port; }` 存于 slot；旧 `port_num/speed` 若保留只作为 root_port/speed_id 的兼容视图。增加 `int xhci_child_location(const struct xhci_controller *, uint8_t parent_slot, uint8_t port, uint8_t speed_class, struct xhci_location *)`；`int xhci_enumerate_at(struct xhci_controller *, const struct xhci_location *, uint64_t deadline_ns, uint8_t *slot_id)`（xHCI 内部，返回时只输出可用 slot；Task 4 在内部增加 seed 参数供严格控制器重建）； `struct xhci_slot_alloc_result { uint8_t id; bool disable_confirmed; bool uncertain; }` 跟踪失败后 slot 归属，raw disable 接收硬件 ID（含软件对象尚未分配）。Task 4 复用同一默认地址门禁，增加外部 USB2 隔离回调。

- [ ] **写失败测试。** `test_xhci_child_route_and_tt`：root 4 路径 2→3 为 0x32；第五 nibble 合法、端口 16/第六层拒绝；FS Hub→FS 孙子继承最近 HS slot 及 HS 出口端口；无翻译字段为零。 `test_xhci_psi_root_scope`：相同协议大版本的两段 capability 各有重叠 PSIV，不串段；非默认 FS 唯一值可反查，显式无匹配/歧义为错误。 `test_xhci_default_target_rollback`：slot/Output Context 预分配失败无 Enable、无 DMA 发布；Enable Slot 成功后其他 base DMA OOM 必须看到有效 DCBAA 已发布才 raw Disable；Reset 成功但 Address 失败时，slot 禁用与 USB2 root PR 已停止且 PED 隔离都确认才允许第二个地址零操作；Disable 超时保留 DCBAA/DMA。
- [ ] **跑红：H。** 记录 route、PSI、失败后发送 Disable Slot 的断言。
- [ ] **实现 common enum + 根端口门禁。** 用 capability 段关联根端口，保存 explicit PSI 的速率/协议/对称信息及可逆映射，无表才用该段标准默认映射。Address Device 保存 root/route/Speed ID，LS/FS 仅在翻译时设 TT 父 slot/port；后续 Configure Endpoint（包括 clear-halt 的 Configure Endpoint）复制/维持合法拓扑字段，使用 32/64 字节现有 helper。MaxSlotsEn = min(硬件、软件 32)，另记录硬件与启用数。默认地址事务从发出 reset 开始，在成功 Address 或失败隔离后关闭；隔离失败锁死本控制器新复位/寻址。USB2 root 通过正确 PORTSC 写掩码禁用 PED，回读断开，或保持连接且 PR=0/PED=0；运行中的复位可暂时清 PED，不能作为隔离证明。SS 维持路由独立和 xHC Default slot 约束。Enable Slot 前先分配 slot 对象及 DMA 验证过的清零 Output Device Context；成功返回合法且未占用的 ID 后，先发布 DCBAA 再申请 Input Context/EP0 ring 等或发布任何目标 slot 命令，包括 raw Disable。随后 OOM 走按 ID Disable；确认为 Disabled/已停机才清 DCBAA 和释放。成功返回的 ID 不合法或已占用，禁止覆盖 DCBAA 并停止控制器扫描。尚未提交 Enable Slot、明确 No Slots Available 和超时未知所有权必须区分。Config 失败仅回收已完成寻址的 slot，不将其当作仍待寻址。Task 4 前 `xhci_probe_ports()` 仍只运行根端口。
- [ ] **跑绿：H、K、Q(ahci)、Q(superspeed)。** 现有直连 SS 回归需成功。
- [ ] **提交：** `xhci: represent device paths and isolate failed addressing`。

## Task 3: 在启用 Hub 以前完成 shared TT 安全恢复

**Files:** `xhci.c`、`xhci.h`、`xhci_msc.c`、`hosttests/mk/drivers.mk`、`test_xhci_phase2.c`、`test_xhci_phase3.c`。

**Interfaces:** 每个 HS Hub slot 有 `enum xhci_tt_state { XHCI_TT_READY, XHCI_TT_CLEARING, XHCI_TT_QUARANTINED }`；内部 `int xhci_tt_clear_locked(struct xhci_controller *, const struct xhci_location *, uint8_t dev_addr, uint8_t ep_addr, uint8_t ep_type, uint64_t cleanup_deadline_ns)`（ep_type 为 USB 0 Control/2 Bulk，不是 xHCI EP type）。Bulk ring 记录恢复前半段是否完成、端点是否还禁止发 TD，以及最近一次已结清 timeout 的结果；`xhci_clear_halt()`/`xhci_recover_bulk_timeout()` 保持签名。另有 `bool xhci_slot_transport_usable(const struct xhci_controller *, uint8_t slot_id)` 检查 controller、slot 及依赖 TT/祖先是否可用（不要求 configured，供 EP0 使用）；MSC 在原 configured/BOT 判定之外也调用它。将 `xhci_msc_init()` 的每控制器循环提取为 `void xhci_msc_probe_controller(struct xhci_controller *, uint8_t *next_disk)`，生产 init 与 fixture 共用；仅在 `OS01_HOST_TEST` 暴露该入口并省略注册。

- [ ] **写失败测试。** `test_xhci_split_address_failure`：TT slot 查的是 HS 祖先，HS 出口非 1 时发送给 HS Hub 的 `{0x23,8,0x0000,1,0}` 和 IN 方向的 `value=0x8000`；清成功兄弟可用，失败共享 TT 上兄弟被门禁，HS 非翻译 sibling 不受影响。 `test_xhci_split_bulk_cleanup_order`：addr 7、EP IN 2 发 `value=0x9072`，事件顺序 Halted Reset Endpoint→清 TT→返回错误→可选 BOT 类 Reset→device clear halt→Set TR Dequeue；同 TT sibling 和类 Reset 不得在 CLEARING 发 TD。 `test_xhci_split_timeout_terminal`：连续第二次和不重试的写入同样 Stop/结清或保留 DMA 并隔离；无重复停止、无新门铃；未提交和正常短包不清 TT。 `test_xhci_split_ep0_terminal`：Halted EP0 不发 Stop；成功 Reset Endpoint 后才清两个方向，Running 要对应 Stop/transfer 证明；两者均不重启废弃的 EP0，证明失败则隔离。 `test_xhci_msc_skip_unusable_path` 链接生产 `xhci_msc.c` 与 fake `usb_msc_probe()`：保留的 configured BOT 子 slot 依赖的 TT 已隔离时 probe 调用数为 0；独立的健康 slot 仍调用一次。
- [ ] **跑红：H。** 现有 split 路径无清理记录。
- [ ] **实现同步结清。** Address Device USB Transaction Error 保留 completion code，以地址零、EP0 双向清；控制/数据运行期从 Output Slot Context 取 USB 地址。使用 Task 1 锁内 no-data helper，只有 HS Hub 非 split 访问可清它自身的 TT。已提交的错误在 xHCI I/O 锁内（启动单 BSP 时为同一序列化域）停稳并清 TT 或 quarantine 后才返回；不能以 BOT 回调是否执行来决定清理。按 spec 区分 Halted 前/后半阶段和 Stop 完成的超时，原回调读结清结果，不能重发 Stop；分裂 EP0 在放弃前保存清理键：Halted 用 Reset Endpoint（TSP=0）确认 Stopped，禁止对 Halted EP0 用 Stop；Running 用 Stop + 匹配事件。若选择 Disable 则只允许仍无孩子的 slot 且必须完成，不能越过叶→父约束；清完也不重启废弃 pipe。TT 失败阻止全 TT 新枚举和传输；MSC eligibility 在任何 BOT/SCSI 命令前查同一谓词，slot/祖先所有权不明时也拒绝。fixture 用 `XHCI_HOST_CFLAGS` 编译 MSC adapter 并仅链接到 `test_xhci_phase3.elf`；使用 fake probe 观测而不替换 eligibility。命令超时按控制器中毒处理；正常直连路径顺序不变。
- [ ] **跑绿：H、K、Q(persistence)。** host fixture 用生产 transfer/recovery 路径；直连写入重启读回仍通过。
- [ ] **提交：** `xhci: settle shared TT errors before releasing split traffic`。

## Task 4: USB 2.0 Hub、端口策略与逐层扫描

**Files:** 新 `xhci_hub.c`、`xhci_hub.h`；改 `xhci.c`、`xhci.h`、`hosttests/mk/drivers.mk` 和 `test_xhci_phase{2,3}.c`。

**Interfaces:** 私有 `struct xhci_hub_info { uint8_t nports, pwr_good_2ms, ttt; uint16_t characteristics; bool superspeed, global_overcurrent; }`；`int xhci_hub_read_info(struct xhci_controller *, uint8_t slot, uint64_t deadline_ns, struct xhci_hub_info *)`；`int xhci_hub_prepare_ports(struct xhci_controller *, uint8_t slot, const struct xhci_hub_info *, uint64_t deadline_ns, uint16_t *candidate_mask)`（bit 1–15 对应端口）；`int xhci_hub_reset_port(struct xhci_controller *, uint8_t slot, const struct xhci_hub_info *, uint8_t port, uint64_t deadline_ns, uint8_t *speed_class, bool *reset_issued)`；`int xhci_hub_isolate_usb2(struct xhci_controller *, uint8_t slot, uint8_t port, uint64_t cleanup_deadline_ns)`。Task 6 扩展同一 API。 `xhci.c` 内 `int xhci_configure_hub_slot(struct xhci_controller *, uint8_t slot_id, const struct xhci_hub_info *, uint64_t deadline_ns)` 实现 slot-only Configure Endpoint / 回读；只在已完成的命令拒绝或读回不符返回 `-EAGAIN`，提交前已超时或提交后所有权不明均返回原错误。Task 4 将 Task 2 的内部枚举签名扩充为 `int xhci_enumerate_at(struct xhci_controller *, const struct xhci_location *, const struct xhci_enum_seed *seed, uint64_t deadline_ns, uint8_t *slot_id)`；`struct xhci_enum_seed { struct xhci_hub_info hub; const uint8_t *identity; size_t identity_len; }`，NULL 走普通枚举，非 NULL 在首个 Configure Endpoint 前核对 Device+Config 并应用 Hub 字段。

- [ ] **写失败测试。** `test_hub_usb2_controls`：收到 0x29、长度/可变位图和 alt 0；FS interrupt MPS=8 → MaxBurst=0/ESIT=8，HS 合法 2 transactions×64 → 1/128，非法长度/端点/bInterval/高带宽码拒绝。ganged port 2 未上电应发送 PORT_POWER；Hub global 过流不复位；2×25 ms 的最小等待、100 ms 去抖、共用空端口窗口。 `test_hub_usb2_reset_and_isolation`：先清旧 C_RESET → PORT_RESET → 新 C_RESET + PED/CCS → 清变化 → 10 ms 恢复；掉线或失能经回读才允许换端口；CCS=1、PED=0、RESET=1 后又变 PED=1 的脚本不得误放行；失联父 EP0 或复位无法证明结束时，锁死该控制器新寻址。脚本化同一 FS Hub 两个兄弟在 OOM 与 SetAddress error 后都必须看见回读隔离才继续。 `test_hub_tier_and_rebuild`：两同层 Hub 都 ready 才有下一层 Address；A0-only 不修改端点；拒绝或输出未生效只重建一次，核对身份/Hub 属性，超时无 retry，深度 5 Hub 不排入，叶到父清理且未停孩子保住父资源。
- [ ] **跑红：H。** 将新的 Hub TU 加入 host 链接；先用已有 fake MMIO/TRB 脚本驱动失败断言，不用 mock 掉生产遍历。
- [ ] **实现 USB2 Hub 协议和 context。** EP0 `0xa0 GET_DESCRIPTOR 0x2900`，`0xa0 GET_STATUS` 为 Hub、`0xa3 GET_STATUS` 为端口，均请求 4 字节 status；端口 SET/CLEAR_FEATURE 用 0x23。验证 HS single-TT alt0 及产生的 status interrupt IN Endpoint Context；保存 Hub descriptor 实际端口数，验证位图容量。先 configure endpoint/SET_CONFIGURATION 再读 Hub descriptor；Slot-only A0-only 保持原 context entries/端点、检查输出字段；仅 `-EAGAIN` 时保存 seed（复制到独立内存，不引用将被回收的 slot），按 spec Disable→强制复位→重新寻址、核对 Device+Config→首个 Configure 写 Hub 字段+端点→SET_CONFIGURATION 并重读 Hub 确认。没有上电扫描/子 slot 前才允许这条路。Hub 分支端口状态逻辑依 spec；空口共用 100 ms 重扫窗口，10 ms 采样；去抖确认后才触发单个在途 reset。
- [ ] **接入扫描和发布不变量。** 根端口形成第 0 层，BFS 队列容量 `MaxSlotsEn`、只保存已完成 context/TT 与逻辑上电等待的 Hub；层尾快照为下一层屏障。10 s 从第一个 Hub 建立开始，交给所有 Hub 命令/重建/等待；局部 deadline 裁剪，安全清理另外最多 1 s/失败分支，超时中毒停止该控制器。端口 reset 已发出时，Task 2 默认地址门禁持有到 Address 成功或 slot＋物理 USB2 port 均隔离；`xhci_hub_isolate_usb2` 验证 CCS/PED/RESET，仍连接时须确认 RESET=0 后 CLEAR_FEATURE(PORT_ENABLE)（如仍使能）并回读 RESET=0/PED=0；超时或 EP0 无法读回即隔离不可确认。直到子设备可用才保留配置候选；已枚举孩子的父若不可用，禁用该分支进一步访问并安全回收。初始化失败只影响该树枝或归属控制器。此任务同时生成 Task 5 约定的 `[xhci] device` 与 `[xhci-hub] ready` 两个日志，以便先做真实拓扑冒烟。
- [ ] **跑绿：H、K、Q(readonly)。** 在 Task 5 正式 runner 前，用以下只读 USB-only 启动冒烟检查。限时退出 124 可以接受；日志必须有 USB2 Hub 下游、容量、GUID 选 `usba` 且 `/boot ok`、runtime ok，无 root FATAL。

  ```bash
  set -e
  make PROFILE=x86_64-clang disk.img
  fw=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^firmware=//p')
  img=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^image=//p')
  test -r "$fw" && test -r "$img" || exit 1
  serial=$(mktemp /tmp/os01-hub-smoke.XXXXXX.log)
  qemu_rc=0
  timeout 45s qemu-system-x86_64 -M q35 -m 512 -smp 2 \
    -drive "if=pflash,format=raw,readonly=on,file=$fw" \
    -drive "file=$img,format=raw,if=none,id=usbdisk,readonly=on" \
    -device qemu-xhci,id=xhci,p2=4,p3=0 \
    -device usb-hub,bus=xhci.0,port=1 \
    -device usb-storage,drive=usbdisk,bus=xhci.0,port=1.2,bootindex=0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
    -serial "file:$serial" -display none -no-reboot -no-shutdown || qemu_rc=$?
  test "$qemu_rc" -eq 0 || test "$qemu_rc" -eq 124
  rg -a '\[xhci\] device .*root=1 route=0x00002 .*iface=08:06:50' "$serial"
  rg -a 'usb_msc: usba: [0-9]+ sectors, 512-byte blocks' "$serial"
  rg -a 'boot disk selected by GUID \(usba\).*\(/boot ok\)' "$serial"
  rg -a 'checkpoint=runtime status=ok' "$serial"
  if rg -aq 'FATAL stage=fs' "$serial"; then exit 1; fi
  ```
- [ ] **提交：** `xhci: enumerate USB 2 hubs in tiers during device boot`。

## Task 5: 可复现的 Hub 启动门禁与日志关联

**Files:** `xhci.c`、`xhci_msc.c`、`qemu_cmd.py`、`usb_boot.py`、两处 Python runner 测试、`mk/components/run.mk`、`docs/build/build.md`。

**Interfaces:** Python `build_qemu_x86(..., usb_hub: bool = False)` 只允许 USB-only、USB 2.0 组合；`hub_argv(qemu, firmware, image, smp)` 按既有 `usb_argv` 参数类型与只读挂接行为；`hub_evidence(serial: str, expected_guid: str, root_marker: str, esp_marker: str) -> bool`。新增 `--case hub`、`USB_BOOT_CASE=hub`。日志契约：在 USB 设备配置成功时 emit `[xhci] device bdf=<bb:ss.f> root=<r> route=0x<5hex> slot=<s> parent=<p> port=<d> speed=<class> speed_id=<psiv> iface=<cc:ss:pp>`；成功绑定时 emit `[xhci-msc] disk=<name> bdf=<bb:ss.f> slot=<s>`，原有容量日志保留。Hub ready marker 为 `[xhci-hub] ready bdf=<bb:ss.f> root=<r> route=0x<5hex> slot=<s> protocol=<usb2|ss> ports=<n> ttt=<n>`，错误含同一位置与阶段。HCD 两个 marker 由 Task 4 落地；Task 5 增加 BOT candidate/attempt/failure 与成功绑定日志，按 BDF+slot+盘名关联。缺接口明确记录 `no-bot-interface`（仅对发现 MSC 但不满足 BOT 的设备或该 controller 汇总，避免每个非存储设备刷屏）；发 probe 前日志记录盘名，失败带 rc，复用现有 `usb_msc.c` 的 TEST UNIT READY、INQUIRY、READ CAPACITY 和注册错误日志来区分阶段，不改协议。

- [ ] **写失败测试。** `test_hub_argv_usb_only` 断言 `p2=4,p3=0`、Hub `port=1`、唯一只读 MSD `port=1.2,bootindex=0`，无非 USB 数据盘，旧 argv 不变，拒绝 SS 或 dual hub 组合。 `test_hub_evidence_chain` 用真实风格串口样本：UEFI 正确 GUID → 根端口 1 的 Hub ready → 同 BDF、root=1、parent=Hub slot、port=2、route=0x00002 的 BOT `08:06:50` → 同 BDF+slot 的 usba 容量和绑定 → GUID 选 usba/root、/boot → prompt、两个镜像标记、再次 prompt。删掉任一必需阶段、互换顺序、slot 相同但 BDF 不同、错误 parent/port、伪造直连 route 或 Fatal 均拒绝。FakeSession 验证只在 ready 后送一次 marker 命令、失败保留日志、清理私有盘。
- [ ] **跑红：P。** 无 hub case 时明确失败。
- [ ] **接线。** 用现有 `prepare_image()`、GPT GUID/marker 生成及私有目录，保持源镜像 SHA 前后校验。Runner 的 argparse、准备、argv、prompt、evidence、错误退出均加 hub。已有 `_guid_boot_evidence` 可复用 GUID/挂载/marker 逻辑，拓扑检查限定同一有序启动链；接受真实 BDF 格式。Make 专用目标 case 校验与 suite 的 readonly/dual/ahci/persistence/superspeed 循环同加 hub；帮助及 build 文档修正旧的 M0-only 描述，firmware case 不进常规 suite。若其他 OVMF 版本未到 OS01 日志，明确报告 firmware gate 失败；任何辅助 EFI loader 独立记作 kernel-only 诊断，不能放宽主门禁。
- [ ] **跑绿：P、Q(hub)、`make PROFILE=x86_64-clang check-test-catalog`。** QEMU serial/gate 证明唯一介质位于 Hub 后，并保存 `serial.log`、metadata、源镜像 SHA；不得用预置成功日志代替。
- [ ] **提交：** `test: prove USB-only boot through a QEMU hub`。

## Task 6: SuperSpeed Gen1×1 拓扑与端口复位

**Files:** `xhci_hub.c`、`xhci_hub.h`、`xhci.c`、`xhci.h`、`test_xhci_phase{2,3}.c`。

**Interfaces:** 扩展 Task 4 hub API，`xhci_hub_read_info()` 选 0x2a；私有 `int xhci_hub_check_gen1(const struct xhci_controller *, uint8_t slot, const uint8_t *bos, size_t actual)`、`int xhci_hub_set_depth(struct xhci_controller *, uint8_t slot, uint8_t depth, uint64_t deadline_ns)`。Gen1 验证失败使 Hub 不进队，USB 2.0 companion 独立照常。

- [ ] **写失败测试。** `test_hub_ss_gen1_boundary`：有效 BOS 有 SS Capability 且无 SSP；截断/坏 cap、SSP 宣告、未知/不对称 PSI 拒绝；4 字节 PORT_STATUS 不用来猜 SSP。 `test_hub_ss_context`：Interrupt IN MPS=8、companion `bMaxBurst=1,wBytesPerInterval=12` 得 Burst=1/ESIT=12；缺 companion、wBytesPerInterval=0 或 >16 拒绝。 `test_hub_ss_depth_and_reset`：发送 `{0x20,12,0,0,0}` 为首个 SS Hub depth，depth 4 可，5 不发；U0+PED 仍发 hot PORT_RESET，暖复位清旧两位、发 BH_PORT_RESET、见本次 C_RESET 和 C_BH_RESET 且 CCS/PED/U0 才寻址；缺本次完成位失败。拒绝上游 Hub SSP 分支不影响独立 USB2 companion。
- [ ] **跑红：H。** USB2 suite 仍可重复运行；新断言未实现。
- [ ] **扩展 SS。** 校验协议段 PSI 的对称 5 Gb/s SuperSpeed（不是仅速率 class）；默认表按版本且只接受确认为 Gen1×1 的 ID。标准 BOS GET_DESCRIPTOR 用长度头和第二次有界读，遍历已验证总长/能力边界；SSP Hub 不进入端口扫描。Hub descriptor 0x2a、slot context 与 SS interrupt companion 编码按 spec，SET_HUB_DEPTH 在配置/校验两侧成功之后、任何下游供电前。区分外部 USB3 PORT_RESET / BH_PORT_RESET 和根 PORTSC PR / WPR；严格 xHC 重建强制根复位，绕过原直连快速路径。下游 SS 只有在复位完成且最终 U0 可 Address，不把已有 U0 当 Default；不新增中断 IN TD 或 LPM 状态机。
- [ ] **跑绿：H、K、Q(hub)、Q(superspeed)。** 后者验证 USB3 直连回归；两者都不能充当 SS Hub 真机证据。
- [ ] **提交：** `xhci: boot through verified SuperSpeed Gen1 hubs`。

## Task 7: 验收并交付可诊断的结果

**Files:** `docs/build/build.md`；验收记录放到执行时的测试产物目录，注明 commit、profile、拓扑、镜像 hash 与日志路径。

- [ ] **跑完整软件门禁。** H、P、`make PROFILE=x86_64-clang test-static`、`make PROFILE=x86_64-clang test-qemu SUITE=usb-boot`。记录 suite 每个分支（含 hub）及源镜像未变的证据。失败先定位是固件、枚举、BOT、GUID 还是 root；不以直接 USB 成功替代 Hub。
- [ ] **准备真机镜像和观测。** `make PROFILE=x86_64-clang LOG_TARGET=both disk.img`；`make -s PROFILE=x86_64-clang print-run-paths` 确认镜像、`sha256sum` 固化。用已经确认可写且容量足够的专用测试 U 盘，保留其他磁盘；未确认目标介质时只交付镜像与 hash。用可正常运行的系统记录 USB 拓扑、速度、存储协议与逻辑扇区；先取完整 serial 或 framebuffer 日志，确认能观测用户态 ready。
- [ ] **执行真机矩阵。** 同一镜像、同一 BOT/512B 盘，冷断电重启：① 真直连对照；② HS Hub，至少记录 HS 链路；③ Gen1×1 SS Hub；④ 照片原接法（若含级联记各端口）；⑤ 至少一次保留不同 ESP GUID 的内置盘并明确从 USB 引导。每次保存固件 ESP GUID、BDF+route、MSC usba/容量、GUID 选 USB 和 ext2 根、/bin/init 与 ready 日志。没有可用硬件或原拓扑未证实时，把相应格标为未验证；HS TT 与 SSP 能力排除不能靠 QEMU 12 Mb/s 推断。
- [ ] **提交说明。** `docs: document hub boot qualification and diagnostics`。软件门禁通过与现场闭环分别报告；只有原接法进入用户态才关闭本次照片对应的真机故障。
