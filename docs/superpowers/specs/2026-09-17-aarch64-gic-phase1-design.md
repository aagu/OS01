---
title: OS01 aarch64 GICv2 通用中断框架 Phase 1 设计
created: 2026-09-17
type: spec
status: draft-v1（待 codex 评审）
version: 1
tags: [osdev, aarch64, gic, interrupt, kernel]
phase: GICv2 Phase 1（QEMU virt UEFI 路径）
related: [2026-08-29-aarch64-phase1-design, os01-roadmap P2]
---

# OS01 aarch64 GICv2 通用中断框架 Phase 1 设计

> **目标**：把 aarch64 现状"单一 CNTP PPI 30 硬编码 ISR"泛化为一个通用 GICv2 中断框架——
> (1) 寄存器访问抽象 + SGI/PPI/SPI 分类 + enable/disable + handler 注册表；
> (2) entry.S 全量通用寄存器 save/restore + aarch64 `pt_regs_t`（对齐 x86_64 facade 模式）+ 通用 IRQ dispatch；
> (3) 一个真实 SPI 中断源（PL011 RX，INTID 33）走 handler 表；
> (4) SGI/IPI：GICD_SGIR 发送 + 跨核 handler，SMP 1/2/4 验证；
> (5) 架构范式与 x86_64 中断路径镜像对齐（但不编 kernel core，不进调度器）。
>
> 衔接 `docs/superpowers/specs/2026-08-29-aarch64-phase1-design.md`（下称"phase1 spec"）：
> 该 spec 交付了 QEMU virt 上的 UEFI 启动 + SMP(PSCI) + CNTP 100 Hz tick + 最小 GICv2 配置（只开 PPI 30），
> 并在 §0"明确不做"中把"完整 GICv2 驱动"推迟——本 spec 就是那一项的兑现。

---

## 1. 背景与动机

### 1.1 现状一句话

aarch64 侧的中断路径是**教学式最小实现**：`entry.S` 的 EL1h IRQ 槽不保存任何通用寄存器，
直接 `bl el1_irq_dispatch`（kernel/arch/aarch64/entry.S:78）；`el1_irq_dispatch` 在
time.c 里把 INTID 与 `dtb_cntp_ppi()` 硬编码比较（kernel/arch/aarch64/time.c:100-115），
除 PPI 30 之外任何中断都被当"misconfiguration"打日志后丢弃。没有 handler 注册表，
没有 SPI 通路，没有 SGI/IPI，没有 `pt_regs_t` 落地（结构体已声明但从未填充）。

### 1.2 为什么要现在做

- **roadmap §P2（docs/roadmap.md:52-70）** 的中期目标是"中断/异常 dispatch 统一"：
  `arch_irq_dispatch` 单入口抽象，x86_64 IDT 与 aarch64 VBAR_EL1 各自封装。
  x86_64 侧的 weak-default + strong-override 钩子（kernel/include/arch/irq.h:31-47）
  早已落地并在 roadmap 标 ✅，aarch64 侧只剩一个 20 行的占位 trap.c
  （kernel/arch/aarch64/trap.c:17-20）。本 Phase 把 aarch64 侧的中断骨架补齐到
  "能对齐 x86_64 范式"的形态，为 Phase 2（tick 接 kernel core jiffies、syscall/EL0、
  调度器集成）铺路。
- ** SMP 已就位但无法跨核通信**：PSCI CPU_ON、per-CPU 槽、spinlock benchmark 全部可用
  （kernel/arch/aarch64/smp.c:127-202），但 AP 上线后 `secondary_idle` 直接
  `for(;;) arch_cpu_halt()`（smp.c:225），DAIF 全掩（head.S:585/609）——没有 IPI 通路，
  后续 TLB shootdown / 抢占调度 / RCU 式同步都无从谈起。
- ** entry.S 零保存是最大单点风险**：当前能跑纯粹是因为被打断的代码
  （`aarch64_main` 末尾的 `for(;;) arch_cpu_halt()`，main.c:263）不依赖跨中断点的
  caller-saved 寄存器。任何后续内核代码（调度器、syscall、甚至更复杂的打印路径）
  一旦在 x0-x18 里持有活值就会静默损坏。这个雷必须在铺更多功能前排掉。

### 1.3 与 2026-08-29 phase1 spec 的关系

| phase1 spec 交付 | 本 spec 复用/改造 |
|---|---|
| UEFI pflash 启动链（BOOTAA64.EFI + kernel.elf FAT 镜像） | 原样复用（构建路径不变） |
| 16 槽 VBAR_EL1 向量表（entry.S） | EL1h IRQ 槽从"零保存 bl"改为全量 save/restore + `b el1_irq_entry` |
| DTB 校验（gicd=0x08000000 / gicc=0x08010000 / pl011=0x09000000 / cntp_ppi=30） | 原样复用，新增 pl011 `interrupts` 解析出 SPI 33 |
| 最小 GICv2（只开 PPI 30） | 泛化为通用 driver + handler 表 |
| CNTP 100 Hz tick ISR（硬编码 intid） | 改为注册进 handler 表的第一个消费者 |
| SMP PSCI + spinlock benchmark | 原样复用；AP 尾循环改为开 IRQ 以接收 IPI |
| 测试通路 `test-aarch64-uefi-smp` | 扩展 `--expect-gic` 断言 + 新增 SPI 注入 harness |

phase1 spec §2.3 定下的 ISR 顺序契约（先重装 TVAL、再 EOI、最后打印）在本 Phase 保持不变，
只是搬进注册的 tick handler 里。

---

## 2. GICv2 硬件模型（QEMU `virt,gic-version=2` 视角）

实现参考 ARM IHI 0048B（GICv2）。本节只列 Phase 1 用到的语义，不追求完备。

### 2.1 两级结构

```
        CPU0 GICC    CPU1 GICC    CPU2 GICC ...
            ^            ^            ^
            | (banked 视角) |            |
        +---+------------+------------+---+
        |        GIC Distributor (GICD)     |
        |  全局 SPI 状态 + 每核 banked SGI/PPI |
        +------------------------------------+
```

