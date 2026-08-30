---
title: OS01 aarch64 Phase 1 实现计划 — QEMU virt 最小骨架
created: 2026-08-29
updated: 2026-08-29
type: plan
status: draft
spec: docs/superpowers/specs/2026-08-29-aarch64-phase1-design.md
---

# OS01 aarch64 Phase 1 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 QEMU `virt` 平台跑通 OS01 aarch64 内核骨架（启动 + spinlock + CNTP tick + SMP），为 phase 2 RPi3 真机适配铺路。phase 1 只求「启动 + 跑通」，无用户态、无调度、无 UEFI。

**Architecture:** `-kernel` ARM64 boot 协议（`x0 = DTB` 物理地址，复位 EL2）→ `head.S _start`（低物理 0x40080000）→ EL2→EL1 降级 → 双 TTBR 页表（TTBR0 identity + MMIO，TTBR1 高半 kernel/direct-map，IPS=40-bit）→ 绝对地址 `blr` 跳高半 → `aarch64_main`（独立入口，不复用 x86 `kernel_main`）→ PL011/GICv2/CNTP(PPI 30) → `smp_boot_aps()`（PSCI `cpu_on`）→ 4 核 spinlock benchmark → `arch_local_irq_enable()` → wfi。

**Tech Stack:** C（freestanding）+ GNU as（aarch64 用 clang 内建 as），`clang -target aarch64-none-elf`、`ld.lld -m aarch64elf`、`llvm-objcopy -O binary`，QEMU `-M virt,gic-version=2 -cpu cortex-a53 -smp 4`。

**Spec:** `docs/superpowers/specs/2026-08-29-aarch64-phase1-design.md`（**v11**）— 计划从 spec 论证，实现者须同时读这两份。spec §2 是架构决策，§3.2 是文件清单，§4 是启动流程图，§6 是 4 条 exit criteria。

## Global Constraints

- **语言**：与用户中文交流；代码注释/标识符用英文，与现有 kernel 风格一致（spec 里的中文注释仅示意）。
- **提交前缀**：语义化前缀（`build(aarch64):` / `feat(aarch64):` / `test(aarch64):`），小步提交，每 Task 至少一次可编译提交。
- **TDD 纪律**：每步 RED→GREEN→全回归再推进；debug 探针只读、提交前移除。
- **验证门**：spec §6 的 A/B/C/D 每条必须附证据（QEMU 串口日志摘录）；「QEMU-verified evidence before progress claims」。
- **x86_64 不回归**：改动 `kernel/Makefile`、`kernel/include/kernel/arch/*.h`（atomic.h/cpu.h）后，必须跑 `make OS01_SYSTEST=1 test-syscall` 确认仍 228/228（exit D）。
- **不改 x86 语义**：atomic.h 的 x86 分支、percpu.h、task.h 一律不动；aarch64 一律走 `__aarch64__` 分支或新增 `arch/aarch64/*` 文件，不污染 x86 路径。
- **无 IRQ 时序**：benchmark 在 `arch_local_irq_enable()` 之前跑，超时只准用 `arch_cycle_counter()`（CNTVCT_EL0），禁止依赖 CNTP tick。
- **执行环境**：编译在本机（clang/lld 齐全）；QEMU 运行在 homeserver（或本机有 `qemu-system-aarch64` 时本机跑）。命令以 `qemu-system-aarch64` 为准，不硬编码 homeserver 路径。
- **启动命令固定**：`-M virt,gic-version=2 -cpu cortex-a53 -smp 4 -m 1G -kernel build/aarch64/kernel/kernel.elf -nographic -serial mon:stdio`（GICv2 必须显式，见 spec §附录 B）。

## Task 依赖顺序

spec 按「目标分组」分 5 任务，但**实际实现顺序**（依赖驱动）如下，plan 照此排 Steps，每个 Step 标注对应 spec 的 Task 归属：

