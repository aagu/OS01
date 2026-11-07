# BOOT-2 spec / plan 文档评审记录

日期：2026-11-08。代码基准：`87ceffa7`；本次编写开始于 `b38b6ca1`，期间 BOOT-1 在其他工作中完成收尾提交。本次只编写文档，未实施代码、未运行构建或 QEMU。

## 最终结论

| 文档 | 版本 | 独立 reviewer | 结论 |
|------|------|---------------|------|
| [早期中断与时间设施启动顺序 spec](../specs/2026-11-08-boot-2-early-irq-time-design.md) | R2 | `boot2_spec_review` | **APPROVE** |
| [BOOT-2 实施 plan](../plans/2026-11-08-boot-2-early-irq-time.md) | R2 | `boot2_plan_review` | **APPROVE** |

两名 reviewer 均为无会话历史的独立子 agent，只读文档并通过 CodeGraph/实际源码核对实现、硬件依赖及构建/测试入口。主 agent 编写、修订并送同一 reviewer 复核；spec 获批之后才编写 plan。批准仅表示文档可供实施，不代表生产问题已修复或验收通过。

## Spec：两轮评审

R1 结论 **REQUEST_CHANGES**，共三项：

| 严重度 | 问题 | 主 agent 修订 |
|--------|------|----------------|
| P1 | register_irq 自动 enable 后再 mask，IF=0仍可能把 PIT 边沿送进 LAPIC IRR；RTC窗口可能提前触发tick/timer-list | 增加内部 IRQF_NO_AUTOEN=0x04；PIT/RTC从首次安装起masked，旧调用者保持auto-enable；host在任意enable时锁存pending |
| P1 | PIC slave IRQ8解屏蔽不足以接收RTC，master IRQ2仍masked | 明确slave enable/disable的cascade契约、保持其它slave使用者与IF；host模型真实检查master/slave双mask |
| P2 | RTC把alarm/update/旧pending当周期样本 | 事务先关闭PIE/AIE/UIE、配置A/清C，再只开PIE；handler仅计C.PF，256饱和；补C=0/AF/UF和257th负例 |

R2 reviewer明确 **APPROVE**：三项均解决，没有新增阻塞问题。评审同时确认early TSS/TR已在task_arch建立、BOOT-1 scheduler_ok门和accounting门存在；LAPIC calibration移到phase4末可消除clocksource同phase顺序依赖。没有以本期文档宣称早期窗口已动态复现。

## Plan：两轮评审

R1 结论 **REQUEST_CHANGES**，共四项：

| 严重度 | 问题 | 主 agent 修订 |
|--------|------|----------------|
| P1 | 无参return总计数可被syscall/#PF/AP或其它来源补足，无法证明目标IRQ完成返回 | handler记录BSP/source/实际frame/phase token；return hook传实际pt_regs，仅配对消费；按阶段取差值，明确测试专用汇编参数、栈对齐和恢复 |
| P2 | 新flag在实现步骤才声明，先执行测试会编译失败，拿不到承诺的行为RED | Step1先只声明常量、保留旧registry行为；独立registration CASE先取得enable/cascade断言RED |
| P2 | 沿用IRQ_CHAIN的测试stub了tick_handler，缺少spec要求的真实tick链 | 明列真实PIT/tick/timer/softirq/registry/route/dispatch对象与poison/精确增量断言；汇编返回另由ELF/QEMU验证 |
| P2 | handoff后的tick快照不能证明phase5前bootstrap PIT已工作 | runtime-open后、phase5前独立等待PIT handler/配对返回/jiffies至少3次；新增bootstrap及devices-begin记录和严格次序负例；handoff另取基线 |

R2 reviewer明确 **APPROVE**：四项均解决，未发现新契约冲突。

## 文档检查与实施边界

主 agent 检查本地链接、spec/plan互引与版本状态、5个任务/25个未完成步骤、NO_AUTOEN与RTC/PIC契约、真实IRQ token与bootstrap证据、roadmap状态和 `git diff --check`。本轮未执行未来实施步骤，也未复跑BOOT-1验证报告中的门禁。

BOOT-2仍待实施。后续必须按plan补真实host RED/GREEN、10组QEMU、BOOT-1及双架构回归，并另写实施验证报告。BOOT-3～5没有随本次评审获批。
