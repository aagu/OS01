---
title: OS01 aarch64 Phase 1 设计 — QEMU virt 最小骨架
created: 2026-08-29
updated: 2026-08-29
type: spec
status: v12-draft（v11 实现后偏差修正）
version: 12
tags: [osdev, aarch64, kernel, system-programming, smp]
phase: 1 (QEMU virt only)
related: [os01-roadmap-and-phases, os01-aarch64-porting-hardware, postmarketos-ai-policy]
---

# OS01 aarch64 Phase 1 设计 — QEMU virt 最小骨架

> **目标**：在 QEMU `virt` 平台上跑通 OS01 aarch64 内核骨架（启动 + spinlock + CNTP tick + SMP），为 phase 2 RPi3 真机适配铺路。本 spec **不**面向完整 aarch64 OS 移植，只求"启动 + 跑通"。
>
> **本版本为 v12**，基于 phase 1 实现完成后的实测（见 §9「实现后偏差记录」）修正三处 QEMU 11.1 与 spec 假设的偏差：① 裸 ELF `-kernel` 下 `x0=0`（非 DTB 地址），DTB 在固定 `0x40000000`；② QEMU virt 裸 ELF 无 EL3 firmware、PSCI 不可用，实际 secondary boot 走 spin-table（需 `-M virt,secure=on`）；③ DTB 合成（真实 DTB 在 identity 窗口外）。

## 0. 范围与边界

**phase 1 做**（QEMU `-M virt,secure=on,gic-version=2 -cpu cortex-a53 -smp 4`）：
- **Task 0：Makefile 改造 + 源码收编 arch 门控**（含 lwIP / font.o 的 arch 排除，见 §5）
- **`-kernel` ARM64 boot 协议入口**（QEMU 直接加载 kernel.elf；**v12 修正：裸 ELF 下 `x0=0`、DTB 在 `0x40000000`，见 §9 偏差①**）
- **新建 `arch/aarch64/main.c` 独立入口**（不复用 x86 `kernel/kernel/main.c`）
- **新建 `aarch64_boot_percpu[NR_CPUS]` 最小 per-CPU 数据**（强制 `.boot.bss`，不依赖 task.h/percpu.h，见 §2.6）
- head.S：EL2→EL1 降级 → 双 TTBR 页表（`.boot` 低段建表）→ MMU enable → **绝对地址 `blr` 跳高半 C** → 高半清零普通 .bss
- aarch64 spinlock 真实现（6 函数 + `spinlock_T`；改 atomic.h `ldxr/stxr` → `ldaxr/stlxr`）
- **最小 PL011 UART 驱动**（QEMU virt @ 0x09000000，**轮询输出**，不用 IRQ）
- CNTP 周期 tick（**PPI 30**；含 `CNTHCTL_EL2` 授权、ISR 重装 TVAL、**初始化完成后 `arch_local_irq_enable()`**）
- 最小 GICv2 配置（**只使能 1 个 PPI = PPI 30**，不启 SPI 33）
- SMP 启动（**v12 修正：QEMU 11.1 裸 ELF 无 PSCI，实际走 spin-table**，见 §9 偏差②；含 AP 降级/独立栈/TPIDR 指 boot_percpu/release 门/online timeout）
- 集成验证（QEMU virt 跑通）

**phase 1 明确不做**：
- **UEFI / BOOTAA64.EFI / PE 打包**（推 phase 2）
- **调度 / 任务切换**（phase 1 无调度，见 §2.6；不编 `sched/*`，不用通用 `percpu_t`/`this_cpu()`）
- **PL011 的 IRQ handler / IMSC 中断**（PL011 纯轮询输出，GIC 不启 SPI 33）
- spin-table SMP 启动（RPi3 EDK2 走 spin-table，留接口位）
- RPi3 真机适配（SD 卡、GPU firmware）
- aarch64 用户态重编（busybox/systest 仍是 x86_64）
- uaccess / signal delivery 真实现（无用户态）
- 完整 GICv2 驱动（phase 1 只配 PPI 30）
- ACPI 表解析（DTB only）
- DTB 完整 FDT 解析器（phase 1 只解析 5 节点，不上 libfdt）
- 完整 lwIP/e1000/AHCI/vga/keyboard/fs/tty 移植（无网络/无 framebuffer/无键盘/无文件系统）
- `arch_register_subsys()` / `arch_register_subsys_percpu()` / `arch_boot_rsdp`（新 main.c 直接调子系统）

**验证策略**：QEMU-first 全部在 homeserver 上跑，本机（aoostar-n100）只读代码做分析。

## 1. 背景与动机

OS01 Roadmap v19 P2 aarch64 适配，参考 `os01-aarch64-porting-hardware`。本次 phase 1 仅做 QEMU virt 最小骨架：
- 避免一上来就啃真机 boot/RAM/电源问题
- QEMU 全控可重现，调试成本低
- 复用现有 8 月沉淀：Phase 9 timer 双层抽象 + Phase 2 arch dispatch 桩 + `e0f5b73` rwlock/seqlock

**QEMU-first 决策树**：
1. QEMU virt SMP=4 跑通 → 2. RPi3 真机适配（phase 2，届时再做 UEFI/EDK2）
2. RPi3 跑通 → 3. 其他开发板（OP R1 Plus LTS 等）

## 2. 架构决策

### 2.1 SMP 启动：spin-table（v12 修正，原 PSCI-only）

**决策**：phase 1 走 **spin-table**（BSP 写 `boot_percpu[i].release = secondary_start` 到共享内存 + `dc cvac` cache flush + `sev`，AP 轮询 `release` 后跳转），aarch64 arch 直接实现 `smp_boot_aps()`。

> **v12 实现后修正（原为 PSCI-only）**：QEMU 11.1 virt 裸 ELF `-kernel` 下，secondary CPU 启动靠 QEMU 写的 smpboot 代码（`default_write_secondary`，本质 spin-table：secondary 轮询 `smp_bootreg_addr`），PSCI SMC 在无 EL3 firmware 时只有 `arm_write_secure_board_setup_dummy_smc` 的 `movs pc, lr` 直接返回——**PSCI 不可用**。已核 QEMU 9.2.0 源码：`virt.c:3286` `secure=false` 默认无 EL3、`boot.c:184-215`。且 QEMU virt 需 `-M virt,secure=on`（EL3）才能启动全部 4 核；`secure=off` 只启 BSP。`psci.c` 仍保留（`psci_version`/`psci_cpu_on`），phase 2 接真实 EL3 firmware 时启用。

**secondary CPU 入口（.boot 低物理 trampoline）**：secondary 复位后 MMU 关，轮询 `boot_percpu[i].release`（spin-table release 门）跳转 `secondary_start`。secondary 入口复用 head.S 的**降级序列**（`CurrentEL` 检查后若 EL3/EL2 则降级 EL1，幂等），然后：

1. **查表匹配逻辑 cpu_id + 独立栈**：读**完整 `MPIDR_EL1`**，线性查 BSP 建立的 `{logical_cpu_id, full_mpidr}` 表（由 DTB `/cpus/reg` 建立，见 BSP 侧）得逻辑 cpu_id，用 `aarch64_boot_percpu[cpu_id].stack` 独立栈（§2.6），**绝不共用 BSP 临时栈**。**不能假定 `MPIDR_EL1.Aff0` 唯一**（多 cluster / 非连续拓扑下 Aff0 可能全为 0，导致多个 AP 索引到 `boot_percpu[0]`）。
2. **设 TPIDR_EL1** → `&aarch64_boot_percpu[cpu_id]`，使最小 per-CPU 数据可访问（**不是** `this_cpu()`，那是通用 percpu_t 接口，phase 1 不接）。
3. **设 VBAR_EL1**（MMU 使能后）→ 全局异常向量表；**初始化本核 GIC CPU interface**（GICC_PMR 等）。
4. **等 release 门**：BSP 清完 `.boot.bss`、建好页表、设好 `aarch64_boot_percpu[cpu_id].go` 后 `dsb sy` 放行；AP 侧 acquire 读（`ldaxr`/`dmb ishld`）后继续。
5. 使能 MMU（复用 BSP 建的 TTBR1 高半 + TTBR0 identity）→ 跳 C 进 `secondary_idle()`（裸 `wfi` 死循环；AP 若不跑 tick/benchmark 则**保持 IRQ mask**）。

**4 核 spinlock benchmark 启动协议（无调度/无 IPI 下的可执行方案）**：
- 状态（`.boot.bss`）：`benchmark_go`（release 门）、`benchmark_done[NR_CPUS]`（完成通知）、`benchmark_total`（**普通非原子计数，唯一受 `bench_lock` 保护**，见 §2.6）。
- **互斥性验证（核心，v10 修正）**：`benchmark_total` **必须在临界区内做普通 `++`，不能是 lock 外的原子累加**——否则 4,000,000 只证明四核循环跑完、不证明临界区互斥。每次迭代严格：
  ```c
  spin_lock(&bench_lock);
  benchmark_total++;      /* 普通非原子递增，唯一受锁保护 */
  spin_unlock(&bench_lock);
  ```
  最终值**不等于** 4,000,000 才能暴露锁失效（丢失更新）。`benchmark_done[]` 仅用于完成通知，不承载互斥性。
