# ARCH-9 Driver Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成 PCI 驱动绑定、多网卡共存和通用块层解耦，使可选设备缺失可跳过且真实故障不被掩盖。

**Architecture:** subsys 保留阶段调度；phase 6 coordinator 统一登记、枚举和绑定 PCI 驱动。硬件状态归属控制器/网卡实例，net/block 提供服务 ops；网络 post-SMP 接入 lwIP。安全回滚、DMA 隔离、readiness 与故障边界按 spec 实施。

**Tech Stack:** freestanding C、Clang/LLD、x86_64 PCI config ports、AHCI、legacy/transitional virtio-net、e1000、lwIP 2.2.0、host C tests、Python QEMU harness。

**Spec:** [2026-10-05-arch9-driver-model-design.md](../specs/2026-10-05-arch9-driver-model-design.md)，用户于 2026-10-05 确认，子 agent 三轮后 APPROVE。

## Global Constraints

- 用户明确要求：**可选设备不存在时可以跳过初始化**。
- AHCI 只匹配 class/subclass/prog_if = `01/06/01`；e1000 首期 ID `0x8086:0x100e`；virtio-net 首期 ID `0x1af4:0x1000`；modern-only `0x1041` 留为 UNBOUND。
- 每次每卡最多 64 个 RX 包；QEMU 静态地址 `10.0.2.15` 兜底只作用于默认接口。
- 无 NIC 不调用 tcpip_init、不启动 DHCP；非 ONLINE 的 socket 创建返回 `-ENETDOWN`；ONLINE 前接口查询为空/getifaddr 为 0。
- 根文件系统为正常 profile 的必需能力；无根盘在启动 init 前明确失败，不能标成可选 SKIP。
- phase 6 早于 GS；无无保护 this_cpu()/cpu_id()、无 pbuf/sys_arch_protect；post-SMP 后才建立 lwIP adapter。
- 不修改 boot_context 或 UAPI；新源目录/公开头目录对称，使用单一 kernel/include 根。
- 结构体变更执行 `make clean`；所有构建和测试从仓库根入口，产物仅进入 profile 目录。
- syscall 回归用 `make OS01_SYSTEST=1 test-syscall`，不混入 KERNEL_SELFTEST。
- 保留 `.pci_drivers` 注册段；段顺序不决定绑定；重复匹配不能依赖链接顺序挑选。
- 不实现热插拔、运行期卸载、动态 IRQ vector/shared INTx、aarch64 PCI/NIC、modern virtio、NVMe/USB、异步块请求、块缓存或 ARCH-8/10 全面改造。
- 新代码用 log_* / 现有 debug channel，不新增 serial_printk；设备失败不掩盖 errno，无法安全停止 DMA 时隔离或 DEVICE_UNSAFE。

## Review Focus

1. 坏 NIC 的 BAR/config 与健康根盘共存：隔离坏 function，不把可选硬件故障升级成全总线不可用（Task 2/5/11）。
2. AHCI 请求超时仍有 DMA，下一次请求到来：拒绝复用和自动重试写入，停止失败不归还页（Task 4）。
3. mailbox 持续非空且一张卡采用 POLL：各卡 RX 和 API 消息都能持续推进（Task 6/9/11）。
4. 无 NIC/所有 adapter 失败/完成回调延迟：socket 在 2 秒内返回 ENETDOWN，不接触未就绪 lwIP（Task 6/10/11）。
5. 早期 pre-GS 与运行期跨 CPU deadline：使用正确时间读数，零频率/inactive 在修改硬件前拒绝 AHCI probe（Task 4/5）。

## 文件职责与阶段

| 阶段 | 任务 | 主要文件 / 职责 |
|---|---|---|
| A | 1 | blockdev + GPT：完整 ops 注册，分区经父入口，稳定设备地址 |
| B | 2–5 | device core、PCI core/match、x86 backend、AHCI、boot coordinator：统一发现和安全绑定 |
| C | 6–10 | net device、lwIP adapter、两类 NIC、sys_arch、socket：逐实例收发与 readiness |
| D | 11–12 | QEMU matrix、边界审计、文档和完整回归 |

这些模块共享启动和资源契约，不作为互不关联的并行项目。按任务顺序执行。阶段 B 完成前网络仍走过渡兼容路径；Task 7/8 保留旧入口转发到单个过渡实例，Task 9 才启用其 PCI 描述符并删除旧硬件 initcall。不得在中间提交同时启用两条 NIC 初始化路径。

本计划只有文档，不执行任何下述实现步骤。开始实现时先按 using-git-worktrees 技能隔离工作区，读取 AGENTS.md；有 .codegraph 时先用 CodeGraph 定位源码。

## 测试入口约定

Task 1 新增根入口 `make test-arch9-host CASE=<case>`，通过 os01_submake 转发 hosttests。CASE 对应本计划各测试二进制：`block`、`pci`、`backend`、`ahci`、`device-boot`、`net`、`lwip`、`e1000`、`virtio`、`socket`；各任务新增自己的映射。CASE 未知时报错；省略 CASE 跑当前已注册 ARCH-9 用例。二进制同时加入 TEST_BINS，使 `make test-host` 覆盖所有新增测试。

每项测试编译真实生产模块，fake 只替换 hardware/allocator/clock/lwIP 边界，不能在 fixture 重写被测算法。`hosttests/include/block/blockdev.h` 是旧 ABI 镜像，Task 1 改为转发生产头，避免双份结构定义。

