---
title: OS01 aarch64 M0 启动直映设计
created: 2026-10-01
updated: 2026-10-01
type: spec
status: draft-for-review
tags: [osdev, aarch64, memory, boot]
related: [docs/roadmap.md P2 M0, 2026-09-11-aarch64-page-table-primitives-design]
---

# OS01 aarch64 M0 启动直映设计

## 1. 目标与边界

在 QEMU `virt,gic-version=2` 的现有 UEFI 启动配置下，MMU 开启时就建立物理 `0x40000000..0x80000000` 的 identity/high-half 双地址映射。内核及 `.boot` 所在的首个 2 MiB block 保持 EL1 可执行；其余 block 为 EL1 可读写、EL1/EL0 均不可执行。普通镜像与 selftest 镜像使用相同的启动页表，不再依赖 selftest 专用的 C 侧补图。

本项只收口 roadmap P2 的 **M0 启动映射契约**。`0x80000000` 以上、非连续 RAM、按 UEFI map 建运行期直映、Slab/VMM/VMA、EL0 地址空间均属后续 M1–M4。当前 QEMU 默认 `-m 512M`，RAM 从 `0x40000000` 起；本设计不把固定 1 GiB 映射宣称为任意内存容量的支持。高半区偏移继续为 `ARCH_PAGE_OFFSET=0xffff000000000000`，不改 TTBR0/TTBR1、TCR、MAIR 或页表总占用。

成功标准：启动页表的 `PMD_low1[0..511]` 在 MMU 开启前全部就位，首 block 和其余 block 的描述符权限精确符合下文约定；首次 `alloc_4k_page()` 的子页池元数据可通过直映地址写入；现有正常启动、SMP/GIC/Timer 与 4 KiB 页表 smoke 无回归。

## 2. 已核实的现状

- `kernel/arch/aarch64/head.S::build_pagetables` 已预置 24 KiB、六页的页表树：`PGD[0] → PUD_low[1] → PMD_low1`。`PMD_low1[0]` 映射 `0x40000000..0x40200000`，其余 511 槽仍为零。`PMD_low0` 另映射 `0..0x40000000`，其中 MMIO 窗口采用 Device 属性。
- `kernel/arch/aarch64/boot/boot_fixup.c::aarch64_extend_direct_map()` 在 C 中填 `PMD_low1[1..511]`；它唯一的调用点在 `kernel/arch/aarch64/boot/main.c` 的 `#if OS01_SELFTEST` 内，并且位于 `pmm_init()` 之后、页表 smoke 之前。普通镜像不执行它。
- 共用 `kernel/memory/pmm.c::alloc_4k_page()` 在新建子页池时通过 `Phy_To_Virt(pg->phy_address)` 写 `struct subpage_pool`。若 PMM 分到 `0x40200000` 以上而直映缺失，EL1 会发生 data abort；已有 EL1h sync 诊断能报告故障，但不能恢复。
- `head.S` 将 `PT_UXN_MASK` 写成 `1 << 52`，并用 `x20 = 0x3 << 52` 作为 PXN|UXN；这实际设置 bit 52（Contiguous hint）和 bit 53（PXN），漏掉 bit 54（UXN）。`PMD_low1[0]` 创建时只清 PXN，既没有先 OR no-execute 掩码，也没有设置 UXN。现有 C 补图和 `memory/page_table.c` 则使用 bit 53/54。Arm 的 [内存模型说明](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20memory%20model%20guide.pdf?revision=58b1dd0a-3800-4218-b21a-f95a0332034c)将 Contiguous/PXN/UXN 分别标在 bit 52/53/54。
- aarch64 linker script 约束 `_kernel_lma_end <= 0x401e0000`，故当前 `.boot` 与内核 LMA 均处于 `PMD_low1[0]` 所覆盖的 2 MiB 内。这个链接断言是 M0 保留首 block 可执行的前提。

## 3. 方案选择

选用**方案 A：在 `head.S::build_pagetables` 完成固定启动窗口，再删除 C 补图**。现有六页页表已包含 `PMD_low1`，不需要新增页表页、内存分配器或新的引导阶段。汇编在 MMU 关闭时写完所有描述符；已有 `dsb sy`、TCR/MAIR/TTBR 设置和 `isb` 顺序继续负责发布启动页表。映射在首次进入高半区 C 代码前即存在，普通镜像和 selftest 镜像不再分叉。

备选方案 B 是把 `aarch64_extend_direct_map()` 挪出 `OS01_SELFTEST` 并提前调用。它能让普通镜像最终拥有映射，但 MMU 启用到 C 调用之间仍有缺口，并保留汇编/C 两份 block 描述符逻辑，故不选。按 UEFI RAM map 动态建完整直映属于 M1；将它塞进 M0 会引入早期页表页分配和稀疏内存策略，故本项不做。

## 4. 启动页表与权限契约

`PMD_low1[i]` 的物理基址为 `0x40000000 + i * 0x200000`，`i=0..511`；每项是有效的 L2 **block** 描述符（`bit[1:0]=01`），不是指向 L3 的 table 描述符。该表由 TTBR0/TTBR1 共享，因此同一描述符同时支撑低地址 identity 和偏移 `0xffff000000000000` 的高半区别名。本项不更改 `PGD`、`PUD`、`PMD_low0` 的拓扑，也不把 `AARCH64_PT_SELFTEST_VA=0xffff800000000000` 的空 PGD 槽映成 RAM。

