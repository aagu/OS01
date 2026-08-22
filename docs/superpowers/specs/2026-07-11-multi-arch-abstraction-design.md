# 多架构抽象设计

> **日期**: 2026-07-11
> **状态**: 设计文档 v3（根据 review #2 修正）
> **目标**: 通过 header 级抽象层将 x86_64 专用代码从通用内核中解耦，为未来 aarch64 移植做准备
> **参考**: ArvernOS 的 `arch/` 抽象模式

---

## 1. 动机

当前 OS01 的通用内核代码（`apic/`, `memory/`, `sched/`, `driver/`）直接包含 `<kernel/arch/x86_64/*.h>` 或直接在 `.c` 文件中嵌入 x86 内联汇编。这导致：

- 移植到新架构时必须逐一排查每个文件中的架构依赖
- 无法编译非 x86 目标
- 没有清晰的接口边界指明 "要实现什么"

**目标不是 "让 aarch64 能启动"**，而是创建一套干净的架构抽象层，使 OS01 的架构端口模式明确、可测试且可持续。

---

## 2. 设计原则

1. **语义化接口，而非指令封装** — 通用代码调用 `arch_cpu_halt()`，不是 `__asm__("hlt")`。不向通用代码暴露 MSR 编号、CR2/CR3 等 x86 寄存器名。
2. **通用代码中零裸 `__asm__`** — Phase E 验证会 grep 确认。例外：arch/x86_64/ 目录下的架构文件。
3. **不抽象概念差异过大的东西** — IDT/GIC、TSS/PSCI、APIC/GIC-400、get_current_task RSP 掩码保持架构特定。
4. **aarch64 编译友好** — aarch64 下端口 I/O 等 x86 特有接口定义为 `__builtin_trap()` 而非 `#error`，使仅在 x86 路径执行的代码能编译通过。

---

## 3. 通用接口定义

新增 `include/kernel/arch/*.h` 头文件集，作为通用内核代码的唯一入口。每个头文件的通用版本通过 `#ifdef __x86_64__` 条件包含 x86 实现；aarch64 下则在相应桩文件中定义。

### 3.1 `arch/io.h` — MMIO + 端口 I/O

```c
// ── MMIO（所有架构通用）──
uint8_t  arch_readb(volatile void *addr);
uint16_t arch_readw(volatile void *addr);
uint32_t arch_readl(volatile void *addr);
uint64_t arch_readq(volatile void *addr);
void     arch_writeb(volatile void *addr, uint8_t val);
void     arch_writew(volatile void *addr, uint16_t val);
void     arch_writel(volatile void *addr, uint32_t val);
void     arch_writeq(volatile void *addr, uint64_t val);

// ── 端口 I/O（x86 特有；aarch64 下为 __builtin_trap()）──
uint8_t  arch_inb(uint16_t port);
void     arch_outb(uint16_t port, uint8_t data);
uint16_t arch_inw(uint16_t port);
void     arch_outw(uint16_t port, uint16_t data);
uint32_t arch_ind(uint16_t port);
void     arch_outd(uint16_t port, uint32_t data);

// ── 空操作（用于 spin-wait 循环）──
void     arch_nop(void);
```

**实现来源**: `kernel/include/kernel/arch/x86_64/hw.h`（现有 `inb`/`outb`/`inw`/`outw`/`ind`/`outd` + 新增 MMIO readonly/writeq 封装 + `nop()`）

**使用者**: `driver/serial.c`, `driver/keyboard.c`, `driver/ahci.c`, `driver/pci.c`, `apic/ioapic.c`

### 3.2 `arch/cpu.h` — CPU 基础操作

```c
void     arch_cpu_halt(void);                 // HLT（或 aarch64 WFI）
void     arch_cpu_pause(void);                // PAUSE / YIELD
uint64_t arch_cycle_counter(void);            // high-res cycle counter
void     arch_cpu_enable_nx(void);            // 启用 No-eXecute (EFER NXE / SCTLR NX)
void     arch_set_percpu_base(void *ptr);     // 设置 per-CPU 数据区基址 (GS base / tpidr_el1)
void     arch_cpu_init_percpu(uint32_t cpu_id, uint32_t apic_id);  // percpu_data[cpu] 初始化
```

**不含**: MSR 操作、CR2/CR3、原子操作、NR_CPUS —— 这些由其他头文件提供。
**不含**: `get_current_task()` (RSP 掩码)——它是架构特定的上下文切换协议，Section 5 中有记录。

