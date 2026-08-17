# Timer 重构：clocksource + clockevent 双层抽象 设计文档

- 日期：2026-08-17
- 状态：待用户 review（尚未进入实现）
- 前置：`docs/pit-200hz-handoff.md`（HANDOFF，2026-08-17）
- 目标：修复 QEMU TCG 下 PIT 200Hz 伪影，使 tick 与时间在 QEMU 与真实硬件都正确，并为未来 aarch64 移植预留接口

---

## 1. 背景与问题

OS01 的时间系统当前以单一 `jiffies` 计数器为唯一时间基，只在 PIT 的
`pit_handler`（IRQ0 → BSP）里 `jiffies++`，并假设 1 jiffy = 10 ms。所有时间
消费者都从它派生：

- `clock_gettime(CLOCK_MONOTONIC)` = `jiffies * 10`（硬编码 ms）
- `nanosleep` / `poll` / `select` 的唤醒 deadline = `jiffies + ticks`
- EEVDF 调度器时间片（`update_curr` 每 tick `vruntime += 1`，
  `EEVDF_MIN_SLICE = 10 tick = 100ms`）
- lwIP 粗超时、watchdog、kernel timer 轮

HANDOFF 已定位根因：**QEMU TCG 的 IOAPIC 边沿投递没有上升沿检测**，PIT
mode-3 方波在每个 10ms 周期内两次驱动 `level=1`，导致 guest 收到 200Hz 的
IRQ0。于是 1 jiffy 实际只有 5ms，所有基于 jiffies 的时间/超时在 QEMU 下 2×
加速。**这不是 OS01 的 bug**（guest 侧 1:1 处理，已测量）；真实硬件上 PIT 是
真 100Hz。修复方向：把 OS tick 源切到 LAPIC（LVT 本地投递，天然免疫该伪影），
并把时间基从 tick 里解耦出来。

## 2. 目标与非目标

### 目标

1. tick 源在 QEMU TCG 与真实硬件都稳定 100Hz（LAPIC 周期模式，PIT 留作 fallback）。
2. 引入 clocksource / clockevent 双层抽象：**时间（纳秒）与 tick（调度）解耦**，
   `jiffies` 退化为纯调度 tick 计数，不再当时间用。
3. `CLOCK_MONOTONIC`、`nanosleep`、`poll`、`select` 的 deadline 全部迁到
   clocksource 纳秒时间戳。
4. 抽象接口不硬编码 x86，为 aarch64（`cntvct_el0` + CNTP 定时器）预留接入点。

### 非目标（YAGNI）

- one-shot clockevent / hrtimer 框架（方案 C，后续增强）
- HPET 作 clocksource（方案 B，后续可选；x86 跨平台最稳，但破坏单一
  `arch_cycle_counter` 抽象且新增 MMIO 驱动）
- TSC 跨核同步、deep-TSC 处理（仅做 invariant-TSC 检测 + 警告）
- aarch64 的 CNTP 定时器实现代码（只留接口 hook，不写 ARM 代码）

## 3. 架构分层

```
┌─────────────────────────────────────────────────────┐
│  消费者：clock_gettime / nanosleep / poll / select   │ ← 用纳秒时间戳
├─────────────────────────────────────────────────────┤
│  clocksource 层（kernel/time/clocksource.c，arch无关）│ ← 单调纳秒
│     clocksource_read_ns() = arch_cycle_counter()*mult>>shift
├─────────────────────────────────────────────────────┤
│  clockevent 层（kernel/time/tick.c，arch无关）        │ ← tick 语义
│     tick_handler(): jiffies++ / nsec扫描 / need_resched
├─────────────────────────────────────────────────────┤
│  arch hook（kernel/include/kernel/arch/*.h）          │
│     arch_cycle_counter()  [已有]  arch_cycle_freq()  [新增]
│     arch_tick_start() [新增]
└─────────────────────────────────────────────────────┘
调度器（EEVDF vruntime/deadline=ticks）、watchdog、kernel timer 轮、lwIP
粗超时 → 继续用 jiffies（不变）
```

关键点：调度器继续挂 jiffies（修好 tick 源后 jiffies 变回真 10ms，EEVDF 的 2×
问题自动消失）；时间消费者改挂 clocksource 纳秒。这与 Linux 一致。

## 4. 接口定义

### 4.1 arch 无关核心