- **AP 侧**：online 后（`go` 放行进 idle 前）先**自旋等 `benchmark_go`**（acquire load）；`benchmark_go==1` 时执行 1M 次上述 lock/incr/unlock，完成后用 **release store** 写 `benchmark_done[cpu_id]=1`，再进 `wfi` 死循环。
- **BSP 侧**：确认所有 AP `online` 后，用 **release store** 置 `benchmark_go=1` **并自己同步参与** 1M 次；BSP 完成后 **release store 写 `benchmark_done[0]=1`**（v11 修正：否则 BSP 从不写 done[0]，轮询「所有 done[i]」必然超时），然后有界轮询，**acquire load** 读所有 `benchmark_done[i]`，按 §2.1 的 PASS/FAIL 判据判定，打印每核 `done` 状态后进 `wfi` idle。
- **PASS/FAIL 判据（v11 精确化）**：**只有当所有 `done[i] == 1` 且 `benchmark_total == active_cpu_count * 1,000,000` 时才 PASS**；`active_cpu_count` = 实际参与的四核数（BSP + 成功 online 的 AP）。**deadline 到达一律 FAIL**，并打印未完成核列表 + 实际 `benchmark_total`。禁止把「`benchmark_total==4,000,000` **或** 超时」实现成错误的 OR 判断——那是「要么计数对、要么超时」都当成功。
- **内存序契约（v10 明确）**：`benchmark_go` 与 `benchmark_done[]` 均必须是 **release store / acquire load**（`stlr`/`ldar`，或 `dmb ish` 配对），不能只靠 `volatile`——`volatile` 不提供跨核内存序。`benchmark_total` 的访问序由 `bench_lock` 的 acquire/release 保证（§2.4 的 LDAXR/STLXR）。
- **超时机制（v10 修正，不可用 CNTP tick）**：benchmark 在 `smp_boot_aps()` 内完成，而 `arch_local_irq_enable()` 在其**之后**才执行，CNTP IRQ tick 此时**不会进 ISR**、tick 计数不推进。超时只能用**不依赖 IRQ 的 counter deadline**：`arch_cycle_counter()`（`CNTVCT_EL0`）取起止差值，超时未集齐 `done` 则报告「未完成核」列表。

**BSP 侧 `smp_boot_aps()` 必须做**：
- **先建 `{logical_cpu_id, full_mpidr}` 表**：遍历 DTB `/cpus` 节点的 `reg` 值，为每个 CPU 建立「逻辑 cpu_id（数组下标）↔ 完整 MPIDR」映射（存 `aarch64_boot_percpu[i].mpidr`）。这是 AP 反查逻辑索引的唯一依据。
- **读取 BSP 自身 `MPIDR_EL1`，映射为 logical CPU 0**：BSP 完整 MPIDR 在 DTB 表中对应逻辑索引 0（`boot_percpu[0]` 已由 BSP 使用）。
- **遍历映射表时跳过 BSP 项**：`smp_boot_aps()` 仅对**非 BSP** 的 CPU 写 `release` 门（spin-table release）；对 BSP 项（logical 0）**不写**（它已在线）。`target` 是完整 MPIDR（DTB `/cpus` 的 `reg` 值），不是逻辑 cpu_id。
- **检查返回码**（x0 = `PSCI_SUCCESS`/错误码）——v12：spin-table 无 SMC 返回码，此条仅对 phase 2 的 PSCI 路径适用
- 以 `aarch64_boot_percpu[cpu_id].online` + 超时轮询等待 AP 就位（online timeout 纳入验收，§6；online 轮询用 `arch_cycle_counter()` deadline，不依赖 IRQ）
- 放行顺序：**BSP 完成 `.boot.bss` 清零、页表、release 门写入并 `dsb sy` + `dc cvac` cache flush 之后，才发 `sev` 唤醒 AP**；否则 AP 跑在垃圾 BSS/未初始化页表上

**DTB CPU 表的失败条件（v10 新增，缺失会数组越界/错栈/BSP 无限等）**：解析 `/cpus` 与建 `{logical_cpu_id, full_mpidr}` 表时，以下情况必须**显式拒绝（panic 或 halt）**，不得静默继续：
- **CPU 数 > `NR_CPUS`**：DTB `/cpus` 的 CPU 个数超过 `NR_CPUS` → 数组越界，panic。
- **MPIDR 重复**：两个 `/cpus/reg` 值相同（或 AP 的 `MPIDR_EL1` 匹配到多个表项）→ 映射表非法，panic。
- **BSP MPIDR 不在表内**：读 BSP 自身 `MPIDR_EL1` 后，在 DTB 表中查不到对应项 → 无法确定 logical 0，panic。
- **AP 查不到自身 MPIDR**：secondary 读完整 `MPIDR_EL1` 线性查表无匹配 → 无法得逻辑索引/栈，panic（写 PL011 或 halt，勿用未初始化栈）。

**理由**：QEMU `virt` 裸 ELF `-kernel` 的 secondary boot 本质是 spin-table（见 v12 修正注）；RPi3 EDK2 也走 spin-table（phase 2 目标一致）；不抽象 `smp_boot_aps()` 公共接口（SIPI/PSCI/spin-table 差异太大）。

### 2.2 MMU：4 KiB + 48-bit VA + 双 TTBR

**决策**：4 KiB granule + 48-bit VA（T0SZ=16 且 T1SZ=16）+ **TTBR0_EL1 与 TTBR1_EL1 双表** + 4 级页表。

**双 TTBR 分工**（v4 已定，保持不变）：

| 寄存器 | 覆盖 VA 区间 | phase 1 映射内容 | 页属性 |
|---|---|---|---|
| **TTBR0_EL1** (T0SZ=16) | `0x0000_0000_0000_0000` .. `0x0000_FFFF_FFFF_FFFF` | **低地址 identity map**（VMA=LMA）：`.boot` 段/DTB/各 CPU 栈/页表自身 | Normal WBWA |
| **TTBR0_EL1**（MMIO 区） | `0x0800_0000`(GICD) / `0x0801_0000`(GICC) / `0x0900_0000`(PL011) | **Device-nGnRnE** | Device |
| **TTBR1_EL1** (T1SZ=16) | `0xFFFF_0000_0000_0000` .. `0xFFFF_FFFF_FFFF_FFFF` | **高半 kernel + direct-map**：kernel image 高半映射、全 RAM direct-map | Normal WBWA |

**TCR_EL1 完整位域（v6 修正 IPS）**：
- `TG0 = 4K`（0b00）**且 `TG1 = 4K`**（0b10）
- `T0SZ = 16`、`T1SZ = 16`
- `SH0 = SH1 = Inner Shareable`（0b11）
- `IRGN0 = IRGN1 = Normal WBWA`（0b01）、`ORGN0 = ORGN1 = Normal WBWA`（0b01）
- **`IPS = 40-bit`（0b010）**——**v6 修正：不能武断设 48-bit**。Cortex-A53 的 `ID_AA64MMFR0_EL1.PARange` 是 **40-bit 物理地址**（A53 TRM 明确），TCR 的 IPS 必须与硬件 PARange 匹配或更低；**48-bit VA 不要求 48-bit PA**。phase 1 固定 `IPS=0b010`（40-bit）；phase 2 若支持大物理内存平台，再启动时读 `ID_AA64MMFR0_EL1.PARange` 动态选值。
- 具体编码留 plan（`TCR_EL1` 值 = 拼接上述字段）

**同步序（v7 修正为无条件，v6 把 dsb 写成条件项）**：
1. **填页表描述符 → `dsb sy`**（页表描述符必须在启用翻译前对 page-table walker 可见——此条件**必然成立**，故 `dsb` 是**无条件**的，不是"若有数据才补"。phase 1 保守用 `dsb sy`；严格可优化为 `dsb ishst`，但为清晰起见先 `dsb sy`）
2. 写 `TCR_EL1`、`MAIR_EL1`、`TTBR0_EL1`、`TTBR1_EL1` → `isb`（确保系统寄存器写完成）
3. **显式构造 `SCTLR_EL1`**：读改写（`mrs`→置位→`msr`）或显式常量，**保留 RES1 位、明确置 `M`(MMU)、`C`(data cache)、`I`(instruction cache)**；**不要对未知复位值只做 `M=1`**（v6 修正）
4. 写 `SCTLR_EL1` → `isb`

**MAIR_EL1 至少 2 个属性索引**（沿用 v4）：
- AttrIdx 0 = **Device-nGnRnE**（0b00000000）→ MMIO
- AttrIdx 1 = **Normal Inner/Outer WBWA**（0b11111111）→ RAM/direct-map/kernel/DTB/栈/页表