```
Task 0（构建门控，无依赖）
   └─> Task 1（head.S/linker/PL011/main 单核启动到 printk）
          └─> Task 2a（spinlock 单核 1M 次正确性，无 SMP）
                 └─> Task 3（CNTP+GIC 单核 tick，无 SMP）
                        └─> Task 4（PSCI SMP + 4 核 benchmark 集成）
                               └─> Task 2b（4 核 benchmark 收敛，归属 spec Task 2 验收）
```

> 说明：spec Task 2 的「4 核 benchmark」依赖 SMP（Task 4），故拆成 2a（单核）+ 2b（4 核，放 Task 4 之后）。spec Task 4 的「集成验证」实际包含 2b。

---

## File Structure

**新增（全部在 `kernel/arch/aarch64/` 或 `kernel/include/kernel/arch/aarch64/`）：**

| 文件 | 职责 | spec 引用 |
|---|---|---|
| `kernel/arch/aarch64/make.config` | toolchain flags（改，见 Task 0） | §3.2 |
| `kernel/arch/aarch64/linker.ld` | `.boot` VMA=LMA=0x40080000 + `ENTRY(_start)` + 高半段 | §2.5 |
| `kernel/arch/aarch64/head.S` | `_start` 降级/建页表/跳高半 + secondary trampoline | §4 |
| `kernel/arch/aarch64/entry.S` | VBAR_EL1 向量表（2 KiB 对齐）+ EL1 IRQ/SVC dispatch | §2.5 |
| `kernel/arch/aarch64/trap.c` | ESR_EL1/FAR_EL1 解析 + EL1 IRQ dispatch | §3.2 |
| `kernel/arch/aarch64/main.c` | `aarch64_main(dtb_base)` 独立入口 | §2.5 |
| `kernel/arch/aarch64/boot_percpu.c` | `aarch64_boot_percpu[NR_CPUS]` + early stacks + benchmark 状态（`.boot.bss`） | §2.6 |
| `kernel/arch/aarch64/pl011.c` | 最小 MMIO UART 轮询输出 | §0/§3.2 |
| `kernel/arch/aarch64/dtb.c` | 最小 FDT（5 节点）+ DTB CPU 表 | §2.1 |
| `kernel/arch/aarch64/gic.c` | GICv2 distributor + CPU interface（只 PPI 30） | §2.3 |
| `kernel/arch/aarch64/time.c` | `arch_tick_start()` + 最小 tick ISR（重装 TVAL → EOI → printk） | §2.3 |
| `kernel/arch/aarch64/psci.c` | SMC `cpu_on` / `psci_version` | §2.1 |
| `kernel/arch/aarch64/smp.c` | `smp_boot_aps()`（查表/跳过 BSP/release 门/online timeout） | §2.1 |
| `kernel/arch/aarch64/test_spinlock.c` | 单核 + 4 核 benchmark 入口 | §5 Task 2 |
| `kernel/include/kernel/arch/aarch64/spinlock.h` | 6 函数 + `spinlock_T`（替换 `#error` stub） | §2.4 |

**修改（arch 共享，须 x86 不回归）：**

| 文件 | 改动 | spec 引用 |
|---|---|---|
| `kernel/Makefile` | CC/LD/EXTRA_OBJECTS/LWIP/KERNEL_C_SOURCES 按 ARCH 条件化 | §3.2/§5 Task 0 |
| `Makefile`（根） | 去 `export CC/LD` target 硬编 | §3.2 |
| `kernel/include/kernel/arch/atomic.h` | aarch64 端 `ldxr/stxr` → `ldaxr/stlxr`（cas/xchg/write） | §2.4 |
| `kernel/include/kernel/arch/cpu.h` | aarch64 端 `arch_tick_start` 改 extern 声明 | §3.2 |

---

## Task 0：Makefile 改造 + 源码收编 arch 门控

**目标**：`make ARCH=aarch64 kernel.elf` 只编 aarch64 兼容源，输出 AArch64 ELF；`make`（x86_64 默认）行为不变。

**Files:** `Makefile`（根）、`kernel/Makefile`、`kernel/arch/aarch64/make.config`

