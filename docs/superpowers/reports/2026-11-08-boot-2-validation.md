# BOOT-2 早期中断与时间设施启动顺序 — 实施验证报告

日期：2026-11-08（实施窗口同日，与文档评审同日闭环）
分支：`feature/boot2-early-irq-time`；基准 `4533d015`（master ff，BOOT-1 收尾）；
最终实现 commit：`466963ab` / `f08f9012` / `3604627c` / `bc9c82f9` + 文档提交。
spec：[BOOT-2 R2 设计](../specs/2026-11-08-boot-2-early-irq-time-design.md)
（`boot2_spec_review` APPROVE）；plan：[R2](../plans/2026-11-08-boot-2-early-irq-time.md)
（`boot2_plan_review` APPROVE）。执行方式：inline（executing-plans），账本
`.superpowers/sdd/2026-11-08-boot-2-early-irq-time/progress.md`。

## 1. 契约落地位置（spec §3/§4/§5/§6 → 代码）

| 契约 | 生产代码 | 证据 |
|------|----------|------|
| IRQF_NO_AUTOEN=0x04 masked registration（§5） | `kernel/intr/irq.c`（enable 缺席即 masked；无 enable-then-disable） | `test_boot_irq_registration` 48 断言：NO_AUTOEN 零 enable、无 IRR 锁存（IF=0 模型下也零 pending）；默认 flag 保持原语义 |
| PIC slave cascade（§5） | `kernel/arch/x86_64/intr/pic_8259a.c`（enable 开 master IRQ2；disable 在最后一个 slave 关闭后才 mask cascade；IRQ-save/restore 保护 IMR） | 真实 IMR 端口模型：IRQ8 仅当 slave bit0 ∧ master IRQ2 均开才可投递（真实 registry dispatch，不绕过级联）；其它 slave 存活时 cascade 保持 |
| `_start` 首条 cli（§3.1） | `kernel/arch/x86_64/head.S` | 源码 + 反汇编审计（Task 5 复核）；QEMU 全矩阵正常启动 |
| 控制器 init 不 sti（§3.1） | `pic_8259a.c` 两处 `arch_local_irq_enable` 移除 | `test_boot_irq_context` trace：首个 sti 在 runtime-open 内 |
| phase3→percpu→context→phase4→runtime-open→5/6（§3.2） | `platform/boot.c x86_64_boot_subsystems` 分段 | context host 组 orchestrator trace 断言顺序 + QEMU 串口 stage 标记 |
| percpu 只构造一次、晚期 facade 只验证（§3.1） | `smp/boot.c`（num_cpus 幂等卫兵）+ `arch_boot_percpu` verify-only | context 组 late-facade 断言（无第二次 GS install）；BOOT-1 lifecycle 套件全绿 |
| 私有一次状态机（§6） | 新 `platform/boot_irq.c/.h`（cold→context⇄calibration→runtime；非法转换在任何 sti/GS 读之前 fatal） | context 组 fatal 路径 + unsafe-open QEMU case（唯一 `[boot-2] FATAL stage=context`、sti 前终止、无 runtime-open/BOOT-1 complete/PID1） |
| PIT prepare/start 分离（§5.1） | `driver/pit.c`：`pit_init()` NO_AUTOEN+100Hz 不 unmask；`pit_start()` 一次性；wrapper 返回真实结果 | context 组：prepare/RTC 阶段 PIT 恒 masked，投递零 tick 效果；runtime 首次投递精确 jiffies+1/watchdog+1/need_resched |
| RTC 受控事务（§4） | `platform/rtc_pie.c` 重写：前置检查不碰硬件；注册在前；仅 C.PF 计数、256 样本饱和；原始 TSC + `BOOT2_CALIBRATION_SPIN_LIMIT=50000000u` 双预算；全部出口恢复 A/B/route/LAPIC/IF=0；输出仅成功写入 | `test_rtc_pie_calibration` 7 用例（6 个对旧事务的行为 RED）；force-rtc QEMU：256 PF + ≥256 配对返回 + 窗口内 jiffies 不增；rtc-timeout：pf=0 有限结束 |
| 频率缓存（§4） | `platform/time.c arch_cycle_freq` 首次决定（含失败 0）缓存，之后纯查询 | host 缓存路径两用例（RTC 决策/CPUID 决策均不再触碰 RTC） |
| 有界 LAPIC 校准（§5.2） | `lapic_timer.c`：TSC 窗口纳入同一 50M 预算；反向/零样本拒绝；init 只装 stub，校准移至 phase4 末一次执行 | `test_boot_tick_handoff`：卡死 TSC 在预算内终止（RED：旧无界循环 alarm 击杀）；同 phase 顺序扰动由 context 组覆盖 |
| 原子 BSP handoff（§5.2） | `platform/time.c arch_tick_start`：IRQ-off 区；PIT 未 prepared 即 fatal；LAPIC 失败保持 masked/stopped、PIT 复 unmask；accounting 恰发布一次；入口 IF 恢复 | handoff 组 6 用例（成功保持 PIT masked、失败回退、fatal 零发布、÷2 语义兼容基线） |