**早期映射 descriptor 最小属性（v8 新增，首个 MMU 开启能否取指的必要契约）**：每个 PTE/block descriptor 必须显式包含以下位，不能留 plan 自行猜测：

| 字段 | phase 1 取值 | 说明 |
|---|---|---|
| **valid / type** | block（level 1/2）或 page（level 3）descriptor，`bit[0]=1`；table descriptor `bit[1]=1` | 页表项 vs 块/页必须正确区分，否则 walker 报 translation fault |
| **AF**（Access Flag） | **`AF=1`** | Cortex-A53 支持硬件 AF，未置 AF 会触发 **access-flag fault**；phase 1 不接软件 AF 处理，必须预置 AF=1 |
| **AttrIndx** | 0（Device）或 1（Normal WBWA） | 引用 MAIR_EL1 的属性索引 |
| **SH**（shareability） | Inner Shareable（0b11） | 与 TCR 的 SH0/SH1 一致 |
| **AP**（EL1 读写权限） | `AP=0b00`（EL1 RW，EL0 none） | phase 1 无用户态，EL0 一律不可访问 |
| **PXN** | kernel image 段 `PXN=0`（可执行）；direct-map/MMIO/DTB/栈/页表 `PXN=1` | 内核代码可执行，数据区不可执行 |
| **UXN** | **`UXN=1`（全部）** | phase 1 无用户态，EL0 执行一律禁止 |

**执行权限策略（v8 明确）**：
- **kernel image**（`.boot` 低段 + 高半 .text）：`PXN=0`、`UXN=1`（EL1 RWX，仅内核可执行）
- **direct-map / MMIO（GICD/GICC/PL011）/ DTB / 栈 / 页表**：`PXN=1`、`UXN=1`（不可执行，RW）
- 保守起见 phase 1 **不启用** WXN（`SCTLR_EL1.WXN`），执行权限完全靠每级 descriptor 的 PXN/UXN 表达（`arch_cpu_enable_nx` 在 cpu.h 是设 WXN，phase 1 不用它）。

**block 映射与 PXN 兼容（v9 新增）**：L1/L2 block（1 GiB/2 MiB）的 PXN/UXN 是整个 block 统一的——若 `.boot`/`.text`（需可执行，PXN=0）与栈/页表/DTB（需不可执行，PXN=1）落在**同一个 2 MiB block** 内，无法用 block 表达不同执行权限。二选一（phase 1 推荐前者）：
- **混合权限区域强制用 L3 4 KiB page mapping**：凡需要区分 PXN 的区域（含 `.boot` 低段若与数据混排）一律用 L3 page，逐页设 PXN。
- **或**：接受低 `.boot` 所在 2 MiB block 为 **RWX**（粗粒度、单 block 内 kernel 代码与数据同权），在文档中显式承认此例外，其余区域仍走 L3 page 精确权限。
- 高半 kernel `.text`（可执行）与高半 direct-map（不可执行）本就落在不同 2 MiB 边界时，可用 block；否则降级 L3。

**映射范围（进入 C 前必须已建好，沿用 v4）**：
1. PL011 MMIO @ 0x09000000（Device）
2. GICD @ 0x08000000 + GICC @ 0x08010000（Device）
3. DTB 物理地址（保留 identity）
4. kernel image 物理加载段（identity + 高半两套）
5. 各 CPU 栈 + 页表自身（identity）

**清 BSS 与页表的先后（沿用 v5）**：页表放 **`.boot.bss`**（低 VMA/LMA）：
- 最早阶段**只清 `.boot.bss`**，**然后建页表**。顺序是「清 .boot.bss → 建页表 → 使能 MMU」。
- 普通高半 `.bss` 在 **MMU 打开、跳到高半之后**再清零。
- 严禁「建页表 → 清整个 .bss」。

**`ARCH_PAGE_OFFSET`**：0xffff000000000000（高半 direct-map 虚址偏移，属 **TTBR1**）。

**不实现项**：`arch_user_range_accessible` 留 false（phase 1 无用户态）。

### 2.3 Generic Timer：CNTP 周期模式 + PPI 30 + 显式开 IRQ（沿用 v5）

**决策**：CNTP 周期模式，**PPI 30**，GICv2 路由到对应 CPU。PPI 号由 DTB 探测，不硬编（预期 30）。

**EL2 授权（沿用 v5）**：head.S 降级 EL1 前在 EL2 设 `CNTHCTL_EL2`：
- **bit 0 `EL1PCTEN`**：允许 EL1 访问物理 timer 寄存器
- **bit 1 `EL1PCEN`**：允许 EL1 访问物理 counter（`CNTPCT_EL0`）
- 两者**一并置位**（Arm Generic Timer guide 标准做法）

**周期不是自动的（沿用 v4）**：`CNTP_TVAL_EL0` 是**一次性递减比较**，触发后**不自动重装**。tick ISR 必须**每次手动重写 `CNTP_TVAL_EL0 = period`** 再 EOI。

**tick ISR 顺序（沿用 v4）**：
```
重写 CNTP_TVAL_EL0 = CNTFRQ_EL0 / HZ   ← 先重装
写 GICC_EOIR = PPI 30                  ← 再 EOI
printk "+tick"
```

**显式解除 IRQ mask（沿用 v5）**：在 **VBAR_EL1、GIC CPU interface、PPI 30 enable、CNTP enable 全部完成后**，BSP 明确执行 `arch_local_irq_enable()`（`msr daifclr, #2`）。AP 若不跑 tick/benchmark 则保持 IRQ mask。

**clocksource vs clockevent 语义差**（沿用）：
- `arch_cycle_counter()` = `mrs cntvct_el0`（**虚拟** counter）
- `arch_tick_start()` 设 `CNTP_TVAL_EL0`（**物理** counter）

**实现位置**：`time.c`（`arch_tick_start()` + 最小 tick ISR）、`gic.c`（只 PPI 30）、`dtb.c`（解析 `/timer`）。

### 2.4 spinlock：LDAXR/STLXR（沿用 v4）

**决策**：aarch64 提供 **6 函数 + `spinlock_T`**，CAS 用 **LDAXR/STLXR**（acquire/release）。

**原子语义修正（沿用 v4）**：现有 `atomic.h` aarch64 端用 `ldxr/stxr`（relaxed）。二选一（plan 定，推荐前者）：
- **改 `atomic.h`**：`arch_atomic_cas`/`arch_atomic_xchg`/`arch_atomic_write` 的 `ldxr/stxr` → `ldaxr/stlxr`；`fetch_add/inc` 保持 relaxed。
- **或**：spinlock 内联自写 `ldaxr/stlxr` CAS 循环。

**实现位置**：`include/kernel/arch/aarch64/spinlock.h` + `include/kernel/arch/atomic.h`。

**接口**（`spinlock_T.lock`=1 表示 unlocked）：
```c
typedef struct { __volatile__ unsigned long lock; } spinlock_T;

void spin_init(spinlock_T *lock);
void spin_lock(spinlock_T *lock);                              // acquire: LDAXR
void spin_unlock(spinlock_T *lock);                            // release: STLXR
long spin_trylock(spinlock_T *lock);
uint64_t spin_lock_irqsave(spinlock_T *lock);                  // 返回旧 DAIF
void spin_unlock_irqrestore(spinlock_T *lock, uint64_t flags); // 恢复 DAIF
```

### 2.5 入口协议 + 链接契约

**决策**：`-kernel kernel.elf`（ARM64 boot 协议），弃 UEFI；新建 `arch/aarch64/main.c` 提供 `void aarch64_main(uint64_t dtb_base)`。

**链接契约（沿用 v5）**：
- **`.boot` 段：`VMA = LMA = 0x40080000`**，含 `_start` + boot 代码 + secondary trampoline + **`.boot.data` / `.boot.bss`**（页表、早期栈、DTB 保存槽、AP 门变量、`aarch64_boot_percpu`）；`ENTRY(_start)`（低物理地址）
- **其余段（.text/.rodata/.data/.bss）：`VMA = 高半`（`0xffff0000_40080000` 起），`LMA = 低物理`（`0x40080000` 起）**
- **高半跳转（沿用 v5）**：低 `.boot` 到高半 `aarch64_main` 用**绝对地址 `blr`**（literal/ABS64 重定位加载绝对高半地址），非 PC-relative `bl`：
  ```asm
  ldr x30, =aarch64_main      /* ABS64 重定位，解析为高半绝对地址 */
  br  x30                     /* 或 blr x30，按需 */
  ```
- **VBAR_EL1 时序（沿用 v5）**：MMU 打开后再写指向高半 entry.S 的 `VBAR_EL1`（2 KiB 对齐）；boot 极早期靠 `DAIFSet` 屏蔽，无需向量表。
- **AP 路径同序**。

