# 每核 tick 与调度记账文档评审记录

日期：2026-11-06；基线 `ca0e43ad`。本次只写文档，不实施、不运行实施门禁、不提交产品变更。

## Spec

文档：[设计](../specs/2026-11-06-aarch64-percpu-tick-accounting-design.md)。独立只读子 agent：`/root/spec_review`；主 agent 负责写作与全部修订。

### R1 — REQUEST_CHANGES

1. BSP idle tick 不置 need_resched 会使空 rq 的到期 blocker 永不扫描。代码依据：idle.c 仅 need/rq 工作时 schedule，scanner 位于 schedule 内。修订：有效 BSP tick（含 idle）保留置位，AP idle 不无条件置位；新增空 rq blocker <=10 jiffies 唤醒及 idle 零收费验收。
2. 原 irq_worker masked-switch fixture 依赖 yield 计费，移除后无法可靠制造切换。修订：rq_lock 保护下建立可控 peer 选择顺序，仍要求 switch/no-switch × IRQ on/off 四项真实证据，不恢复计费、不放宽断言；plan 展开 fixture。

同时补足 init/admission/start 失败路径不得碰未初始化锁或早期 current。

### R2 — APPROVE

Reviewer 原文结论：**“APPROVE（R2）。”** 两项阻塞均确认解决；全文未发现其他阻塞项。具体 fixture、observer reset 握手和构建接线细节交由 plan 展开。

### R3 — APPROVE

Plan 第一轮核对真实 runner 后发现：普通 smp/no-ack 要求至少三条精确 `[tick] N`，不能把既有协议限制到 selftest 构建。修订：普通产品和 selftest 均保留 BSP 既有 `[cntp]`/每秒 `[tick]`，通过 log_info 发表，AP 不新加周期输出；不删除旧 runner 存活断言。并修正最终 kernel-selftest 核数参数为实际生效的 KERNEL_SELFTEST_SMP。原 spec reviewer 已复审，明确 **“APPROVE（R3）。”**，确认旧 runner 与 log_impl 的协议兼容、核数参数真实生效，R2 契约完整无新增阻塞。

## Plan

文档：[计划](../plans/2026-11-06-aarch64-percpu-tick-accounting.md)。另一位独立只读子 agent：`/root/plan_review`；主 agent 负责写作与修订。

### R1 — REQUEST_CHANGES

1. Task1 的 selftest-only tick 标记与 Task5 普通产品 smp/no-ack 回归冲突。修订：保留既有 BSP 协议，经 log_info 输出；同步 spec R3 并交原 reviewer 复审。
2. Task5 `SMP=2/8` 不控制 kernel-selftest；实际 runner 使用 KERNEL_SELFTEST_SMP，默认4。修订：显式使用 KERNEL_SELFTEST_SMP=2 和 =8。

采纳非阻塞澄清：peer退出后停止使用指针、靠独立reap barrier确认回收；每核测试批次串行；global observer/accounting取同一前后快照窗口。

### R2 — APPROVE

Reviewer 原文结论：**“APPROVE（R2）。”** 两项阻塞均确认解决；peer生命周期、批次串行和同窗口计数澄清与spec R3一致，未发现新增阻塞项。

## 最终状态

Spec R3 APPROVE；Plan R2 APPROVE。主 agent 按用户要求在此停止。只新增本任务三份文档，未修改产品/测试/构建代码，未执行实施门禁，未创建实现分支或提交。已检查三份文档本地链接、代码围栏及未执行checkbox；工作区其它并行文档改动不属于本任务，未触碰。
