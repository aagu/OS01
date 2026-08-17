# Timer 重构：clocksource + clockevent 双层抽象 设计文档

- 日期：2026-08-17
- 状态：待用户 review（尚未进入实现）
- 前置：`docs/pit-200hz-handoff.md`（HANDOFF，2026-08-17）
- 目标：修复 QEMU TCG 下 PIT 200Hz 伪影，使 tick 与时间在 QEMU 与真实硬件都正确，并为未来 aarch64 移植预留接口
- 修订记录：v1（初版）→ v2（并入用户 review：boot 窗口 tick、IRQ8 时序、sys_now 语义、mult/shift 公式、CLOCK_REALTIME、跨核 TSC、handler 签名契约、内核 self-test）

---

## 1. 背景与问题

OS01 的时间系统当前以单一 `jiffies` 计数器为唯一时间基，只在 PIT 的
`pit_handler`（IRQ0 → BSP）里 `jiffies++`，并假设 1 jiffy = 10 ms。所有时间
消费者都从它派生：

- `clock_gettime(CLOCK_MONOTONIC)` = `jiffies * 10`（硬编码 ms）
- `nanosleep` / `poll` / `select` 的唤醒 deadline = `jiffies + ticks`
- EEVDF 调度器时间片（`update_curr` 每 tick `vruntime += 1`，
  `EEVDF_MIN_SLICE = 10 tick = 100ms`）
- lwIP `sys_now()` = `jiffies * 10`（`sys_arch.c:500`）及其粗超时
- AHCI 初始化 busy-wait 超时（`WAIT_WHILE`，`ahci.c:29,69-74,325`）
- watchdog、kernel timer 轮、`LWIP_RAND()` 种子（`cc.h:80`）

HANDOFF 已定位根因：**QEMU TCG 的 IOAPIC 边沿投递没有上升沿检测**，PIT
mode-3 方波在每个 10ms 周期内两次驱动 `level=1`，导致 guest 收到 200Hz 的
IRQ0。于是 1 jiffy 实际只有 5ms，所有基于 jiffies 的时间/超时在 QEMU 下 2×
加速。**这不是 OS01 的 bug**（guest 侧 1:1 处理，已测量）；真实硬件上 PIT 是
真 100Hz。修复方向：把 OS tick 源切到 LAPIC（LVT 本地投递，天然免疫该伪影），
并把时间基从 tick 里解耦出来。

## 2. 目标与非目标

### 目标

1. tick 源在 QEMU TCG 与真实硬件都稳定 100Hz（LAPIC 周期模式，PIT 先跑后由
   LAPIC 接管，保留 fallback）。
2. 引入 clocksource / clockevent 双层抽象：**精粒度时间（clock_gettime /
   nanosleep / poll / select deadline）与 tick 解耦**。
3. `CLOCK_MONOTONIC`、`CLOCK_REALTIME`、`nanosleep`、`poll`、`select` 的 deadline
   全部迁到 clocksource 纳秒时间戳。
4. 抽象接口不硬编码 x86，为 aarch64（`cntvct_el0` + CNTP 定时器）预留接入点。

### 非目标（YAGNI）

- one-shot clockevent / hrtimer 框架（方案 C，后续增强）
- HPET 作 clocksource（方案 B，后续可选；x86 跨平台最稳，但破坏单一
  `arch_cycle_counter` 抽象且新增 MMIO 驱动）
- 跨核 TSC 点对点同步循环（只做每 CPU 偏移粗修正，见 §8.3）
- aarch64 的 CNTP 定时器实现代码（只留接口 hook，不写 ARM 代码）

### jiffies 语义（精确定义，避免「不再当时间用」的歧义）

`jiffies` **仍然隐含 10ms/tick 的时间语义**，精度就是 tick 粒度。它继续服务于：

- 调度器（EEVDF vruntime/deadline 以 tick 计）——这是 tick 语义，不是时间
- watchdog、kernel timer 轮（`kernel/timer/timer.c`）
- lwIP 粗超时（`sys_arch.c`，粒度 100ms+ 足够）
- AHCI 初始化 busy-wait 超时
- `LWIP_RAND()` 种子、`hang.c` 调试时间戳、`test_kthread_self_reap.c` 的
  tick 预算