**握手约定（v6 修正 DTB 槽，v12 修正裸 ELF x0）**：
- `-kernel` 复位：**裸 ELF 下 `x0 = 0`（QEMU 不设 x0），DTB 放在固定 `0x40000000`**（`loader_start` = VIRT_MEM base = 1 GiB）；复位在 EL2（`secure=on` 时 EL3）。**v12 修正：spec 原写「x0 = DTB 物理地址」只对 Linux Image 格式成立（`arm_write_bootloader` 设 x0），裸 ELF 不成立**。`dtb_init()` 若 `x0` 处 magic 不对，fallback 到 `0x40000000` 读 DTB。
- `_start`：**先把 x0 保存到 callee-saved 寄存器（如 x19）跨 EL3→EL1 降级与 `.boot.bss` 清零存活 → 清 `.boot.bss` → 建双 TTBR 页表 → 设 TCR/MAIR/TTBR（isb）→ 显式 SCTLR_EL1（isb）→ 绝对地址 `blr` 高半 → 高半清普通 .bss → `mov x0, x19` 恢复 dtb_base（v12 修正：`setup_bsp_stack_and_tpidr` 会覆盖 x0，跳转前必须恢复）→ `aarch64_main(dtb_base)`**
- **DTB 槽时序（v6 关键修正，v12 补充）**：**禁止「先写 DTB 槽 → 再清 `.boot.bss`」**（会把自己刚写的槽清零）。正确顺序是「x0 存寄存器 → 清 `.boot.bss` → 建表 → MMU → 高半清 .bss → `mov x0, x19` 直接传参」。v12 实测发现 x0 传参链路有 bug（`setup_bsp_stack_and_tpidr` 覆盖 x0），已改为跳转前 `mov x0, x19`，不再依赖 DTB 槽。
- `aarch64_main` 不读 x86 boot_info；子系统顺序初始化；不接 subsys 框架

### 2.6 per-CPU 数据：新建 `aarch64_boot_percpu`

**决策**：phase 1 **不使用通用 `percpu_t`/`percpu_data`/`this_cpu()`**，新建最小结构 `aarch64_boot_percpu[NR_CPUS]`。

**为什么（沿用 v5）**：`percpu.h:6` `#include <kernel/task.h>`，`percpu_t` 含 `tss_struct`/`run_queue`/`idle`/`rq_lock`，与「phase 1 不编 task/sched」矛盾。

**`aarch64_boot_percpu` 定义**：
```c
// kernel/arch/aarch64/aarch64_percpu.h
typedef struct {
    uint64_t self;      // 自指针（offset 0，供 asm 直接 load）
    uint32_t cpu_id;    // 逻辑 CPU 号（查 {logical_cpu_id, full_mpidr} 表所得本地索引，不取自 Aff0）
    uint64_t mpidr;     // 完整 MPIDR_EL1 / DTB /cpus 的 reg 值（spin-table release 门索引 / phase 2 PSCI cpu_on 的 target 用）
    uint64_t stack;     // 本核独立栈顶（early stack）
    uint32_t online;    // 1 = 已就位（BSP 轮询）
    uint32_t go;        // release 门（BSP 放行 AP，进入 secondary_idle）
} aarch64_boot_percpu_t;

// benchmark 共享状态（同样放 .boot.bss，供无调度/无 IPI 的四核协同）
spinlock_T bench_lock;                        // 保护 benchmark_total（§2.4 的 acquire/release 锁）
volatile uint32_t benchmark_go;               // BSP release store 置 1 后四核开跑
volatile uint32_t benchmark_done[NR_CPUS];    // 每核 release store 写 1 表示完成 1M 次
volatile uint32_t benchmark_total;            // 普通非原子计数，唯一受 bench_lock 保护（临界区内 ++，期望 4,000,000）

// 定义处必须强制进 .boot.bss，见下
extern aarch64_boot_percpu_t aarch64_boot_percpu[NR_CPUS];
```
- **强制段属性（v6 关键修正，v5 漏）**：AP 在 MMU 关时读 `stack`/`go`/`online`，数组**必须**落在低 VMA/LMA 的 `.boot.bss`，而不是普通高半 `.bss`。定义处写：
  ```c
  __attribute__((section(".boot.bss"), aligned(8)))
  aarch64_boot_percpu_t aarch64_boot_percpu[NR_CPUS];
  ```
  早期栈同理 `__attribute__((section(".boot.bss")))`。linker.ld 用 **`KEEP(*(.boot.bss*))`** 把 `.boot.bss` 收入低地址 boot 段。**不要依赖「文件位于 arch/aarch64/」来决定 section**——C 全局变量默认进普通 `.bss`，必须显式 section。
- **`mpidr` 字段是 `uint64_t`**：保存完整 MPIDR/DTB `/cpus/reg` 值；`cpu_id` 单独存作本地索引。**spin-table release 门（phase 1）与 phase 2 的 PSCI `cpu_on` 都以完整 MPIDR 作 target，不能把逻辑 cpu_id 混作该参数**（§2.1）。
- TPIDR_EL1 指向 `&aarch64_boot_percpu[cpu_id]`（head.S 直接 `msr tpidr_el1`）
- **不实现** `this_cpu()` 语义；phase 1 per-CPU 访问直接 `&aarch64_boot_percpu[cpu_id]` 或读 TPIDR
- **接入通用 `percpu_t` 留 phase 2**（届时 aarch64 提供自己的 `get_current_task` + 最小 percpu_t）

## 3. 接口定义

### 3.1 架构无关接口（沿用）

phase 1 **不引入新的架构无关接口**。aarch64 arch 自有实现（与 x86_64 同名同语义）：

| 接口 | aarch64 phase 1 实现位置 | x86_64 实现位置 |
|---|---|---|
| `void smp_boot_aps(void)` | `kernel/arch/aarch64/smp.c` | `kernel/arch/x86_64/smp.c` |
| `bool arch_tick_start(void)` | `kernel/arch/aarch64/time.c` | `kernel/arch/x86_64/time.c` |

`kernel/include/kernel/smp.h` 现有声明**不动**。

### 3.2 aarch64 真实现替换清单

| 文件 | 当前状态 | phase 1 动作 |
|---|---|---|
| `kernel/arch/aarch64/make.config` | 骨架 | **改**：去 `-fpie`（`-fno-pie -fno-pic`）、补 `ASM`/`OBJFORMAT` + platform 源 |
| `kernel/arch/aarch64/head.S` | 不存在 | **新建**：`_start`（x0→x19 保存 DTB、降级+CNTHCTL、清 .boot.bss、双 TTBR 页表、TCR/MAIR/TTBR(isb)+SCTLR(isb)、绝对地址 blr 高半、高半清 .bss、写 DTB 槽）+ secondary trampoline |
| `kernel/arch/aarch64/entry.S` | 不存在 | **新建**：VBAR_EL1 异常向量表（2 KiB 对齐）+ EL1 IRQ/SVC dispatch |
| `kernel/arch/aarch64/trap.c` | 不存在 | **新建**：ESR_EL1/FAR_EL1 解析 + EL1 IRQ dispatch 到 GIC |
| `kernel/arch/aarch64/main.c` | 不存在 | **新建**：`aarch64_main(dtb_base)`（初始化末 `arch_local_irq_enable()`） |
| `kernel/arch/aarch64/linker.ld` | 不存在 | **新建**：`.boot` VMA=LMA=0x40080000 + `.boot.data`/`.boot.bss`（`KEEP(*(.boot.bss*))`）+ `ENTRY(_start)`；其余段高半 VMA/低物理 LMA |
| `kernel/arch/aarch64/boot_percpu.c` | 不存在 | **新建**：`aarch64_boot_percpu[NR_CPUS]` + early stacks，**均 `section(".boot.bss")`** |
| `kernel/arch/aarch64/time.c` | 不存在 | **新建**：`arch_tick_start()` + 最小 tick ISR（重装 TVAL → EOI → printk） |
| `kernel/arch/aarch64/gic.c` | 不存在 | **新建**：最小 GICv2（只 PPI 30） |
| `kernel/arch/aarch64/dtb.c` | 不存在 | **新建**：最小 FDT（5 节点，读 `/cpus` 的完整 `reg`=MPIDR） |
| `kernel/arch/aarch64/psci.c` | 不存在 | **新建**：SMC `cpu_on(target_mpidr, entry)`（查返回码） |
| `kernel/arch/aarch64/smp.c` | 不存在 | **新建**：`smp_boot_aps()`（release 门 + online timeout） |
| `kernel/arch/aarch64/pl011.c` | 不存在 | **新建**：最小 MMIO UART（轮询输出 @ 0x09000000） |
| `kernel/arch/aarch64/test_spinlock.c` | 不存在 | **新建**：spinlock benchmark |
| `kernel/include/kernel/arch/aarch64/spinlock.h` | `#error` stub | **替换**：6 函数 + `spinlock_T` |
| `kernel/include/kernel/arch/atomic.h` | aarch64 端 `ldxr/stxr` | **改**：`cas/xchg/write` → `ldaxr/stlxr` |
| `kernel/include/kernel/arch/cpu.h` | aarch64 端 `arch_tick_start` 是 `static inline ... false` | **改**：改 extern 声明 `bool arch_tick_start(void);` |
| `kernel/include/kernel/arch/mmu.h` | `arch_user_range_accessible()` 返 false | 不动 |
| `kernel/include/kernel/arch/thread.h` | signal delivery stub | 不动 |
| `kernel/include/kernel/arch/gate.h` | 需 `sys_vector_install` 实实现 | 由 trap.c 提供（或 main.c 直接设 VBAR_EL1，plan 定） |