每个任务的 RED 命令均为该 CASE 的根入口；新增测试尚未实现时，期望测试断言失败或缺失符号的编译/链接失败，不能将坏 fixture 编译错误当 RED。GREEN 必须 exit 0 且该 CASE 所有断言通过。后续任务不反复跑无关全量套件；各阶段末运行指定回归。

---

### Task 1：完整 block ops 注册与 GPT 包装（阶段 A）

**Files:** Modify `kernel/{block/blockdev.c,include/block/blockdev.h,driver/ahci.c,fs/gpt.c,fs/boot.c}`、`hosttests/include/block/blockdev.h`、`hosttests/cases/test_fat32_basic.c`、`hosttests/Makefile`、`mk/components/run.mk`；Create `hosttests/cases/test_block_device_ops.c`、`hosttests/mock/arch9/block_runtime.h`。

**Interfaces:**

- 定义 `struct block_device_ops`：`int read(block_device_t *, uint64_t, uint32_t, void *)`、`int write(block_device_t *, uint64_t, uint32_t, const void *)`、`int flush(block_device_t *)`。
- 定义 `struct block_device_desc`：name、sector_count、sector_size、ops、private_data、`block_device_t *parent`、`enum block_device_kind { BLOCK_DISK, BLOCK_PARTITION }`。
- 产出 `int block_device_register(const struct block_device_desc *, block_device_t **out)`、`int block_device_unregister_boot(block_device_t *)`、`void block_device_mark_failed(block_device_t *, int error)`；保留既有 `block_device_read/write/get/count/init`。
- 新结构移除 port_num 和直接 read/write；保留 name/present/容量，新增 ops/kind/parent/last_error。注册时 out 先置 NULL；拒绝坏描述符 EINVAL、重名 EEXIST、表满 ENOSPC。

- [ ] **Step 1 — RED 测试：** `test_complete_registration` 验证成功设备所有字段/ops 已填充；`test_readonly_and_bounds` 断言 write=NULL → EROFS，lba=UINT64_MAX 非零请求 → EINVAL，合法边界 count=0 → 0 且不调用驱动，非零 NULL buffer → EINVAL；`test_registry_failures` 断言过长/重名/表满不增加 count、不发布 out；`test_partition_parent_dispatch` 断言父入口收到 offset+lba、越界不调用父 ops；`test_boot_unregister_stability` 断言撤销一个设备不移动其他对象地址。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(-EROFS, block_device_write(ro_dev, 0, 1, buffer));
assert_eq(0, block_device_read(dev, dev->sector_count, 0, NULL));
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=block`，预期上述测试不能通过。
- [ ] **Step 3 — 实现块层和迁移调用者：** read 必需；write/flush 可选；保留 BLOCKDEV_MAX=8 的静态槽，撤销留洞、枚举跳过洞，不压缩对象。AHCI wrappers/private_data 留在 ahci.c；GPT 用完整 partition_desc + parent block_device_read/write，检查偏移/容量溢出与 sector_size==512。fs/boot 只从 BLOCK_DISK 中选第一盘；迁移 host ABI 镜像及所有旧字段引用。
- [ ] **Step 4 — GREEN/阶段 A：** `make clean`；`make test-arch9-host CASE=block`；`make test-host`；`make test-qemu SUITE=phase-0`。均 exit 0；正常盘仍名为 hda，GPT/根挂载正常。
- [ ] **Step 5 — 提交：** 只暂存本任务文件，commit `refactor(block): register complete driver operations`。

### Task 2：device 身份与 fake backend 下的 PCI 枚举/绑定

**Files:** Create `kernel/device/core.c`、`kernel/include/device/device.h`、`kernel/bus/pci/{core,match,platform}.c`、`kernel/include/bus/pci/{pci,driver}.h`、`kernel/include/arch/pci.h`、`hosttests/cases/test_pci_driver_model.c`、`hosttests/mock/arch9/pci_runtime.h`；Modify `kernel/Makefile`、host/root测试 CASE 映射。

**Interfaces:**

- `struct device`：uint64_t id、name、parent、state、last_error、quarantined；`enum device_state { DEV_DISCOVERED, DEV_PROBING, DEV_BOUND, DEV_UNBOUND, DEV_FAILED }`。
- `struct pci_device`：内嵌 dev、domain/BDF、vendor/device/subvendor/subdevice、24-bit class_code、BAR[6]（kind/address/valid）、driver、driver_data、枚举错误标记；`PCI_ID_ANY=UINT32_MAX`，ID table 字段 uint32_t，显式 id_count。
- `struct pci_driver`：name/id_table/id_count、`int (*probe)(struct pci_device *, const struct pci_device_id *)`、`void (*remove)(struct pci_device *)`。
- backend 定义 `struct pci_root { uint16_t domain; uint8_t bus; }`；`enum pci_error_scope { PCI_ERROR_FUNCTION, PCI_ERROR_ROOT }`；`int config_read32(domain,bus,slot,fn,offset,uint32_t *value,enum pci_error_scope *scope)`、对应 write32；root数组及数量、IRQ route callback 位于 `struct pci_backend`；`const struct pci_backend *arch_pci_backend(void)` 的弱默认返回 NULL。
- 产出 `int device_core_init(void)`、`void device_quarantine(struct device *, void *retained_owner)`、`int pci_register_driver(const struct pci_driver *)`、`int pci_enumerate(void)`、`int pci_bind_all(void)`、`struct pci_device *pci_device_get(unsigned index)`、`unsigned pci_device_count(void)`。`DEVICE_UNSAFE=1` 为 probe 特殊致命结果，普通失败为负 errno；core 不将其归为可选错误。

- [ ] **Step 1 — RED 测试：** `test_all_functions_and_bridge_cycle` 断言 function 0..7 可见、visited 防环、BDF 去重且顺序稳定；`test_matching_precedence` 验证精确 ID 优先、同等级不同 driver → FAILED/EEXIST、id_count 无 sentinel；`test_absent_driver_probe_zero` 空总线 probe_calls==0；`test_fault_isolation` 坏 BAR/config function → FAILED、健康 function BOUND、bus级错误返回负值；`test_probe_cleanup_contract` probe 失败不调用 remove；`test_registry_oom_and_duplicates` OOM/重名返回框架失败且不遗留可绑定半对象。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(0, pci_enumerate());
assert_eq(2, pci_device_count());
assert_eq(0, absent_driver_probe_calls);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=pci`。
- [ ] **Step 3 — 实现：** 强类型 PCI core，设备堆链表，BSP串行初始化；按root/BDF排序并维护每domain visited。成功 vendor==0xffff 为缺失，backend错误保留 BDF/scope；invalid bridge隔离分支并记 ENUMERATION_INCOMPLETE。匹配前拒绝资源错误设备。BAR不做破坏性 sizing；64-bit BAR检查索引/上半槽/保留类型/溢出。通用代码不含 APIC、x86 I/O或固定高半地址。
- [ ] **Step 4 — GREEN：** `make test-arch9-host CASE=pci`，检查 NULL backend / 空总线成功统计与真实错误的区别。暂不启用 kernel boot coordinator，不影响旧驱动路径。
- [ ] **Step 5 — 提交：** commit `feat(pci): enumerate and bind typed driver instances`。

