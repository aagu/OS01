# Timer 重构：clocksource + clockevent 双层抽象 设计文档

- 日期：2026-08-17
- 状态：待用户 review（尚未进入实现）
- 前置：`docs/pit-200hz-handoff.md`（HANDOFF，2026-08-17）
- 目标：修复 QEMU TCG 下 PIT 200Hz 伪影，使 tick 与时间在 QEMU 与真实硬件都正确，并为未来 aarch64 移植预留接口
- 修订记录：
  - v1：初版
  - v2：并入首轮 review（boot 窗口 tick、IRQ8 时序、sys_now 语义、mult/shift、CLOCK_REALTIME、跨核 TSC、handler 契约、内核 self-test）
  - v3：并入次轮 review（正交性=联合校准、tsc_offset 握手采样、unregister 索引坑、PIE 超时、mult/shift 算术、BSP 二次启动、irq_mask API、nanosleep 迁移补全）
  - v4：并入三轮 review（register/unregister 参数约定写反、irq_mask 的 gsi→vector 转换、PIE 采样点+divisor、watchdog 双计、EINTR rem 下溢保护、握手有界等待、fallback AP 无 tick、门④.14 放宽、AP 复用 BSP 校准状态）
  - v5：并入四轮 review（AP 握手超时后无条件采样 bug、0.4%/0.8% 矛盾 + off-by-one 修复、超时 300→500ms、LVT_TIMER 掩蔽、IRQ8 读 0x0C 清标志、arch_tick_start 返回 bool、SYNC_SPIN_LIMIT 双侧同宏、lfence 可选、CPUID15h 有效=RTC 不需要注记、门②⑥ 忙等基准）
  - v6：并入五轮 review（300/500ms 三处统一、校准优先级死代码修正 + §8.1 正交表述如实化、DIV=0 是 ÷2 非 ÷1、register_irq(8) 带 IRQF_TRIGGER_LEVEL）
  - v7：并入 Hermes 外部评审（明确 lapic_timer_hz 语义=递减率不 ×2、irq_mask ioapic 对称性验证结论、tick_handler poll 扫描的 GS 时序假设、500ms 超时改 2^32 宽松兜底、divisor 注释统一 ÷2）
  - v8：并入实现期门①实测（QEMU 双核 298Hz 定位：`lapic_timer_start` 必须写 per-LAPIC `LAPIC_TIMER_DIV`，因 AP 复位 ÷1 而静态值 ÷2 折算 → 漏写导致 AP 200Hz）

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
- watchdog、kernel timer 轮（`kernel/time/timer.c`）
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

// 启动 arch tick 源：x86 配 LAPIC 周期模式（失败退 PIT）；aarch64 配 CNTP。
// 返回 bool：true = tick 源已启动（LAPIC/CNTP），false = 启动失败（PIT 继续）。
bool     arch_tick_start(void);
```

**`arch_tick_start` 返回 bool（写死，勿在 void/检查之间纠结）**：§7.2/§8.2.3
需要知道 LAPIC 启动成败来决定 `irq_unmask(0)`（回退 PIT）还是保持掩蔽。若
`arch_tick_start` 声明为 void，`tick_start` 就得事后查 `lapic_timer_hz != 0`，
但它是文件级 static 需要额外 getter。**直接让 `arch_tick_start` 返回 bool**：
x86 实现里返回「`lapic_timer_start(100)` 的 bool 结果」，`tick_start` 据此决定
掩蔽/回退。aarch64 未来实现返回「CNTP 配置成功」。接口写死，实现者照做即可。

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
void irq_mask(uint32_t gsi);     // 只 disable，保留 handler（内部 gsi→vector 转换）
void irq_unmask(uint32_t gsi);   // 只 enable，handler 仍在
```

实现走 `controller->disable/enable`（`pic_disable/enable` 或 IOAPIC mask/unmask），
**不触碰 `irq_desc` 的 handler 字段**。PIT 的接管掩蔽/回退使能用它，而非
`unregister_irq`。

