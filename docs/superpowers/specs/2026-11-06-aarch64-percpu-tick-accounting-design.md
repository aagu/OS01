# aarch64 每核 tick 与调度记账设计

日期：2026-11-06。基线：`ca0e43ad`。版本：R3。独立 reviewer `spec_review` 已于本日明确 **APPROVE（R3）**。评审与修订记录见 [review ledger](../reports/2026-11-06-aarch64-percpu-tick-accounting-doc-review.md)。

## 1. 目标与边界

为已录用的 aarch64 调度 CPU 提供 100 Hz 本地 CNTP tick，复用公共 CPU busy/idle 与 task runtime 记账，让 aarch64 的 vruntime 按实际收到的本地 tick 推进，而非按 schedule 调用次数推进。BSP 继续独占 jiffies、poll timeout、timer-list/TIMER_SIRQ 和 blocker 扫描。

本期仍是 EL1 协作式调度：timer/SGI handler 和 IRQ 返回路径均不得调用 schedule。线程不 yield/block 时，其 runtime/vruntime 可持续增长、need_resched 可置位，但不会被强制切走。同核两个无限计算线程轮转属于下一期 IRQ 返回抢占验收，本期不能声称已支持。

不包括迁移、EL0/SVC、用户 MM/ASID/COW、FP/SIMD、tickless、CPU hotplug、统一 kernel_main 或通用 clockevent 框架改造。不改变 UAPI、boot_context、task_t、thread_t、percpu_t 布局；继续 `-mgeneral-regs-only`。x86 调度与时间行为保持现状。

## 2. 当前实现与选择

- `arch/aarch64/platform/time.c` 的 BSP `arch_tick_start()` 注册全局 CNTP handler 并启动本核；ISR rearm 后调用 `time/tick.c::tick_handler()`，每秒打印一次 `[tick] N`。
- `smp/smp.c::secondary_idle()` 禁用 AP CNTP。GIC banked PPI 已在各核 `gic_cpu_init()` 配好；handler table 只允许 BSP 注册一次，不能在 AP 重注册。
- accepted AP 在 `cpu/ap_sched.c::aarch64_ap_idle_handoff()` 安装合法 task stack 和 rq idle 后发布 scheduler_ok；未录用或迟到 AP 留在旧 work loop。
- `sched/accounting.c` 已提供 irqsave 保护的每核 busy/idle/epoch/hz 快照与 atomic runtime_ticks 增量，但 aarch64 产品未链接此文件，也未调用初始化和启动接口。
- `sched/core.c::schedule()` 调用 `fair.c::update_curr()`，每次加 1 vruntime。直接再在 tick 加 1 会重复收费。
- 原 scheduler timer observer 的 `aarch64_sched_timer_tick_count` 是全局计数，开启 AP timer 后必须消除并发写，并保持原 BSP probe 语义。

考虑三种方案：只加使用率而保留 schedule 计费，交付较小但继续让 yield 频度决定 vruntime；全面改造双架构 tick/clockevent，扩大 x86 风险；选择 **aarch64 本地 tick 后端 + 复用现有 accounting，限定 aarch64 的 vruntime 计费迁移**。该选择不改变 x86 的 update_curr 调用和 BSP/AP timer 分工。

## 3. 文件与接口

新增源目录/头目录成对：

- `kernel/arch/aarch64/cpu/sched_tick.c` / `kernel/include/arch/aarch64/sched_tick.h`：本地调度 tick 生命周期、ready gate 和调度记账。
- `kernel/selftest/test_aarch64_sched_tick.c` / `kernel/include/selftest/aarch64_sched_tick.h`：专用产品路径自测及证据。

冻结接口：

```c
/* arch/aarch64/sched_tick.h */
#define AARCH64_SCHED_TICK_HZ 100U
void aarch64_sched_tick_init(void); /* BSP once, before AP handoff */
bool aarch64_sched_tick_start(void); /* current CPU; IRQ masked, legal idle stack */
void aarch64_sched_tick(void); /* current CPU; CNTP ISR only; IRQ masked */

/* arch/aarch64/timer.h — 与 platform/time.c 对应的公开后端头 */
bool aarch64_timer_start_local(void); /* AP local arm; no handler registration */
```

新增 `kernel/include/arch/aarch64/timer.h` 对应已有 `platform/time.c`（依照当前 arch 头布局统一放 arch/aarch64）。`arch_tick_start()` 仍是 BSP boot 入口，签名和调用点保留。不得用相同名字建立第二个时钟源。

