# Timer 重构：clocksource + clockevent 双层抽象 设计文档

- 日期：2026-08-17
- 状态：待用户 review（尚未进入实现）
- 前置：`docs/pit-200hz-handoff.md`（HANDOFF，2026-08-17）
- 目标：修复 QEMU TCG 下 PIT 200Hz 伪影，使 tick 与时间在 QEMU 与真实硬件都正确，并为未来 aarch64 移植预留接口
- 修订记录：
  - v1：初版
  - v2：并入首轮 review（boot 窗口 tick、IRQ8 时序、sys_now 语义、mult/shift、CLOCK_REALTIME、跨核 TSC、handler 契约、内核 self-test）
  - v3：并入次轮 review（正交性=联合校准、tsc_offset 握手采样、unregister 索引坑、PIE 超时、mult/shift 算术、BSP 二次启动、irq_mask API、nanosleep 迁移补全）

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
并把精粒度时间从 tick 里解耦出来。

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
- 完整点对点跨核 TSC 同步循环（只做握手采样粗偏移修正，见 §8.3）
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
// 静态 inline（热路径，syscall / tick_handler / sched_unblock 都调）。
// mult/shift 由 clocksource_init() 依据 arch_cycle_freq() 计算（公式见 §5.1）。
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

### 4.4 新增 IRQ mask-only 原语（掩蔽但不注销）

框架现有 `register_irq` / `unregister_irq`：`unregister_irq` 会**同时** disable
并清空 handler（`irq.c:52-62`），无法表达「掩蔽但保留 handler 作 fallback」。
故新增：

```c
// kernel/include/kernel/interrupt.h
void irq_mask(uint32_t gsi);     // 只 disable（controller->disable），保留 handler
void irq_unmask(uint32_t gsi);   // 只 enable（controller->enable），handler 仍在
```

实现走 `controller->disable/enable`（`pic_disable/enable` 或 IOAPIC mask/unmask），
**不触碰 `irq_desc` 的 handler 字段**。PIT 的接管掩蔽/回退使能用它，而非
`unregister_irq`。

## 5. TSC 频率校准（x86，真实硬件的关键）

三级优先级：

1. **CPUID leaf 15h**（首选）：`ECX` = core crystal Hz（非 0 时有效），
   `TSC_Hz = ECX × EBX / EAX`。真硬件普遍提供，零延时，误差 ppm 级。
2. **RTC PIE 联合校准**（回落；QEMU TCG 走这条，因 TCG 返回 0）：配置 RTC 的
   PIE（寄存器 0x0B bit6 使能，0x0A 低 4 位设 1024Hz），**一次 PIE 窗口同时测
   TSC 和 LAPIC 两个计数器**（见 §5.1）。这是「正交性」的实现关键——见 §8.1。
3. **ARM**：`arch_cycle_freq()` 直接读 `CNTFRQ_EL0`，无需校准。

### 5.1 RTC PIE 联合校准（消除「LAPIC 硬依赖 TSC_HZ」的假正交）

`lapic_timer_calibrate` 若走 TSC 窗口法，就需要 `TSC_HZ`；若 TSC 只能靠 RTC PIE
得到，则 LAPIC 校准也被 RTC PIE 串起来。为避免「TSC 未知 → LAPIC 无法校准」的
耦合，用**同一段 PIE 窗口测两个计数器**：

```c
// 校准原语：一次 RTC PIE 窗口，同时测 TSC 和 LAPIC 两个计数器。
// 返回 0 成功 / -1 失败（超时或 IRQ8 不到）。带 TSC 硬超时，绝不无限自旋。
static int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out)
{
    // 1. LAPIC 装载 0xFFFFFFFF 并启动（countdown 开始跑，无需 IDT gate）
    // 2. 临时 register_irq(0x20 + 8, ...)   ← 注意 gsi=8，注销见下
    //    使能 PIE（1024Hz，周期 ~976.5625µs）
    // 3. 记录 tsc0；等 N=256 个 PIE tick，同时用 TSC 硬超时 ~300ms 兜底
    //    （300ms 内没来够 N tick → 放弃，返回 -1）
    // 4. 记录 tsc1；读 LAPIC CUR，算 elapsed_lapic = 0xFFFFFFFF - cur
    // 5. 禁 PIE；unregister_irq(0x20 + 8)
    //    ⚠️ 必须传 vector 0x28 而非 gsi 8 —— unregister_irq 用 nr-32 索引
    //       irq_table（irq.c:52），传 8 会下溢成 irq_table[-24] 野内存写。
    // 6. *tsc_hz_out   = (tsc1 - tsc0)   * 1024 / N
    //    *lapic_hz_out = elapsed_lapic   * 1024 / N
}
```