### Task 3：x86 backend、资源访问与 IRQ 防覆盖

**Files:** Create `kernel/arch/x86_64/bus/pci.c`、`kernel/include/arch/x86_64/pci.h`、`hosttests/cases/test_pci_backend_irq.c`、`hosttests/mock/arch9/backend_runtime.h`；Modify `kernel/driver/pci.c`、`kernel/include/driver/pci.h`、`kernel/include/bus/pci/pci.h`、`kernel/intr/irq.c`、`kernel/Makefile`、host CASE 映射。

**Interfaces:**

- 产出 x86 `arch_pci_backend()`；arch source discovery 加入 `bus` 组，aarch64 gate 保持不链接PCI/NIC。
- 强类型 wrappers `int pci_config_read32(struct pci_device *, uint16_t offset, uint32_t *out)`、`int pci_config_write32(struct pci_device *, uint16_t offset, uint32_t value)`、`int pci_set_bus_master(struct pci_device *, bool enabled)`、`int pci_set_intx(struct pci_device *, bool enabled)`、`int pci_set_decode(struct pci_device *, bool io, bool mmio)`、`int pci_route_gsi(struct pci_device *, uint32_t *out)`、`int pci_msix_enable(struct pci_device *, uint8_t vector)`、`int pci_interrupts_disable(struct pci_device *)`（MSI/MSI-X/INTx全禁用）。
- 保留现有 `register_irq/unregister_irq` 签名和 1/0 返回语义，但已占槽拒绝，未更改原 handler/parameter/controller；NIC后续自己记录已持有的slot。既有旧 PCI 接口转发 backend，Task 9 移除。

