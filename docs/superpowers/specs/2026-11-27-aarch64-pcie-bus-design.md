# AArch64 PCIe 总线一期（QEMU virt 通用 ECAM 枚举与树莓派 5 架构预留）设计方案

**状态**：已评审定稿（第 4 轮评审修订，待实施）
**日期**：2026-11-27
**适用架构与目标仓库**：AArch64（`/home/aagu/OS01`，Craton / OS01 内核）
**关联文档**：[实施计划](../plans/2026-11-27-aarch64-pcie-bus.md)

---

## 1. 目标、非目标与设计原则

### 1.1 分期演进目标
1. **一期完整落地（QEMU `virt` 通用 ECAM 控制器）**：
   - 通过 DTB 识别并解析 `compatible = "pci-host-ecam-generic"` 节点，完成 Host Bridge 拓扑与 `ranges` 地址翻译表构建；
   - 在 SMP 核间调度与 VMM 门禁收口之前完成 ECAM MMIO 2 MiB `Device-nGnRnE` 预映射；
   - 接入统一 PCI 核心（`kernel/bus/pci/`）与 `device-boot` 协调器，实现 Type 0 终端、多功能设备与 Type 1 PCI-to-PCI 桥递归扫描；
   - 开放 ECAM 4 KiB 扩展配置空间读/写契约（严格保留 x86_64 CF8/CFC `offset <= 252` 约束与既有宿主测试行为）；
   - 非破坏沿用固件/EDK2 已分配的 BAR，通过 `ranges` 执行 PCI 总线地址到 CPU 物理地址翻译，同时正确忽略未编程 MMIO BAR 并兼容 `< 4 GiB` 64 位 BAR；
   - 发布 `/dev/pci/<dddd:bb:ss.f>/*` 命名空间，使用户态 `lspci`（`-n` / `-v` / `-x`）可在 AArch64 产品镜像下稳定验证。
2. **树莓派 5（`brcm,bcm2712-pcie` + RP1）架构预留与一期优雅降级**：
   - 在 Host Bridge 抽象、4 KiB 扩展配置空间、多域（`domain` + `bus`）与 `simple-bus` / `ranges` 地址翻译契约上为 RPi5 预留清晰演进插槽；
   - 一期不实现 Broadcom BCM2712 RC 硬件初始化、Outbound ATU 配置、BCM2712 间接配置访问或 RP1 接管；
   - RPi5 DTB 无 `pci-host-ecam-generic` 节点时，`arch_pci_backend()` 返回 `NULL`，`pci_enumerate()` 优雅降级返回 `BUS_UNAVAILABLE`（`-ENODEV`），`device-boot` 将其识别为非致命降级，保持 RPi5 启动链（PL011/RP1 UART、M0/M1 双窗口、GIC、时钟、根挂载）零破坏。

### 1.2 明确排除出一期的范围（仅作为后续演进接口预留）
以下特性不纳入一期代码实现，仅在本方案第 10 节与相关接口中明确演进契约：
- **INTx `interrupt-map` GIC 路由**：一期 AArch64 `route_gsi()` 统一返回 `-ENOTSUP`（使 `/dev/pci/<BDF>/irq` 稳定输出 `none`）；二期根据 DTB `interrupt-map` / `interrupt-map-mask` 映射至 GICv2 SPI（或 RP1 子中断控制器）。
- **GICv2m / ITS MSI 与 MSI-X 启用**：一期继续沿用 `kernel/bus/pci/platform.c:11-23` 弱符号 `arch_pci_msix_map()` 返回 `-ENOTSUP`，待 GIC ITS / v2m 控制器或 BCM2712 MIP（`msi-controller@130000`）接入后再覆盖实现。
- **内核态 BAR 空间动态重分配**：一期信任 UEFI/EDK2 固件分配的 BAR 与 Type 1 桥存储器窗口；内核不重写未分配的 BAR，也不改写桥基址/界限寄存器。
- **DMA / IOMMU 映射与 `dma-ranges` 翻译**：一期不包含 PCI DMA 地址翻译、IOMMU 页表绑定或 `dma-coherent` 属性消费。
- **具体 PCIe 设备驱动**：一期不在 `.pci_drivers` 注册 `virtio-pci`、`xHCI` 或 `NVMe` 驱动；验证闭环以总线枚举、`/dev/pci` 发布及 `lspci` 展示为主（配合 QEMU `virtio-rng-pci` 与 Host Bridge 设备观测）。

---

## 2. 平台现状、DTB 实证与架构对比

### 2.1 仓库现有 PCI 与 AArch64 引导基线（源码核实）
通过核查仓库当前实现，影响本次接入的关键基线事实如下：
1. **x86_64 与 PCI 核心接口**：
   - `kernel/include/arch/pci.h:10-26` 定义 `struct pci_root { uint16_t domain; uint8_t bus; }`、`enum pci_error_scope { PCI_ERROR_FUNCTION = 0, PCI_ERROR_ROOT }` 与 `struct pci_backend`（静态 `roots`、`root_count`、`read32`、`write32`、`route_gsi`）。
   - `kernel/bus/pci/core.c:575,588` 在通用入口 `pci_config_read32()` / `pci_config_write32()` 硬性拒绝 `offset > 252`；`kernel/arch/x86_64/bus/pci.c:49,76` 亦在 CF8/CFC 实现内部拒绝 `offset > 252`（返回 `-EINVAL`，`*scope = PCI_ERROR_FUNCTION`）和 `domain != 0`（返回 `-ENODEV`，`*scope = PCI_ERROR_ROOT`）。
   - `kernel/bus/pci/core.c:223-288` 对 `hdr_layout == 0`（Type 0）执行**非破坏只读** BAR0..BAR5 解析（不写入 `0xFFFFFFFF` 探测长度）：目前仅当 `bar_val == 0` 记为 `PCI_BAR_NONE`，I/O、MMIO32、MMIO64 均解码原值，BAR5 为 64 位或保留类型时置 `bad_bar = true`（`DEV_FAILED` / `-EINVAL` / `enum_error = true`）。
   - `kernel/bus/pci/core.c:289-319` 支持 `hdr_layout == 1` Type 1 PCI-to-PCI 桥递归与 `bus_is_visited()` 循环防护。
   - `kernel/bus/pci/pci_devfs.c:30` 固定暴露 256 字节配置空间文件（`PCI_DEVFS_CFG_LEN = 256`），`fmt_irq` 在 `pci_route_gsi() != 0` 时稳定输出 `none\n`。
2. **AArch64 启动与 VMM / UEFI Memory Map 契约**：
   - `kernel/core/main.c` 启动序列为：Stage 3 `arch_boot_controllers(bootctx)` → Stage 4 `arch_boot_percpu()` → Stage 5 `arch_boot_devices(bootctx)` → Stage 6 `fs_boot_prepare()` 及 `#if OS01_HAS_PCI` 下的 `pci_devfs_register_all()`。
   - `kernel/arch/aarch64/boot/main.c:674-702` 在 Stage 3 依次调用 `dtb_init(bootctx)`（676 行）、`pl011_init()`（677 行）、`aarch64_virtio_mmio_prepare(bootctx)`（679 行），然后通过 `msr daifclr, #4`（685 行）解锁 SError，调用 `gic_init()`（696 行）、`arch_register_subsys()`（699 行）与 `subsys_init_phase(SUBSYS_PHASE_CONTROLLERS)`（701 行）。
   - Stage 4 `arch_boot_percpu()` 调用 `smp_starting_enter()`；此后 `kernel/arch/aarch64/memory/vmm_gate.c` 中的 `vmm_gate_check()` 在 `ipi_ready_count < cpu_count` 期间直接触发 `vmm_gate_violation` 宕机。因此**任何 `aarch64_pt_ensure_device_2m()` 调用必须在 Stage 3 完成，严禁推迟到 Stage 5 `pci_enumerate()`**。
   - `kernel/include/arch/aarch64/uefi_memory_map.h:97-99` 提供 `int aarch64_uefi_range_overlaps_ram(const void *map, uint32_t entry_size, uint32_t count, uint64_t start, uint64_t end)`（5 参签名）；`kernel/arch/aarch64/driver/virtio_mmio.c:54-77` 给出了访问 `ctx->memory.entries` 前完整的 M0 指针、乘法/加法防溢出与 Boot Block B 互斥校验范式；`AARCH64_PT_PA_LIMIT` (`UINT64_C(0x10000000000)`) 当前仅私有定义于 `kernel/arch/aarch64/memory/page_table.c:51`，尚未在 `kernel/include/arch/aarch64/page_table.h` 中导出。