**Interfaces:**
- Produces：`ARCH`/`ARCHDIR`/`BUILD_DIR` 已存在；本任务补 `CC`/`LD`/`OBJFORMAT`/`ASM` 的 aarch64 取值，以及源码收编门控。

- [ ] **Step 0.1: 根 Makefile 去 export 硬编**

`Makefile:2-3` 当前：
```makefile
export CC=clang -target x86_64-unknown-none
export LD=ld.lld -m elf_x86_64
```
改为不 export target（让 kernel/Makefile 按 ARCH 自决）。根 Makefile 的其他 `clang -target x86_64-unknown-none` 调用（busybox 编译等）保持不动——那些是用户态 x86，phase 1 不碰。仅移除顶部两行 `export CC/LD`，并确认根 Makefile 后续无依赖这两个 export 的 kernel 编译路径（kernel 由 `make -C kernel` 走 kernel/Makefile）。

- [ ] **Step 0.2: kernel/Makefile CC/LD/OBJFORMAT 条件化**

`kernel/Makefile:12-15` 当前 `CC ?= clang -target x86_64-unknown-none`。改为按 ARCH：

```makefile
ifeq ($(ARCH),aarch64)
CC        ?= clang -target aarch64-none-elf
LD        ?= ld.lld -m aarch64elf
OBJFORMAT ?= elf64-littleaarch64
else
CC        ?= clang -target x86_64-unknown-none
LD        ?= ld.lld -m elf_x86_64
OBJFORMAT ?= elf64-x86-64
endif
```

- [ ] **Step 0.3: EXTRA_OBJECTS 条件化（trampoline + font.o）**

`kernel/Makefile:62-64` 当前 `EXTRA_OBJECTS` 无条件含 `trampoline_bin.o` 与 `font.o`。aarch64 无 `trampoline.S/.ld`、无 framebuffer。改为：

```makefile
EXTRA_OBJECTS :=
ifeq ($(ARCH),x86_64)
EXTRA_OBJECTS += \
    $(BUILD_DIR)/kernel/font.o \
    $(BUILD_DIR)/$(ARCHDIR)/trampoline_bin.o
endif
```
（`trampoline_*.o` 的编译规则 §155-168 保留不动——它们引用 `$(ARCHDIR)/trampoline.S`，aarch64 下无文件、且 EXTRA_OBJECTS 已不含它，不会触发。）

- [ ] **Step 0.4: LWIP_OBJECTS 条件化**

`kernel/Makefile:100` `KERNEL_OBJECTS += $(LWIP_OBJECTS)` 改为仅 x86_64：
```makefile
ifeq ($(ARCH),x86_64)
KERNEL_OBJECTS += $(LWIP_OBJECTS)
endif
```

- [ ] **Step 0.5: KERNEL_C_SOURCES 按 ARCH 门控**

`kernel/Makefile:23-43` 当前 wildcard 全收编。改为：x86_64 走原路径；aarch64 走白名单。最小白名单从空起步，随 Task 进展补源（spec 策略「链接器缺啥补啥」）：

```makefile
ifeq ($(ARCH),aarch64)
# aarch64 phase 1 最小集：只收 arch-neutral 支撑源。
# 核心逻辑全在 arch/aarch64/*.c（由 ARCH_C_SOURCES wildcard 自动收编）。
KERNEL_C_SOURCES :=
# （随 Task 进展补，例如 kernel/printk.c 若复用则加到这里）
else
KERNEL_C_SOURCES := \
    $(wildcard kernel/*.c) \
    $(wildcard net/*.c) \
    ...（原 wildcard 列表不变）
KERNEL_C_SOURCES := $(filter-out driver/pit.c driver/rtc.c,$(KERNEL_C_SOURCES))
KERNEL_C_SOURCES += $(ARCH_PLATFORM_C_SOURCES)
endif
```

**关键**：aarch64 下 `ARCH_C_SOURCES := $(wildcard $(ARCHDIR)/*.c)` 与 `ARCH_ASM_SOURCES := $(wildcard $(ARCHDIR)/*.S)` 已自动收编 `arch/aarch64/*`，无需手列。