**⚠️ 索引转换（同一类坑在本 API 内重演，必须写明）**：controller 层的
`enable/disable` 全部按 **vector** 索引——`ioapic_disable` 用 `nr - 0x20`
（`ioapic.c:195`）、`pic_disable` 用 `1 << (nr-0x20)`（`8259A.c:85`）。而
`irq_mask`/`irq_unmask` 对外收 **gsi**。实现必须内部转换
`controller->disable(0x20 + gsi)`，否则 `irq_mask(0)` 透传 gsi=0 → `0-0x20`
下溢 → `pic_disable` 掩错 bit / `ioapic_disable` 的 `find_ioapic_for_gsi` 不匹配
直接 return，**PIT 从未被掩蔽** → LAPIC 接管后两者都调 `tick_handler()` →
`jiffies` 双计（正是 §7.2 要防的）。

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
    // 1. 先掩 LAPIC_LVT_TIMER（lapic_write(LAPIC_LVT_TIMER, LVT_MASK)）——
    //    必须与现有 lapic_timer.c:48 校准第一行一致。否则 countdown 到零时以
    //    vector 0x38 触发中断，而 0x38 的 IDT gate 要等 lapic_timer_init 才注册
    //    （IDT 只盖 0x20-0x37 + 异常）→ GP# → 三连 fault。
    //    然后写 LAPIC_TIMER_DIV = 0（divide-by-2，按 Intel SDM 000b=÷2）——
    //    必须显式写，否则 elapsed_lapic 用的是上次遗留的 divisor，lapic_hz
    //    算错，且与 lapic_timer_start 复用 divisor 的前提不一致
    //    （lapic_timer.c:101 的 init_count 依赖 divisor 确定）。再装载
    //    0xFFFFFFFF 启动 countdown（无需 IDT gate，已掩）。
    //    ⚠️ divisor 值语义：DIV=0 是 ÷2（不是 ÷1，÷1 是 DIV=7/0b111）。
    //    校准/启动只要 divisor 一致即可，此处沿用 lapic_timer.c:53 的 DIV=0。
    // 2. 临时 register_irq(8, NULL, rtc_pie_handler, 0, IRQF_TRIGGER_LEVEL, "rtc-pie")
    //    —— 第一参数是 gsi（interrupt.h:50），不是 vector。RTC IRQ8 是 level 触发，
    //    显式传 IRQF_TRIGGER_LEVEL 避免 ioapic_enable 按 GSI 默认值猜触发模式
    //    （ioapic.c:138-153）。⚠️ 传 vector 0x28 会 ≥ MAX_GSI(24) → irq.c:13
    //    return 0 静默不注册。必须检查返回值：失败直接 return -1（走超时/兜底），
    //    别假设必然成功。然后使能 PIE（1024Hz，周期 ~976.5625µs）。
    // 3. IRQ8 handler 内：
    //    a. 每次进 handler 先读 RTC reg 0x0C 清中断标志，否则真实硬件上第一个
    //       中断后 PIE 停摆 → 校准等不到 N tick → 假超时：
    //          arch_outb(CMOS_ADDR, 0x80 | 0x0C); arch_inb(CMOS_DATA);
    //       （0x80 顺手满足 NMI 掩码。）
    //    b. 捕获 tsc0（第 1 个 PIE 沿）、计数；到第 N 个沿时捕获 tsc1。
    //       采样必须在 handler 内 PIE 沿上做——主循环采样会加中断延迟抖动。
    // 4. 主循环用 TSC 硬超时 ~500ms 兜底（500ms 内没来够 N tick → 放弃 -1）。
    //    判据用「已计 tick 数 + TSC 流逝」双条件，见下「超时兜底」段。
    // 5. 读 LAPIC CUR，算 elapsed_lapic = 0xFFFFFFFF - cur。
    // 6. 禁 PIE；unregister_irq(0x28) —— unregister 传 vector（irq.c:52 用 nr-32）。
    //    ⚠️ register/unregister 参数约定相反：register 传 gsi，unregister 传 vector。
    //    写 unregister_irq(8) → irq_table[-24] 野内存写（irq.c:58-62 清任意地址）。
    // 7. *tsc_hz_out   = (tsc1 - tsc0) * 1024 / (N - 1)   // 修 off-by-one，见下
    //    *lapic_hz_out = elapsed_lapic * 1024 / (N - 1)
    //    ⚠️ lapic_hz_out 语义 = 递减率（divisor ÷2 已折算），不是真实总线频率。
    //    因此**不 ×2**：elapsed_lapic 是 ÷2 后的递减量，除以窗口秒数 (N-1)/1024
    //    即得递减率。lapic_timer_start 的 init_count = hz/freq 直接基于递减率。
    //    详见下方「lapic_timer_hz 语义」段。
}
```

**lapic_timer_hz 语义（关键，两条校准路径与启动必须一致）**：`lapic_timer_hz` 存的
是**递减率**（= 总线频率 ÷ divisor，divisor ÷2 已折算），**不是真实 LAPIC 总线
频率**。这是现有 `lapic_timer.c` 的自洽语义：`lapic_timer_hz = elapsed * 100`
（elapsed 是 ÷2 后 10ms 递减量 → 递减率），随后 `lapic_timer_start` 的
`init_count = lapic_timer_hz / freq_hz` 直接基于递减率。**因此两条校准路径都不
×2**：
- TSC 窗口（10ms）：`lapic_hz = elapsed * 100`
- RTC PIE（n/1024s）：`lapic_hz = elapsed * 1024 / (n-1)`

若误 ×2 得到「真实频率」，`init_count = 真实频率/freq` 会被 divisor 再 ÷2，最终
LAPIC 跑 ~50Hz（验证门①直接失败）。这是 Hermes 评审「漏 ×2」的误报方向——
正确做法是**不 ×2**，并在此明确语义防再犯。

**采样窗口规格与精度（off-by-one 已修，数字统一）**：PIE 1024Hz，N=256 tick ≈
250ms。**主导误差是 off-by-one**：tsc0 在沿 #1、tsc1 在沿 #N → 实际跨越 (N-1)
个周期，公式若除 N 就系统性偏大 1/255 ≈ 0.39%。**正确做法除 `(N-1)`**，把
off-by-one 消掉。edge 上采样时，中断延迟抖动（µs 级 vs 976µs 周期）相对误差
< 0.1%，可忽略。修复后残余误差 ~0.1-0.2%，仍在验证门 ±1% 内。N 越大越准但
启动越慢；250ms 是精度/延迟的合理折中。（HANDOFF 的 TSCCAL 0.006%=60ppm 是
离线精细测量，不是启动校准目标——启动校准 ~0.2% 已够。）

**IRQ8 索引不一致的历史坑（必须写进实现注释）**：`register_irq` 的第一参数是
**gsi**，内部才 `vector = 0x20 + gsi`（`irq.c:32`），索引 `irq_table[gsi]`
（`irq.c:34`）；`unregister_irq` 的第一参数是 **vector**，内部用 `nr-32` 索引
`irq_table[nr-32]`（`irq.c:52`）。**两个 API 参数约定相反**：注册 gsi=8 后，注销
必须 `unregister_irq(0x28)`（vector）。写 `register_irq(0x28)` → gsi=40 ≥
MAX_GSI(24) → `irq.c:13` return 0 静默不注册（校准等不到 IRQ8 → 超时落兜底）；
写 `unregister_irq(8)` → `irq_table[-24]` 野内存写（`irq.c:58-62` 清任意地址）。
两条都要写对，且实现时在调用点各加一行「register 传 gsi / unregister 传 vector」
注释。

**超时兜底**：等 N tick 的循环用 TSC 硬超时 **~500ms**（N=256 名义 250ms，留
~250ms 余量应对 boot 期中断风暴/竞争）。判据用「已计 tick 数 + TSC 流逝」双
条件：主循环每次检查「(tick 数未达 N) && (TSC 流逝 < 500ms)」，任一条不满足
即退出。若 IRQ8 因任何原因不到（IOAPIC 路由失败、QEMU 变体、0x0C 未清导致
PIE 停摆），500ms 后放弃，`tsc_hz=0` → 落 jiffies 兜底，**绝不无限自旋挂
boot**。

**校准职责划分（优先复用联合结果）**：

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
    if (g_lapic_premeasured_hz != 0)
        lapic_hz = g_lapic_premeasured_hz;       // 优先：RTC PIE 联合结果（250ms 更稳）
    else if (tsc_hz != 0)
        lapic_hz = measure_against_tsc(tsc_hz);  // 回落：TSC 窗口（~10ms）
        // = elapsed * 100（elapsed 是 ÷2 后 10ms 递减量 → 递减率，不 ×2）
    else
        lapic_hz = 0;                            // 退 PIT
```

