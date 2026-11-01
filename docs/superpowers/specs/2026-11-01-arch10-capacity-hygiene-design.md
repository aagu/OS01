# ARCH-10：静态容量与工程卫生设计

日期：2026-11-01

状态：子 agent 两轮严格评审后 APPROVE，待用户确认；不表示实现或测试已完成。

## 1. 目标与范围

对应 [roadmap ARCH-10](../../roadmap.md#剩余架构治理任务)：统一 CPU 容量来源，按发现的硬件表示 GSI，解除 poll、块设备和 devfs 的独立固定表限制，并修复汇编忽略规则、构建依赖说明和源码树遗留产物。

本设计以 2026-11-01 工作区源码为现状依据。默认 `NR_CPUS=8`、`NOFILE=16` 不变；“移除静态容量”不等于取消硬件、内存或进程资源上限。新上限必须有来源、有检查、有失败语义，不能简单把 8/16/24/32 换成更大的魔数。

本项交付规格包含必要的 IRQ 向量映射和最小 `RLIMIT_NOFILE` 支持。实现阶段须整体完成这些依赖，不能把 libc 的假成功资源限制当作完成条件。

### 范围边界

- 不实现 CPU 热插拔、超过 8 核的运行支持、通用 IRQ domain、共享 INTx、动态 MSI/MSI-X 分配或设备热卸载。
- 保留 `MAX_IOAPICS=4` 的平台发现容量；超额必须显式失败，不静默截断。GSI 数量从这些控制器的实际 pin 数计算，不使用 `4 * 24`。
- 不扩大 aarch64 PCI、VFS、用户态能力；共享头变更必须通过其现有构建和启动检查。
- devfs 仍是单段命名，保留现有注册 API；多级目录、路径 API、trie 属于 ARCH-11。本项先完成稳定对象存储，ARCH-11 可在其上增加目录索引。
- ARCH-8 的 subsys 表、ARCH-6 的 FS 并发/块缓存不纳入本项。
- 不修改无关终端显示代码；当前工作区的 terminal_display 修改不属于本项。

## 2. 现状与约束

| 位置 | 核实的当前行为 | 设计影响 |
|---|---|---|
| `kernel/include/arch/cpu.h`、`arch/x86_64/cpu.h` | 三处直接定义 `NR_CPUS=8` | 移入不依赖 ISA 的公共配置头 |
| `fs/procfs_fmt.h`、`arch/aarch64/ap_work.h` | 另有 `NR_CPUS` fallback；procfs 将 8 写进断言 | 不能只清理 roadmap 提到的三处 |
| `arch/aarch64/{dtb,boot_offsets}.h` | `AARCH64_BOOT_MAX_CPUS`、`AARCH64_BOOT_CAPACITY` 各自定义 8 | 与 C/汇编共享同一容量来源；保留 ABI 检查 |
| `intr/irq.c`、`intr/interrupt.h` | `irq_table[24]`；注册、屏蔽、注销均按 24 判断 | 描述符查询与静态数组解耦 |
| x86 `intr/irq.c`、`irq_hooks.c`、`intr_vectors.h`、`dispatch.c` | 仅有 `0x20..0x37` 设备入口；GSI/vector 用加减 `0x20` | 扩容必须同时修改 stub、路由及审计 |
| x86 `intr/apic/ioapic.c` | 从版本寄存器读 `max_redir`；控制器回调又做 ISA→GSI 转换，并直接索引 `irq_table[nr-32]` | 区分 ISA source、GSI、vector；ISO 只应用一次 |
| `driver/e1000.c`、`bus/pci/core.c` | MSI-X 固定 vector 0x30，经伪 GSI16 登记；PCI helper 会开启消息 | 消息 IRQ 与实际 GSI 分离，先登记后启用 |
| `fs/poll.c`、`fs/poll.h` | `kfds[16]`；wait entries 已堆分配，但 setup 仍限制 32 项 | 按调用尺寸分配两类对象，保留等待链同步协议 |
| `fs/select.c` | 已堆分配 pfds，并复用 poll table | 同步修改共享 helper，避免 poll 修好而 select 回退 |
| `fs/file.h`、`libc/resource/resource.c` | `NOFILE=16`；getrlimit 任意资源均报 65536，setrlimit 无操作返回成功 | 引入真实 NOFILE 限制；不扩张为完整资源管理系统 |
| `block/blockdev.c` | 8 项内联对象表，boot unregister 留洞；ARCH-9 消费者保存对象指针 | 不能 realloc 搬动公开对象 |
| `fs/devfs.c` | 32 项表；lookup 按需创建节点，I/O、poll/ioctl/release 等将 `fs_data` 当索引；readdir 也暴露索引；mount 仅设根节点，文件系统设置 `VFS_FS_NO_DCACHE` | 所有派发路径一起迁移，保证既有打开文件稳定 |
| `.gitignore` | 全局忽略 `*.S`；thread_entry 例外路径过时 | 正常汇编默认可跟踪，仅忽略实际生成文件 |
| `kernel/Makefile` | C 和主要汇编规则生成 `.d`；末尾 `ALL_DEPS` 当前只收集 C 对象 | 不能仅把文档改成“全部依赖已完善”，须补齐汇编消费 |
| `AGENTS.md` | 仍声称结构体变化必须 clean，理由是没有头文件依赖 | 按真实 profile/依赖规则重写 |
| `kernel/fs/select.o`、`kernel/kernel.bin` | 工作区存在、未被 Git 跟踪的旧构建产物 | 定向清理；不能用全仓库 clean 删除无关文件 |

## 3. 方案比较与推荐

以下为本设计的推荐选择，尚未经用户确认。

| 方案 | 收益 | 代价与结论 |
|---|---|---|
| 提高各个常量 | 修改少 | 仍依赖猜测，不能满足按硬件/资源决定容量的要求 |
| **推荐：共享配置 + 按需稳定对象 + 有界架构映射** | 保持已有调用方式，解决实际容量与生命周期问题 | IRQ 需要小型映射表，资源限制需要贯通内核与 libc |
| 全量可扩展框架 | 支持热插拔、通用 IRQ domain、动态 fdtable、通用设备命名空间 | 超出 ARCH-10，增加不必要的引用计数与卸载机制 |

CPU 和 fdtable 保留明确编译期容量，poll 不再拥有独立的 16 项上限；GSI 按拓扑存储，vector 按架构可用槽分配；block/devfs 使用逐对象分配的链表。无需给所有表引入同一种容器。

## 4. CPU 容量单点定义

新增 `kernel/include/config/limits.h`，作为纯配置头，仅含可供 C 和预处理汇编使用的整数宏，不包含类型、结构体或 ISA 指令。这里是生产代码唯一的 `NR_CPUS` 数值定义，默认 8。新增配置目录为 header-only，不为目录对称创建空源文件。

- `arch/cpu.h`、x86 cpu 头、procfs formatter、aarch64 AP work、DTB 和 boot offsets 直接或间接包含它；删除数值 fallback。
- `AARCH64_BOOT_MAX_CPUS` 与 `AARCH64_BOOT_CAPACITY` 如需保留兼容名字，必须别名到 `NR_CPUS`，不再次写 8。
- 保留/新增实际结构体大小、汇编步长和数组容量断言；将 procfs 的“必须等于 8”改为与公共配置的一致性检查。
- 扫描 `.S` 内 `.space/.skip` 及启动栈计算，确保数量来自同一来源。不可改变 boot_context v2 字段布局或 PERCPU 偏移。
- host mock 默认包含公共配置头。确需单核等隔离 fixture 时使用专用测试常量/专用 mock 类型，并在审计中逐项列明；不能以 mock 再覆盖生产 NR_CPUS 后混用布局。
- 运行期循环继续以已发现且 online 的 CPU 为准；超过当前容量的发现结果遵循既有明确拒绝策略，不因本项开放大核数支持。

## 5. GSI 容量、路由与 x86 向量

### 5.1 拓扑和初始化

以每个已枚举 IOAPIC 的 `[gsi_base, gsi_base + max_redir + 1)` 表示有效范围。端点使用加宽算术计算并检查溢出；检测重复/重叠范围及不可表示的 redirection register 索引，失败时保持相关硬件 masked。`gsi_limit` 是所有有效范围最大 end exclusive，**不等于有效 GSI 个数**。

采用按控制器分配的连续 descriptor 数组，另保存小型 range 表；空洞不分配 descriptor。这样高起点稀疏 GSI 不会触发以最大编号为尺寸的大分配。查找至多扫描当前控制器数量，IRQ 热路径不分配、不扩容。

PIC fallback 独立使用 0..15 的固定早期存储。早期 `irq_install()` 只装入口和基础状态，不调用 allocator；IOAPIC 拓扑存储在内存分配器可用后、设备 IRQ 注册之前准备。保持当前 BSP 阶段的 pre-GS 约束：不调用要求有效 percpu 的 helper，不把容量初始化移进更早入口。

构造完整拓扑、描述符与向量状态后一次发布；失败不切换到半初始化 IOAPIC 模式。无 IOAPIC 可按既有 PIC 路径运行；非法拓扑、超过 MAX_IOAPICS 或 OOM 不能伪装成无 IOAPIC，须向启动边界传播并停止依赖初始化。无需等待 ARCH-8 才正确处理本路径失败。

通用 IRQ 层提供按 GSI 查找描述符的内部 API，取代公开 `irq_table[]`/`NR_IQRS`/`MAX_GSI` 数组契约。保留 `register_irq()` 成功 1/失败 0 的现有调用约定；内部错误分别记录为无 controller、无效 GSI、冲突、OOM、vector 耗尽。所有现有调用者检查失败。

### 5.2 ISA、GSI、vector 分离

- `register_irq()`、`irq_mask()`、`irq_unmask()`、`unregister_irq()` 的参数只表示实际 GSI。
- ISA 设备在入口显式调用平台 ISA→GSI helper；PIC 下为 identity，IOAPIC 下应用 MADT ISO。更新 keyboard、mouse、serial、PIT/RTC 等全部调用点，包括随后 mask/unregister 的保存值。
- IOAPIC 回调通过 vector→route 查到 GSI/pin/flags，不再对 `vector-0x20` 再套 ISO。ISO trigger/polarity 由对应路由记录提供，优先于设备 hint 与总线默认值。
- 保留 handler 第一参数为 native vector 的既有契约；不得悄悄改成 GSI，审计所有使用该参数的驱动和 ACK。

### 5.3 向量分配

首期保留现有设备布局 `0x20..0x37`，新增设备池 `0x42..0x7f`。保留 `0x38` timer、`0x40/0x41` IPI、`0x80` syscall、`0xff` spurious，`0x39` 专供 §5.4 的固定 MSI-X 消息槽，`0x3a..0x3f` 暂不使用。pool 定义集中在 x86 向量头，入口生成、安装、分类与静态审计使用同一集合。

启动时为每个有效 GSI 0..23 预留其历史 `0x20+gsi` 映射；更高 GSI 在首次注册时从新增池取一个空闲 vector。所有映射具有明确 invalid 值；不能以 vector 0 表示成功。维护双向映射，任何 vector→GSI 转换均校验范围、是否分配和拓扑覆盖。

- `Build_IRQ` 或等价生成机制为新增池提供真正的汇编入口；不能把 C 函数直接装入 IDT。
- 更新 x86 IRQ backend、family validator、`intr_vectors.h` 及 `intr_dispatch_audit.py`，允许扩展池中有映射的设备 IRQ；异常、固定消息 IRQ、timer、IPI、syscall、spurious 的分类保持独立。
- 硬件路由在 descriptor/handler 完整发布后才 unmask。vector 分配失败或槽占用时不写设备路由，不覆盖原 handler，不产生假成功。
- 映射容量由有限 vector 池决定，控制器发现成功不代表每个 GSI 均能同时注册；耗尽是可诊断失败，ARCH-9 NIC 可依既有策略回退 POLL。
- 注册和 mask 状态用 irqsave 锁保护；不持注册表锁调用驱动回调。IOAPIC 的 selector/data 成对访问按控制器加锁。
- 首期 unregister 屏蔽路由并解除 handler，保留该 GSI 的 vector 映射至关机，防止迟到中断误投给新 GSI；再次注册同 GSI 可复用。无人注册但仍有映射的迟到中断只走安全 ACK，不访问旧参数。并发 handler 撤销的同步边界须遵循 §7 的启动期静止约束。

该方案使 GSI 编号与 vector 解耦，但不承诺无限同时活跃 IRQ。aarch64 的原生 GIC INTID 路径不借用 x86 vector 池，也不因缺少 IOAPIC 初始化而失败。

### 5.4 现有 e1000 固定 MSI-X 的迁移

现有 `e1000_probe()` 先 `pci_msix_enable(pdev, 0x30)`，再用伪 GSI16 调 `register_irq()`；该路径必须在本项迁移。保留单个固定 MSI-X 槽，但改为独立的 vector `0x39`，不引入动态 MSI 分配。

- 在 x86 层提供固定 message IRQ 的登记/撤销接口和独立 descriptor；不加入 GSI range 表，不要求 IOAPIC 覆盖，不调用 IOAPIC install/enable/disable，不应用 ISO、trigger/polarity。设备 IRQ 入口按 vector 路由类别选择 line 或 message descriptor，0x39 不可进入 vector→GSI 转换。更新真正的汇编 stub、分类和审计。
- 全局单槽有独占 owner，冲突时返回明确失败；GSI16 仍可使用 0x30，不能因消息槽的占用而拒绝其登记。e1000 实例分别保存 mode、message owner/handle 和真实 INTx GSI，不能再用 `inst->gsi=16` 表示 MSI-X 所有权。
- probe 先保持设备中断源及 MSI-X masked，完成 descriptor、handler 参数及必要实例状态的发布，再由 PCI backend 写入指向 0x39 的 message 并启用；最后才开放设备中断源。拆分或调整当前 `pci_msix_enable()` 的调用契约，禁止在成功取得槽位之前产生消息。
- 不支持 MSI-X、槽位被占或配置失败时，屏蔽本设备的 MSI-X 并确认不再产生消息，撤销本设备持有的 descriptor 后尝试真实 INTx，再按 ARCH-9 约定回退 POLL；失败路径不能撤销其他设备的槽位或屏蔽真实 GSI16。未取得所有权不得调用撤销。
- message handler 仍接收 native vector；正常消息和已登记槽上的迟到消息由消息派发路径恰好一次 LAPIC EOI，不使用 IOAPIC ACK/屏蔽接口。message mask 由 PCI function/table mask 和设备 source mask 完成，不调用 `irq_mask(gsi)`。
- 撤销只在 §7 的启动期静止条件下进行：先停设备消息并同步已在执行的 handler，再移除其参数；未绑定时进入安全 ACK 路径。固定槽首次交给某设备后，即使启动回滚也保留该设备身份至关机，仅允许同设备在确认旧消息已排空后重新登记；不把它转交另一设备，避免迟到消息访问新 owner。后续设备使用 INTx/POLL。
- 同步更新 `e1000_legacy_init()` 中 use_msi 路径、probe/unwind/remove、PCI helper 契约、ARCH-9 故障注入与观测。冲突 fixture 必须分别制造真正的 message 槽冲突和 INTx GSI 冲突，不能继续篡改伪 GSI 来代表 MSI-X 冲突。

## 6. poll 容量与最小资源限制

### 6.1 真实的 RLIMIT_NOFILE

`NOFILE` 保留为 fdtable 的硬容量来源，默认 16；本项不引入动态 fdtable。初始任务的 soft/hard `RLIMIT_NOFILE` 均为 NOFILE，fork 复制，exec 保留。内核保存限制，libc 不得继续报告虚构的 65536。

本项新增仅服务 `RLIMIT_NOFILE` 的 get/set 资源限制能力：在 OS01 原生 UAPI 末尾追加 syscall，禁止重排既有编号；跨边界结构使用两个 `uint64_t`，显式定义 infinity 值。新增 `kernel/syscall/sys_resource.c` ↔ `kernel/include/syscall/resource.h`，UAPI 定义在 `kernel/include/uapi/`，libc wrapper 显式转换。

- `getrlimit` 返回实际 soft/hard；`setrlimit` 要求 `soft <= hard <= NOFILE`。首期不提供提升 hard 的权限模型，hard 只能保持或降低，尝试提高返回 `-EPERM`，非法 soft/hard 关系返回 `-EINVAL`。
- 不支持的 resource 返回 `-EINVAL`，坏指针通过 fault-tolerant uaccess 返回 `-EFAULT`；不为其他 resource 假造成功。
- 降低 soft 不关闭现有 fd，也不令已打开的高编号 fd 无效；之后的 open/dup/pipe/socket/PTY 等所有 fd 安装路径通过公共分配 helper 遵循 soft。无槽返回 `-EMFILE`；多 fd 创建失败须撤销本次已安装的 fd。
- poll 在入口快照 soft；进行中的调用不因之后的限制变化重新缩小数组。资源限制字段的读写使用既有任务同步机制或专用短锁。
- Linux x86_64 compat 映射 getrlimit/setrlimit，并支持 BusyBox 可能使用的 prlimit64：仅 pid=0 或当前任务，其他 pid 返回 `-ESRCH`；单次操作在锁内完成旧值快照与新值提交。若旧值输出 EFAULT，明确允许新限制已经生效，不能回滚覆盖其他更新。原生 ABI 始终是权威来源。

这是支撑 roadmap “RLIMIT_NOFILE 上限”所需的最小闭环；其他 RLIMIT、跨进程管理和权限系统不在本项。实现时分配尚未使用的 syscall 编号，并同步 native dispatch、libc、compat table 及 syscall 文档。

### 6.2 分配与错误语义

`do_poll()` 不再使用 `kfds[POLL_MAX_FDS]`。入口先检查 `nfds <= soft_limit`，否则 `-EINVAL`；检查 `nfds * sizeof(pollfd)`、`nfds * POLL_WAIT_SLOTS_PER_FD` 及 entries 字节数的溢出和内部 int 表示范围，然后校验/复制用户数组。采用堆分配 kfds 与 wait entries，失败返回 `-ENOMEM`。

`POLL_WAIT_SLOTS_PER_FD=2` 暂保留为当前设备协议：每个条目至多登记独立 read/write 两个等待链，重复 fd 也按数组条目计费。删除与 16 绑定的 `POLL_MAX_FDS`/`POLL_MAX_WAIT_ENTRIES`，setup 校验实际容量和分配大小。若未来设备突破两个 slot，必须修改协议；本项至少确保溢出时显式报错，不允许 `poll_wait()` 静默丢弃等待项后永久睡眠。

所有路径（复制输入失败、信号、timeout、输出失败、setup 失败）释放已分配对象。清理顺序必须先退出 timeout registry、撤销 task wait、在 fd_lock 下摘除 entry，再释放 entries/kfds；保留现有 scan→arm→rescan 的失唤醒防护和 data-before-EINTR 规则。输出 EFAULT 可以已经写回部分 revents，不承诺跨整个用户数组的原子写回。

`nfds=0` 不访问用户数组、不分配 kfds；保留当前 `timeout<=0` 立即返回、正 timeout 走等待协议的行为，不借容量改造改变现有零 fd 语义。timeout=0 的普通调用可直接扫描而不分配等待链，属于实现者可选优化。

`select()` 继续使用既有 `FD_SETSIZE`/fd_set ABI，复用新 helper；其等待容量按实际要扫描的有效 fd 数推导，不因 poll 删除常量而越界或拒绝合法请求。保留本项之外的 select 零 fd 语义。

默认 soft=16 时，17 项 poll 仍因真实资源上限返回 EINVAL，这是有意兼容。必须增加 NOFILE=64 的隔离测试配置，验证 17/32/64 项实际可用，65 项按 RLIMIT 失败；不能只删除旧宏就称完成。

## 7. block/devfs 的稳定动态对象

### 7.1 通用原则

两者分别持有自己的动态链表，逐对象分配，不共享重型 device/class 框架。登记先验证参数、分配并初始化，再在短 irqsave 锁内复查重名并发布；分配和驱动/VFS 回调均在锁外。对象发布后不移动。注册失败返回真实 errno，计数、可枚举集合及已有对象保持不变。

现有系统没有运行期硬件卸载。本项只支持注册、查询，以及已经存在的启动期块对象回滚；回滚必须处于驱动已停、IRQ handler 不再执行、对象尚无挂载/打开文件/分区引用的静止阶段。不可把该 API 当成并发 hot-unplug。运行期已发布对象存活至关机，因此读者离开注册表锁后仍能安全持有指针。

### 7.2 block

- 删除 `block_devices[BLOCKDEV_MAX]` 及数值容量常量，保留 `block_device_desc`/ops 的 ARCH-9 契约；链表节点可嵌入对象或作为私有 wrapper，由实现者决定。
- `block_device_count/get(index)` 枚举活跃对象，按成功注册顺序排列；boot unregister 后再注册追加到末尾，不复用旧表洞的枚举位置。默认单盘及根盘选择顺序不变。
- `block_device_unregister_boot()` 按注册表成员身份验证指针，不能再用数组地址范围判断；父盘存在子分区时返回 `-EBUSY`，回滚先子后父。
- 名称长度规则及重复名称拒绝保留；OOM 为 `-ENOMEM`，不再因第 9 个对象返回 `-ENOSPC`。初始化变成幂等初始化，不能重置并泄漏已发布对象。
- 读写越界/溢出/只读/错误状态语义保持不变；AHCI、GPT、devfs、boot 与 fixture 中所有假定固定数组的循环一起迁移。动态对象不意味着复用 AHCI DMA 或放宽其生命周期要求。

### 7.3 devfs

- 每条记录拥有稳定、非零、单调分配的内部 ID；`ino/rdev/readdir` 使用一致 ID。计数/ID 算术溢出明确拒绝，正常注册不人为设置 32 项限制。
- `node->fs_data` 改存 devfs 记录指针，由统一 accessor 校验节点种类/归属并取 ops/private_data；只改数组为 realloc 会留下悬空指针，禁止采用。
- 完整迁移 read/write、poll/poll_file、ioctl/ioctl_file、release_file、open、自定义 tty/PTMX 路径、get_private/get_file_ops、lookup、mount 根节点初始化及 readdir。fake tty 节点要明确绑定真实记录，不能保留整数 fs_data 的隐含兼容分支。
- 保持 `release_file` 在 `vfs_node_put` 前调用；记录存活至关机，mount 卸载释放 VFS 节点但不销毁全局设备记录，重新挂载可重新枚举。
- 单段名称不得为空、含 `/` 或达到 DEVFS_NAME_MAX；返回 `-EINVAL`，不截断为潜在同名。重复完整名称返回 `-EEXIST`。默认 `/dev/null`、zero、random、serial、tty、输入设备等名字保持不变。
- 保留 `VFS_FS_NO_DCACHE` 和按需 lookup，不启用缓存、不预建设备节点。lookup/readdir 按记录查询，mount 后注册的新设备也可被发现；先 lookup 返回 ENOENT，再注册同名设备，后续 lookup 必须成功。lookup 的节点或名字分配失败返回 ENOMEM，并回收本次临时节点；设备记录已成功登记的状态不因独立 lookup 失败回滚。本项无需引入 devfs 负缓存或清除负缓存的协议。
- 所有注册调用者检查错误：启动必需节点失败必须阻止依赖它的用户程序启动，可选设备按既有故障策略回滚/报告；不得把 OOM 当设备缺失。保留 kernel log_* / debug_<channel> 规范。

ARCH-11 后续只需将单段名字索引替换为目录结构；记录对象、ID 和已打开文件的引用方式无需再次改变。

## 8. 工程卫生与构建契约

### 8.1 忽略规则

移除全局 `*.S` 和为抵消它设置的 `!kernel/arch/...` 例外。保留 `/build/` 忽略，并仅为当前/遗留生成器确实产生的 `kallsyms.S` 路径补精确规则，不新增匹配整个汇编源码类别的规则。

验收使用 `git check-ignore`：真实 `kernel/arch/x86_64/cpu/thread_entry.S`、新假想 `.S` 路径及 aarch64 汇编均不得被忽略；build 内 kallsyms.S 和确认的遗留生成路径仍被忽略。记录已存在但因旧规则未跟踪的汇编清单，逐项判断，不批量 add 不明文件。

### 8.2 依赖跟踪与文档

核查并补齐 C、常规 `.S`、特殊 trampoline 等显式规则的 depfile 生成和 `-include` 消费。生成型汇编对象以生成源/链接阶段为依赖，不声称它与普通源相同。相应 host mock/build contract 按真实规则调整。

更新仓库 `AGENTS.md` 的 quick start、critical gotcha，以及仍以“没有头文件依赖”为由要求 clean 的活跃构建文档：正常头文件/结构体改动由 `.d` 驱动重编译；sysroot generation、profile/variant、工具链/flags 变化按 [构建指南](../../build/build.md) 的实际失效机制处理。历史 spec 的当时描述无需批量改写。

在依赖契约验证通过前，实现过程中仍按现有仓库要求执行必要 clean；先证明新规则覆盖 C 和汇编消费者，再撤销过时的强制 clean 描述。boot_context 等跨编译域 ABI 仍需检查双侧布局和正确重建，不能把自动依赖当 ABI 验证。

### 8.3 源码树产物

列出源码树非 profile 构建目录中的产物，核实 Git 跟踪状态、文件类型和生成来源。定向删除已确认的 `kernel/fs/select.o`、`kernel/kernel.bin` 及同类旧构建输出；不得运行 `git clean -fdx`，不得删除用户修改、资源、测试 fixture 或尚未跟踪的汇编源。

补静态审计，覆盖 kernel 源码树遗留 `.o/.d/.elf/.bin` 等实际构建输出，精确排除合法 fixture；fresh build 前后均检查。顶层兼容链接按构建指南保留，不将合法 profile/variant 产物误判为污染。

## 9. 交付切分与文件边界

| 阶段 | 主要文件 | 独立完成门槛 |
|---|---|---|
| A CPU 与卫生 | 公共 limits 头、CPU/procfs/aarch64 头、Makefile、忽略规则、AGENTS、静态审计 | 单点数值来源；双架构 C/汇编一致；依赖与忽略检查通过 |
| B IRQ 容量 | `intr/irq.c` ↔ `include/intr/interrupt.h`；x86 IRQ/backend/IOAPIC/ACPI/vector 头；ISA 调用者、e1000/PCI MSI-X helper | 多 IOAPIC/空洞/ISO/扩展 vector/OOM/耗尽不串路由；固定 MSI-X 与 line IRQ 独立 |
| C poll 与资源限制 | poll/select/file/fd 分配、任务生命周期、sys_resource 及头/UAPI、libc、compat | 真 RLIMIT；默认兼容与 NOFILE=64 扩容；清理和唤醒无回退 |
| D 设备表 | blockdev/devfs 及对应公开头、驱动/GPT/boot 消费者 | >8 block、>32 devfs；地址稳定；错误完整传播 |
| E 集成验收 | hosttests、qemutests、build 文档、roadmap | 所有验收证据齐全后才把 ARCH-10 标为完成 |

以上为设计交付边界，不代替逐步实现计划。新增源与公开头保持子系统对称，禁止把实现放入 `kernel/core/`。spec 阶段只新增本文和 roadmap 入口，不改产品代码或清理产物。

## 10. 验证与最终验收

### Host / 静态

1. **CPU**：生产 NR_CPUS 数值定义唯一；aarch64 兼容名均为别名；C/预处理汇编和 procfs 单独编译一致；mock 特例有清单。
2. **GSI 拓扑**：模拟 `[0,24)` 与 `[32,56)`，验证 GSI 40 可路由、空洞 24..31 拒绝；高起点稀疏范围不按最大编号分配。覆盖重叠、端点溢出、非法 pin 编号、超过四个 IOAPIC、每个分配点 OOM。
3. **IRQ**：保留 0..23 历史 vector；GSI 24（在覆盖它的 fixture）进入扩展池；pool 耗尽/重名不覆盖已有路由；固定 MSI-X/timer/IPI/syscall/spurious 不被 line pool 分配；所有扩展 stub 与 decoder/backend 分类一致。另验证 MSI-X 0x39 与真实 GSI16/0x30 同时登记和独立派发，消息路径 IOAPIC 操作计数为零、LAPIC EOI 恰好一次；槽位冲突与 PCI 配置故障均不提前启用消息、不撤销他人资源，INTx/POLL 回退有效；撤销后的迟到消息不访问旧参数或新 owner。
4. **ISO**：source IRQ0→GSI2 等非 identity 场景从 register 到 dispatch、mask、unregister、trigger/polarity 只转换一次；PIC fallback 不受 IOAPIC 容量影响。迟到 IRQ 不触碰已撤销参数，EOI 仍恰好一次。
5. **poll**：真实生产路径测试 0/1/16 项，以及 NOFILE=64 fixture 的 17/32/64/65 项；重复/负/无效 fd、混合 read/write、整数溢出、输入/输出 EFAULT、每处分配失败、timeout、signal、并发 pollers 和 arm 前后唤醒竞争。退出后 timeout/等待链无残留，释放计数平衡。
6. **资源限制**：默认值、fork 继承、exec 保留、降低限制、超限/非法资源/坏指针；open/dup/pipe/socket/PTY 的统一分配上限；已打开高 fd 降限后仍可读写；libc native 与 Linux compat 查询一致，prlimit 自身进程语义明确。
7. **block**：注册至少 12 个 fake 磁盘/分区；扩容后旧指针与数据不变；重名/OOM/父子回滚/非成员注销/计数及遍历顺序；保留 ARCH-9 ops、边界和只读测试。
8. **devfs**：至少 40 个 fake 设备，包含 mount 后注册；lookup/readdir/open/read/write/poll/ioctl/release 全链验证，旧 fd 在后续注册后仍指向同设备；OOM、同名、超长/带 slash 名称；卸载/重挂载；先 ENOENT 再注册后 lookup 成功，仍保留 NO_DCACHE。
9. **构建卫生**：Git ignore 正反例；源码树污染审计；在隔离构建中更改一个 C 共用头与一个 `.S` 消费头，观察依赖消费者确实重编译、无关对象不误重建，并恢复文件。不能只检查 `.d` 文件存在。

优先扩展 `test_block_device_ops.c`、`test_devfs_input_dispatch.c`、`test_poll_requested.c`、`test_poll_waitqueue_handshake.c`、`test_intr_irq_x86.c` 与 x86 intr backend/audit，用真实被修改的生产函数。新增复杂测试可单独建 case，并纳入现有 test-host/test-static bucket。

### QEMU / 双 profile

- 默认 x86 SMP=1/2：phase-0、inittab-phase、network；另用支持当前 e1000 驱动且提供 MSI-X 能力的受控 fixture 验证固定消息路径，并与真实 GSI16 路由同时触发，缺少相应 QEMU 设备时以 fake PCI/MMIO host 测试为必需证据，不能把 INTx/POLL 通过记成 MSI-X 通过；单独运行 `make OS01_SYSTEST=1 test-syscall`，不得混入 KERNEL_SELFTEST。
- x86 SMP=8 与 aarch64 现有 SMP 冒烟使用构建指南中的现成目标，确认共享容量头未破坏启动栈、percpu 和计时/IPI。
- 新容量 E2E：测试 variant 将 NOFILE 设为 64，用户态实际调用 32/64 项 poll 并触发后半数组的可读唤醒；降低 RLIMIT 后核对 poll 和新 fd 分配限制。fixture 产生超过 8 个块对象与超过 32 个 devfs 节点，用户态读取旧/新节点并验证内容。
- 第二 IOAPIC 的路由正确性以 fake MMIO host 测试为必需证据；若 QEMU 平台确实支持相应拓扑，再增加硬件 E2E，不得把 Q35 单 IOAPIC 启动当作多 IOAPIC 通过。x86 真实扩展 IDT stub 可通过独立测试 variant 的受控中断注入验证；注入必须使用有效注册映射并验证 ACK/恢复路径。
- 执行 `make test-host`、`make test-static` 及构建指南的双 profile build-contract。内核 selftest 独立执行；profile/variant 切换遵守构建失效规则，记录测试实际运行的镜像路径。

最终完成需要同时证明：CPU 单点来源、按真实拓扑覆盖 GSI 且无向量冲突、poll 容量来自真实 RLIMIT、设备数量可越过旧表上限且旧引用稳定、汇编源码默认可跟踪、增量头依赖有效、源码树无确认的旧构建产物。任何一项未完成，不关闭 roadmap ARCH-10。

## 11. 待确认的设计选择

以下为作者提出的默认方案，用户可在设计确认时调整：默认 NOFILE 仍为 16，通过 NOFILE=64 测试 variant 证明扩容；资源限制仅实现 NOFILE；扩展设备向量池限于 0x42..0x7f、现有单槽 MSI-X 独立迁到 0x39；保留最多四个 IOAPIC 的发现界限并显式拒绝超额；devfs 保留单段命名和存活至关机的设备记录。

本文未执行实现测试。

## 12. 规格评审记录

2026-11-01，独立子 agent `spec_builder_check` 只读检查本文，重点核对 IRQ 映射/注销约束、RLIMIT 的 fd 安装范围与 compat、devfs 节点及全局记录寿命，以及 NOFILE=64 和扩展 stub 的验收方式。结论：`nothing important left`，未发现阻碍制定实现计划的关键未决问题。

评审仅针对规格可实施性，不代表用户已批准设计，也不代表实现或回归测试已完成。文档相对链接、范围覆盖及空白格式检查通过。

### 用户要求的严格评审循环

| 轮次 | Reviewer | 结论 | 意见处理 |
|---|---|---|---|
| 1 | `arch10_spec_review` | REQUEST_CHANGES | 修正 1 Important：补 §5.4 固定 MSI-X 独立槽、登记/开启顺序、EOI、撤销与回退，并补共存测试；修正 1 Minor：devfs 保留 NO_DCACHE/按需 lookup，删除预建节点及既有负缓存的失实描述 |
| 2 | `arch10_spec_review` | APPROVE | reviewer 复查最新文件，确认首轮两项意见闭环，固定 MSI-X 所有权/回退及 devfs 生命周期规则一致，无新增矛盾或阻碍实施计划的问题 |

首轮意见已对照 `e1000.c`、`devfs.c` 和 `dcache.c` 核实后修订；第二轮明确 APPROVE。本轮循环仅修改规格，未修改产品代码或执行实现测试。