- [ ] **Step 0.6: aarch64/make.config 修正**

`kernel/arch/aarch64/make.config` 当前有 `-fpie`（内核需 `-fno-pie -fno-pic`）。改为：
```makefile
ARCH_LINKER    = linker.ld
ARCH_CFLAGS    = -march=armv8-a -ffreestanding -Wall -Wextra \
                 -mgeneral-regs-only -fno-pie -fno-pic
ARCH_ASMFLAGS  =
ARCH_LDFLAGS   = -Wl,-m -Wl,aarch64elf -static -Wl,-z,muldefs
ASM            = $(CC)
OBJFORMAT      = elf64-littleaarch64
ARCH_PLATFORM_C_SOURCES :=
```

- [ ] **Step 0.7: 验证门（Task 0 完成）**

```bash
make -C kernel ARCH=aarch64 kernel.elf 2>&1 | tail -30
file build/aarch64/kernel/kernel.elf
# 期望：ELF 64-bit LSB executable, ARM aarch64（此时可能因缺 main/head 符号链接失败，
#       属预期 RED；确认错误是「缺符号」而非「x86 源被误编译」）
make -C kernel kernel.elf        # x86_64 默认，仍成功（不回归）
```

**RED 判据**：改前 `make ARCH=aarch64` 会去编 `intr/apic/*.c` `driver/serial.c` 等 x86 源并报 `__x86_64__` 依赖错。**GREEN 判据**：改后 aarch64 只碰 `arch/aarch64/*` + 白名单，报错变成「未定义 `_start`/`aarch64_main`」这类链接符号缺失，证明门控生效。

---

## Task 1：head.S + linker.ld + PL011 + main（单核启动到 printk）

**目标**：`_start` 完成降级→页表→MMU→跳高半→`aarch64_main`，PL011 打印 "OS01 aarch64 phase1 boot ok" 后进 wfi（spec exit A）。

**Files:** `linker.ld`、`head.S`、`entry.S`（最小）、`trap.c`（stub）、`main.c`、`pl011.c`、`boot_percpu.c`

**Interfaces:**
- Consumes：`arch_cycle_counter()`（cpu.h 已 inline）、`arch_local_irq_disable/enable`（arch/irq.h 已 inline）
- Produces：`void aarch64_main(uint64_t dtb_base)`、`void pl011_init(void)`、`void pl011_putc(char)`、`aarch64_boot_percpu_t aarch64_boot_percpu[NR_CPUS]`

- [ ] **Step 1.1: linker.ld**

低地址 `.boot` 段（VMA=LMA=0x40080000），含 `_start` + `.boot.text`/`.boot.data`/`.boot.bss`；`ENTRY(_start)`。其余 `.text/.rodata/.data/.bss` 高半 VMA（`0xffff000040080000` 起）、低物理 LMA。`.boot.bss` 用 `KEEP(*(.boot.bss*))` 收入低地址段。关键断言：`_start` 符号地址 == 0x40080000（`nm` 验证）。

- [ ] **Step 1.2: head.S `_start`（spec §4 流程逐条）**

按 spec §4 流程图实现，顺序严格：
1. `mov x19, x0`（保存 dtb_base 到 callee-saved，跨降级/清零存活）
2. `mrs x0, CurrentEL` → 若 EL2：设 `HCR_EL2.RW=1`、`SPSR_EL2`(EL1h + DAIF mask)、`CNTHCTL_EL2 = (1<<0)|(1<<1)`、`VBAR_EL2` → `eret` 降级
3. `msr daifset, #0xf`
4. `mrs MIDR_EL1` 验证 Cortex-A53（不符则 panic loop）
5. 只清 `.boot.bss`（页表/early stack/DTB 槽/boot_percpu 的低地址容器）
6. 建双 TTBR 页表（§2.2 映射范围；页表放 `.boot.bss`；MAIR AttrIdx0=Device/AttrIdx1=Normal；descriptor 按 §2.2 属性表：AF=1、AP=EL1 RW、PXN/UXN 按执行策略）
7. `dsb sy` → 写 `TCR_EL1`(T0SZ=T1SZ=16, TG0=TG1=4K, SH/IRGN/ORGN, IPS=40) + `MAIR_EL1` → 写 `TTBR0_EL1`/`TTBR1_EL1` → `isb`
8. 显式构造 `SCTLR_EL1`（保留 RES1、置 M/C/I）→ 写 → `isb`
9. 设 BSP 栈（early stack）+ `msr tpidr_el1, &aarch64_boot_percpu[0]`
10. `ldr x30, =aarch64_main` + `blr x30`（绝对地址，非 PC-relative bl）

