# 定时器系统（Timer 重构后）

> 本文档描述 timer 重构（2026-08-17~18，clocksource+clockevent 双层抽象）之后的定时器架构。
> 重构解决了 QEMU TCG 下 PIT 200Hz 伪影（jiffies 2x），并把精粒度时间与 tick 解耦。
> 设计文档：`docs/superpowers/specs/2026-08-17-timer-clocksource-clockevent-design.md`（v8）。

## 架构总览

```
┌─────────────────────────────────────────────────────┐
│ 精粒度消费者: clock_gettime / nanosleep / poll/select │ ← 纳秒时间戳
├─────────────────────────────────────────────────────┤
│ clocksource 层 (kernel/time/clocksource.c, arch无关) │ ← 单调纳秒
│   clocksource_read_ns() = (cycle+offset)*mult>>shift │
├─────────────────────────────────────────────────────┤
│ clockevent 层 (kernel/time/tick.c, arch无关)         │ ← tick 语义
│   tick_handler(): jiffies++ / poll超时纳秒扫描 /      │
│                   need_resched / watchdog / TIMER_SIRQ
├─────────────────────────────────────────────────────┤
│ arch hook (kernel/include/kernel/arch/*.h)           │
│   arch_cycle_counter() [已有]  arch_cycle_freq() [新增]
│   arch_tick_start() [新增]  ← x86: LAPIC; aarch64: CNTP(预留)
└─────────────────────────────────────────────────────┘
粗粒度消费者: EEVDF、watchdog、kernel timer 轮、lwIP 粗超时、AHCI → jiffies
（10ms/tick，不变）
```

**核心原则**：精粒度时间（用户可感知）挂在 clocksource 纳秒上；粗粒度超时继续用
jiffies（tick 粒度）。tick 源修好后 jiffies 恢复真 10ms，两类消费者都正确。

## jiffies 语义（精确定义）

`jiffies` 仍隐含 **10ms/tick** 的时间语义，精度就是 tick 粒度。它继续服务于：

- 调度器（EEVDF vruntime/deadline 以 tick 计）—— tick 语义，不是时间
- watchdog（`watchdog_counter`）、kernel timer 轮（`kernel/time/timer.c`）
- lwIP 粗超时（`sys_arch.c`，粒度 100ms+ 足够）
- AHCI 初始化 busy-wait 超时（`WAIT_WHILE`）
- `LWIP_RAND()` 种子、`hang.c` 调试时间戳

**迁到 clocksource 纳秒的只有精粒度路径**：`clock_gettime`（MONOTONIC 与
REALTIME）、`nanosleep`、`poll`、`select` 的 deadline。这些路径用户可感知精度。

## 核心组件

### 1. clocksource 层 — 单调纳秒（kernel/time/clocksource.c）

```c
void        clocksource_init(void);       // arch_cycle_freq() 校准 + mult/shift
static inline uint64_t clocksource_read_ns(void);  // (cycle+offset)*mult>>shift
uint64_t    clocksource_cycles(void);     // 原始 cycle（调试/校准用）
uint64_t    clocksource_freq_hz(void);
```

- `clocksource_init()`：调 `arch_cycle_freq()`（x86: TSC 频率，CPUID15h + RTC PIE
  联合校准）。freq 为 0 时 `clocksource_active=false`（回退路径）。
- `compute_mult_shift(freq, &mult, &shift)`：找最大 shift 使
  `mult = (1e9 << shift)/freq` 落在 `[1, 2^32)`，尽量接近 2^31 最大化精度。
  - s 上限 64：>1GHz CPU 需要 shift>31（如 2.994GHz → 33）
  - **中间值必须 `__uint128_t`**：`1e9 << s` 在 s≥35 时溢出 uint64_t，
    而 ≥4.3GHz 的 TSC 恰好需要 shift≥35（commit 496a210 实证修复）
  - freq 在 1MHz~10GHz 时循环很快（s≈22..34）
- `clocksource_read_ns()`：静态 inline（热路径，syscall / tick_handler /
  sched_unblock 都调），读 `arch_cycle_counter() + this_cpu()->tsc_offset` 换算。

### 2. clockevent 层 — tick 语义（kernel/time/tick.c）

```c
void tick_start(void);     // 显式启动（percpu+GS 就绪后调用）：掩 PIT → LAPIC 接管或回退
void tick_handler(void);   // 统一 tick 语义（arch IRQ handler 调用，无参）
uint64_t tick_get_jiffies(void);
```

> 注意：**没有独立的 tick_init()**——PIT 在 phase 4 由 `pit_init()` 照常启动，
> `tick_handler()` 由 PIT/LAPIC 的 arch IRQ handler 直接调用；`tick_start()` 在
> `kernel/kernel/main.c:294`（percpu+GS 就绪后）显式调用。