`sched_tick.c` 使用静态 NR_CPUS 长度的 started 状态数组，初始化前的 ready gate 使用 release/acquire；不得增加汇编可见结构字段。初始化调用 `sched_accounting_init()` 一次，必须早于任一 `sched_accounting_cpu_start()`，不能依靠 BSS=0 初始化 spinlock。

## 4. 启动时序与失败

### BSP

保留 boot/main.c 当前 clocksource → softirq_init → arch_tick_start 的早期 BSP timer 顺序。这个阶段 ISR 只做全局 tick，不允许通过 SP 推导 current。

在 `arch_aarch64_prepare_bsp_handoff()` 的 boot-stack 阶段调用 aarch64_sched_tick_init；只初始化数据，不能读 current。进入 `arch_aarch64_bsp_handoff()` 后，idle 元数据与 `sched_boot_publish_idle()` 已就绪、IRQ 仍关闭时，调用 aarch64_sched_tick_start，再 `sched_boot_start()` 发布 BSP scheduler_ok。BSP 启动记账只验证现有 CNTP ENABLE=1/IMASK=0，不重写周期或注册 handler，不重置全局 jiffies/g_ticks。

### AP

secondary_idle 中继续明确禁用 CNTP，旧 work loop 不启动 tick。在 aarch64_ap_idle_handoff 校验合法栈、rq idle、cpu 身份后，IRQ 仍关闭时调用 aarch64_sched_tick_start，再按原有顺序 IRQ enable + ISB → release scheduler_ok。timer 可在 scheduler_ok 发布前 pending/到达，但 gate 必须使调度记账惰性返回；不能在该窗口读取 current。

AP 的 aarch64_timer_start_local 仅使用 BSP 已发布的只读 timer 配置（频率、周期和注册完成状态），核对本核频率与 BSP 一致，配置本核 TVAL/CTL，执行 ISB 并检查 ENABLE=1、IMASK=0。不写全局 GIC handler table，不重新初始化 GIC、不写其它 CPU 的 timer。失败关闭本地 CNTP。

timer 配置由 BSP 在注册成功后 release 发布；AP acquire 检查。CNTP 100 Hz 周期保持原有 `max(freq / 100, 1)`，但 TVAL 为有符号 32 位：配置须满足 freq>=100 且 1<=period<=INT32_MAX，不可截断溢出。零频、不一致频率和非法 PPI（必须 16..31）失败；BSP注册失败包括 `-2` 一律失败，不能把未知旧 handler 当成功。重复成功的本地 sched tick start 是幂等操作，不 rearm、不清零统计、不推进 epoch；本期不支持 stop/restart 或动态改频。

start 先验证 init ready、CPU 边界及 accepted mask（CPU0 亦须包含），再验证合法本核 idle；拒绝路径不得读取未就绪 current。初始化完成后，每核先确认 timer 成功，再调用 sched_accounting_cpu_start(cpu,100,true)，最后 release 发布 started。失败发布 started=false、accounting invalid 并返回 false；init 未 ready 时直接返回，不触碰尚未初始化的 accounting 锁。handoff 调用者以 log_err 报告并 fail-stop，不能带坏 timer 发布 scheduler_ok。未录用 AP 永远 started=false/accounting valid=0，即使其 online/ipi_ready 为 1。

## 5. ISR 和计费契约

CNTP ISR 顺序：

1. 先按只读 period rearm 当前 CPU 的 TVAL。
2. 只在 CPU0 调用 tick_handler，保留全局 jiffies/poll/timer pending 与 `[tick] N` 序列；g_ticks 仍为 BSP 私有。AP 不触碰这些全局对象，也不执行 do_softirq/do_timer/blocker scan。
3. 所有 CPU 调用 aarch64_sched_tick。初始化 ready、local started、acquire scheduler_ok、rq idle 均成立后才读取 current。IRQ masked 保证本核执行任务不能切换或释放。先调用 sched_account_tick（其 usage lock 独立释放），再取本核 rq_lock，执行 update_curr(current)，并增加本核 watchdog_counter。idle 不增加 vruntime/runtime_ticks，只增加 idle_ticks 与 watchdog。
4. timer handler 返回后，仅原 gic_dev_dispatch 写一次完整 raw IAR 到 EOIR。handler 不 EOI、不 schedule、不解开 IRQ、不清 need_resched、不分配；除保留的 BSP 每秒 `[tick] N` 外不格式化日志。