**实现来源**:
- `include/kernel/arch/x86_64/asm.h` → `hlt()`, `sti()`, `cli()`
- `include/kernel/arch/x86_64/cpu.h` → `rdtsc()` → `arch_cycle_counter()`
- `arch_set_percpu_base()` → `wrmsr(IA32_GS_BASE, ...)`（原生在 percpu.c）
- EFER NXE 设置（目前在 main.c 内联 rdmsr/wrmsr）→ 封装到此
- `arch_cpu_init_percpu()` → 现有 `percpu_init()` 调用（泛化名称）

#### CR2 操作（架构特定，不进入通用接口）

`arch_read_cr2()` 保留在 `arch/x86_64/cpu.h` 中，仅在架构文件（`arch/x86_64/trap.c`）中使用。通用文件不读 CR2——所以它不进入通用 `arch/cpu.h`。

### 3.3 `arch/atomic.h` — 原子操作

从 `arch/cpu.h` 拆出独立的原子操作接口：

```c
uint64_t arch_atomic_fetch_add(volatile uint64_t *ptr, uint64_t val);
uint64_t arch_atomic_fetch_sub(volatile uint64_t *ptr, uint64_t val);
uint64_t arch_atomic_inc(volatile uint64_t *ptr);
uint64_t arch_atomic_read(volatile uint64_t *ptr);
void     arch_atomic_write(volatile uint64_t *ptr, uint64_t val);
int      arch_atomic_cas(volatile uint64_t *ptr, uint64_t old, uint64_t new);
uint64_t arch_atomic_xchg(volatile uint64_t *ptr, uint64_t val);
```

**实现来源**: `include/kernel/arch/x86_64/cpu.h` 中现有的 7 个内联函数，加 `#define arch_atomic_fetch_add atomic_fetch_add` 别名。

**使用者**: 各处需要 SMP 安全计数的代码（percpu、tlb shootdown、VMA cow_count 等）

### 3.4 `arch/irq.h` — 中断控制

```c
typedef uint64_t arch_irq_state_t;   // x86: RFLAGS; aarch64: DAIF (只需 4 位)
                                     // 64 位选择：简单性 > 类型窄化。aarch64 只需低 4 位，
                                     // 但 uint64_t 使 save/restore 接口一致，不会引发
                                     // 高位被截断的 bug。

void          arch_local_irq_enable(void);
void          arch_local_irq_disable(void);
arch_irq_state_t arch_local_irq_save(void);
void          arch_local_irq_restore(arch_irq_state_t flags);

// 中断注册（由通用 intr/ 调用，架构特定的 install 函数在 arch/x86_64/irq.c）
typedef void (*arch_intr_handler_fn)(uint64_t nr, uint64_t param, pt_regs_t *regs);
void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist);
void arch_irq_install(void);   // 初始化架构中断硬件（IDT / vector table）
```

**不包含**: INTR_SAVE_ALL、DEFINE_INTR_STUB、REGISTER_INTR_HANDLER——这些是 x86 特有的汇编宏，留在 `gate.h` 中。

**实现来源**:
- `include/kernel/arch/x86_64/asm.h` → `sti()` / `cli()` → `arch_local_irq_enable/disable()`
- 当前在 `intr/irq.c` 中的 `irq_install()` 和 `Build_IRQ` → 移到 `arch/x86_64/irq.c`
- `intr/` 目录保留：`generic_intr_dispatch`、`register_irq`（架构中立查表分发）

### 3.5 `arch/barrier.h` — 内存屏障

```c
void arch_mb(void);      // 全屏障 (x86: mfence; aarch64: dmb sy)
void arch_rmb(void);     // 读屏障 (x86: lfence; aarch64: dmb ld)
void arch_wmb(void);     // 写屏障 (x86: sfence; aarch64: dmb st)
```

### 3.6 `arch/mmu.h` — 内存管理单元

```c
// TLB 维护
void arch_flush_tlb_all(void);
void arch_flush_tlb_page(uintptr_t vaddr);   // 统一名称，非 x86 的 invlpg
// flush_tlb(old_cr3) 宏：通过重新加载 CR3/TTBR0_EL1 刷新全部 TLB
void arch_switch_mm(uint64_t *pml4);          // 切换地址空间（加载 CR3 / TTBR0_EL1）

// 页表遍历 — 返回完整物理地址（含页内偏移），不解读 PTE flags
// 输出：如 va=0x1234_5678_9000_1000 → 可能返回 0x0000_0000_0ABC_D000 + 0x1000
// x86: PML4→PDPT→PD→PT 四级页表，与 aarch64 stage-1 四级结构概念一致
// 注意：PTE flags 解析（R/W、Present、COW、XD 等）保留在 arch/x86_64/ 内的代码中
uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va);
```