```c
// kernel/include/kernel/clocksource.h
void        clocksource_init(void);           // 从 arch_cycle_freq() 算 mult/shift
uint64_t    clocksource_read_ns(void);        // 单调纳秒（含 wrap 处理）
uint64_t    clocksource_cycles(void);         // 原始 cycle（调试/校准用）

// kernel/include/kernel/clockevent.h
void        tick_init(void);                  // 选 tick 源：LAPIC → PIT fallback
void        tick_handler(void);               // 统一 tick 语义（arch IRQ handler 调用）
uint64_t    tick_get_jiffies(void);           // 访问 jiffies（可选封装）
```

核心换算用 Linux 式 mult/shift 固定点（`__uint128_t` 中间乘防溢出，避免热路径
64 位除法）：

```c
static inline uint64_t clocksource_read_ns(void) {
    uint64_t c = arch_cycle_counter();
    return (uint64_t)(((__uint128_t)c * clocksource_mult) >> clocksource_shift);
}
```

### 4.2 arch hook（新增）

```c
// x86: 返回校准后的 TSC 频率；aarch64: 返回 CNTFRQ_EL0 值
uint64_t arch_cycle_freq(void);

// 启动 arch tick 源：x86 配 LAPIC 周期模式（失败退 PIT）；aarch64 配 CNTP
void     arch_tick_start(void);
```

现有 `arch_cycle_counter()`（`kernel/include/kernel/arch/cpu.h`）已经抽象好了：
x86 用 `rdtsc`，aarch64 用 `mrs cntvct_el0`。ARM 未来只需实现两个 hook——
`arch_cycle_freq()` 读 `CNTFRQ_EL0`（一行，无需校准）、`arch_tick_start()` 写
`CNTP_TVAL_EL0`/`CNTP_CTL_EL0` 并让 CNTP IRQ handler 调 `tick_handler()`，
clocksource/clockevent 核心一行不改。

## 5. TSC 频率校准（x86，真实硬件的关键）

三级优先级：

1. **CPUID leaf 15h**（首选）：`ECX` = core crystal Hz（非 0 时有效），
   `TSC_Hz = ECX × EBX / EAX`。真硬件普遍提供，零延时。
2. **RTC 周期中断校准**（回落；QEMU TCG 走这条，因 TCG 返回 0）：配置 RTC 的
   PIE（寄存器 0x0B bit6 使能，0x0A 低 4 位设 1024Hz），注册 IRQ8 handler，
   测固定 PIE 周期的 TSC 差值。HANDOFF 的 TSCCAL probe 即此法，实测 0.006%
   误差。CMOS 访问需屏蔽 NMI（0x70 bit7）。
3. **ARM**：`arch_cycle_freq()` 直接读 `CNTFRQ_EL0`，无需校准。

需新增一小段 RTC 周期中断代码（现有 `rtc.c` 只有 wall-clock 读秒）。这是
「真实硬件要能工作」的代价——QEMU TCG 没有 CPUID 15h，只有 RTC 校准能拿到
TSC 频率。

## 6. 迁移明细（jiffies → 纳秒）

| 位置 | 现状 | 迁移后 |
|---|---|---|
| `trap.c` `SYS_clock_gettime` | `ms = jiffies * 10` | `ns = clocksource_read_ns()` |
| `trap.c` `SYS_nanosleep` | `target = jiffies + ticks`(10ms) | `target_ns = now_ns + ns` |
| `task.h` `wakeup_jiffies` | 存 jiffies | 改名 `wakeup_ns`，存纳秒 |
| `trap.c` `nanosleep_should_unblock` | `jiffies >= wakeup_jiffies` | `clocksource_read_ns() >= wakeup_ns` |
| `poll.c` `do_poll_core` | `deadline = jiffies + (ms+9)/10` | `deadline = now_ns + ms*1e6` |
| `poll.h` `poll_timeout_node_t.deadline` | jiffies | 纳秒（字段类型 uint64_t 不变） |
| `pit.c` `pit_handler` 的 poll 扫描 | `jiffies >= n->deadline` | 迁到 `tick_handler()`，用 `clocksource_read_ns()` |

**精度说明**：deadline 值本身变成纳秒精确，但唤醒仍由 tick 扫描驱动（10ms 扫
一次），所以唤醒粒度上限仍 ~10ms——这正确且够用。真正要亚 tick 唤醒需 one-shot
（方案 C，本次不做）。

