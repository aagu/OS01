# Timer 重构：clocksource + clockevent 双层抽象 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 OS01 的 OS tick 源从 PIT 切到 LAPIC（校准改 TSC/联合校准），并引入 clocksource+clockevent 双层抽象，使 QEMU TCG 与真实硬件下 tick 都稳定 100Hz、精粒度时间（clock_gettime/nanosleep/poll/select）迁到纳秒。

**Architecture:** 三层：clocksource 层（`arch_cycle_counter` × mult/shift → 单调纳秒）、clockevent 层（`tick_handler()` 统一 jiffies++/poll扫描/need_resched）、arch hook（`arch_cycle_freq()` + `arch_tick_start()`，x86 用 TSC+LAPIC、aarch64 用 cntvct_el0+CNTP 预留）。PIT 先跑（boot 期 phase 4→6 驱动 jiffies），LAPIC 就绪后先掩 PIT 再接管；失败回退 PIT。

**Tech Stack:** C（freestanding），clang -target x86_64-unknown-none，ld.lld，QEMU (q35/TCG) + KVM，OS01 内核子系统框架（subsys.c），内核 selftest 框架，用户态 systest。

**Spec:** `docs/superpowers/specs/2026-08-17-timer-clocksource-clockevent-design.md`（v6）— 计划从 spec 论证，实现者须同时读这两份。

## Global Constraints

- **语言**：与用户中文交流；架构级改动先讨论再动（本次已完成讨论并获批 spec）。
- **提交前缀**：语义化前缀（`feat(time):` / `feat(tick):` / `test(time):`），小步提交。
- **TDD 纪律**：每步 RED→GREEN→全回归再推进；debug 探针只读、提交前移除。
- **验证门**：spec §9 的每条门必须附证据（trace 摘录 + 计时数字），「QEMU-verified evidence before progress claims」。
- **PIT 除数 11931 永不改**；`jiffies` 继续隐含 10ms/tick 语义（粗超时保留）。
- **index 坑**：`register_irq` 第一参数是 **gsi**，`unregister_irq` 第一参数是 **vector(0x20+gsi)**；`irq_mask/irq_unmask` 内部必须 gsi→vector 转换。
- **超时**：RTC PIE 校准硬超时 ~500ms，握手采样双侧有界，绝不无限自旋挂 boot。
- **构建**：`make` / `make disk.img`；内核 selftest：`make KERNEL_SELFTEST=1 disk.img`；用户态：`make test-syscall`（OS01_SYSTEST=1）；运行：`make run`（SMP 默认 2）。
- **`clocksource_read_ns()` 只在 GS base 装之后调用**（boot 期校准用原始 `arch_cycle_counter()`）。

---

## File Structure

**新增（arch 无关核心 + 测试）：**

| 文件 | 职责 |
|---|---|
| `kernel/include/kernel/clocksource.h` | clocksource 接口（`init`/`read_ns`/`cycles`/`freq_hz` + extern mult/shift/active） |
| `kernel/include/kernel/clockevent.h` | clockevent 接口（`tick_init`/`tick_start`/`tick_handler`） |
| `kernel/time/clocksource.c` | mult/shift 计算、`clocksource_init()`、`clocksource_cycles()`、`clocksource_freq_hz()` |
| `kernel/time/tick.c` | `tick_init()`/`tick_start()`/`tick_handler()`（统一 tick 语义） |
| `kernel/test/test_timer.c` | 内核 selftest：jiffies ~100Hz |

**修改：**

| 文件 | 改动 |
|---|---|
| `kernel/Makefile` | KERNEL_C_SOURCES 加 `$(wildcard time/*.c)` |
| `kernel/include/kernel/arch/cpu.h` | 加 `arch_cycle_freq()`（x86 extern / aarch64 inline） |
| `kernel/arch/x86_64/time.c`（新增，x86 平台源） | `arch_cycle_freq()` x86 实现 + `arch_tick_start()` |
| `kernel/driver/rtc.c` + `kernel/include/driver/rtc.h` | `rtc_pie_calibrate()` 联合校准 |
| `kernel/intr/apic/lapic_timer.c` | 校准改 §5.1、handler BSP/AP 分支、`lapic_timer_start` 返回 bool、删 divisor-16 retry |
| `kernel/driver/pit.c` | `pit_handler` 改调 `tick_handler()`，`serial_poll` 留本层 |
| `kernel/intr/irq.c` + `kernel/include/kernel/interrupt.h` | `irq_mask()`/`irq_unmask()` |
| `kernel/arch/x86_64/smp.c` | 握手采样（`tsc_offset`）、`lapic_timer_start` 适配 bool |
| `kernel/include/kernel/percpu.h` | `percpu_t` 增 `tsc_offset`/`tsc_sync_go`/`tsc_sampled` |
| `kernel/arch/x86_64/subsys.c` | 初始化顺序重排（加 `clocksource`） |
| `kernel/arch/x86_64/subsys_percpu.c` | `lapic_timer_start` 适配 bool |
| `kernel/kernel/main.c` | percpu 循环后插入 `tick_start()` |
| `kernel/arch/x86_64/trap.c` | `clock_gettime`/`nanosleep` 迁纳秒 |
| `kernel/include/kernel/task.h` | `wakeup_jiffies` → `wakeup_ns` |
| `kernel/fs/poll.c` + `kernel/include/kernel/poll.h` | deadline 迁纳秒 |
| `user/systest.c` | 扩展 time 断言 |

---

## Task 1: clocksource + clockevent 抽象层（接口，无行为变化）

**Files:**
- Create: `kernel/include/kernel/clocksource.h`, `kernel/include/kernel/clockevent.h`, `kernel/time/clocksource.c`, `kernel/time/tick.c`, `kernel/arch/x86_64/time.c`
- Modify: `kernel/Makefile`（KERNEL_C_SOURCES 段）, `kernel/include/kernel/arch/cpu.h`
- Modify: `kernel/include/kernel/interrupt.h` + `kernel/intr/irq.c`（`irq_mask`/`irq_unmask`，因 Task 1 的 `tick.c` 引用它们）

**Interfaces:**
- Consumes: `arch_cycle_counter()`（已存在，`kernel/include/kernel/arch/cpu.h:14`）、`jiffies`（`device/timer.h`）、`this_cpu()`/`percpu_t`（`kernel/percpu.h`）
- Produces（后续任务依赖的精确签名）:
  - `void clocksource_init(void);`
  - `uint64_t clocksource_freq_hz(void);` — 返回已校准 freq（0 = 未校准）
  - `uint64_t clocksource_cycles(void);` — 原始 cycle
  - `static inline uint64_t clocksource_read_ns(void);`
  - `void tick_init(void); void tick_start(void); void tick_handler(void);`
  - `uint64_t arch_cycle_freq(void);`（x86 由 time.c 提供，aarch64 inline）
  - `bool arch_tick_start(void);`

> 注：spec 把文件放 `kernel/time/`，但 Makefile 只有 `timer/*.c` wildcard 没有 `time/*.c`，本任务加一行 wildcard。头文件放 `kernel/include/kernel/`（include path 已有 `-I$(KERNEL_HEADERS)`）。