**不在通用接口中暴露**: `arch_invlpg`（x86 特有指令名，内部别名即可）；`user_va_to_phys()` 的 PTE flags 读取留在 `trap.c`。`user_va_to_phys()` 本身保留在 `arch/x86_64/trap.c` 中（它同时做了遍历 + flags 解析）。

**实现来源**:
- `vmm.c` 中的 `invlpg` 内联 → `arch_flush_tlb_page()`
- `vmm.h` 中的 `flush_tlb()` 宏（CR3 reload）→ `arch_flush_tlb_all()`
- `vmm.h` 中的 `switch_tlb()` 宏（CR3 write）→ `arch_switch_mm()`
- `trap.c` 中的 `user_va_to_phys()` → 保持架构文件，新增 `arch_virt_to_phys()` 为其精简版

### 3.7 `arch/segment.h` — 保护模式段选择子

```c
// x86 GDT 选择子（aarch64 下 undefined，但需有定义以使 task.h 可编译）
#define ARCH_KERNEL_CS  0x08
#define ARCH_KERNEL_DS  0x10
#define ARCH_USER_CS    0x23
#define ARCH_USER_DS    0x2B
```

**来源**: 当前在 `kernel/include/kernel/task.h` 和 `kernel/arch/x86_64/trap.c` 中直接使用 `KERNEL_CS`/`USER_CS` 等常量。

### 3.8 `arch/thread.h` — 寄存器上下文（pt_regs）

```c
// 异常/中断入口时保存的完整寄存器上下文
typedef struct pt_regs pt_regs_t;  // 架构精确定义
// x86:  entry.S 中 INTR_SAVE_ALL 布局
// aarch64: EL1 异常向量表硬件压栈布局
```

**源文件**: 从 `kernel/include/kernel/arch/x86_64/regs.h` 迁移，`arch/thread.h` 只是条件包含重定向。

**影响范围**: `include/kernel/interrupt.h`、`include/kernel/task.h`、`include/kernel/trace.h`、`include/kernel/wait.h` 等依赖 pt_regs_t 的头文件。这是必需的——把这些头文件改为 `#include <kernel/arch/thread.h>` 而非 `<kernel/arch/x86_64/regs.h>` 正是抽象的目标。

### 3.9 `arch/percpu.h` — per-CPU 数据访问

```c
// 返回当前 CPU 的 percpu_t 指针
// x86: movq %gs:0, %rax
// aarch64: mrs x0, tpidr_el1
static inline percpu_t *arch_this_cpu(void);
```

**目的**: 将 `percpu.h` 中当前直接内联的 `movq %%gs:0` 抽象化。

`percpu_t` 结构变更：
- `uint32_t apic_id` → `uint32_t arch_processor_id`（aarch64 使用 MPIDR_EL1）
- `tlb_wanted`/`tlb_ack` 保留——它们是架构特定的 TLB shootdown 实现细节。aarch64 端口可能用 IPI + 广播 TLBI 指令，仍需要 ACK 同步。

### 3.10 `arch/elf.h` — ELF 常量

```c
#define ARCH_ELF_MACHINE  0x3E    // EM_X86_64 (aarch64: 0xB7 EM_AARCH64)
// 为将来预留:
// #define ARCH_ELF_CLASS   ELFCLASS64
// #define ARCH_ELF_DATA    ELFDATA2LSB
```

**使用者**: `kernel/fs/elf.c`

### 3.11 `arch/cache.h` — DMA 缓存操作（桩）

```c
static inline void arch_flush_dcache(void *addr, size_t len) {}
static inline void arch_inval_dcache(void *addr, size_t len) {}
```

aarch64 下的 DMA 一致性需要真正的 cache 维护；x86 下为空操作。

---

## 4. 具体文件变更

### 4.1 新增通用头文件（11 个）