**关于 `percpu.h` / `this_cpu()`**：phase 1 **不改 `percpu.h`、不接入通用 `percpu_t`**。新增独立的 `aarch64_boot_percpu_t`（§2.6，强制 `.boot.bss`），TPIDR_EL1 由 head.S 直接写。接入通用 percpu_t 留 phase 2。

**关于 `arch_boot_rsdp` / `arch_register_subsys(_percpu)`（沿用 v4）**：phase 1 既不定义也不引用。

**总计新建 ~13 个文件 + 改 4 个头/配置文件**，预计 1300-1900 行（含注释）。

**Makefile 改造范围（沿用 v4）**：
1. 根 Makefile：去 `export CC/LD` target 硬编
2. kernel/Makefile `CC`/`LD` 条件化（`-target aarch64-none-elf` / `-m aarch64elf`）
3. kernel/Makefile `EXTRA_OBJECTS` 条件化（`trampoline_bin.o` + `font.o` 仅 x86_64）
4. kernel/Makefile `LWIP_OBJECTS` 条件化（仅 x86_64）
5. kernel/Makefile `KERNEL_C_SOURCES` 按 arch 门控（aarch64 只收最小集）

## 4. head.S 启动流程（文字流程图）

```
[QEMU -M virt,secure=on,gic-version=2 -kernel kernel.elf]     (无 UEFI；裸 ELF：x0=0、DTB@0x40000000；复位 EL3→EL2→EL1)
        │
        ▼
head.S _start  (低物理 0x40080000，ENTRY 点，MMU 关)
        │
        ├─ 保存 x0 = dtb_base 到 callee-saved 寄存器 x19（跨降级/清零存活，勿写槽）
        ├─ CurrentEL 检查：若 EL2 → HCR_EL2(RW=1)、SPSR_EL2(EL1h, DAIF mask)、
        │    CNTHCTL_EL2.{EL1PCTEN,EL1PCEN}=1、VBAR_EL2 → eret 降级 EL1
        ├─ 关中断 (msr DAIFSet, #0xf)
        ├─ 读 CPUID (mrs MIDR_EL1) → 验证 Cortex-A53
        ├─ 只清 .boot.bss（页表/early stack/DTB 槽/AP 门变量/boot_percpu 的低地址容器）
        ├─ 建双 TTBR 页表（见 §2.2 映射范围；页表放 .boot.bss）
        ├─ dsb sy（页表描述符对 walker 可见，无条件）
        ├─ 设 TCR_EL1(T0SZ=T1SZ=16, TG0=TG1=4K, SH/IRGN/ORGN, IPS=40-bit) + MAIR_EL1
        ├─ 写 TTBR0_EL1 + TTBR1_EL1 → isb
        ├─ 显式构造 SCTLR_EL1（保留 RES1、置 M/C/I）→ 写 SCTLR_EL1 → isb
        ├─ 设 BSP 栈 + TPIDR_EL1=&aarch64_boot_percpu[0]
        ├─ 绝对地址 blr 跳高半 →（高半）清普通 .bss → 设 VBAR_EL1（2 KiB 对齐）
        ├─ mov x0, x19 恢复 dtb_base（v12：setup_bsp_stack_and_tpidr 覆盖了 x0）
        └─ aarch64_main(x0 = dtb_base)   ← 直接传参，不再依赖 DTB 槽
        ▼
aarch64_main  (kernel/arch/aarch64/main.c)
        │
        ├─ pl011_init() → printk 通道（轮询）
        ├─ dtb 解析 → /cpus(完整 MPIDR) /psci /timer /interrupt-controller /pl011
        │      └─（v12：裸 ELF DTB 在 identity 窗口外，fallback 合成 4-CPU 表）
        ├─ gic_init()（distributor + CPU interface，只 PPI 30）
        ├─ arch_tick_start()（CNTP 周期，PPI 30）
        ├─ smp_boot_aps() → 设 release 门 + dsb sy + dc cvac + sev（spin-table 唤醒 AP）
        │      └─（内含 benchmark：go release → 四核临界区 ++ → 集齐 done → PASS/FAIL 报告）
        ├─ arch_local_irq_enable()   ← benchmark 结束后才解除 DAIF.IRQ mask（benchmark 超时用 CNTVCT，不依赖 IRQ）
        └─ wfi 死循环（tick ISR 唤醒 printk "+tick"）
        ▼
secondary 入口 (head.S secondary trampoline，低物理)
        └─ 降级检查(幂等) → 读完整 MPIDR_EL1 → 查表匹配 cpu_id + 独立栈(boot_percpu.stack)
             → TPIDR_EL1 → 等 release 门(acquire) → dsb/isb → 使能 MMU → 设 VBAR_EL1
             → GIC CPU iface → secondary_idle()（裸 wfi / benchmark 循环；不跑 tick 则保持 IRQ mask）
```

**关键点（补强）**：
- **DTB 槽时序**：x0 存 x19 跨降级/清零 → 清 `.boot.bss` → 建表 → MMU → 高半清 .bss → **再写 DTB 槽** → 从槽重载 dtb_base（§2.5）。禁止「先写槽再清 .boot.bss」
- **boot_percpu 强制 `.boot.bss`**：AP 关 MMU 读 stack/go/online，必须低地址（§2.6）
- **IPS=40-bit**：匹配 Cortex-A53 PARange，不武断 48-bit（§2.2）
- **dsb/isb 完整序 + 显式 SCTLR 构造**（§2.2）
- **清 BSS 与页表顺序**：只清 `.boot.bss` → 建表 → MMU → 高半清普通 .bss（§2.2）
- **高半跳转**：绝对地址 `blr`（literal/ABS64），非 PC-relative `bl`（§2.5）
- **VBAR 时序**：MMU 开后写高半 `VBAR_EL1`，2 KiB 对齐（§2.5）
- **IRQ 解除**：`arch_local_irq_enable()` 在 GIC/CNTP 就绪**且 benchmark 结束后**（§2.3）；benchmark 自身超时用 CNTVCT，不依赖 IRQ
- **双 TTBR**：高半走 TTBR1，低地址 identity + MMIO 走 TTBR0（§2.2）
- **链接契约**：`_start` 与 secondary entry 都低物理 `.boot`，MMU 后才切高半（§2.5）
- page table 放 `.boot.bss`；PL011 在 `pl011_init()` 前 printk 不可用

## 5. 实施任务分解（5 任务）

### Task 0：Makefile 改造 + 源码收编 arch 门控（沿用 v4）

**目标**：`make ARCH=aarch64 kernel.elf` 只编 aarch64 兼容源，不编 x86 全量、不编 lwIP、不编 font.o；输出 aarch64 ELF。

**策略**：aarch64 phase 1 = `arch/aarch64/*.c` + `*.S` + 少量 arch-neutral 支撑源。
- **明确排除**：`kernel/main.c`；一切 `#include <kernel/task.h>` 的文件（`time/tick.c` `time/timer.c` `test/selftest.c` `memory/pmm.c`→`slab.c`→`vmm.c`）；`sched/*` `fs/*` `tty/*` `block/*` `net/*` `driver/*` `intr/apic/*` `intr/pic/*`；**`test/*` 全部**；**lwIP**、**font.o**。
- **保留**（链接期需求为准，从少到多）：`kernel/printk.c`（若 arch-neutral 则复用，否则 `arch/aarch64/` 最小 printk）+ libc string/memcpy（`-lk`）。每个新源核对不触 `task.h`/x86 头。
- **方法**：从 `arch/aarch64/*.c` 起步，链接器缺啥补啥。

**改动清单**（5 条）：根 Makefile 去 export；`CC`/`LD` 条件化；`EXTRA_OBJECTS`（trampoline+font）条件化；`LWIP_OBJECTS` 条件化；`KERNEL_C_SOURCES` 门控。

**验证**：`make ARCH=aarch64 kernel.elf` 成功；`file` 输出 `AArch64`。

### Task 1: head.S + linker.ld + panic loop

- **目标**：QEMU virt 启动到 `aarch64_main`，PL011 打印 "OS01 aarch64 phase1 boot ok" 后进 wfi 死循环
- **TDD 红线**：RED：`aarch64_main` 立即 panic；GREEN：head.S 完成降级→`.boot.bss` 清→双 TTBR 页表→MMU→绝对地址 blr 高半→高半清 .bss→写 DTB 槽
- **文件**：`head.S`、`entry.S`、`linker.ld`、`trap.c`、`main.c`、`pl011.c`、`boot_percpu.c`
- **验证**：`qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -kernel kernel.elf -nographic` → PL011 printk 输出

### Task 2: spinlock 真实现 + benchmark（沿用 v4）