- [ ] **Step 1: 加 Makefile wildcard**

在 `kernel/Makefile` 的 KERNEL_C_SOURCES 段（约 line 28-36，`$(wildcard timer/*.c)` 之后）加一行：

```makefile
    $(wildcard timer/*.c) \
    $(wildcard time/*.c) \
```

- [ ] **Step 2: 写 clocksource.h**

Create `kernel/include/kernel/clocksource.h`:

```c
#ifndef _KERNEL_CLOCKSOURCE_H
#define _KERNEL_CLOCKSOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <device/timer.h>      // jiffies
#include <kernel/percpu.h>     // this_cpu(), percpu_t->tsc_offset
#include <kernel/arch/cpu.h>   // arch_cycle_counter()

// mult/shift 由 clocksource_init() 计算并导出（static inline read_ns 引用）。
extern bool     clocksource_active;
extern uint32_t clocksource_mult;
extern uint32_t clocksource_shift;

// 依据 arch_cycle_freq() 计算 mult/shift；freq=0 时 active=false（退 jiffies）。
void     clocksource_init(void);

// 已校准的 cycle 频率（Hz），0 = 未校准。
uint64_t clocksource_freq_hz(void);

// 原始 cycle 计数（调试/校准用），不加 tsc_offset。
uint64_t clocksource_cycles(void);

// 单调纳秒。active 时 = (cycle+tsc_offset)*mult>>shift；否则退 jiffies*10ms。
// 仅在 GS base 装之后调用（boot 期校准用 arch_cycle_counter()）。
static inline uint64_t clocksource_read_ns(void)
{
    if (!clocksource_active)
        return jiffies * 10000000ULL;
    uint64_t c = arch_cycle_counter() + (uint64_t)this_cpu()->tsc_offset;
    return (uint64_t)(((__uint128_t)c * clocksource_mult) >> clocksource_shift);
}

#endif
```

- [ ] **Step 3: 写 clockevent.h**

Create `kernel/include/kernel/clockevent.h`:

```c
#ifndef _KERNEL_CLOCKEVENT_H
#define _KERNEL_CLOCKEVENT_H

#include <stdint.h>

// 静态注册阶段：不启动任何 tick 源（PIT 在 phase 4 由 pit_init 照常启动）。
void tick_init(void);

// 显式启动（percpu+GS 就绪后调用）：先掩 PIT，arch_tick_start() 成功则 LAPIC
// 接管，失败则回退 PIT。
void tick_start(void);

// 统一 tick 语义（arch IRQ handler 调用，无参）：jiffies++ → poll 纳秒扫描 →
// need_resched → watchdog → TIMER_SIRQ。EOI 由各 arch handler 负责。
void tick_handler(void);

#endif
```

- [ ] **Step 4: 写 clocksource.c**

Create `kernel/time/clocksource.c`:

```c
#include <kernel/clocksource.h>

bool     clocksource_active = false;
uint32_t clocksource_mult   = 0;
uint32_t clocksource_shift  = 0;
static uint64_t clocksource_freq = 0;

// 找最大 shift 使 mult = (1e9 << shift)/freq 落在 [1, 2^32)，尽量接近 2^31
// 以最大化精度。freq 在 1MHz~10GHz 范围时循环很快（s≈22..34）。
static void compute_mult_shift(uint64_t freq_hz, uint32_t *mult, uint32_t *shift)
{
    uint32_t s = 1;
    uint64_t m = 0;
    for (; s < 32; s++) {
        m = (1000000000ULL << s) / freq_hz;
        if (m >= (1ULL << 32))
            break;
    }
    s -= 1;                       // 退回最后一个不溢出的 shift
    m = (1000000000ULL << s) / freq_hz;
    if (m == 0) m = 1;            // 极低 freq 保护
    *shift = s;
    *mult  = (uint32_t)m;
}

void clocksource_init(void)
{
    clocksource_freq = arch_cycle_freq();
    if (clocksource_freq == 0) {
        clocksource_active = false;
        return;
    }
    compute_mult_shift(clocksource_freq, &clocksource_mult, &clocksource_shift);
    clocksource_active = true;
}

uint64_t clocksource_freq_hz(void) { return clocksource_freq; }
uint64_t clocksource_cycles(void)  { return arch_cycle_counter(); }
```

- [ ] **Step 5: 写 tick.c**

Create `kernel/time/tick.c`:

```c
#include <kernel/clockevent.h>
#include <kernel/clocksource.h>
#include <device/timer.h>
#include <kernel/task.h>          // current, TIMER_SIRQ? 见下
#include <kernel/softirq.h>       // set_softirq_status, TIMER_SIRQ
#include <kernel/percpu.h>        // this_cpu()
#include <kernel/arch/cpu.h>      // arch_tick_start()
#include <kernel/interrupt.h>     // irq_mask / irq_unmask

// 从 pit_handler 迁来的 poll 超时注册表（定义在 kernel/fs/poll.c）。
extern struct poll_timeout_node *poll_timeout_head;
extern spinlock_T poll_timeout_lock;

void tick_init(void)
{
    // 静态注册阶段无 tick 源切换；PIT 由 pit_init 照常启动。
}

void tick_handler(void)
{
    jiffies++;

    // poll 超时扫描（纳秒比较）—— 从 pit_handler 迁来。
    // ⚠️ 时序假设：boot 期 poll_timeout_head 恒 NULL（poll 只在用户态进程里调，
    // 用户态进程 task_init() 之后才有），此短路保证 GS base 装之前（phase 4 到
    // main.c:276）不调 clocksource_read_ns()（它读 this_cpu()->tsc_offset）。
    if (poll_timeout_head) {
        spin_lock(&poll_timeout_lock);
        uint64_t now_ns = clocksource_read_ns();
        for (struct poll_timeout_node *n = poll_timeout_head; n; n = n->next)
            if (now_ns >= n->deadline)
                wait_queue_wake_all(n->wq);
        spin_unlock(&poll_timeout_lock);
    }

    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}

void tick_start(void)
{
    // 先掩 PIT IRQ0，防止交接窗口 LAPIC+PIT 双计 jiffies。
    irq_mask(0);
    if (arch_tick_start()) {
        // LAPIC 接管成功，PIT 保持掩蔽。
    } else {
        // LAPIC 未校准/失败：回退 PIT。
        irq_unmask(0);
    }
}
```

> 注：`wait_queue_wake_all` 声明在 `kernel/include/kernel/wait.h`；若 tick.c 缺 include 报错，加 `#include <kernel/wait.h>`。`struct poll_timeout_node` 的正确 tag 名以 `kernel/include/kernel/poll.h:78` 为准（`poll_timeout_node_t`）。

- [ ] **Step 6: 写 x86 time.c 骨架（arch_cycle_freq + arch_tick_start）**

Create `kernel/arch/x86_64/time.c`（被 `$(wildcard $(ARCHDIR)/*.c)` 自动发现）:

```c
#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/x86_64/cpuid.h>   // cpuid()
#include <kernel/apic.h>                 // lapic_timer_start

// 三级回落：CPUID 15h → RTC PIE（Task 2 接入）→ 0。
// 本任务只做 CPUID 15h；RTC PIE 由 Task 2 补全。
uint64_t arch_cycle_freq(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);
    if (ecx != 0)
        return (uint64_t)ecx * ebx / eax;   // core crystal Hz * ratio
    return 0;                                // Task 2 接 RTC PIE 回落
}

// 启动 x86 tick 源：LAPIC 周期模式。返回是否启动成功。
bool arch_tick_start(void)
{
    // lapic_timer_start 在 Task 3 改签名返回 bool；本任务先按 void 处理。
    return true;  // Task 3 替换为 lapic_timer_start(100) 的 bool 结果
}
```

- [ ] **Step 7: 加 arch_cycle_freq 声明到 arch/cpu.h**

x86 分支（`#ifdef __x86_64__` 内，`arch_cycle_counter` 那行之后）加：

```c
// TSC 频率（Hz）。实现见 kernel/arch/x86_64/time.c（CPUID 15h → RTC PIE → 0）。
uint64_t arch_cycle_freq(void);
```

aarch64 分支（`#elif defined(__aarch64__)` 内，`arch_cycle_counter` 那行之后）加：

```c
static inline uint64_t arch_cycle_freq(void)
{
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}
```

- [ ] **Step 7b: 加 irq_mask/irq_unmask（tick.c 依赖）**

在 `kernel/include/kernel/interrupt.h`（`register_irq` 声明后）加：

```c
// 只掩蔽/解掩蔽，不触碰 irq_desc 的 handler（作 fallback 保留用）。
// 参数是 gsi；内部转 vector(0x20+gsi) 再调 controller->disable/enable。
void irq_mask(uint32_t gsi);
void irq_unmask(uint32_t gsi);
```

在 `kernel/intr/irq.c` 末尾加：

```c
void irq_mask(uint32_t gsi)
{
    irq_desc_t *p = &irq_table[gsi];
    if (p->controller)
        p->controller->disable(0x20 + gsi);
}

void irq_unmask(uint32_t gsi)
{
    irq_desc_t *p = &irq_table[gsi];
    if (p->controller)
        p->controller->enable(0x20 + gsi);
}
```

> **对称性已验证（Hermes 评审问题 2，已读源码确认，无需实现者再查）**：
> - `ioapic_disable`（ioapic.c:193-208）= 置 IOREDTBL mask 位（`low | IOAPIC_RED_MASK`），
>   **保留** vector/trigger/polarity/dest 字段，是 mask-only。
> - `ioapic_enable`（ioapic.c:108-191）= **重建** redirection entry（重新算 trigger/
>   polarity 并写 high+low），写的 low_flags **不含 mask 位** → 清除 mask、恢复路由。
> - 两者对称：disable 置 mask 保留路由，enable 清 mask 重建路由。PIT（IRQ0）flags=
>   IRQF_TRIGGER_EDGE，enable 重建时经 `irq_table[nr-32].flags` 正确还原 edge 触发。
> - PIC 路径同样 mask-only（8259A.c `pic_disable/enable` 用 OCW1 掩码位，天然对称）。
> 结论：`irq_mask` 用 `controller->disable` 安全，回退 PIT 时 `irq_unmask` 能正确恢复 IRQ0。

- [ ] **Step 8: 编译验证（无行为变化）**

Run: `make -C kernel clean && make disk.img`

Expected: 编译通过，`kernel.bin` 生成。此时 `clocksource_init()`/`tick_init()`/`tick_start()` 尚未被调用，`arch_cycle_freq()` 在 QEMU 下返回 0（TCG 无 CPUID 15h），系统行为不变。

- [ ] **Step 9: 运行基线回归**

Run: `make test-syscall`

Expected: systest 全绿（记录基线；若本任务前已绿则无变化）。

- [ ] **Step 10: Commit**

```bash
git add kernel/time kernel/include/kernel/clocksource.h kernel/include/kernel/clockevent.h kernel/arch/x86_64/time.c kernel/include/kernel/arch/cpu.h kernel/Makefile kernel/include/kernel/interrupt.h kernel/intr/irq.c
git commit -m "feat(time): clocksource + clockevent 双层抽象（TSC/cntvct_el0，接口层）"
```

---

## Task 2: TSC 频率校准（CPUID 15h + RTC PIE 联合校准，含超时）

**Files:**
- Modify: `kernel/driver/rtc.c`, `kernel/include/driver/rtc.h`, `kernel/arch/x86_64/time.c`
- Modify: `kernel/intr/apic/lapic_timer.c`（加 `lapic_timer_set_premeasured`）

**Interfaces:**
- Consumes: `arch_cycle_freq()`（Task 1）、`lapic_read/lapic_write`（`apic.h`）、`register_irq/unregister_irq`（`interrupt.h`）、`get_rtc_register/set_rtc_register`（`rtc.h`）、`cpuid()`（`cpuid.h`）
- Produces:
  - `int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out);`（rtc.h）
  - `void lapic_timer_set_premeasured(uint64_t hz);`（apic.h / lapic_timer.c）

> **关键 index 坑（照 spec §5.1 写，写错即静默失败/野写）**：`register_irq(8, ...)` 传 **gsi**；`unregister_irq(0x28)` 传 **vector**（`0x20+8`）。传反 → `register_irq(0x28)` 因 gsi=40≥MAX_GSI(24) 静默不注册；`unregister_irq(8)` → `irq_table[-24]` 野内存写。

- [ ] **Step 1: 写 lapic_timer_set_premeasured（在 lapic_timer.c）**

在 `lapic_timer.c` 的静态变量区（`lapic_timer_hz`/`lapic_timer_divisor` 附近，约 line 35-38）加：

```c
// RTC PIE 联合校准测得的 LAPIC 频率，供 lapic_timer_calibrate 优先复用。
static uint64_t lapic_premeasured_hz = 0;

void lapic_timer_set_premeasured(uint64_t hz)
{
    lapic_premeasured_hz = hz;
}
```

在 `kernel/include/kernel/apic.h`（`lapic_timer_calibrate` 声明附近，约 line 216）加：

```c
void lapic_timer_set_premeasured(uint64_t hz);
```

- [ ] **Step 2: 写 rtc_pie_calibrate（在 rtc.c）**

在 `rtc.h` 加声明：

```c
// RTC PIE 联合校准：一次 PIE 窗口（1024Hz × N=256 tick ≈250ms）同时测 TSC 和
// LAPIC 两个计数器频率。返回 0 成功 / -1 失败（超时或 IRQ8 不到）。
// tsc_hz_out / lapic_hz_out 仅在成功时被写。
int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out);
```

在 `rtc.c` 顶部加 include 与实现：

