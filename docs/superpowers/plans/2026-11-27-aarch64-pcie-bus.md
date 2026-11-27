# AArch64 PCIe 总线一期实施计划

**Goal:** 在 OS01 AArch64（QEMU `virt` 通用 ECAM 控制器 `compatible = "pci-host-ecam-generic"`）上落地完整的 PCIe 枚举、4 KiB 扩展配置空间读写、`ranges` 地址翻译、`/dev/pci/<BDF>/*` 暴露与用户态 `lspci` 验证闭环；同时在 Host Bridge 抽象、DTB 解析与多域模型中为树莓派 5（`brcm,bcm2712-pcie` + RP1）预留清晰演进插槽，保证无 ECAM 节点时优雅降级返回 `BUS_UNAVAILABLE`（`-ENODEV`），零破坏 RPi5 现有启动链。

**Architecture:** 遵循 `qemutests/driver_model_boundary_audit.py` 边界约束，所有 DTB 解析、`ARCH_PAGE_OFFSET` 与 32 位 `Device-nGnRnE` MMIO 访问均封装在 `kernel/arch/aarch64/platform/dtb_parse_pcie.c` 与 `kernel/arch/aarch64/bus/pci.c` 中；通用 PCI 核心（`kernel/bus/pci/core.c`）仅通过扩展后的 `struct pci_backend`（`max_config_offset`、`translate_resource`、`bus_range`）协作，兼容 x86_64 与所有既有宿主测试（包括不链接 `platform.c` 的 `test_pci_devfs`）。DTB PCIe 拓扑存放于静态 `.bss`（不增大 `struct aarch64_platform_info` 以保住 `dtb_init` 768 B 栈帧死限）；ECAM 2 MiB 设备映射（使用 `<arch/aarch64/page_table.h>` 导出的 `AARCH64_PT_PA_LIMIT` 与 `<memory/pmm.h>` 的 `PAGE_2M_SIZE`/`PAGE_2M_MASK`，按 7 项 M0 边界检查及 5 参 `aarch64_uefi_range_overlaps_ram()` 验证）在 Stage 3 `arch_boot_controllers()` 中、`aarch64_virtio_mmio_prepare()` 之后与 `daifclr #4` / Stage 4 `smp_starting_enter()` 之前完成准备。

**Tech Stack:** C11（Freestanding `x86_64-clang` / `aarch64-clang`）、LD 链接脚本、GNU Make、FDT 解析、ARMv8 2 MiB Block 页表与 UEFI Raw Memory Map、`hosttests`（`@os01-test` 与 `LINK_CFLAGS_*`/`LINK_GROUP_*` 框架）、`qemutests` Python 测试与静态审计套件。

**Spec:** [AArch64 PCIe 总线一期设计方案](../specs/2026-11-27-aarch64-pcie-bus-design.md)

**Global Constraints:**
- 严格保留 x86_64 CF8/CFC `offset > 252` 拒绝语义及所有既有宿主测试契约（`test_pci_driver_model`、`test_pci_backend_irq`、`test_pci_devfs`、`test_device_boot`）。
- 严禁扩展 `struct aarch64_platform_info`；必须确保 `qemutests/stack_frame_audit.py` 中 `dtb_init <= 768 B` 与 `-Wframe-larger-than=512` 持续通过。
- 日志头文件统一使用 **`<log/log.h>`**（不存在 `<core/log.h>`）；共用代码（`kernel/bus/pci/core.c`）与 AArch64 新增日志仅允许使用 `kernel/arch/aarch64/runtime/log_impl.c:34-104`（`vlog()`）支持的 **`%s` / `%u` / `%d` / `%lu` / `%lx` / `%lX` / `%p`**，**严禁使用 `%02x` / `%04x` / `%llx` / `%llu` 等格式符**，且预期降级路径不得输出 `FATAL`/`PANIC`/`FAIL`/`ERROR` 关键字，禁止 `serial_printk` / `color_printk`。
- 所有新头文件必须通过 `make PROFILE=x86_64-clang test-header-object-audit`（`DBCRT == 0`）与 `aarch64-none-elf` freestanding 语法检查；不得破坏 `test-driver-model-boundary`；所有含 `rootfs` 能力依赖的宿主测试与审计均在 `PROFILE=x86_64-clang` 下执行。
- 新增 `@os01-test` 必须同步登记 `config/test-scenarios.json` 与 `LINK_CFLAGS_*`/`LINK_GROUP_*`（不得向只减不增的 `config/test-bridge.json` 新增条目），并运行 `make PROFILE=x86_64-clang regen-test-catalog check-test-catalog` 及 `python3 qemutests/scenario_catalog.py --check`。
- 一期范围冻结：不实现 INTx `interrupt-map` GIC 路由（`route_gsi` 返回 `-ENOTSUP`，`/dev/pci/<BDF>/irq` 稳定输出 `none`）、GICv2m/ITS MSI-X、BAR 重分配、DMA/IOMMU 映射或具体 PCIe 设备驱动。