**采样窗口规格与精度**：PIE 1024Hz，N=256 tick ≈ 250ms。粒度误差 = 1/N ≈ 0.4%
（4000ppm），满足验证门 ±1%（10000ppm）要求。N 越大越准但启动越慢；250ms 是
精度/延迟的合理折中。（HANDOFF 的 TSCCAL 0.006%=60ppm 是离线精细测量，不是
启动校准目标——启动校准 0.4% 已够。）

**IRQ8 索引不一致的历史坑（必须写进实现注释）**：`register_irq` 用 `gsi` 索引
`irq_table[gsi]`（`irq.c:34`），`unregister_irq` 用 `nr-32` 索引
`irq_table[nr-32]`（`irq.c:52`）。注册 gsi=8（vector 0x28）后，注销必须
`unregister_irq(0x28)`，即 `0x20 + 8`。写 `unregister_irq(8)` → `irq_table[-24]`
→ 野内存写（`irq.c:58-62` 清任意地址）。

**超时兜底**：等 N tick 的循环同时用 TSC 硬超时（~300ms）。若 IRQ8 因任何原因
不到（IOAPIC 路由失败、QEMU 变体），300ms 后放弃，`tsc_hz=0` → 落 jiffies 兜底，
**绝不无限自旋挂 boot**。

**校准职责划分（复用联合结果）**：

```c
clocksource_init():
    tsc_hz = cpuid_15h()
    if (tsc_hz == 0) {
        uint64_t lapic_hz;                       // 顺带测，暂存复用
        if (rtc_pie_calibrate(&tsc_hz, &lapic_hz) != 0)
            tsc_hz = 0;                          // clocksource 退 jiffies
        else
            g_lapic_premeasured_hz = lapic_hz;
    }

lapic_timer_calibrate():
    if (tsc_hz != 0)
        lapic_hz = measure_against_tsc(tsc_hz)   // 首选：快 ~10ms，覆盖暂存值
    else if (g_lapic_premeasured_hz != 0)
        lapic_hz = g_lapic_premeasured_hz;       // 复用 RTC PIE 联合结果
    else
        lapic_hz = 0;                            // 退 PIT
```

这样 LAPIC 在 TSC_HZ 未知时复用 RTC PIE 联合结果，**真正正交**：clocksource 与
clockevent 各自独立可测，启动延迟只一次 PIE 窗口。

### 5.2 mult/shift 换算公式（实现者需要的公式）

把 cycle 转 ns：`ns = (cycle * mult) >> shift`，其中

```
mult = (10^9 << shift) / freq_hz
```

**`shift` 取使 `mult` 落在 `[2^31, 2^32)` 的最大值**（mult 不溢出 32 位，且
`__uint128_t` 中间乘不溢出）。算法：从 `shift` 高位（如 32）**递增**，直到
`mult < 2^32`（即 `shift` 递减过程里第一个满足 `<2^32` 的、且 `mult >= 2^31`
的最大 `shift`）。

实例（实测 TSC 2.994GHz）：

- `shift=32` → `mult = (1e9·2^32)/2.994e9 ≈ 1.434e9`，**< 2^31=2.147e9**，不在
  下界内 → 不采用。
- `shift=33` → `mult = (1e9·2^33)/2.994e9 ≈ 2.869e9`，`∈ [2.147e9, 4.295e9)` ✓。
  **正确值 shift=33**。换算舍入误差 ≈ 0.5/2.869e9 ≈ 1.7e-10 ≈ 0.00017ppm。