3. **日志、跨架构编译与构建接线缺口**：
   - 内核公共日志头文件位于 `<log/log.h>`（`kernel/include/log/log.h`，仓库不存在 `<core/log.h>`）；AArch64 内核链接 `kernel/arch/aarch64/runtime/log_impl.c`（34–104 行 `vlog()`），**仅支持 `%s` / `%u` / `%d` / `%lu` / `%lx` / `%lX` / `%p`**，不支持 `%02x` / `%04x` / `%llx` / `%llu` 等带宽度或 `ll` 修饰符。
   - `kernel/include/arch/features.h:54,68` 对 AArch64 定义 `OS01_HAS_NET = 0` 与 `OS01_HAS_PCI = 0`；`qemutests/test_boot_facade_headers.py:112` 存在静态断言 `OS01_HAS_PCI == 0`。
   - `kernel/device/boot.c:8-9,101` 无条件包含 `<net/net.h>`、`<net/device.h>` 并调用 `net_device_init()`，尚未门控 `#if OS01_HAS_NET`。
   - `kernel/Makefile`（128–241 行 AArch64 源码清单与 312 行 `ARCH_SOURCE_DIRS`）未编入 `device/core.c`、`device/boot.c`、`bus/pci/*.c` 及 `bus` 目录。
   - `kernel/arch/aarch64/linker.ld` 缺少 `.pci_drivers` 段（`__pci_drivers_start` / `__pci_drivers_end`）。
   - `mk/profiles/aarch64-clang.mk:8` 能力为 `PROFILE_CAPABILITIES := kernel uefi userland`（无 `rootfs`），而 `mk/components/run.mk:777,1768-1776` 将 `test-host`、`test-header-object-audit`、`test-driver-model-boundary` 门控在 `rootfs` 能力上。

### 2.2 三平台实测硬件拓扑与 DTB 实证对比
通过实测 `qemu-system-aarch64 -M virt,gic-version=2,dumpdtb` 导出的 QEMU `virt` DTB 与 `/home/aagu/Downloads/RPi5_UEFI_Release_v0.3/bcm2712-rpi-5-b.dtb`（使用 `dtc` 反汇编校验），三类平台关键差异如下：

| 维度 | x86_64（PC/Q35 基线） | AArch64 QEMU `virt`（一期目标） | AArch64 树莓派 5（二期目标） |
|---|---|---|---|
| 拓扑发现源 | 固定约定 Root Bus 0 | DTB `/pcie@10000000` | DTB `/axi/pcie@120000` 等（`pcie0`/`pcie1` 为 `disabled`） |
| `compatible` | 无（CF8/CFC I/O 端口） | `"pci-host-ecam-generic"` | `"brcm,bcm2712-pcie"` |
| 父总线与地址映射 | 无（直连 I/O `0xCF8`/`0xCFC`） | 直属根结点 `/`，`#address-cells=<2>, #size-cells=<2>` | 挂于 `/axi`（`simple-bus`，`ranges` 映射子域 `0x10..` / `0x1c..` 至 CPU） |
| 配置空间基址与大小 | I/O 端口（256 B） | `reg = <0x40 0x10000000 0x00 0x10000000>`（PA `0x4010000000`，256 MiB ECAM，4 KiB/函数） | `reg = <0x10 0x120000 0x00 0x9310>`（约 36.7 KiB RC 寄存器窗，间接配置索引） |
| 域与总线范围 | `domain = 0`，`bus = 0..255` | `linux,pci-domain = <0x00>`，`bus-range = <0x00 0xff>` | 多 RC 需分配多 `domain`（如 `pcie2` 下挂 `rp1` 芯片） |
| Outbound `ranges` | I/O port + MMIO 1:1 | I/O：`0x0 -> 0x3eff0000` (`64 KiB`)<br>MMIO32：`0x10000000 -> 0x10000000` (`0x2eff0000`)<br>MMIO64：`0x80_00000000 -> 0x80_00000000` (`512 GiB`) | MMIO32：`PCI 0x0 -> CPU 0x1f_00000000`（`0xfffffffc`）<br>Prefetch MMIO64：`PCI 0x4_00000000 -> CPU 0x1c_00000000`（`12 GiB`） |
| Inbound DMA 语义 | CPU PA ≈ 总线地址 | `dma-coherent` 无偏移 | 非恒等 `dma-ranges`（PCI `0x10_00000000` ↔ CPU `0x0`）与 IOMMU 偏移 |
| 40 位 PA 上限 (`1 TiB`) | N/A | ECAM `0x40_10000000..0x40_20000000` (`< 257 GiB`)；MMIO64 上界 `0x100_00000000` (`= 1 TiB`) | CPU 物理窗集中于 `0x10_00000000..0x1f_ffffffff`（`< 128 GiB`） |
| 中断路由属性 | Q35 PIRQ 约定 GSI 16..19 | `interrupt-map`（SPI 3..6）与 `msi-map`（`0x8003` ITS） | `interrupt-map` + `msi-controller`（MIP）+ `rp1` 次级中断控制器 |

关键观察：
1. QEMU `virt` 的 **I/O 空间本身就是非恒等映射**（PCI `0x0` → CPU `0x3eff0000`），且 MMIO64 起始于 `0x80_0000_0000`（`512 GiB`）。
2. 未编程的 64 位 / 可预取 MMIO BAR 读取低 32 位时保留属性位 `[3:1]`，形成 `0x4`/`0x8`/`0xC`（masked 基址为 `0`）；且部分兼容 64 位设备在固件下可能被赋予 `< 4 GiB` 的 `MMIO32` 窗内地址。
3. RPi5 的 **MMIO32 空间平移到了 4 GiB 之上**（PCI `0x0` → CPU `0x1f_0000_0000`），并且同一个 DT 中存在 `status = "disabled"` 的兄弟控制器（`pcie0`、`pcie1`）。

---

## 3. 总体架构与跨模块边界

### 3.1 分层结构图
```text
Stage 3 (arch_boot_controllers)           Stage 5 (arch_boot_devices)        Stage 6 / Userspace
+---------------------------------+      +---------------------------+      +----------------------+
| dtb.c: dtb_init()               |      | device/boot.c             |      | core/main.c          |
|  -> aarch64_dtb_parse_pcie()    |      |  -> #if OS01_HAS_NET      |      |  -> fs_boot_prepare()|
|  -> 填充 BSS: s_pcie_topology   |      |  -> pci_enumerate()       |      |  -> pci_devfs_       |
+----------------+----------------+      +-------------+-------------+      |     register_all()   |
                 v                                     v                    +----------+-----------+
+---------------------------------+      +---------------------------+                 |
| arch/aarch64/bus/pci.c          | <--- | bus/pci/core.c            |                 v
|  -> aarch64_pcie_prepare(ctx)   |      |  -> scan_bus() / Type0/1  |      +----------------------+
|     [vmm_gate=ok, M0+UEFI校验   |      |  -> translate_resource()  |      | /dev/pci/<BDF>/*     |
|      -> pt_ensure_device_2m]    |      |  -> pci_bar_window()      |      | user/lspci (-n/-v/-x)|
|  -> arch_pci_backend()          |      |  -> read32/write32 (4 KiB)|      +----------------------+
+---------------------------------+      +---------------------------+
```