**Review Focus:**
- `struct pci_backend` 与 `struct pci_bar` 的零默认兼容性，重点核验未编程 MMIO BAR（`0x4`/`0x8`/`0xC`）不置 `bad_bar`、`< 4 GiB` `PCI_BAR_MMIO64` 回退匹配 `PCI_SPACE_MMIO32`、`test_pci_devfs.elf`（不链接 `platform.c`）链接闭合性，以及 `pci_runtime.h`/`backend_runtime.h` 对 `_KERNEL_LOG_H` 的守卫；
- Stage 3 `aarch64_pcie_prepare()` 在 `main.c` 中是否严格先于 `daifclr #4` 与 `smp_starting_enter()`，完整覆盖 7 项 M0 指针/溢出/Boot Block B 检查、5 参 `aarch64_uefi_range_overlaps_ram()` 与导出 `AARCH64_PT_PA_LIMIT`；
- DTB 结构损坏（`rc < 0` 清空 `s_pcie_topology`）与单节点局部错误（记 `log_warn` 跳过坏节点、保留其他根并返回 `0`）的严格区分；
- `kernel/device/boot.c` `#if OS01_HAS_NET` 与 `linker.ld` `.pci_drivers`（含 `. = ALIGN(8);` 与 `:data` `LMA = VMA - _kernel_page_offset`）在双架构及 `KERNEL_SELFTEST=1` 下全过；
- `mk/components/run.mk` 中 `test-aarch64-pcie` 与 `qemutests/test_aarch64_pcie.py` 接线闭合、所有验证命令在目标 Profile 下可真实执行，且无实体板卡时 RPi5 真机明确标注“待测”。

---

### Task 1: 扩展 PCI Core 后端契约、4 KiB 配置空间与 BAR / `ranges` 翻译

**Files:**
- Modify: `kernel/include/arch/pci.h`
- Modify: `kernel/include/bus/pci/pci.h`
- Modify: `kernel/bus/pci/core.c`
- Modify: `hosttests/mock/arch9/pci_runtime.h`
- Modify: `hosttests/mock/arch9/backend_runtime.h`
- Create: `hosttests/cases/test_pci_extended_config.c`
- Modify: `hosttests/mk/drivers.mk`
- Modify: `mk/components/run.mk`
- Modify: `config/test-scenarios.json`
- Regenerate: `mk/generated/*` (`make PROFILE=x86_64-clang regen-test-catalog`)

**Interfaces:**
- `kernel/include/arch/pci.h`：
  - 新增 `#define PCI_SPACE_IO 1U`、`#define PCI_SPACE_MMIO32 2U`、`#define PCI_SPACE_MMIO64 3U`（保持仅包含 `<stdint.h>` 和 `<stdbool.h>`）；
  - 保留既有 `struct pci_root` 与 `enum pci_error_scope { PCI_ERROR_FUNCTION = 0, PCI_ERROR_ROOT }`；
  - 在 `struct pci_backend` 末尾追加零默认字段：
    - `uint16_t max_config_offset;`
    - `int (*translate_resource)(uint16_t domain, uint8_t space, uint64_t pci_addr, uint64_t size, uint64_t *out_cpu_addr);`
    - `int (*bus_range)(uint16_t domain, uint8_t *out_bus_start, uint8_t *out_bus_end);`
- `kernel/include/bus/pci/pci.h`：
  - 在 `struct pci_bar` 末尾追加 `uint64_t bus_address; bool translated;`。
- `kernel/bus/pci/core.c` 不变量：
  - 包含 **`<log/log.h>`**，所有新日志仅用 `vlog()` 支持的 `%s` / `%u` / `%d` / `%lu` / `%lx` / `%p`；
  - `pci_config_read32` / `pci_config_write32` 严格三步校验：`!pdev || !out || (offset & 3U) != 0` → `-EINVAL`；`!backend || !backend->read32/write32` → `BUS_UNAVAILABLE`；`limit = (backend->max_config_offset > 252U && backend->max_config_offset <= 4092U) ? backend->max_config_offset : 252U`，`offset > limit` → `-EINVAL`（保留 `x86_pci_read32`/`write32` 自身拒绝 `offset > 252`）；
  - Type 0 BAR0..BAR5（先检查保留 `mem_type == 0x01/0x03` 与 BAR5 64 位非法 `bar_idx >= 5 -> bad_bar = true`）：
    - 掩码提取 `pci_addr`（I/O `& ~0x03ULL`、MMIO `& ~0x0FULL`、`MMIO64` 拼 `upper_val`）；
    - 当 **`pci_addr == 0`** 且（`backend->translate_resource == NULL` 或翻译失败）时，**视作未编程 BAR（含 `0x4`/`0x8`/`0xC`），置 `PCI_BAR_NONE` / `valid = false`，绝不置 `bad_bar = true`**；
    - 当 `backend->translate_resource != NULL` 且为 `PCI_BAR_MMIO64`、`pci_addr < 0x100000000ULL` 时，主查 `PCI_SPACE_MMIO64` 失败允许回退匹配 `PCI_SPACE_MMIO32`（仍保持 `kind = PCI_BAR_MMIO64` 及 `PCI_BAR_UPPER`）；
    - 仅当 `pci_addr != 0` 且主查与合法回退均失败时，记 `log_warn`（`vlog()` 安全格式）、恢复 `PCI_BAR_NONE`/`valid = false` 并置 `bad_bar = true`；
  - `pci_bar_window()` 对 `PCI_BAR_MMIO32` 用 `const uint64_t check_base = bar->translated ? bar->bus_address : bar->address;` 检查 4 GiB 上界，返回 `*physical = bar->address + offset`；
  - Type 1 桥递归调用 `backend->bus_range`（若非 `NULL`）校验 `[secondary, subordinate]` 不越界且 `secondary > bus`。