（验证 aarch64 24MHz：`shift=26` → `mult=(1e9·2^26)/2.4e7≈2.796e9` ∈ 范围，约束
本身可达。）

### 5.3 误差的两种来源（区分，勿混）

- **换算舍入误差**：上述 mult/shift 量化，**< 1ppm**（实际 ~0.0002ppm）。
- **校准误差**：RTC PIE 启动校准的测量误差，**~0.4%**（N=256）；CPUID 15h 则
  ppm 级。

验证门里的「±1%」是给**启动校准误差**（RTC PIE ~0.4%）留余量，不是换算误差。
换算误差小到可忽略。

## 6. 迁移明细（jiffies → 纳秒）

| 位置 | 现状 | 迁移后 |
|---|---|---|
| `trap.c` `SYS_clock_gettime` | `ms = jiffies * 10`（REALTIME==MONOTONIC） | `ns = clocksource_read_ns()`（两钟同迁） |
| `trap.c` `SYS_nanosleep` 整体 | `target = jiffies + ticks`(10ms) | `target_ns = now_ns + ns`（**含 do/while 循环与 EINTR rem 计算，见下**） |
| `task.h` `wakeup_jiffies` | 存 jiffies | 改名 `wakeup_ns`，存纳秒 |
| `trap.c` `nanosleep_should_unblock` | `jiffies >= wakeup_jiffies` | `clocksource_read_ns() >= wakeup_ns` |
| `trap.c` `SYS_nanosleep` do/while 条件 | `while (r==0 && jiffies < target)`（trap.c:1918） | `while (r==0 && clocksource_read_ns() < target_ns)` |
| `trap.c` `SYS_nanosleep` EINTR rem | `remain = target - jiffies`（trap.c:1924-1929） | `remain_ns = target_ns - clocksource_read_ns()` |
| `poll.c` `do_poll_core` | `deadline = jiffies + (ms+9)/10` | `deadline = now_ns + ms*1e6` |
| `poll.h` `poll_timeout_node_t.deadline` | jiffies | 纳秒（字段类型 uint64_t 不变） |
| `select.c` `do_select`/`do_pselect6` | 走 `do_poll_core`（select.c:32,54） | **随 poll 一起迁**，无需单独改 |
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
          lapic-timer  (校准 + 注册 IDT gate，不启动)     ← 改校准（§5.1）
phase 6:  ahci ...                              ← 不变，仍由 PIT 驱动 jiffies
```

`main.c` 里，在 `percpu_init` 循环（`main.c:258-289`，`percpu_install_gs(0)` 在
`main.c:276`）之后、`smp_boot_aps()`（`main.c:291`）之前，插入：

```
显式启动: tick_start()  (GS base 已装，this_cpu() 可用)
          → 先 irq_mask(0)（掩蔽 PIT IRQ0，防止交接窗口双计）
          → arch_tick_start()：BSP 启动 LAPIC 周期 100Hz
          → 成功 → PIT 保持掩蔽，LAPIC 成为主 tick
          → 失败(lapic_timer_hz==0) → irq_unmask(0)，PIT 继续跑（硬件 100Hz）