### 3.2 模块边界与静态审计约束
1. **`qemutests/driver_model_boundary_audit.py`（90–98 行）**：严禁在 `kernel/bus/` 与 `kernel/include/bus/` 出现 `outb`/`outl`/`inb`/`inl`/`wrmsr`/`rdmsr`/`apic_write` 或物理高半区魔数（`0xffff800000000000`）。因此一切 `ARCH_PAGE_OFFSET` 加法、AArch64 `ldr`/`str`、`dsb` 内存屏障、以及 DT/ECAM 寄存器编码**严格收敛于 `kernel/arch/aarch64/bus/pci.c` 与 `kernel/arch/aarch64/platform/dtb_parse_pcie.c`**；通用层 `kernel/bus/pci/core.c` 仅通过 `struct pci_backend` 函数指针交互。
2. **`qemutests/header_object_audit.py`**：`kernel/include/` 下的所有头文件必须可独立以 freestanding C 编译且导出符号数（`DBCRT`）为 0。因此新头文件 `kernel/include/arch/aarch64/pcie.h` 与扩展后的 `kernel/include/arch/pci.h`、`kernel/include/arch/aarch64/page_table.h` 仅允许标准基础头文件、`typedef`/`struct`/`enum`、`#define` 常量与外部函数声明，禁止定义全局非 `static` 数据对象或强符号函数。
3. **`hosttests/mk/fs.mk` 链接闭合性（594–635 行）**：`test_pci_devfs.elf` 编译 `kernel/bus/pci/core.c` 与 `kernel/bus/pci/pci_devfs.c`，但**不链接 `kernel/bus/pci/platform.c`**，而是由测试自供 `arch_pci_backend()`。故 `core.c` 对新能力（4 KiB 扩展配置空间、资源翻译、总线范围约束）**只能通过 `struct pci_backend` 新增成员读取**，不得新增 `platform.c` 中的全局函数强依赖。

---

## 4. Host Bridge 与多域 Backend 抽象

### 4.1 `kernel/include/arch/pci.h` 零破坏兼容扩展
保持原有 `struct pci_root`（`domain`, `bus`）与 `enum pci_error_scope`（`PCI_ERROR_FUNCTION = 0`, `PCI_ERROR_ROOT`）定义不变，增加 PCI 地址空间常数并在 `struct pci_backend` 末尾追加 3 个零默认兼容字段：

```c
#define PCI_SPACE_IO      1U
#define PCI_SPACE_MMIO32  2U
#define PCI_SPACE_MMIO64  3U

struct pci_backend {
    const struct pci_root *roots;
    unsigned int root_count;
    int (*read32)(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                  uint16_t offset, uint32_t *out, enum pci_error_scope *scope);
    int (*write32)(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                   uint16_t offset, uint32_t value, enum pci_error_scope *scope);
    int (*route_gsi)(const struct pci_device *pdev, uint32_t *out);

    /* Phase-1 扩展字段：未显式初始化（0 / NULL）时完全保留既有 x86_64 行为 */
    uint16_t max_config_offset;
    int (*translate_resource)(uint16_t domain, uint8_t space,
                              uint64_t pci_addr, uint64_t size,
                              uint64_t *out_cpu_addr);
    int (*bus_range)(uint16_t domain, uint8_t *out_bus_start, uint8_t *out_bus_end);
};
```

兼容性与语义约定：
- **既有测试与 x86_64 零侵入**：`kernel/arch/x86_64/bus/pci.c:124` 的 `s_x86_pci_backend` 以及 `hosttests/cases/test_pci_driver_model.c`、`test_pci_devfs.c`、`test_device_boot.c`、`test_pci_backend_irq.c` 均采用 C11 指定初始化器，新增成员默认置 `0`/`NULL`，行为保持字节级一致。
- **4 KiB 扩展配置空间边界与 x86_64 保留契约**：
  - `kernel/bus/pci/core.c` 中新增内部辅助规则：
    ```c
    const uint16_t limit =
        (backend->max_config_offset > 252U && backend->max_config_offset <= 4092U)
        ? backend->max_config_offset : 252U;
    ```
  - `pci_config_read32()`（`core.c:575`）与 `pci_config_write32()`（`core.c:588`）保持严格的三段校验顺序：先校验 `!pdev || !out || (offset & 3U) != 0`（写路径无 `!out`）返回 `-EINVAL`；再获取 `backend = arch_pci_backend()`，若 `!backend || !backend->read32`（或 `write32`）返回 `BUS_UNAVAILABLE`（`-ENODEV`）；最后按上述 `limit` 检查 `if (offset > limit) return -EINVAL;`。
  - x86_64 后端 `max_config_offset == 0` → `limit == 252`，且 `x86_pci_read32`/`x86_pci_write32` 内部继续强制 `offset > 252` 返回 `-EINVAL` 与 `*scope = PCI_ERROR_FUNCTION`，双重守卫既有 CF8/CFC 契约；
  - AArch64 ECAM 后端设置 `.max_config_offset = 4092`，允许 4 字节对齐的 `0x000..0xFFC` 读写，覆盖 PCIe Extended Capabilities（`0x100..0xFFF`）；
  - 当 `domain` 未知或后端尚未就绪时，`read32`/`write32` 设置 `*scope = PCI_ERROR_ROOT` 并返回 `-ENODEV`；当 `slot >= 32`、`fn >= 8`、`bus` 超出域的 `[bus_start, bus_end]`、或 `offset > 4092` / 未对齐时，设置 `*scope = PCI_ERROR_FUNCTION` 并返回 `-EINVAL`。
- **`translate_resource` 语义**：`NULL` 表示 PCI 总线地址与 CPU 物理地址 1:1 恒等；非 `NULL` 时，按 `(domain, space, pci_addr, size)` 在该域的 Host Bridge `ranges` 窗口表查找连续覆盖区间。当 `size == 0`（用于非破坏 BAR 枚举，因为配置阶段不探测 BAR 长度）时执行单点命中 `[r->pci_base, r->pci_base + r->size)` 并输出 `cpu_base + (pci_addr - pci_base)`；当 `size > 0` 时额外检查 `pci_addr + size` 无溢出且不超越窗口末尾。
- **`bus_range` 语义**：多域扩展与非零 `bus-range` 下限/上限隔离。若 `NULL` 则默认允许 `[root.bus, 255]`。

### 4.2 多域 Root 管理边界
- 沿用 `kernel/bus/pci/core.c:19` 的 `MAX_DOMAINS = 16`；`kernel/include/arch/aarch64/pcie.h` 定义一期静态上限 `AARCH64_PCIE_MAX_ROOTS = 4`，`struct pci_root s_aarch64_roots[AARCH64_PCIE_MAX_ROOTS]` 在初始化后只读导出，每个根记录独立 `domain` 与起始 `bus`（`bus_start`）。
- 尽管一期 QEMU `virt` 仅暴露 1 个 `domain = 0` 根桥，底层查找均按键 `domain`（而非假定 `domain == 0` 单例）索引，确保 RPi5 后续接入 1~3 个独立 RC 时直接以 `domain = 0, 1, 2` 共存。

---

## 5. DTB `pci-host-ecam-generic` 解析与栈帧安全

### 5.1 栈帧危机规避（`dtb_init <= 768 B` 与 `-Wframe-larger-than=512`）
`qemutests/stack_frame_audit.py:30` 对 `"dtb_init"` 设有严格的 **768 字节** 栈帧上限（当前实现已占 688 B，仅余 80 B），且 `kernel/arch/aarch64/make.config:18` 启用了 `-Wframe-larger-than=512`。同时，`kernel/arch/aarch64/platform/dtb.c:84` 在 `dtb_init()` 栈上分配整个 `struct aarch64_platform_info parsed;`。

因此架构上确立两条硬规矩：
1. **严禁**向 `struct aarch64_platform_info` 添加 PCIe 控制器数组或 `ranges` 表。
2. PCIe 拓扑描述符置于**静态 `.bss` 对象** `static struct aarch64_pcie_topology s_pcie_topology;`（定义于 `kernel/arch/aarch64/platform/dtb.c`），由独立解析函数填充：
   ```c
   int aarch64_dtb_parse_pcie(const void *blob, uint32_t size,
                              struct aarch64_pcie_topology *out);
   ```
   该函数签名为 `(blob, size, out)`，与 `kernel/include/arch/aarch64/dtb.h:37-40` 中的 `aarch64_dtb_parse()` / `aarch64_dtb_parse_for_console()` 保持相同的边界检查语义。`dtb_init()` 栈上仅多出指针传参及 `int` 状态变量，栈帧增量趋近于 0，且不影响 `qemutests/aarch64_dtb_test.py` 全量 41 个既有测例。解析器内部嵌套总线栈采用轻量定长数组（最大深度 `AARCH64_PCIE_FDT_MAX_DEPTH = 8`，单帧仅保存 `address_cells`、`size_cells` 与可选 `ranges` 偏移/长度共 ~24 B/层，总栈帧 `< 320 B`），远低于 512 B 警告线。