- `hosttests/mock/arch9/{pci_runtime.h, backend_runtime.h}`：添加 `#ifndef _KERNEL_LOG_H #define _KERNEL_LOG_H #endif` 保护（同 `boot_runtime.h:19-21`），避免 `core.c` 引入 `<log/log.h>` 触发宏重定义或未定义日志符号。
- `hosttests/mk/drivers.mk` 与 `mk/components/run.mk`：
  - `drivers.mk` 显式定义 **`LINK_CFLAGS_pci-extended-config = $(PCI_HOST_CFLAGS)`** 与 **`LINK_GROUP_pci-extended-config := $(TEST_BLD)/device_core_production.o $(TEST_BLD)/pci_core_production.o $(TEST_BLD)/pci_match_production.o $(TEST_BLD)/pci_platform_production.o $(LIBC_OBJS)`**（因为 `core.c:106` 调用 `device_alloc_id()` 与 `snprintf`/`strcmp`，缺一不可）及 `.PHONY: test-pci-extended-config`；
  - `mk/components/run.mk:940` `test-arch9-host` 的 `cases` 与 `mapping` 追加 `pci-extended:test-pci-extended-config`。

- [ ] **Step 1: 编写红灯宿主测试与规则接线**
  - 创建 `hosttests/cases/test_pci_extended_config.c`（`@os01-test`：`id: test_pci_extended_config`、`layer: host`、`subsystem: drivers`、`link: pci-extended-config`、`scenarios: [drivers.pci.extended_config]`），覆盖：
    1. 遗留后端（`max_config_offset = 0`, `translate_resource = NULL`）访问 `256`、`4092` 返 `-EINVAL`；
    2. ECAM 后端（`max_config_offset = 4092`）读写 `0x100`、`0xFFC` 成功，`4096` / 未对齐返回 `-EINVAL`；
    3. **未编程 MMIO BAR 豁免**：`bar_val = 0x4` / `0x8` / `0xC`（`upper = 0`）在 QEMU `virt` 模拟 `ranges` 下被识别为 `PCI_BAR_NONE`，`enum_error == false` 且设备状态正常；同时 BAR5 设 `0xF0000004` / `0x00000004` 仍触发 `bad_bar`；
    4. **`< 4 GiB` MMIO64 BAR 回退匹配**：64 位 BAR 配 `pci_addr = 0x10004000`、`ranges` 仅有 `MMIO32` (`0x10000000..0x3eff0000`) 与 `MMIO64` (`0x80_00000000..`) 时，回退匹配 `PCI_SPACE_MMIO32` 并保留 `PCI_BAR_MMIO64` 与 `PCI_BAR_UPPER`；
    5. 非恒等 `translate_resource`（I/O `0x1000 -> 0x3eff1000` 及 MMIO32 `0x0 -> 0x1f_00000000`）经 `pci_bar_window()` 返回翻译后 CPU PA；非零越界 BAR 置 `bad_bar = true`；Type 1 桥越界 `bus_range` 触发隔离。
  - 在 `hosttests/mk/drivers.mk` 加入 `LINK_CFLAGS_pci-extended-config`、`LINK_GROUP_pci-extended-config` 与 `test-pci-extended-config`，在 `mk/components/run.mk:940` 注册 `pci-extended`，在 `config/test-scenarios.json` 注册 `drivers.pci.extended_config`，运行 `make PROFILE=x86_64-clang regen-test-catalog`。
- [ ] **Step 2: 确认红灯失败**
  - 运行：`make PROFILE=x86_64-clang check-test-catalog && make PROFILE=x86_64-clang test-arch9-host CASE=pci-extended`
  - 预期：因字段缺失与 `core.c` 逻辑未改而失败。
- [ ] **Step 3: 实施头文件、Mock 保护与 `core.c` 改动**
  - 按 `Interfaces` 修改 `kernel/include/arch/pci.h`、`kernel/include/bus/pci/pci.h`、`kernel/bus/pci/core.c`（包含 `<log/log.h>` 且仅用 `%s`/`%u`/`%d`/`%lu`/`%lx`）以及 `hosttests/mock/arch9/{pci_runtime.h, backend_runtime.h}`。
- [ ] **Step 4: 转绿并执行既有宿主与静态回归**
  - 运行：
    `make PROFILE=x86_64-clang test-arch9-host CASE=pci && make PROFILE=x86_64-clang test-arch9-host CASE=backend && make PROFILE=x86_64-clang test-arch9-host CASE=device-boot && make PROFILE=x86_64-clang test-arch9-host CASE=pci-extended && make PROFILE=x86_64-clang test-host`
  - 运行静态审计（`x86_64-clang` 具备 `rootfs`）：
    `make PROFILE=x86_64-clang test-header-object-audit test-driver-model-boundary`
  - 预期：全部通过，确认 `test_pci_devfs.elf` 在 `test-host` 中无链接/日志宏冲突且 `DBCRT == 0`。

---

### Task 2: AArch64 PCIe DTB 拓扑定义与 `dtb_parse_pcie` 解析器

**Files:**
- Create: `kernel/include/arch/aarch64/pcie.h`
- Create: `kernel/arch/aarch64/platform/dtb_parse_pcie.c`
- Modify: `kernel/arch/aarch64/platform/dtb.c`
- Create: `hosttests/cases/test_aarch64_dtb_pcie.c`
- Modify: `hosttests/mk/memory.mk`
- Modify: `mk/components/run.mk`
- Modify: `config/test-scenarios.json`
- Regenerate: `mk/generated/*` (`make PROFILE=x86_64-clang regen-test-catalog`)