- [ ] **Step 1.3: 高半收尾（在 head.S 或首个高半 C stub 内）**

跳到高半后：清普通高半 `.bss` → 设 `VBAR_EL1`（2 KiB 对齐，指向 entry.S）→ 把 `x19` 写回 DTB 槽（`.boot.bss`）→ 从槽重载 `x0 = dtb_base` → 进 `aarch64_main`。

- [ ] **Step 1.4: entry.S（最小向量表）**

VBAR_EL1 向量表，至少 `EL1t/EL1h × (sync/irq/fiq/serror)` 8 项 + 32 项占位（2 KiB 对齐）。phase 1 仅需 EL1 IRQ 入口跳 `el1_irq_handler`（Task 3 才填 GIC dispatch，先 `eret` 空转）+ 其余 `b .`（死循环）。`SVC` dispatch 留 phase 2。

- [ ] **Step 1.5: pl011.c（轮询输出）**

MMIO @ 0x09000000（Device 映射）。`pl011_init()`：设波特率（QEMU 无需真实配 baud，但设 LCR 8N1 + 使能 TX）。`pl011_putc(char)`：轮询 `FR.TXFF` 空后写 `DR`。提供 `printk`/`write_serial` 的 aarch64 出口（若复用 `kernel/printk.c`，则在 arch 层接 `arch_putc`；否则 `main.c` 直接调 `pl011_putc` 拼 boot 串）。

- [ ] **Step 1.6: main.c + boot_percpu.c + trap.c stub**

`aarch64_main(dtb_base)`：`pl011_init()` → printk "OS01 aarch64 phase1 boot ok" → `wfi` 死循环。`boot_percpu.c`：`aarch64_boot_percpu[NR_CPUS]` + early stacks + benchmark 状态，均 `__attribute__((section(".boot.bss")))`。`trap.c`：`sys_vector_install()` 或直接设 VBAR（plan 采用：main.c 直接设 VBAR，trap.c 只放 `el1_irq_handler` stub）。

- [ ] **Step 1.7: 验证门（exit A）**

```bash
make -C kernel ARCH=aarch64 kernel.elf
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 1G \
  -kernel build/aarch64/kernel/kernel.elf -nographic -serial mon:stdio
# 期望：串口出现 "OS01 aarch64 phase1 boot ok"
```

---

## Task 2a：spinlock 单核真实现 + 单核 benchmark

**目标**：替换 spinlock `#error` stub，6 函数基于 LDAXR/STLXR；单核 1M 次 lock/unlock 正确。

**Files:** `kernel/include/kernel/arch/aarch64/spinlock.h`、`kernel/include/kernel/arch/atomic.h`、`kernel/arch/aarch64/test_spinlock.c`

**Interfaces:**
- Produces：`spin_init`/`spin_lock`/`spin_unlock`/`spin_trylock`/`spin_lock_irqsave`/`spin_unlock_irqrestore`（签名与 x86_64 6 函数一致，`spinlock_T.lock`=1 unlocked）

- [ ] **Step 2.1: atomic.h 修原子语义（spec §2.4）**

`kernel/include/kernel/arch/atomic.h` 的 `#elif defined(__aarch64__)` 分支，把 `arch_atomic_cas`/`arch_atomic_xchg`/`arch_atomic_write` 的 `ldxr/stxr` → `ldaxr/stlxr`（acquire load + release store）；`fetch_add/inc` 保持 `ldxr/stxr` relaxed。x86 分支不动。