**迁到 clocksource 纳秒的只有精粒度路径**：`clock_gettime`（MONOTONIC 与
REALTIME）、`nanosleep`、`poll`、`select` 的 deadline。这些路径用户可感知精度，
值得亚 tick 分辨率。lwIP/AHCI/watchdog 的粗超时保持 jiffies 不变——修好 tick 源
后它们自动变回正确速率（10ms/tick），无需迁移，也避免把网络栈那套已被验证稳定
的 deadline 算术重写一遍引入风险。

## 3. 架构分层

```
┌─────────────────────────────────────────────────────┐
│  精粒度消费者：clock_gettime / nanosleep / poll/select│ ← 用纳秒时间戳
├─────────────────────────────────────────────────────┤
│  clocksource 层（kernel/time/clocksource.c，arch无关）│ ← 单调纳秒
│     clocksource_read_ns() = (cycle+offset)*mult>>shift
├─────────────────────────────────────────────────────┤
│  clockevent 层（kernel/time/tick.c，arch无关）        │ ← tick 语义
│     tick_handler(): jiffies++ / nsec扫描 / need_resched
├─────────────────────────────────────────────────────┤
│  arch hook（kernel/include/kernel/arch/*.h）          │
│     arch_cycle_counter()  [已有]  arch_cycle_freq()  [新增]
│     arch_tick_start() [新增]
└─────────────────────────────────────────────────────┘
粗粒度消费者：EEVDF、watchdog、kernel timer 轮、lwIP 粗超时、AHCI → 用 jiffies
（10ms/tick，不变）
```

关键点：精粒度时间消费者改挂 clocksource 纳秒；粗粒度消费者继续挂 jiffies。tick
源修好后 jiffies 变回真 10ms，两类消费者都正确。

## 4. 接口定义

### 4.1 arch 无关核心

```c
// kernel/include/kernel/clocksource.h

// 时钟源换算：ns = (cycle + 每CPU偏移) * mult >> shift。
// 静态 inline（热路径，syscall 每次调用）。mult/shift 由 clocksource_init()
// 依据 arch_cycle_freq() 计算（公式见 §5）。
void        clocksource_init(void);
static inline uint64_t clocksource_read_ns(void);   // 单调纳秒
uint64_t    clocksource_cycles(void);               // 原始 cycle（调试/校准用）

// kernel/include/kernel/clockevent.h
void        tick_init(void);      // 静态注册：装 IDT gate / 注册软中断 / 挂 tick_handler
void        tick_start(void);     // 显式启动：percpu+GS 就绪后调用，选 tick 源并启动
void        tick_handler(void);   // 统一 tick 语义（arch IRQ handler 调用，无参）
uint64_t    tick_get_jiffies(void);
```

**`tick_init` vs `tick_start` 职责**：`tick_init`（phase 4）只做静态注册
（gate、软中断、timer 轮），**不启动任何 tick 源**；`tick_start` 在
`percpu_init` + `percpu_install_gs(0)` 之后显式调用，选择并启动 tick 源
（LAPIC → PIT fallback）。原因见 §7：tick 源的启动依赖 GS base（`this_cpu()`）
且必须在 boot 期 PIT 跑完之后才可接管。

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

### 4.3 handler 签名契约（写死）

- `tick_handler()` **无参**，只做 tick 语义（jiffies++ → poll 纳秒扫描 →
  need_resched → watchdog → TIMER_SIRQ）。
- **EOI 由各 arch IRQ handler 负责**，不在 `tick_handler()` 里：
  - PIT 路径：`do_IRQ` → `irq_desc->ack`（`pic_ack` 或 IOAPIC EOI）
  - LAPIC 路径：`lapic_timer_handler` 调 `tick_handler()` 后 `lapic_eoi()`
- 现有 handler 签名不一致（`pit_handler(nr, param, regs)` vs
  `lapic_timer_handler(regs, ec)`）是历史遗留，各自保留，只在内部**统一调
  `tick_handler()` 做共享语义**，EOI 各自处理。