普通 RAM block 的低位属性沿用当前 C 补图和 `head.S` 的 `PT_V | PT_ATTR_NORMAL | PT_SH_IS | PT_AP_EL1_RW | PT_AF = 0x705`；不设置 bit 1、bit 52 或 EL0 可访问的 AP 值。

| 槽 | PA 范围 | PXN bit 53 | UXN bit 54 | 期望描述符 |
|----|---------|------------|------------|------------|
| `PMD_low1[0]` | `0x40000000..0x40200000` | 0 | 1 | `0x40000000000705 | 0x40000000` |
| `PMD_low1[1..511]` | `0x40200000..0x80000000` | 1 | 1 | `0x60000000000705 | PA` |

修正 `PT_UXN_MASK` 为 bit 54，并保证共用的 no-execute 掩码只含 bit 53/54；首 block 必须先取得 UXN，再仅清 PXN，不能沿用当前对未置位 PXN 执行 `bic` 的空操作。`PMD_low0` 的既有 Normal/Device block 也必须使用修正后的 no-execute 掩码；MMIO 的 Device AttrIndx、物理路由和执行禁止语义不得改变。`kernel/arch/aarch64/memory/page_table.c` 的 PXN/UXN 编码已正确，本项不修改其 API。

对于固定窗口内并非已公布 RAM 的地址，本项维持当前 C 补图会建立的 block 描述符，但测试不得解引用 QEMU 未提供的物理地址。M1 才按真实 RAM map 收紧或扩展直映；本项不得把固定窗口当成 PMM 可分配范围的来源。

## 5. 启动顺序和清理

顺序保持：清 `.boot.bss` → `build_pagetables` 填满固定窗口 → `dsb sy` → 装 TCR/MAIR/TTBR → 开 MMU → 进入 `aarch64_main()` → `aarch64_ram_init()` → `pmm_init()` → 页表 smoke（仅 selftest）→ DTB/GIC/SMP/Timer。`pmm_init()` 不得因 M0 引入新的分配器或设备依赖。

删除 `boot/boot_fixup.c` 与 `main.c` 中的前向声明、调用和过时注释。保留 `aarch64_pt_smoke_test()` 与它现有的 marker/清理语义；不因补图迁移而更改 `KERNEL_SELFTEST` 选择、测试镜像变体或 QEMU harness 对现有日志的判定。

## 6. 验证与失败证据

1. **描述符验证**：在现有 aarch64 selftest 的 PMM smoke 之后、4 KiB 页表 smoke 之前，读取活动页表中的 `PMD_low1[0..511]`，逐项核对物理基址、`0x705` 低位属性、block 类型及 PXN/UXN；明确断言 bit 52 为零。再核对 `PMD_low0` 的一个 Normal 属性 block 和一个 MMIO Device block 的 no-execute 位及 AttrIndx。验证应使用实际页表内容，而非重复调用构造描述符的 helper；第一个、最后一个及中间任意槽均在全表遍历中。失败输出独立的 M0 FATAL marker，并在 GIC/SMP 前停机。`aarch64_pt_query_4k()` 遇 L2 block 返回 `ECONFLICT`，不能把它用作本项的 block 验证器。
2. **实际访问**：保留现有 `alloc_4k_page()` + 4 KiB map/query/unmap smoke，证明从新子页池取得的物理页可经直映地址读写。记录/断言该测试使用的物理页落在已验证的启动窗口中，避免将未来超出窗口的内存误判为 M0 已覆盖。
3. **QEMU 回归**：构建并运行普通 `aarch64-uefi` 镜像，复用 `qemutests/aarch64_uefi_smp.py` 的非 selftest 模式核对正常完成；运行 `make PROFILE=aarch64-clang test-aarch64 MODE=smp`（1/2/4 CPU）和 `MODE=gic-spi`。测试不得仅因 QEMU 超时、UEFI banner 或单个页表 marker 就算通过。
4. **边界/隔离**：检查 `boot_fixup.c` 已不在任何 aarch64 构建输入中；普通镜像不依赖 `OS01_SELFTEST` 才获得 `PMD_low1[1..511]`。确认 `AARCH64_PT_SELFTEST_VA` 初始仍未映射，sync-fault 独立变体仍能触发预期 data abort；x86_64 的 head/PMM/VMM 构建输入不变。

如果权限位错误或中间槽缺失，描述符验证必须明确失败；如果实际直映不可访问，现有 EL1h sync 诊断应给出 data-abort 证据，不能把挂起或超时当作成功。正常镜像的完成标记与 selftest 的全表验证共同证明同一份无条件启动页表被使用。

## 7. 文件职责与完成定义

| 文件 | 职责 |
|------|------|
| `kernel/arch/aarch64/head.S` | 修正 UXN 位、生成 `PMD_low1` 全部 512 个 block，保留启动屏障和页表布局 |
| `kernel/arch/aarch64/boot/boot_fixup.c` | 删除，不再在 MMU 开启后补固定窗口 |
| `kernel/arch/aarch64/boot/main.c` | 删除补图调用；selftest 读取实际启动描述符并维持原有 smoke 顺序 |
| `qemutests/aarch64_uefi_smp.py` / 现有构建目标 | 复用普通与 selftest QEMU 完成判定；只有新增 marker 确有必要时才调整解析器 |

完成 M0 不意味着 `memory/vmm.c` 已可用于 aarch64，也不意味着 `0x80000000` 以上的 RAM 已建立直映。下一阶段 M1 以发布的 RAM map 为输入处理这些范围，并定义页表页来源及 SMP TLB 规则。