**tick_start() 的源选择**：

```
1. irq_mask(0)                     // 先掩 PIT IRQ0，防交接窗口双计 jiffies
2. if (arch_tick_start())          // LAPIC 周期模式接管（每 CPU）
       PIT 保持掩蔽                // LAPIC 成功 = QEMU 免疫 200Hz 伪影
   else
       irq_unmask(0)               // 回退 PIT（LAPIC 未校准/失败）
```

**tick_handler() 职责**（从旧 pit_handler 迁来 + 扩展）：

1. `jiffies++`
2. **poll 超时扫描**（纳秒比较）：遍历 `poll_timeout_head` 注册表，
   `clocksource_read_ns() >= n->deadline` → `wait_queue_wake_all(n->wq)`
   （poll.c 注册 deadline 用 `clocksource_read_ns() + timeout*1e6`，同一时间轴）
   - ⚠️ 取锁必须 `spin_lock_irqsave`：poll.c 持锁时被本 tick 抢占会自旋死锁
   - ⚠️ boot 期 `poll_timeout_head` 恒 NULL（poll 只在用户态进程里调）——
     此短路保证 GS base 装之前不调 `clocksource_read_ns()`（读 this_cpu()->tsc_offset）
3. `this_cpu()->need_resched = 1` + `watchdog_counter++`
4. timer wheel 到期（`expire_jiffies <= jiffies`）→ `set_softirq_status(TIMER_SIRQ)`

### 3. arch hook

| hook | x86_64 | aarch64（预留） |
|------|--------|----------------|
| `arch_cycle_counter()` | RDTSC | `cntvct_el0`（待实现） |
| `arch_cycle_freq()` | CPUID15h + RTC PIE 联合校准 | 读 CNTFRQ（待实现） |
| `arch_tick_start()` | LAPIC 周期模式（每 CPU 写 per-LAPIC DIV） | CNTP（待实现） |

### 4. LAPIC 定时器（tick 源，kernel/intr/apic/lapic_timer.c）

- `lapic_timer_init()`：注册 IDT 门 + 校准（subsys Phase 4）
- `lapic_timer_calibrate()`：**RTC PIE 联合结果优先**（250ms 窗口更稳），
  否则 TSC 窗口（~10ms）
- `lapic_timer_start()`：每 CPU 周期模式启动
  - ⚠️ **必须写 per-LAPIC `LAPIC_TIMER_DIV`**：AP 复位后 DIV=÷1，静态值 ÷2
    折算会导致 AP 200Hz（QEMU 双核 298Hz 实证，spec v8 门①）
- `lapic_timer_handler()`：调 `tick_handler()` + 发送 EOI

### 5. PIT（fallback + boot 窗口，kernel/driver/pit.c）

- `pit_init()`：PIT 100Hz（IOAPIC 或 PIC），**boot 早期先跑**保证 tick 可用
- `pit_handler()` → 调 `tick_handler()`
- `tick_start()` 后：LAPIC 成功 → PIT 掩蔽；失败 → PIT 继续
- PIT 200Hz 伪影背景：QEMU TCG 的 IOAPIC edge 投递无上升沿检测，PIT mode-3
  每 10ms 双触发 → guest 收 200Hz。**这不是 OS bug**（guest 侧 1:1 处理），
  真实硬件 PIT 是真 100Hz。LAPIC LVT 本地投递天然免疫。证据链见
  `docs/pit-200hz-analysis.md`。

### 6. 软件定时器管理（timer wheel，kernel/time/timer.c，未变）

```c
typedef struct timer {
    struct list_head list;
    void (*func)(void * data);
    void * data;
    uint64_t expire_jiffies;
} timer_t;
```

- `init_timer` / `create_timer` / `add_timer` / `del_timer` / `do_timer`
- 链表按 `expire_jiffies` 排序；tick_handler 检查头部到期则触发 `TIMER_SIRQ`
- 软中断 `do_timer` 遍历到期回调（硬中断处理时间短，回调简短原则不变）

## 时间转换

```c
// jiffies ↔ 毫秒（tick = 10ms）
uint64_t ms_to_jiffies(uint64_t ms)   { return ms / 10; }
uint64_t sec_to_jiffies(uint64_t sec) { return sec * 100; }
uint64_t jiffies_to_ms(uint64_t jif)  { return jif * 10; }
uint64_t jiffies_to_sec(uint64_t jif) { return jif / 100; }
```

精粒度纳秒（clocksource）直接 `ns / 1e9 = sec`，`ns % 1e9 = nsec`。

## 子系统注册（kernel/arch/x86_64/subsys.c）