- [ ] **Step 2.2: spinlock.h 实现**

`spin_lock` 用 `arch_atomic_cas(&lock->lock, 1, 0)` 的 CAS 自旋（acquire）；`spin_unlock` 用 release store 写回 1（`stlr` 或 `arch_atomic_cas` 写回）；`spin_trylock` 用 `arch_atomic_xchg`；`spin_lock_irqsave` 用 `arch_local_irq_save()` 返回旧 DAIF；`spin_unlock_irqrestore` 恢复 DAIF。

- [ ] **Step 2.3: 单核 benchmark**

`test_spinlock.c`：单核跑 1M 次 `spin_lock`/`spin_unlock`，验证计数正确、无死锁。结果 printk 到 PL011。

- [ ] **Step 2.4: 验证门**

```bash
make -C kernel ARCH=aarch64 kernel.elf
qemu-system-aarch64 ... -smp 1 ...   # 单核跑 1M 次，串口报告 PASS
make OS01_SYSTEST=1 test-syscall     # x86_64 不回归（atomic.h 改了，必须验）
```

---

## Task 3：CNTP + GICv2 PPI 30（单核 tick）

**目标**：100 Hz CNTP tick 触发 EL1 IRQ，tick ISR 重装 TVAL + EOI + printk "+tick"（spec exit B）。

**Files:** `kernel/arch/aarch64/dtb.c`、`gic.c`、`time.c`（+ 改 `kernel/include/kernel/arch/cpu.h`）

**Interfaces:**
- Consumes：`arch_cycle_freq()`（CNTFRQ_EL0，已 inline）、`arch_local_irq_enable/disable`
- Produces：`bool arch_tick_start(void)`（extern 声明替换 cpu.h 里的 `static inline ...false`）、`void dtb_init(uint64_t dtb_base)`、`void gic_init(void)`

- [ ] **Step 3.1: cpu.h 改 extern**

`kernel/include/kernel/arch/cpu.h` aarch64 端，把 `static inline bool arch_tick_start(void){return false;}` 改为 `bool arch_tick_start(void);`（extern 声明，让 time.c 真实现可见，避免 static inline 遮蔽）。

- [ ] **Step 3.2: dtb.c 最小 FDT 解析（5 节点）**

解析 `/cpus`（拿每个 CPU 的完整 `reg`=MPIDR，建 `{logical_cpu_id, full_mpidr}` 表 + 失败条件见 spec §2.1）、`/psci`（method smc/hvc）、`/timer`（拿 PPI 号，预期 30）、`/interrupt-controller`（GICD/GICC 地址）、`/pl011`（uart 地址/irq）。实现 spec §2.1 的 4 条失败条件（CPU>NR_CPUS / MPIDR 重复 / BSP 不在表 / AP 查不到，均 panic/halt）。

- [ ] **Step 3.3: gic.c 最小 GICv2**

GICD @ 0x08000000 + GICC @ 0x08010000（Device 映射）。`gic_init()`：使能 PPI 30（GICD_ISENABLER + GICD_ITARGETSR0 路由到当前 CPU + GICC_PMR/CTLR），**不启 SPI 33**（PL011 轮询）。

- [ ] **Step 3.4: time.c + tick ISR**

`arch_tick_start()`：`CNTP_TVAL_EL0 = CNTFRQ_EL0 / HZ`，`CNTP_CTL_EL0.IENABLE=1`，返回 true。tick ISR：重写 `CNTP_TVAL_EL0` → 写 `GICC_EOIR=PPI30` → printk "+tick"。`entry.S` 的 EL1 IRQ 入口 dispatch 到这里（读 `GICC_IAR` 判 PPI 30）。

- [ ] **Step 3.5: 验证门（exit B）**

```bash
make -C kernel ARCH=aarch64 kernel.elf
qemu-system-aarch64 ... -smp 1 ...   # 跑 10 秒，串口 "+tick" ≈ 1000 次（±5%）
```

---