## 2. 隔离 QEMU 矩阵（spec §7.2，10 组）

`make test-qemu SUITE=boot-irq-time`：normal 1/2/4/8（普通镜像
`-snapshot`）、force-rtc 2、rtc-timeout 1、bsp-lapic-fail 1/2/4、
unsafe-open 1 — **全部 PASS**（task4-matrix.log；RunArchive 归档于
`build/x86_64-clang/logs/tests/boot-irq-time/`）。隔离变体经单宏
`X86_BOOT_IRQ_TIME_TEST` 派生（kernel/image 变体 + fixture/entry.S
测试尾仅在宏下编译；普通 ELF 无 fixture 符号与 marker，runner 预审
ELF 身份）；普通镜像 hash 在故障构建前取、矩阵后 cmp 不变。

证据要点：force-rtc 256 PF 样本 + ≥256 真实配对返回 + RTC 窗口内
jiffies 不增；bootstrap ≥3 真实 PIT IRQ 及同源同帧返回；tick 记录来自
实际选中源（bsp-lapic-fail 必经 PIT）；unsafe-open 唯一 context fatal、
≥1 s 观察、runner 自停 QEMU。

## 3. 最终门禁（最终文件状态）

命令与返回码：`.superpowers/sdd/2026-11-08-boot-2-early-irq-time/final-gate.log`。
覆盖 plan Task 5 Step 2 全清单：x86 test-host / test-static /
test-harness（含 44 项 boot-irq-time 契约测试）/ boot-irq-time 10 组 /
boot-lifecycle 8 组 / intr-x86 / test-intr-irq-x86 / phase-0 1&2 核 /
network / syscall（`KERNEL_SELFTEST=` 留空，99/99）/ kernel-selftest
SMP=8；ARM smp、no-ack（注入镜像）、m3-probe、sched-ap、sched-wake、
sched-tick。

## 4. RED→GREEN 记录

- registration RED：NO_AUTOEN 未实现时启用位/IRR 锁存断言失败；
  cascade 未管理时 IRQ8 不可投递。
- rtc RED（6/7 用例）：旧事务无 NULL/IF 前置、非 PF 计数、无界等待
  （alarm 击杀）、注册失败留下 LAPIC INIT、无频率缓存。
- handoff RED：无 PIT prepared fatal（直接越权发布）、freq=0 触发
  SIGFPE、卡死 TSC 无界循环（alarm 击杀）。
- parser/runner RED：NotImplementedError stub + 接线缺失下的行为失败
  （非 ImportError-only）。
- 对已正确行为（÷2 语义、PIT 100Hz、默认 auto-enable）按 plan 记录
  兼容性基线 GREEN。

## 5. Rulings（实施中的偏差决策，全文见账本）

- `arch_local_irq_save` 返回完整 RFLAGS：状态卫兵测 IF bit（1<<9），
  不测整字（0x46 曾导致真实 QEMU 上误 fatal）。
- rtc-timeout 的终态即 `[kernel panic] root filesystem unavailable`：
  clocksource inactive 时 ahci 的 deadline 等待拒绝 probe ⇒ 无块设备
  ⇒ 无用户态；parser 钉死「无 tick 记录 + 无 PID1 + 终态行」。spec
  §7.2 的目标是降级链本身；死时钟源下的完整 PCI-rootfs 链路明确不在
  本期范围（BOOT-3～5 边界）。
- fixture 配对用 (phase,source) 序列 token 而非裸帧指针（多个在途
  IRQ 共享帧地址时指针配对丢计数，64/256 实测）；跨期残留计入
  overlap 失败。
- 变体身份 `#if` 将 rtc-timeout 排在 force-rtc 之前（后者蕴含前者）。
- `KERNEL_BOOT2_IRQ_TIME_FLAGS` 在 `ALL_CFLAGS :=` 重定义之后折叠
  （首次放置被覆盖，runner 的 ELF 身份审计将其暴露）。

## 6. 未覆盖范围与遗留边界

- NMI/异常早期处理不在 IF 控制内，本期结论不覆盖（spec §1）。
- 全局 softirq 的 SMP 设计、AP 无本地 timer 的既有行为、用户态抢占
  模型均未改动（spec §1）；BOOT-3 通用失败传播、BOOT-4 阶段枚举、
  BOOT-5 task_init 拆分未实施。
- rtc-timeout 终态依赖 ahci 的 clocksource 前置：若未来 ahci 改为
  clocksource 无关，终态行会消失，parser 需随行为更新（显式、非静默）。
- PIC 级联保持启动期 BSP 用法（不引入完整 SMP PIC 重构）；spurious
  沿用既有安全 handler，未新增固件历史 pending 的通用 drain。