```
Phase 4:
  1. timer（必需）→ timer_init()      — jiffies、链表头、软中断注册
  2. pit（可选）→ pit_init()          — PIT 100Hz，boot 窗口 tick（handler 直接调 tick_handler）
  3. lapic-timer（可选）→ lapic_timer_init() — 门 + 校准
  4. clocksource（必需）→ clocksource_init() — TSC 频率 + mult/shift
main.c（percpu_init + GS 基址设置后）:
  → tick_start()                    — 掩 PIT → arch_tick_start()（LAPIC）或回退 PIT
```

## 定时器使用示例（timer wheel）

```c
void callback(void *data) { color_printk(GREEN, BLACK, "Timer expired! %p\n", data); }

timer_t *t = create_timer(callback, NULL, 50); // 50 * 10ms = 500ms
add_timer(t);                                  // 到期自动删除（一次性）
```

## 代码结构

| 文件 | 职责 |
|------|------|
| `kernel/time/clocksource.c` + `include/kernel/clocksource.h` | 单调纳秒层（mult/shift 换算） |
| `kernel/time/tick.c` + `include/kernel/clockevent.h` | tick 语义层（jiffies/poll 扫描/源选择） |
| `kernel/intr/apic/lapic_timer.c` | LAPIC tick 源（校准 + 周期模式 + per-LAPIC DIV） |
| `kernel/driver/pit.c` | PIT（boot 窗口 + fallback） |
| `kernel/time/timer.c` + `include/device/timer.h` | 软件定时器轮（timer wheel） |
| `kernel/arch/x86_64/`（arch hook） | TSC/RTC 校准、LAPIC 启动 |

## 精度说明

- tick 精度 10ms（LAPIC 周期模式 100Hz，与旧 PIT 一致）
- clocksource 纳秒精度受 TSC 频率与 mult/shift 影响（~1ns 分辨率，亚微秒实际）
- nanosleep/poll/select 的 deadline 用纳秒比较，唤醒仍由 tick 驱动
  （每个 tick 扫描一次到期项）——唤醒延迟 ≤ 1 tick（10ms）

## 故障排除

| 症状 | 排查 |
|------|------|
| tick 速率 2×（200Hz） | QEMU TCG PIT 伪影：确认 `tick_start()` 后 LAPIC 接管成功（`arch_tick_start()` 返回 true），PIT 保持掩蔽 |
| AP tick 速率异常（~200Hz） | `lapic_timer_start` 漏写 per-LAPIC `LAPIC_TIMER_DIV`（AP 复位 ÷1） |
| `clocksource_read_ns()` 崩溃 | GS base 装之前调用（boot 期）——tick_handler 有 `poll_timeout_head` 短路保护，新调用点需同样注意 |
| 定时器不执行 | timer 未 add / `expire_jiffies` 错误 / TIMER_SIRQ 未触发 |
| 定时器执行多次 | 回调中重复 add_timer |

---

## nanosleep 修复实施总结（已完成，2026-08-17）

> 状态：**已完成**（commit `2faccbc`）— 唤醒走 blocker（`wakeup_jiffies` + `BLOCKER_NANOSLEEP`）+ 掩码感知信号唤醒 + `-EINTR`/rem + `CLOCK_MONOTONIC`。systest 150/150。
> 连带发现的 PIT 200Hz（jiffies 2x）为 QEMU TCG artifact——已由 timer 重构根治（见上文），不再阻塞任何后续工作。证据链归档 `docs/pit-200hz-analysis.md`。

### 背景（tetris 开发中实证发现）

`SYS_nanosleep`（kernel/arch/x86_64/trap.c）睡眠实现无唤醒源：

```c
uint64_t target = jiffies + ticks;
while (jiffies < target) {
    current->state = TASK_INTERRUPTIBLE;   // 标记睡眠
    schedule();                              // dequeue 后不重新入队
    ...
}
```

schedule() 对 INTERRUPTIBLE 任务 dequeue 后**不重新入队**；nanosleep **没有注册任何唤醒机制**（无 wait queue、无 timer 回调、无 wake 路径），PIT tick 不唤醒普通睡眠任务 → 无 signal 任务**永久睡死**。

QEMU 实证：
- tetris game-over 后 `nanosleep(1s)` 永久卡死（无 `\e[?1049l`、init 不 respawn）
- busybox `sleep 1` **0.05s 瞬间返回**（pending signal 假醒，根本没睡）
- 与 poll bug 同源（1ef8e1f 已修）：睡眠/超时机制缺唤醒注册；nanosleep 比 poll 更严重（连 wq 都没有）

### 修复方案