| 文件 | 职责 | 分发机制 |
|------|------|---------|
| `arch/io.h` | MMIO + 端口 I/O + nop | `#ifdef __x86_64__` → 包含 hw.h + alias |
| `arch/cpu.h` | halt/pause/cycle_counter/NX/per-CPU base | `#ifdef __x86_64__` → 包含 asm.h + cpu.h |
| `arch/atomic.h` | 原子操作 | 来自现有 cpu.h 的 7 个函数 |
| `arch/irq.h` | 中断使能/禁用 + 架构安装 | `#ifdef __x86_64__` → 包含 asm.h |
| `arch/barrier.h` | 内存屏障 | `#ifdef __x86_64__` → mfence/lfence/sfence |
| `arch/mmu.h` | TLB 刷新 + switch_mm + virt_to_phys | `#ifdef __x86_64__` → 内联 invlpg |
| `arch/segment.h` | GDT 选择子常量 | `#ifdef __x86_64__` → 硬编码值 |
| `arch/thread.h` | pt_regs_t | 包含 x86_64/regs.h |
| `arch/percpu.h` | per-CPU 指针访问 | `#ifdef __x86_64__` → `movq %%gs:0` |
| `arch/elf.h` | ELF 机器常量 | `#ifdef __x86_64__` → EM_X86_64 |
| `arch/cache.h` | DMA cache 操作 | x86: 空桩；aarch64: 实现 |

通用头文件的模式：

```c
// kernel/include/kernel/arch/cpu.h
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

#include <stdint.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/asm.h>

static inline void arch_cpu_halt(void) { hlt(); }
static inline void arch_cpu_pause(void) { __asm__ __volatile__("pause"); }
static inline uint64_t arch_cycle_counter(void) { return rdtsc(); }
// ...
#elif defined(__aarch64__)
// 待实现：包含 arch/aarch64/cpu.h
#error "aarch64 cpu.h not yet implemented"
#endif

#endif
```

### 4.2 x86_64 实现头文件变更

| 文件 | 变更 | 说明 |
|------|------|------|
| `hw.h` | 新增 MMIO readb/w/l/q 内联 + 确认 nop() 已存在 | 现有端口 I/O 保持不变；`nop()` 在 `asm.h` 中 |
| `cpu.h` | 不再包含原子操作；新增 `arch_pause()` | 原子操作移到 arch/atomic.h。`arch_read_cr2()` 保留——仅在 arch/x86_64/trap.c 中使用 |
| `asm.h` | 无变更 | 保持 sti/cli/hlt/nop/io_mfence。`nop()` 被 `arch/io.h` 或直接 `__asm__("nop")` 引用 |
| `msr.h` | 无变更—保留 x86 专用 | 不再被通用代码包含 |
| `regs.h` | 无变更 | 通过 arch/thread.h 暴露 |
| `gate.h` | 无变更 | x86 代码直接包含 |
| `spinlock.h` | 无变更 | 架构特定 |

### 4.3 新文件: `arch/x86_64/`

| 新文件 | 内容 |
|--------|------|
| `arch/x86_64/irq.c` | 从 `intr/irq.c` 迁移：`irq_install()` + `Build_IRQ` + 所有 `set_intr_gate_raw` 调用 |

### 4.4 通用内核文件迁移

每个通用 `.c` 文件替换旧 include，并在必要时将裸汇编替换为 `arch_` 函数调用：