- [ ] **Step 1 — RED 测试：** `test_config_pair_atomicity` fake并发请求确认 CF8/CFC 配对不会交错；`test_irq_slot_preserved` 先注册A再B，B返回0且A完整；`test_msix_mapping_failure` 映射失败不写table/不enable；`test_disable_interrupt_sources` fake capability list验证MSI/MSI-X/INTx禁用；`test_capability_cycle` malformed cap指针/循环有界失败；`test_bar_space_validation` I/O和MMIO不可混用。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(0, second_registration_result);
assert_eq(first_handler, irq_table[gsi].handler);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=backend`。
- [ ] **Step 3 — 实现：** CF8/CFC 配对用短irqsave锁；platform持有Q35路由、LAPIC MSI地址和MMIO映射逻辑，迁移时补检查错误。MSI-X映射不得依赖AHCI先初始化；capabilities走visited/步数上限，失败不留下已开启源。保留旧init路径转发，不改硬件支持范围。
- [ ] **Step 4 — GREEN：** `make test-arch9-host CASE=backend`；`make test-qemu SUITE=phase-0`；`make test-qemu SUITE=network`。确认IRQ拒绝不会破坏既有timer/serial/keyboard登记。
- [ ] **Step 5 — 提交：** commit `refactor(pci): isolate x86 backend and preserve IRQ ownership`。

### Task 4：AHCI 实例化、deadline、超时与安全撤销

**Files:** Modify `kernel/driver/ahci.c`、`kernel/include/driver/ahci.h`；Create `kernel/driver/ahci_lifecycle.c`、`kernel/include/driver/ahci_lifecycle.h`、`hosttests/cases/test_ahci_lifecycle.c`、`hosttests/mock/arch9/ahci_runtime.h`；Modify host CASE 映射。

**Interfaces:**

- `struct ahci_controller` 持有 pci_device/HBA/ports；`struct ahci_port` 持有 controller/port_num/DMA/identity/busy/state/短锁/block_device。
- `int ahci_probe(struct pci_device *, const struct pci_device_id *)`、`void ahci_remove(struct pci_device *)`；ops 从 port private_data 定位 controller。
- 生命周期公开测试核心 `int ahci_deadline_start(bool boot_phase, uint32_t timeout_ms, struct ahci_deadline *out)`、`bool ahci_deadline_expired(const struct ahci_deadline *)`、`int ahci_port_claim(struct ahci_port *, const struct ahci_deadline *)`、`void ahci_port_release(struct ahci_port *)`、`int ahci_port_fail(struct ahci_port *, int cause)`。struct ahci_deadline含boot_phase/expiry；boot phase显式由调用路径传入，不能通过早期cpu_id猜测。
- 计划固定等待预算：gate 500ms、command/IDENTIFY 500ms、engine-stop 500ms、BIOS handoff 500ms；将当前AHCI所有等待统一有界。超时原请求ETIMEDOUT，之后EIO；gate到期EBUSY；无有效时间源probe ENOTSUP。

- [ ] **Step 1 — RED 测试：** `test_no_clock_no_hardware_side_effect` inactive/freq0→ENOTSUP，bus-master/MMIO/DMA写计数0；`test_deadline_with_stopped_jiffies` 只推进clock依然timeout；`test_compensated_runtime_deadline` 切换CPU rawTSC不同但补偿后deadline一致；`test_gate_and_timeout_no_reuse` 请求A超时后B→EIO，bounce写次数不变；`test_quarantine_on_stop_failure` 不调用free_pages、记录隔离；`test_unsafe_failure_escalates` 未能屏蔽中断/限制写范围→DEVICE_UNSAFE；`test_port_fault_isolation` 健康端口继续、无盘不分配DMA、坏IDENTIFY不注册块设备。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(-ENOTSUP, ahci_probe(pdev, id));
assert_eq(0, fake_mmio_write_count);
assert_eq(0, fake_dma_start_count);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=ahci`。
- [ ] **Step 3 — 实现生命周期：** 在 bus mastering/MMIO写/DMA之前验证active和freq；preGS用raw cycles换算（128位中间值、wrap比较），runtime用补偿后的read_ns且不允许inactive fallback。短锁只维护gate/state，独占token覆盖command准备至安全收尾；硬件等待不持长irqsave锁。超时停用端口，不自动重试写、不复位整个健康controller；不能证明停止则保留DMA/私有owner，无法安全隔离则fatal。回滚未消费服务使用Task1接口。单盘名hda、多controller/port全局递增且重名拒绝；容量满不谎报注册成功。
- [ ] **Step 4 — GREEN：** `make clean`；`make test-arch9-host CASE=ahci`。暂保留旧ahci_init wrapper调用同一probe核心，只用于旧启动集成；下一任务删除旧initcall。
- [ ] **Step 5 — 提交：** commit `refactor(ahci): own controller state and quarantine timed-out DMA`。

### Task 5：phase 6 coordinator、AHCI PCI 绑定与根能力检查（阶段 B）

**Files:** Create `kernel/device/boot.c`、`kernel/include/device/boot.h`、`hosttests/cases/test_device_boot.c`；Modify `kernel/driver/ahci.c`、`kernel/include/bus/pci/driver.h`、`kernel/arch/x86_64/{linker.ld,platform/boot.c}`、`kernel/fs/boot.c`、`kernel/include/fs/boot.h`、`kernel/Makefile`、host CASE 映射。

**Interfaces:**

- 定义 `PCI_DRIVER_DECLARE(driver)`，链接段内存放静态driver指针；边界 `__pci_drivers_start/end`，KEEP并按指针对齐。
- `int device_boot_init(void)`、`int device_boot_result(void)`、`void device_fail_unsafe(struct device *, const char *reason)`（fatal路径不返回）；device boot init为唯一phase6硬件coordinator。
- coordinator初始化device/block（后续加net），登记driver→枚举→绑定；保存框架/fatal结果。x86_64_boot_subsystems在subsys_init_all后检查结果。
- fs_boot_mounts现有void签名保留；内部根mount成功状态必须为真，否则通过已有panic路径报 `root filesystem unavailable`；不以/tmp、/proc等挂载替代根成功。