| 方案 | 改动 | 代价 |
|------|------|------|
| A 最小可用 | nanosleep 循环**不置 INTERRUPTIBLE**（保持 RUNNING）→ schedule 轮询，每 tick 调度回来检查 jiffies | 10ms 粒度空转；信号打断靠循环检查 |
| B 事件驱动（推荐） | task 加 `wakeup_jiffies` 字段 + PIT tick 扫描唤醒到期的 INTERRUPTIBLE 任务（复用 poll timeout registry 思路，~30 行） | 干净、真睡眠；需处理多核并发唤醒 |

### 分步实现（TDD 门禁）

1. **Step 1 复现测试**（RED）：systest 加 nanosleep case——`nanosleep(100ms)` 断言实际睡眠时长 ≥80ms。当前实现：无 signal 时睡死（systest 超时失败）；有 signal 时假醒（时长断言失败）
2. **Step 2 GREEN**：方案 B 优先（wakeup_jiffies + PIT 扫描）；方案 A 作 fallback
3. **Step 3 回归**：systest 全量 + host 全绿 + QEMU 实证——`sleep 1` 真睡 ~1s（非 0.05s）；tetris game-over 后正常退出恢复终端
4. **Step 4 commit**：`fix(sched): nanosleep with real wakeup`（独立 commit）

### 连带问题（待查，不阻塞）

- **signal 假醒**：busybox `sleep 1` 0.05s 返回——pending signal 来源待确认（疑似 fork 复制父进程 pending SIGCHLD / exec 未清 signal）。修复后 sleep 应真睡 1 秒，假醒自然消除

---

## Timer 重构实施总结（commit 与验证）

> 目标：根治 QEMU TCG 下 PIT 200Hz 伪影（jiffies 2x），把精粒度时间与 tick 解耦，为 aarch64 预留时钟接口。前置：`docs/pit-200hz-handoff.md`（根因定位）。spec/plan 见 `docs/superpowers/specs/2026-08-17-timer-clocksource-clockevent-design.md`（v8 修订）。
> 架构与组件细节见本文档上文各节。

### 关键决策（摘要）

| 决策 | 选择 | 理由 |
|------|------|------|
| tick 源 | LAPIC 周期模式接管，PIT 掩蔽 + 未校准回退 | LVT 本地投递免疫 QEMU IOAPIC edge 伪影；PIT 先跑保证 boot 窗口有 tick |
| TSC 频率校准 | CPUID15h + RTC PIE 联合校准（RTC PIE 结果优先复用） | 250ms PIE 窗口比 TSC 窗口更稳；AP 复用 BSP 校准状态 |
| mult/shift | `__uint128_t` 中间值 + shift 上限放宽 | 高频 TSC（≥4.3GHz）需 shift≥35，64 位中间值会溢出（commit 496a210） |
| 时间语义 | 精粒度路径迁纳秒；调度器/watchdog/lwIP 粗超时保持 jiffies | 亚 tick 分辨率只对用户可感知路径有价值；避免重写已验证的 deadline 算术 |
| jiffies | 仍隐含 10ms/tick，修好 tick 源后自动恢复正确速率 | EEVDF 是 tick 粒度语义，不迁纳秒 |
| aarch64 | clocksource/clockevent 接口 hook 预留（cntvct_el0 + CNTP），不写 ARM 代码 | YAGNI，避免未验证代码 |

### 实现期发现

- **per-LAPIC DIV 必须写**：QEMU 双核实测 298Hz 定位——AP 复位后 DIV 为 ÷1，静态值 ÷2 折算导致 AP 200Hz。`lapic_timer_start` 必须在每个 CPU 写 `LAPIC_TIMER_DIV`（spec v8 门① 实证）。

### Commit 列表

| Commit | 内容 |
|--------|------|
| `dc25e97`~`41b3541` | spec/plan 迭代 v1→v8 + Hermes 外部评审归档（9 个 docs commit） |
| `de052f2` | clocksource + clockevent 双层抽象（TSC/cntvct_el0，接口层） |
| `4cc5779` | TSC 频率校准（CPUID15h + RTC PIE 联合校准，含超时） |
| `67f99e2` | BSP 切 LAPIC 周期 tick，先掩 PIT 再接管 + 握手采样 |
| `86f4ceb` | CLOCK_MONOTONIC/REALTIME + nanosleep + poll 迁纳秒 |
| `7a06a6e` | 内核 jiffies 频率 self-test + 验证门证据 + 回归 |
| `14db988` | 最终评审修复 — RTC PIE 窗口对齐/mult-shift 上界/aarch64 stub/CPUID 守卫/删死代码/IRQ-safe 锁 |
| `496a210` | compute_mult_shift 高频 TSC 溢出 — __uint128_t 中间值 |

### 验证

- systest **150/150**（含 jiffies 频率 self-test：QEMU 实测 tick 速率与期望值匹配）
- 回归：select/poll 超时、EEVDF 时间片、lwIP 超时、busybox `sleep 1` ≈ 1s（恢复真 10ms/tick）