```c
#include <driver/rtc.h>
#include <kernel/arch/io.h>
#include <kernel/interrupt.h>      // register_irq / unregister_irq / IRQF_TRIGGER_LEVEL
#include <kernel/apic.h>           // lapic_read / lapic_write / LAPIC_* / LVT_MASK
#include <kernel/arch/cpu.h>       // arch_cycle_counter / rdtsc

#define RTC_PIE_TICKS    256      // 采样 tick 数（~250ms）
#define RTC_PIE_IRQ_GSI  8        // register_irq 用 gsi
#define RTC_PIE_IRQ_VEC  0x28     // unregister_irq 用 vector（0x20 + 8）

static volatile uint32_t rtc_pie_count;
static volatile uint64_t rtc_pie_tsc0;
static volatile uint64_t rtc_pie_tsc1;

static void rtc_pie_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr; (void)parameter; (void)regs;
    // 读 RTC reg 0x0C 清 PIE 中断标志（否则真实硬件第一个中断后 PIE 停摆）。
    arch_outb(CMOS_ADDR, 0x80 | 0x0C);
    arch_inb(CMOS_DATA);

    if (rtc_pie_count == 0)
        rtc_pie_tsc0 = arch_cycle_counter();
    rtc_pie_count++;
    if (rtc_pie_count >= RTC_PIE_TICKS)
        rtc_pie_tsc1 = arch_cycle_counter();
}

int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out)
{
    // 1. 掩 LAPIC timer，避免 countdown 到零触发未注册的 vector 0x38。
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);
    //    divisor=0（÷2，SDM 000b）。必须显式写。
    lapic_write(LAPIC_TIMER_DIV, 0);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // 2. 临时注册 IRQ8（gsi=8），level 触发。检查返回值。
    if (!register_irq(RTC_PIE_IRQ_GSI, NULL, rtc_pie_handler, 0,
                      IRQF_TRIGGER_LEVEL, "rtc-pie")) {
        return -1;
    }

    // 3. 使能 PIE：reg 0x0B bit6 = PIE；reg 0x0A 低 4 位 = 1024Hz (0b0110=6)。
    uint8_t b = get_rtc_register(0x0B);
    set_rtc_register(0x0B, b | 0x40);
    uint8_t a = get_rtc_register(0x0A);
    set_rtc_register(0x0A, (a & 0xF0) | 0x06);

    // 4. 主循环：双条件（tick 数未达 && TSC 流逝 < 宽松上限）。
    //    RTC PIE 校准时 freq 未知（cpuid 15h=0 才会进这里），无法把「500ms」精确
    //    换算成 cycle 数。用 2^32 cycle 作宽松兜底：@8.6GHz ≈ 500ms、@3GHz ≈
    //    1.43s、@1GHz ≈ 4.3s。正常路径 250ms 内 tick 达标，不依赖此值精度；
    //    它只防 IRQ8 完全失效时的无限自旋（挂 boot）。
    rtc_pie_count = 0;
    uint64_t tsc_start = arch_cycle_counter();
    while (rtc_pie_count < RTC_PIE_TICKS) {
        if (arch_cycle_counter() - tsc_start > 0x100000000ULL) {  // 2^32 cycle 兜底
            break;
        }
        arch_cpu_pause();
    }

    // 5. 禁 PIE，注销 IRQ8。
    b = get_rtc_register(0x0B);
    set_rtc_register(0x0B, b & ~0x40);
    unregister_irq(RTC_PIE_IRQ_VEC);     // 传 vector 0x28

    // 6. 读 LAPIC 剩余计数。
    uint32_t cur = lapic_read(LAPIC_TIMER_CUR);
    uint64_t elapsed_lapic = 0xFFFFFFFFULL - cur;

    // 7. 检查是否采到足够 tick。
    if (rtc_pie_count < RTC_PIE_TICKS)
        return -1;                        // 超时/中断不到

    uint64_t tsc_elapsed = rtc_pie_tsc1 - rtc_pie_tsc0;
    // off-by-one 修正：tsc0 在沿#1、tsc1 在沿#N，实际跨 N-1 个周期。
    uint64_t n = RTC_PIE_TICKS - 1;
    *tsc_hz_out   = tsc_elapsed * 1024 / n;
    // ⚠️ lapic_hz_out = 递减率（divisor ÷2 已折算），**不 ×2**：elapsed_lapic 是
    // ÷2 后递减量，除以窗口秒数 n/1024 即递减率；lapic_timer_start 的
    // init_count=hz/freq 直接基于递减率。×2 会得真实频率 → init_count 被 divisor
    // 再 ÷2 → 50Hz。与 Task 3 Step 1 的 `elapsed * 100`（不 ×2）语义一致。
    *lapic_hz_out = elapsed_lapic * 1024 / n;
    return 0;
}
```

- [ ] **Step 3: 接入 arch_cycle_freq（time.c）**

Modify `kernel/arch/x86_64/time.c` 的 `arch_cycle_freq()`，加 RTC PIE 回落：

```c
#include <driver/rtc.h>   // rtc_pie_calibrate

uint64_t arch_cycle_freq(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);
    if (ecx != 0)
        return (uint64_t)ecx * ebx / eax;

    // RTC PIE 联合校准：同时测 TSC + LAPIC，LAPIC 结果暂存复用。
    uint64_t tsc_hz = 0, lapic_hz = 0;
    if (rtc_pie_calibrate(&tsc_hz, &lapic_hz) == 0) {
        lapic_timer_set_premeasured(lapic_hz);
        return tsc_hz;
    }
    return 0;
}
```

- [ ] **Step 4: 写/跑内核 selftest 验证 arch_cycle_freq**

在 `kernel/test/test_timer.c`（本任务先放一个最小 selftest，Task 5 扩展）:

```c
#if defined(OS01_SELFTEST)
#include <kernel/clocksource.h>

// 验证 arch_cycle_freq 在 QEMU（RTC PIE 回落）下返回非零（~2.99GHz）。
int test_timer_tsc_freq(void)
{
    uint64_t f = clocksource_freq_hz();
    if (f == 0) {
        serial_printk("[selftest] timer_tsc_freq: freq=0 (calibration failed)\n");
        return -1;
    }
    serial_printk("[selftest] timer_tsc_freq: %lu Hz\n", (unsigned long)f);
    return 0;
}
#endif
```

在 `kernel/test/selftest.c` 里 forward-declare + register（`selftest_run_all` 内）:

```c
#ifdef OS01_SELFTEST
int test_timer_tsc_freq(void);
int test_timer_jiffies_hz(void);
#endif
// ... 在 selftest_register 调用区加：
#ifdef OS01_SELFTEST
    selftest_register("timer_tsc_freq",    test_timer_tsc_freq);
#endif
```

> 注：`test_timer_jiffies_hz` 在 Task 5 定义，本任务先 forward-declare 不注册，或一并写 stub 返回 0 避免链接错误（选择：本任务只在 selftest.c 注册 `timer_tsc_freq`，`test_timer_jiffies_hz` 的声明留到 Task 5）。

- [ ] **Step 5: 接线 clocksource_init（subsys.c）**

在 `kernel/arch/x86_64/subsys.c` 加 wrapper 并注册（phase 4，`timer` 之后、`pit` 之前）:

```c
#include <kernel/clocksource.h>

static int _clocksource_init_wrapper(void)
{
    clocksource_init();
    return 0;
}

// 在 arch_register_subsys() 的 phase 4 注册区：
register_subsys("clocksource", _clocksource_init_wrapper, SUBSYS_PHASE_4, 0);
```

> 此步同时把 Task 1 的 `clocksource_init` 真正接入 boot 路径，使 Task 2 的 selftest 能测到。

- [ ] **Step 6: 编译 + selftest 验证**

Run: `make -C kernel clean && make KERNEL_SELFTEST=1 disk.img && make run`

Expected: serial 输出 `[selftest] timer_tsc_freq: 2993154817 Hz`（或相近，~2.99GHz），`PASS`。若 QEMU 无 CPUID 15h，走 RTC PIE 回落，freq 非 0。

- [ ] **Step 7: 验证门 ④.13（RTC PIE 超时兜底）**

人为把 `rtc_pie_calibrate` 的 `register_irq` 返回值改为 0（临时探针，测完撤销），run 一次确认 ~500ms 后 `freq=0` 且 boot 不挂起。

- [ ] **Step 8: Commit**

```bash
git add kernel/driver/rtc.c kernel/include/driver/rtc.h kernel/arch/x86_64/time.c kernel/intr/apic/lapic_timer.c kernel/include/kernel/apic.h kernel/test/test_timer.c kernel/test/selftest.c kernel/arch/x86_64/subsys.c
git commit -m "feat(time): TSC 频率校准（CPUID15h + RTC PIE 联合校准，含超时）"
```

---

## Task 3: BSP 切 LAPIC 周期 tick（先掩 PIT 再接管）+ 握手采样

**Files:**
- Modify: `kernel/intr/apic/lapic_timer.c`（校准改 §5.1、handler 分支、`lapic_timer_start` 返回 bool、删 divisor-16 retry）
- Modify: `kernel/driver/pit.c`（`pit_handler` 改调 `tick_handler()`）
- Modify: `kernel/intr/irq.c` + `kernel/include/kernel/interrupt.h`（`irq_mask/irq_unmask`）
- Modify: `kernel/arch/x86_64/smp.c`（握手采样）、`kernel/include/kernel/percpu.h`（字段）
- Modify: `kernel/arch/x86_64/subsys_percpu.c`（bool 适配）
- Modify: `kernel/kernel/main.c`（插 `tick_start()`）

**Interfaces:**
- Consumes: `tick_handler()`（Task 1）、`clocksource_freq_hz()`/`clocksource_cycles()`（Task 1/2）、`lapic_timer_set_premeasured`（Task 2）
- Produces:
  - `bool lapic_timer_start(uint32_t freq_hz);`（改签名，返回是否启动成功）
  - `void irq_mask(uint32_t gsi); void irq_unmask(uint32_t gsi);`

- [ ] **Step 1: 重写 lapic_timer_calibrate（TSC 窗口 / RTC PIE 复用，删 divisor-16 retry）**

替换 `kernel/intr/apic/lapic_timer.c` 的 `lapic_timer_calibrate()`（约 line 45-88）:

```c
void lapic_timer_calibrate(void)
{
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);

    uint64_t tsc_hz = clocksource_freq_hz();

    // 1. 优先复用 RTC PIE 联合结果（250ms 更稳）。
    if (lapic_premeasured_hz != 0) {
        lapic_timer_hz = lapic_premeasured_hz;
        lapic_timer_divisor = 0;   // 联合校准用 ÷2（DIV=0）
        debug_sched("LAPIC timer: %lu Hz (from RTC PIE)\n",
                    (unsigned long)lapic_timer_hz);
        return;
    }

    // 2. 回落 TSC 窗口（~10ms）。
    if (tsc_hz == 0) {
        lapic_timer_hz = 0;   // 无法校准 → 退 PIT
        debug_sched("LAPIC timer: no TSC freq, fallback PIT\n");
        return;
    }

    lapic_timer_divisor = 0;   // ÷2（SDM 000b；⚠️ 原代码 lapic_timer.c:53 注释
                               // "divide by 1" 是错的，实现时一并改正为 ÷2）
    lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    uint64_t tsc0 = rdtscp_serialized();
    while (rdtscp_serialized() - tsc0 < tsc_hz / 100)   // 10ms TSC 窗口
        arch_cpu_pause();

    uint32_t cur = lapic_read(LAPIC_TIMER_CUR);
    uint64_t elapsed = 0xFFFFFFFFULL - cur;

    // lapic_timer_hz 语义 = 递减率（÷2 已折算），不是真实频率。
    // elapsed 是 ÷2 后 10ms 递减量，除以 10ms 得递减率 → elapsed * 100（不 ×2）。
    // 若误写 *200 得真实频率，lapic_timer_start 的 init_count=hz/freq 会被 divisor
    // 再 ÷2 → 50Hz（Hermes 评审「漏 ×2」的误报方向，正确是保持 ×100）。
    lapic_timer_hz = elapsed * 100;

    debug_sched("LAPIC timer: %u ticks/10ms → %lu Hz (div=%u, TSC)\n",
                (unsigned)elapsed, (unsigned long)lapic_timer_hz,
                (unsigned)lapic_timer_divisor);
}
```

> 注：原代码 divisor-16 retry（lapic_timer.c:68-80）整段删除——TSC 窗口法下 10ms 内 32 位溢出需 LAPIC>429THz，不再可达（spec §10）。若采用 DIV=0(÷2) 而非原 ÷1，`lapic_timer_start` 的 `init_count = lapic_timer_hz/freq_hz` 已正确表达周期，无需改。

- [ ] **Step 2: 改 lapic_timer_start 返回 bool**

替换 `lapic_timer_start()`（约 line 92-114）:

```c
bool lapic_timer_start(uint32_t freq_hz)
{
    if (!lapic_timer_hz) {
        debug_sched("LAPIC timer: not calibrated, skipping start\n");
        return false;
    }

    uint32_t init_count = (uint32_t)(lapic_timer_hz / freq_hz);
    if (init_count == 0)
        init_count = 1;

    // 写 divisor 到当前 CPU 的 LAPIC 硬件寄存器（DIV 是 per-LAPIC 的，不是
    // 全局变量）。值仍是校准产物，但必须每 CPU 各写一次：AP 的 LAPIC 复位后
    // count_shift=0(÷1)，若不写则 AP 复用 ÷2 折算的 lapic_timer_hz 却用 ÷1
    // 硬件 → AP 频率 ×2 = 200Hz（spec §7.2，Ruling 4）。先定 divisor 再装 count。
    lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor);

    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1 << 17));
    lapic_write(LAPIC_TIMER_INIT, init_count);

    debug_sched("LAPIC timer: started at %u Hz (init_count=%u) on CPU %u\n",
                (unsigned)freq_hz, init_count, (unsigned)cpu_id());
    return true;
}
```

更新 `kernel/include/kernel/apic.h` 的声明（约 line 218）:

```c
bool lapic_timer_start(uint32_t freq_hz);
```

- [ ] **Step 3: 改 lapic_timer_handler（BSP/AP 分支）**

