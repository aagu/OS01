# USB 存储读取与 U 盘启动设计（x86_64 / QEMU 优先）

日期：2026-11-14
状态：设计文档

## 1. 意图与为什么

目标：**在 QEMU 里从 U 盘启动，并为直连、512 字节逻辑扇区的 BOT U 盘保留同一条真机启动路径。**

当前启动链：

1. OVMF 从 GPT 盘的 ESP（FAT32）加载 `EFI/BOOT/BOOTX64.EFI`；
2. 引导器从同一个 ESP 读 `kernel.bin`，构造 `boot_context`，`ExitBootServices`；
3. 内核在 `xhci_probe()` 里 `xhci_reset()` 做 HCRST，重新接管 xHCI；
4. `arch_boot_devices()` 阶段运行 xHCI 与 AHCI 的 PCI 探测，AHCI 注册 `BLOCK_DISK`；
5. `fs_boot_mounts()` 取第一块 `BLOCK_DISK`，扫 GPT，挂 `partitions[1]`（ext2）为 `/`、`partitions[0]`（FAT32 ESP）为 `/boot`。

缺口是第 4 步：内核没有 USB 大容量存储（MSC）驱动，所以 U 盘上的 ext2 根无法被挂载。UEFI 固件自己的 USB 栈只负责把 `BOOTX64.EFI` + `kernel.bin` 读进内存，`ExitBootServices` 之后与内核无关。

同时，内核无法知道"自己是从哪块盘启动的"——`boot_context`（`kernel/include/core/bootinfo.h:71`）没有任何启动设备信息，`fs_boot_mounts()` 只是取注册表里第一个 `BLOCK_DISK`。真机上如果同时存在内置盘和 U 盘，这会静默挂错根（QEMU 单盘测试暴露不了）。

### 成功标准

- QEMU 中只挂一块 U 盘（`usb-storage` on `qemu-xhci`），能完整启动到用户态 shell。
- QEMU 中同时挂 AHCI 内置盘和 U 盘，内核通过引导器传下来的启动设备信息选中 U 盘并挂根；AHCI-only 启动无回归。
- 同一份 GPT 镜像 `dd` 到真实 U 盘后，真机 UEFI 能引导，内核能读同一块盘上的 ext2 根（真机验证不在本设计的门禁内，但代码路径必须成立）。

## 2. 目标 / 非目标 / 反目标

### 目标

- xHCI bulk 传输（polling，复用现有 event ring 轮询）。
- USB MSC Bulk-Only Transport（BOT）+ 最小 SCSI 命令集。
- 把 MSC 设备注册成 `block_device_t`（`kind = BLOCK_DISK`），沿用现有 `devfs`/GPT/ext2 路径。
- 引导器把启动分区身份写入 `boot_context` v3；内核用它选择根盘。
- QEMU 端到端启动测试（USB-only 与双盘选择两个用例）。

### 非目标（明确不做）

- **不把 USB 子系统改成中断驱动**。见 §4.1 的决策记录。
- 不做热插拔/热移除。设备在启动时存在即可；运行期不支持插拔。
- 不做 USB hub 拓扑（多级 route string、hub class 驱动）。只处理 root port 直连设备。
- 不做 UAS、多个 LUN（只用 LUN 0）、USB Attached SCSI。
- 不做 USB 1.1 companion controller / EHCI/UHCI；只做 xHCI。
- 不做 MBR/legacy BIOS 引导；GPT + UEFI。
- 不做 aarch64 USB（aarch64 平台没有本设计中的 xHCI 路径）。
- 不做写缓存/回写策略优化；`flush` 直接下发 SYNCHRONIZE CACHE。

### 反目标（即使技术上能跑也算失败）

- 为了中断而中断：在功能跑通前引入 IRQ 路由、event ring 竞争、teardown 竞态。
- 改变普通 AHCI-only 镜像的启动结果；缺失启动身份时保留现有首盘行为。已知身份却无法唯一解析时，宁可显式停止也不静默挂错盘。
- 把 USB 特有的协议细节泄漏进通用 block/VFS 层。

## 3. 约束

- xHCI 驱动当前是 polling 设计：`xhci_poll_event_internal()` 按 cycle bit 轮询 event ring，`xhci_start()` 明确 `cmd &= ~XHCI_CMD_INTE`，`xhci_setup_interrupter0()` 保持 `IE=0`。
- bulk endpoint ring 已经建好并配置：`xhci_setup_endpoint_rings()`（`xhci.c:733`）为每个端点分配 ring，`xhci_configure_endpoints()`（`xhci.c:977`）写入 EP context。缺的只是"往 ring 上挂 TRB + 敲 doorbell + 等 Transfer Event"。
- 本设计接受的逻辑扇区固定 512 字节；`gpt_scan()` 和 `devfs` 块路径都按 512 字节工作。
- 内核直接映射的 slab 对象物理连续；当前 64 KiB size class 来自 2 MiB 对齐的物理页。xHCI bulk 适配层仍须显式检查 DMA 窗口的物理边界和地址宽度，见 §5.2。xHCI TRB、事件与端点命令依据 [Intel xHCI 1.2b](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf) §§4.6、4.10、6.4。
- 引导器和内核一起构建，`BOOT_CONTEXT_VERSION` 可以严格升级，不需要向后兼容。
- 根文件系统和 ESP 在同一块 GPT 盘上（已确认）。