```

**掩蔽先于启动**：必须先 `irq_mask(0)` 再启 LAPIC，否则 LAPIC 启动后与 PIT 短暂
同时活跃，两者都调 `tick_handler()` → `jiffies` 双计。掩蔽在控制器层
（`controller->disable`），handler 保留，回退只需 `irq_unmask(0)`。

**BSP 二次启动（幂等，需说明）**：`tick_start()` 对 BSP 启动一次 LAPIC；随后
`subsys_init_percpu()`（`main.c:295`）的 `"lapic-timer-start"`
（`subsys_percpu.c:9`）对 `cpu=0..num_cpus` **含 BSP** 再调一次
`lapic_timer_start(100)`（`subsys.c:88` 循环）。二次启动幂等（重写相同
initial_count），不崩，但 `lapic_timer_start` 的 bool 返回值适配必须**同时改
`subsys_percpu.c`**（不只 `smp.c`），且实现要确认重写 initial_count 无副作用。

### 7.3 IF 前置条件（写明）

phase 4 执行 `clocksource_init` 的 RTC PIE 校准时，中断必须已开启。事实链：
`pic_init` 在 phase 3 末尾 `arch_local_irq_enable()`（`8259A.c:29`），且 `pic`
虽标 `SUBSYS_FLAG_OPTIONAL` 但**无条件注册**（`subsys.c:77`，OPTIONAL 只是
「失败不致命」，不是「跳过」），所以无论 PIC 还是 IOAPIC 模式，phase 4 开始时
IF=1。IOAPIC 模式下 8259 被 `pic_init` 掩蔽（`OCW1=0xff`），但 IRQ8 经 IOAPIC
路由不受 PIC 掩码影响。此前提成立，文档显式记录以消除实现者疑虑。

### 7.4 boot 期 GS base=0 的既有窗口（pre-existing，非本次回归）

phase 4-6 期间 `pit_handler` 已在 `percpu_install_gs(0)`（`main.c:276`）之前调
`this_cpu()`（`pit.c:42`），而 GS base 直到 `main.c:276` 才装（`head.S` 只
`mov %ax,%gs`，base=0）。现状「没炸」只是 boot 快。本次引入
`clocksource_read_ns()`（内部读 `this_cpu()->tsc_offset`）会增加 GS 读取，这个
既有窗口值得**顺手记录**（spec 内记录即可），或加一条 boot 期 GS 早期安装（在
`arch_register_subsys` 前把 GS base 指向 `&percpu_data[0]`）。列为可选改进，不
阻断本次。

**BSP 的 LAPIC handler 改动**：现在 `lapic_timer_handler` 里 `if (cpu_id() != 0)`
只给 AP 用；改成 BSP 走 `tick_handler()`（驱动 jiffies），AP 只做
`need_resched + watchdog`。

## 8. 错误处理

### 8.1 失败矩阵（tick 源 与 时间基 独立回退，靠 RTC PIE 联合校准保正交）

```
TSC 频率（clocksource）:
  CPUID 15h 有效 ──→ 用它（ppm 级）
  CPUID 15h=0 ────→ RTC PIE 联合校准（§5.1，同时测 TSC + LAPIC）
  RTC PIE 也失败 ─→ tsc_hz=0 → clocksource 退 jiffies×10ms（= 现状）

tick 源（clockevent）:
  PIT 先跑（boot 期，phase 4→6 驱动 jiffies）
  tick_start() 启动 LAPIC：
    tsc_hz 已知 ──→ LAPIC 用 TSC 窗口校准 → 成功 → 掩 PIT，LAPIC 主 tick
    tsc_hz 未知 ──→ LAPIC 复用 RTC PIE 联合结果（§5.1 的 g_lapic_premeasured_hz）
                    → 成功 → 掩 PIT；失败 → PIT 保持运行