### 5.2 `<arch/aarch64/pcie.h>` 数据结构与生命周期接口
```c
#ifndef OS01_AARCH64_PCIE_H
#define OS01_AARCH64_PCIE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct boot_context;

#define AARCH64_PCIE_MAX_ROOTS   4U
#define AARCH64_PCIE_MAX_RANGES  8U

enum aarch64_pcie_ctrl_kind {
    AARCH64_PCIE_CTRL_NONE = 0,
    AARCH64_PCIE_CTRL_ECAM_GENERIC = 1,
    AARCH64_PCIE_CTRL_BCM2712 = 2, /* 二期 RPi5 预留枚举项 */
};

struct aarch64_pcie_range {
    uint8_t space;        /* PCI_SPACE_IO / PCI_SPACE_MMIO32 / PCI_SPACE_MMIO64 */
    bool prefetchable;
    uint64_t pci_base;
    uint64_t cpu_base;
    uint64_t size;
};

struct aarch64_pcie_root_desc {
    bool valid;
    enum aarch64_pcie_ctrl_kind kind;
    uint16_t domain;
    uint8_t bus_start;
    uint8_t bus_end;
    uint64_t reg_cpu_base;
    uint64_t reg_size;
    uint8_t range_count;
    struct aarch64_pcie_range ranges[AARCH64_PCIE_MAX_RANGES];
};

struct aarch64_pcie_topology {
    uint32_t root_count;
    struct aarch64_pcie_root_desc roots[AARCH64_PCIE_MAX_ROOTS];
};

int aarch64_dtb_parse_pcie(const void *blob, uint32_t size,
                           struct aarch64_pcie_topology *out);
const struct aarch64_pcie_topology *aarch64_pcie_topology_get(void);
int aarch64_pcie_prepare(const struct boot_context *ctx);

#endif /* OS01_AARCH64_PCIE_H */
```
注意：`aarch64_pcie_prepare()` 与 `<arch/aarch64/virtio_mmio.h:10>` 中的 `aarch64_virtio_mmio_prepare(const struct boot_context *ctx)` 保持一致，前置声明 `struct boot_context;` 并显式包含基础标准头，即可使 `<arch/aarch64/pcie.h>` 在不引入重型 MMU 头文件的前提下通过 `header_object_audit.py` 与 `aarch64-none-elf` 独立编译检验。

### 5.3 FDT 遍历、字符串安全、祖先翻译与错误处理区分
`aarch64_dtb_parse_pcie(blob, size, out)` 首先清零 `*out`，然后校验 FDT 魔数 `0xd00dfeed`、`totalsize <= size`、structure/strings block 边界与 4 字节对齐。遍历遵循以下契约：
1. **全局结构性错误 vs 单节点局部错误语义**：
   - **FDT 容器级结构错误**（`!blob || !out || size < 40`、魔数错误、`totalsize > size`、token 流越界、栈深溢出等）：立即清零 `*out` 并返回 `-1`（`rc < 0`）；`dtb.c` 收到 `rc < 0` 时清空 `s_pcie_topology` 并记录 `log_warn`（不调用 `dtb_fatal`，以免破坏非 PCI 启动流程）。
   - **单个控制器的属性局部错误**（某个节点的 `reg` / `bus-range` / `ranges` / `linux,pci-domain` 非法或窗口数超限）：仅记录 `log_warn`（使用 `vlog()` 安全格式符）并**跳过该坏节点**，继续解析 DTB 内其余节点；只要 DTB 结构完好，函数最终返回 `0`，`s_pcie_topology` 保留其他有效根桥（若无有效根桥则 `root_count == 0`）。
2. **字符串属性 NUL 边界与 `status` 过滤**：
   - 参照 `kernel/arch/aarch64/platform/dtb_parse.c:78-106`（`strings_valid()`），对所有 `status` 与 `compatible` 字符串属性先校验 `prop.len > 0 && blob[prop.off + prop.len - 1] == '\0'`（无末尾 `\0` 则拒绝解析，防越界读）；
   - 若节点存在 `status` 且不等于 `"okay"` 或 `"ok"`（如 RPi5 `pcie0`/`pcie1` `"disabled"`），整棵跳过该控制器节点。
3. **精确 `compatible` token 匹配**：
   - 按 NUL 分隔逐 token 比对，一期仅接收精确等于 `"pci-host-ecam-generic"` 的节点。像 RPi5 `"brcm,bcm2712-pcie"` 等非 ECAM 控制器被干净跳过（不告警、不报错，保持 `root_count == 0`）。
4. **`#address-cells` / `#size-cells` 约定与父子区分**：
   - 祖先后代总线（root `/`、`simple-bus` 等）解析沿用 IEEE 1275 / `dtb_parse.c` 惯例：若父节点未显式给出 `#address-cells`/`#size-cells`，默认分别为 `2` 与 `1`，仅接受 `1` 或 `2`；
   - PCI Host Bridge 节点自身声明子属性 `#address-cells` 与 `#size-cells` 用于 `ranges` 的 child-address 列：当桥节点存在 `ranges` 时，**强制要求**本节点显式声明 `#address-cells = <3>` 与 `#size-cells = <2>`，缺失或不匹配则记录 `log_warn` 并跳过该节点；
   - 而桥节点自己的 `reg` 属性与 `ranges` 中的 parent-address 列**使用其直接父节点的 `#address-cells` 与 `#size-cells`**。解析器不可对 3-cell 子地址直接调用 `aarch64_fdt_translate_reg_step()`（因 `kernel/include/arch/aarch64/fdt_address.h` 限定 `child_cells` 为 `1` 或 `2`），而是先解析出 `parent_base` / `reg_base`，再沿祖先 `simple-bus` 迭代 `ranges` 完成至 CPU PA 的翻译。
5. **`reg` 校验**：检查存在至少 1 条合法 `(base, size)`、祖先翻译至 CPU PA `reg_cpu_base` 无溢出，要求 `reg_size >= 0x100000ULL` 且 `(reg_cpu_base & 0xFFFULL) == 0`。
6. **`bus-range` 校验**：若存在则必须为 8 字节大端 `(bus_start, bus_end)` 且 `bus_start <= bus_end <= 255`；若缺省则默认 `bus_start = 0` 并按 `ecam_buses = min(256, reg_size >> 20)` 推算 `bus_end = bus_start + ecam_buses - 1`。强制校验 `((uint64_t)(bus_end - bus_start + 1U) << 20) <= reg_size` 且 `reg_cpu_base <= UINT64_MAX - active_size`，否则跳过该节点并记 `log_warn`。
7. **`linux,pci-domain` 校验**：若存在且为 4 字节，需满足 `val <= 0xFFFFU`；缺省时按当前未冲突的最小域号顺序分配（优先 `0`）；若与已收录根桥出现同 `domain` 且 `[bus_start, bus_end]` 重叠，跳过冲突节点并记 `log_warn`。
8. **3-Cell `ranges` 解码**：元组字节步长为 `(3 + parent_addr_cells + 2) * 4`，要求 `ranges.len` 为其整数倍；提取 `phys.hi = cell[0]`（bits `[25:24]`：`0` 配置空间忽略、`1` I/O、`2` MMIO32、`3` MMIO64；bit `30` `prefetchable`）、`pci_base`、沿祖先 `ranges` 平移 `cpu_base` 与 `size`（跳过 `size == 0` 项，校验 64 位无溢出）；超过 `AARCH64_PCIE_MAX_RANGES` 记 `log_warn` 并跳过该节点。

---

## 6. ECAM MMIO 预映射、Stage-3 / VMM 门禁与异常边界

### 6.1 启动时序与 VMM 门禁耦合分析
```text
Stage 2: arch_boot_memory()   -> pmm_init(), 构建并验证 M1 运行时页表 (TTBR1)
                                 此时尚未启用 SMP；vmm_gate 允许建立新设备映射
Stage 3: arch_boot_controllers(bootctx)
   ├─ dtb_init(bootctx)       -> 解析平台设备 + s_pcie_topology (静态 BSS)
   ├─ pl011_init()            -> 早串口就绪 (log_warn / log_info 可输出)
   ├─ aarch64_virtio_mmio_prepare(bootctx)
   ├─ aarch64_pcie_prepare(bootctx)   <=== 必须插在此处（daifclr/SMP 前）！
   ├─ msr daifclr, #4         -> 解除 SError 屏蔽
   └─ gic_init() -> arch_register_subsys() -> SUBSYS_PHASE_CONTROLLERS
Stage 4: arch_boot_percpu()   -> smp_starting_enter() 【VMM Gate 关闸点】
                                 此后 vmm_gate_check() 在 ipi_ready < cpus 时必挂
Stage 5: arch_boot_devices()  -> SUBSYS_PHASE_STORAGE -> device-boot
                                 -> pci_enumerate() 只读已映射的 ECAM VA
Stage 6: kernel_main()        -> fs_boot_prepare() -> pci_devfs_register_all()
```