## 4. 已定决策

### 4.1 保持 polling，不做中断驱动

理由：

- 启动路径是单线程、同步、无热插拔；设备在 `fs_boot_mounts()` 之前就枚举完了。
- bulk 完成事件和现有控制传输走同一个 event ring，`xhci_poll_transfer()` 已经能等到。
- 中断改造的收益是 CPU 效率/延迟，不是功能；在 U 盘启动这个目标下收益为零，风险为正。
- 后续若需要，可先把等待改成 "poll + schedule"，再考虑 IRQ；`xhci_wait_event()` 的 API 可以保持不变。

### 4.2 启动设备信息由引导器传下来

`boot_context` 升到 v3，新增 `boot_device` 记录。内核用它匹配 GPT 分区，而不是靠端口号或 PCI BDF：

- GUID 跨传输层成立（xHCI/AHCI/NVMe 一视同仁）。
- 内核重新枚举后 slot/port 会变，GPT GUID 不变。
- 镜像 `dd` 后 GUID 保留；同时接入两个克隆时该标识有歧义，按 §5.8 拒绝猜盘。

### 4.3 形态：ESP 的 GPT UniquePartitionGUID

引导器从 UEFI device path 的 HardDrive 节点取 `PartitionSignature`、`PartitionStart`、`PartitionSize`、`PartitionNumber`。内核扫每块盘的 GPT，用 16 字节原始 GUID 匹配。两边都存 EFI 混合端序的磁盘字节，直接 `memcmp`，不做文本化。

## 5. 设计

### 5.1 分层与文件布局

```
kernel/include/driver/usb_msc.h      # 通用 MSC 接口 + transport ops
kernel/driver/usb_msc.c              # BOT + SCSI + block_device_t（架构无关）
kernel/include/driver/xhci.h         # 增加 bulk/control 传输与 interface 端点字段
kernel/arch/x86_64/driver/xhci.c     # xhci_control_transfer / xhci_bulk_transfer
kernel/fs/boot.c                     # 启动盘选择与挂载
kernel/include/fs/boot.h             # fs_boot_mounts 签名变更
kernel/include/core/bootinfo.h       # boot_context v3 + boot_device_info
kernel/fs/gpt.c                      # 记录 unique partition GUID
boot/uefi/boot_device.c              # 纯函数：device path -> boot_device_info
boot/uefi/main.c                     # capture_boot_device()
mk/components/uefi.mk                # 两个 EFI profile 的源与 receipt 输入
kernel/arch/x86_64/driver/xhci_msc.c  # 传输适配与可选存储子系统注册
mk/components/run.mk                 # run-usb / QEMU 摆法
qemutests/usb_boot.py                # 端到端用例
hosttests/cases/test_usb_msc.c       # BOT/SCSI 状态机
hosttests/cases/test_boot_devpath.c  # HD 节点解析
hosttests/cases/test_gpt_bootdev.c   # GUID 匹配/启动盘选择
```

`usb_msc.c` 只调用 transport ops；不包含 xHCI 头、不遍历控制器。x86 适配器枚举已配置的 xHCI slot，将接口号、bulk 端点地址及 transport ops 交给通用 MSC probe。通过 `register_subsys_deps()` 注册可选 MSC 存储子系统，在 `SUBSYS_PHASE_STORAGE` 中声明依赖 `device-boot`，于现有 PCI 驱动探测之后、`fs_boot_mounts()` 之前运行；没有设备或单个设备探测失败不影响 AHCI。通用 `kernel_main` 不硬接 xHCI/MSC。aarch64 不链接适配器。

```c
struct usb_msc_transport {
    void *ctx;
    int (*control_no_data)(void *ctx, uint8_t request_type, uint8_t request,
                           uint16_t value, uint16_t index);
    int (*bulk)(void *ctx, uint8_t ep_addr, void *buf, size_t len, size_t *actual);
    int (*clear_halt)(void *ctx, uint8_t ep_addr);
};
```

`bulk` 成功时提供实际长度（包括短 IN）；STALL 返回 `-EPIPE`，超时返回 `-ETIMEDOUT`。MSC 负责 BOT 命令、reset 后 IN/OUT 的顺序；`clear_halt` 是一次 USB CLEAR_FEATURE 与对应 host 状态恢复的组合操作，适配器保证二者同步。MSC 缓冲区是 CPU 内存，适配器拥有 DMA staging，不把任意调用者地址交给控制器。

### 5.2 xHCI 侧新增

#### 配置和接口

保留 `slot->iface`（interface 0 / alt 0，供 lsusb）。从现有选中并成功 SET_CONFIGURATION 的配置中选择描述符顺序第一个完整的 interface **alt 0**：class 0x08 / subclass 0x06 / protocol 0x50，具备有效的 bulk IN 和 OUT。接口号不必为 0；端点必须属于同一个接口，保存 USB 端点地址及 DCI（`ep_num * 2 + is_in`）。不发送 SET_INTERFACE。两个 descriptor pass（记录与建 ring）都只消费活跃的 alt 0 端点；不得让非活跃 alternate 的同 DCI 覆盖实际 endpoint context。已选端点必须与有效 ring 的方向、类型一致。若以 SuperSpeed 枚举，bulk endpoint context 的 Max Burst Size 从紧随端点的 SuperSpeed companion 描述符取 bMaxBurst，streams 保持关闭；缺有效 companion 则跳过该 MSC 接口。M1 的自动化门禁固定 USB 2.0 High Speed（QEMU qemu-xhci 的 USB 2 root port）；M3 再将 SuperSpeed 单列验证。