- [ ] **Step 1 — RED 测试：** `test_optional_absent_continues` 无AHCI probe=0、coordinator成功、SKIPPED_ABSENT；`test_registration_and_root_failure` 登记OOM/DEVICE_UNSAFE阻止消费者；`test_bad_nic_good_root` 坏function不阻止AHCI；`test_pre_gs_boot` fixture使this_cpu/cpu_id调用立即失败，phase6流程不得调用；`test_root_requirement` 无盘/根mount失败报fatal，正常GPT及原单FAT fallback成功；`test_single_init` 重复调用不得清空已发布表或重复probe。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(0, device_boot_init());
assert_eq(0, absent_ahci_probe_calls);
assert_eq(0, fake_pre_gs_cpu_id_calls);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=device-boot`。
- [ ] **Step 3 — 实现：** 只启用AHCI `.pci_drivers` 描述符，删除其旧硬件initcall和无返回值wrapper。coordinator非OPTIONAL登记；保存检查结果，单设备FAILED汇总继续，框架/unsafe阻断。缺失统计按匹配候选而非单凭绑定数；不支持设备不能称物理缺失。BSP串行，preGS安全日志。aarch64保持原启动不引用x86段边界。
- [ ] **Step 4 — GREEN/阶段 B：** `make clean`；本阶段4个CASE；`make test-host`；`make test-static`；`make test-qemu SUITE=phase-0`；`make test-contract PROFILE=aarch64-clang`。检查正常root、hda命名、所有旧启动marker必要顺序；改变合法marker时同步harness断言而不删掉检查。
- [ ] **Step 5 — 提交：** commit `feat(device): coordinate PCI boot and require a usable root filesystem`。

### Task 6：net_device 注册、统一 lwIP adapter 与 readiness

**Files:** Create `kernel/net/{device,lwip}.c`、`kernel/include/net/{device,lwip}.h`、`hosttests/cases/{test_net_device,test_net_lwip}.c`、`hosttests/mock/arch9/net_runtime.h`；Modify `kernel/net/net.c`、`kernel/include/net/net.h`、host CASE 映射。

**Interfaces:**

- `struct net_device`：name/MAC/MTU/link/parent/ops/priv；`struct net_device_ops`：`int xmit(struct net_device *, struct pbuf *)`、`unsigned poll_rx(struct net_device *, unsigned budget)`、`bool get_link(struct net_device *)`、`int stop(struct net_device *)`，stop返回0/负errno/DEVICE_UNSAFE。
- `int net_device_init(void)`、`int net_device_register(struct net_device *)`、`int net_device_unregister_boot(struct net_device *)`、`struct net_device *net_device_get(unsigned)`、`unsigned net_device_count(void)`、`void net_poll_rx(void)`。
- `int net_receive(struct net_device *, struct pbuf *)`：成功adapter接管pbuf，失败调用方释放；TX从不接管调用方pbuf。
- `enum net_service_state { NET_OFF, NET_STARTING, NET_ONLINE, NET_FAILED }`；`bool net_service_ready(void)`、`uint32_t net_default_ipv4(void)`、`void net_lwip_init(void)`，保留main的调用点。

- [ ] **Step 1 — RED 测试：** `test_per_device_dispatch` 两设备独立TX/RX，poll预算64、不串priv；`test_pbuf_ownership` TX成功/失败均不free caller，RX失败caller只free一次；`test_readiness_callback_order` tcpip返回/adapter先完成不ONLINE，callback+至少一adapter才ONLINE；`test_no_nic_and_failed_adapters` OFF/FAILED→ready=false、ip=0、无重复tcpip_init；`test_default_addresses` eth0静态10.0.2.15，eth1地址0后独立DHCP，不复制IP；`test_adapter_input_in_core` ethernet_input在tcpip-thread内而不是投到自身mailbox。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(false, net_service_ready());
assert_eq(0, net_default_ipv4());
assert_eq(64, fake_poll_budget);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=net`、`make test-arch9-host CASE=lwip`。
- [ ] **Step 3 — 实现：** MAC/ops完整才登记；eth编号按成功绑定顺序。adapter内嵌netif，state=ndev，统一linkoutput/ethernet input；release/acquire发布ONLINE。pre-task_init不等待尚未调度线程，失败adapter不阻挡其他卡，link_down不撤销core就绪。旧net.c路径保持临时single-NIC适配，Task9统一切换，禁止重复tcpip_init；不提供新user ABI。
- [ ] **Step 4 — GREEN：** net/lwip CASE exit0；两套fake只替换lwIP边界，真实device/adapter代码被编译；旧单NIC网络QEMU仍通过。
- [ ] **Step 5 — 提交：** commit `feat(net): register device operations and publish stack readiness`。

### Task 7：e1000 私有实例与三种中断模式

**Files:** Modify `kernel/driver/e1000.c`、`kernel/include/driver/e1000.h`；Create `hosttests/cases/test_e1000_instances.c`、`hosttests/mock/arch9/e1000_runtime.h`；Modify host CASE 映射。

**Interfaces:**

- `int e1000_probe(struct pci_device *, const struct pci_device_id *)`、`void e1000_remove(struct pci_device *)`；driver描述符 `const struct pci_driver e1000_pci_driver`（本任务不emit注册段）。
- 私有实例包含MMIO、RX/TX rings、RX软件队列、MAC、锁、ndev、IRQ owned标记；`enum nic_irq_mode { NIC_MSIX, NIC_INTX, NIC_POLL }` 放net/device.h供两驱动复用。
- 调用Task3 PCI和IRQ接口、Task6 net接口；暂保留旧e1000_init/netif/poll入口作为单过渡实例转发，Task9删除。