**Interfaces:**
- `kernel/include/arch/aarch64/pcie.h`：
  - 定义 `AARCH64_PCIE_MAX_ROOTS = 4U`、`AARCH64_PCIE_MAX_RANGES = 8U`、`enum aarch64_pcie_ctrl_kind`、`struct aarch64_pcie_range`、`struct aarch64_pcie_root_desc`、`struct aarch64_pcie_topology`；前置声明 `struct boot_context;`，声明 `aarch64_dtb_parse_pcie(const void *blob, uint32_t size, struct aarch64_pcie_topology *out)`、`aarch64_pcie_topology_get(void)`、`aarch64_pcie_prepare(const struct boot_context *ctx)`。
- `kernel/arch/aarch64/platform/dtb_parse_pcie.c`：
  - 包含 `<arch/aarch64/pcie.h>`、`<arch/aarch64/fdt_address.h>`、`<log/log.h>`（仅用 `%s`/`%u`/`%d`/`%lu`/`%lx`）；
  - 复用 `strings_valid()` 校验所有字符串属性长度非 0 且以 `'\0'` 结尾；父总线缺省 `#address-cells`/`#size-cells` 时按 IEEE 1275 默认取 `2`/`1`，匹配到的 ECAM 桥带 `ranges` 时强制校验 `#address-cells == 3` 且 `#size-cells == 2`；
  - **错误分级边界**：FDT 头/块结构损坏返回 `-1` 并清空 `*out`；单个控制器 `reg`/`bus-range`/`ranges`/`linux,pci-domain` 冲突或超 `AARCH64_PCIE_MAX_RANGES` 仅记 `log_warn` 并跳过该节点继续解析其余节点，函数最终返回 `0`；
  - 精确匹配 `"pci-host-ecam-generic"` 与 `status` 过滤（跳过 `"disabled"` 及 `"brcm,bcm2712-pcie"`）；严禁扩大 `struct aarch64_platform_info`。
- `kernel/arch/aarch64/platform/dtb.c`：
  - 静态 `.bss` 定义 `static struct aarch64_pcie_topology s_pcie_topology;` 与 `aarch64_pcie_topology_get()`；
  - `dtb_init()` 解析基础平台表后调用 `aarch64_dtb_parse_pcie(b, size, &s_pcie_topology)`，仅当 `rc < 0`（FDT 结构错误）时清空 `s_pcie_topology` 并记录提示，保留局部合法根桥，绝不触发 `dtb_fatal`。
- `hosttests/mk/memory.mk` 与 `mk/components/run.mk`：
  - `memory.mk` 新增 `$(TEST_BLD)/aarch64_dtb_pcie_production.o` 编译规则、`LINK_CFLAGS_aarch64-dtb-pcie = $(HOST_CFLAGS) $(KERNEL_INC) $(FRAMEWORK_INC) -DOS01_HOST_TEST` 与 **`LINK_GROUP_aarch64-dtb-pcie := $(TEST_BLD)/aarch64_dtb_pcie_production.o $(TEST_BLD)/serial_sink_stub.o $(LIBC_OBJS)`**（`test_aarch64_dtb_pcie.c` 定义 `int g_log_level = 4;` 及 `_log_info_impl` 桩，`serial_sink_stub.o` 满足弱符号 `_log_warn_impl`/`_log_err_impl`），并定义 `.PHONY: test-aarch64-dtb-pcie`；
  - `mk/components/run.mk` 新增 `host_case_group` `test-aarch64-pcie-host`（支持 `CASE=dtb-pcie` 与 Task 3 的 `CASE=ecam`）。

- [ ] **Step 1: 编写红灯宿主 DTB 解析单测并注册规则**
  - 新建 `hosttests/cases/test_aarch64_dtb_pcie.c`（`@os01-test`：`subsystem: memory`、`link: aarch64-dtb-pcie`、`scenarios: [memory.aarch64.dtb_pcie]`），覆盖：
    1. QEMU `virt` `pcie@10000000` 全属性解析（3 条 `ranges`、domain 0、bus 0..255）；
    2. RPi5 `brcm,bcm2712-pcie`（`disabled` 与 `okay`）及禁用 ECAM 均优雅跳过（`rc == 0, root_count == 0`）；
    3. 挂于 `simple-bus` 祖先下的有效 ECAM 正确平移 `reg` 与 `ranges`；
    4. **坏节点隔离 vs FDT 结构错误**：一个坏 `bus-range` 节点与一个合法节点共存时返回 `0` 且保留合法节点；无 `\0` 结尾字符串属性安全拒绝；损坏 FDT 头/size 返回 `-1` 且 `*out` 清零。
  - 在 `hosttests/mk/memory.mk` 添加规则、`mk/components/run.mk` 添加 `test-aarch64-pcie-host`、在 `config/test-scenarios.json` 注册 `memory.aarch64.dtb_pcie`，运行 `make PROFILE=x86_64-clang regen-test-catalog`。
- [ ] **Step 2: 确认红灯失败**
  - 运行：`make PROFILE=x86_64-clang check-test-catalog && make PROFILE=x86_64-clang test-aarch64-pcie-host CASE=dtb-pcie`
