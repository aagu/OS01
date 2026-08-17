# Timer 重构 spec/plan 评审意见（Hermes 外部评审，2026-08-17）

> 来源：Hermes Agent 对 `docs/superpowers/specs/2026-08-17-timer-clocksource-clockevent-design.md`（v6）
> 与 `docs/superpowers/plans/2026-08-17-timer-clocksource-clockevent.md` 的独立评审。
> 评审基于源码交叉验证（~/OS01 实际代码 + QEMU 9.2.0/11.1.0 源码对照）。
> 请 Claude Code 据此修订 spec/plan，**不必逐条照做**，有异议的地方先说明理由再改。

## 总体结论

架构合理、深度罕见（6 轮 review 痕迹明显），plan 可执行性高。可以进入实施。
但发现 **1 个真实 bug 级问题、1 个 API 语义风险、2 处需注意的时序耦合**。
建议先修 4 点再开工。

---

## 🔴 问题 1（必改）：RTC PIE 联合校准的 lapic_hz_out 漏 ×2（divisor 折算不一致）

**位置**：plan Task 2 Step 2 的 `rtc_pie_calibrate()`：

```c
*lapic_hz_out = elapsed_lapic * 1024 / n;   // ← 漏了 ×2
```

**问题**：`elapsed_lapic` 来自 ÷2 divisor（`LAPIC_TIMER_DIV=0` → QEMU count_shift=1 → 每 tick 2ns，即 ÷2）下的 countdown 计数。真实 LAPIC 频率 = `elapsed_lapic × 2 × 1024 / n`，**当前公式少算一半**。

**对照**：plan Task 3 Step 1 的 TSC 窗口路径就写了 `lapic_timer_hz = elapsed * 200`（= elapsed × 2 × 100），**两条路径对同一 divisor 的处理不一致**。若 RTC PIE 路径优先（spec §5.1 顺序），LAPIC 实际会跑 ~50Hz 而非 100Hz，验证门①直接失败。

**修法**：`*lapic_hz_out = elapsed_lapic * 2 * 1024 / n`，或统一用一个 `LAPIC_DIV_FACTOR` 宏（=2），两条路径共用，避免再犯。

**验证**：QEMU LAPIC 频率已知 1 tick/ns（apic_common.c count_shift 语义），÷2 后 10ms 窗口 elapsed ≈ 5,000,000 → ×200 = 1e9 ✓（TSC 路径）；RTC PIE 路径 250ms 窗口 elapsed ≈ 125,000,000 → ×2×4 = 1e9 ✓。

---

## 🟠 问题 2（需验证）：irq_mask 用 controller->disable 是否真能"掩蔽并保留 handler"

**位置**：plan Task 1 Step 7b 的 `irq_mask/irq_unmask`。

**问题**：`irq_mask` 调 `p->controller->disable(0x20 + gsi)`，但 **controller 层的 disable/enable 语义未在 plan 里验证**：
- 若 `ioapic_disable`（ioapic.c:195 附近）是"清除 IOREDTBL 路由"而非"置 mask 位"，回退时 `ioapic_enable` 能否恢复？
- 若语义不对称（disable 注销路由、enable 重新注册需要额外参数），回退 PIT 时 IRQ0 永久失效。

**修法**：plan 加一步（Task 1 gate）——**实现 irq_mask/unmask 前，先读 `ioapic.c` 的 disable/enable 实现，确认语义对称**；若不对称，改用直接操作 IOREDTBL mask 位（`entry |= IOAPIC_LVT_MASKED`）的方式，或换 `pic_disable/enable`（8259 的 OCW1 天然是 mask-only）。

---

## 🟠 问题 3（时序耦合）：PIT 期 pit_handler → tick_handler → clocksource_read_ns() 的 GS base 时序

**位置**：spec §7.4（GS base=0 窗口）+ plan Task 3 Step 4（pit_handler 改调 tick_handler）。