**优先级顺序说明（消除 v5 的死代码）**：`g_lapic_premeasured_hz` 只在
`rtc_pie_calibrate` 成功时被赋值，而成功必然 `tsc_hz != 0`——所以把
`measure_against_tsc` 放第一支会让 `g_lapic_premeasured_hz` 的 else-if 成为
**死路径**，联合测量的 LAPIC 结果从未被用，与「TSC 未知时复用」的叙事矛盾。
**正确顺序是优先复用联合结果**：它来自 250ms 的 RTC PIE 窗口，比 10ms 的 TSC
窗口更稳（更长的采样时间平滑抖动）；只有 RTC PIE 没跑（CPUID 15h 直接给了
tsc_hz，`g_lapic_premeasured_hz==0`）时才回落 TSC 窗口。两路径都可用时，
LAPIC 校准的**精度**不再依赖 TSC 校准链路，只依赖 RTC PIE。

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
- **校准误差**：RTC PIE 启动校准的测量误差，**~0.2%**（off-by-one 已修，见
  §5.1）；CPUID 15h 则 ppm 级。

验证门里的「±1%」是给**启动校准误差**（RTC PIE ~0.2%）留余量，不是换算误差。
换算误差小到可忽略。

## 6. 迁移明细（jiffies → 纳秒）