- **GICD（Distributor）**：全局配置 + 路由。GICD_CTLR bit0 = 全局使能（gic.c:34 已开）。
  SGI/PPI 的 IGROUPR/IPRIORITYR/ISENABLER 等"每 INTID 寄存器组"是 **banked** 的——
  每个 CPU 接口看到自己的一份（这正是 `gic_cpu_init()` 在每个核上各跑一次的原因，
  BSP 在 gic.c:35，AP 在 smp.c:208）。
- **GICC（CPU Interface）**：每核私有。GICC_PMR 优先级掩码（当前 0xff，gic.c:22）、
  GICC_CTLR bit0 使能（gic.c:23）、GICC_IAR 应答读、GICC_EOIR 完成写。

### 2.2 INTID 分类（Phase 1 需要的完整分类函数）

| 区间 | 类型 | banking | 路由 | Phase 1 用途 |
|---|---|---|---|---|
| 0-15 | SGI | 每核 banked | 软件指定（GICD_SGIR） | IPI（G5/G6） |
| 16-31 | PPI | 每核 banked | 物理连线固定到本核 | CNTP tick（PPI 30，dtb_cntp_ppi()） |
| 32-1019 | SPI | 全局 | GICD_ITARGETSR 指定核集合 | PL011 RX（INTID 33） |
| 1020-1021 | 特殊 | — | — | 拒绝 |
| 1022 | 特殊（v2） | — | — | 拒绝 |
| 1023 | spurious | — | — | IAR 读到即静默返回（time.c:102-104 已处理） |

`gic_irq_type(intid)` 返回 `{INVALID, SGI, PPI, SPI}`，是 driver 的公共分类原语。

### 2.3 IAR / EOIR 与"潜伏优先级"

- **IAR 读的副作用是 drop priority**：读 GICC_IAR 把该 INTID 的运行优先级压栈
  （进入 active 状态）。**EOIR 写的副作用是 deactivate**（弹栈）。
  在 IAR 与 EOIR 之间，同优先级及更低优先级的中断被屏蔽——Phase 1 不做中断嵌套，
  dispatch 全程保持 IRQ masked（异常进入时硬件自动 set DAIF.I），这天然安全。
- **EOIR 必须写回"IAR 读到的原始值"**：IAR bits[12:10] 是 CPUID（仅 SGI 有意义），
  EOIR 写值若丢了 CPUID 位，SGI 的 deactivate 会被路由到错误的 CPU 接口。
  **现状缺陷**：time.c:110/119 写 `gicc_write32(GICC_EOIR, intid)` 只回写了低 10 位
  ——对 PPI 恰好无害（PPI 的 CPUID 位为 0），但 SGI 路径是 latent bug。本 Phase 的
  `gic_eoi(dev, iar)` 接收**完整 IAR 值**并原样写回，hosttest 用 mock 断言这一点。
- **handler 在 EOI 之前执行**：对 level 触发的 PL011 RX，handler 里清设备中断源
  （读 DR / 写 ICR）必须发生在 EOI 前，否则 EOI 后同一 level 会立刻再次 pending。
  phase1 spec §2.3 的"TVAL 重装先于 EOI"是同一原理。

### 2.4 GICD_SGIR（软件生成中断）

写 GICD_SGIR（reg.h:39 已有偏移 0xF00）触发 SGI：

```
bit[25:24] TargetListFilter:  0b00 = 用 bits[23:16] 的目标列表
                              0b01 = 发给除自己外的所有核（All others）
                              0b10 = 只发给自己
bits[23:16] CPUTargetList:    Aff0 CPU 接口位图（filter=0b00 时有效）
bits[3:0]   SGIINTID:         SGI 编号 0-15
```

IPI 测试用 filter=0b01（All others）：一次写广播所有 AP，BSP 自身不收，天然对称。

### 2.5 Phase 1 用到的 GICD 每芯 INTID 寄存器组

| 寄存器 | 偏移 | 密度 | 语义 |
|---|---|---|---|
| GICD_IGROUPR | 0x080 | 1 bit/INTID | 组 0（secure）/组 1。QEMU 无 EL3，统一清 0（沿用 gic.c:16 的做法） |
| GICD_ISENABLER | 0x100 | 1 bit/INTID | 写 1 使能（写 0 无副作用） |
| GICD_ICENABLER | 0x180 | 1 bit/INTID | 写 1 禁用 |
| GICD_ISPENDR | 0x200 | 1 bit/INTID | **软件置 pending**——测试注入用（见 §7.4） |
| GICD_IPRIORITYR | 0x400 | 1 byte/INTID | 值越小优先级越高；Phase 1 统一 0x00 |
| GICD_ITARGETSR | 0x800 | 1 byte/INTID | SPI 目核位图（bit0=cpu0...）；SGI/PPI 只读 banked |
| GICD_TYPER | 0x004 | RO | bits[4:0] ITLinesNumber：`(v+1)*32` = 实现的总 INTID 线数 |

以上偏移全部已在 `kernel/arch/aarch64/reg.h:27-39` 定义（含未用过的 ICENABLER/ISPENDR/
ITARGETSR/SGIR）——**driver 不需要新增任何寄存器常量**，只需要不再经由 reg.h:70-88
那组**硬编码基址**的 inline 访问器（见 §3 缺口 D4）。

---

## 3. 现状代码审计（真实 line number）

### 3.1 kernel/arch/aarch64/gic.c（43 行）

| 行号 | 现状行为 |
|---|---|
| 9-25 | `gic_cpu_init()`：banked PPI 配置——IGROUPR 清组位（16）、IPRIORITYR 清优先级字节（17-19）、ISENABLER 置位（21）、GICC PMR=0xff + CTLR=1（22-23）、`dsb sy; isb`（24）。**只处理 CNTP intid 一个中断**，函数名暗示通用实则专用 |
| 13-14 | 基址来自 `dtb_gicd_base()/dtb_gicc_base()`——DTB 校验过（见 §3.6） |
| 27-43 | `gic_init()`：GICD_IIDR==0 则打日志 `wfi` 死循环（30-33）；GICD_CTLR=1（34）；调 gic_cpu_init（35）；打印基址与 PPI 号，**并明说 "SPI 33 NOT enabled"（42）** |
| — | 无 handler 注册、无 SPI/SGI 能力、无 IAR/EOIR（那两个在 time.c 里） |