**问题**：`tick_handler()` 内的 poll 扫描会调 `clocksource_read_ns()`，而它读 `this_cpu()->tsc_offset`（GS 基址偏移）。**GS base 直到 main.c:276 才装**，但 phase 4-6 之间 PIT 一直在跑（`pit_handler` 每 tick 调 `tick_handler`）。若 poll 注册表非空（boot 期 unlikely，但 select/poll 可能在 subsys 期间被调），GS base=0 时 `this_cpu()` 解引用会崩。

**现状**：spec 说"boot 期校准用原始 arch_cycle_counter() 不走它"——但那只覆盖校准路径，**没覆盖 tick_handler 的 poll 扫描路径**。现状代码 pit_handler 调 `this_cpu()`（pit.c:42）没炸只是 boot 快（GS=0 时 GS 段偏移访问恰好落到低内存，可能碰巧可读）。

**修法**：plan 加一条显式验证/防护——**tick_handler 的 poll 扫描在 `clocksource_active && GS 已装` 时才读 ns**；或在 main.c 的 `percpu_install_gs(0)` 之前不注册任何 poll（boot 期本来就没有）。至少要在 plan 里写明这条时序假设。

---

## 🟡 问题 4（精度）：rtc_pie_calibrate 的 500ms 超时硬编码 3GHz

**位置**：plan Task 2 Step 2 的 `arch_cycle_counter() - tsc_start > 1500000000ULL`（~500ms @3GHz）。

**问题**：硬编码 3GHz 假设，真实硬件若为 2.5GHz/4.5GHz，超时预算偏差 ±50%。QEMU TCG 下 TSC=2.994GHz 恰好接近，掩盖了这个问题。

**修法**：超时预算用已知 freq（`clocksource_freq_hz()` 若已有，或先用 `cpuid_15h()` 粗测），否则用 3GHz 粗估并注释"实际由调用方 freq 校准误差兜底"。plan 里已有 `(void)budget` 的自我怀疑注释，落实即可。

---

## 🟡 问题 5（注释矛盾）：divisor 语义两处注释打架

**位置**：现有 `lapic_timer.c:52` 注释 `// divide by 1` vs plan Task 3 Step 1 注释 `// ÷2`。

**事实**（已对照 QEMU 源码）：`apic.c:1040-1045`：DIV 写 0 → `count_shift=1` → 每 tick 2ns = **÷2**。现有注释"divide by 1"是**错的**（行为自洽是因为校准/启动用同一 divisor）。

**修法**：实现时统一注释为"divisor=0 → ÷2"，并加一行说明 QEMU/SDM 语义，避免误导。

---

## 🟡 建议（不阻断）：Task 顺序的承重墙

**Task 3 落地后先跑验证门①（trace LAPIC 100Hz）确认接管成功，再动 Task 4 迁移**。这是整个重构的承重墙——先出一版"tick 正确 + 时间仍走 jiffies"的可运行系统，再叠纳秒迁移，失败定位面小。

另外：KVM（非 TCG）下握手采样的 store 延迟可能 >100ns，若测得 offset 异常大，实现注释里提示加 `lfence`（spec 已标可选）。

---

## 已确认正确（无需改）

- `register_irq` 收 gsi / `unregister_irq` 收 vector 的警告（irq.c:12,47 验证属实，写反会野写）
- DIV=0 是 ÷2 的 spec 判断（纠正了现有代码错误注释）
- smp.c online:123 先于 tsc_boot:127 的竞态（握手采样方向正确）
- `lapic_timer_hz = elapsed*100` 被 200Hz 污染的分析（对应实测 629Hz）
- MAX_GSI=24、percpu_t.tsc_boot、arch_cycle_counter 双架构存在
- poll 结构体 tag 名（plan 已自注 `poll_timeout_node_t`）
- mult/shift 公式与示例数值（shift=33 @2.994GHz）