替换 `lapic_timer_handler()`（约 line 120-131）:

```c
void lapic_timer_handler(pt_regs_t *regs __attribute__((unused)),
                         uint64_t error_code __attribute__((unused)))
{
    if (cpu_id() == 0) {
        // BSP 驱动 jiffies + poll 扫描（tick_handler 内含 watchdog++/softirq）。
        tick_handler();
    } else {
        // AP 只做 need_resched + watchdog（BSP 的 tick_handler 已覆盖全局 jiffies）。
        this_cpu()->need_resched = 1;
        this_cpu()->watchdog_counter++;
        set_softirq_status(TIMER_SIRQ);
    }
    lapic_eoi();
}
```

在 `lapic_timer.c` 顶部加 include：`#include <kernel/clockevent.h>`（tick_handler）、`#include <kernel/clocksource.h>`（clocksource_freq_hz，供 calibrate）。

- [ ] **Step 4: 改 pit_handler 调 tick_handler**

替换 `kernel/driver/pit.c` 的 `pit_handler()` 主体（约 line 17-53）:

```c
void pit_handler(uint64_t nr __attribute__((unused)),
                 uint64_t parameter __attribute__((unused)),
                 pt_regs_t *regs __attribute__((unused)))
{
    tick_handler();      // 统一 tick 语义（jiffies++/poll/need_resched/watchdog/softirq）
    serial_poll();       // x86 串口轮询 fallback，留在本层
}
```

在 `pit.c` 顶部加 `#include <kernel/clockevent.h>`。删除原 `pit_handler` 里的 jiffies++、poll 扫描、need_resched、watchdog、softirq 代码（全部并入 `tick_handler`）。

> **GS 时序假设（Hermes 评审问题 3，需在 tick_handler 注释里写明）**：`tick_handler()`
> 的 poll 扫描调 `clocksource_read_ns()`（读 `this_cpu()->tsc_offset`），而
> `tick_handler` 在 phase 4（PIT 启动）到 `main.c:276`（GS 装）之间每 tick 都跑。
> 安全前提是 `poll_timeout_head` 在 boot 期恒 NULL（poll 只在用户态进程里调，用户态
> 进程 `task_init()` 之后才有），`if (poll_timeout_head)` 短路使 `clocksource_read_ns()`
> 不被调用。**实现 tick.c 时必须在 poll 扫描里保留 `if (poll_timeout_head)` 短路，
> 并加注释「boot 期 poll 空 → 不读 GS」**。若未来允许 boot 期注册 poll，须先解决
> GS 早期安装。

- [ ] **Step 5: （irq_mask/irq_unmask 已在 Task 1 Step 7b 定义，无需重复）**

- [ ] **Step 6: 改 arch_tick_start（time.c）返回 lapic_timer_start 结果**

替换 `kernel/arch/x86_64/time.c` 的 `arch_tick_start()`:

```c
bool arch_tick_start(void)
{
    return lapic_timer_start(100);
}
```

- [ ] **Step 7: 加 percpu 字段**

在 `kernel/include/kernel/percpu.h` 的 `percpu_t`（`tsc_boot` 那行后）加：

```c
    uint64_t tsc_boot;          // TSC value after AP startup (for warp check)
    uint64_t tsc_sync_go;       // BSP→AP：发起 TSC 握手采样
    uint64_t tsc_sampled;       // AP→BSP：采样完成
    int64_t  tsc_offset;        // bsp_tsc - ap_tsc（BSP=0），clocksource_read_ns 用
```

- [ ] **Step 8: 改 smp.c 握手采样（AP 超时后绝不采样）**

替换 `kernel/arch/x86_64/smp.c` 的 `ap_entry()` 里 `lapic_timer_start(100); cpu->online=1; ...; cpu->tsc_boot = rdtsc();` 段（约 line 120-127）:

```c
    // Start per-CPU scheduling tick
    lapic_timer_start(100);

    __sync_synchronize();
    cpu->online = 1;                     // 1. 先活（BSP 等待循环退出条件）
    uint32_t w = 0;
    while (w++ < SYNC_SPIN_LIMIT && !cpu->tsc_sync_go)  // 2. 有界等 BSP
        arch_cpu_pause();
    if (cpu->tsc_sync_go) {              // 3. 只有观测到 go 才采样；超时跳过（sampled 留 0）
        cpu->tsc_boot = rdtsc();
        __sync_synchronize();
        cpu->tsc_sampled = 1;            // 4. 回写完成
    }
    arch_local_irq_enable();
```

替换 `smp_boot_aps()` 里原 warp 检测块（约 line 245-253）:

```c
            // TSC 握手采样：BSP 发起、等 AP 回写，算跨核偏移。
            percpu_data[i].tsc_sync_go = 1;
            uint64_t bsp_tsc = rdtsc();
            for (uint32_t w = 0; w < SYNC_SPIN_LIMIT && !percpu_data[i].tsc_sampled; w++)
                arch_cpu_pause();
            if (!percpu_data[i].tsc_sampled) {
                debug_sched("SMP: TSC sync AP%u timeout, offset unset\n", i);
                percpu_data[i].tsc_offset = 0;
            } else {
                uint64_t ap_tsc = percpu_data[i].tsc_boot;
                percpu_data[i].tsc_offset = (int64_t)(bsp_tsc - ap_tsc);
                debug_sched("SMP: TSC sync AP%u: offset=%+ld\n", i,
                            (long)percpu_data[i].tsc_offset);
            }
```

在 `smp.c` 顶部加 `#define SYNC_SPIN_LIMIT 1000000u`（两侧同宏）。

- [ ] **Step 9: 改 subsys_percpu.c 适配 bool**

`kernel/arch/x86_64/subsys_percpu.c` 的 `_lapic_timer_start_percpu`:

```c
static int _lapic_timer_start_percpu(int cpu_id)
{
    (void)cpu_id;
    lapic_timer_start(100);   // 返回值忽略：AP 失败不致命
    return 0;
}
```

（`lapic_timer_start` 现返回 bool，丢弃返回值即可，无其他改动。）

- [ ] **Step 10: main.c 插 tick_start**

在 `kernel/kernel/main.c` 的 `smp_boot_aps();`（约 line 291）之前插入：

```c
    // 显式启动 tick 源：GS base 已装（main.c:276 percpu_install_gs(0)），
    // this_cpu() 可用。tick_start 先掩 PIT 再启 LAPIC，失败回退 PIT。
    tick_start();
```

在 `main.c` 顶部加 `#include <kernel/clockevent.h>`。

- [ ] **Step 11: 编译 + 运行验证（门①②）**

Run: `make -C kernel clean && make disk.img`

Run QEMU trace 验证 LAPIC 100Hz：

```bash
printf 'apic_local_deliver\n' > /tmp/ev.txt
qemu-system-x86_64 -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
  -netdev user,id=net0 -device e1000e,netdev=net0 \
  -drive file=disk.img,format=raw,if=none,id=disk \
  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -m 512 -display none -serial file:/tmp/s.log \
  -trace events=/tmp/ev.txt,file=/tmp/t.log -no-reboot
# 10s 间隔两次 grep LVT idx 0 计数 → 期望 ~100Hz
```

