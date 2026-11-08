# BOOT-5 spec / plan 文档评审记录

日期：2026-11-08；源码基线 `f610e665`。只覆盖文档与评审，不表示实施或运行验收。

- [Spec](../specs/2026-11-08-boot-5-scheduler-services-design.md)
- [Plan](../plans/2026-11-08-boot-5-scheduler-services.md)

## Spec

独立 reviewer：`boot5_spec_review`，fresh context，仅提供任务、文档及基线，不参与编辑。

| 轮次 | 反馈 / 复核 | 主 agent 修订 |
|---|---|---|
| R1审阅期间自查 | PID1默认读/etc/inittab，并不保证进入namespace写锁，单看gate不能证明加锁 | 将overlap observer补到namespace及root FAT/ext2实际mutex取得分支；仅采样与release发布，不持锁输出或等待 |
| R2复核 | **APPROVE**，无阻断项 | 冻结VFSruntime先于首次publish、AP可先于BSP调度、UPcreation window及ARM独立handoff契约 |

Reviewer 核验：spawn完成ELF/VFS操作及put node后才publish，BSP之后的net没有namespace操作；提前end与pid0豁免边界相容；target资格重检/统一持锁发布及锁外清理可实施；真实FSmutex观察能够覆盖PID1读inittab。

## Plan

独立 reviewer：`boot5_plan_review`，与spec reviewer不同、fresh context。

| 轮次 | 反馈 / 结论 | 主 agent 修订 |
|---|---|---|
| R1 | **REQUEST_CHANGES**，一个P2：策略stub与结构扫描不能验证真实x86 TSS/idle机制 | 新增mechanisms独立host binary及runtime mock，链接真实机制hook和arch_task_init_platform；断言十TSS参数/更新rsp0、need_resched两分支及schedule后恢复IRQ；补齐文件归属、focused入口、sched.mk注册和GREEN要求 |
| R2复核 | **APPROVE**；P2已解决，无剩余阻断，无需spec回审 | 更新版本与roadmap，保持实施步骤未勾选 |


Reviewer最终结论：APPROVE（仅计划文档）。新增mechanisms独立binary、真实TSS与idle行为断言、focused入口及test-host注册，明确禁止以策略stub或结构扫描代替。计划与已批准spec一致，不代表实现或验收完成。

最终交付：spec R2 / plan R2，两个不同独立reviewer批准，主agent负责全部文档修订。

## 验证范围

已核对四份文档的本地Markdown链接与尾随空白、R2版本/APPROVE状态、5任务/25未实施步骤、6个focused host CASE、8组QEMU矩阵及git diff --check，均通过。本轮没有修改源代码，没有执行host/kernel/QEMU验收，没有Git提交。