| 文件 | 旧 include / 裸汇编 | 新 include / 调用 |
|------|--------------------|--------------------|
| `kernel/main.c` | `rdmsr`/`wrmsr` EFER NXE, `cli`, `hlt` | `<arch/cpu.h>`, `<arch/irq.h>`; `arch_cpu_enable_nx()` |
| `intr/irq.c` | `<arch/x86_64/gate.h>`, `set_intr_gate_raw` | 移走 x86 代码 → `arch/x86_64/irq.c`；保留: `register_irq()` |
| `intr/dispatch.c` | `<arch/x86_64/gate.h>` (intr_handler_table) | `<arch/irq.h>` |
| `sched/smp.c` | `sti`, `hlt`, `pause`, `lgdt`, `lidt` | `<arch/cpu.h>`, `<arch/irq.h>`; `arch_cpu_halt/pause()`。**注意**: `lgdt`/`lidt` + `lretq` CS reload 是 x86 特有协议，保留裸汇编，加 `// x86 specific: AP GDT/IDT reload` 注释 |
| `sched/task.c` | 直接使用 `RSP & ~(STACK_SIZE-1)`、`switch_to` 宏、`__switch_to` 中的 `cli/sti/pushq` | 这些是架构特定的上下文切换协议——**不抽象**。任务调度器在架构层之上的概念通用，但 `__switch_to` 和 `get_current_task()` 在 Section 5 中列为架构特定 |
| `memory/tlb.c` | `pause`, `invlpg` | `<arch/cpu.h>`, `<arch/mmu.h>`; `arch_cpu_pause()`, `arch_flush_tlb_all()` |
| `memory/vmm.c` | `invlpg`, `flush_tlb()` CTR reload | `<arch/mmu.h>`; `arch_flush_tlb_page()`, `arch_flush_tlb_all()` |
| `memory/pmm.c` | `<arch/x86_64/string.h>` (仅为了 memset) | `<string.h>`（libc 通用版）。x86 优化的 `rep stosq` memset 在当前编译优化下没有可测量收益，移除它以消除架构依赖 |
| `driver/serial.c` | `pushfq;cli;popq`, `sti`, `pause`, `inb`/`outb` | `<arch/io.h>`, `<arch/cpu.h>`, `<arch/irq.h>` |
| `driver/keyboard.c` | `inb`/`outb`, `pause` | `<arch/io.h>`, `<arch/cpu.h>` |
| `driver/ahci.c` | `<arch/x86_64/asm.h>` (仅 `nop()`，在 `WAIT_WHILE` 宏中), `pause` | `<arch/io.h>`, `<arch/cpu.h>`; `arch_nop()`, `arch_cpu_pause()`. **注意**: `WAIT_WHILE` 宏中的 `nop()` 映射到 `arch_io.h` 的 `arch_nop()` |
| `driver/pci.c` | `ind`/`outd` | `<arch/io.h>` |
| `driver/pit.c` | `<arch/x86_64/hw.h>`, `asm.h` | 保留直接 include — PIT 是纯 x86 驱动，不在通用路径中执行。标记为 `// x86 specific driver` |
| `intr/pic/8259A.c` | `<arch/x86_64/hw.h>`, `asm.h` | 保留直接 include — 8259A PIC 是纯 x86 驱动 |
| `apic/lapic.c` | `<arch/x86_64/msr.h>`, `regs.h`, `cpuid.h`, `gate.h`, `asm.h` | `<arch/cpu.h>`, `<arch/irq.h>` |
| `apic/ioapic.c` | `<arch/x86_64/asm.h>`, `hw.h` | `<arch/io.h>` |
| `apic/ipi.c` | `<arch/x86_64/gate.h>` | `<arch/irq.h>` |
| `apic/lapic_timer.c` | `<arch/x86_64/gate.h>`, `regs.h` | `<arch/irq.h>` |
| `kernel/panic.c` | `cli; hlt` | `<arch/irq.h>`, `<arch/cpu.h>` |
| `percpu/percpu.c` | `<arch/x86_64/msr.h>`, `wrmsr(IA32_GS_BASE)` | `<arch/cpu.h>`; `arch_set_percpu_base()` |
| `arch/x86_64/trap.c` | 直接使用 `KERNEL_CS`/`USER_CS`；`movq %%cr2, %0`（line 79 expr + line 437 `do_page_fault`） | `<arch/segment.h>` (KERNEL_CS/USER_CS)。**CR2 读取保留裸汇编**——这是架构文件，`arch_virt_to_phys()` 供通用代码调用，而 `user_va_to_phys()` 位于该文件中作为 x86 内部函数 |
| `fs/elf.c` | 硬编码 `EM_X86_64=0x3E` | `<arch/elf.h>` |
| `fs/devfs.c` | `<arch/x86_64/cpu.h>` (仅为了 `/dev/random` 中的 `rdtsc()`) | `<arch/cpu.h>`; `arch_cycle_counter()` |

### 4.5 头文件 include 路径迁移

受影响的内核头文件（非 `.c` 文件）也需要更新其 include：