#### 控制和 bulk

- 新控制原语只支持无数据阶段（BOT reset、CLEAR_FEATURE）；复用 EP0 Setup/Status TRB，Status 为 IN，IOC 在 Status Stage。完成按 slot、DCI=1 及本 TD 的 TRB 地址匹配；错误事件可指向 Setup，不把它当成成功。以后有实际用户再扩展控制数据阶段。
- bulk 原语每次只提交一个 Normal TRB，`1 <= len <= 65536`，且该段 **物理地址不得跨 64 KiB 边界**。适配器独占每 slot 一块经过检查的 64 KiB 对齐 DMA staging；检查整个窗口符合控制器地址宽度；CBW、数据、CSW 分别从该窗口基址开始。发送前复制 OUT；仅在确认完成且长度有效后复制实际 IN 内容。MSC 的 read-ahead 数据另存，不会被 CSW、探测命令或下一次失败 DMA 覆写。
- 单 TRB 就是整个 TD；TD Size=0、IOC=1。同步消费，上一 TD 未退休不能再用其 TRB 或缓冲区。跳过 index 255 的 Link TRB，发布正确的 Link cycle（CH=0，TC=1），回绕到 index 0 后切换 producer cycle。先写 parameter/status，再发布 cycle 并保证对 DMA 可见，最后敲 slot doorbell（target=DCI）。
- 等待本次 TRB 的 Transfer Event：匹配 slot、DCI（event control[20:16]）及 TRB 物理指针（ED=0）。接受 SUCCESS 或 bulk IN SHORT_PACKET；`actual = len - residual`，residual 大于 len 或 OUT 短传输为错误。STALL 映射为 `-EPIPE`；其他完成码按端点状态恢复或停用，不能直接重用尚未退休的 TD。公共 event ring 继续消费非目标事件；启动时已有 EP0 调用也要传 DCI=1。
- 每控制器 `io_lock` 包住提交、轮询、数据复制及 xHCI 恢复命令。运行期调用限可睡眠任务上下文；用 `vfs_sched_ready()` 区分单线程 bootstrap 和运行期，记录本次是否加锁。每设备另有锁覆盖整笔 BOT（CBW 至 CSW、恢复、重试）及 cache 查找/填充/复制；固定顺序 **MSC 设备锁 → 控制器锁**，控制器锁内不回调 MSC。不同设备可在相继完成的 USB TD 之间使用控制器，不得同时消费 event ring。
- `clear_halt` 持控制器锁，且设备锁保证没有并发 BOT TD。Halted pipe 按 xHCI §4.6.8 执行 Reset Endpoint（type 14，TSP=0）→ EP0 CLEAR_FEATURE(ENDPOINT_HALT) → Set TR Dequeue Pointer（type 16）。指向被舍弃 TD 之后的下一 Normal TRB；若 enqueue 位于 Link，先正规化为 index 0 与翻转后的 cycle；DCS 为该位置的 producer cycle。
- 对本来没有 Halt 的 pipe，USB CLEAR_FEATURE 也会重置设备侧 toggle/sequence。确认没有未退休 TD 且端点为 Stopped 或 Running but Idle，发 CLEAR_FEATURE 后以 Configure Endpoint（为该 DCI 同时置 Drop+Add，保留端点参数和正确 dequeue/DCS）重置 host 侧 toggle/sequence。不能对 Running pipe 发 Reset Endpoint/Set TR Dequeue。不能确认状态或恢复命令失败就停用，不再敲该 pipe 的 doorbell。
- 超时不能假定 DMA 已停止。M1 对无法确认退休的 TD 将 slot 标为不可用，后续请求拒绝；仍可能被访问的 DMA 窗口、ring 和 context 保留至确认该 slot 已禁用或控制器已停。若是 command ring 命令超时、无法安全继续命令提交，则停用整个控制器，并保留所涉 DMA。探测失败也遵守该规则，不能走无条件 free。
- M3：针对超时 TD 发 Stop Endpoint（type 15，SP=0）。同一 event ring 按 xHCI §4.6.9 记录先到的 Stopped Transfer Event（以 slot/DCI 匹配，可能指向卸完旧 TD 后的 dequeue），以及之后该命令对应的成功 Command Completion；若旧 TD 抢先正常完成，空 ring 仍须有 Force Stopped Event，不能拿正常完成替代。确认 Stopped 后直接 Set TR Dequeue。若 Stop 返回 Context State Error 且输出 context 已确认 Halted，按 Halted 恢复路径处理；其他未能确认安全的情况停用并保留 DMA。随后由 MSC 按 §5.3 执行完整 BOT Reset Recovery；EP0/命令超时不要求 M3 重试。

### 5.3 MSC Bulk-Only Transport

按 [USB-IF BOT 1.0 §5.3.4、§6.3、§6.5–6.7](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf) 实现：