- [ ] **Step 1 — RED 测试：** `test_two_e1000_instances` 两套MMIO/ring/RX队列/TX lock独立；`test_poll_mode_does_not_register_irq` 固定槽16被占→POLL、不覆盖/不屏蔽A；`test_enable_after_owner_ready` IRQ handler/instance先就绪才enable源；`test_probe_failure_unwinds` 在mapping/DMA/IRQ/net登记步骤注入失败；`test_rx_single_consumer` handler只ack/wake，不消费ring；`test_unsupported_id_no_probe` 限0x8086:0x100e。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(NIC_POLL, second_irq_mode);
assert_eq(0, second_irq_owned);
assert_eq(first_handler, irq_table[first_gsi].handler);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=e1000`。
- [ ] **Step 3 — 实现：** 拆全局state，handler parameter为instance；ops用ndev->priv，收包调用net_receive。MSIX优先独占16，INTX可用才登记，冲突进入POLL，关闭设备源/PCI中断而不mask别人GSI；MSIX/INTX/POLL明确分支。stop确认静止、只释放自有IRQ/DMA；不安全走隔离策略。
- [ ] **Step 4 — GREEN：** e1000 CASE；兼容入口下原单e1000 `make test-qemu SUITE=network` exit0。
- [ ] **Step 5 — 提交：** commit `refactor(e1000): isolate NIC state and support polling fallback`。

### Task 8：transitional virtio-net 私有实例与 POLL

**Files:** Modify `kernel/driver/virtio-net.c`、`kernel/include/driver/virtio-net.h`；Create `hosttests/cases/test_virtio_net_instances.c`、`hosttests/mock/arch9/virtio_runtime.h`；Modify host CASE 映射。

**Interfaces:**

- `int virtio_net_probe(struct pci_device *, const struct pci_device_id *)`、`void virtio_net_remove(struct pci_device *)`；描述符 `const struct pci_driver virtio_net_pci_driver`，仅ID 0x1af4:0x1000，本任务不emit注册段。
- private持有io_base、virtqueues、TX indices、MAC、IRQ/ndev；调用Task3/6/7接口，保留旧单实例入口至Task9。

- [ ] **Step 1 — RED 测试：** `test_two_virtio_instances` 两套io_base/queue indices不串；`test_transport_validation` modern-only/MMIO BAR不触碰I/O，legacy I/O BAR才probe；`test_intx_conflict_poll` 冲突禁用自身中断源且双卡RX/TX成功；`test_queue_feature_failure` queue/feature/net登记失败只回收自身；`test_reset_unconfirmed_quarantines` stop不能确认→保留被设备访问的页；`test_budget_and_ownership` RX≤64且pbuf规则一致。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(0, fake_modern_device_io_writes);
assert_eq(0, fake_other_instance_queue_mutations);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=virtio`。
- [ ] **Step 3 — 实现：** 移除vnet/io_base/TX全局；handler只ack/wake；transitional feature/queue验收失败返回真实errno，不以vendor推测modern支持。所有pbuf接入只在post-SMP/core运行；stop reset确认后才释放virtqueue。队列DMA不静止时按device隔离/fatal规则。
- [ ] **Step 4 — GREEN：** virtio CASE；旧入口下单transitional NIC网络测试通过。硬件测试命令由Task11统一提供，当前host断言不代替最终QEMU证据。
- [ ] **Step 5 — 提交：** commit `refactor(virtio-net): own transport queues per NIC`。

### Task 9：NIC PCI 统一绑定、core mailbox 公平轮询与移除过渡层

**Files:** Modify `kernel/driver/{e1000,virtio-net}.c`、`kernel/include/driver/{e1000,virtio-net}.h`、`kernel/device/boot.c`、`kernel/net/{net,sys_arch}.c`、`kernel/include/net/net.h`、`hosttests/cases/test_net_lwip.c`；Delete `kernel/driver/pci.c`、`kernel/include/driver/pci.h`；Modify `kernel/Makefile`。

**Interfaces:** 只消费Task2/3/6–8接口；`net_poll_rx(void)`从所有成功注册设备逐卡调ops(64)，两个NIC描述符此时emit `.pci_drivers`。coordinator在probe前调用net_device_init；net_lwip_init仍在原main阶段。

