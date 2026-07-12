# 子系统注册框架实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增运行时子系统注册框架，将 `kernel_main` 中 Phase 3-6 的硬编码初始化调用替换为框架驱动，使新增 Arch 时只需写自己的 `arch_register_subsys()` 文件。

**Architecture:** 借鉴 softirq 的运行时注册模式。框架提供 `register_subsys(name, init_fn, phase, flags)` 和 `subsys_init_all()`。每个 Arch 在 `subsys.c` 文件里调用 `register_subsys()` 注册自己的硬件子系统。所有当前 init 函数返回 `void`，需要编写返回 `int` 的包装函数。

**Tech Stack:** C (kernel), Makefile

## 全局约束

- 子系统 init 函数签名为 `int (*)(void)`，返回 0=成功，非0=失败
- per-CPU init 函数签名为 `int (*)(int cpu_id)`
- 当前所有 init 函数返回 `void` — 需要包装函数适配
- `apic_init` 接受 `uint64_t rsdp_phys` 参数 — 需要包装函数 + 全局变量传递该值
- `kernel_main` 中 Phase 8 的 `lapic_timer_start(100)` 移入 per-CPU 子系统注册

---

## 文件清单

| 文件 | 操作 | 职责 |
|------|------|------|
| `kernel/include/kernel/subsys.h` | **新建** | 框架 API 头文件 |
| `kernel/subsys/subsys.c` | **新建** | 框架实现（注册表 + 按 phase 执行） |
| `kernel/arch/x86_64/subsys.c` | **新建** | x86_64 的 Phase 3-6 子系统注册 |
| `kernel/arch/x86_64/subsys_percpu.c` | **新建** | x86_64 的 per-CPU 子系统注册 |
| `kernel/kernel/main.c` | **修改** | 替换 Phase 3-6 调用；Phase 8 增加 `subsys_init_percpu` |
| `kernel/Makefile` | **修改** | 添加 `subsys/*.c` 通配符 |

---

### Task 1: 创建 `kernel/include/kernel/subsys.h`

**Files:**
- Create: `kernel/include/kernel/subsys.h`

**Interfaces:**
- Produces: `SUBSYS_PHASE_3/4/5/6` 宏定义、`SUBSYS_FLAG_OPTIONAL` 宏、`subsys_entry_t` 和 `subsys_percpu_entry_t` 结构体、`register_subsys()` / `subsys_init_all()` / `subsys_init_phase()` / `subsys_init_percpu()` / `subsys_status()` / `register_subsys_percpu()` 函数声明

- [ ] **Step 1: 创建 header 文件**

```c
// kernel/include/kernel/subsys.h
#ifndef _KERNEL_SUBSYS_H
#define _KERNEL_SUBSYS_H

#include <stdint.h>

// ── Phase 编号 ────────────────────────────────────────────
// Phase 1-2（CPU 基础设施 / 内存）和 Phase 7-9（TTY / SMP / 调度器）
// 保持硬编码在 kernel_main 中。
#define SUBSYS_PHASE_3   3   // 中断控制器 (APIC/PIC/GIC/PLIC)
#define SUBSYS_PHASE_4   4   // 定时器 (PIT/LAPIC/Generic Timer)
#define SUBSYS_PHASE_5   5   // 设备 IRQ 注册 (键盘、串口 IRQ)
#define SUBSYS_PHASE_6   6   // 存储控制器 (AHCI/VirtIO-BLK)

// ── Flags ─────────────────────────────────────────────────
#define SUBSYS_FLAG_OPTIONAL   (1 << 0)   // init 失败不视为致命

// ── 子系统入口（BSP 一次性 init） ────────────────────────
typedef struct {
    const char *name;           // 子系统名称，用于日志/调试
    int  (*init)(void);         // 初始化函数，0=成功，非0=失败
    int   phase;                // 所属 Phase (3-6)
    uint32_t flags;
    // private:
    int   initialized;          // 0=未执行, 1=成功, <0=失败
} subsys_entry_t;

// ── 子系统入口（per-CPU 二次 init） ──────────────────────
typedef struct {
    const char *name;
    int  (*init_percpu)(int cpu_id);   // 每个在线 CPU 上执行一次
    uint32_t flags;
    // private:
    int initialized;
} subsys_percpu_entry_t;

// ── 函数声明 ─────────────────────────────────────────────
int  register_subsys(const char *name, int (*init)(void),
                     int phase, uint32_t flags);
int  register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags);
void subsys_init_all(void);
void subsys_init_phase(int phase);
void subsys_init_percpu(void);
int  subsys_status(const char *name);

#endif
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/kernel/subsys.h
git commit -m "feat(subsys): add subsys framework API header"
```