| 头文件 | 旧 include | 新 include |
|--------|-----------|-----------|
| `task.h` | `<arch/x86_64/cpu.h>` (NR_CPUS, atomics), `<arch/x86_64/regs.h>` (pt_regs_t); 裸定义 `KERNEL_CS`/`USER_CS`/`KERNEL_DS`/`USER_DS` | `<arch/cpu.h>` (for NR_CPUS), `<arch/thread.h>` (pt_regs_t), `<arch/segment.h>` (CS/DS 常量) |
| `interrupt.h` | `<arch/x86_64/gate.h>` (intr_handler_fn) | `<arch/irq.h>` |
| `percpu.h` | 内联 `movq %%gs:0`；裸 `#ifndef NR_CPUS` 检查 | `<arch/percpu.h>`; `arch_this_cpu()`；NR_CPUS 检查改为包含 `<arch/cpu.h>`（或独立的 `cpumask.h`） |
| `vmm.h` | `flush_tlb()` 宏 (CR3 reload), `switch_tlb()` 宏 (CR3 write) | `<arch/mmu.h>`; `arch_flush_tlb_all()`, `arch_switch_mm()` |
| `trace.h` | `<arch/x86_64/regs.h>` | `<arch/thread.h>` |
| `wait.h` | `<arch/x86_64/regs.h>` | `<arch/thread.h>` |
| `file.h` | 无 arch 依赖（可能通过 task.h 间接） | 检查确认，可能需要更新为 `<arch/thread.h>` |

### 4.6 NR_CPUS 的显式依赖链

当前 chain: `percpu.h` → `#ifndef NR_CPUS → #error` → 依赖 `arch/x86_64/cpu.h` 定义 NR_CPUS。迁移后:

```
task.h → <arch/cpu.h> → <arch/x86_64/cpu.h> → `#define NR_CPUS 8`
percpu.h → 移除 bare `#ifndef NR_CPUS #error` → 改为依赖 task.h 已经包含的 arch/cpu.h
```

或更简洁：将 `#define NR_CPUS 8` 移到 `include/kernel/cpumask.h`，然后 task.h 和 percpu.h 都包含它。NR_CPUS 是架构中立的配置常量。

### 4.7 构建系统

kernel/Makefile 中当前的 `ALL_CFLAGS` += `-I$(KERNEL_HEADERS)` 已经包含 `kernel/include/`。
因此：

- `#include <kernel/arch/cpu.h>` → 找到 `kernel/include/kernel/arch/cpu.h` ✅
- `arch/cpu.h` 内的 `#include <kernel/arch/x86_64/cpu.h>` → 找到 `kernel/include/kernel/arch/x86_64/cpu.h` ✅

**不需要新增 `-I` 路径**。唯一的构建变更是将根 Makefile 中的硬编码 `x86_64-unknown-none` 改为：
```makefile
ARCH ?= x86_64
export CC = clang -target $(ARCH)-unknown-none
export LD = ld.lld -m elf_$(ARCH)
```
但此变更仅在需要支持非 x86 编译目标时才有意义。在本设计中，根 Makefile 不变。

---

## 5. 不抽象的内容

以下子系统保持为 x86 特定，不创建通用接口。此外，一些在"概念上通用但实现方案架构特定"的核心调度原语也在此列出。

| 子系统 | 理由 |
|--------|------|
| IDT/GDT 描述符 (`gate.h`) | aarch64 使用 vector table，概念完全不同 |
| INTR_SAVE_ALL / DEFINE_INTR_STUB / REGISTER_INTR_HANDLER | x86 硬件压栈 + 软件保存独特的汇编协议 |
| `tss_struct` 和 `init_tss[]` | x86 TSS/IST；aarch64 通过 SP_EL0/SP_EL1 切换栈 |
| `trampoline.S` (AP 启动) | x86: INIT-SIPI-SIPI；aarch64: PSCI |
| `head.S` / `entry.S` | 启动代码 + 异常入口永远架构特定 |
| `linker.ld` | 链接脚本架构特定 |
| `cpuid.h` | x86 CPUID 指令；aarch64 对应寄存器不同 |
| `msr.h` | MSR 架构特有；通用代码封装为语义函数（如 `arch_cpu_enable_nx()`） |
| `spinlock.h` | 自旋锁本质上架构特定（x86: `lock xchg`；aarch64: `ldaxr`/`stlxr`） |
| `apic/` + `intr/pic/` 目录 | 中断控制器架构特定 — aarch64 使用 GIC-400 |
| `kallsyms` 栈回溯 | 回溯算法通用，但 DWARF2 解析架构相关 |
| **`get_current_task()` 宏** (RSP 掩码) | `RSP & ~(STACK_SIZE-1)` 是 x86 内核栈布局假设。aarch64 可能用 SP_EL1 或专用寄存器 |
| **`switch_to` 宏** + `__switch_to()` | `cli/sti`、`pushq callee-saved`、`jmp` 是 x86 调用约定。aarch64 用 `stp x19-x30`/`sp` |
| **`kernel_thread_func`** (x86 asm thunk) | 调用参数传递约定（RDI = fn, RSI = arg）架构特定 |
| **`user_va_to_phys()`** | x86 页表遍历 + PTE flags 解析混合实现——保留在 `arch/x86_64/trap.c` 中 |
| `percpu_t.tlb_wanted`/`tlb_ack` | TLB shootdown 实现细节。aarch64 端口可能用 IPI + TLBI 广播，仍需要 ACK |
| `KERNEL_CS/DS`/`USER_CS/DS` | GDT 选择子，通过 `arch/segment.h` 做类型隔离但不抽象 |