- [ ] **Step 3: 创建 `<arch/aarch64/pcie.h>`、实现 `dtb_parse_pcie.c` 并接入 `dtb.c`**
- [ ] **Step 4: 转绿并验证 DTB 回归与跨架构头文件**
  - 运行：
    `make PROFILE=x86_64-clang test-aarch64-pcie-host CASE=dtb-pcie && python3 qemutests/aarch64_dtb_test.py && make PROFILE=x86_64-clang test-header-object-audit && clang --target=aarch64-none-elf -ffreestanding -fsyntax-only -Ikernel/include kernel/include/arch/aarch64/pcie.h`
  - 预期：全部通过，`DBCRT == 0` 且 AArch64 freestanding 语法无错。

---

### Task 3: Stage-3 ECAM MMIO 预映射与 AArch64 PCI Backend 实现

**Files:**
- Modify: `kernel/include/arch/aarch64/page_table.h`
- Modify: `kernel/arch/aarch64/memory/page_table.c`
- Create: `kernel/arch/aarch64/bus/pci.c`
- Modify: `kernel/arch/aarch64/boot/main.c`
- Create: `hosttests/cases/test_aarch64_pcie_ecam.c`
- Modify: `hosttests/mk/memory.mk`
- Modify: `mk/components/run.mk`
- Modify: `config/test-scenarios.json`
- Regenerate: `mk/generated/*` (`make PROFILE=x86_64-clang regen-test-catalog`)

**Interfaces:**
- `kernel/include/arch/aarch64/page_table.h` & `kernel/arch/aarch64/memory/page_table.c:51`：
  - 将 `#define AARCH64_PT_PA_LIMIT UINT64_C(0x10000000000)` 正式导出至 `kernel/include/arch/aarch64/page_table.h`（`page_table.c` 改用 `#ifndef AARCH64_PT_PA_LIMIT` 或直接复用头文件单一权威定义），满足 `pci.c` 与宿主测试直接引用及 `DBCRT == 0`。
- `kernel/arch/aarch64/bus/pci.c`：
  - 包含 `<arch/pci.h>`、`<arch/aarch64/pcie.h>`、`<arch/aarch64/page_table.h>`、`<arch/aarch64/boot_direct_map.h>`、`<arch/aarch64/uefi_memory_map.h>`、`<arch/aarch64/handoff_layout.h>`、`<arch/aarch64/boot_layout.h>`、`<core/bootinfo.h>`、`<arch/boot.h>`、`<arch/mmu.h>`、`<memory/pmm.h>`（引入 `PAGE_2M_SIZE`/`PAGE_2M_MASK`）、`<log/log.h>`；
  - `int aarch64_pcie_prepare(const struct boot_context *ctx)`：
    - 若 `topology->root_count == 0`：置 `s_ready_root_count = 0` 并直接返回 `0`（RPi5 优雅降级）；
    - 解引用 `ctx->memory.entries` 前执行与 `kernel/arch/aarch64/driver/virtio_mmio.c:54-77` 对等的全部 7 项检查（`boot_context_valid(ctx)`、`BOOT_CONTEXT_HAS_MEMORY_MAP`、`BOOT_MEMORY_FORMAT_UEFI_RAW`、`entries != 0`、`entry_size >= AARCH64_UEFI_MEM_DESC_MIN_SIZE`、`entry_count != 0`、`desc_count * stride` 乘法与 `entries + map_bytes` 加法不溢出、`uefi_map_end <= AARCH64_BOOT_MAP_END`、且不与 `[AARCH64_BOOT_BLOCK_BASE, AARCH64_BOOT_BLOCK_END)` 重叠）；校验不过记 `log_warn` 返回 `0`；
    - 对每个 root：算 `active_size = (uint64_t)(bus_end - bus_start + 1U) << 20`，检验 `reg_cpu_base <= UINT64_MAX - active_size`，算 2 MiB 包络 `[map_start, map_end)`（基于 `<memory/pmm.h>` `PAGE_2M_SIZE`/`PAGE_2M_MASK` 并校验不溢出），校验 `map_end <= AARCH64_PT_PA_LIMIT` 且不重叠 Boot Block B；
    - 逐块调用 **5 参 `aarch64_uefi_range_overlaps_ram(mmap_ptr, stride, desc_count, blk_pa, blk_pa + (uint64_t)PAGE_2M_SIZE)`**（必须 `== 0`）与 `aarch64_pt_ensure_device_2m(ttbr1_root, ARCH_PAGE_OFFSET + blk_pa, blk_pa)`（处于 Stage 3、`smp_starting_enter()` 之前，`vmm_gate_check()` 放行）；任一块返回 `!= 0` 或 `AARCH64_PT_ECONFLICT` 停用该 `root`（保留已验证通过的前序无冲突设备块，不再对其发出 ECAM 访问），全部 `root` 后通过宿主可编译原语 `aarch64_pcie_dsb_sy_isb()` 执行 `dsb sy; isb`；
  - 汇编原语隔离（对照 `kernel/arch/aarch64/memory/page_table.c:23-29`）：将 32 位 `ldr`/`str`+`dsb ld`/`dsb st` 及 `dsb sy; isb` 统一收敛到受 `#if defined(__aarch64__) && !defined(AARCH64_PCIE_HOST_TEST)` 保护的静态内联函数，`#else` 分支在宿主机执行模拟基址读写与编译器内存屏障 `__asm__ volatile("" ::: "memory")`；
  - `arch_pci_backend()`：`s_ready_root_count == 0` 返回 `NULL`（触发 `BUS_UNAVAILABLE`）；就绪返回 `s_aarch64_pci_backend`（`.max_config_offset = 4092`、`read32`/`write32` 遵守 `PCI_ERROR_ROOT`/`PCI_ERROR_FUNCTION` 与 32 位 MMIO 原语、`route_gsi` 返回 `-ENOTSUP`、`translate_resource`、`bus_range`；日志严格遵守 `vlog()` 格式子集）。