---

### Task 2: 创建 `kernel/subsys/subsys.c`

**Files:**
- Create: `kernel/subsys/subsys.c`

**Interfaces:**
- Consumes: `#include <kernel/subsys.h>`, `#include <kernel/printk.h>`, `#include <kernel/percpu.h>`, `#include <string.h>`
- Produces: `register_subsys()`(将 entry 写入 `subsys_table[]`)、`register_subsys_percpu()`(将 entry 写入 `subsys_percpu_table[]`)、`subsys_init_phase()`(遍历 table 执行指定 phase 的子系统)、`subsys_init_all()`(循环 phase 3→6)、`subsys_init_percpu()`(遍历 percpu table，对每个在线 CPU 执行 init 函数)、`subsys_status()`(按 name 查询状态)

- [ ] **Step 1: 创建框架实现文件**

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

// ── 注册（BSP 一次性 init） ───────────────────────────────

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

// ── 注册（per-CPU 二次 init） ─────────────────────────────

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

// ── 执行（BSP 一次性 init，按 phase） ─────────────────────

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

// ── 执行（per-CPU 二次 init） ─────────────────────────────

void subsys_init_percpu(void)
{
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

// ── 查询 ─────────────────────────────────────────────────

int subsys_status(const char *name)
{
    for (int i = 0; i < subsys_count; i++)
        if (strcmp(subsys_table[i].name, name) == 0)
            return subsys_table[i].initialized;
    return -999;
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/subsys/subsys.c
git commit -m "feat(subsys): add subsys framework implementation"
```

---

### Task 3: 创建 `kernel/arch/x86_64/subsys.c`

**Files:**
- Create: `kernel/arch/x86_64/subsys.c`

**Interfaces:**
- Consumes: `register_subsys()`(来自 Task 1)、`apic_init(uint64_t)`、`pic_init()`、`timer_init()`、`pit_init()`、`lapic_timer_init()`、`keyboard_init()`、`init_serial_irq()`、`ahci_init()`
- Produces: `void arch_register_subsys(void)`

**注意点：**
- `apic_init()` 接受 `uint64_t rsdp_phys` 参数。需要一个全局变量 `arch_boot_rsdp` 在 `kernel_main` 中设置，包装函数读取该值后调用 `apic_init`。
- 当前所有 init 函数返回 `void`，需要返回 `int` 的包装函数。

- [ ] **Step 1: 创建文件**

```c
// kernel/arch/x86_64/subsys.c

#include <kernel/subsys.h>
#include <kernel/apic.h>
#include <device/pic.h>
#include <device/timer.h>
#include <driver/pit.h>
#include <driver/keyboard.h>
#include <driver/serial.h>
#include <driver/ahci.h>

// ── RSDP 地址（由 kernel_main 在调用 arch_register_subsys 前设置） ──
uint64_t arch_boot_rsdp = 0;

// ── 包装函数（void → int (*)(void)） ──────────────────────

static int _apic_init_wrapper(void)
{
    apic_init(arch_boot_rsdp);
    return 0;
}

static int _pic_init_wrapper(void)
{
    pic_init();
    return 0;
}

static int _timer_init_wrapper(void)
{
    timer_init();
    return 0;
}

static int _pit_init_wrapper(void)
{
    pit_init();
    return 0;
}

static int _lapic_timer_init_wrapper(void)
{
    lapic_timer_init();
    return 0;
}

static int _keyboard_init_wrapper(void)
{
    keyboard_init();
    return 0;
}

static int _init_serial_irq_wrapper(void)
{
    init_serial_irq();
    return 0;
}

static int _ahci_init_wrapper(void)
{
    ahci_init();
    return 0;
}

// ── Arch 注册入口 ─────────────────────────────────────────

void arch_register_subsys(void)
{
    // Phase 3: 中断控制器
    register_subsys("apic", _apic_init_wrapper,           SUBSYS_PHASE_3, 0);
    register_subsys("pic",  _pic_init_wrapper,            SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);

    // Phase 4: 定时器
    register_subsys("timer",      _timer_init_wrapper,      SUBSYS_PHASE_4, 0);
    register_subsys("pit",        _pit_init_wrapper,        SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    register_subsys("lapic-timer", _lapic_timer_init_wrapper, SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);

    // Phase 5: 设备 IRQ
    register_subsys("keyboard", _keyboard_init_wrapper,    SUBSYS_PHASE_5, SUBSYS_FLAG_OPTIONAL);
    register_subsys("serial",   _init_serial_irq_wrapper,  SUBSYS_PHASE_5, 0);

    // Phase 6: 存储
    register_subsys("ahci", _ahci_init_wrapper,            SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/arch/x86_64/subsys.c
git commit -m "feat(subsys): add x86_64 subsystem registration"
```

---

### Task 4: 创建 `kernel/arch/x86_64/subsys_percpu.c`

**Files:**
- Create: `kernel/arch/x86_64/subsys_percpu.c`

**Interfaces:**
- Consumes: `register_subsys_percpu()`(来自 Task 1)、`lapic_timer_start(uint32_t)`
- Produces: `void arch_register_subsys_percpu(void)`

- [ ] **Step 1: 创建 per-CPU 注册文件**

```c
// kernel/arch/x86_64/subsys_percpu.c

#include <kernel/subsys.h>
#include <kernel/apic.h>

static int _lapic_timer_start_percpu(int cpu_id)
{
    (void)cpu_id;
    lapic_timer_start(100);
    return 0;
}

void arch_register_subsys_percpu(void)
{
    register_subsys_percpu("lapic-timer-start", _lapic_timer_start_percpu, 0);
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/arch/x86_64/subsys_percpu.c
git commit -m "feat(subsys): add x86_64 per-CPU subsystem registration"
```

---

### Task 5: 修改 `kernel/kernel/main.c`

**Files:**
- Modify: `kernel/kernel/main.c`

**改动：**
1. 在 includes 区域末尾添加 `#include <kernel/subsys.h>`
2. 移除不需要的 includes: `<device/pic.h>`(仅 `pic_init`)、`<driver/pit.h>`(仅 `pit_init`)、`<driver/ahci.h>`(仅 `ahci_init`)、`<device/timer.h>`(仅 `timer_init`)
3. 在 Phase 1 区域末尾、Phase 3 之前，添加 `extern uint64_t arch_boot_rsdp;` 和 `arch_boot_rsdp = bootinfo->RSDP;`
4. 将 Phase 3-6 的逐个调用替换为 `arch_register_subsys(); subsys_init_all();`
5. 在 Phase 8 的 `smp_boot_aps()` 之后，添加 `arch_register_subsys_percpu(); subsys_init_percpu();`，移除原来的 `lapic_timer_start(100);`

- [ ] **Step 1: 在 main.c 添加 subsys.h include、设置 RSDP、替换 Phase 3-6**

在文件头部 include 区域末尾添加：
```c
#include <kernel/subsys.h>
```

移除以下无需的 include：
```c
// 移除这三个：
// #include <device/pic.h>
// #include <driver/pit.h>
// #include <driver/ahci.h>
// #include <device/timer.h>
```

在 Phase 2 末尾（即 `color_printk(GREEN, BLACK, "frame buffer remap succeed\n");` 之后，Phase 3 注释之前），添加 RSDP 传递：
```c
    // ═══ RSDP: 传递给 arch 子系统（arch_register_subsys 会用） ═══
    extern uint64_t arch_boot_rsdp;
    arch_boot_rsdp = bootinfo->RSDP;
```

将 Phase 3-6 的 8 行直接调用 `apic_init(bootinfo->RSDP);` `pic_init();` `timer_init();` `pit_init();` `lapic_timer_init();` `keyboard_init();` `init_serial_irq();` `ahci_init();` 替换为：
```c
    // ═══ 3-6. Subsystem framework ═════════════════════════
    arch_register_subsys();
    subsys_init_all();
```

同时更新注释：将原来的多行 phase 注释替换为：
```c
    // ═══ 3-6. Subsystem framework ═════════════════════════
    // arch_register_subsys() + subsys_init_all() 按序执行：
    //   Phase 3: 中断控制器 (apic, pic)
    //   Phase 4: 定时器 (timer, pit, lapic-timer)
    //   Phase 5: 设备 IRQ (keyboard, serial)
    //   Phase 6: 存储 (ahci)
```

移除原有的 Phase 3/4/5/6 单独注释块。

- [ ] **Step 2: 修改 Phase 8，增加 per-CPU 子系统注册 + 执行**

找到 `lapic_timer_start(100);` 那一行（在 `smp_boot_aps();` 之后），替换为：
```c
    // per-CPU 子系统二次 init（如 LAPIC timer per-CPU tick）
    arch_register_subsys_percpu();
    subsys_init_percpu();
```

以及：在文件尾部添加 `extern void arch_register_subsys_percpu(void);` 声明，或确认 subsys.h 中不包含该声明（arch 特有函数不需要在框架里声明）。

实际上，`arch_register_subsys_percpu` 定义在 `kernel/arch/x86_64/subsys_percpu.c`，需要在 main.c 中声明为 extern。可以在 main.c 的 extern 区域（靠近文件顶部）添加：
```c
extern void arch_register_subsys(void);
extern void arch_register_subsys_percpu(void);
```

- [ ] **Step 3: 验证 main.c 中 Phase 8 的 `smp_boot_aps` 注释区域**

确保修改后的完整 Phase 8 区域看起来像：
```c
    // ═══ 8. Per-CPU + SMP ═══════════════════════════════════
    {
        uint32_t cpu_idx = 0;
        for (uint32_t i = 0; i < apic_info.lapic_count; i++) {
            // ... (percpu_init loop 保持不变) ...
        }
    }

    smp_boot_aps();

    // per-CPU 子系统二次 init
    arch_register_subsys_percpu();
    subsys_init_percpu();
```

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel/main.c
git commit -m "refactor(main): replace hardcoded init calls with subsys framework"
```

---

### Task 6: 修改 `kernel/Makefile`

**Files:**
- Modify: `kernel/Makefile`

**改动：** 在 `KERNEL_C_SOURCES` 的通配符列表中新增 `$(wildcard subsys/*.c)`

- [ ] **Step 1: 添加 subsys 目录到源文件发现**

在 `KERNEL_C_SOURCES` 的 `$(wildcard *.c)` 之前，插入新行：
```makefile
    $(wildcard subsys/*.c) \
```

即修改后的列表：
```makefile
KERNEL_C_SOURCES := \
    $(wildcard kernel/*.c) \
    $(wildcard memory/*.c) \
    $(wildcard fs/*.c) \
    $(wildcard sched/*.c) \
    $(wildcard driver/*.c) \
    $(wildcard tty/*.c) \
    $(wildcard apic/*.c) \
    $(wildcard intr/*.c) \
    $(wildcard block/*.c) \
    $(wildcard timer/*.c) \
    $(wildcard percpu/*.c) \
    $(wildcard pic/*.c) \
    $(wildcard test/*.c) \
    $(wildcard subsys/*.c) \
    $(wildcard *.c)               # completion.c
```

- [ ] **Step 2: Commit**

```bash
git add kernel/Makefile
git commit -m "build(kernel): add subsys/ directory to source discovery"
```

---

### Task 7: 构建验证

**Files:** (无代码修改)
**验证方法：** 确保 `make clean && make` 成功，`kernel_main` 中的 Phase 3-6 日志通过 `subsys:` 前缀输出。

- [ ] **Step 1: clean & rebuild**

```bash
cd /home/aagu/OS01
make clean
make 2>&1 | tail -30
```

Expected: 编译成功，无链接错误，生成 `kernel.bin` 和 `disk.img`。

- [ ] **Step 2: QEMU 快速启动验证**

```bash
make run &
sleep 5
pkill -f qemu
```

或使用：
```bash
timeout 10 make run 2>&1 | head -30 || true
```

Expected output 应包含：
```
subsys: init apic ... ok
subsys: init pic ... ok
subsys: init timer ... ok
subsys: init pit ... ok
subsys: init lapic-timer ... ok
subsys: init keyboard ... ok
subsys: init serial ... ok
subsys: init ahci ... ok
subsys: percpu lapic-timer-start cpu=0 ... ok
subsys: percpu lapic-timer-start cpu=1 ... ok
```

- [ ] **Step 3: Commit 所有改动**

```bash
git add -A
git commit -m "feat(subsys): integrate subsystem registration framework

- New kernel/include/kernel/subsys.h — framework API header
- New kernel/subsys/subsys.c — framework implementation
- New kernel/arch/x86_64/subsys.c — x86_64 subsystem registration
- New kernel/arch/x86_64/subsys_percpu.c — per-CPU registration
- Updated kernel/kernel/main.c — use framework for Phase 3-6, 8
- Updated kernel/Makefile — add subsys/ source discovery"
```