---

## 6. 迁移顺序

### Phase A: 创建通用头文件（纯新增，无风险）

创建 `include/kernel/arch/{io,cpu,atomic,irq,barrier,mmu,segment,thread,percpu,elf,cache}.h`

每个头文件通过 `#ifdef __x86_64__` 包含 x86_64 实现。不修改任何现有代码。

**验证**: `make clean && make` 0 警告。

### Phase B: x86_64 实现补充

在 `include/kernel/arch/x86_64/` 新增/修改少量函数：
- `hw.h`: 新增 MMIO `readb`/`readw`/`readl`/`readq`/`writeb`/`writew`/`writel`/`writeq`
- `cpu.h`: 新增 `arch_pause()`
- 新增 `arch/x86_64/irq.c`（从 `intr/irq.c` 抽出 x86 特定代码：`irq_install()` + `Build_IRQ` + 所有 `set_intr_gate_raw` 调用）
- 确认 `asm.h` 中已有 `nop()` 宏

**验证**: `make clean && make` 0 警告。

### Phase C: 迁移通用内核 `.c` 文件

按依赖顺序逐个迁移。每个文件：
1. 替换 include
2. 替换裸汇编为 `arch_` 函数调用
3. 注意 `sched/smp.c` 中的 `lgdt/lidt` + `lretq` CS reload 是 x86 特有——保留裸汇编，加注释标记
4. 注意 `sched/task.c` 的 `switch_to` 宏和 `__switch_to()` — 不抽象，Section 5 已记录
5. `driver/pit.c` 和 `intr/pic/8259A.c` 保留直接 include

每迁移 3-4 个文件后 `make` 编译验证。

**`sched/task.c` 的 `get_current_task()` 怎么办？**
它通过 `RSP & ~(STACK_SIZE-1)` 在 `task.h` 中定义为内联函数/宏，被许多调度文件使用。**保持原位**，不做抽象。这个宏是 kernel 栈布局的核心假设，架构差异会影响整个内存布局。新架构会替换这个宏的新定义。

**验证**: 每 3-4 个文件后 `make` 编译。

### Phase D: 迁移受影响的内核头文件

更新 `task.h`、`interrupt.h`、`percpu.h`、`vmm.h`、`trace.h`、`wait.h`、`file.h` 的 include。

NR_CPUS 依赖链处理：选择一种方案执行——从 `arch/x86_64/cpu.h` 提取 #define 到独立 `include/kernel/cpumask.h`，或者继续通过 `task.h → <arch/cpu.h> → <arch/x86_64/cpu.h>` 链间接提供。

这是最高风险阶段——因为每个 `.c` 文件都间接包含这些头。

**验证**: `make clean && make` + `make test-syscall`。

### Phase E: 验证

```bash
make clean && make                         # 无警告编译
make run                                   # 启动到 shell
make test-syscall                          # 70/70 PASS
make DEBUG_CHANNELS=sched,irq,mm run       # SMP + TLB shootdown 高覆盖
grep -r '__asm__' kernel/memory/ kernel/sched/ kernel/driver/ kernel/kernel/ kernel/fs/ kernel/intr/apic/ kernel/intr/ kernel/tty/ --include='*.c' | grep -v 'arch/x86_64/' | grep -v '.d:'
# 确认通用代码中无裸汇编（仅允许在 arch/x86_64/ 内）
```

### Phase F: aarch64 桩（可选）

为 `include/kernel/arch/aarch64/` 创建空桩头文件，端口 I/O 用 `__builtin_trap()`，使 `ARCH=aarch64` 编译到链接期再失败。这提供了一个早期的"移植起点"信号。

---

## 7. 测试策略

| 检查项 | 方法 | 触发条件 |
|--------|------|---------|
| 编译无警告 | `make clean && make` | 每个 Phase（A/B/C/D） |
| 启动到 shell | `make run` (5s 超时) | Phase E |
| syscall 回归 | `make test-syscall` (70/70) | Phase E |
| SMP + 中断 + MMIO 覆盖 | `make DEBUG_CHANNELS=sched,irq,mm run` | Phase E |
| 通用代码零裸汇编 | `grep -r '__asm__' kernel/... --include='*.c' | grep -v arch/x86_64/` | Phase E |
| git bisectable | 每个子 Phase 后 commit | 全程 |