### 3.2 kernel/arch/aarch64/entry.S（117 行）

| 行号 | 现状行为 |
|---|---|
| 35-39 | `.section .text.entry`，`.balign 0x800`，`exception_vectors` 16 槽表 |
| 41-116 | 其余 15 个槽全部 `b .` 死循环（43/47/51/55/59/63/67/71/83/87/91/95/99/103/107/111/115 各槽） |
| 61-79 | EL1h IRQ 槽（偏移 +0x280）：注释明说 "We don't need to save any additional state in phase 1"（64-66）；`bl el1_irq_dispatch`（78）+ `eret`（79）。**零通用寄存器保存**——依赖 AAPCS64 callee-saved 约定（x19-x28 由 C 被调方保存）+ 硬件 ELR/SPSR/SPSR 自动保存；x0-x18（caller-saved）在异步中断点持有的任何活值都会被 C dispatch 静默毁掉 |
| 74-75 | 注释承认 `bl` 会毁 LR，靠 `eret` 读 ELR_EL1 兜底——正确但仅在最窄的意义上 |

`main.c:188-189` 在 PL011 初始化后用 `msr vbar_el1` 安装本表（BSP）；
AP 在 head.S:660-662 安装同一张表（MMU 打开后）。

### 3.3 kernel/arch/aarch64/time.c（133 行）

| 行号 | 现状行为 |
|---|---|
| 32-33 | `HZ=100`、`TICKS_PER_SECOND` |
| 39 | `static volatile uint64_t g_ticks`——每 tick ++，每 100 次（每秒）打印 `[tick] N`（127-131）。这是 SMP harness 的终止判据之一（aarch64_uefi_smp.py:348 要求 ≥3 个 `[tick]` 行） |
| 46-84 | `arch_tick_start()`：CNTFRQ/HZ 算 period、装 TVAL、CNTP_CTL EN=1 IMASK=0、回读校验、打印 `[cntp] freq=...` |
| 96-133 | `el1_irq_dispatch()`：读 IAR（98，**经 reg.h:85-88 的硬编码 GICC_BASE 访问器**）；1023 spurious 返回 0（102-104）；`intid != expected` → EOI 后打 `[gic] unexpected IRQ intid=N`（105-115）；命中 → 重装 TVAL（118）、EOI（119，**只写低 10 位**，见 §2.3）、per-second 打印（125-131） |
| 100 | `expected = dtb_cntp_ppi()`——**handler 逻辑与具体设备耦合**，没有注册机制 |

### 3.4 kernel/arch/aarch64/trap.c（21 行）

- 17-20：`arch_install_exception_vectors()` 是 no-op 占位；注释（4-11）说明这是
  "future home for ESR_EL1/FAR_EL1 decoding"。**本 Phase 把通用 IRQ dispatch 落在这里**，
  sync exception 解码仍留给 Phase 2。

### 3.5 kernel/arch/aarch64/smp.c（226 行）与 head.S（755 行）

- smp.c:204-226 `secondary_idle(cpu_id)`：AP 的 C 入口。208 `gic_cpu_init()`（banked
  PPI/GICC 使能）；209 关本核 CNTP；212-218 等待 `boot_go` 命令；219-220 跑
  spinlock benchmark；224 再次关 CNTP；**225 `for(;;) arch_cpu_halt()`（wfi），且全程
  DAIF 掩死（head.S:585/609 `daifset #0xf`，此后从未清过）**——IPI 需要在这里开
  DAIF.I。
- head.S:580-672 `secondary_start`：EL2→EL1 降级、MPIDR/槽/栈校验、设 TPIDR_EL1
  （650，指向 `aarch64_boot_percpu[cpu_id]` 槽——IPI handler 取本核逻辑号的现成途径）、
  MMU on、换 `exception_vectors`（660-662）、`blr secondary_idle`（666-668）。

### 3.6 DTB 解析（dtb.c 84 行 / dtb_parse.c 279 行）

- dtb.c:16-19 暴露 `dtb_gicd_base()/dtb_gicc_base()/dtb_pl011_base()/dtb_cntp_ppi()`。
- **dtb_parse.c:115-121 并不真正解析 GIC `reg`**：它用 `device_reg(n,0,0x08000000)` /
  `device_reg(n,1,0x08010000)` 校验后**硬编码回填**同样的值（119-120），compatible 只认
  `"arm,cortex-a15-gic"`（115）——即"DTB 校验 + QEMU virt 固定值"，不是通用解析。
- pl011 节点（122-127）同样只校验 reg=0x09000000；**`interrupts` 属性（含 SPI 33 信息）
  目前完全不解析**——本 Phase 补上（INTID = 32 + DTB 第二个 word）。
- timer 节点（128-135）校验 interrupts 形状后固定 `cntp_ppi=30`（134）。

### 3.7 构建/测试通路（真实路径，纠正任务描述里的三处未验证路径）

| 任务描述里的路径 | 实际情况 |
|---|---|
| `mk/components/aarch64.mk` | **不存在**。aarch64 构件规则在 `mk/components/image.mk:96-164`（kernel artifact / QEMU_EFI.fd / aarch64-uefi.img）与 `mk/components/run.mk:104-195`（run/test targets） |
| `mk/qemu.mk` | **不存在**。QEMU 命令全在 `mk/components/run.mk`（aarch64 段 125-148）与 `qemutests/aarch64_uefi_smp.py:387-398` |
| `thirdpart/aarch64/inc/aarch64.h` | **不存在**。`thirdpart/` 下只有 busybox/lwip/mbedtls/posix-uefi。GICv2 寄存器常量的真身在 `kernel/arch/aarch64/reg.h:18-65` |

其余已核实的关键事实：

- `kernel/Makefile:42-43`：aarch64 白名单 `KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c`；
  `kernel/Makefile:70-71` `ARCH_C_SOURCES := $(wildcard $(ARCHDIR)/*.c)` ——**在
  kernel/arch/aarch64/ 下新增的 .c 会自动编进内核**，无需改 Makefile。
  aarch64 **不编** kernel/core、kernel/intr、selftest/systest（kernel/Makefile:44-67）。