- `kernel/arch/aarch64/boot/main.c`：在 `arch_boot_controllers(bootctx)` 第 679 行 `aarch64_virtio_mmio_prepare` 检查后、第 685 行 `msr daifclr, #4` 前加入 `(void)aarch64_pcie_prepare(bootctx);`。
- `hosttests/mk/memory.mk` 与 `mk/components/run.mk`：
  - `memory.mk` 新增 `$(TEST_BLD)/aarch64_pcie_production.o`（使用 `$(AARCH64_PT_SW_HOST_CFLAGS) -DAARCH64_PCIE_HOST_TEST=1`）、**`LINK_CFLAGS_aarch64-pcie-ecam = $(AARCH64_PT_SW_HOST_CFLAGS) -DAARCH64_PCIE_HOST_TEST=1 $(FRAMEWORK_INC)`** 与 **`LINK_GROUP_aarch64-pcie-ecam := $(TEST_BLD)/aarch64_pcie_production.o $(TEST_BLD)/aarch64_pt_production.o $(TEST_BLD)/vmm_gate_production.o $(TEST_BLD)/vmm_gate_test_stubs.o $(TEST_BLD)/serial_sink_stub.o $(LIBC_OBJS)`**（因该链接组不链接 `dtb.o`，`test_aarch64_pcie_ecam.c` 须参照 `hosttests/cases/test_aarch64_virtio_mmio_map.c:52-84` 提供 `aarch64_pcie_topology_get()`、`alloc_4k_page()`、`free_4k_page()`、`aarch64_read_ttbr1()`、`tlb_invalidate_local()`、`tlb_shootdown_all()`、`int g_log_level = 4;` 及 `_log_info_impl()` 桩函数），加 `.PHONY: test-aarch64-pcie-ecam` 并在 `mk/components/run.mk` `test-aarch64-pcie-host` 映射 `ecam:test-aarch64-pcie-ecam`。

- [ ] **Step 1: 编写红灯 ECAM 映射与访问器宿主测试并注册规则**
  - 新建 `hosttests/cases/test_aarch64_pcie_ecam.c`（`@os01-test`：`subsystem: memory`、`link: aarch64-pcie-ecam`、`scenarios: [memory.aarch64.pcie_ecam]`），实现上述 `aarch64_pcie_topology_get()`、页表及日志桩函数并覆盖：
    1. 合法 ECAM 映射在 `vmm_gate` 放行前完成 2 MiB `Device` 映射，`arch_pci_backend()` 非空；
    2. `ctx->memory` M0 越界（`entries == 0`、`entry_size` 过小、乘法/加法溢出、`map_end > AARCH64_BOOT_MAP_END`、与 Boot Block B 重叠）均安全拒绝且不崩；
    3. ECAM 与 RAM 重叠、`>= AARCH64_PT_PA_LIMIT` (`1 TiB`) 或中途块冲突 (`AARCH64_PT_ECONFLICT`) 停用对应 root；
    4. 0 ECAM root（模拟 RPi5）返回 `0` 且 `arch_pci_backend() == NULL`；
    5. 精确检验偏移编码、`PCI_ERROR_ROOT`/`PCI_ERROR_FUNCTION` 错误域、`route_gsi == -ENOTSUP` 及 3 类空间 `translate_resource`。
  - 更新 `hosttests/mk/memory.mk`、`mk/components/run.mk`、`config/test-scenarios.json` (`memory.aarch64.pcie_ecam`)，运行 `make PROFILE=x86_64-clang regen-test-catalog`。
- [ ] **Step 2: 确认红灯失败**
  - 运行：`make PROFILE=x86_64-clang check-test-catalog && make PROFILE=x86_64-clang test-aarch64-pcie-host CASE=ecam`
- [ ] **Step 3: 导出 `AARCH64_PT_PA_LIMIT`、实现 `kernel/arch/aarch64/bus/pci.c` 并接入 `boot/main.c`**
- [ ] **Step 4: 转绿并验证页表关联测试、头文件及边界审计**
  - 运行：
    `make PROFILE=x86_64-clang test-aarch64-pcie-host CASE=ecam && make PROFILE=x86_64-clang test-host && make PROFILE=x86_64-clang test-header-object-audit test-driver-model-boundary`
  - 预期：全部通过。

---

### Task 4: 构建门控、`device/boot.c`、`linker.ld` `.pci_drivers` 与全核集成

**Files:**
- Modify: `kernel/include/arch/features.h`
- Modify: `qemutests/test_boot_facade_headers.py`
- Modify: `kernel/device/boot.c`
- Modify: `kernel/arch/aarch64/linker.ld`
- Modify: `kernel/Makefile`