将 `aarch64_pcie_prepare(bootctx)` 置于 `kernel/arch/aarch64/boot/main.c:679`（`aarch64_virtio_mmio_prepare` 之后）至 `685` 行（`msr daifclr, #4` 之前）具有 4 重必然性：
1. **规避 `vmm_gate_violation`**：Stage 4 执行 `smp_starting_enter()` 后，Stage 8 从核就绪前调用 `aarch64_pt_ensure_device_2m()` 均会触发 `vmm_gate_check()` 致命异常。
2. **PMM 已可用**：Stage 2 已完成 PMM 与 M1 运行时表初始化。QEMU `virt` 256 MiB ECAM 仅跨 1 个 1 GiB 范围（`L0[0] -> L1[256]`），只需按需分配 1 张 L2 页表并写入 128 个连续 2 MiB Block 描述符。
3. **UEFI Raw Memory Map 仍位于安全 M0 低地址**：此时 `bootctx->memory` 仍可通过 M0 恒等映射校验边界后直接访问。
4. **单核独占写页表无需跨核 TLB 广播**：映射完成后执行 `dsb sy; isb`，后续从核直接从统一 `TTBR1_EL1` 继承映射。

### 6.2 `aarch64_pcie_prepare(ctx)` 实现与头文件导出规范
- **头文件常量导出**：将 `AARCH64_PT_PA_LIMIT` (`UINT64_C(0x10000000000)`，即 1 TiB / 40-bit IPS 上限) 从 `kernel/arch/aarch64/memory/page_table.c:51` 导出至公共头文件 `kernel/include/arch/aarch64/page_table.h`（`page_table.c` 保留 `#ifndef AARCH64_PT_PA_LIMIT` 或统一复用头文件定义）；`PAGE_2M_SIZE` 与 `PAGE_2M_MASK` 来自 `<memory/pmm.h>`（`kernel/include/memory/pmm.h:13,16`）。
- **空拓扑优雅返回**：当 `s_pcie_topology.root_count == 0`（如 RPi5 或无 ECAM DTB）时，`aarch64_pcie_prepare(ctx)` 将 `s_ready_root_count` 置 `0` 并直接返回 `0`，完全不触碰页表或 `ctx->memory`。
- **UEFI Raw Memory Map 指针与 M0 边界完整校验**：参照 `kernel/arch/aarch64/driver/virtio_mmio.c:54-77`，在解引用 `ctx->memory.entries` 前必须全部通过以下 7 项检查（任一项不满足时记录 `log_warn`，标记 0 root 就绪并返回 `0`）：
  1. `boot_context_valid(ctx)`（定义于 `<core/bootinfo.h>` 第 152 行）；
  2. `(ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) != 0u && ctx->memory.format == BOOT_MEMORY_FORMAT_UEFI_RAW`；
  3. `ctx->memory.entries != 0u && ctx->memory.entry_size >= AARCH64_UEFI_MEM_DESC_MIN_SIZE && ctx->memory.entry_count != 0u`；
  4. 乘法防溢出：`(uint64_t)desc_count <= UINT64_MAX / stride`，计算 `map_bytes = (uint64_t)desc_count * stride`；
  5. 加法防溢出：`ctx->memory.entries <= UINT64_MAX - map_bytes`，得 `uefi_map_end = ctx->memory.entries + map_bytes`；
  6. 处于 M0 映射下：`uefi_map_end <= (uint64_t)AARCH64_BOOT_MAP_END`（来自 `<arch/aarch64/handoff_layout.h>`）；
  7. 不与 Boot Block B 重叠：`!(ctx->memory.entries < (uint64_t)AARCH64_BOOT_BLOCK_END && uefi_map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)`（来自 `<arch/aarch64/boot_layout.h>`）。
  通过后转换为 `const void *mmap_ptr = (const void *)(uintptr_t)ctx->memory.entries;`。
- **逐根桥安全校验与 2 MiB 设备映射**：
  1. 计算活跃区 `active_size = (uint64_t)(root->bus_end - root->bus_start + 1U) << 20`，校验 `root->reg_cpu_base <= UINT64_MAX - active_size` 与对齐上限后计算 2 MiB 包络：`map_start = root->reg_cpu_base & (uint64_t)PAGE_2M_MASK`，`last_blk = (root->reg_cpu_base + active_size - 1ULL) & (uint64_t)PAGE_2M_MASK`，`map_end = last_blk + (uint64_t)PAGE_2M_SIZE`（先校验 `last_blk <= UINT64_MAX - (uint64_t)PAGE_2M_SIZE`）。
  2. 物理上限校验：`map_start < map_end && map_end <= AARCH64_PT_PA_LIMIT`。
  3. Boot Block B 互斥：`!(map_start < (uint64_t)AARCH64_BOOT_BLOCK_END && map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)`。
  4. **逐 2 MiB 块 UEFI RAM 检查与页表写入**：按 `uint64_t blk_pa = map_start; blk_pa < map_end; blk_pa += (uint64_t)PAGE_2M_SIZE`：
     - 调用真实 5 参签名 `aarch64_uefi_range_overlaps_ram(mmap_ptr, stride, desc_count, blk_pa, blk_pa + (uint64_t)PAGE_2M_SIZE)`（`kernel/include/arch/aarch64/uefi_memory_map.h:97-99`），仅返回 `0` 时放行（`!= 0` 说明重叠 RAM 或内存图描述符损坏，立即拒绝该 root）；
     - 调用 `aarch64_pt_ensure_device_2m(ttbr1_root, ARCH_PAGE_OFFSET + blk_pa, blk_pa)`（其中 `ttbr1_root = (uint64_t *)(uintptr_t)(ARCH_PAGE_OFFSET + (aarch64_read_ttbr1() & AARCH64_TTBR_BASE_MASK))`）。
  5. **多块映射中途失败回退策略**：同一 2 MiB 块内若存在异属性既有页（如 RAM/Boot 映入导致 `AARCH64_PT_ECONFLICT = -5`）或中间页表分配失败（`-ENOMEM`），应立即停用该 `root` 并记 `log_warn`（不登记进 `s_ready_roots[]`），继续处理后续其他 `root`。此前已在本轮写入的前序 `blk_pa` 块因均已先行通过 Boot Block B 与 UEFI RAM 无重叠检验，保留为只读隔离后的空置 `Device-nGnRnE` 项而无需额外拆表（无 RAM 碰撞风险，且任何上层不会再向禁用 root 发起 ECAM 访问）。全根处理完成后统一通过宿主可编译原语 `aarch64_pcie_dsb_sy_isb()` 执行 `dsb sy; isb`。

### 6.3 ECAM 32 位访问序列、宿主编译隔离与异步异常边界
1. **ECAM 线性偏移公式**：对于有效域内已就绪根桥的 `(bus, slot, fn, offset)`（满足 `bus_start <= bus <= bus_end`、`slot < 32`、`fn < 8`、`(offset & 3U) == 0`、`offset <= 4092U`）：
   $$\text{ecam\_off} = ((\text{uint64\_t})(\text{bus} - \text{bus\_start}) \ll 20) \mid ((\text{uint64\_t})\text{slot} \ll 15) \mid ((\text{uint64\_t})\text{fn} \ll 12) \mid \text{offset}$$
   访问虚拟地址 $\text{va} = \text{ARCH\_PAGE\_OFFSET} + \text{reg\_cpu\_base} + \text{ecam\_off}$。
