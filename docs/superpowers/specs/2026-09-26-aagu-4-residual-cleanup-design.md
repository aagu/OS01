# AAGU-4 残留清理设计稿

> 基准：`d695020`（2026-09-26）
> 规范：[`docs/arch/cross-boundary-symbols.md`](../arch/cross-boundary-symbols.md) §2.3、§3.3
> 目标：通用 TU 不含处理器架构分支；平台行为留在 arch，poll 行为留在 fs；不增加重复实现；softirq 原子操作维持内联热路径。

## 1. 范围与现状

roadmap Parked 表中 `kernel/include/compat/` 裁撤和 x86-only PIC/APIC 驱动迁移已完成；本次不重复实施。当前自有 `kernel/` 源码中的待处理点如下：

| 类别 | 位置 | 当前问题 |
|---|---|---|
| 通用 TU | `intr/softirq.c` 2 处 | 用架构宏选择原子操作 |
| 通用 TU | `time/clocksource.c`、`time/timer.c` 各 1 处 | 用架构宏包住通用 `SUBSYS_INITCALL` |
| 通用 TU | `time/tick.c` 2 处 | 架构宏同时包住 poll 超时扫描和 PIT/LAPIC handoff |
| 通用 TU | `time/timer.c` 1 处 | 架构宏选择 spin hint |
| 通用头 | `include/time/clocksource.h` 2 处 | 架构宏包住 x86 专用 per-CPU include 和 `clocksource_read_ns()` |
| 仅 x86 构建的 TU | `driver/ahci.c`、`keyboard.c`、`pit.c`、`serial.c`、`net/net.c` 各 1 处 | 自注册块有构建路径已保证为真的架构宏 |

合计 **14 个条件编译块，分布于 10 个现有文件**；每块可包含多行预处理指令。aarch64 源列表明确包含上述四个通用 TU；x86 通过 wildcard 收集。验收按条件编译块及实际构建结果计数，不按源码行数计数。

## 2. 边界设计

### 2.1 softirq：通用调用面，架构内联实现

`kernel/intr/softirq.c` 的 `set_softirq_status()` 和 `do_softirq()` 只调用 `arch_atomic_or_u64()`、`arch_atomic_and_u64()`，不含架构判断或汇编。现有 `kernel/include/arch/atomic.h` 是架构分派 facade；删除其中两条外部函数原型，由其对应分支包含新建的 `kernel/include/arch/x86_64/atomic_bitops.h` 或 `kernel/include/arch/aarch64/atomic_bitops.h`。两个 per-arch 头各提供 **static inline + `__attribute__((always_inline))`** 实现，沿用当前 `kernel/arch/<arch>/cpu/atomic.c` 中的 `lock orq/andq` 和 `ldaxr/stlxr` 循环。删除两份旧 `.c` 定义，避免同名外部定义和双份算法。

内联是默认 -O2 和 DEBUG=1/-O0 构建共同的功能约束：普通 `static inline` 在 -O0 可生成函数调用；现有 softirq 注释记录过 out-of-line 调用引发 CI kernel-selftest 波动。迁移时保留 x86 `memory` clobber、aarch64 `=&r` early-clobber 和 acquire-release 语义。现有 `kernel/selftest/test_arch_atomic_u64.c` 继续检查两种操作，并单独运行 x86 QEMU kernel-selftest。

### 2.2 tick：按责任拆分，不新增 arch tick TU

`kernel/time/tick.c` 保留共同的 `jiffies`、resched、watchdog 和 timer softirq 逻辑。它每 tick 调用 `poll_timeout_tick()`；本 TU 给出 weak 空默认实现，供尚未编译 poll 子系统的 aarch64 phase 1 使用。`kernel/fs/poll.c` 给出唯一 strong 实现，将当前扫描、锁和唤醒逻辑原样从 tick.c 搬入；注册表和锁仍由 poll.c 拥有。链接时 strong 实现覆盖 weak 默认，不新增第二份扫描算法。此 hook 表示可选文件系统功能，名称不带 `arch_`。代价是每 tick 一次跨 TU 调用（aarch64 当前为空调用）；这是为隔离可选 poll 功能接受的固定成本，应在两架构 QEMU tick 测试中检查计时与稳定性。

`tick_start()` 只调用现有 `arch_tick_start()`。将 x86 的 `irq_mask(0)`、LAPIC 启动结果判断和失败时 `irq_unmask(0)` 搬到已有 `kernel/arch/x86_64/platform/time.c::arch_tick_start()`；保持该函数的 bool 返回值与 PIT 回退语义。aarch64 的 `arch_tick_start()` 仍由 `kernel/arch/aarch64/platform/time.c` 实现，现有 aarch64 启动流程直接调用它。本次不改变启动调用顺序，也不增加 `arch_tick_handoff()` 或 weak 平台空实现。

poll 超时是文件系统功能是否编入的问题；PIT/LAPIC 是 x86 平台行为。不得把 poll 注册表扫描放进 `kernel/arch/x86_64/`。

### 2.3 clocksource：保持通用头轻量

`kernel/include/time/clocksource.h` 保留 `clocksource_active`、`clocksource_mult/shift`、`clocksource_init()`、`clocksource_freq_hz()`、`clocksource_cycles()` 声明，删除 x86 per-CPU include、`clocksource_read_ns()` inline 和两个条件编译块。