- `kernel/arch/aarch64/make.config:9-10`：`clang -target aarch64-none-elf` +
  `ld.lld -m aarch64elf`；16-17 `-march=armv8-a -ffreestanding -mgeneral-regs-only -fno-pie`。
- `mk/components/run.mk:161-172`：`test-aarch64-uefi-smp` 先 `$(MAKE) KERNEL_SELFTEST=1
  aarch64-uefi`（165），再跑 `python3 qemutests/aarch64_uefi_smp.py --cpus 1 2 4 --repeat 3
  --timeout 90 --expect-selftest --diagnostic-dtb=auto ...`（166-172）。
  KERNEL_SELFTEST=1 经 kernel/Makefile:192-194 变成 `-DOS01_SELFTEST=1`——
  **新探针代码的编译门**。
- QEMU 启动形态（run.mk:143-148 / aarch64_uefi_smp.py:387-398）：UEFI pflash 引导
  （`-drive if=pflash,...QEMU_EFI.fd` + FAT 镜像里的 BOOTAA64.EFI），**不是**
  `-kernel boot.bin`；机器型号 `virt,gic-version=2`，CPU cortex-a53，m 512，
  DTB 用 `--diagnostic-dtb=auto` 逐 case 生成（406-439）。
- harness 断言模式（aarch64_uefi_smp.py:263-348 `passed()`）：RAM summary 唯一且算术
  自洽、`[smp] topology/cpum/summary`、逐核 spinlock done、`[smp-test] no_ack_cpu=0`、
  `[tick] N` ≥3 行；`--expect-selftest` 额外要求 RAM 与 topology 行之间恰有一条
  `UEFI-A64: pmm alloc smoke OK` + 一条 `pt map smoke OK`（272-325）。
  `--self-test` 模式（541-562）先跑纯 Python fixture 断言——新断言逻辑必须先在这里红。
- hosttests 注册模式：`hosttests/Makefile` TEST_BINS 列表（55-83 行的 bin 清单）+
  逐 case 的 `.o`/`.elf` 规则 + PHONY 便捷 target（参考 test_lwip_rand 尾部规则）；
  断言宏来自 `hosttests/include/test_framework.h`（assert_true/assert_eq...）。
  hosttests 只能在 repo 根用 `PROFILE=aarch64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk
  make -C hosttests <target>` 直连调用（root 的 `test` target 被 rootfs capability 门挡住，
  run.mk:258）。

### 3.8 x86_64 中断范式（镜像对象，全部只读不改）

| 层 | x86_64 真实位置 | aarch64 Phase 1 对应物 |
|---|---|---|
| pt_regs_t | `kernel/include/arch/x86_64/regs.h:18-44`（字段顺序=INTR_SAVE_ALL 压栈序，注释 14-16 明说契约）；facade `kernel/include/arch/regs.h:25-31` | `kernel/include/arch/aarch64/regs.h:19-28` **已存在但缺 x30**，且无任何 .S 填充它 |
| 向量桩（save/restore） | `kernel/arch/x86_64/irq.c:14-55` Build_IRQ 生成的汇编桩（push 序列 + `jmp arch_irq_dispatch`，rdi=pt_regs*, rsi=vector）；RESTORE_ALL 在 `kernel/arch/x86_64/entry.S:28-49` | entry.S EL1h IRQ 槽 + `el1_irq_entry` save/restore 序列（本 Phase 新增） |
| arch dispatch 钩子 | `kernel/include/arch/irq.h:47` `arch_irq_dispatch(pt_regs_t*, uint64_t hwirq)` 声明；strong 实现在 `kernel/arch/x86_64/irq_hooks.c:65-85`（vector→gsi→irq_table 查 handler→ack） | trap.c `el1_irq(struct pt_regs*)` → `gic_dev_dispatch()`（driver 内，hosttest 可测） |
| 中性 handler 表 | `kernel/include/intr/interrupt.h:27-35` irq_desc_t + 50-52 register_irq；`kernel/intr/dispatch.c:9-10` intr_handler_table[256] | aarch64 不编 kernel/intr——handler 表内嵌在 aarch64 gic driver 里，签名对齐 `(nr, param, regs)` |
| 控制器 ops | hw_int_controller_t（interrupt.h:16-25，enable/disable/install/ack 函数指针） | Phase 1 以直接函数 API 呈现（gic_irq_enable 等），ops 结构体化留给 kernel/intr 接入时（Phase 2+） |
| 中断状态原语 | arch/irq.h:52-66 sti/cli/save/restore | arch/irq.h:88-109 **已存在**（daifclr bit1，注释 79-86 还记载了"I 是 imm 的 bit1 不是 bit2"的经典坑）——原样复用 |

### 3.9 缺口汇总表

| # | 缺口 | 位置 | 后果 |
|---|---|---|---|
| D1 | entry.S 零寄存器保存 | entry.S:61-79 | 任何依赖 x0-x18 跨中断点的代码静默损坏（最大单点） |
| D2 | pt_regs_t 声明但无 x30、无填充者 | include/arch/aarch64/regs.h:19-28 | facade 模式空转；未来 syscall/信号无法落地 |
| D3 | ISR 与设备硬编码耦合 | time.c:96-133 | 加任何新中断源都要改 dispatch 本体 |
| D4 | MMIO 访问器硬编码基址 | reg.h:70-88（time.c:98 在用） | 与 gic.c:13-14 的 DTB 基址路径不一致；不可 mock、不可 hosttest |
| D5 | 无 SPI 通路 | gic.c:42 明说 SPI 33 NOT enabled | PL011 只能轮询输出，RX 中断不可用 |
| D6 | 无 SGI/IPI | 全 aarch64 无 GICD_SGIR 写 | 跨核通信缺失 |
| D7 | EOIR 丢 CPUID 位 | time.c:110/119 | SGI 路径 latent bug（本 Phase 修） |
| D8 | AP 永久掩中断 | head.S:585/609 + smp.c:225 | IPI 无从接收 |
| D9 | trap.c 是 no-op | trap.c:17-20 | 无 dispatch 落点 |
| D10 | dtb 不解析 pl011 interrupts | dtb_parse.c:122-127 | SPI 33 只能硬编码（本 Phase 补解析） |