> 额外建议（CI）：如有 `.github/workflows/ci.yml`，在 Phase E 后添加一个 `ARCH=x86_64 make clean && make` 构建 job（如果还没有的话）。

---

## 8. 工作量重估

| 阶段 | 文件数 | 估算耗时 |
|------|--------|---------|
| A: 通用头文件 | 11 新增 | 45 min |
| B: x86 实现补充 | ~6 修改 + 1 新增 (arch/x86_64/irq.c) | 45 min |
| C: 迁移通用代码 | ~18 `.c` 文件 + devfs.c + pmm.c fix | 2.5 hr |
| D: 迁移头文件 | ~8 头文件 + NR_CPUS 处理 | 1.5 hr |
| E: 验证 | — | 30 min |
| F: aarch64 桩 | ~11 桩 | 20 min |
| **合计** | **~50 文件** | **~6-9 小时（1-1.5 天）** |

---

## 9. 设计决策记录

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 分发机制 | `#ifdef __x86_64__` 条件包含 | clang 自动定义，零额外构建复杂度 |
| 2 | 原子操作拆分 | 独立 `arch/atomic.h` | `cpu.h` 已承载过多；原子操作用途独立 |
| 3 | MSR 不进入通用接口 | `arch_cpu_enable_nx()` 替代 `rdmsr/wrmsr` | MSR 编号是 x86 ABI 细节 |
| 4 | aarch64 端口 I/O | `__builtin_trap()` 而非 `#error` | 仅 x86 路径执行的代码也应能编译 |
| 5 | `arch_irq_state_t` | `uint64_t` 别名 | x86: RFLAGS 64-bit；aarch64: DAIF 只需 4 位。选择 64 位而非 `uint32_t` 的理由：简单性 > 类型窄化，且避免了高位被截断的隐晦 bug。aarch64 端口在实现时可以用更窄的类型或 struct |
| 6 | `arch/mmu.h` 范围 | 暴露 TLB flush + switch_mm + virt_to_phys；不含标志解析 | page table walk flags 解析留在架构代码中；`arch_virt_to_phys()` 返回含偏移的完整物理地址 |
| 7 | `intr/irq.c` 拆分 | 把 `irq_install()`/`Build_IRQ` 移到 `arch/x86_64/irq.c` | IDT 安装是纯 x86 代码；通用 `intr/` 只做查表分发 |
| 8 | `percpu_t.apic_id` | 改为 `uint32_t arch_processor_id` | aarch64 使用 MPIDR_EL1，名称应架构中立 |
| 9 | 构建系统 | 不需新增 `-I` 路径 | 现有 `-I$(KERNEL_HEADERS)` 已覆盖 |
| 10 | `KERNEL_CS/DS`/`USER_CS/DS` | 移到 `arch/segment.h` | 当前在 task.h 中硬编码，应架构隔离 |
| 11 | `get_current_task()`/`switch_to`/`__switch_to` | 不抽象 | 上下文切换协议在架构间差异过大——RSP 掩码、callee-saved regs、返回协议完全不同。将它们标记为架构特定比抽象更有价值 |
| 12 | `arch_set_percpu_base()` 命名 | 不包含 `gs` | GS 是 x86 MSR 名；语义化命名使 aarch64 实现自然对应 `tpidr_el1` |
| 13 | `percpu_t.tlb_wanted`/`tlb_ack` | 保留 | 即使 aarch64 使用不同的 TLB 维护协议（在广播 TLBI 时），ACK 同步是通用需求 |
| 14 | `driver/pit.c` + `intr/pic/8259A.c` | 保留 arch/x86_64/ 直接 include | 纯 x86 驱动，aarch64 不会编译它们 |
| 15 | `pmm.c` `<string.h>` | 切换到通用 `<string.h>` | x86 优化的 `rep stosq` memset 在当前编译优化下无显著收益，且是唯一依赖 `<arch/x86_64/string.h>` 的通用文件 |
| 16 | `arch_nop()` | 放入 `arch/io.h` | `nop()` 主要用例在 `driver/ahci.c` 的 `WAIT_WHILE` 循环中，紧邻 I/O 操作 |