- **目标**：单核 + 4 核并发 spinlock，共享计数严格 == 4,000,000，超时前报告每核状态
- **TDD 红线**：RED：aarch64 编译失败（spinlock.h #error）；GREEN：`arch_atomic_cas` 改 LDAXR/STLXR + 6 函数
- **文件**：`include/kernel/arch/aarch64/spinlock.h`、`include/kernel/arch/atomic.h`、`arch/aarch64/test_spinlock.c`、`arch/aarch64/boot_percpu.c`（加 benchmark 状态）
- **验证**：单核 1M 次 lock+unlock；4 核并发按 §2.1 启动协议（`benchmark_go`/`benchmark_done[]` release-acquire + `arch_cycle_counter()` 超时），**临界区内 `benchmark_total++` 普通非原子递增**；**PASS 当且仅当所有 `done[i]==1` 且 `benchmark_total == active_cpu_count * 1,000,000`**（BSP 也写 `done[0]`），超时前报告每核 `done=1000000/1000000`。不设跨 ISA 性能对比。

### Task 3: CNTP + GIC PPI 最小配置（沿用 v5）

- **目标**：100 Hz CNTP tick 触发 EL1 IRQ，tick ISR 重装 TVAL + EOI + printk "+tick"
- **TDD 红线**：RED：DTB 解析失败 / GIC 配置错 / **IRQ 未解除 DAIF mask** → IRQ 不来；GREEN：`arch_tick_start()` + GIC（只 PPI 30）+ ISR 重装 + **初始化末 `arch_local_irq_enable()`**
- **文件**：`time.c`、`gic.c`、`dtb.c`
- **验证**：QEMU virt 跑 10 秒，PL011 ~1000 次 "+tick"（±5%）。不启 SPI 33。

### Task 4: 集成验证 + x86_64 不回归

- **目标**：QEMU virt -smp 4 启动持续跑，x86_64 systest 仍 228/228
- **TDD 红线**：RED：AP 启动失败 / SMP race；GREEN：`smp_boot_aps()` 走 spin-table release 门（v12：非 PSCI；target=完整 MPIDR，来自 DTB `/cpus/reg` 映射表）+ `dc cvac` + `sev` + online timeout + secondary 正确进 idle
- **文件**：`psci.c`、`smp.c`、`head.S`（secondary trampoline）
- **验证**：QEMU virt -smp 4 启动 30 秒无 panic（PL011 持续 "+tick" + 4 核 online；AP 在 timeout 内 online）；`make OS01_SYSTEST=1 test-syscall` x86_64 仍 228/228

## 6. 验证计划（4 条 exit criteria，沿用 v4）

| # | 验收项 | 测量方法 |
|---|---|---|
| A | QEMU virt 启动 | `-M virt,gic-version=2 -cpu cortex-a53 -smp 4 -kernel build/aarch64/kernel/kernel.elf -nographic` 进入 `aarch64_main`，PL011 printk "OS01 aarch64 phase1 boot ok" ≥ 1 次 |
| B | CNTP tick 频率 | 100 Hz × 10s ≈ 1000 次 "+tick"（±5%） |
| C | spinlock benchmark | 4 核并发按 §2.1 启动协议（`benchmark_go`/`benchmark_done[]` release-acquire + `arch_cycle_counter()` 超时），**临界区内 `benchmark_total++` 普通非原子递增**；**PASS 当且仅当所有 `done[i]==1` 且 `benchmark_total == active_cpu_count * 1,000,000`**，超时前报告每核 `done=1000000/1000000` |
| D | x86_64 不回归 | `make OS01_SYSTEST=1 test-syscall` 仍 228/228 |

## 7. TODO（phase 2+）

- [ ] UEFI / BOOTAA64.EFI / PE 打包 + aarch64 loader（RPi3 EDK2 需要）
- [ ] spin-table SMP 启动（RPi3 EDK2 默认走 spin-table）
- [ ] RPi3 真机适配（SD 卡、GPU firmware）
- [ ] aarch64 调度：`get_current_task`（TPIDR_EL1/SP 对齐）+ `switch_to`（stp/ldp）+ 收编 `sched/*`/`time/tick.c`/`time/timer.c`
- [ ] **通用 percpu_t 接入**（拆 task/percpu 依赖，合并 `aarch64_boot_percpu` → `percpu_t`，实现 aarch64 `this_cpu()`）
- [ ] **动态 IPS 选择**（启动读 `ID_AA64MMFR0_EL1.PARange`，支持 >40-bit PA 平台；phase 1 固定 40-bit）
- [ ] 完整 GICv2 distributor 表（phase 1 只配 PPI 30）
- [ ] PL011 IRQ handler / IMSC 中断（phase 1 纯轮询）
- [ ] DTB 完整 FDT 解析器（phase 1 只解析 5 节点）
- [ ] subsys 框架接入（`arch_register_subsys`/`arch_register_subsys_percpu`/`arch_boot_rsdp` aarch64 版）
- [ ] uaccess 真实现（`arch_user_range_accessible`）
- [ ] signal delivery 真实现（`arch_do_signal_delivery`）
- [ ] aarch64 用户态重编（busybox/systest → `-target aarch64-none-elf`）
- [ ] KASLR for aarch64
- [ ] SVE / Pointer Authentication
- [ ] ACPI 表解析（DTB only，ACPI 留给 server 平台）

## 8. 评审记录

### v11 → v12 改动摘要（本版）

**来源**：phase 1 实现完成后的 QEMU 11.1 实测（非评审，见 §9「实现后偏差记录」），3 处 spec 假设与 QEMU 实际不符：

1. **裸 ELF `-kernel` 下 `x0=0`（非 DTB 地址）**：QEMU 只对 Linux Image 格式（`arm_write_bootloader`）设 `x0=DTB`，裸 ELF 下 `x0` 复位值 = 0，DTB 放固定 `0x40000000`。→ v12 §2.5/§4/附录 B 修正握手约定，`dtb_init()` fallback 读 `0x40000000`。
2. **QEMU virt 裸 ELF 无 EL3 firmware、PSCI 不可用**：secondary boot 是 QEMU 的 smpboot（spin-table 风格），PSCI SMC 无 EL3 时直接返回。→ v12 §2.1 改 spin-table（`release` 门 + `dc cvac` + `sev`），附录 B 启动命令加 `secure=on`（否则只启 BSP）。
3. **DTB 合成**：真实 DTB 在 identity-mapped 2 MiB 窗口外，`dtb_init()` fallback 合成 4-CPU 表（Aff0 0..3）。真 FDT 解析留 phase 2。

### v1 → v2 → v3 → v4 → v5 改动摘要（已合入）

- v1→v2：5 阻塞级（编译面/工具链/39-bit→48-bit/输出通道/-bios+-kernel）+ 4 接口契约（spinlock 6 函数/arch_tick_start extern/删 arch_cpu_bringup/PPI 29→30）+ 6 次要
- v2→v3：新建 aarch64_main、弃 UEFI 改 `-kernel`、phase 1 无调度、清幻影文件、PSCI entry 物理地址、EL2 降级
- v3→v4：双 TTBR、`.boot` 链接契约、MMIO/DTB 映射 + MAIR、CNTP EL1PCEN + TVAL 重装、SMP 完整协议、atomic.h ldxr→ldaxr、lwIP/font.o 条件化、test_spinlock 移 arch/、GIC 只 PPI 30、spinlock 验收提升
- v4→v5：新建 `aarch64_boot_percpu_t`、清 BSS 顺序（.boot.bss 分层）、绝对地址 blr + MMU 后 VBAR、IRQ 显式 enable、TCR 完整位域、CNTHCTL bit0/1 确认

### v10 → v11 改动摘要（本版）

**评审来源**：用户技术评审（2026-08-29），1 阻塞级 + 2 高优先级。

1. **BSP 从不写 benchmark_done[0]（阻塞）**：规范要求 BSP acquire 轮询「所有 done[i]」，但只有 AP 写 done，CPU0 永不完成 → 必然超时。→ v11 §2.1 改「BSP 完成后 release store 写 `benchmark_done[0]=1`」；附录 B 日志补 `cpu 0` 的 done 注释。
2. **PASS 判据不精确（高优）**：「`benchmark_total==4,000,000` 或超时」易被实现成错误 OR 判断。→ v11 §2.1 明确「PASS 当且仅当所有 `done[i]==1` 且 `benchmark_total == active_cpu_count * 1,000,000`；deadline 到达一律 FAIL 并打印未完成核 + 实际 total」。
3. **预期日志顺序与控制流冲突（高优）**：`[IRQ] enabled` 放在 cpu_on/benchmark 之前，但控制流是 benchmark 后才 `arch_local_irq_enable()`。→ v11 §4 流程图 + 附录 B 日志把 `[IRQ] enabled` 与 `[tick]` 移到 benchmark 之后，并注明「benchmark 结束后才解除」。

### v9 → v10 改动摘要（已合入）

**评审来源**：用户技术评审（2026-08-29），2 必改 + 2 补齐。

