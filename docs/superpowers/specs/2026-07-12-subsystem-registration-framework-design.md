# 子系统注册框架设计

**日期**: 2026-07-12  
**状态**: 定稿  
**动机**: 支持扩展 arch 时，不同硬件架构可以自行注册合适的硬件子系统，不再硬编码在 `kernel_main()` 中。

---

## 1. 问题描述

当前 `kernel_main()` 硬编码了 x86_64 特有的初始化调用序列：

```c
apic_init();
pic_init();
pit_init();
keyboard_init();
ahci_init();
// ...
```

加新 arch（如 riscv64、aarch64）必须修改 `main.c`，且 init 序列和具体 arch 耦合。需要一个轻量框架，让每个 arch 声明自己的硬件子系统集合。

## 2. 设计原则

- **零或微运行时开销** — 注册就是写几个指针
- **与 softirq 同风格** — 运行时 `register_*()` 调用，不引入 GCC attribute / ELF section 依赖
- **按 phase 分组** — 保持 `kernel_main` 已有的 init 顺序语义
- **每 arch 自声明** — 新增 arch 只需一个 `.c` 文件 + 若干 `register_subsys()` 调用

## 3. 核心 API

```c
// kernel/include/kernel/subsys.h

#ifndef _KERNEL_SUBSYS_H
#define _KERNEL_SUBSYS_H

#include <stdint.h>

// ── Phase 编号（对应 kernel_main 的 9 个阶段） ──────────

// Phase 1-2（CPU 基础设施、内存子框架）保持硬编码——
// 在 serial_printk 可用 + 堆分配就绪之前，不适合框架。

#define SUBSYS_PHASE_3   3   // 中断控制器 (APIC/PIC/GIC/PLIC)
#define SUBSYS_PHASE_4   4   // 定时器 (PIT/LAPIC/Generic Timer)
#define SUBSYS_PHASE_5   5   // 设备 IRQ 注册 (键盘、串口 IRQ)
#define SUBSYS_PHASE_6   6   // 存储控制器 + 文件系统

// Phase 7-9（TTY/SMP/调度器）保持硬编码——
// TTY 创建需要明确 console 分配，SMP 启动涉及 trampoline + percpu 依赖。

// ── Flags ────────────────────────────────────────────────

#define SUBSYS_FLAG_OPTIONAL   (1 << 0)   // init 失败不视为致命

// ── 入口类型 ─────────────────────────────────────────────

typedef struct {
    const char *name;           // 子系统名称，用于日志/调试
    int  (*init)(void);         // 初始化函数，0=成功，非0=失败
    int   phase;                // 所属 Phase (3-6)
    uint32_t flags;
    // private:
    int   initialized;          // 0=未执行, 1=成功, <0=失败
} subsys_entry_t;

// ── 函数声明 ─────────────────────────────────────────────

int  register_subsys(const char *name, int (*init)(void),
                     int phase, uint32_t flags);
void subsys_init_all(void);
void subsys_init_phase(int phase);
int  subsys_status(const char *name);

#endif
```

### Per-CPU 二次 init API

```c
// kernel/include/kernel/subsys.h（追加）

typedef struct {
    const char *name;
    int  (*init_percpu)(int cpu_id);   // 每个在线 CPU 上执行一次
    uint32_t flags;
    // private:
    int initialized;
} subsys_percpu_entry_t;

int  register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags);
void subsys_init_percpu(void);
```

### 实现