---

## 4. 目标架构

### 4.1 分层

```
┌────────────────────────────────────────────────────────────┐
│ 消费者：cntp tick handler（time.c）/ SPI 测试 handler /      │
│         ipi handler（新 ipi_test.c）                        │
│   签名统一：void fn(uint32_t intid, uint64_t param,          │
│                     struct pt_regs *regs)                   │
├────────────────────────────────────────────────────────────┤
│ handler 注册表（gic_driver.c 模块级全局）                    │
│   gic_register_handler / gic_get_handler / unregister       │
├────────────────────────────────────────────────────────────┤
│ 通用 dispatch（gic_driver.c::gic_dev_dispatch，hosttest 可测）│
│   IAR 读 → 1023 静默返 → 查表 → handler(intid,param,regs)    │
│   → 无 handler 打 unexpected → EOIR 写回完整 IAR             │
├────────────────────────────────────────────────────────────┤
│ hw 访问层（gic_driver.c，全部经 struct gic_dev 的指针）       │
│   gic_dev_init / dist/cpu enable / irq_config(enable,prio,  │
│   targets) / irq_type 分类 / ack / eoi / send_sgi /         │
│   set_pending(测试注入)                                      │
├────────────────────────────────────────────────────────────┤
│ 生产 wrapper（gic.c 改造）：static struct gic_dev 挂 DTB 基址 │
│   gic_init() / gic_cpu_init() 签名不变（main.c/smp.c 不动）  │
└────────────────────────────────────────────────────────────┘
        ▲ 异常入口：entry.S el1_irq_entry（全量 save/restore）
        ▲ trap.c el1_irq(regs) —— 三行壳，调 gic_dev_dispatch
```

设计规则：

- **hw 层不落任何 UART 日志**（只返回错误码）——这是 hosttest 能直接编译真实源文件的
  前提（镜像 test_lwip_rand 对 kernel/net/lwip_sys_arch.c 的做法）。
- **driver 源文件是 `kernel/arch/aarch64/gic_driver.c`**，头文件
  `kernel/include/arch/aarch64/gic.h`。kernel/Makefile:70 的 wildcard 自动把它编进内核。
- handler 表是**模块级全局、非 per-CPU**（Phase 1 无调度器/无 percpu_t）；SGI 0 的
  handler 在所有核共享同一表项，handler 内部用 TPIDR_EL1 区分收到者。
- 对齐 x86_64 的 `hw_int_controller_t`（interrupt.h:16-25）暂不做函数指针化——那是
  kernel/intr 接入时（Phase 2+）的事；本 Phase 保持直接函数 API，边界见 §6。

### 4.2 entry.S save/restore + 通用 dispatch 流程（文字流程图）

```
IRQ (EL1h, SP_EL1)
 └─ 硬件: ELR_EL1←返回PC, SPSR_EL1←PSTATE, DAIF.I 置位, SP 保持 SP_EL1
 └─ 向量表 +0x280: b el1_irq_entry            （槽内只放这一条，≤0x80 字节约束）
     el1_irq_entry:
       sub  sp, sp, #PT_REGS_SIZE(272)          ; 保持 16 字节对齐
       stp  x0,x1  .. stp x28,x29              ; 15 对 stp，偏移 0..224
       str  x30, [sp,#240]
       mrs  x1, spsr_el1; str [sp,#264]         ; 系统状态
       mrs  x1, elr_el1; str [sp,#256]
       mrs  x1, sp_el0;  str [sp,#248]          ; 无条件保存（EL1h 下 SP_EL0 未用
                                                ;  但保存/恢复无害且为 EL0 铺路）
       mov  x0, sp
       bl   el1_irq                             ; trap.c
         └─ gic_dev_dispatch(&g_gic, regs)
              ├─ iar = GICC_IAR 读              ; 副作用: drop priority
              ├─ intid = iar & 0x3FF
              ├─ intid==1023 → return           ; spurious，绝不写 EOIR
              ├─ fn = gic_get_handler(intid)
              │    ├─ fn → fn(intid, param, regs)   ; 设备清源在 EOI 前（§2.3）
              │    └─ NULL → log "[gic] unexpected IRQ intid=N"（保留现状文案）
              └─ gic_eoi(dev, iar)              ; 写回完整 IAR（含 CPUID）
       ldr/msr 恢复 sp_el0/elr_el1/spsr_el1
       ldr  x30; ldp x28,x29 .. ldp x0,x1
       add  sp, sp, #PT_REGS_SIZE
       eret
```

其余 15 个槽维持 `b .`：sync exception 解码（ESR_EL1/FAR_EL1）是 Phase 2 范围；
EL0 IRQ 槽不接——EL0 来源需要 SP_EL0→SP_EL1 切换语义，本 Phase 无用户态，先不做
（见 §10 non-goals）。

### 4.3 与 x86_64 范式对照（验收口径）

| 范式要素 | x86_64 | aarch64 Phase 1 后 |
|---|---|---|
| pt_regs_t 字段序 = 压栈序 | regs.h:14-16 契约 | regs.h（x0..x30, sp_el0, elr, spsr）+ entry.S stp/str 偏移一一对应，偏移常量在头文件 Section 2 共享给 .S |
| 桩只做 save + 跳 dispatch | Build_IRQ 桩 | el1_irq_entry |
| dispatch 收 (regs, hwirq) | arch_irq_dispatch(regs, vector) | el1_irq(regs) → gic_dev_dispatch（hwirq 在内部 IAR 读出——GIC 的 vector 是 ack 的副产品，这是两架构的本质差异，spec 承认不强行同形） |
| handler 签名 (nr, param, regs) | interrupt.h:33 | gic.h 同形（uint32_t intid 代替 uint64_t vector） |
| spurious 处理 | irq_hooks.c:68-71 | intid 1023 静默返回 |
| unexpected 处理 | gsi 越界 debug_irq（irq_hooks.c:73-77） | 无 handler 打日志 + EOI |
| 中断状态原语 | sti/cli 族 | arch/irq.h:88-109 已有，复用 |

---