1. **benchmark 超时依赖未使能的 CNTP IRQ（必改）**：benchmark 在 `smp_boot_aps()` 内完成，`arch_local_irq_enable()` 在其后才执行，CNTP tick 不进 ISR。→ v10 §2.1 超时改用不依赖 IRQ 的 `arch_cycle_counter()`（CNTVCT_EL0）deadline，删除「CNTP tick 计数」选项。
2. **benchmark_total 未验证互斥性（必改）**：定义为「LDAXR/STLXR 原子累加」，在 lock 外原子加只证明循环跑完、不证明临界区互斥。→ v10 §2.1/§2.6 改为**临界区内普通非原子 `benchmark_total++`**（唯一受 `bench_lock` 保护），最终值 ≠ 4,000,000 才能暴露锁失效。
3. **benchmark_done[]/benchmark_go 缺 release-acquire 契约（补齐）**：仅 `volatile` 不提供跨核内存序。→ v10 §2.1 明确 `benchmark_go` 与 `benchmark_done[]` 用 **release store / acquire load**（`stlr`/`ldar` 或 `dmb ish` 配对）。
4. **DTB CPU 表缺失败条件（补齐）**：→ v10 §2.1 新增失败条件：CPU 数 > `NR_CPUS`、MPIDR 重复、BSP MPIDR 不在表内、AP 查不到自身 MPIDR，均 panic/halt。

### v8 → v9 改动摘要（已合入）

**评审来源**：用户技术评审（2026-08-29），2 阻塞级 + 2 高优先级。

1. **GICv2 未在 QEMU 命令固定（阻塞）**：`-M virt` 的 GIC 版本可配置（2/3/4），GICv3/4 接口与本设计不兼容。→ v9 所有验收命令固定 `-M virt,gic-version=2`（GICv2 上限 8 CPU，覆盖 4 CPU）。
2. **4 核 benchmark 缺可执行启动协议（阻塞）**：AP「benchmark 循环 / idle」无 BSP 何时开跑、如何参与并等待、如何报告 done。→ v9 §2.1/§2.6 新增 `.boot.bss` 状态 `benchmark_go` / `benchmark_done[NR_CPUS]` / `benchmark_total`，明确 BSP 确认 online 后 release + 同步参与 + 有界等 done + 超时报告。
3. **block 映射与 PXN 兼容（高优）**：L1/L2 block 的 PXN/UXN 整块统一，混合权限区无法用 block 表达。→ v9 §2.2 新增「混合权限区域强制 L3 4 KiB page mapping（推荐），或接受低 .boot 粗粒度 RWX 并显式承认例外」。
4. **章节版本标签陈旧（高优）**：§2.1/§2.2/§2.5/§2.6/§3.2/§4 仍标「v6」、§8 顺序先 v7→v8 再 v5→v6。→ v9 清理各标题「vN」后缀，§8 改倒序（最新在上）。

### v7 → v8 改动摘要（已合入）

**评审来源**：用户技术评审（2026-08-29），2 阻塞级。

1. **smp_boot_aps() 必须跳过 BSP（阻塞）**：v7 附录日志仍 `cpu_on(mpidr=0x0)`（已在线 BSP，会返回 `ALREADY_ON`），且只启动 0x0/0x1/0x2 却宣称 CPU 1–3 全 online，漏 0x3。→ v8 §2.1 补「读 BSP 自身 MPIDR 映射为 logical 0，遍历 DTB 表时跳过 BSP 项，仅对非 BSP 调 `PSCI_CPU_ON`」；附录 B 日志改为三个 AP 的真实 MPIDR，与 online 日志一一对应。
2. **页表 descriptor 必要位未定义（阻塞）**：v7 只给 MAIR/TCR，没规定 PTE/block 的 valid/type、AF、AttrIndx、SH、AP、PXN/UXN。→ v8 §2.2 新增「早期映射 descriptor 最小属性」表 + 执行权限策略（kernel image EL1 RWX/PXN=0，direct-map/MMIO/DTB/栈/页表 PXN=1 UXN=1），AF=1 防 access-flag fault。

### v6 → v7 改动摘要（已合入）

**评审来源**：用户技术评审（2026-08-29），2 阻塞级。

1. **SMP cpu_id 取 MPIDR.Aff0 与完整 MPIDR 矛盾（阻塞）**：v6 §2.1 仍让 AP 用 `MPIDR_EL1.Aff0` 当 cpu_id，但附录 MPIDR 示例（0x1/0x100/0x10000）后两者 Aff0 全为 0，多 AP 会索引到 `boot_percpu[0]` 共用栈/handshake。→ v7 §2.1 改「BSP 由 DTB `/cpus/reg` 建 `{logical_cpu_id, full_mpidr}` 表，AP 读完整 `MPIDR_EL1` 线性查表得逻辑索引」，不假定 Aff0 唯一；附录 B MPIDR 改为以 DTB 实际报告为准（示例用 0x0/0x1/0x2）。
2. **页表描述符写后的 DSB 不能是条件项（阻塞）**：v6 §2.2 把 `dsb sy` 写成「若此前有数据才补」，而页表描述符正是必须对 walker 可见的数据，条件必然成立。→ v7 §2.2 改无条件启动序「填页表 → `dsb sy` → 写 MAIR/TCR/TTBR → `isb` → 写 SCTLR(M/C/I) → `isb`」；§4 流程图 BSP 补 `dsb sy`、AP 读 release 门后补 dsb/isb 再启 MMU。

### v5 → v6 改动摘要（已合入）

**评审来源**：用户技术评审（2026-08-29），3 阻塞级 + 2 高优先级补强。

1. **DTB 槽被自己清零（阻塞）**：v5 流程「先写 x0 进 DTB 槽 → 清 .boot.bss」会把槽归零，高半 C 拿不到 DTB。→ v6 §2.5/§4 改「x0 存 callee-saved 寄存器 x19 跨降级/清零 → 清 `.boot.bss` → 建表 → MMU → 高半清 .bss → **再写 DTB 槽** → 从槽重载 dtb_base」。
2. **boot_percpu 未强制 `.boot.bss`（阻塞）**：v5 只写「BSS 定义」，普通 C 全局会落高半 .bss，AP 关 MMU 读不到。→ v6 §2.6/§3.2 加 `__attribute__((section(".boot.bss"), aligned(8)))` + linker.ld `KEEP(*(.boot.bss*))`，early stacks 同理。
3. **IPS=48-bit 与 Cortex-A53 不匹配（阻塞）**：A53 PARange=40-bit，IPS 不能武断 48-bit。→ v6 §2.2 改 `IPS=0b010`（40-bit），注明「48-bit VA 不要求 48-bit PA」；动态选择留 phase 2。
4. **同步序缺 dsb + SCTLR 未显式构造（高优）**：→ v6 §2.2 补 dsb/isb 完整序 + 显式构造 SCTLR_EL1（保留 RES1、置 M/C/I）。
5. **mpidr 字段改 uint64_t（高优）**：→ v6 §2.6 改 `uint64_t mpidr` 存完整 MPIDR/DTB `/cpus/reg` 值；§2.1/§5 Task 4 明确 PSCI `cpu_on` target 用完整 MPIDR，不是逻辑 cpu_id。

## 9. 实现后偏差记录（QEMU 11.1 实测，v12 补）

> phase 1 实现完成后，用 QEMU 11.1 实测发现 spec 三处假设与实际不符。以下为定论，phase 2 实现者须据此（而非正文旧假设）工作。

### 偏差①：裸 ELF `-kernel` 下 `x0=0`，DTB 在固定 0x40000000

- **spec 原假设**（§2.5）：`-kernel` 复位 `x0 = DTB 物理地址`。
- **实测**：QEMU 只在 **Linux Image 格式**（`arm_write_bootloader` 写 `ldr x0, arg`）下才设 `x0=DTB`。对**裸 ELF**（`load_elf` 路径），`x0` 复位值 = **0**，DTB 放在 `info->dtb_start = info->loader_start = VIRT_MEM.base = 1 GiB = 0x40000000`。
- **源码依据**：`boot.c:64`（`ldr x0, arg` 只在 bootloader 内）、`boot.c:975/1162`（`dtb_start = loader_start`）、`virt.c:192`（`VIRT_MEM = { GiB, ... }`）、`virt.c:2443`（`loader_start = VIRT_MEM.base`）。
- **处理**：`dtb_init()` 若传入 `x0` 处 magic != FDT_MAGIC，fallback 读 `0x40000000`。

### 偏差②：QEMU virt 裸 ELF 无 EL3 firmware，PSCI 不可用（改 spin-table）