## 5. TSC 频率校准（x86，真实硬件的关键）

三级优先级：

1. **CPUID leaf 15h**（首选）：`ECX` = core crystal Hz（非 0 时有效），
   `TSC_Hz = ECX × EBX / EAX`。真硬件普遍提供，零延时。
2. **RTC 周期中断校准**（回落；QEMU TCG 走这条，因 TCG 返回 0）：配置 RTC 的
   PIE（寄存器 0x0B bit6 使能，0x0A 低 4 位设 1024Hz），测固定 PIE 周期的 TSC
   差值。HANDOFF 的 TSCCAL probe 即此法，实测 0.006% 误差。CMOS 访问需屏蔽
   NMI（0x70 bit7；现有 `get_rtc_register` 已带 `0x80|nr`，可直接复用）。
3. **ARM**：`arch_cycle_freq()` 直接读 `CNTFRQ_EL0`，无需校准。

**RTC PIE 校准是自包含的**：`arch_cycle_freq()` 内部临时 `register_irq(8)` →
使能 PIE → busy-wait 若干 IRQ8 tick → 禁 PIE → `unregister_irq(8)`。不依赖单独
的 RTC subsys，也不留常驻 IRQ8 handler（校准完成后释放）。**前提**：phase 4 时
中断必须已开启（见 §7 的 IF 前置条件），且 IRQ8 走 IOAPIC 路由（GSI 8）能正常
投递。

需新增一小段 RTC 周期中断代码（现有 `rtc.c` 只有 wall-clock 读秒）。这是
「真实硬件要能工作」的代价——QEMU TCG 没有 CPUID 15h，只有 RTC 校准能拿到
TSC 频率。

### 5.1 mult/shift 换算公式（实现者需要的公式，而非承诺）

把 cycle 转 ns：`ns = (cycle * mult) >> shift`，其中

```
mult = (10^9 << shift) / freq_hz
```

`shift` 取使 `mult` 落在 `[2^31, 2^32)` 的最大值（保证 mult 不溢出 32 位，且
`__uint128_t` 中间乘不溢出）。算法：从 `shift=32` 起，若 `mult >= 2^32` 则递减
`shift` 直到 `< 2^32`。

以实测 TSC 2.994 GHz 为例：`shift=32`，`mult = (1e9·2^32)/2.994e9 ≈ 1.434e9`，
落在 `[2^31, 2^32)`。舍入误差 = 丢掉的小数部分 / mult ≈ 0.5/1.434e9 ≈
**3.5e-10 ≈ 0.00035 ppm**。

### 5.2 误差的两种来源（区分，勿混）

- **换算舍入误差**：上述 mult/shift 量化，**< 1ppm**（实际远小于）。
- **校准误差**：RTC PIE 校准的测量误差，**~60ppm**（TSCCAL 实测 0.006%）。

验证门里的「±1%」是给**校准误差**留余量，不是换算误差。换算误差小到可忽略。

## 6. 迁移明细（jiffies → 纳秒）

| 位置 | 现状 | 迁移后 |
|---|---|---|
| `trap.c` `SYS_clock_gettime` | `ms = jiffies * 10`（REALTIME==MONOTONIC） | `ns = clocksource_read_ns()`（两钟同迁，见下） |
| `trap.c` `SYS_nanosleep` | `target = jiffies + ticks`(10ms) | `target_ns = now_ns + ns` |
| `task.h` `wakeup_jiffies` | 存 jiffies | 改名 `wakeup_ns`，存纳秒 |
| `trap.c` `nanosleep_should_unblock` | `jiffies >= wakeup_jiffies` | `clocksource_read_ns() >= wakeup_ns` |
| `poll.c` `do_poll_core` | `deadline = jiffies + (ms+9)/10` | `deadline = now_ns + ms*1e6` |
| `poll.h` `poll_timeout_node_t.deadline` | jiffies | 纳秒（字段类型 uint64_t 不变） |
| `pit.c` `pit_handler` 的 poll 扫描 | `jiffies >= n->deadline` | 迁到 `tick_handler()`，用 `clocksource_read_ns()` |