**`tick_handler()` 统一 tick 语义**（arch 无关，承接现在 `pit_handler` 的主体）：
`jiffies++` → poll 超时扫描（纳秒比较）→ `need_resched=1` → `watchdog++` →
到期的 timer 触发 `TIMER_SIRQ`。BSP 的 LAPIC handler 和 PIT handler 都调它。
**只有 BSP 驱动 jiffies**。

**`serial_poll()` 归属**：现在 `pit_handler` 每 tick 调 `serial_poll()`（x86 串口
轮询 fallback）。它与「通用 tick」无关，是 x86 设备层的事，**留在 x86 的
LAPIC/PIT handler 层**，不放进 arch 无关的 `tick_handler()`。

## 7. 初始化顺序（subsys.c 重排）

**现在**（phase 4 顺序）：`timer` → `pit`（启动 100Hz）→ `lapic-timer`
（校准 + 注册，不启动）。

**迁移后**：

```
phase 3:  apic, pic                          ← 不变
phase 4:  timer        (jiffies=0 + softirq注册)      ← 不变
          clocksource  (mult/shift ← arch_cycle_freq)  ← 新增
          pit          (init，但掩蔽不启动，作 fallback) ← 改
          lapic-timer  (TSC 校准 + 注册 IDT gate，不启动) ← 改校准
显式启动: tick_start()  (percpu_init + GS 安装之后)
          → arch_tick_start()：LAPIC 周期 100Hz 成功
          → 失败(lapic_timer_hz==0) 则回退 unmask PIT
```

**BSP 的 LAPIC handler 改动**：现在 `lapic_timer_handler` 里 `if (cpu_id() != 0)`
只给 AP 用；改成 BSP 走 `tick_handler()`（驱动 jiffies），AP 只做
`need_resched + watchdog`。`lapic_timer_start(100)` 现在 BSP 和 AP 都调（BSP 由
`tick_start` 调，AP 由 `smp.c` 现有逻辑调）。

## 8. 错误处理

### 8.1 失败矩阵（tick 源 与 时间基 独立回退）

```
                  ┌── CPUID 15h 有效 ──→ 用它
TSC 频率 ────────┤
（clocksource）   ├── CPUID 15h=0 ────→ RTC 周期中断校准（IRQ8, 1024Hz）
                  └── RTC 也失败 ─────→ clocksource 退化为 jiffies×10ms
                                        （= 现状，时间基不再纳秒）

tick 源 ────────── LAPIC 周期 100Hz 启动成功 → 主 tick
（clockevent）     └── lapic_timer_hz==0 ──→ unmask PIT（硬件 100Hz，准的）
```

两条线正交：即使 LAPIC 失败退回 PIT，clocksource 仍可用 TSC（纳秒时间照常）；
即使 TSC 频率拿不到，LAPIC tick 仍驱动 jiffies。最坏情况（两者都挂）= 完全回到
今天的行为，系统不会比现在更糟。

### 8.2 具体点

1. TSC 频率三级回落，每级失败打印 `log_warn` 并落到下一级；最终兜底
   `clocksource_active = false`，`clocksource_read_ns()` 返回
   `jiffies * 10'000'000`。
2. invariant-TSC 检测：`CPUID 0x80000007 EDX bit8`；非 invariant 时打印警告
   （P-state 变频率影响速率）但继续用（现代 x86 普遍 invariant）。
3. LAPIC 启动失败：`lapic_timer_start()` 改返回 bool；`tick_start()` 收到 false
   时 unmask PIT。除数 11931 不动。
4. 64 位 wrap：`rdtsc`/`cntvct_el0` 64 位，3GHz 下 ~195 年才 wrap，忽略；但所有
   deadline 比较用环绕安全式 `(int64_t)(now_ns - deadline_ns) >= 0`。
5. mult/shift 精度：`mult` 32 位、`shift` 保证换算误差 < 1ppm。
6. RTC CMOS 访问：屏蔽 NMI（0x70 bit7）、检测 `is_updating_rtc()`（已有）再读。

## 9. 验证门（MANDATORY）

每条必须附证据（trace 摘录 + 计时数字），遵守「QEMU-verified evidence before
progress claims」规则。

### ① tick 频率正确性

1. `-trace events=apic_local_deliver` → LAPIC LVT TIMER ~**100Hz**（非 200/629）
2. 串口每 500 tick 打印 → jiffies ~**100Hz**，与 LAPIC 投递比 **1.000**
3. `busybox sleep 5` → **~5.0s** 墙钟（原 PIT 下 2.5s）
4. `clock_gettime` Δ vs host → **1:1**（±1%）
5. `-smp 1` 和 `-smp 2` 都过