2. **`Device-nGnRnE` 严格 32 位 MMIO 与宿主跨编译隔离原语**：
   - `aarch64_pt_ensure_device_2m()` 采用 `AARCH64_PT_DEVICE`（`AttrIndx=0`, `Device-nGnRnE`、`PXN|UXN`、`SH_NS`）；
   - 对照 `kernel/arch/aarch64/memory/page_table.c:23-29`，在 `kernel/arch/aarch64/bus/pci.c` 中将所有 AArch64 内联汇编统一收敛到受 `#if defined(__aarch64__) && !defined(AARCH64_PCIE_HOST_TEST)` 保护的静态内联原语中：
     - 目标机分支：32 位 `ldr %w0, [%1]\n\tdsb ld` 读、32 位 `str %w0, [%1]\n\tdsb st` 写（确保写入先于紧随状态读到达硬件），以及 `aarch64_pcie_prepare()` 末尾的 `dsb sy; isb`；
     - 宿主测试 `#else` 分支：仅允许经测试注入基址指针执行普通 32 位内存加载/存储与编译器屏障 `__asm__ volatile("" ::: "memory")`，确保 `kernel/arch/aarch64/bus/pci.c` 可在 x86_64 宿主机直接编译测试。
3. **空槽位 Unsupported Request（UR）与 External Abort / SError 边界**：
   - 按 PCIe Base Spec 与 QEMU `virt` ECAM 契约，访问不存在设备的合法 ECAM 地址由主桥返回 UR 应答 `0xFFFFFFFF`，不产生同步 Data Abort 或 SError。`core.c` 读偏移 `0x00` 遇到 `0xFFFF` Vendor ID 即跳过；
   - `main.c:685` 解除了 SError 屏蔽；二期若支持对 UR 上报同步外部中止（`EC=0x25, DFSC=0x10`）或 SError 的硬件 RC，应在 `ecam_ldr32` 外围补充内核异常修复表（`extable`）捕获外部中止。

---

## 7. BAR 解析、`ranges` 地址翻译与 Type 1 桥递归

### 7.1 `struct pci_bar` 扩展、未编程 MMIO BAR 与 `< 4 GiB` 64 位 BAR 语义
在 `kernel/include/bus/pci/pci.h:39-44` 保持现有字段不变并在末尾增加两个零默认字段：

```c
struct pci_bar {
    enum pci_bar_kind kind;
    uint64_t address;      /* 对外界（pci_bar_window/devfs）暴露的 CPU 物理地址 */
    bool valid;
    uint8_t index;
    uint64_t bus_address;  /* 原始 PCI 总线域 BAR 基址 */
    bool translated;       /* true 表示由 backend->translate_resource 完成翻译 */
};
```

配合 `kernel/bus/pci/core.c:223-288` 修改遵循以下精确判定顺序：
1. **沿用固件 BAR 与先行结构合法性检查**：
   - 仅对 `hdr_layout == 0`（Type 0）扫描 BAR0..BAR5（Type 1 桥跳过 BAR2..5 避免将 `0x18..0x24` 窗口寄存器误当 BAR）。绝不向未编程 BAR 写 `0xFFFFFFFF` 或重分配基址；
   - **保留先行的非法编码拒绝**（守护 `hosttests/cases/test_pci_driver_model.c:575` 等既有用例）：对于 MMIO 类型 `mem_type == 0x01` 或 `0x03`（保留类型），以及 `mem_type == 0x02 && bar_idx >= 5`（BAR5 声明 64 位无高槽位），仍立即置 `bad_bar = true`（`DEV_FAILED` / `-EINVAL` / `enum_error = true`）。
2. **掩码基址提取与未编程 BAR 识别（关键修正）**：
   - I/O BAR：`pci_addr = bar_val & ~0x03ULL`；
   - MMIO32 BAR：`pci_addr = bar_val & ~0x0FULL`；
   - MMIO64 BAR：读取 `BAR[n+1]` `upper_val`，拼接 `pci_addr = ((uint64_t)upper_val << 32) | (bar_val & ~0x0FULL)`。
   - **未编程 MMIO / I/O BAR 语义**：PCIe 未编程的 64 位或可预取 MMIO BAR 常保留只读属性位 `[3:1]`，读回 `bar_val = 0x4` / `0x8` / `0xC`（`upper_val = 0` → `pci_addr == 0`）。当掩码地址 `pci_addr == 0` 时：
     - 若 `backend->translate_resource == NULL`，或尝试 `translate_resource()` 未命中任何窗口：均**视作未编程 BAR**（设置 `kind = PCI_BAR_NONE`、`valid = false`、`address = 0`，对于 `MMIO64` 同步保持次槽为 `PCI_BAR_NONE` 并步进 `bar_idx++`），**严禁设置 `bad_bar = true`**；
     - 仅当某个平台显式在 DT `ranges` 中配置了始于 `PCI 0x0` 的合法映射窗（且翻译成功）时，才按有效 BAR 收录（为 RPi5 `PCI 0x0 -> CPU 0x1f_0000_0000` 保留兼容性）。
3. **`ranges` 地址翻译与 `< 4 GiB` MMIO64 回退匹配**：
   - 若 `backend->translate_resource != NULL`：
     - 按 `PCI_BAR_IO -> PCI_SPACE_IO`、`PCI_BAR_MMIO32 -> PCI_SPACE_MMIO32`、`PCI_BAR_MMIO64 -> PCI_SPACE_MMIO64` 调用 `backend->translate_resource(domain, space, pci_addr, 0, &cpu_addr)`；
     - **64 位 BAR `< 4 GiB` 窗回退规则**：当 `kind == PCI_BAR_MMIO64` 且 `pci_addr < 0x100000000ULL` 时，若 `PCI_SPACE_MMIO64` 查表失败（如 QEMU `virt` 的 `MMIO64` 窗始于 `0x80_0000_0000`，而固件将支持 64 位 BAR 的设备放进了 `< 4 GiB` 的 `MMIO32` 窗 `0x10000000..0x3eff0000`），允许回退调用 `backend->translate_resource(domain, PCI_SPACE_MMIO32, pci_addr, 0, &cpu_addr)`；
     - 命中时写 `bar->bus_address = pci_addr`、`bar->address = cpu_addr`、`bar->translated = true`，`MMIO64` 的 `BAR[n+1]`（`PCI_BAR_UPPER`）同步记录 `address = cpu_addr`；
     - 若 **`pci_addr != 0` 且上述主查与回退均失败**，使用 `vlog()` 安全格式符记 `log_warn`（不含 `%llx`/`%04x`），置 `PCI_BAR_NONE` / `valid = false` 并置 `bad_bar = true`（`enum_error = true`）。
4. **`pci_bar_window()` 对非恒等 32 位 MMIO 翻译的修正（`core.c:563-566`）**：
   - 使用 `const uint64_t check_base = bar->translated ? bar->bus_address : bar->address;` 校验 `PCI_BAR_MMIO32` 的 32 位上界 `<= 0x100000000ULL`，避免非恒等 `ranges` 将 32 位 PCI BAR 映射至 `>= 4 GiB` CPU 物理地址时产生 `-EOVERFLOW` 误报，同时返回 translated CPU PA `bar->address + offset` 至 `*physical`。

### 7.2 Type 1 PCI-to-PCI 桥递归增强
- 在保留 `core.c:289-319` 已有 `secondary == 0 || secondary == bus || secondary > subordinate` 与 `bus_is_visited(domain, b)` 保护基础上：
- 若 `backend->bus_range != NULL`，在递归前获取 `[domain_bus_start, domain_bus_end]`；若 `secondary < domain_bus_start || subordinate > domain_bus_end` 或 `secondary <= bus`，记 `log_warn` 并置 `pdev->enum_error = true` 隔离坏桥分支。

---

## 8. 内核接线、链接脚本与构建门控

### 8.1 `kernel/device/boot.c` 跨架构 `#if OS01_HAS_NET` 门控
- 在 `kernel/device/boot.c` 顶部引入 `#include <arch/features.h>`，用 `#if OS01_HAS_NET` 包裹 `<net/net.h>`、`<net/device.h>` 与第 101 行 `net_device_init();` 调用，消除 AArch64（`OS01_HAS_NET == 0`）下的网络未定义引用；x86_64 与 `test_device_boot` 测试（`OS01_HAS_NET == 1`）零变化。

### 8.2 `kernel/arch/aarch64/linker.ld` `.pci_drivers` 段定义
仿照 `.subsys_init`（`kernel/arch/aarch64/linker.ld:141-146`）置于 `:data` 段，并对齐 `kernel/arch/x86_64/linker.ld:45` 在输入段前插入 `. = ALIGN(8);` 保障指针数组 8 字节边界：