**Interfaces:**
- `kernel/include/arch/features.h:68`：`#define OS01_HAS_PCI 1`；
- `qemutests/test_boot_facade_headers.py:112`：`_Static_assert(OS01_HAS_PCI == 1, "aarch64 must have OS01_HAS_PCI == 1");`；
- `kernel/device/boot.c`：包含 `<arch/features.h>`，用 `#if OS01_HAS_NET` 门控 `<net/net.h>`、`<net/device.h>` 与 `net_device_init();`；
- `kernel/arch/aarch64/linker.ld`：在 `.subsys_init`（141–146 行）后加入：
  ```ld
    .pci_drivers : AT(ADDR(.pci_drivers) - _kernel_page_offset)
    {
        . = ALIGN(8);
        PROVIDE(__pci_drivers_start = .);
        KEEP(*(.pci_drivers))
        PROVIDE(__pci_drivers_end = .);
    } :data
  ```
- `kernel/Makefile`（严格区分通用源码与架构通配目录，对照 `kernel/Makefile:345-346, 428, 437, 476, 1418`）：
  - AArch64 `KERNEL_C_SOURCES`（128–241 行）**仅添加架构无关源文件**：`device/core.c`、`device/boot.c`、`bus/pci/core.c`、`bus/pci/match.c`、`bus/pci/platform.c`、`bus/pci/pci_devfs.c`；
  - **严禁**将 `arch/aarch64/bus/pci.c` 或 `arch/aarch64/platform/dtb_parse_pcie.c` 加入 `KERNEL_C_SOURCES`（因 `ALL_C_SOURCES := $(KERNEL_C_SOURCES) $(ARCH_C_SOURCES) $(ALL_C_SOURCES_EXTRA)` 不经过 `$(sort ...)` 去重，重复列出将导致同一 `.o` 两次传入 `ld.lld` 报重复符号错误）；
  - 第 312 行 `ARCH_SOURCE_DIRS`（现为 `boot intr memory smp cpu platform runtime driver sched sync syscall`）在保留既有 `platform` 的基础上追加 `bus`（变为 `boot intr memory smp cpu platform runtime driver sched sync syscall bus`），由第 345–346 行自动通配收录 `arch/aarch64/platform/dtb_parse_pcie.c` 与 `arch/aarch64/bus/pci.c`。

- [ ] **Step 1: 确认改前门面基线**
  - 运行 `python3 -m unittest qemutests.test_boot_facade_headers` 确认基线断言存在。
- [ ] **Step 2: 落地 5 处集成修改**
- [ ] **Step 3: 跨架构构建与链接/栈帧审计验证**
  - 验证 x86_64 编译及宿主回归（均用具备 `rootfs` 能力的 `PROFILE=x86_64-clang`）：
    `make PROFILE=x86_64-clang kernel.bin && make PROFILE=x86_64-clang test-arch9-host CASE=pci && make PROFILE=x86_64-clang test-arch9-host CASE=backend && make PROFILE=x86_64-clang test-arch9-host CASE=device-boot && make PROFILE=x86_64-clang test-header-object-audit`
  - 验证 AArch64 产品镜像（`OS01_HAS_PCI=1, OS01_HAS_NET=0, OS01_HAS_USERSPACE=1`）：
    `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi`
  - 验证 AArch64 selftest 变体（`OS01_HAS_PCI=1, OS01_HAS_USERSPACE=0`）：
    `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi`
  - 验证栈帧上限（`test-stack-frame-audit` 门控在 `kernel` 能力下，可直接于 `PROFILE=aarch64-clang` 执行）与门面头断言：
    `make PROFILE=aarch64-clang test-stack-frame-audit && python3 -m unittest qemutests.test_boot_facade_headers`
  - 预期：`ld.lld` 零重复符号且 `dtb_init <= 768 B`、全内核零 `-Wframe-larger-than=512` 报错；`llvm-nm` 可见 `__pci_drivers_start`/`__pci_drivers_end` 位于高半区 `:data`。

---

### Task 5: 用户态 `lspci` 审计、QEMU `virt` E2E、`run.mk` 派发接线与 RPi5 降级验证

**Files:**
- Modify: `user/lspci.c`
- Create: `qemutests/aarch64_pcie_test.py`
- Create: `qemutests/test_aarch64_pcie.py`
- Modify: `mk/components/run.mk`
- Modify: `tools/tests/test_aarch64_uefi_images.py`
- Modify: `config/test-scenarios.json`
- Regenerate: `mk/generated/*` (`make PROFILE=x86_64-clang regen-test-catalog`)

**Interfaces:**
- `user/lspci.c`：
  - 基于源码核实（`user/lspci.c:128` 已有 `{ 0x1af4, 0x1005, "Virtio RNG device" }`，第 140 行已有厂商 `{ 0x1b36, ... }`），精准补齐缺失条目：`{ 0x1b36, 0x0008, "QEMU PCIe Host bridge" }`、`{ 0x1b36, 0x000c, "QEMU PCIe Root Port" }` 与 `{ 0x1af4, 0x1044, "Virtio modern RNG device" }`；
  - 保持单域 `"0000:"` 打印 `bb:ss.f` 兼容格式，非 `"0000:"` 域（面向 RPi5 多域）输出完整 `dddd:bb:ss.f`；审计并验证 `-v` 对 `irq == "none"` 的稳健输出。