`time/tick.c` 中 aarch64 分支不再无条件写 need_resched/watchdog；这两项迁入 ready-gated 本地调度 tick。x86 分支原样保留（包含 BSP sched_account_tick）。**每个有效 BSP 本地 tick 均 release-store need_resched=1，包括 idle，以使空 rq 的 BSP 在 idle loop 内进入 schedule 并扫描到期 blocker。** AP 仅在 busy task vruntime>=deadline 时通过 update_curr 置位；AP idle tick 不无条件请求 schedule。SGI4 的已有置位仍有效，tick 不覆盖/清除它。BSP tick 置位并不在 IRQ 内执行扫描或切换，仍等原协作入口消费。

仅 aarch64 从 schedule 中移除 update_curr 调用；x86 保留。无需用户态 ABI 或 deadline/rbtree 算法改动。tick 更新的 task 是本核正在执行且 off-rq 的任务；rq_lock 同时保护调度字段。任何读取这些字段的新增测试要在 IRQ masked 或 rq lock 下取快照，不能借 volatile 进行跨核同步。

这里的 100 Hz 是名义采样频率，**每次真实递送的 CNTP IRQ计一个样本**，不按过期墙钟时间补算丢失 tick。中断长时间关闭、TVAL 相对 rearm 和串口开销会影响采样，不承诺精确墙钟 CPU 时间。零 tick 的若干次 yield 不应产生 vruntime/runtime 费用；busy N 次有效 tick 应使 runtime_ticks 和 vruntime 各增 N，usage busy 增 N；idle N tick 只增加 idle 统计。未 ready/started 的 tick 不产生调度费用。

## 6. 并发、观测与回归约束

- rq_lock 和 usage lock 不嵌套；tick 不获取 task_list_lock。自测持有存活 worker 所有权到快照结束，再允许其返回并回收，不读取已释放 task 指针。
- 本地启动状态由本 CPU 在 IRQ masked 时写；跨核观察使用 acquire/release。全局 ready/config 同样明确发表，不在 AP 改 period。
- 原 scheduler timer observer：保留全局 aarch64_sched_timer_tick_count 仅由 BSP 写（给现有 probe.S 使用），AP 只更新自身槽；已有 per-CPU frame/switch 观测保持分核。禁止 AP 累加或被 BSP memset 清零仍在写的共享状态。
- `test_aarch64_scheduler.c::irq_worker/case_irq` 原本依靠至多 512 次 yield 推进 vruntime 来制造 masked switch；改用 tick 计费后不能原样保留此 fixture。必须在 rq_lock/IRQ 保护下可控地建立 runnable peer 的选择次序，仍逐项验证 switch/no-switch × IRQ on/off 四种真实执行路径。不得恢复 schedule 计费、放宽断言或只检查 mock 切换；具体 fixture 放入 plan。其余依赖 yield 次数的旧用例同样审计，保留原行为保证。
- AP 周期 tick 会使 WFI 定期返回，这是预期行为，不再承诺只有 SGI 唤醒。原 P2 lost-wakeup 测试的关键窗口须通过 **仅自测** 的本核 CNTP IMASK save/restore 排除 timer 救场；必须有目标核已完成屏蔽的握手，测试后恢复原 CTL、rearm 并 ISB。生产保持 tick 打开；不允许整个 sched-wake variant 永久禁用 AP timer。该 helper 仅存在于 selftest build，不提供生产 stop API。
- 原 time.c 中修改涉及的运行日志迁至 log_*。`[tick] N`、`[cntp] freq=...` 是现有 harness 协议，**普通产品和 selftest 都精确保留既有 BSP 输出**，经 log_info（当前 aarch64 log_impl 支持 %lu 且不加前缀）发表；AP 不输出周期日志，普通产品不新加周期串口输出。旧 `aarch64_uefi_smp.py` 的 smp/no-ack 存活证据不变，不能通过删除 tick 断言绕过。证据输出使用 selftest marker 例外。

## 7. 验收

### Host：执行生产代码，mock 只替换机器边界

新增 test_aarch64_sched_tick，编译真实 sched_tick.c、platform/time.c 和必要的 fair/accounting 生产逻辑；CNTP/GIC/CPU identity/current/IRQ 边界由 preinclude mock 提供，不能重写计费或启动算法。覆盖：

- init/start 幂等与 epoch；未初始化、无 idle、scheduler_ok=0、不在 admission 范围均不能读取 current 或收费。
- BSP 全局 tick 一次，AP 全局 tick 零次；同一 IRQ 只记账一次；N=7 busy 与 N=5 idle 精确增量；deadline 前/到达时置位；pending resched 不被清除；yield 无 tick 不收费。
- mock CNTP 操作顺序、每核启动、失败关闭且 invalid、注册冲突、频率不一致/零/小于100、period 超过 INT32_MAX、PPI 边界 15/16/31/32；重复 local start 无寄存器重写和 epoch 变化。
- production GIC dispatch 的 MMIO 写 spy 验证 PPI 一次完整 raw EOI，spurious 零 EOI。重用现有 gic trace seam，不在测试里模拟一次理想 EOI。
- x86 原始 accounting host suite 和 schedule 计费分支回归；read/usage 锁一致性维持。