1. 发恰好 31 字节 CBW（签名 `0x43425355`，递增 tag，transfer length，方向，LUN 0，CDB 长度与 16 字节 CDB 区；多字节 CBW 字段为小端）。无数据命令直接读 CSW。
2. 数据阶段按 CBW 方向传输；数据 pipe STALL 时，调用该 pipe 的 `clear_halt`，随后照常读 CSW，保留已取得的字节数；拿不到可信字节数时不把部分数据交给调用者。IN 短包也进入 CSW。
3. 读恰好 13 字节 CSW。第一次 CSW IN STALL 清该 pipe 并重读 CSW 一次；第二次 STALL 进入完整 Reset Recovery。验证签名 `0x53425355`、回显 tag、实际长度、合法状态；对 pass/fail 验证 residue 不超过预期长度，phase error 的 residue 不用于计算数据。IN 最多信任 `min(transport actual, expected - residue)`，不会读出未填充的数据。WRITE 要求实际 OUT 长度等于 CBW 长度且 residue 为 0；无数据命令的合法 residue 为 0。WRITE 即使收到 CSW status 0，数据不完整也返回错误；READ 的短传输按 §5.4 判断。status 1 返回命令失败（可 REQUEST SENSE），不因其本身重置；status 2、无效 CSW、CBW 发送失败或其他导致 BOT 失步的传输错误须 Reset Recovery。
4. 完整 Reset Recovery：发 Mass Storage Reset（`0x21/0xff`，`wValue=0, wIndex=interface`），依次对 bulk IN、bulk OUT 调 `clear_halt`（底层标准请求 `0x02/0x01`，`wValue=0, wIndex=USB endpoint address`）。无论是否发生 Halt，都按 §5.2 同步主机与设备两边。任一步失败即停用该设备；EP0 或命令超时使用 §5.2 的 DMA 生命周期规则。
5. 恢复完成后最多重发一次安全的命令，构造带新 tag 的 CBW。WRITE/FLUSH 不自动重发（原操作可能已执行）；返回错误，调用者决定后续操作。再次失步仍需复位以恢复同步；恢复失败或再次超时按上面的停用规则处理。M1 超时直接停用，不发 BOT reset。

不发 GET_MAX_LUN，固定使用 LUN 0。BOT 所需控制请求若在 EP0 STALL，当前设备恢复失败并停用；不增加通用 EP0 STALL 恢复。

### 5.4 SCSI 最小命令集

| 命令 | 用途 | 备注 |
|---|---|---|
| TEST UNIT READY (0x00) | 等设备就绪 | 未就绪时短延时重试 |
| INQUIRY (0x12) | 日志打印 vendor/product/rev | 标准 36 字节响应 |
| READ CAPACITY(10) (0x25) | 容量 + 块大小 | 要求块大小 = 512，否则拒绝注册 |
| READ(10) (0x28) | 读扇区 | 每次 ≤ 128 扇区（64 KiB staging） |
| WRITE(10) (0x2A) | 写扇区 | M3 实现；用于保持 ext2 可写 |
| SYNCHRONIZE CACHE(10) (0x35) | flush | M3 实现 |
| REQUEST SENSE (0x03) | 失败诊断 | 可选，仅日志 |

容量：`READ CAPACITY(10)` 必须返回完整的 8 字节，解析两个大端字段；last LBA 为 `0xffffffff` 时需要 READ CAPACITY(16) 才能消除歧义，本设计拒绝注册。其他值用 64 位算 `sector_count = last_lba + 1`。READ/WRITE(10) CDB 的 LBA 与传输块数为大端。

`block_device_ops`：

```c
.read  -> usb_msc_read(dev, lba, count, buf)
.write -> usb_msc_write(dev, lba, count, buf)   /* M3；M1 为 NULL -> -EROFS */
.flush -> usb_msc_flush(dev)                    /* M3 */
```

`block_device_register()` 只要求 `read`，所以 M1 只读启动可行。

#### 读放大：bcache 只按 1 扇区调用驱动

必须显式处理：`block_device_read()` 对物理盘逐扇区走 `bcache_read_sector()`，后者填充时调用 `dev->ops->read(dev, lba, 1, buf)`（`kernel/block/bcache.c:519`）。所以正常 bcache 填充路径里 `usb_msc_read()` 收到 `count == 1`；如果不做处理，64 KiB staging buffer 和"128 扇区/READ(10)"都不会被用到，挂 ext2 根会退化成每 512 字节一次 CBW/数据/CSW 往返，QEMU 下可能直接撞 60 秒测试超时。

因此 `usb_msc` 内部实现一个 64 KiB read-ahead cache：

- 1 扇区读未命中时，从请求 LBA 起发一个 READ(10)，扇区数 clamp 到 `min(128, sector_count - lba)`，读取到 CPU scratch；仅在 BOT 成功并确定可信完整扇区数后发布新 cache，再复制请求扇区。**必须 clamp**：否则读盘尾最后几扇区时，READ(10) 越过介质末尾会返回 CHECK CONDITION/ILLEGAL REQUEST，合法的读也会失败。
- 命中窗口 `[prefetch_lba, prefetch_lba + prefetch_count)` 内直接 memcpy，不再发命令。
- 请求跨窗口边界时：先服务窗口内的部分，再对剩余部分重新填充；不要假设一次 prefetch 覆盖整个请求。
- 短传输/residue 策略：CSW status 0 但可信数据少于请求字节数时视为短传输；按 512 字节向下取整计算可用扇区。prefetch 短传输只要覆盖了请求扇区就接受（cache 只标记实际读到的扇区数）；没有覆盖则 `-EIO`。非 prefetch 的直接读短传输一律 `-EIO`，不允许把部分填充的缓冲区交给块层。
- M3 发送 WRITE 前先失效重叠窗口（包括稍后返回错误的 WRITE），防止设备实际写入后仍命中旧数据。
- 随机读最多多读 64 KiB，可接受；顺序读把往返次数降低约 128 倍。