## 5. save/restore 寄存器清单与 pt_regs_t 精确布局

### 5.1 保存什么、为什么

| 寄存器 | 保存? | 依据 |
|---|---|---|
| x0-x30（31 个 GPR） | **全部** | 异步中断可落在任意指令边界；AAPCS64 只约束**函数调用**，对中断点无 callee-saved 保障。x0-x17 caller-saved 必然被 C dispatch 毁；x18 是平台寄存器（clang -mgeneral-regs-only 下也保守保存）；x19-x28 虽是 callee-saved，但 dispatch 链上若有手写汇编（eret 路径）无 ABI 兜底，全量保存是唯一便宜的正确解 |
| sp_el0 | **保存** | 为 EL0 来源铺路；EL1h 下未用但恢复无害。若 SPSR 显示来源 EL0（Phase 2），eret 后硬件用 SP_EL0 |
| elr_el1 / spsr_el1 | **软件保存后由 C 侧可观察** | 硬件已自动保存，eret 自动恢复；再入 pt_regs 是为了让 C dispatch/未来信号框架能读/改返回状态（x86_64 的 pt_regs.rip/rflags 同理） |
| tpidr_el1 | 不保存 | 只被启动路径写（head.S:650），handler/dispatch 链不改它 |
| daif | 不显式保存 | 异常进入硬件置 SPSR，eret 恢复；dispatch 全程不嵌套开中断 |
| FPSIMD v0-v31 | 不保存 | kernel 编译带 `-mgeneral-regs-only`（make.config:17），内核路径不碰浮点；EL0 FPSIMD 上下文属 Phase 2+ |
| SCTLR/TTBR 等系统寄存器 | 不保存 | dispatch 不改页表 |

### 5.2 pt_regs_t 布局（字段顺序 = 内存偏移序 = entry.S 写入序）

修改 `kernel/include/arch/aarch64/regs.h:19-28`（现状缺 x30），新布局：

```c
typedef struct pt_regs
{
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;      /*   0 ..  56 */
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;/*  64 .. 120 */
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;/*128 .. 184 */
    uint64_t x24, x25, x26, x27, x28, x29, x30;   /* 192 .. 240 */
    uint64_t sp_el0;                              /* 248 */
    uint64_t elr_el1;                             /* 256 */
    uint64_t spsr_el1;                            /* 264 */
} pt_regs_t;                                      /* sizeof = 272, 16 对齐 */
```

头文件 Section 2（`#ifndef __ASSEMBLER__` 之外）新增 .S 共享偏移常量：

```c
#define PT_REGS_X0        (0  * 8)
#define PT_REGS_X2        (2  * 8)
/* ... 偶数寄存器每个一对 stp ... */
#define PT_REGS_X28       (28 * 8)
#define PT_REGS_X30       (30 * 8)
#define PT_REGS_SP_EL0    (31 * 8)
#define PT_REGS_ELR_EL1   (32 * 8)
#define PT_REGS_SPSR_EL1  (33 * 8)
#define PT_REGS_SIZE      (34 * 8)   /* 272：15 对 stp + 1 str + 3 str */
```

272 % 16 == 0：`sub sp, sp, #272` 后每个 stp 地址都 16 对齐（AAPCS64 SP 契约），
无 padding 字段、无 hole。`_Static_assert(sizeof(pt_regs_t) == 34*8)` 放头文件
（C 侧）；.S 侧靠 PT_REGS_SIZE 与 stp 偏移由人审 + QEMU 探针测试兜底。

### 5.3 栈预算

IRQ 打在当前 SP_EL1 上（无独立 IST 概念）。BSP 的 C 栈与 AP 的
`aarch64_boot_stacks` 栈（head.S:641-649）都要预留 ≥272 字节的 headroom 加上
dispatch C 帧与 PL011 打印深度；现状栈尺寸（AARCH64_BOOT_STACK_SIZE）足够，
不改动。

---

## 6. 集成边界（硬约束）

1. **不编 kernel core**：kernel/Makefile:42-43 白名单不动。dispatch 不进调度器、
   不碰 softirq、不调 `generic_intr_dispatch`/`register_irq`（那是 kernel/intr 的世界）。
   aarch64 的 handler 表是 driver 私有的最小复刻，签名对齐以备 Phase 2 接管。
2. **不动 x86_64 任何文件**：kernel/arch/x86_64/、kernel/include/arch/x86_64/、
   kernel/intr/ 全部只读。`kernel/include/arch/aarch64/regs.h` 的修改只影响
   aarch64 编译路径（facade 按 `__aarch64__` 分发，regs.h:27-28）。
3. **不动 task.c / 调度 / syscall / EL0**：tick ISR 仍只 `g_ticks++`，不产生
   jiffies、不 set need_resched。Generic Timer 接 kernel core 是 Phase 2。
4. **trap.c 只落 IRQ dispatch**：ESR_EL1/FAR_EL1 sync 解码、double fault 语义等
   留给 Phase 2（trap.c:4-11 注释的原始意图）。
5. **测试通过 UEFI 镜像 + serial 断言**：所有 QEMU 验证都走
   `make PROFILE=aarch64-clang aarch64-uefi` 产出的 FAT 镜像（image.mk:154-162），
   断言基于 PL011 serial 文本行（harness 模式见 §3.7）。
6. **探针与测试代码一律 `#if OS01_SELFTEST` 门控**（镜像 main.c:18/224 与
   aarch64_pt_smoke_test 的既有模式），生产启动路径零新增行为（IPI 的 AP 开中断除外，
   见 §8 R5）。

---

## 7. QEMU 验证路径与断言设计

### 7.1 真实命令