```

**正交性成立的关键**：LAPIC 校准**不硬依赖 TSC_HZ**——它要么用 TSC 窗口（TSC_HZ
已知时），要么复用 RTC PIE 联合校准的结果（TSC_HZ 只能靠 RTC PIE 时）。两个
计数器都能从同一个 RTC PIE 窗口独立测出频率。因此「TSC 频率拿不到」≠「LAPIC
起不来」——只要 RTC PIE 可用，两者都能校准；RTC PIE 也挂时，两者一起退（
TSC→jiffies，LAPIC→PIT），这才是真正的最坏情况，且 = 现状。

### 8.2 具体点

1. TSC 频率三级回落（§8.1），每级失败打印 `log_warn` 并落到下一级；最终兜底
   `clocksource_active = false`，`clocksource_read_ns()` 返回
   `jiffies * 10'000'000`。
2. invariant-TSC 检测：`CPUID 0x80000007 EDX bit8`；非 invariant 时打印警告
   （P-state 变频率影响速率）但继续用（现代 x86 普遍 invariant）。
3. LAPIC 启动失败：`lapic_timer_start()` 改返回 bool；`tick_start()` 收到 false
   时 `irq_unmask(0)` 保持 PIT。除数 11931 不动。
4. 64 位 wrap：`rdtsc`/`cntvct_el0` 64 位，3GHz 下 ~195 年才 wrap，忽略；但所有
   deadline 比较用环绕安全式 `(int64_t)(now_ns - deadline_ns) >= 0`。
5. mult/shift 精度：换算舍入误差 < 1ppm（§5.2）；启动校准误差 ~0.4%（§5.3）。
6. RTC CMOS 访问：屏蔽 NMI（0x70 bit7）、检测 `is_updating_rtc()`（已有）再读。
7. RTC PIE 校准超时：§5.1 的 TSC 硬超时 ~300ms，绝不自旋挂 boot。

### 8.3 跨核 TSC 时间线（握手采样粗偏移修正）

`clocksource_read_ns()` 可能在任意 CPU 的任意上下文执行：syscall
（`clock_gettime`/`nanosleep` 的 deadline 计算）、`tick_handler` 的 poll 扫描
（BSP）、每 CPU 的 `sched_unblock_blocked`（nanosleep 唤醒比较）。两条 `rdtsc`
时间线若跨核偏差，会产生早期/迟到唤醒。QEMU TCG 所有 vCPU 共享 TSC 故测不出；
真实硬件会踩。

**致命竞态（v2 的错）**：原 `smp.c` 顺序 `online=1`（:123）在 `tsc_boot=rdtsc()`
（:127）之前，BSP 等待循环（:236）见 online 即退，随后 `bsp_tsc=rdtsc()`（:246）
可能在 AP 写 `tsc_boot` 前采样到 memset 的 0 → offset ≈ 几十亿 cycle。**必须先
消除这个竞态，再谈采样精度。**

**修正（握手采样，`percpu_t` 增 3 字段）**：

```c
// percpu_t 新增：
//   uint64_t tsc_sync_go;      // BSP→AP：发起采样
//   uint64_t tsc_sampled;      // AP→BSP：采样完成
//   int64_t  tsc_offset;       // 结果：bsp_tsc - ap_tsc（BSP=0）

// AP ap_entry（改）：
lapic_timer_start(100);
__sync_synchronize();
cpu->online = 1;                     // 1. 先活（BSP 等待循环退出条件）
while (!cpu->tsc_sync_go)            // 2. 等 BSP 发起采样
    arch_cpu_pause();
cpu->tsc_boot = rdtsc();             // 3. AP 读 TSC（与 BSP 几乎同时）
__sync_synchronize();
cpu->tsc_sampled = 1;                // 4. 回写完成

// BSP smp_boot_aps（等 online 后，替换原 warp 检测块）：
percpu_data[i].tsc_sync_go = 1;      // 发起
bsp_tsc = rdtsc();                   // BSP 读 TSC（与 AP 间隔仅一次内存写传播延迟 ~100ns）
while (!percpu_data[i].tsc_sampled)  // 等 AP 完成
    arch_cpu_pause();