新建 `kernel/include/arch/x86_64/clocksource.h`，显式包含 `time/clocksource.h`、`time/timer.h`、`arch/cpu.h`、`percpu/percpu.h`，保留当前纳秒计算与未激活时 `jiffies * 10000000ULL` 回退。它必须可独立 include。`kernel/fs/poll.c`、`kernel/arch/x86_64/intr/trap.c` 改用此头；`kernel/time/tick.c` 不再需要 clocksource 头。`hosttests/cases/test_clocksource.c` 也改用新头，并在 hosttests Makefile 中增加头依赖，保留 active/fallback 断言。`kernel/time/clocksource.c` 继续使用通用头，并显式包含 `arch/cpu.h`（原先由旧通用头传递）。`kernel/arch/x86_64/intr/apic/lapic_timer.c` 只使用通用频率 API，保留其原 include。

### 2.4 其余清理

- `kernel/time/timer.c` 的 spin hint 改为 `arch_cpu_pause()`，显式包含定义它的 `arch/cpu.h`。
- `kernel/time/clocksource.c` 和 `kernel/time/timer.c` 的 `SUBSYS_INITCALL` 块删除架构条件；构建选择已由 `kernel/Makefile` 决定。
- `kernel/driver/{ahci,keyboard,pit,serial}.c` 和 `kernel/net/net.c` 删除仅包住自注册块的 x86 条件；五个 TU 均不在 aarch64 源列表中。`driver/pit.c` 由 x86 `ARCH_PLATFORM_C_SOURCES` 添加。

## 3. 文件改动与构建约束

| 动作 | 文件 |
|---|---|
| 新增 | `kernel/include/arch/x86_64/atomic_bitops.h`、`kernel/include/arch/aarch64/atomic_bitops.h`、`kernel/include/arch/x86_64/clocksource.h` |
| 删除 | `kernel/arch/x86_64/cpu/atomic.c`、`kernel/arch/aarch64/cpu/atomic.c` |
| 修改 | `kernel/include/arch/atomic.h`、`kernel/include/time/clocksource.h`、`kernel/intr/softirq.c`、`kernel/time/{tick,timer,clocksource}.c`、`kernel/fs/poll.c`、`kernel/arch/x86_64/platform/time.c`、`kernel/arch/x86_64/intr/trap.c`、`kernel/driver/{ahci,keyboard,pit,serial}.c`、`kernel/net/net.c`、`hosttests/cases/test_clocksource.c`、`hosttests/Makefile` |

不新增 `kernel/arch/<arch>/time/` 或 `kernel/time/tick_default.c`，不改变 `kernel/Makefile` 的 aarch64 源列表和 `ARCH_SOURCE_DIRS`。新头通过现有 `-Iinclude` 解析；两个旧 atomic TU 的删除由现有 arch `cpu/*.c` wildcard 自动反映。无结构体或 UAPI 变更。实施验证前对相关 profile 执行 `make clean`，排除缺少头依赖导致的陈旧对象。

## 4. 验证与验收

1. 静态检查：10 个现有目标文件中原有 14 个架构条件编译块均已删除；`kernel/intr/softirq.c`、`kernel/time/tick.c` 不含架构分支。检查两个 per-arch 原子头只有各自一份实现，旧 `.c` 文件已删除；默认优化与 DEBUG=1 两种编译产物中的 softirq 调用点均无 `arch_atomic_or_u64` / `arch_atomic_and_u64` 调用指令。
2. 编译检查：干净构建 x86_64 与 aarch64；确认没有 `poll_timeout_tick`、`arch_atomic_*_u64`、`clocksource_read_ns` 的未定义或重定义，也没有新头的隐式声明。核对 x86 PIT/LAPIC handoff 与 aarch64 CNTP 调用路径。
3. 宿主检查：`make test-host`，重点检查 clocksource 与 poll 相关测试；clocksource 测试必须实际 include 新 x86 头，并覆盖纳秒计算和 jiffies 回退。
4. QEMU 检查：x86 `make OS01_SYSTEST=1 test-qemu SUITE=systest`、`make test-qemu SUITE=network`；单独运行 `make KERNEL_SELFTEST=1 test-kernel-selftest` 观察 softirq 原子操作与既有 CI 波动；aarch64 运行 `make PROFILE=aarch64-clang test-aarch64 MODE=smp`。各 suite 的期望计数以运行时测试清单为准，不写死旧基线数。
5. 文档检查：落地后更新 `docs/arch/cross-boundary-symbols.md` §3.3 与 §6、`docs/changelog.md`。闭环报告只在项目流程要求时创建，避免再复制一份变更清单。

## 5. 风险与回退

- softirq：保留原汇编约束和内存顺序；功能 selftest 与 x86 QEMU kernel-selftest 都通过才验收。若重现波动，检查生成汇编与 tick 路径时序，不退回通用 TU 的架构分支。
- poll：保持 `spin_lock_irqsave`、deadline 时间轴、`wait_queue_wake_all` 和每 tick 重试语义；hook 多一次函数调用，由 host poll 测试、systest 及两架构 QEMU tick 稳定性检查覆盖。
- clockevent：x86 在尝试 LAPIC 前屏蔽 PIT，失败时恢复 PIT；aarch64 现有 CNTP 直接启动路径保持不变。
- 单个 PR 可回退；按 softirq、tick/poll、clocksource、条件清理四组检查点验证，不强制压成单 commit。

## 6. Spec 自检

- 范围限于 14 个现有条件编译块及直接依赖的接口、构建与测试迁移。
- 新增 3 个头，删除 2 个旧实现 TU；不新增 tick TU 或头，不改变 aarch64 whitelist。
- 通用层无架构判断；架构汇编留在 `include/arch/<arch>/` 或 `kernel/arch/<arch>/`；poll 注册表由 fs 维护。
- 文件清单、调用路径与验证命令按当前仓库核对，最终以干净构建和测试输出为准。