```c
// kernel/subsys/subsys.c

#include <kernel/subsys.h>
#include <kernel/printk.h>
#include <kernel/percpu.h>
#include <string.h>

#define MAX_SUBSYS        64
#define MAX_SUBSYS_PERCPU 16

static subsys_entry_t        subsys_table[MAX_SUBSYS];
static int                   subsys_count = 0;
static subsys_percpu_entry_t subsys_percpu_table[MAX_SUBSYS_PERCPU];
static int                   subsys_percpu_count = 0;

// ── 注册 ────────────────────────────────────────────────────────

int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags)
{
    if (!name || !init || subsys_count >= MAX_SUBSYS)
        return -1;
    subsys_entry_t *e = &subsys_table[subsys_count];
    e->name   = name;
    e->init   = init;
    e->phase  = phase;
    e->flags  = flags;
    e->initialized = 0;
    subsys_count++;
    return 0;
}

int register_subsys_percpu(const char *name,
                           int (*init_percpu)(int cpu_id),
                           uint32_t flags)
{
    if (!name || !init_percpu || subsys_percpu_count >= MAX_SUBSYS_PERCPU)
        return -1;
    subsys_percpu_entry_t *e = &subsys_percpu_table[subsys_percpu_count];
    e->name        = name;
    e->init_percpu = init_percpu;
    e->flags       = flags;
    e->initialized = 0;
    subsys_percpu_count++;
    return 0;
}

// ── 执行 ────────────────────────────────────────────────────────

void subsys_init_phase(int phase)
{
    for (int i = 0; i < subsys_count; i++) {
        subsys_entry_t *e = &subsys_table[i];
        if (e->phase != phase || e->initialized != 0)
            continue;

        serial_printk("subsys: init  %s ... ", e->name);
        int ret = e->init();
        e->initialized = (ret == 0) ? 1 : -1;

        if (ret == 0) {
            serial_printk("ok\n");
        } else if (e->flags & SUBSYS_FLAG_OPTIONAL) {
            serial_printk("SKIP (optional, ret=%d)\n", ret);
        } else {
            serial_printk("FAIL (ret=%d)\n", ret);
        }
    }
}

void subsys_init_all(void)
{
    for (int phase = 3; phase <= 6; phase++)
        subsys_init_phase(phase);
}

void subsys_init_percpu(void)
{
    // 对每个子系统的 percpu init，在所有在线 CPU 上执行
    for (int i = 0; i < subsys_percpu_count; i++) {
        subsys_percpu_entry_t *e = &subsys_percpu_table[i];
        if (e->initialized)
            continue;

        for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
            serial_printk("subsys: percpu %s cpu=%u ... ", e->name, cpu);
            int ret = e->init_percpu((int)cpu);
            if (ret != 0 && !(e->flags & SUBSYS_FLAG_OPTIONAL)) {
                serial_printk("FAIL (ret=%d)\n", ret);
            } else {
                serial_printk("ok\n");
            }
        }
        e->initialized = 1;
    }
}

// ── 查询 ────────────────────────────────────────────────────────

int subsys_status(const char *name)
{
    for (int i = 0; i < subsys_count; i++)
        if (strcmp(subsys_table[i].name, name) == 0)
            return subsys_table[i].initialized;
    return -999;   // 未注册
}
```

## 4. Arch 侧接口

每个 arch **按需**提供以下两个函数，分别在各自的文件里实现：

| 函数 | 文件 | 必须？ | 用途 |
|------|------|--------|------|
| `void arch_register_subsys(void)` | `kernel/arch/<arch>/subsys.c` | ✅ 是 | 注册 Phase 3-6 的子系统 |
| `void arch_register_subsys_percpu(void)` | `kernel/arch/<arch>/subsys_percpu.c` | ❌ 可选 | 注册 per-CPU 二次 init 子系统 |

### x86_64 示例

```c
// kernel/arch/x86_64/subsys.c

#include <kernel/subsys.h>
#include <kernel/apic.h>
#include <device/pic.h>
#include <driver/pit.h>
#include <driver/keyboard.h>
#include <driver/serial.h>
#include <device/timer.h>

extern int ahci_init(void);

void arch_register_subsys(void)
{
    // Phase 3: 中断控制器
    register_subsys("apic", apic_init,           SUBSYS_PHASE_3, 0);
    register_subsys("pic",  pic_init,            SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);

    // Phase 4: 定时器
    register_subsys("timer",      timer_init,     SUBSYS_PHASE_4, 0);
    register_subsys("pit",        pit_init,       SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    register_subsys("lapic-timer", lapic_timer_init, SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);

    // Phase 5: 设备 IRQ
    register_subsys("keyboard", keyboard_init,   SUBSYS_PHASE_5, SUBSYS_FLAG_OPTIONAL);
    register_subsys("serial",   init_serial_irq, SUBSYS_PHASE_5, 0);

    // Phase 6: 存储
    register_subsys("ahci", ahci_init,           SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
```

```c
// kernel/arch/x86_64/subsys_percpu.c

#include <kernel/subsys.h>
#include <kernel/apic.h>

extern int lapic_timer_start(int freq_hz);

static int _lapic_timer_percpu(int cpu_id)
{
    (void)cpu_id;
    lapic_timer_start(100);
    return 0;
}

void arch_register_subsys_percpu(void)
{
    register_subsys_percpu("lapic-timer-start", _lapic_timer_percpu, 0);
}
```

### riscv64 示例（未来）

```c
// kernel/arch/riscv64/subsys.c

void arch_register_subsys(void)
{
    register_subsys("plic",        plic_init,         SUBSYS_PHASE_3, 0);
    register_subsys("timer",       timer_init,        SUBSYS_PHASE_4, 0);
    register_subsys("riscv-timer", riscv_timer_init,  SUBSYS_PHASE_4, 0);
    register_subsys("ns16550",     ns16550_irq_init,  SUBSYS_PHASE_5, 0);
    register_subsys("virtio-blk",  virtio_blk_init,   SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
```

## 5. kernel_main 集成