### QEMU 产品路径

新增 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick`，专用 selector `AARCH64_SCHED_TICK_TEST=1`，KERNEL_SELFTEST=1，variant `sched-tick`。与旧 sched/AP/M1/sync-fault/no-ack/canary/weak-selftest/OS01_SYSTEST selector 对称互斥；所有递归 make 白名单、help、路径、catalog、scenario 和 CI 同时接线。

runner `qemutests/aarch64_sched_tick.py`：1/2/4/8 核，每种两次，512 MiB，每次固定 120s 总时限（marker 不延长截止时间）。每个 admitted CPU 必须有一份 idle-ready 后的 tick/usage/worker 证据；不允许靠 `[tick] N` 或 BSP 总样本数冒充 AP 工作。独立结果目录，校验实际 ELF、variant、SMP 和 selftest BEGIN/END 一致，拒绝 panic/FATAL、重复/缺失/未知 case、缺 END、非零失败与超时。

至少以下产品 case，SMP1 的 remote case 必须显式 NA 而非伪造通过：

1. 每核 timer 与 accounting started/valid/hz=100；本核 idle delta>=3，runtime/vruntime 不增长。
2. 每核 pinned busy worker 在不 yield 的有界计数器等待期间获得 >=7 次本核 tick：busy delta=runtime delta=vruntime delta，并在 IRQ masked 快照中检查；足够期限未收到 tick 即 FAIL。循环最终主动返回，不能依赖下一期抢占。
3. 关闭 IRQ 后 worker 调用多次 kthread_yield，限定无其它 ready task 的场景，验证真实 schedule 自选路径不增加费用；恢复 IRQ 后仍可继续收到 tick。
4. 本核过 deadline 后 need_resched=1，同时 IRQ switch/nested schedule 增量=0；固定保存的寄存器 probe 在 AP 也验证 IRQ frame 未破坏。
5. 按 CPU 局部 IRQ masked 读数核对总样本=busy+idle，BSP jiffies 增量等于 BSP 全局 tick 次数，AP 全局 tick 次数=0；使用生产边界的分核 selftest observer，不能以墙钟容差替代所有权证明。
6. SGI4 和 TLB SGI3 在周期 timer 打开时保持有效，WFI 路径继续可达；另回归 sched-wake 的 timer-masked 窗口测试。
7. BSP 空 rq、仅有未来时间条件 blocker：idle tick 触发协作扫描并在原用例 <=10 jiffies 窗口内唤醒；BSP idle runtime/vruntime 不收费。原 MODE=sched IRQ 状态用例保留四种 switch/no-switch × IRQ on/off 的实际路径证据。

runner 自测必须通过删 AP record、重复一 CPU、伪造全局计数、busy/runtime 不等、irq_switches>0、错误 variant、缺 END 和持续 tick 超时等负例。Host 故意增加一次计费、让 AP 调用 tick_handler、重复 EOI 的可逆 mutation 必须被拒绝。

### 最终门禁（实施时运行，本次文档工作不运行）

`make PROFILE=x86_64-clang test-host`、`test-static`、`test-qemu SUITE=phase-0`、独立 `KERNEL_SELFTEST=1 test-kernel-selftest KERNEL_SELFTEST_SMP=2/8`、`OS01_SYSTEST=1 test-qemu SUITE=systest`；aarch64 `test-static CASE=sched-common-compile`、`test-aarch64-audit`、MODE=smp/no-ack/m3-probe/gic-spi/sync-fault/sched/sched-ap/sched-wake/sched-tick；`make test-harness`。普通两个 profile 产品构建确认测试 selector/observer 不泄漏。每项记录完整命令、退出码与日志路径，未跑不得记 PASS。

## 8. 完成标准与后续

每核真实 tick 能按此契约计费，AP 不推进全局时间；无重复计费、无早期 current、无 IRQ 内切换，原跨核唤醒证明不被周期 tick 掩盖，全部门禁通过并独立评审 approve，才算实施完成。随后单独设计 IRQ 返回抢占及其 preempt/锁约束。

本次仅产出 spec 与 plan 文档：spec 独立 approve 后写 plan，plan 独立 approve 后停止。产品代码、测试代码和构建配置均不在本次编辑范围。