**并发**：read-ahead 窗口是 MSC 设备私有状态。`bcache` 只对同一 LBA 的填充去重，不同 LBA 的 miss 会并发调用 `dev->ops->read`；`/`（ext2）和 `/boot`（vfat）是两个独立挂载，裸 `/dev/usba` 读取还会绕过两者。因此 MSC 设备必须有自己的 mutex，覆盖"窗口查找 + 填充 + 拷贝"整个过程；控制器的 `io_lock` 仍只在 transport 内部持有。固定加锁顺序 **MSC 设备锁 → 控制器 io_lock**，控制器锁内不得回调 MSC 层。`vfs_sched_ready()` 为 false 时（boot 单线程）两者都跳过。

宿主测试要覆盖：连续 200 个 1 扇区读只产生每 128 扇区一次 transport 调用、跨窗口边界、盘尾 clamp、短 prefetch 覆盖/不覆盖请求扇区、写入使窗口失效，以及两个逻辑读者并发时窗口不被互相覆写。不改 `bcache` 本身：那是全局路径，影响所有驱动。

### 5.5 块设备注册与探测时序

- 名称：`usba`、`usbb`…（沿用 AHCI `hda`…`hdz` 的单字母风格，且不以数字结尾）。`block_device_create_partition()` 的命名规则是"父名 + 序号"，所以分区自然得到 `usba1`、`usba2`；不要用 `usb0`，否则分区会变成容易误读的 `usb01`。
- `sector_count` 来自 READ CAPACITY，`sector_size = 512`，`kind = BLOCK_DISK`，`parent = NULL`。
- 探测入口：按 §5.1 作为 x86 可选存储子系统运行。按控制器枚举顺序和 slot 编号给成功注册的设备分配全局 `usba`…`usbz`；耗尽则 warn 并跳过，不重用名称。
- 探测失败（设备不响应、非 BOT、块大小非 512）只记日志并跳过，不 panic；是否致命取决于根是否在它上面（由 `fs_boot_mounts()` 决定）。
- 不注册 devfs 节点：`fs_boot_mounts()` 的 `register_block_devices()` 会统一注册（现有行为）。

### 5.6 boot_context v3

```c
#define BOOT_CONTEXT_VERSION 3u
#define BOOT_CONTEXT_HAS_BOOT_DEVICE (1u << 6)

struct boot_device_info {
    uint8_t  partition_guid[16];  /* ESP 的 GPT UniquePartitionGUID，磁盘字节序 */
    uint64_t partition_start_lba;
    uint64_t partition_size_lba;
    uint32_t partition_number;
    uint32_t signature_type;      /* UEFI HD 节点：1 = MBR, 2 = GPT */
};

struct boot_context {
    /* ...existing fields... */
    struct boot_device_info boot_device;  /* valid iff flags & HAS_BOOT_DEVICE */
};
```

`partition_start_lba` / `partition_size_lba` / `partition_number` 不作为匹配主键，只用于日志和一致性校验：GUID 匹配后，如果记录的 start/size/number 与实际 GPT 条目不一致，打警告（镜像被重新分区但仍复用了 GUID），仍以 GUID 为准。匹配主键是 GUID。

- `boot_context_valid()` 仍严格比对版本；引导器与内核必须一起构建。
- 固定宽度字段追加在 v2 尾部：`boot_device` offset 136，长度 40，v3 总大小 176；原字段偏移不变。x86 context 必须在 0x60000 的第一页内，不侵入相邻内存表；同步检查 aarch64 的 context 预留区。给 EFI 与内核编译都可见的大小/偏移加静态断言，更新两侧 handoff 注释，并验证两个 profile 的 EFI/kernel 构建和启动。
- aarch64 共用同一结构体；它没有 ext2 根，`boot_device` 填了也不使用，代码不分叉。

### 5.7 引导器：捕获启动设备

新增纯函数（无 UEFI 依赖，可被 hosttest 直接编译）：

```c
/* boot/uefi/boot_device.c */
int os01_bootdev_from_devpath(const uint8_t *dp, size_t max_len,
                              struct boot_device_info *out);
```