```ld
    .pci_drivers : AT(ADDR(.pci_drivers) - _kernel_page_offset)
    {
        . = ALIGN(8);
        PROVIDE(__pci_drivers_start = .);
        KEEP(*(.pci_drivers))
        PROVIDE(__pci_drivers_end = .);
    } :data
```

该定义同时满足 AArch64 高半区 `VMA - LMA == _kernel_page_offset` 契约及 `pmm.c` / UEFI ELF 加载要求。

### 8.3 `kernel/Makefile`、`features.h` 与测试门控联动
1. `kernel/include/arch/features.h:68`：在 `#elif defined(__aarch64__)` 下将 `#define OS01_HAS_PCI 0` 修改为 `#define OS01_HAS_PCI 1`。
2. `qemutests/test_boot_facade_headers.py:112`：同步更新为 `_Static_assert(OS01_HAS_PCI == 1, "aarch64 must have OS01_HAS_PCI == 1");`。
3. `kernel/Makefile`（严格区分通用清单与架构自动通配目录，对照 `kernel/Makefile:345-346, 428, 437, 476, 1418`）：
   - AArch64 `KERNEL_C_SOURCES`（128–241 行）**仅添加架构无关源文件**：`device/core.c`、`device/boot.c`、`bus/pci/core.c`、`bus/pci/match.c`、`bus/pci/platform.c`、`bus/pci/pci_devfs.c`；
   - **严禁**将 `arch/aarch64/bus/pci.c` 与 `arch/aarch64/platform/dtb_parse_pcie.c` 加入 `KERNEL_C_SOURCES`（由于 `ARCH_C_SOURCES` 自动按 `ARCH_SOURCE_DIRS` 通配展开，且 `ALL_C_SOURCES := $(KERNEL_C_SOURCES) $(ARCH_C_SOURCES) $(ALL_C_SOURCES_EXTRA)` 不做 `$(sort ...)` 去重，重复列出将导致同一 `.o` 两次传入 `ld.lld` 报重复符号错误）；
   - `ARCH_SOURCE_DIRS`（312 行原文为 `boot intr memory smp cpu platform runtime driver sched sync syscall`）在保留已有 `platform` 的基础上追加 `bus`（变为 `boot intr memory smp cpu platform runtime driver sched sync syscall bus`），自动收录 `arch/aarch64/platform/dtb_parse_pcie.c` 与 `arch/aarch64/bus/pci.c`。
4. **`KERNEL_SELFTEST=1` 兼容性**：`KERNEL_SELFTEST=1` 下 `OS01_HAS_PCI=1` 且 `OS01_HAS_VFS=1`；无 ECAM 节点时仅通过 `log_info` 降级且不输出 `FATAL`/`PANIC`/`FAIL`/`ERROR`，保证既有 SMP/selftest 日志契约。

---

## 9. `devfs` 发布、`lspci` 验证面与日志策略

### 9.1 `/dev/pci/<dddd:bb:ss.f>/*` 行为
Stage 6 `kernel/core/main.c` 在 `#if OS01_HAS_PCI` 下调用 `pci_devfs_register_all()`：
- QEMU `virt` 枚举后发布 `/dev/pci/0000:00:00.0/*`（QEMU Host bridge）及外设（如挂载 `-device virtio-rng-pci,rng=rng0` 时的 `0000:00:01.0`）；
- `config` 保留 `PCI_DEVFS_CFG_LEN = 256`（`kernel/bus/pci/pci_devfs.c:30`），既保持 `test_pci_devfs` 兼容又避免占用栈帧；
- `irq` 因 `route_gsi()` 返回 `-ENOTSUP` 而稳定输出 `none\n`；
- 无 ECAM 设备时返回 `0`，不打印误报错误。

### 9.2 用户态 `user/lspci.c` 审计与补充
`mk/components/image.mk:295` 已包含 `user/lspci.c`：
1. **QEMU PCIe 设备名称表补充（核实基线）**：`user/lspci.c:128` 已有 `{ 0x1af4, 0x1005, "Virtio RNG device" }`，第 140 行已有厂商 `{ 0x1b36, "Red Hat, Inc. (QEMU Virtual Machine)" }`；因此实际缺失并需补充的设备条目为：
   - `{ 0x1b36, 0x0008, "QEMU PCIe Host bridge" }`
   - `{ 0x1b36, 0x000c, "QEMU PCIe Root Port" }`
   - `{ 0x1af4, 0x1044, "Virtio modern RNG device" }`
2. **多域 BDF 与 `-v` `irq=none` 审计**：`user/lspci.c:289` 使用 `strchr(bdf, ':')` 无条件剥离域号；改为仅当域前缀为 `"0000:"` 时简写为 `bb:ss.f`（兼容既有输出），非 `"0000:"`（面向 RPi5 多域）时保留完整 `dddd:bb:ss.f`；并核验 `lspci -v` 对 `irq == "none"` 的健壮展示。

### 9.3 内核日志头文件与格式符硬性规范
- 所有需要记录日志的内核源码（包括新增或修改的 `kernel/bus/pci/core.c`、`kernel/arch/aarch64/platform/dtb_parse_pcie.c`、`kernel/arch/aarch64/bus/pci.c`）一律包含 **`<log/log.h>`**（`kernel/include/log/log.h`），严禁引用不存在的 `<core/log.h>`，亦禁止 `serial_printk` / `color_printk`。
- 因 AArch64 内核链接 `kernel/arch/aarch64/runtime/log_impl.c:34-104`（`vlog()`），上述共用及 AArch64 新增日志**仅允许使用 `%s` / `%u` / `%d` / `%lu` / `%lx` / `%lX` / `%p`**（64 位或 BDF 数值强转 `(unsigned long)` 配 `%lx` / `%lu`），**严禁出现 `%02x` / `%04x` / `%llx` / `%llu` 等带宽度或 `ll` 格式符**（防止 `vlog()` 将未知格式原样打印至串口）。
- **Hosttest mock 同步**：为避免 `core.c` 引入 `<log/log.h>` 触发宏重定义及链接缺失，`hosttests/mock/arch9/pci_runtime.h` 与 `backend_runtime.h` 同步参照 `boot_runtime.h:19-21` 添加 `#ifndef _KERNEL_LOG_H #define _KERNEL_LOG_H #endif` 保护。

---

## 10. 树莓派 5（`brcm,bcm2712-pcie` + RP1）二期演进路径

基于对真实 `bcm2712-rpi-5-b.dtb` 的核验，RPi5 `/axi` 下挂 `pcie0@100000` (`disabled`)、`pcie1@110000` (`disabled`) 及 `pcie2@120000` (`okay`, `brcm,bcm2712-pcie`, 直连 `rp1`)：

| 架构切面 | 一期设计落位 | 二期接入 `brcm,bcm2712-pcie` + RP1 的最小扩展 |
|---|---|---|
| **1. 控制器分发** | `enum aarch64_pcie_ctrl_kind` 预留 `AARCH64_PCIE_CTRL_BCM2712` | Stage 3 为 ~37 KiB `reg` 映射 1 个 2 MiB `Device-nGnRnE` 块；在 `arch/aarch64/bus/pci.c` 分派 `EXT_CFG_INDEX`/`EXT_CFG_DATA` 间接配置访问 |
| **2. 多域独立扫描** | `struct pci_root` + `MAX_DOMAINS` + `backend->bus_range` | 独立 RC 分配不同 `domain`（`0/1/2`），各持独立 `[bus_start, bus_end]` |
| **3. 4 KiB 扩展配置空间** | `max_config_offset = 4092` | 通过 BCM2712 间接扩展索引读取 RP1 PCIe 扩展能力 |
| **4. 祖先总线与非恒等 `ranges`** | `simple-bus` 祖先翻译 + `pci_bar_window` 用 `bus_address` 检 4 GiB + 未编程 `0` 与 `< 4 GiB` MMIO64 回退 | 直接承接 RPi5 `/axi` 平移及 `PCI 0x0 -> CPU 0x1f_00000000`（`MMIO32` `> 4 GiB`）、`0x4_00000000 -> CPU 0x1c_00000000`（`MMIO64`）；增量编程 Outbound ATU |
| **5. DMA 偏移与中断路由** | `route_gsi()` 接收 `pdev` 含 `domain`，默认输出 `none` | 解析 `dma-ranges`（PCI `0x10_00000000` ↔ CPU `0x0`）；对接 `interrupt-map`、`mip@130000` MSI 与 `rp1` 次级中断控制器 |

