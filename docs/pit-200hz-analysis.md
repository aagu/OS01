# PIT 200Hz 之谜 — 外部分析结论（递送时间 2026-08-17 ~01:05 +0800）

> 来源：Hermes（host 侧）直接读 QEMU 9.2.0 源码 + homeserver 实测，非猜测。
> QEMU 源码位置：`/home/aagu/OS01/toolchain/qemu-9.2.0`（在你工作目录的父仓库里，之前以为"不在本地"是找错地方了）。

## 一、已确认（证据自洽，矛盾不在这几处）

1. **guest TSC = host TSC**。`hw/i386/x86-cpu.c:35`：
   ```c
   uint64_t cpu_get_tsc(CPUX86State *env) { return cpus_get_elapsed_ticks(); }
   ```
   未启用 icount 时 → `system/cpus.c:248` `cpus_get_elapsed_ticks()` → `cpu_get_ticks()`（`system/cpu-timers.c:63`）→ `cpu_get_host_ticks()`（host CPU cycle counter）。
2. **homeserver host 实测 TSC 频率**：`2,994,492,000 Hz`（AMD Ryzen 9 7845HX，C 程序 1s 窗口 rdtsc 差值实测）。
3. **代入你的测量** `avg_tsc_per_tick = 14,970,245`：
   ```
   14,970,245 / 2,994,492,000 = 0.0050 s = 5 ms
   ```
   → **PIT 中断周期确实是 5ms = 200Hz**。你的墙钟证据（guest 报 10.01s vs wall 5.048s 精确 2x）与 TSC 测量、PITRATE 探针三者完全自洽。jiffies++ 单处、IRQ0 单注册、divisor 11931 编程正确、IOAPIC 路由正确——这些排查都对，不是问题所在。
4. **QEMU PIT mode 3 理论 100Hz 也没错**（`hw/timer/i8254_common.c` `pit_get_out`：mode 3 每 count 个 PIT 时钟一个完整方波周期 = 一个上升沿；divisor 11931 → 1193182/11931 ≈ 100Hz）。

## 二、矛盾点（缩小到一个）

PIT 计时用 `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`（`hw/timer/i8254.c:57`）。
非 icount 模式下 `cpus_get_virtual_clock()`（`system/cpus.c:212`）回退到 `cpu_get_clock()` = **host 墙钟** → 理论仍 100Hz。
但实测 guest 每 5ms 墙钟就 tick 一次 → **要么 QEMU_CLOCK_VIRTUAL 实际推进是墙钟 2 倍，要么 PIT divider 被隐含减半（count=5965=11931/2 → 恰好 200Hz）**。

## 三、建议的下一步验证（按优先级）

1. **QEMU monitor `info qtree`**：查 i8254 channel-0 的 `count` / `mode` 实际值。
   - `count == 11931` → PIT 编程正确，问题在虚拟时钟推进（查 icount/虚拟时钟）。
   - `count == 5965` → divider 减半，查 QEMU 对 PIT 0x36 命令字节/写两次的解释，或 OS01 是否第二次写 PIT。
2. **查 QEMU 启动参数**：当前命令行无 `-icount`（`ps` 可见 `-M q35 -smp 1` 等），确认是否有 `-rtc`/`-global` 影响时钟。
3. 你的 RTC 校准（`[TSCCAL]`）可以交叉验证：RTC 是独立 CMOS 时钟，用它对 TSC 校准后反推 PIT 速率，与上面 `info qtree` 对照。

## 四、附注

- OS01 侧 `CLOCK_FREQUENCY = 1193180`，`PIT_ICW = 0x36`（channel0/lo-hi/mode3/binary），`set_frequency(100)` → divisor 11931。写入序列标准。
- 若 `info qtree` 显示 count 正常且无 icount，建议在 QEMU monitor 里 `info registers` 对照虚拟时钟 vs 墙钟；或临时加 `-icount shift=0` 跑一次对比（注意：icount 会改变 TSC 语义，仅作诊断）。
- 已确认 OS01 `kernel/driver/pit.c` 里有 TEMP DIAG 探针（PITRATE/PITDIV/TSCCAL），做完可清理。