Expected（spec §9 门①）: LAPIC LVT TIMER ~100Hz（非 200/629）；`busybox sleep 5` ~5.0s（门③，`make run` 后 shell 里测）。

- [ ] **Step 12: 验证门 ④.11（LAPIC 失败回退 PIT）**

人为 `lapic_timer_hz = 0`（临时探针），run 确认 tick 走 PIT、时间走 TSC，系统正常。

- [ ] **Step 13: Commit**

```bash
git add kernel/intr/apic/lapic_timer.c kernel/include/kernel/apic.h kernel/driver/pit.c kernel/intr/irq.c kernel/include/kernel/interrupt.h kernel/arch/x86_64/smp.c kernel/include/kernel/percpu.h kernel/arch/x86_64/subsys_percpu.c kernel/kernel/main.c kernel/arch/x86_64/time.c
git commit -m "feat(tick): BSP 切 LAPIC 周期 tick，先掩 PIT 再接管 + 握手采样"
```

---

## Task 4: CLOCK_MONOTONIC/REALTIME + nanosleep + poll 迁纳秒

> **承重墙前置（Hermes 建议）**：必须**先**在 Task 3 Step 11 用 QEMU trace 确认
> LAPIC 100Hz 接管成功（门①），再动本任务。这是整个重构的承重墙——先有一版
> 「tick 正确 + 时间仍走 jiffies」的可运行系统，再叠纳秒迁移，失败定位面小。
> KVM（非 TCG）下握手采样的 store 延迟可能 >100ns，若 Task 3 测 `tsc_offset`
> 异常大，实现注释提示加 `lfence`（spec §8.3 已标可选）。

**Files:**
- Modify: `kernel/include/kernel/task.h`（`wakeup_jiffies`→`wakeup_ns`）
- Modify: `kernel/arch/x86_64/trap.c`（clock_gettime / nanosleep / nanosleep_should_unblock）
- Modify: `kernel/fs/poll.c` + `kernel/include/kernel/poll.h`（deadline 纳秒）
- Modify: `kernel/time/tick.c`（已用 `clocksource_read_ns()`，本任务确认 poll 扫描用纳秒）

**Interfaces:**
- Consumes: `clocksource_read_ns()`（Task 1）
- Produces: 无新接口（字段语义变化：`wakeup_ns`、`deadline` 纳秒）

- [ ] **Step 1: task.h 字段改名**

`kernel/include/kernel/task.h`（约 line 147-149）:

```c
    // nanosleep_should_unblock callback checks clocksource_read_ns() >= wakeup_ns.
    uint64_t wakeup_ns;
```

- [ ] **Step 2: 改 nanosleep_should_unblock（trap.c）**

`kernel/arch/x86_64/trap.c`（约 line 951-958）:

```c
static bool nanosleep_should_unblock(struct task_struct *waiter)
{
    return clocksource_read_ns() >= waiter->wakeup_ns;
}
```

- [ ] **Step 3: 改 SYS_clock_gettime（trap.c，REALTIME==MONOTONIC 同迁）**

`trap.c` `SYS_clock_gettime`（约 line 1875-1896），把 `uint64_t ms = jiffies * 10; ...` 换成：

```c
        uint64_t ns = clocksource_read_ns();
        tp->tv_sec  = ns / 1000000000ULL;
        tp->tv_nsec = ns % 1000000000ULL;
        regs->rax = 0;
```

（`clk_id` 校验、`tp` 指针校验、`-EINVAL`/`-EFAULT` 分支原样保留。）

- [ ] **Step 4: 改 SYS_nanosleep（trap.c，含 do/while + EINTR rem 下溢保护）**

`trap.c` `SYS_nanosleep`（约 line 1897-1935），核心改动：

```c
        uint64_t ns = 0;
        if (req && (uint64_t)req < current->addr_limit)
            ns = req->tv_sec * 1000000000ULL + req->tv_nsec;

        uint64_t target_ns = clocksource_read_ns() + ns;
        current->wakeup_ns = target_ns;

        int r;
        do {
            r = blocker_wait(nanosleep_should_unblock, BLOCKER_NANOSLEEP, true);
        } while (r == 0 && clocksource_read_ns() < target_ns);
        current->wakeup_ns = 0;

        if (r == -EINTR) {
            uint64_t now_ns = clocksource_read_ns();
            uint64_t remain_ns = (now_ns < target_ns) ? (target_ns - now_ns) : 0;  // 保留下溢保护
            if (rem && (uint64_t)rem < current->addr_limit) {
                rem->tv_sec  = remain_ns / 1000000000ULL;
                rem->tv_nsec = remain_ns % 1000000000ULL;
            }
            regs->rax = -EINTR;
        } else {
            regs->rax = 0;
        }
```

> 删除原 `ticks`/`(ns+9999999)/10000000` 的 10ms 量化；`wakeup_jiffies` 全部改 `wakeup_ns`。

- [ ] **Step 5: poll.c deadline 迁纳秒**

`kernel/fs/poll.c` `do_poll_core`（约 line 391-405）:

```c
    uint64_t deadline = 0;
    bool timed = (timeout_val > 0);
    if (timed) {
        deadline = clocksource_read_ns() + (uint64_t)timeout_val * 1000000ULL;  // ms→ns
        poll_tmo_register(pt, deadline);
    }
```

`poll.c` 里所有 `jiffies >= deadline` 比较（约 line 439, 452）改为：

```c
        if (timed && clocksource_read_ns() >= deadline) { ... }
```

在 `poll.c` 顶部加 `#include <kernel/clocksource.h>`。

- [ ] **Step 6: poll.h deadline 注释**

`kernel/include/kernel/poll.h`（约 line 81）:

```c
    uint64_t                  deadline; // 纳秒 deadline（clocksource_read_ns 时间轴）
```

- [ ] **Step 7: 编译 + 运行验证（门②③）**

Run: `make -C kernel clean && make disk.img && make run`

Expected（spec §9 门②③）: `clock_gettime` 连续两次读（间插忙等）反映亚 tick 变化；`nanosleep(15ms)` ≥15ms 且 <25ms；`poll(100ms timeout)` ~100ms；`busybox sleep 5` ~5s。

- [ ] **Step 8: 跑用户态 systest**

Run: `make test-syscall`

Expected: 全绿（含 test_clock_gettime / test_nanosleep / test_select）。

- [ ] **Step 9: Commit**

```bash
git add kernel/include/kernel/task.h kernel/arch/x86_64/trap.c kernel/fs/poll.c kernel/include/kernel/poll.h
git commit -m "feat(time): CLOCK_MONOTONIC/REALTIME + nanosleep + poll 迁纳秒"
```

---

## Task 5: 内核 jiffies 频率 selftest + 验证门证据 + 回归

**Files:**
- Modify: `kernel/test/test_timer.c`（补 `test_timer_jiffies_hz`）
- Modify: `kernel/test/selftest.c`（注册）
- Modify: `user/systest.c`（扩展 time 断言）