```sh
# 全量回归（SMP 1/2/4 ×3，KERNEL_SELFTEST=1 构建）
make PROFILE=aarch64-clang test-aarch64-uefi-smp
#   ↳ run.mk:161-172: $(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
#     + python3 qemutests/aarch64_uefi_smp.py --cpus 1 2 4 --repeat 3
#       --timeout 90 --expect-selftest --diagnostic-dtb=auto ...
#     每个case的真实 QEMU 形态（aarch64_uefi_smp.py:387-398）：
#     qemu-system-aarch64 -M virt,gic-version=2,acpi=off -cpu cortex-a53 -smp <N>
#       -drive if=pflash,format=raw,file=build/aarch64-clang/image/QEMU_EFI.fd
#       -drive if=none,file=build/aarch64-clang/image/aarch64-uefi.img,format=raw,readonly=on,id=disk
#       -device virtio-blk-device,drive=disk -dtb <generated> -serial stdio
#       -display none -no-reboot -no-shutdown

# hosttest（GIC driver 单元，RED 先行）
make -C hosttests PROFILE=aarch64-clang \
     OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver

# SPI 注入（Task 2.3 新增 harness + make target）
make PROFILE=aarch64-clang test-aarch64-gic-spi
```

### 7.2 SMP harness 扩展断言（`--expect-gic`）

`aarch64_uefi_smp.py` 新增 `--expect-gic` flag；`passed()` 在该 flag 下追加：

| 断言行 | 含义 | 出现时机 |
|---|---|---|
| `[gic] GICv2 driver: intids=<N>` | driver init 读 TYPER 成功 | gic_init 后 |
| `[gic] dispatch ready` | 向量表 + handler 表 + dispatch 链闭合 | 注册 tick handler 后 |
| `[gic-probe] save-restore OK` | clobber 探针通过（§7.3） | irq_enable 后 |
| `[gic-probe] unexpected intid=40 survived` | 破坏性探针：软件 pend 无 handler 的 SPI 40，打 unexpected、EOI、tick 继续 | 同上 |
| `[ipi] summary targets=<cpus-1> received=<cpus-1> status=PASS` | IPI 全收（cpus=1 时 targets=0 received=0） | IPI 测试后 |

纯 Python fixture（self_test()）先行的红/绿：断言函数必须先在 `--self-test` 里对
构造 log 红绿翻转，再上真机（镜像 expect_selftest 的既有纪律，aarch64_uefi_smp.py:186-227）。

### 7.3 破坏性探针（针对 D1 这个最大单点）

- **clobber 探针**（`#if OS01_SELFTEST`，kernel/arch/aarch64/irq_probe.c）：
  一段内联汇编把哨兵值放进 x3/x4/x5/x18，`wfi` 等至少一个 tick（读 time.c 导出的
  `g_ticks` 全局），再校验哨兵未变。**在现状 entry.S（零保存）上必红**——C dispatch
  按 AAPCS64 可自由毁 x0-x17；在 Task 2.2 后必绿。探针必须整体在一个 asm 块内
  （编译器不能替它"恢复"哨兵），并打印：
  `[gic-probe] save-restore OK` / `[gic-probe] save-restore FAIL regs=<mask>`。
- **unexpected-intid 探针**：irq_enable 后软件写 GICD_ISPENDR 置 SPI 40 pending
  （driver 提供 `gic_set_pending()`）→ dispatch 打 unexpected + EOI，随后 `[tick]`
  行继续出现证明存活。现状代码路径相似但无该 marker，故可作 RED 证据。

### 7.4 SPI 中断源选型（核查结论）

- **选 PL011 RX（INTID 33）**：QEMU virt DTB 的 pl011 节点带
  `interrupts = <0x00 0x01 0x04>`（GIC_SPI / #1 / LEVEL_HIGH），INTID = 32+1 = 33，
  与 gic.c:42 的注释互证。真实设备、level 触发、能完整练习"handler 清设备源先于 EOI"
  的 §2.3 契约（读 DR 清 RX + 写 ICR）。
- **注入方式**：新 harness `qemutests/aarch64_gic_spi.py` 用
  `-chardev socket,id=ser0,path=...,server=on,wait=off -serial chardev:ser0` 起 QEMU，
  等 serial 出现 `[gic] spi-test armed intid=33` 后向 socket 写 1 字节，断言
  `[gic-spi] intid=33 handled count=1`。UEFI 固件自身的输出也走这条 PL011，读端
  只做行扫描不受影响；kernel 侧 IMSC 只开 RXIM（bit4），TX 中断保持掩死。
- **备选/辅助**：GICD_ISPENDR 软件置 pending（`gic_set_pending(33)`）作为不依赖
  输入注入的 debug 通路保留在 driver API 里（§7.3 的 SPI 40 探针即此用法）。
- **DTB 侧**：dtb_parse.c:122-127 补解析 pl011 `interrupts`（3 个 be32 word：
  word0==0(SPI)、word1 为编号、word2==4(LEVEL_HIGH)）→ `platform.pl011_spi = 32+word1`，
  校验失败按既有 -4 语义拒绝。driver/测试从 `dtb_pl011_spi()` 取号，不硬编码 33。

### 7.5 IPI 测试（Task 3）

```
BSP: 注册 ipi handler(intid 0) → 打 "[ipi] send sgi=0 filter=others"
     → gic_send_sgi(0, 0, FILTER_OTHERS)
     → 有界轮询（cntvct deadline，镜像 phase1 spec §2.1 的无 IRQ 超时法）
        ipi_received[1..N-1] 全 ≥1
     → 逐核打印 "[ipi] cpu=<n> received=<k>" + summary 行
AP : handler 里读 TPIDR_EL1 → boot_percpu 槽 → cpu_id（head.S:650 的槽布局）
     → ipi_received[cpu_id]++（release 语义写，BSP acquire 读）
     → handler 不打印（多核并发写 PL011 会绞线，见 §8 R4）
前置: secondary_idle 尾循环前 arch_local_irq_enable()（改 smp.c:225 前的行为，
      §8 R5 详述风险）；gic_cpu_init 里为每核 banked 使能 SGI 0 bit0。
```

---