## Task 4：PSCI SMP + 4 核 benchmark 集成（含 Task 2b）

**目标**：`-smp 4` 启动，3 个 AP 经 PSCI `cpu_on` 上线，4 核 benchmark 计数严格 == 4,000,000（spec exit C），30 秒无 panic；x86_64 仍 228/228（exit D）。

**Files:** `kernel/arch/aarch64/psci.c`、`smp.c`、`head.S`（secondary trampoline）、`test_spinlock.c`（4 核路径）

**Interfaces:**
- Produces：`void smp_boot_aps(void)`、`int psci_cpu_on(uint64_t mpidr, uintptr_t entry)`

- [ ] **Step 4.1: head.S secondary trampoline**

secondary 入口（低物理标号，`cpu_on` 的 entry）：降级检查（幂等）→ 读完整 `MPIDR_EL1` → 查 `{logical_cpu_id,full_mpidr}` 表得 cpu_id（查不到 panic）→ 独立栈 `aarch64_boot_percpu[cpu_id].stack` → `msr tpidr_el1` → 等 release 门（acquire 读 `go`）→ `dsb/isb` → 使能 MMU（复用 BSP 的 TTBR）→ 设 `VBAR_EL1` → GIC CPU iface → `secondary_idle()`（等 `benchmark_go` → 跑 1M 次 → release store `done[cpu_id]=1` → `wfi`）。

- [ ] **Step 4.2: psci.c**

`psci_version()`（SMC PSCI_0_2_FN_PSCI_VERSION）、`psci_cpu_on(uint64_t mpidr, uintptr_t entry)`（SMC PSCI_0_2_FN64_CPU_ON，返回 x0 判 `PSCI_SUCCESS`）。

- [ ] **Step 4.3: smp.c `smp_boot_aps()`**

读 BSP 自身 `MPIDR_EL1` 映射 logical 0 → 遍历 DTB 表**跳过 BSP** → 对每个非 BSP 调 `psci_cpu_on(mpidr, entry)` 查返回码 → 以 `aarch64_boot_percpu[i].online` + `arch_cycle_counter()` 超时轮询。全部 online 后执行 benchmark 协议（spec §2.1 v11）：release store `benchmark_go=1` + BSP 同步参与 1M 次 + release store `done[0]=1` + acquire 轮询所有 `done[i]` + PASS/FAIL 判据（所有 done==1 且 `benchmark_total == active_cpu_count*1,000,000`）。

- [ ] **Step 4.4: 4 核 benchmark（Task 2b）**

`test_spinlock.c` 4 核路径：临界区内 `spin_lock(&bench_lock); benchmark_total++; spin_unlock(&bench_lock);`（普通非原子 ++，唯一受锁保护）。BSP 汇总打印每核 `done` + 总计数 + PASS/FAIL。

- [ ] **Step 4.5: 验证门（exit C + D）**

```bash
make -C kernel ARCH=aarch64 kernel.elf
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 4 -m 1G \
  -kernel build/aarch64/kernel/kernel.elf -nographic -serial mon:stdio
# 期望：3 个 AP online + [spinlock] total=4000000 PASS + [tick] 持续，30 秒无 panic
make OS01_SYSTEST=1 test-syscall   # x86_64 仍 228/228
```

---

## 收尾

- [ ] spec §6 四条 exit criteria（A/B/C/D）全部附 QEMU 日志证据，更新本 plan 顶部 status 为 `done`。
- [ ] 更新 `docs/superpowers/specs/2026-08-29-aarch64-phase1-design.md` 的 TODO（phase 2+ 清单已在 §7，无需新增，但可标注 phase 1 已验证项）。
- [ ] 提交历史保持小步：Task 0/1/2a/3/4 各自独立 commit，前缀 `build(aarch64):` / `feat(aarch64):` / `test(aarch64):`。

---

**Plan 版本**：v1（draft）  
**Spec 版本**：v11  
**实施前提**：spec 已通过设计评审 → 按本 plan 5 任务（Task 0/1/2a/3/4，其中 2b 并入 Task 4）TDD 推进