契约（[UEFI Device Path §10.3.5、§10.5](https://uefi.org/specs/UEFI/2.11/10_Protocols_Device_Path_Protocol.html)）：

- `max_len` 是调用方提供的实际可读字节数。逐字节或用安全拷贝解码 little endian；每个节点先检查至少 4 字节 header、`4 <= node_len <= remaining`。HD 节点是 Type **0x04**（Media）、Subtype 0x01、长度 42；0x03 是 Messaging。
- HD 节点字段从 offset 4 起：PartitionNumber (LE32)、Start/Size (LE64)、Signature[16]、MBRType、SignatureType。要求 MBRType=0x02、SignatureType=0x02，GUID 非零，分区号与长度非零。GUID 字节原样复制。
- 要求单实例路径完整到 End Entire（0x7f/0xff，长度 4，恰好消费调用方长度），恰好一个有效 HD 节点。拒绝 End Instance、重复 HD、截断、无结束节点或非法长度；成功时才写 `out`。

`boot/uefi/main.c` 在最终 GetMemoryMap/ExitBootServices 循环**之前**捕获：

1. 用 `BS->HandleProtocol(LIP->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, &dp)` 取加载介质 handle 的路径，和 posix-uefi 打开内核文件的文件系统同源；不能拿仅含 EFI 文件路径的 Loaded Image FilePath 字段替代。
2. EFI Device Path Protocol 不附长度，固件提供的是可信的、End Entire 结尾的对象。优先用 Device Path Utilities Protocol 的 GetDevicePathSize；缺失时允许在固件路径上限 4 KiB 内做一次只读节点遍历，遇到 End Entire 得到实际长度。两者都限制解析长度不超过 4 KiB。后备遍历的上限限制访问范围，不能证明损坏的固件指针有同等大小的分配；host parser 才对其真实切片有严格边界保证。
3. 解析成功则填字段并设置 HAS_BOOT_DEVICE，失败清 flag 并记原因；不使 EFI 启动失败。接入两个 profile 的 EFI source 与 receipt 依赖；aarch64 共享 ABI，但不消费此字段。

### 5.8 内核：启动盘选择与挂载

`fs_boot_mounts(const struct boot_context *bootctx)` 仍先调用 `register_block_devices()`；更新 `kernel/core/main.c` 和测试 fixture 调用点。之后：

1. **没有 HAS_BOOT_DEVICE**：走旧的首个 `BLOCK_DISK` 路径，只扫首盘。没有盘返回 `-ENODEV`；`gpt_scan()` 为 NULL 则整盘挂 vfat 到 `/`；有 GPT 且少于两分区返回 `-EINVAL`；否则保留 FAT32/根分区的原有位置顺序及错误处理。记录使用首盘回退的原因。
2. **有 HAS_BOOT_DEVICE**：先做只读 GPT 元数据解析，后发布分区。快照物理 `BLOCK_DISK`，忽略后续生成的分区节点。每盘只解析一次 GPT：用现有 CRC/大小限制作为基础，增加 `92 <= header_size <= 512`、非零且有界的表参数、无溢出的 LBA 范围检查，并检查每条使用中分区的范围。扫描所有有效条目，不受当前 `GPT_PARTITION_MAX=16` 和分区注册成功与否影响。可让旧 `gpt_scan()` 复用解析帮手，保持其发布顺序和既有 NULL/少分区语义。新路径不读取备用 GPT。
3. 元数据保留类型 GUID、offset 16 的 unique GUID、起止 LBA 和真实 GPT 槽位号（从 1 开始，计入空槽），不要拿现有压缩后的设备名后缀当槽位号。匹配所有有效条目的 ESP 类型 GUID `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` 及原始 unique GUID。全体候选盘恰好一项才继续；几何/槽号不一致仅告警。没有 GPT 或校验确定无效的盘不是候选；I/O、分配或实现上限导致无法完成判断时，报错停止，不能把未读完整的盘算作无匹配。
4. 选中的盘恰好有一个 Linux filesystem data 类型 `0FC63DAF-8483-4772-8E79-3D69D8477DE4` 时用它挂 ext2；没有该类型且恰好只有一个非 ESP 分区时用那个分区。多个 Linux 类型、其他不唯一布局、零匹配、重复 ESP GUID 或必要分区发布失败均记录具体原因，通过原有 `boot_failure_stop("fs", "root", ...)` 停止。不在有身份时退到首盘，也不因 mount 失败尝试其他盘。
5. 仅发布选中盘的分区；全盘块节点仍由原流程注册。对于典型镜像保持 `usba1`、`usba2`。现有最多发布 16 个分区的限制保留；如果所需 ESP 或根不在可发布集合内，明确报布局不支持，禁止丢掉候选再猜盘。释放扫描元数据及未发布的临时记录；已发布的分区设备继续归 registry 所有。
6. 根挂载使用所选 ext2 分区；`/boot` 使用匹配到的 ESP。保留已有 root failure 注入、root-ready sentinel、`/boot` 失败仅告警和 `tmpfs`、`procfs` 顺序。GUID 选择日志在根挂载成功后记录，并报告 `/boot` 是否实际挂载。

已知启动身份与容错是不同路径：复制同一镜像后同时接上两个克隆，系统会报歧义；拔掉一个或为磁盘重建独立 GPT GUID 后再启动。

### 5.9 QEMU 验证摆法

新增 `make run-usb` 和 `make test-qemu-usb-boot`（M0 专用目标；M1b 起该 suite 登记到生成的 suite catalog，届时也可用 `make test-qemu SUITE=usb-boot`），仅对 x86_64 用户态 profile 可用；将 suite 接到现有 dispatcher 并登记在 `docs/build/build.md`。使用 profile 的 OVMF、镜像和环境；保留 q35、512 MiB、SMP、串口收发和 `virtio-rng-pci` 接 `/dev/urandom`。USB-only 不添加隐式 IDE/AHCI/virtio 磁盘，USB 2.0 root port 直连示例：

```
-drive file=<USB image>,format=raw,if=none,id=usbdisk
-device qemu-xhci,id=xhci,p2=4,p3=0
-device usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0
```

`usb_boot.py` 通过共享的 `qemutests/harness/qemu_cmd.py` 选择 `storage="usb"` 存储拓扑（共享 `run_test.py` 的 argv 始终添加另一块磁盘）；可复用串口/prompt 辅助逻辑。自动门禁固定 bootindex，AHCI 盘设较低优先级（例如 bootindex=1）。固件若不按预期启动，菜单可用于 M0 手工诊断；`-boot order` 不能与 bootindex 混用，两者都不能替代自动门禁。

- M0：仅 USB。证明固件加载了 EFI 与 kernel 并进入 `kernel_main`；此时旧内核找不到块盘并停止，属于预期。
- M1：仅 USB，断言 `usb_msc: usba: ... sectors, 512-byte blocks`，到 shell 后读取 USB 镜像预置的 ext2 和 ESP 文件，各得到对应内容。此阶段无 v3/GUID 日志要求。
- M2：用独立构建的、各自可启动且可挂载的 AHCI/USB 镜像，GUID 不同，分别在 ext2 根和 ESP 的相同路径预置不同内容。断言 AHCI 确实先进入注册表，EFI 捕获的 ESP GUID 等于 USB 镜像 GUID，根挂载后输出 `fs: boot disk selected by GUID (usba), root=usba2 boot=usba1`，并从 shell 读取 `/` 和 `/boot` 的文件、收到只存在于 USB 镜像的内容。断言值不能出现在注入命令中，避免把串口回显当结果。AHCI 镜像若被错误选中也会启动，因此只看提示符不足以通过。
- M3：在一次性可写 USB 镜像上由 guest 写 ext2 文件并调用 `sync()`；断言写入与设备 flush 成功（包含真实 WRITE(10) 与 SYNCHRONIZE CACHE(10) 路径），关闭 QEMU 后用同一份镜像新启动并读取已持久化内容。禁止使用重启后丢弃写入的 overlay。另跑 SuperSpeed USB-only，复用双分区读验收。
- AHCI-only：继续运行现有 phase-0，对同样的 AHCI 测试镜像验证其两个 sentinel。原有 AHCI-only 无 GUID 场景由 host fixture 锁住。

### 5.10 测试策略

**Host tests（`hosttests/cases/`）**

- `test_usb_msc.c`：以 fake transport 验证 CBW 字节、CDB 端序、命令 pass/fail/phase error、实际长度与 residue、无效签名/tag、data STALL 后读 CSW、首次与重复 CSW STALL、两个 pipe 都未 Halt 时仍执行完整 Reset Recovery、恢复失败后拒绝新请求，以及 M1 超时绝不发第二个 CBW。验证非 512 块及 READ CAPACITY(10) 哨兵拒绝注册，READ(10) 分割、200 个连续单扇区读只发两次 READ、跨窗口、盘尾、残余数据和并发填充；M3 加 WRITE/FLUSH 的 CDB 与 payload、拒绝 WRITE 的非零 residue、失败写也失效、WRITE/FLUSH 不自动重发。
- 复用 `test_xhci_phase2.c` / `test_xhci_phase3.c` 的合成事件/MMIO fixture：验证 alt 0、DMA 边界、错 slot/DCI/TRB 不匹配、短包的 actual、ring 回绕、dequeue 落在 Link 时的地址/DCS、区分 Halted 的 Reset → ClearFeature → Set Dequeue 与空闲非 Halted 的 ClearFeature → Drop+Add；迟到完成事件不能成为新请求完成。M1 测超时后 DMA 不释放/复用；M3 加中途 Stop、旧 TD 先成功后再收到 FSE、Stop 与 Halted 状态竞争、缺少 FSE/命令完成时拒绝复用。
- `test_boot_devpath.c`：使用字节 fixture 覆盖真实的 `04 01 2a 00` HD header、原样 GUID 和非对齐 LE 字段、错误 GPT/MBR 标识、重复 HD、End Instance、缺 End Entire、坏长度及截断。
- `test_gpt_bootdev.c`：挂载 fixture 精确检查设备；覆盖 GUID 选注册较晚的盘，唯一根类型位序变动，槽位号有洞、几何只告警，未识别身份保留旧路径，已知身份零匹配、重复 GUID、根布局歧义、扫描未完成和分区发布失败均停止。用实际 GPT 解析 fixture 验证过长 header、CRC/表范围无效、超过旧 16 条上的重复 GUID 能被发现。
- M2 运行 `make test-host` / `make test-kernel-layout`，并使用两个 profile 的 EFI/kernel 构建与各自基础启动门禁验证共享 v3 ABI。

**QEMU tests**：按 §5.9 分阶段验证。成功 Stop 的 FSE 契约通过合成事件验证；若模拟器未发规范要求的 FSE，保持停用与 DMA 隔离，不放宽恢复条件。不新增中断测试。

### 5.11 错误处理摘要

| 场景 | 行为 |
|---|---|
| M1 超时 | 返回 `-ETIMEDOUT`，按 §5.2 停用与保留仍可能 DMA 的资源 |
| M3 bulk 超时 | 停止端点、确认旧 TD 退出、恢复 dequeue，再按 §5.3 恢复 BOT；未确认即停用 |
| data STALL / 首次 CSW STALL | 仅恢复对应的 pipe；继续读 CSW |
| 重复 CSW STALL / 无效 CSW / phase error | 完整 Reset Recovery 后最多重试一次安全命令 |
| 命令 fail | `-EIO`，可按需 REQUEST SENSE |
| 短 IN | 根据 transport actual 与 residue 共同判断，块读只交付完整且覆盖请求的扇区 |
| TUR 未就绪 / 几何不支持 | 有限重试 TUR；失败或非 512/容量不明则跳过注册 |
| 无启动身份 / 有身份但无法唯一选择 | 前者沿用首盘路径；后者明确停止（§5.8） |

### 5.12 真机加固（M3，非 QEMU 门禁）

- **控制器所有权**：若 xHCI 扩展能力包含 USB Legacy Support，按 xHCI §4.22.1 在 HCRST 前完成 BIOS/OS ownership handoff，并处理 legacy SMI；超时跳过该控制器。明确支持的硬件组合是 root port 直连、BOT/SCSI、LUN 0、512 字节逻辑块。
- **DMA**：任何控制器 DMA 前检查 `HCCPARAMS1.AC64`；AC64=0 时 DCBAA、scratchpad、设备 context、环与 staging 整段均须低于 4 GiB。不能满足时在开始 DMA 前拒绝 probe。为需要非零 scratchpad 的 xHC 分配并初始化 scratchpad 指针数组与页（或显式报不支持并限定真机验收控制器为零 scratchpad）。
- **高速与超高速**：用 Supported Protocol 能力区分 USB 2/USB 3 root port。USB 3 已启用且 U0 时可直接枚举；按链路状态选热复位 PR 或暖复位 WPR，不能把 PR 当暖复位。验证 companion/Max Burst；另测 USB 3 直连启动。hub、UAS-only 设备及不支持的控制器保持不在范围内。
- **真机条件**：若有满足上述范围且可用的真机，验证 USB-only 与带不同 GUID 内置盘的 USB 启动。真机验证不是 M0–M2 的门禁，不凭 QEMU 结果宣称已验证真机。

## 6. 分阶段交付

| 阶段 | 内容 | 验收 |
|---|---|---|
| M0 | 用现有镜像（`usb-storage` on `qemu-xhci`）验证 OVMF 能加载 `BOOTX64.EFI` + `kernel.bin` | 串口日志进入 `kernel_main`/设备初始化；当前内核会在 `fs_boot_mounts` 失败，属预期。失败则先解决固件/加载器，不写驱动 |
| M1 | `xhci_control_transfer` + `xhci_bulk_transfer` + BOT pipe 与 Reset Recovery、超时停用保护 + `usb_msc.c`（只读，含 64 KiB read-ahead）+ block device 注册 | QEMU 纯 USB 启动到 shell 并读取两处分区；host BOT/read-ahead 与 xHCI 适配测试通过 |
| M2 | `boot_context` v3 + 引导器捕获 + GPT GUID 匹配 + `fs_boot_mounts` 选择 | QEMU 双盘证明两处分区来自 USB；AHCI-only 无回归；两架构 ABI 检查 |
| M3 | WRITE(10)/SYNCHRONIZE CACHE + 超时 Stop Endpoint 恢复 + AC64/DMA/真机加固 | host 恢复及写路径测试；QEMU USB 写入后重启读回；真机验证（条件允许时） |
| M4（可选，不在本次） | poll + schedule → 中断驱动 | 性能/延迟有明确需求时再做 |

M1 不依赖 M2：纯 USB 场景下 `find_first_disk()` 会选中唯一的 `usba`。M2 提供可验证的多盘选择，并在身份歧义时停止。

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| OVMF 固件根本不带 USB 启动栈，或读不了 `usb-storage` | 整个 U 盘启动前提不成立 | M0 先做一次 spike；失败则换或修复带 xHCI + MSC 支持的 edk2 OVMF；仍失败则重新评估目标 |
| QEMU `usb-storage` 在 OVMF 下 bootindex 不生效 | 无法从 USB 引导 | 用 OVMF 启动菜单/`-boot order` 验证；只用于诊断，自动门禁仍须证明 USB 引导 |
| `kmalloc` 缓冲区对齐/连续性在真机上不满足 xHCI | bulk 传输失败 | 统一走 64 KiB staging buffer（64K 对齐、物理连续） |
| 多端点 Transfer Event 误匹配 | 读数据错乱 | 按 slot/DCI/TD 过滤，超时不复用未退休的资源 |
| 并发读同一控制器 | ring 状态竞争 | 每设备事务锁与控制器级 `io_lock` |
| 真机 DMA 在 4 GiB 以上 | 控制器报错/数据损坏 | M3 检查 `AC64` 并约束分配 |
| 真机 USB 口在 hub 后 | 设备不可见 | 明确非目标；作为单独项目 |
| 普通 U 盘 BOT 怪癖 | 读失败 | 有界的 BOT 恢复与 REQUEST SENSE 日志 |

## 8. 留给实现者的决定

以下细节由实现者选择，外部契约以 §5 为准：

- 有界轮询超时、TEST UNIT READY 重试次数及延时；同时满足 firmware/USB 等待时序。
- 只读 GPT 元数据在实现中的临时存储方式与 host fixtures 的编译接线。
- 实现 M3 前维持只读块设备；SCSI 写和 flush 支持由 M3 提供。