- `mk/components/run.mk`、`tools/tests/test_aarch64_uefi_images.py` 与 `qemutests/aarch64_pcie_test.py`（参照 `test-aarch64-boot-logo` @ `run.mk:122, 697-710` 模式，不动只减不增的 `config/test-bridge.json` 与关闭白名单 `test-aarch64 MODE=`）：
  - 在 `mk/components/run.mk:122` 将 `test-aarch64-pcie` 同步加入解析期 `QEMU-only` 检查列表（`ifneq ($(filter run-aarch64-uefi test-aarch64 test-aarch64-boot-logo test-aarch64-pcie,$(MAKECMDGOALS)),)`），使其在 `AARCH64_PLATFORM=rpi5` 下直接解析报错并由 `tools/tests/test_aarch64_uefi_images.py:101-113` 补充回归校验；
  - 在 `mk/components/run.mk` 新增独立 `.PHONY: test-aarch64-pcie`（校验 `require_aarch64_uefi`、`require_capability,uefi`，构建 `aarch64-uefi` 并调用 `python3 qemutests/aarch64_pcie_test.py --check all --diagnostic-dtb=auto --timeout 120 --firmware "$(AARCH64_UEFI_FIRMWARE)" --image "$(AARCH64_UEFI_DISK)" --qemu "$(AARCH64_QEMU)" --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)"`）；
  - `qemutests/aarch64_pcie_test.py` 自建含 `-device virtio-rng-pci,rng=rng0` 与 `-dtb` 的 QEMU 命令（不依赖 `run.mk:231` 默认 `virtio-rng-device`），覆盖：
    - **正向 (`drivers.pci.ecam_virt_lspci`)**：断言 `subsys: init  device-boot ... ok`、`ls /dev/pci` 含 `0000:00:00.0` 与 `0000:00:01.0`、`lspci` / `-n`（含 `1b36:0008` 与 `1af4:1005`/`1044`） / `-v`（资源 + `none` IRQ） / `-x` 全通过且退出码 `0`；
    - **负向 (`drivers.pci.no_ecam_graceful_fallback`)**：向诊断 DTB 移除 `pci-host-ecam-generic` 或注入 `brcm,bcm2712-pcie`，断言无 `FATAL`/`PANIC`、正常抵达 shell 且 `lspci` 安全退出 `1`；
    - 提供 `--self-test` 模式做 QEMU-free 命令构造、DTB 剥离与串口输出解析器单元测试。
- `qemutests/test_aarch64_pcie.py`：
  - 附 `@os01-test` / `TEST_MANIFEST`（`id: test_aarch64_pcie`、`layer: harness`、`subsystem: drivers`、`timing: strict`、`tier: pr`、`scenarios: [drivers.pci.ecam_virt_lspci, drivers.pci.no_ecam_graceful_fallback]`），执行 `aarch64_pcie_test.py --self-test` 及 script-mode `sys.path` 导入检验（同时在 `config/test-scenarios.json` 两场景中补齐 `owner: "qemu_e2e"`、`primary.source: "qemutests/aarch64_pcie_test.py"` 及带有明确 `distinct_risk` 的 `supporting` `[layer: "harness"]` 条目，满足 `testing-policy.md` §6）。

- [ ] **Step 1: 创建 QEMU E2E runner、Harness 守卫测试与 `run.mk` / 场景登记**
  - 新建 `qemutests/aarch64_pcie_test.py` 与 `qemutests/test_aarch64_pcie.py`，在 `mk/components/run.mk` 加入 `test-aarch64-pcie` 及其第 122 行 `qemu-virt` 解析期门禁，在 `tools/tests/test_aarch64_uefi_images.py` 补齐 `test-aarch64-pcie` 的 `rpi5` 拒绝校验，在 `config/test-scenarios.json` 注册 `drivers.pci.ecam_virt_lspci` 与 `drivers.pci.no_ecam_graceful_fallback`，运行 `make PROFILE=x86_64-clang regen-test-catalog`。
- [ ] **Step 2: 更新 `user/lspci.c` 并重建双平台 AArch64 镜像**
  - 按 `Interfaces` 更新 `user/lspci.c`，先后执行 `make PROFILE=aarch64-clang aarch64-uefi` 与 `make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 aarch64-uefi`（生成 `build/aarch64-clang/image/aarch64-uefi.img` 与 `build/aarch64-clang/image/rpi5/aarch64-uefi.img`，其中 RPi5 打包不依赖 `QEMU_EFI.fd`）。
- [ ] **Step 3: 运行全量交叉验证与已有回归**
  - 运行目录、场景、双镜像门禁与框架契约校验（均用具备 `rootfs` 能力的 `PROFILE=x86_64-clang` 执行，确保双镜像已先经 Step 2 生成于 `build/aarch64-clang`）：
    `make PROFILE=x86_64-clang check-test-catalog && python3 qemutests/scenario_catalog.py --check && python3 tools/tests/test_aarch64_uefi_images.py --build-root build/aarch64-clang && make PROFILE=x86_64-clang test-framework-contract test-host`
  - 运行 AArch64 PCIe QEMU E2E 与已有 SMP 回归（用 `PROFILE=aarch64-clang`）：
    `make PROFILE=aarch64-clang test-aarch64-pcie && make PROFILE=aarch64-clang test-aarch64 MODE=smp`
  - **真机验收纪律**：若物理树莓派 5 开发板未实机运行，在最终提交/验收说明中必须明确标明 **“RPi5 真机（`brcm,bcm2712-pcie` 启动链零破坏及 `BUS_UNAVAILABLE` 降级）真机待测”**，仅采信 DTB 宿主测试与 QEMU 无 ECAM 负向 DTB 仿真测试结果。