### ② 纳秒迁移正确性

6. `clock_gettime` 连续两次读：间隔 <10ms 也能反映**亚 tick 变化**（证明已从
   jiffies 迁到 clocksource）
7. `nanosleep(15ms)` → 实际 ≥15ms 且 <25ms
8. `poll(100ms timeout)` 空 fd → 返回 ~100ms（非 50ms）

### ③ 回归

9. 全测试套件绿：EEVDF（双核）、poll/select、lwIP DHCP 续约、nanosleep
   systests、`spawn >2 init.elf`、70/70 systest

### ④ fallback 路径（一次性验证，非每次）

10. 人为 `lapic_timer_hz=0` → 回退 PIT 仍正常运行（tick 走 PIT，时间走 TSC）
11. 人为禁用 RTC 校准 → 时间基退 jiffies×10ms，系统可跑

## 10. 文件改动清单

### 新增（arch 无关核心）

| 文件 | 内容 |
|---|---|
| `kernel/time/clocksource.c` | `clocksource_init()` / `read_ns()` / `cycles()` / mult/shift |
| `kernel/time/tick.c` | `tick_init()` / `tick_start()` / `tick_handler()` |
| `kernel/include/kernel/clocksource.h` | 接口声明 |
| `kernel/include/kernel/clockevent.h` | 接口声明 |

### 修改

| 文件 | 改动 |
|---|---|
| `kernel/apic/lapic_timer.c` | 校准改 TSC 基准；handler BSP→tick_handler / AP→need_resched；start 返回 bool |
| `kernel/driver/pit.c` | pit_init 改掩蔽不启动；抽 pit_enable()；handler 改调 tick_handler() |
| `kernel/driver/rtc.c` + `rtc.h` | 新增 RTC 周期中断（PIE + IRQ8 + TSC 校准辅助） |
| `kernel/arch/x86_64/subsys.c` | 初始化顺序重排：加 clocksource，pit 改掩蔽，显式 tick_start |
| `kernel/arch/x86_64/trap.c` | clock_gettime / nanosleep / nanosleep_should_unblock 迁纳秒 |
| `kernel/include/kernel/task.h` | `wakeup_jiffies` → `wakeup_ns` |
| `kernel/fs/poll.c` + `kernel/include/kernel/poll.h` | deadline 语义迁纳秒（字段类型不变） |
| `kernel/include/kernel/arch/cpu.h` | 加 `arch_cycle_freq()` 声明 |
| `kernel/arch/x86_64/cpu.h` | `arch_cycle_freq()` x86 实现（CPUID15h→RTC→0） |
| `kernel/arch/aarch64/cpu.h` 内联 | `arch_cycle_freq()` 读 CNTFRQ_EL0（仅接口） |
| `kernel/arch/x86_64/smp.c` | AP 的 lapic_timer_start 适配 bool 返回值（AP 失败不致命） |

## 11. 测试与提交计划

### 提交序列（小步、语义化前缀，每步可独立验证）

1. `feat(time): clocksource + clockevent 双层抽象（TSC/cntvct_el0，接口层）`
   — 纯新增，编译过，无行为变化
2. `feat(time): TSC 频率校准（CPUID15h + RTC PIE fallback）`
   — 可独立验证 `arch_cycle_freq()` 正确
3. `feat(tick): BSP 切 LAPIC 周期 tick，PIT 掩蔽留 fallback`
   — **验证门 ①② 在此步过**
4. `feat(time): CLOCK_MONOTONIC + nanosleep + poll 迁纳秒`
   — **验证门 ③ 在此步过**
5. `test(time): 验证门证据 + 回归修复` — 全测试套件 + fallback 路径（门 ④）

### 每步 RED→GREEN

- RED：先跑现有 systest 确认基线绿（记录），再写/改对应测试
- GREEN：实现后该步验证门必须过
- 全回归：提交前跑满 systest（70/70）+ 双核 + lwIP DHCP

### 测试载体

`user/systest.c` 现有 `test_clock_gettime` / `test_nanosleep` / `test_select` 已
覆盖时间路径，扩展断言（亚 tick 分辨率、nanosleep 精确度），加一个
`test_lapic_tick` 验证 jiffies 频率。

### 回退

任何一步回归失败 → 停下定位，不跨步推进；debug 探针只读、提交前移除。