**CLOCK_REALTIME**：现在 `trap.c:1878` 明确 REALTIME == MONOTONIC（都返回
jiffies×10ms）。迁移后**两者同迁到 `clocksource_read_ns()`**，保持相等。不能
一个迁一个留，否则两钟分裂。（真正 wall-clock 的 RTC 校准是未来独立任务，
本次 REALTIME 继续等于 MONOTONIC，只是精度升到纳秒。）

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

### 7.1 核心原则：PIT 先跑，LAPIC 就绪后接管

**PIT 必须在 phase 4 照常启动（保持现状）**，因为 phase 6 的 `ahci_init`
（`WAIT_WHILE` busy-wait 依赖 `jiffies` 前进，`ahci.c:325`）以及后续
`gpt_scan`/`fat32_init`/`ext2_init` 都在 `subsys_init_all()` 里执行，此时
`percpu_init`/`percpu_install_gs` 还没跑（`main.c:258-289`）。若 PIT 在 phase 4
掩蔽，phase 6 期间 `jiffies` 冻结为 0 → 超时永不触发，真实硬件/慢设备上会永久
挂起。**这是对现状（phase 4 PIT 启动 → phase 6 有 tick）的直接回归，必须避免。**

### 7.2 迁移后的顺序

**现在**（phase 4）：`timer` → `pit`（启动 100Hz）→ `lapic-timer`（校准+注册，
不启动）。

**迁移后**：

```
phase 3:  apic, pic                          ← 不变
          （pic_init 末尾 sti → IF=1，见 7.3）
phase 4:  timer        (jiffies=0 + softirq注册)      ← 不变
          clocksource  (mult/shift ← arch_cycle_freq，内部自含 RTC PIE) ← 新增
          pit          (照常启动 100Hz，作 boot 期 tick)  ← 不变
          lapic-timer  (TSC 校准 + 注册 IDT gate，不启动) ← 改校准
phase 6:  ahci ...                              ← 不变，仍由 PIT 驱动 jiffies
```

`main.c` 里，在 `percpu_init` 循环（`main.c:258-289`，`percpu_install_gs(0)` 在
`main.c:276`）之后、`smp_boot_aps()`（`main.c:291`）之前，插入：

```
显式启动: tick_start()  (GS base 已装，this_cpu() 可用)
          → arch_tick_start()：BSP 启动 LAPIC 周期 100Hz
          → 成功后掩蔽 PIT（IRQ0 disable，handler 保留作 fallback）
          → 失败(lapic_timer_hz==0) 则 PIT 保持运行（硬件 100Hz，准的）
```

**掩蔽 PIT 是必要的**：若 LAPIC 与 PIT 同时活跃，两者都调 `tick_handler()` →
`jiffies` 双计。掩蔽在**控制器层**（IOAPIC mask GSI0 / PIC `pic_disable(0)`），
不是 handler 内忽略，这样 fallback 重新使能只是一次 `enable`。

### 7.3 IF 前置条件（写明）

phase 4 执行 `clocksource_init` 的 RTC PIE 校准时，中断必须已开启。事实链：
`pic_init` 在 phase 3 末尾 `arch_local_irq_enable()`（`8259A.c:29`），且 `pic`
虽标 `SUBSYS_FLAG_OPTIONAL` 但**无条件注册**（`subsys.c:77`，OPTIONAL 只是
「失败不致命」，不是「跳过」），所以无论 PIC 还是 IOAPIC 模式，phase 4 开始时
IF=1。IOAPIC 模式下 8259 被 `pic_init` 掩蔽（`OCW1=0xff`），但 IRQ8 经 IOAPIC
路由不受 PIC 掩码影响。此前提成立，文档显式记录以消除实现者疑虑。

