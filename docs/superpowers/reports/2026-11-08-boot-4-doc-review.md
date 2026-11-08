# BOOT-4 spec / plan 文档评审记录

日期：2026-11-08。基线 `eebecfbf`。本记录仅覆盖文档与独立评审，不表示代码已实施或测试通过。

- [Spec](../specs/2026-11-08-boot-4-stage-contracts-design.md)
- [Plan](../plans/2026-11-08-boot-4-stage-contracts.md)

## Spec 评审

独立 reviewer：`boot4_spec_review`（fresh context，仅提供文档及源码基线；不代写、不编辑）。

| 轮次 | 反馈 / 结论 | 主 agent 修订 |
|---|---|---|
| R1 阅读与反馈 | 无架构阻断；发现“五/六参数”事实笔误；确认ARM softirq先后问题与公共边界方案 | 修正fatal为五参数、rc=int64_t；自查发现kernel errno无EDEADLK，改为已有ELOOP=40；同步环/无进展及矩阵期望 |
| R2 复核 | **APPROVE**；核对修订已落盘；要求plan环诊断指向最早实际环成员 | plan用有界逐节点可达性检查，专测上游依赖环但自身不在环的节点；不以首个pending冒充环成员 |

Reviewer 最终结论：

> APPROVE（仅设计文档，不代表实现验收）。公共钩子拆分保持x86 IRQ/校准顺序及ARM admission、IPI、自测协议；ARM softirq提前且先于timer_init；preflight、有限依赖、失败传播与兼容API可实施；14组新矩阵具备明确验收标准。

## Plan 评审

独立 reviewer：`boot4_plan_review`，与spec reviewer不同，fresh context。

| 轮次 | 反馈 / 结论 | 主 agent 修订 |
|---|---|---|
| R1 | **REQUEST_CHANGES**，1个P2：遗漏受控selector跨子make传播路径；其余设计/实现步骤无阻断 | 加入mk/project.mk及ARM image.mk；明确默认/none、合法值、互斥与CFLAGS冲突检查、OS01_SUBMAKE_ALLOWED/ARGS及IMAGE_VARIANT；dryrun核对双架构实际子make与固定编译宏 |
| R2 复核 | **APPROVE**，P2完整解决，无新增阻断，无需spec回审 | 更新版本及roadmap，保留所有实施checkbox未勾选 |

Reviewer 最终结论：

> APPROVE。已补齐mk/project.mk和ARM image路径、受控子make传播、selector归一化与互斥校验，并要求双架构实际编译宏验证。仅批准文档可执行性，不表示实现或验收完成。

最终交付：spec R2 / plan R2；两个不同独立reviewer分别批准，主agent负责全部文档修订。

## 本轮验证范围

已执行：四份文档的本地Markdown链接与尾随空白检查、spec/plan版本和APPROVE状态核对、5个任务/25个未实施步骤计数、14组矩阵计数与git diff --check，全部通过。本轮未修改源代码，未执行host/kernel/QEMU验收，未提交Git。