ap_tsc = percpu_data[i].tsc_boot;
percpu_data[i].tsc_offset = (int64_t)(bsp_tsc - ap_tsc);
```

`clocksource_read_ns()` 用 `arch_cycle_counter() + (uint64_t)this_cpu()->tsc_offset`
作逻辑 cycle。**`this_cpu()` 在运行时安全**：BSP 在 `main.c:276` 装 GS，AP 在
trampoline 设 `gs_base=tdata->gs_base`（`smp.c:212`），所有上述上下文运行时 GS
已装（`clocksource_read_ns()` 不会在 GS 装之前被调，boot 期校准用原始
`arch_cycle_counter()` 不走它）。

**限制**：这是握手采样的粗偏移修正，把流逝时间从「boot 累计」降到「~100ns」，
足以消除 gross skew。完整点对点同步循环（Linux `tsc_sync` 级）列为后续增强。

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

6. `clock_gettime` 连续两次读，**两次读之间插一个短忙等循环**（TCG 下紧挨的两次
   读可能落在同一虚拟时间片返回同值，忙等确保时间前进）→ 间隔 <10ms 也能反映
   **亚 tick 变化**（证明已从 jiffies 迁到 clocksource）
7. `nanosleep(15ms)` → 实际 ≥15ms 且 <25ms
8. `poll(100ms timeout)` 空 fd → 返回 ~100ms（非 50ms）

### ③ 回归

9. 全测试套件绿：EEVDF（双核）、poll/select、lwIP DHCP 续约、nanosleep
   systests、`spawn >2 init.elf`、70/70 systest
10. **boot 到 AHCI/FS 挂载正常**（确认 phase 4→6 期间 PIT 驱动 jiffies，
    `ahci_init`/`gpt_scan`/`fat32_init`/`ext2_init` 不挂起）

### ④ fallback 路径（一次性验证，非每次）

11. 人为 `lapic_timer_hz=0` → 回退 PIT 仍正常运行（tick 走 PIT，时间走 TSC）
12. 人为禁用 RTC 校准 → 时间基退 jiffies×10ms，系统可跑
13. 人为禁用 RTC PIE 且 CPUID 15h=0 → RTC PIE 校准超时 ~300ms 后落 jiffies 兜底，
    **boot 不挂起**（验证 §5.1 超时兜底）
14. 跨核 TSC 握手采样：code review + 断言 `tsc_offset` 量级 < 百万 cycle（
    QEMU 不支持真实偏移，靠单测 `tsc_offset` 计算逻辑）

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
| `kernel/apic/lapic_timer.c` | 校准改 §5.1（TSC 窗口 / RTC PIE 复用）；handler BSP→tick_handler / AP→need_resched；start 返回 bool；**删 divisor-16 retry 死代码**（TSC 窗口法下 10ms 内 32 位溢出需 LAPIC>429THz，原 lapic_timer.c:68-80 不再可达） |
| `kernel/driver/pit.c` | handler 改调 tick_handler()；serial_poll 留在本层 |
| `kernel/driver/rtc.c` + `rtc.h` | 新增 RTC PIE 联合校准（§5.1：IRQ8 临时注册 + PIE + 超时 + TSC/LAPIC 采样） |
| `kernel/arch/x86_64/subsys.c` | 初始化顺序重排：加 clocksource；保留 pit 启动；lapic-timer 改校准 |
| `kernel/arch/x86_64/subsys_percpu.c` | `lapic_timer_start` 适配 bool 返回值（**含 BSP 二次启动说明**） |
| `kernel/arch/x86_64/trap.c` | clock_gettime（含 REALTIME）/ nanosleep（含 do/while + EINTR rem）/ nanosleep_should_unblock 迁纳秒 |
| `kernel/include/kernel/task.h` | `wakeup_jiffies` → `wakeup_ns` |
| `kernel/fs/poll.c` + `kernel/include/kernel/poll.h` | deadline 语义迁纳秒（字段类型不变） |
| `kernel/intr/irq.c` + `kernel/include/kernel/interrupt.h` | 新增 `irq_mask()`/`irq_unmask()`（§4.4，mask-only，不动 handler） |
| `kernel/include/kernel/arch/cpu.h` | 加 `arch_cycle_freq()` 声明 |
| `kernel/arch/x86_64/cpu.h` | `arch_cycle_freq()` x86 实现（CPUID15h→RTC PIE→0） |
| `kernel/arch/aarch64/cpu.h` 内联 | `arch_cycle_freq()` 读 CNTFRQ_EL0（仅接口） |
| `kernel/arch/x86_64/smp.c` | AP 的 lapic_timer_start 适配 bool；握手采样（§8.3，online 挪到 tsc_boot 后） |
| `kernel/include/kernel/percpu.h` | `percpu_t` 增 `tsc_offset` / `tsc_sync_go` / `tsc_sampled` |
| `kernel/kernel/main.c` | percpu 循环后插入 `tick_start()`（先 irq_mask(0) 再 arch_tick_start） |

## 11. 测试与提交计划

### 提交序列（小步、语义化前缀，每步可独立验证）

1. `feat(time): clocksource + clockevent 双层抽象（TSC/cntvct_el0，接口层）`
   — 纯新增，编译过，无行为变化
2. `feat(time): TSC 频率校准（CPUID15h + RTC PIE 联合校准，含超时）`
   — 可独立验证 `arch_cycle_freq()` 正确 + RTC PIE 超时兜底
3. `feat(tick): BSP 切 LAPIC 周期 tick，先掩 PIT 再接管`
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