| 位置 | 现状 | 迁移后 |
|---|---|---|
| `trap.c` `SYS_clock_gettime` | `ms = jiffies * 10`（REALTIME==MONOTONIC） | `ns = clocksource_read_ns()`（两钟同迁） |
| `trap.c` `SYS_nanosleep` 整体 | `target = jiffies + ticks`(10ms) | `target_ns = now_ns + ns`（**含 do/while 循环与 EINTR rem 计算，见下**） |
| `task.h` `wakeup_jiffies` | 存 jiffies | 改名 `wakeup_ns`，存纳秒 |
| `trap.c` `nanosleep_should_unblock` | `jiffies >= wakeup_jiffies` | `clocksource_read_ns() >= wakeup_ns` |
| `trap.c` `SYS_nanosleep` do/while 条件 | `while (r==0 && jiffies < target)`（trap.c:1918） | `while (r==0 && clocksource_read_ns() < target_ns)` |
| `trap.c` `SYS_nanosleep` EINTR rem | `remain = (jiffies < target) ? (target-jiffies) : 0`（trap.c:1924-1929） | `remain_ns = (now_ns < target_ns) ? (target_ns - now_ns) : 0`（**必须保留下溢保护**） |
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

**AP 复用 BSP 校准状态（关键：divisor 是 per-LAPIC 硬件，不是全局变量）**：
`lapic_timer_hz` / `lapic_timer_divisor` 是**静态全局**（`lapic_timer.c:35-38`），
BSP 在 `lapic_timer_calibrate`（phase 4）算好它们。但 **`LAPIC_TIMER_DIV` 是
per-LAPIC 的硬件寄存器**：BSP 校准写了 BSP 的 `LAPIC_TIMER_DIV`，AP 的 LAPIC
复位后 `count_shift=0`（÷1，见 QEMU `apic_common.c` reset）。因此
**`lapic_timer_start()` 必须写 `lapic_write(LAPIC_TIMER_DIV, lapic_timer_divisor)`**
——把校准好的 divisor **值**应用到当前 CPU 的 LAPIC 硬件寄存器（不是重新校准
divisor 值，值始终是校准产物、不变量）。若省略，AP 复用 BSP 的 ÷2 折算
`lapic_timer_hz` 却用 ÷1 硬件 → AP 频率 ×2 = 200Hz，双核 100+200=300Hz（QEMU
实测 298Hz 吻合，门①失败）。此约束与「不在 start 里重新校准 divisor」不矛盾：
值不变，只是必须**每 CPU 各写一次**自己的硬件寄存器。

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

**tick_handler 的 poll 扫描路径同样受此窗口影响（本次改造后新增的 GS 读取点）**：
`tick_handler()` 内 poll 扫描调 `clocksource_read_ns()`（读 `this_cpu()->tsc_offset`），
而 `tick_handler` 在 phase 4（PIT 启动）到 `main.c:276`（GS 装）之间每 tick 都跑。
**安全前提是 `poll_timeout_head` 在 boot 期恒为 NULL**（poll 只在用户态进程里调，
用户态进程在 `task_init()` 之后才有），`tick_handler` 里的 `if (poll_timeout_head)`
短路使 `clocksource_read_ns()` 不被调用。实现时必须在 `tick_handler` 的 poll 扫描
里保留这个短路，并在注释里写明「boot 期 poll 空 → 不读 GS」这一时序假设；若未来
允许 boot 期注册 poll，须先解决 GS 早期安装。