**Interfaces:**
- Consumes: `clocksource_freq_hz()`/`clocksource_read_ns()`、`jiffies`、`arch_cycle_counter()`
- Produces: 无新接口

- [ ] **Step 1: 写 test_timer_jiffies_hz**

`kernel/test/test_timer.c` 追加：

```c
// 测 jiffies ~100Hz：TSC 采样 ~500ms 窗口，断言 Δjiffies ≈ 50（±10%）。
// 注意：selftest.h 建议测试 sub-ms，本测试是 spec §11 明确要求的 500ms 窗口，
// 例外处理。
int test_timer_jiffies_hz(void)
{
    uint64_t freq = clocksource_freq_hz();
    if (!freq) {
        serial_printk("[selftest] timer_jiffies_hz: no freq, skip\n");
        return 0;  // 无法精确计时，跳过（不算失败）
    }
    uint64_t tsc0 = arch_cycle_counter();
    uint64_t jif0 = jiffies;
    uint64_t budget = freq / 2;   // 0.5s
    while (arch_cycle_counter() - tsc0 < budget)
        arch_cpu_pause();
    uint64_t dj = jiffies - jif0;
    if (dj >= 45 && dj <= 55)     // 期望 ~50（±10%）
        return 0;
    serial_printk("[selftest] timer_jiffies_hz: dj=%lu (expected ~50)\n", (unsigned long)dj);
    return -1;
}
```

- [ ] **Step 2: selftest.c 注册**

`kernel/test/selftest.c` 的 `selftest_run_all` 注册区（`#ifdef OS01_SELFTEST` 内）:

```c
    selftest_register("timer_jiffies_hz",  test_timer_jiffies_hz);
```

（`test_timer_jiffies_hz` 的 forward-declare 已在 Task 2 加。）

- [ ] **Step 3: 扩展 systest.c 时间断言**

`user/systest.c` `test_clock_gettime`（约 line 424-435）加亚 tick 断言（用 `arch_cycle_counter` 不可得，用户态用两次 gettimeofday 差值 + 忙等）:

```c
static void test_clock_gettime(void)
{
    struct timespec a, b;
    int ra = clock_gettime(CLOCK_MONOTONIC, &a);
    // 短忙等（固定空转，用户态无 TSC，靠 tick 前进），确保时间前进。
    for (volatile int i = 0; i < 2000000; i++) {}
    int rb = clock_gettime(CLOCK_MONOTONIC, &b);
    CHECK3(ra == 0 && rb == 0, "clock_gettime", "returns 0");
    CHECKF(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec),
           "clock_gettime", "monotonic", "a=%lu.%09lu b=%lu.%09lu",
           (unsigned long)a.tv_sec, (unsigned long)a.tv_nsec,
           (unsigned long)b.tv_sec, (unsigned long)b.tv_nsec);
}
```

（原测试已覆盖「单调不减」，本次主要靠内核 selftest + QEMU trace 证明 100Hz；用户态断言保持。）

- [ ] **Step 4: 全回归（门③）**

Run: `make -C kernel clean && make disk.img && make test-syscall && make test-network`

Expected: systest 70/70、EEVDF 双核、lwIP DHCP 续约全绿；boot 到 AHCI/FS 挂载正常（门③.10）。

- [ ] **Step 5: 收尾证据 + Commit**

按 spec §9 逐条附证据（trace 摘录 + 计时数字）到 commit message 或 doc 更新；确认无残留 debug 探针。

```bash
git add kernel/test/test_timer.c kernel/test/selftest.c user/systest.c
git commit -m "test(time): 内核 jiffies 频率 self-test + 验证门证据 + 回归"
```

---

## Self-Review 记录

**Hermes 外部评审修订（v7，本次）**：
- 问题 1「lapic_hz_out 漏 ×2」→ **误报，方向反了**。`lapic_timer_hz` 语义 = 递减率
  （÷2 已折算），不是真实频率。真正的 bug 是 plan Task 3 Step 1 写了 `elapsed * 200`
  （多 ×2），已改回 `elapsed * 100`。RTC 路径 `elapsed * 1024 / n` 本就正确（不 ×2），
  加注释说明语义。spec §5.1 新增「lapic_timer_hz 语义」段。
- 问题 2「irq_mask 对称性」→ 已读 ioapic.c 源码确认对称（disable 置 mask 保留路由，
  enable 重建清 mask），在 plan Task 1 Step 7b 加验证结论，无需实现者再查。
- 问题 3「GS 时序」→ spec §7.4 补 tick_handler poll 扫描路径；plan Task 1 Step 5
  tick_handler 代码 + Task 3 Step 4 各加「boot 期 poll 空 → 不读 GS」注释。
- 问题 4「500ms 超时硬编码 3GHz」→ plan Task 2 Step 2 改 2^32 cycle 宽松兜底
  （@8.6GHz≈500ms、@3GHz≈1.43s），注释说明不依赖精度、只防无限自旋。
- 问题 5「divisor 注释矛盾」→ plan Task 3 Step 1 注明原 lapic_timer.c "divide by 1"
  是错的，实现时统一 ÷2。
- 承重墙建议 → plan Task 4 开头加「先过 Task 3 门①再动」前置 + KVM lfence 提示。

**1. Spec coverage**（spec §2-§11 → 任务映射）:
- §3 架构分层 / §4 接口 → Task 1（clocksource.h/clockevent.h/time.c/tick.c）
- §5 TSC 校准（CPUID15h + RTC PIE 联合 + mult/shift）→ Task 1（mult/shift）+ Task 2（RTC PIE）
- §6 迁移明细（clock_gettime/nanosleep/poll）→ Task 4
- §7 初始化顺序（PIT 先跑 + tick_start + BSP 二次启动）→ Task 1（subsys）+ Task 3（main.c tick_start）
- §8 错误处理（失败矩阵/超时/握手采样）→ Task 2（PIE 超时）+ Task 3（握手采样 + irq_mask）
- §9 验证门 → 各任务 Step 的验证命令 + Task 5 全回归
- §10 文件清单 → File Structure 全列（`kernel/Makefile` 加 time/*.c wildcard 是 spec 路径 `kernel/time/` 的落点）

**2. Placeholder scan**: 无 TBD/TODO；每个代码步骤含实际代码。Task 1 Step 6/8 的「Task 3 替换」是跨任务指针而非占位（当前任务可独立编译通过）。

**3. Type consistency**: `lapic_timer_start` 从 `void`→`bool` 在 Task 3 Step 2 定义，Step 8（smp.c）/Step 9（subsys_percpu.c）一致引用；`wakeup_ns` 在 Task 4 Step 1 定义、Step 2/4 一致；`rtc_pie_calibrate`/`lapic_timer_set_premeasured` 在 Task 2 Step 1/2 定义、Step 3 一致；`irq_mask/irq_unmask` 提前到 Task 1 Step 7b（tick.c 在 Task 1 就引用它们），Task 3 不再重复定义。

---

## Execution Handoff

计划完成并保存到 `docs/superpowers/plans/2026-08-17-timer-clocksource-clockevent.md`。