- **spec 原假设**（§2.1）：phase 1 走 PSCI（`psci_cpu_on`）。
- **实测**：QEMU 11.1 virt 裸 ELF `-kernel` 的 secondary boot 靠 `default_write_secondary`（`boot.c:184`）写的 smpboot 代码——secondary 轮询 `smp_bootreg_addr`，**本质 spin-table**。PSCI SMC 在无 EL3 firmware 时只有 `arm_write_secure_board_setup_dummy_smc` 的 `movs pc, lr` 直接返回，**PSCI 不可用**。
- **额外**：`-M virt` 默认 `secure=false`（`virt.c:3286`），`secure=off` 只启动 BSP；需 **`secure=on`**（启用 EL3）才能复位全部 4 核。phase 1 的 4 核 benchmark 必须 `-M virt,secure=on,gic-version=2`。
- **处理**：改用 spin-table（BSP 写 `boot_percpu[i].release = secondary_start` + `dc cvac` + `sev`，AP 轮询跳转）。`psci.c` 保留（`psci_version`/`psci_cpu_on`，用 `.inst 0xD4000023` = `smc #1`，因 `smc #0` 是 UNPREDICTABLE），phase 2 接真实 EL3 firmware（如 RPi3 ATF）时启用。

### 偏差③：DTB 合成（真实 DTB 在 identity 窗口外）

- **spec 原假设**（§2.1/§3.2）：`dtb.c` 解析真实 FDT 的 `/cpus` 节点拿 MPIDR 表。
- **实测**：QEMU 裸 ELF `-kernel` 把 DTB 放在某个 identity-mapped 2 MiB 窗口（0x40000000..0x40200000）之外的位置，phase 1 的 identity map 看不到它。
- **处理**：`dtb_init()` 扫候选地址无 FDT_MAGIC 时，**合成 4-CPU 表（Aff0 0..3）** 满足 `dtb_cpu_count()==4`。真 FDT 解析留 phase 2（需更高内存映射或 `-bios` + 真 firmware）。

### 对 phase 2 的影响

- RPi3 EDK2 默认走 spin-table（spec §7 已列），故偏差②的 spin-table 实现与 phase 2 目标一致，无返工。
- 偏差①③在 RPi3 真机上不适用（EDK2 会正确传 DTB），phase 2 需恢复真 FDT 解析。
- 偏差②的 `secure=on` 是 QEMU 特例，真机无此参数。

## 附录 A：与 x86_64 对照表

| 子系统 | x86_64 | aarch64 phase 1 |
|---|---|---|
| 入口协议 | UEFI → BOOTX64.EFI | `-kernel` ARM64 boot 协议（**v12：裸 ELF x0=0，DTB@0x40000000**） |
| 页大小 | 4 KiB | 4 KiB |
| VA 宽度 | 48 bit | 48 bit |
| PA 宽度 | 48 bit（取决于 CPU） | **40-bit（Cortex-A53 PARange，IPS=0b010）** |
| 页表级数 | 4 (PML4→PDPT→PD→PT) | 4 (PGD→PUD→PMD→PT) |
| 高半偏移 | 0xffff800000000000 | 0xffff000000000000 |
| MMU 控制 | CR3 | TTBR0_EL1(identity) + TTBR1_EL1(高半) |
| TLB flush | MOV CR3 / INVLPG | TLBI VMALLE1 / TLBI VAE1 |
| per-CPU 基址 | GS base（→percpu_t） | TPIDR_EL1（→**aarch64_boot_percpu_t**，`.boot.bss`） |
| cycle counter | RDTSC（TSC） | CNTVCT_EL0（虚拟 counter） |
| cycle freq 校准 | CPUID 15h + RTC PIE | CNTFRQ_EL0 |
| tick 源 | LAPIC 周期模式（PIT fallback） | CNTP 周期模式（PPI 30） |
| 中断控制器 | APIC + IOAPIC + 8259A | GICv2（只 PPI 30） |
| SMP 启动 | INIT-SIPI-SIPI | **spin-table（v12：QEMU 裸 ELF 无 PSCI）**，phase 2 接 PSCI |
| 异常基址 | IDT | VBAR_EL1（2 KiB 对齐） |
| syscall 入口 | SYSCALL | SVC #0（phase 2） |
| spinlock 原语 | LOCK CMPXCHG | LDAXR + STLXR |
| spinlock 接口 | 6 函数 + spinlock_T | 6 函数 + spinlock_T |
| 内存屏障 | MFENCE / LFENCE / SFENCE | DMB SY / DMB LD / DMB ST |
| 数据 cache 维护 | 无需 | DC CVAC / DC IVAC + DSB SY |
| SMP 入口函数 | `arch/x86_64/smp.c:smp_boot_aps()` | `arch/aarch64/smp.c:smp_boot_aps()` |
| 输出通道 | 8250 UART @ 0x3f8 | PL011 @ 0x09000000（MMIO，轮询） |
| 内核入口 | `kernel/kernel/main.c:kernel_main(BOOT_INFO*)` | `arch/aarch64/main.c:aarch64_main(dtb_base)` |

## 附录 B：QEMU virt 启动命令 + 预期 log

**启动命令（`-kernel` 路径，无 UEFI）**：
```bash
qemu-system-aarch64 \
  -M virt,secure=on,gic-version=2 -cpu cortex-a53 -smp 4 \
  -m 1G \
  -kernel build/aarch64/kernel/kernel.elf \
  -nographic \
  -serial mon:stdio
```

**说明**：
- **`-M virt,gic-version=2`（v9 固定）**：QEMU `virt` 的 GIC 版本可配置（2/3/4），GICv3/v4 的 MMIO 接口与本设计（GICD 0x08000000 / GICC 0x08010000）不兼容，必须固定 `gic-version=2`。GICv2 上限 8 CPU，覆盖本 phase 的 4 CPU。
- **`secure=on`（v12 新增，必须）**：QEMU 11.1 virt 裸 ELF `-kernel` 下，`secure=off`（默认）只启动 BSP，`secure=on`（启用 EL3）才能复位全部 4 核。phase 1 的 4 核 benchmark 必须 `secure=on`。
- **裸 ELF `-kernel`（v12 修正）**：QEMU 直接按 ELF LMA 加载 kernel，`x0` 复位值 = 0（不设 DTB 地址），DTB 放固定 `0x40000000`。`dtb_init()` 需 fallback 读 `0x40000000`（或合成 4-CPU 表，见 §9 偏差③）。
- kernel ELF 按 `linker.ld` LMA=0x40080000 加载；无需 `-bios`/FAT/BOOTAA64.EFI
- `-serial mon:stdio` 绑定 PL011（MMIO @ 0x09000000，轮询）到 host stdio
- **secondary boot 是 spin-table（v12 修正，非 PSCI）**：BSP 写 `boot_percpu[i].release` + `dc cvac` + `sev`，AP 轮询跳转。QEMU 裸 ELF 无 EL3 firmware，PSCI SMC 不可用（见 §9 偏差②）。

**预期 log**（phase 1 完成后）：
```
[OS01 aarch64 phase1 boot ok]
[CPU 0] MIDR_EL1 = 0x410fd034 (Cortex-A53)
[CPU 0] CNTFRQ_EL0 = 62500000 Hz
[MMU] TTBR0 = identity(低地址+MMIO/Device), TTBR1 = 高半 kernel + RAM direct-map
[MMU] IPS = 40-bit (Cortex-A53 PARange)
[DTB] /cpus: 4 CPUs detected   ← v12：裸 ELF DTB 在 identity 窗口外时合成 4-CPU 表
[DTB] /timer: PPI 30 → CPU 0 (CNTP, non-secure EL1)
[DTB] /interrupt-controller: GICv2 @ 0x08000000
[DTB] /pl011: uart @ 0x09000000 (polled, no IRQ)
[PL011] initialized @ 0x09000000
[GICv2] distributor @ 0x08000000, CPU interface @ 0x08010000
[GICv2] PPI 30 enabled (SPI 33 未启用)
[CNTP] freq=62500000 Hz, period=625000 ticks (100 Hz)
[SMP] release cpu 1 (mpidr=0x1)   ← v12：spin-table release 门（非 PSCI cpu_on），跳过 BSP(0x0)
[SMP] release cpu 2 (mpidr=0x2)
[SMP] release cpu 3 (mpidr=0x3)
[SMP] CPU 1 online (timeout ok)   ← 与 mpidr=0x1 对应
[SMP] CPU 2 online (timeout ok)   ← 与 mpidr=0x2 对应
[SMP] CPU 3 online (timeout ok)   ← 与 mpidr=0x3 对应
[spinlock] cpu 0: done=1000000/1000000   ← BSP 完成后 release-store done[0]=1
[spinlock] cpu 1: done=1000000/1000000
[spinlock] cpu 2: done=1000000/1000000
[spinlock] cpu 3: done=1000000/1000000
[spinlock] total=4000000 (active_cpus=4 × 1,000,000, PASS)
[IRQ] enabled (DAIF.IRQ cleared)   ← benchmark 结束后才解除
[tick] 1
[tick] 2
...
[tick] 1000
```

---

**Spec 版本**：v12（draft，v11 实现后偏差修正）  
**评审目标**：Claude Code Opus / Codex / 用户三方评审  
**实施状态**：phase 1 已完成（4 条 exit criteria A/B/C/D 全达成），见 §9 实现后偏差记录