**BSP 的 LAPIC handler 改动**：现在 `lapic_timer_handler` 里 `if (cpu_id() != 0)`
只给 AP 用；改成 BSP 走 `tick_handler()`（驱动 jiffies），AP 只做
`need_resched + watchdog`。

## 8. 错误处理

### 8.1 失败矩阵（tick 源 与 时间基 独立回退，靠 RTC PIE 联合校准保正交）

```
TSC 频率（clocksource）:
  CPUID 15h 有效 ──→ 用它（ppm 级）【RTC PIE 完全不需要走，见下注记】
  CPUID 15h=0 ────→ RTC PIE 联合校准（§5.1，同时测 TSC + LAPIC）
  RTC PIE 也失败 ─→ tsc_hz=0 → clocksource 退 jiffies×10ms（= 现状）

tick 源（clockevent）:
  PIT 先跑（boot 期，phase 4→6 驱动 jiffies）
  tick_start() 启动 LAPIC：
    有 g_lapic_premeasured_hz ──→ 优先复用 RTC PIE 联合结果（§5.1 顺序）
                                   → 成功 → 掩 PIT；失败 → PIT 保持运行
    无联合结果但 tsc_hz 已知 ───→ LAPIC 用 TSC 窗口校准 → 同上
    两者皆无 ───────────────────→ lapic_hz=0 → PIT 保持运行
```

**正交性表述（v5 修正，如实）**：LAPIC 校准的**精度**不再依赖 TSC 校准链路——
它优先复用 RTC PIE 联合结果（250ms 更稳），只在 RTC PIE 没跑时才回落 TSC 窗口。
但要注意**可及性**：LAPIC 校准能否成功，实际仍绑定 RTC PIE 或 TSC 窗口二者至少
其一可用（`g_lapic_premeasured_hz` 来自 RTC PIE，`tsc_hz` 来自 CPUID 或 RTC
PIE）。因此严格说「tick 源与时间基**完全独立**回退」不成立——它俩共享 RTC PIE
这一个校准底座。真正的独立是「**精度**独立」：TSC 校准失败不会拖累 LAPIC 精度
（LAPIC 有自己独立的 RTC PIE 结果）。「TSC 拿不到」≠「LAPIC 起不来」仍成立，
但成立的原因是两者都能从 RTC PIE 独立取结果，而非各自有独立校准源。此表述与
§5.1 的优先级顺序一致，避免「正交复用」叙事与死代码逻辑冲突。

**注记（消除读者困惑）**：矩阵里没有「CPUID 15h 有效但 RTC PIE 挂」这个组合——
因为 CPUID 15h 有效时**根本不走 RTC PIE**（`clocksource_init` 直接采用 CPUID
值，RTC PIE 只作为 CPUID=0 时的回落）。此时 LAPIC 用 TSC 窗口校准，RTC 的状态
与 tick 完全无关。

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
5. mult/shift 精度：换算舍入误差 < 1ppm（§5.2）；启动校准误差 ~0.2%（§5.3）。
6. RTC CMOS 访问：屏蔽 NMI（0x70 bit7）、检测 `is_updating_rtc()`（已有）再读。
7. RTC PIE 校准超时：§5.1 的 TSC 硬超时 ~500ms，绝不自旋挂 boot。
8. **fallback 模式的 AP 无 tick（预期降级，需注明）**：当 LAPIC 未校准成功
   （`lapic_timer_hz==0`）时，AP 的 `lapic_timer_start` no-op → **AP 没有任何
   调度 tick**（PIT 只投递 BSP）。此时 AP 上任务永不因 tick 抢占，只能靠
   IPI reschedule 或自愿让出。这是可接受的降级——tick 频率错误（200Hz）比
   AP 无抢占更严重，优先保证 BSP 时间正确。文档明确记录为预期行为，不视为
   bug。

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
uint32_t w = 0;
while (w++ < SYNC_SPIN_LIMIT && !cpu->tsc_sync_go)  // 2. 有界等 BSP
    arch_cpu_pause();
if (cpu->tsc_sync_go) {              // 3. ★只有观测到 go 才采样；超时则跳过（tsc_sampled 留 0）
    cpu->tsc_boot = rdtsc();         //    AP 读 TSC（与 BSP 几乎同时）
    __sync_synchronize();
    cpu->tsc_sampled = 1;            // 4. 回写完成
}