**BSP 的 LAPIC handler 改动**：现在 `lapic_timer_handler` 里 `if (cpu_id() != 0)`
只给 AP 用；改成 BSP 走 `tick_handler()`（驱动 jiffies），AP 只做
`need_resched + watchdog`。`lapic_timer_start(100)` 现在 BSP 和 AP 都调（BSP 由
`tick_start` 调，AP 由 `smp.c:121` 现有逻辑调）。

## 8. 错误处理

### 8.1 失败矩阵（tick 源 与 时间基 独立回退）

```
                  ┌── CPUID 15h 有效 ──→ 用它
TSC 频率 ────────┤
（clocksource）   ├── CPUID 15h=0 ────→ RTC 周期中断校准（IRQ8, 1024Hz，自含）
                  └── RTC 也失败 ─────→ clocksource 退化为 jiffies×10ms
                                        （= 现状，时间基不再纳秒）

tick 源 ────────── PIT 先跑（boot 期，phase 4→6 驱动 jiffies）
（clockevent）     └── tick_start() 启动 LAPIC 成功 → 掩蔽 PIT，LAPIC 主 tick
                      启动失败(lapic_timer_hz==0) → PIT 保持运行（硬件 100Hz）
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
   时保持 PIT 运行（不掩蔽）。除数 11931 不动。
4. 64 位 wrap：`rdtsc`/`cntvct_el0` 64 位，3GHz 下 ~195 年才 wrap，忽略；但所有
   deadline 比较用环绕安全式 `(int64_t)(now_ns - deadline_ns) >= 0`。
5. mult/shift 精度：换算舍入误差 < 1ppm（§5.1）；校准误差 ~60ppm（§5.2）。
6. RTC CMOS 访问：屏蔽 NMI（0x70 bit7）、检测 `is_updating_rtc()`（已有）再读。

### 8.3 跨核 TSC 时间线（每 CPU 偏移修正）

`clock_gettime` 可在任意 CPU 执行（用户进程被 load-balance 过去），`nanosleep`
deadline 在 AP 算 `now_ns`，而 poll 扫描在 BSP 的 `tick_handler`、nanosleep 唤醒
在每 CPU 的 `sched_unblock_blocked` 做比较。两条 `rdtsc` 时间线若跨核偏差，会
产生早期/迟到唤醒。QEMU TCG 所有 vCPU 共享 TSC 故测不出；真实硬件会踩。

对策（近零成本，复用现有 `smp.c:245-253` 的 warp 检测数据）：

- `percpu_t` 增 `int64_t tsc_offset`（BSP=0）。
- AP 启动时（`smp.c` 现有 warp 检测处）存 `tsc_offset[i] = bsp_tsc - ap_tsc`。
- `clocksource_read_ns()` 用 `arch_cycle_counter() + (uint64_t)this_cpu()->tsc_offset`
  作为逻辑 cycle（`this_cpu()` 需 GS，而 `clocksource_read_ns()` 只经 syscall
  路径，运行时 GS 已装）。

**限制**：这是粗偏移修正（boot 单点采样，非点对点同步循环），足以消除 gross
skew。完整点对点同步循环列为后续增强，不在本次范围。

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
10. **boot 到 AHCI/FS 挂载正常**（确认 phase 4→6 期间 PIT 驱动 jiffies，
    `ahci_init`/`gpt_scan`/`fat32_init`/`ext2_init` 不挂起）——真实设备慢速路径
    用 QEMU `-device ahci` + 人为延迟或直接 code review 佐证

### ④ fallback 路径（一次性验证，非每次）

11. 人为 `lapic_timer_hz=0` → 回退 PIT 仍正常运行（tick 走 PIT，时间走 TSC）
12. 人为禁用 RTC 校准 → 时间基退 jiffies×10ms，系统可跑
13. 人为制造跨核 TSC 偏移（QEMU 不支持，靠 code review + 单测断言 `tsc_offset`
    计算）→ `clocksource_read_ns()` 在 AP 上仍单调

## 10. 文件改动清单

### 新增（arch 无关核心 + 内核 self-test）

| 文件 | 内容 |
|---|---|
| `kernel/time/clocksource.c` | `clocksource_init()` / `read_ns()` / `cycles()` / mult/shift 计算 |
| `kernel/time/tick.c` | `tick_init()` / `tick_start()` / `tick_handler()` |
| `kernel/include/kernel/clocksource.h` | 接口声明（含 static inline `read_ns`） |
| `kernel/include/kernel/clockevent.h` | 接口声明 |
| `kernel/test/test_timer.c` | 内核 self-test：测 jiffies ~100Hz（TSC 窗口内采样） |

### 修改

| 文件 | 改动 |
|---|---|
| `kernel/apic/lapic_timer.c` | 校准改 TSC 基准；handler BSP→tick_handler / AP→need_resched；start 返回 bool |
| `kernel/driver/pit.c` | handler 改调 tick_handler()；pit_enable()/pit_disable() 供接管掩蔽 |
| `kernel/driver/rtc.c` + `rtc.h` | 新增 RTC PIE 自含校准（IRQ8 临时注册 + PIE + TSC 采样） |
| `kernel/arch/x86_64/subsys.c` | 初始化顺序重排：加 clocksource；保留 pit 启动；lapic-timer 改 TSC 校准 |
| `kernel/arch/x86_64/trap.c` | clock_gettime（含 REALTIME）/ nanosleep / nanosleep_should_unblock 迁纳秒 |
| `kernel/include/kernel/task.h` | `wakeup_jiffies` → `wakeup_ns` |
| `kernel/fs/poll.c` + `kernel/include/kernel/poll.h` | deadline 语义迁纳秒（字段类型不变） |
| `kernel/include/kernel/arch/cpu.h` | 加 `arch_cycle_freq()` 声明 |
| `kernel/arch/x86_64/cpu.h` | `arch_cycle_freq()` x86 实现（CPUID15h→RTC→0） |
| `kernel/arch/aarch64/cpu.h` 内联 | `arch_cycle_freq()` 读 CNTFRQ_EL0（仅接口） |
| `kernel/arch/x86_64/smp.c` | AP 的 lapic_timer_start 适配 bool；存 `tsc_offset` |
| `kernel/include/kernel/percpu.h` | `percpu_t` 增 `tsc_offset` |
| `kernel/kernel/main.c` | percpu 循环后插入 `tick_start()` |

## 11. 测试与提交计划

### 提交序列（小步、语义化前缀，每步可独立验证）

1. `feat(time): clocksource + clockevent 双层抽象（TSC/cntvct_el0，接口层）`
   — 纯新增，编译过，无行为变化
2. `feat(time): TSC 频率校准（CPUID15h + RTC PIE fallback）`
   — 可独立验证 `arch_cycle_freq()` 正确
3. `feat(tick): BSP 切 LAPIC 周期 tick，PIT 接管后掩蔽`
   — **验证门 ①② 在此步过**（含 boot 到 AHCI/FS 门 ③.10）
4. `feat(time): CLOCK_MONOTONIC/REALTIME + nanosleep + poll 迁纳秒`
   — **验证门 ③ 在此步过**
5. `test(time): 内核 jiffies 频率 self-test + 验证门证据 + 回归修复`
   — 全测试套件 + fallback 路径（门 ④）

### 每步 RED→GREEN

- RED：先跑现有 systest 确认基线绿（记录），再写/改对应测试
- GREEN：实现后该步验证门必须过
- 全回归：提交前跑满 systest（70/70）+ 双核 + lwIP DHCP

### 测试载体

- **内核 self-test**（`kernel/test/test_timer.c`，`OS01_SELFTEST`）：jiffies 不暴露
  给用户态，只能在内核态验证 ~100Hz。方法：TSC 采样一个 ~500ms 窗口，断言
  `Δjiffies ≈ 50`（±10%）。跑在 `main.c:299` 的 `selftest_run_all()`，此时
  tick 已启动。
- **用户态 systest**（`user/systest.c`）：`test_clock_gettime` / `test_nanosleep` /
  `test_select` 扩展断言（亚 tick 分辨率、nanosleep 精确度）。

### 回退

任何一步回归失败 → 停下定位，不跨步推进；debug 探针只读、提交前移除。