- [ ] **Step 1 — RED 测试：** `test_nonempty_core_mailbox_polls` 1000个连续API消息中每次fetch前都执行sweep，消息也被返回；`test_application_mailbox_no_poll` 非core mailbox poll_calls=0；`test_bounded_rx_and_no_lock` 每卡64包且poll时mailbox锁未持有；`test_no_duplicate_driver_init` 每BDF probe一次且旧net-hw/ahci initcall不存在；混合/同类双卡经真实matcher得到两个ndev。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(1000, returned_api_messages);
assert_eq(1000, core_fetch_rx_sweeps);
assert_eq(0, application_mailbox_rx_sweeps);
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=lwip`、`make test-arch9-host CASE=pci`。
- [ ] **Step 3 — 实现切换：** 删除net_hw_init首设备扫描、is_virtio/net_hw_ok、硬件驱动旧netif callback/单实例wrapper；Task10的socket引用迁移在删除os01_netif对象之前完成，不重新创建全局兼容对象。core mailbox取消息之前sweep且锁外运行；空时保留50ms wake和lost-wakeup检查；非core不poll。移除旧driver/pci所有include/calls，替换backend访问；不留下双重NIC初始化。
- [ ] **Step 4 — GREEN：** 全部已注册ARCH9 host CASE通过；Task9/10形成同一网络集成组，完成Task10再做网络QEMU阶段验收，避免以尚未迁移socket的中间kernel声称C阶段完成。
- [ ] **Step 5 — 提交：** Task9/10代码迁移一起验证后，分别提交可构建变更或单个集成commit `feat(net): bind all supported NICs and fairly poll each device`；不提交不能链接的os01_netif悬空状态。

### Task 10：socket readiness 与默认接口查询（阶段 C）

**Files:** Modify `kernel/net/socket.c`；Create `hosttests/cases/test_socket_net_readiness.c`、`hosttests/mock/arch9/socket_runtime.h`；Modify host CASE 映射。

**Interfaces:** 消费 `bool net_service_ready(void)`、`uint32_t net_default_ipv4(void)`；保留所有do_socket/do_getsockname/do_getifaddr和user ABI签名。socket_alloc内部guard，do_socket提前ENETDOWN以区分ENOMEM。

- [ ] **Step 1 — RED 测试：** `test_socket_off_starting_failed` 每状态do_socket→ENETDOWN，netconn_new_calls=0；`test_online_allocation_errors` ONLINE才能分配，真实allocation failure→ENOMEM；`test_getifaddr_before_online` →0；`test_getsockname_prefers_bound_address` 优先netconn实际非零IP，只有零IP时使用默认接口；`test_bad_fd_before_lwip` invalid fd→EBADF且无netconn调用；`test_no_nic_socket_finishes` fake deadline防永久mailbox等待。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```c
assert_eq(-ENETDOWN, do_socket(AF_INET, SOCK_DGRAM, 0));
assert_eq(0, fake_netconn_new_calls);
assert_eq(0, do_getifaddr());
```

- [ ] **Step 2 — 验证 RED：** `make test-arch9-host CASE=socket`。
- [ ] **Step 3 — 实现：** readiness guard在netconn分配之前，删除所有os01_netif extern；不修改无关用户指针/accept生命周期问题。default IPv4查询内部检查ONLINE，不直接读取未发布adapter。
- [ ] **Step 4 — GREEN/阶段 C：** `make clean`；`make test-arch9-host`；`make test-host`；`make test-static`；phase-0和network QEMU。需Task11再验证同类/混合双卡、无NIC真实用户调用，host不代替硬件证据。
- [ ] **Step 5 — 提交：** 与Task9协调为可构建提交，commit `fix(net): reject socket creation before network service is ready`。

### Task 11：QEMU 多卡/缺失/故障矩阵（阶段 D）

**Files:** Create `qemutests/driver_model_matrix.py`、`qemutests/test_driver_model_matrix.py`、`user/netmodeltest.c`；Modify `qemutests/run_test.py`、`mk/components/run.mk`、`user/Makefile`、`config/rootfs.mk`、`kernel/device/boot.c`、`kernel/net/lwip.c`、`kernel/driver/ahci.c`；Create `kernel/device/test_fault.c`、`kernel/include/device/test_fault.h`；Modify `kernel/Makefile`。

**Interfaces:** 新增 `make test-qemu SUITE=driver-model`；使用normal image中netmodeltest.elf并serial stdio运行，根harness自动从print-run-paths解析firmware/image。matrix支持 `--case {all,e1000,virtio,mixed,two-e1000,two-virtio,no-nic,no-ahci,empty-ahci,poll-busy,bad-nic,adapter-fail}` 与 `--smp {1,2}`。测试程序 `netmodeltest.elf no-nic` / `netmodeltest.elf udp <local-ip> <port>` 使用既有socket/bind/send/recv/getifaddr ABI；无新增网卡配置syscall。

- [ ] **Step 1 — RED harness 测试：** Python unittest `test_no_nic_argv`断言无默认NIC；`test_dual_netdev_separate_subnets`断言两netdev、两个MAC、10.0.2.0/24和10.0.3.0/24；`test_boot_failure_is_not_pass`无UEFI handoff/内核marker不能通过；`test_each_card_evidence_required`只eth0成功拒绝；`test_probe_and_socket_deadlines`缺失marker/2秒socket上限导致失败；`test_variant_hash_protection` fault fixture不能污染normal image。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```python
self.assertIn("-nic", argv)
self.assertEqual(argv[argv.index("-nic") + 1], "none")
```

- [ ] **Step 2 — 验证 RED：** `python3 qemutests/test_driver_model_matrix.py`，预期没有生产matrix前不能通过。
- [ ] **Step 3 — 实现harness/guest探针：** 复用run_test.TestRunner，扩展可传入设备/存储argv；独立netdev/MAC和子网，每卡hostfwd到guest UDP echo端口，guest分别bind 10.0.2.15/10.0.3.15，host发随机nonce核对对应回复与MAC/接口证据。避免只ping默认卡。poll-busy持续默认卡TX/API同时向第二卡注入包，必须响应。无NIC在shell可用后运行probe，计时只从命令执行marker开始不含boot，2秒内ENETDOWN/getifaddr0。
- [ ] **Step 4 — 实现故障fixture：** `ARCH9_FAULT=none|bad-nic-bar|adapter-fail|ahci-empty|irq-conflict` 经root传kernel flag并纳入fingerprint，非none仅matrix fixture启用，不新增production ABI。bad-nic仅污染一张NIC资源，不影响root/第二卡；adapter-fail拒绝全部adapter；ahci-empty在安全枚举点抑制介质发布不启动DMA；irq-conflict在第二张NIC申请IRQ的测试边界使其候选slot指向第一张NIC已持有的slot，走真实已占槽拒绝路径，然后POLL。未取得slot不能开启硬件中断，也不能改写第一张卡的路由/handler；matrix必须核对第一卡继续收发和第二卡POLL marker，不能假设随便添加两卡就会自然冲突。无AHCI通过OVMF支持的virtio-blk-pci启动同一EFI镜像，先确认UEFI已装入内核，再断言无AHCI/根缺失；firmware不支持该组合时报告fixture失败，不能当PASS。故障build用已有variant路径隔离，每次切flag按指南clean，记录normal hash。AHCI DMA停止失败以Task4 host fake为主，不在真实QEMU故意让硬件破坏内存。
- [ ] **Step 5 — GREEN硬件矩阵：** `python3 qemutests/test_driver_model_matrix.py`；`make test-qemu SUITE=driver-model`自动执行SMP=1/2全部case及必要fixture构建；全部exit0，记录每case证据。single e1000/virtio、混合、同类双卡实际收发，无NIC和adapter-fail真实socket测试，不支持卡UNBOUND、不写BAR，坏卡FAILED而健康root/NIC可用。无AHCI/空端口精确识别root fatal诊断。
- [ ] **Step 6 — 提交：** commit `test(qemu): cover driver discovery multi-NIC and optional absence`。

### Task 12：边界审计、全回归与文档闭环

**Files:** Create `qemutests/driver_model_boundary_audit.py`、`qemutests/test_driver_model_boundary_audit.py`；Modify `mk/components/run.mk`、`docs/{roadmap.md,driver/driver.md,subsys/subsys.md,build/build.md}`。

**Interfaces:** audit为 `python3 qemutests/driver_model_boundary_audit.py`，加入test-static；禁止通用block AHCI/port_num、net adapter按硬件类型分支、硬件pci_find_device、通用PCI x86 asm/APIC/固定高半地址、os01_netif旧引用；只扫描生产模块避免fake fixture误报。

- [ ] **Step 1 — RED audit测试：** `test_detects_ahci_dependency`、`test_detects_legacy_nic_globals`、`test_detects_x86_pci_leak` 在临时fixture注入违规，断言非零；`test_allows_arch_backend`合法backend和普通字符串不误报。
关键断言（变量由该任务 fixture 提供；fake 计数器只观测边界，不重写生产逻辑）：

```python
self.assertNotEqual(violation_result.returncode, 0)
self.assertEqual(valid_backend_result.returncode, 0)
```

- [ ] **Step 2 — 验证 RED：** `python3 qemutests/test_driver_model_boundary_audit.py`。
- [ ] **Step 3 — 实现审计与文档：** 描述真实新接口/启动顺序/错误状态/POLL限制；修正旧subsys文档宏示例，以源码为准，不顺手实现ARCH8。roadmap保留spec和plan链接，只在以下验收通过后标ARCH9完成。
- [ ] **Step 4 — GREEN/完整验收：** 依次 `make test-host`、`make test-static`、`make test-qemu SUITE=phase-0`、`make test-qemu SUITE=inittab-phase`、`make test-qemu SUITE=network`、`make test-qemu SUITE=driver-model`、`make OS01_SYSTEST=1 test-syscall`；单独 `make KERNEL_SELFTEST=1 test-kernel-selftest`，按已有间歇超时问题如实记录而非声称通过；随后双profile `make test-contract PROFILE=x86_64-clang` / `make test-contract PROFILE=aarch64-clang` 和 `make PROFILE=aarch64-clang test-aarch64 MODE=smp`。切flag/profile前按构建指南clean，固件/镜像重查print-run-paths。
- [ ] **Step 5 — 收集证据与最终评审：** 按spec §11逐条填写结果、退出码、日志位置和限制；必要故障用例不得以SKIP代替PASS。提交前 `git diff --check`。执行模式要求的独立评审发现阻断则修复并复验受影响测试；无实现证据不标完成。
- [ ] **Step 6 — 提交：** commit `docs(test): enforce and document ARCH-9 driver boundaries`；然后使用finishing-a-development-branch技能处理集成，未获合并/发布授权不执行这些动作。

## 计划自检与评审映射

- spec §1–4范围/文件布局 → Tasks1–12，所有新公开头与源子目录成对。
- §5枚举/backend/match → Tasks2/3/5，错误scope与候选统计区分缺失和不支持。
- §6错误/资源/remove → Tasks2/4/5/7/8，rollback只对未消费服务，runtime不卸载。
- §7网络/ownership/readiness/IRQ/poll → Tasks6–11，迁移窗口在Task9/10一起闭合。
- §8块层/AHCI/deadline → Tasks1/4/5，时源缺失、跨CPU和DMA不静止都有具体测试。
- §9phase/linker/profile → Tasks2/3/5/9，aarch64不链接新PCI代码。
- §10交付 → A(1)、B(2–5)、C(6–10)、D(11–12)。
- §11host/QEMU/static回归 → Tasks1–12，不将文档检查当作实现测试。
- 5条Review Focus均映射到具体测试。计划只增加必要接口/fixture和验证入口，没有扩展已排除功能。

## 执行交接

计划待用户评审并选择执行方式。本任务尚未开始实现。推荐子agent逐任务实现/评审：本计划有12个任务，涉及PCI、DMA、IRQ、lwIP与boot接口；阶段性独立评审有助于及时发现跨模块契约错误。也可选择本agent逐任务执行并在分支完成后独立总评审；选定方式后按对应superpowers技能执行。