// BSP smp_boot_aps（等 online 后，替换原 warp 检测块）：
percpu_data[i].tsc_sync_go = 1;      // 发起
bsp_tsc = rdtsc();                   // BSP 读 TSC（与 AP 间隔仅一次内存写传播延迟 ~100ns）
for (uint32_t w = 0; w < SYNC_SPIN_LIMIT && !percpu_data[i].tsc_sampled; w++)  // 有界等 AP
    arch_cpu_pause();
if (!percpu_data[i].tsc_sampled) {   // 超时：AP 异常/被抢占/或 AP 侧也超时未采样，放弃修正
    debug_sched("SMP: TSC sync AP%u timeout, offset unset\n", i);
    percpu_data[i].tsc_offset = 0;
} else {
    ap_tsc = percpu_data[i].tsc_boot;
    percpu_data[i].tsc_offset = (int64_t)(bsp_tsc - ap_tsc);
}
```

**AP 超时后绝不采样（v4 的 bug 修正）**：AP 侧有界等待退出有两条路径——观测到
`tsc_sync_go`（正常）或计数耗尽（超时）。**超时后 AP 必须跳过采样、保持
`tsc_sampled=0`**。若超时后仍无条件采样，`tsc_boot` 会比 `bsp_tsc` 晚一整个
超时窗口，BSP 读到它算出的大负 offset 会把 AP 时间整体后推一个窗口——比不修正
还糟（v2 竞态的同款后果，只是量级变小）。BSP 侧的 `if (!tsc_sampled)` 救不了
这种情况（AP 已把 `tsc_sampled` 置 1，BSP 误以为成功）。因此两侧超时都落到
「offset 留 0」。

**有界等待**：`tsc_sync_go` / `tsc_sampled` 两个自旋都必须有界（与 `smp.c:235`
的 online 等待同风格），**两侧用同一个 `SYNC_SPIN_LIMIT` 宏**（如 ~1e6 次
pause，远大于正常传播时间、又远小于「挂死」）。MTTCG 下宿主线程抢占或 AP 异常
时，两侧都超时放弃修正（`tsc_offset` 留 0），**绝不无限自旋挂 boot**。

**序列化（可选收紧）**：`rdtsc` 不序列化，BSP 的 `tsc_sync_go=1` 写可能还没出
store buffer 就执行了 `rdtsc`。不影响正确性（gap 会被 offset 捕获，本来就是粗
修正），但想收紧 ~100ns 精度可在 BSP 侧 `bsp_tsc = rdtsc()` 前加 `lfence`（或
AP 用 `rdtscp`）。列为可选，非必需。

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

6. `clock_gettime` 连续两次读，**两次读之间用 `arch_cycle_counter()` 走固定
   cycle 数（如 ~10ms 的 cycle 预算）的忙等**（TCG 下紧挨的两次读可能落在同一
   虚拟时间片返回同值，固定 cycle 忙等确保跨过虚拟时间片且语义稳定——别用空转
   次数，那在 TCG/KVM 两种实现下语义不稳）→ 间隔 <10ms 也能反映**亚 tick 变化**
   （证明已从 jiffies 迁到 clocksource）
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
13. 人为禁用 RTC PIE 且 CPUID 15h=0 → RTC PIE 校准超时 ~500ms 后落 jiffies 兜底，
    **boot 不挂起**（验证 §5.1 超时兜底）
14. 跨核 TSC 握手采样：code review + 断言 `tsc_offset` 量级（**放宽**：MTTCG 宿主
    线程抢占会偶发延迟 AP 的 rdtsc，`tsc_offset` 可能超标；只断言「非零且符号
    合理」或仅记录值，不设 < 百万 cycle 硬阈值，避免偶发误报）

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
| `kernel/apic/lapic_timer.c` | 校准改 §5.1（TSC 窗口 / RTC PIE 复用）；handler BSP→tick_handler / AP→need_resched，**BSP 分支不再单独 `watchdog_counter++`**（避免与 `tick_handler()` 内的 watchdog++ 双计，现 lapic_timer.c:129 是无条件 ++）；start 返回 bool；**删 divisor-16 retry 死代码**（TSC 窗口法下 10ms 内 32 位溢出需 LAPIC>429THz，原 lapic_timer.c:68-80 不再可达） |
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