```c
int kernel_main(struct BOOT_INFO *bootinfo)
{
    // ═══ 0. Stack canary ═══════════════════════════════════
    __stack_chk_guard = rdtsc() ^ 0xDEADBEEFCAFEBABE;

    // ═══ 1. CPU + interrupt infrastructure ════════════════
    // load_TR, set_tss64, sys_vector_install, irq_install,
    // init_serial, EFER NXE
    // （保持硬编码 — 架构无关，且 serial_printk 依赖 init_serial）

    // ═══ 2. Memory subsystem ══════════════════════════════
    // pmm_init, vmm_init, framebuffer_init
    // （保持硬编码 — 堆分配器必须在任何子系统 init 之前就绪）

    // ═══ 3-6. Subsystem framework ═════════════════════════
    arch_register_subsys();
    subsys_init_all();
    // 代替之前的:
    //   apic_init(); pic_init(); pit_init(); timer_init();
    //   lapic_timer_init(); keyboard_init();
    //   init_serial_irq(); ahci_init();

    // ═══ 7. Console TTY ═══════════════════════════════════
    // tty_alloc, 注册到 keyboard/serial/devfs
    // （保持硬编码 — TTY 是架构无关的抽象层）

    // ═══ 8. Per-CPU + SMP ═════════════════════════════════
    // percpu_init, percpu_install_gs(BSP), smp_boot_aps

    arch_register_subsys_percpu();      // 注册 per-CPU 二次 init
    subsys_init_percpu();              // 在每个在线 CPU 上执行

    // ═══ 9. Scheduler + user-space init ═══════════════════
    // task_init
}
```

### 依赖关系图

```
Phase 3 ─── apic ──> pic (optional)
               │
               └───────> ioapic (apic_init 内部调用)
               │
Phase 4 ─── timer ──> pit (optional) ──> lapic-timer (optional)
                                       (lapic_timer_calibrate 依赖 PIT)
Phase 5 ─── keyboard (optional)
         ─── serial
Phase 6 ─── ahci (optional)

Phase 8 ─── lapic-timer-start (per-CPU)
         ─── tlb-percpu (未来)
```

同一个 Phase 内的子系统**按注册顺序**执行，所以依赖关系通过注册顺序保证。

## 6. Makefile 集成

框架源文件：

```makefile
# kernel/Makefile（改动点）
KERNEL_SRCS += kernel/subsys/subsys.c
```

arch 文件通过现有的通配符自动纳入：

```makefile
# kernel/arch/x86_64/make.config
ARCH_SRCS += kernel/arch/x86_64/subsys.c
ARCH_SRCS += kernel/arch/x86_64/subsys_percpu.c    # 可选文件不存在也不会报错
```

BusyBox 或第三方代码不受影响。

## 7. 边界情况

| 场景 | 行为 |
|------|------|
| 子系统 init 返回非 0 | 日志记录 FAIL / SKIP；**不 panic**，除非是 mandatory（无 `SUBSYS_FLAG_OPTIONAL`） |
| 某子系统未注册 | 框架跳过，不存在即不执行 |
| per-CPU init 的 `cpu_id` 超出范围 | 由 `subsys_init_percpu` 循环控制，不会传给不存在的 CPU |
| 同一 name 注册两次 | 两次都会写入 `subsys_table`，执行两次。由 `arch_register_*` 的开发者在 code review 时避免 |
| 跨 arch 同名子系统 | 互不影响 — 每个 arch 编译自己的一份 `subsys.c` |
| `subsys_init_all` 被调用两次 | 第二次全部 skip（`initialized != 0` 检查） |

## 8. 不加的功能（YAGNI）

- **自动依赖解析** — 当前依赖链是线性的，注册顺序足够。未来需要 DAG 解析时可扩展（在 `register_subsys` 中添加 `const char *const *deps`）。
- **ELF section 自动收集** — 不引入 GCC `__attribute__((section))` + 链接脚本排序，运行时注册更简洁且与现有 softirq 风格一致。
- **热插拔** — 子系统注册在 boot 时一次性完成，没有运行时卸载需求。
- **per-CPU init 的并行执行** — 当前 `subsys_init_percpu` 串行调用每个 CPU。AP 数量很少（通常 1-8），串行足够。

## 9. 文件清单

| 文件 | 动作 | 说明 |
|------|------|------|
| `kernel/include/kernel/subsys.h` | **新建** | 框架 API 头文件 |
| `kernel/subsys/subsys.c` | **新建** | 框架实现 |
| `kernel/arch/x86_64/subsys.c` | **新建** | x86_64 子系统注册 |
| `kernel/arch/x86_64/subsys_percpu.c` | **新建** | x86_64 per-CPU 子系统注册 |
| `kernel/kernel/main.c` | **修改** | 替换 Phase 3-6 的直接调用为框架调用；在 Phase 8 增加 `subsys_init_percpu()` |
| `kernel/Makefile` | **修改** | 添加 `kernel/subsys/subsys.c` |
| `kernel/arch/x86_64/make.config` | **修改** | 添加 `subsys.c` 和 `subsys_percpu.c` |