## 8. 风险表

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R1 | **entry.S 零寄存器保存是最大单点**：改坏向量表/偏移即全静默 | 高 | ① clobber 探针作为 RED 证据 + 常驻 SELFTEST 断言（§7.3）；② 偏移常量单一来源（regs.h Section 2）+ `_Static_assert(sizeof==272)`；③ 每槽 ≤0x80 字节（槽内只放一条 `b`）；④ `--expect-gic` 进标准回归 target，1/2/4 ×3 反复打 |
| R2 | EOIR 丢 CPUID 位（D7）在 SGI 路径爆雷 | 高 | `gic_eoi` 只接受完整 IAR；hosttest 断言 EOIR mock == IAR 原值（含 bits[12:10]） |
| R3 | level 触发 SPI 在 EOI 前未清设备源 → 中断风暴 | 中 | 契约写进 §2.3；SPI handler 顺序（读 DR → ICR → 返回）由 dispatch 在 handler 之后 EOI 保证；harness 超时 90s 兜底 |
| R4 | 多核并发写 PL011 绞线/死锁 | 中 | AP 侧 handler 不打印，只更新 per-CPU 计数；所有打印由 BSP 串行完成 |
| R5 | AP 开 DAIF.I 后可能收到非预期中断（若 AP banked enable 有残留） | 中 | gic_cpu_init 显式走"先全 banked ISENABLER 现状（PPI30/SGI0）"白名单路径；AP 的 CNTP 已关（smp.c:209/224）；unexpected 路径有日志 + EOI 兜底 |
| R6 | dtb_parse 新增 pl011 interrupts 校验过严导致某固件 DTB 被拒 | 低 | 与既有 device_reg 校验同等严格度；拒绝即 dtb_fatal，测试用的是 QEMU 生成 DTB（--diagnostic-dtb=auto），可复现可控 |
| R7 | hosttest 编译真实 gic_driver.c 需要它零依赖 | 低 | hw 层禁日志/禁 arch asm；`struct pt_regs` 前向声明代替 include facade（facade 按 `__aarch64__` 分发，host x86_64 编译会 #error，regs.h:29-30） |
| R8 | KERNEL_SELFTEST=1 与生产镜像行为分叉 | 低 | 探针全部门控（§6.6）；唯一生产行为变化 = AP 开中断收 IPI，在 spec/commit message 里显式声明 |
| R9 | reg.h 的 GICD_BASE/GICC_BASE 硬编码访问器残留误用 | 低 | Task 2.2 后 time.c 不再引用 gicc_read32/gicc_write32；reg.h 访问器加注释指向 gic_dev，逐步退役（不删，避免无关 churn） |

---

## 9. G1-G6 目标列表（用户可校验）

| # | 目标 | 完成判据 |
|---|---|---|
| G1 | **GICv2 driver 泛化**：hw 访问抽象（struct gic_dev 指针式 MMIO）+ SGI/PPI/SPI/invalid 分类 + enable/disable/priority/targets 配置 + handler 注册表 | hosttest `test_gic_driver`（mock MMIO 数组）覆盖 init(TYPER/IIDR)/enable(ICENABLER)/prio/targets/分类/注册表/SGIR 编码/EOIR 回写；内核里 gic_init/gic_cpu_init 行为不回退 |
| G2 | **entry.S 全量 save/restore + pt_regs_t**：31 GPR + sp_el0 + elr + spsr，布局=字段序=偏移常量，对齐 x86_64 facade | clobber 探针从红转绿并常驻 SELFTEST；`_Static_assert(sizeof(pt_regs_t)==272)` |
| G3 | **通用 IRQ dispatch**（trap.c）：IAR→查表→handler→EOIR(完整值)，spurious/unexpected 分支；time.c 硬编码 intid 比较删除，tick 变注册消费者 | `[gic] dispatch ready` + unexpected-intid=40 探针存活 + `[tick]` 不断流（--expect-gic 进 1/2/4 ×3 回归） |
| G4 | **SPI 中断源走 handler 表**：PL011 RX INTID 33（DTB 解析），QEMU socket 注入 | `test-aarch64-gic-spi` harness 断言 `[gic] spi-test armed intid=33` → 注入 1 字节 → `[gic-spi] intid=33 handled count=1` |
| G5 | **SGI/IPI**：GICD_SGIR 发送 + 跨核 handler + SMP 1/2/4 验证 | `[ipi] summary targets=N-1 received=N-1 status=PASS` 在 -smp 1/2/4 全部出现（cpus=1 为 targets=0） |
| G6 | **范式对齐 + 零回归**：不动 x86_64 任何文件、不编 kernel core、dispatch 不进调度器；既有 SMP/spinlock/tick 断言全绿 | `git diff --stat` 证明 kernel/arch/x86_64、kernel/include/arch/x86_64、kernel/intr 零改动；`make PROFILE=aarch64-clang test-aarch64-uefi-smp` 全绿；x86 构建不受影响（`make PROFILE=x86_64-clang kernel.bin` 仍可构建） |

---

## 10. 明确 non-goals（本 Phase 不做）

1. **Generic Timer 接 kernel core**：不产生 jiffies、不进 clockevent/clocksource 框架、
   不设 need_resched。tick ISR 仍是"g_ticks++ + 每秒一行打印"（Phase 2 集成项）。
2. **syscall / EL0 / 用户态**：entry.S 的 EL0 槽、AArch32 槽维持 `b .`；pt_regs 的
   elr/spsr 修改权（信号回注入等）不暴露。
3. **sync exception 解码**（ESR_EL1/FAR_EL1、data abort、page fault 路径）——trap.c:4-11
   原注释的 Phase 2 内容。
4. **kernel/intr 接入**：不实现 `arch_irq_select_controller` / `register_irq` 的
   aarch64 strong override，不做 `hw_int_controller_t` 函数指针化，不编 kernel/intr/。
5. **GICv3/v4、ITS、MSI、中断嵌套、优先级抢占**（GICC_BPR 组优先级）、**1-N NMI**、
   虚拟化扩展——QEMU virt,gic-version=2 用不到。
6. **per-CPU handler 表 / 中断亲和性 API**（irqbalance 之类）：handler 表 Phase 1 是
   全局单份；ITARGETSR 只在 SPI enable 时写死 BSP 位图。
7. **PL011 TX 中断 / console 框架接入**：输出继续轮询；只开 RXIM。
8. **改 x86_64 任何文件**（含 kernel/intr/ 的 weak 默认）——对齐是"形似"，不是共享代码。
9. **真实多核 IPI 语义**（TLB shootdown 队列、smp_call_function）：只验证"发—收—计数"。
10. **RPi3 / 非 QEMU virt 平台**：DTB 解析仍限定 QEMU virt 的固定值校验范式。