**一期零破坏保障矩阵**：无 `pci-host-ecam-generic` 时，`aarch64_dtb_parse_pcie` 得 `root_count == 0` → `aarch64_pcie_prepare()` 空操作返回 `0` → `arch_pci_backend()` 返回 `NULL` → `device-boot` 非致命跳过 `BUS_UNAVAILABLE` → `/dev/pci` 无设备，RPi5 串口与根挂载启动链零干扰。

---

## 11. 分层测试矩阵与验收标准

> 注：遵守 `mk/profiles/aarch64-clang.mk:8`（`PROFILE_CAPABILITIES := kernel uefi userland`，无 `rootfs`）与 `mk/project.mk:617` 能力门控边界，一切要求 `rootfs` 能力的宿主回归及跨目录静态审计均在 `PROFILE=x86_64-clang` 下执行；`aarch64-clang` 聚焦内核/镜像/栈帧审计及 QEMU UEFI E2E 套件。

| 测试层级 | 目标文件 / 执行命令 | 核心验证断言与验收门禁 |
|---|---|---|
| **Host Unit — 扩展配置空间与 BAR/桥** | `hosttests/cases/test_pci_extended_config.c`（`@os01-test`, 场景 `drivers.pci.extended_config`）+ `hosttests/mk/drivers.mk` (`LINK_CFLAGS_pci-extended-config`, `LINK_GROUP_pci-extended-config` 含 `device_core_production.o` 与 `$(LIBC_OBJS)`)<br>命令：`make PROFILE=x86_64-clang test-arch9-host CASE=pci-extended`、`make PROFILE=x86_64-clang test-arch9-host CASE=pci`、`backend`、`device-boot` | 1. 遗留后端 (`max_config_offset=0`) 对 `256..4092` 返 `-EINVAL`；ECAM (`4092`) 支持 `0x100..0xFFC` 读写；<br>2. **未编程 MMIO BAR（`0x4`/`0x8`/`0xC`，`upper=0`）** 记为 `PCI_BAR_NONE` 而**不触发 `bad_bar`**（BAR5 64 位仍非法拒绝）；<br>3. **`PCI_BAR_MMIO64` 且 `pci_addr < 4 GiB` 时正确回退匹配 `PCI_SPACE_MMIO32`**；<br>4. 非恒等 `MMIO32 -> > 4 GiB CPU PA` 通过 `pci_bar_window()` 检查；Type 1 桥越界 `bus_range` 被隔离 |
| **Host Unit — DTB 解析与静态容量** | `hosttests/cases/test_aarch64_dtb_pcie.c`（`@os01-test`, 场景 `memory.aarch64.dtb_pcie`）+ `hosttests/mk/memory.mk` (`LINK_CFLAGS_aarch64-dtb-pcie`, `LINK_GROUP_aarch64-dtb-pcie`)<br>命令：`make PROFILE=x86_64-clang test-aarch64-pcie-host`、`python3 qemutests/aarch64_dtb_test.py` | 1. 解析 QEMU `virt` `pcie@10000000`；<br>2. RPi5 `brcm,bcm2712-pcie` 与 `status="disabled"` 被跳过（`root_count == 0`, `rc == 0`）；<br>3. `simple-bus` 父总线翻译正确；无 NUL 字符串越界防御有效；<br>4. **单节点属性错误跳过该节点（保留同 DTB 其他有效根，返回 `0`），FDT 结构损坏返回 `-1` 清空**；<br>5. 既有 41 个 `aarch64_dtb_test` 全过 |
| **Host Unit — ECAM MMIO 与 Stage 3** | `hosttests/cases/test_aarch64_pcie_ecam.c`（`@os01-test`, 场景 `memory.aarch64.pcie_ecam`）+ `hosttests/mk/memory.mk` (`LINK_CFLAGS_aarch64-pcie-ecam`, `LINK_GROUP_aarch64-pcie-ecam`)<br>命令：`make PROFILE=x86_64-clang test-aarch64-pcie-host`、`make PROFILE=x86_64-clang test-host` | 1. 验证 7 项 UEFI Raw Memory Map M0 指针/溢出/Boot Block B 边界；<br>2. 校验 5 参 `aarch64_uefi_range_overlaps_ram()` 重叠 RAM、`>= AARCH64_PT_PA_LIMIT` (`1 TiB`) 安全拒绝，且部分块失败停用该 root 而不拆先序干净设备块；<br>3. 0 ECAM root 下 `aarch64_pcie_prepare()` 返回 `0` 且 `arch_pci_backend() == NULL`；<br>4. 验证 ECAM 偏移计算、`route_gsi == -ENOTSUP` 及 3 类空间 `translate_resource()` |
| **静态审计、门面与目录契约** | `make PROFILE=x86_64-clang check-test-catalog`<br>`python3 qemutests/scenario_catalog.py --check`<br>`make PROFILE=x86_64-clang test-header-object-audit test-driver-model-boundary`<br>`make PROFILE=aarch64-clang test-stack-frame-audit`<br>`clang --target=aarch64-none-elf -ffreestanding -fsyntax-only -Ikernel/include kernel/include/arch/aarch64/pcie.h`<br>`python3 -m unittest qemutests.test_boot_facade_headers qemutests.test_aarch64_pcie` | 1. 无目录漂移、三段场景目录与 `@os01-test` 完全匹配（不修改 `config/test-bridge.json`）；<br>2. `dtb_init <= 768 B` 且零 `-Wframe-larger-than=512` 报错；<br>3. 所有头文件 `DBCRT == 0` 且 `<arch/aarch64/pcie.h>` 通过 `aarch64-none-elf` 语法检查；<br>4. `kernel/bus/` 零 MMIO / 端口指令泄漏；<br>5. `test_boot_facade_headers` 对 `OS01_HAS_PCI == 1` 通过 |
| **双架构 / 变体编译验证** | `make PROFILE=x86_64-clang kernel.bin`<br>`make PROFILE=aarch64-clang aarch64-uefi`<br>`make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 aarch64-uefi`<br>`make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi` | x86_64、`aarch64-clang` `qemu-virt` 与 `rpi5` 双平台产品镜像（`OS01_HAS_PCI=1, OS01_HAS_NET=0`；后者不依赖 `QEMU_EFI.fd`）与 `KERNEL_SELFTEST=1` 全量零警告编过；`llvm-nm` 确认 `__pci_drivers_start`/`__pci_drivers_end` 位于 `:data` |
| **QEMU `virt` E2E 与 RPi5 负向降级** | `qemutests/aarch64_pcie_test.py`（独立 E2E runner，由 `mk/components/run.mk` 新增 `.PHONY: test-aarch64-pcie` 并纳入第 122 行 `AARCH64_PLATFORM=qemu-virt` 解析期门禁，内建 `-device virtio-rng-pci,rng=rng0` + `-dtb`）+ `qemutests/test_aarch64_pcie.py`（`layer: harness`）+ `tools/tests/test_aarch64_uefi_images.py` + `config/test-scenarios.json` (`drivers.pci.ecam_virt_lspci`, `drivers.pci.no_ecam_graceful_fallback`)<br>命令：`make PROFILE=aarch64-clang test-aarch64-pcie`、`make PROFILE=aarch64-clang test-aarch64 MODE=smp`、`python3 tools/tests/test_aarch64_uefi_images.py --build-root build/aarch64-clang`（前置已构建双平台镜像）、`make PROFILE=x86_64-clang test-framework-contract` | 1. **正向**：通过 `subsys: init  device-boot ... ok` 进 shell，`ls /dev/pci` 含 `0000:00:00.0` 与 `0000:00:01.0`，`lspci` / `-n` / `-v`（有效 BAR + `irq=none`） / `-x` 全通过；<br>2. **负向与双镜像门禁**：仅含 `brcm,bcm2712-pcie` / 无 ECAM 的诊断 DTB 优雅跳过（无 `FATAL`/`PANIC`）并进 shell，双平台镜像 ESP 内容一致且 `AARCH64_PLATFORM=rpi5 test-aarch64-pcie` 在解析期拒绝；<br>3. **真机标注**：若现场无物理树莓派 5 开发板实测，交付验收记录必须显式标注 **“RPi5 真机板测：待测”** |
