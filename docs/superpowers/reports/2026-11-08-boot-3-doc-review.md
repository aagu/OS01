# BOOT-3 spec / plan 文档评审记录

日期：2026-11-08；代码基准：`1212913c`。本次只编写和评审文档，未修改生产代码、未运行构建或 QEMU，也未复跑既有 BOOT-1/2 验收。

## 最终结论

| 文档 | 最终版本 | 独立 reviewer | 结论 |
|------|----------|---------------|------|
| [启动失败传播与必需能力检查 spec](../specs/2026-11-08-boot-3-failure-policy-design.md) | R3 | `boot3_spec_review` | **APPROVE** |
| [BOOT-3 实施 plan](../plans/2026-11-08-boot-3-failure-policy.md) | R2 | `boot3_plan_review` | **APPROVE** |

两个 reviewer 均为无会话历史的独立子 agent，只读文档并按 CodeGraph 要求核对真实实现与测试/构建入口。主 agent 写作、修订并送原 reviewer 复核。spec R2 获批后编写 plan；plan 评审发现的证据措辞问题同步回修 spec R3，两个 reviewer 分别确认最终版本。

## Spec：三轮评审

R1 **REQUEST_CHANGES**：

| 严重度 | 问题 | 主 agent 修订 |
|--------|------|----------------|
| P1 | spawn_user_task未检查files_dup失败，仍可能发布无标准fd的PID1 | 限定补充stdio继承屏障：parent有files而dup失败必须在publish前清理并返回-ENOMEM；新增真实exec.c host验证和child-fds单/双核QEMU，矩阵由15组增为17组 |
| P2 | ARM真实logger不支持%d/%ld，无法输出约定signed字段 | boot failure单元固定栈缓冲预格式化signed字段，仅用%s交给log_*；使用unsigned幅值处理INT64_MIN；host编译真实ARM formatter捕获完整fatal/warn行 |

R2 **APPROVE**：两项阻碍已解决，无新增阻碍。保留可选硬件降级、partial节点资源保留、私有files引用回滚和成功spawn已发布的边界。

R3 **APPROVE**：在plan评审后明确§8.2的normal/fault证据分流；normal不含故障fixture身份/注入/能力哨兵，只要求生产成功链及ELF/image hash归档；故障变体要求variant/injected，optional还要求失败状态与后续能力哨兵。17组验收条件保持可实施。

## Plan：两轮评审

R1 **REQUEST_CHANGES**，一项P2：Task4同时要求普通镜像不编入fixture，又要求“所有case唯一身份/注入证据”，normal三组无法兼容两项约束。

主 agent 修订 R2：

- normal仅使用生产init-created、PID1/shell成功链与归档ELF/image hash；出现fault身份、注入或fixture能力哨兵即拒绝。
- 故障变体要求唯一variant/injected；fatal检查component/rc/阶段停止规则。
- optional额外要求真实status/raw结果、后哨兵以及root/terminal/stdio能力链。
- parser正负例与spec同步，不再对normal要求不存在的fixture证据。

R2 reviewer明确 **APPROVE**，无剩余阻断项。独立评审还核对了：旧API行为RED可独立执行；percpu/latch/query接口；真实ARM logger链接；TTY partial对象保留；真实file/exec/task_init测试边界；17组数量及SMP前后不同的禁止标记。

## 文档检查与实施边界

检查范围：本地链接与互引、spec R3/plan R2批准状态、5个任务/25个未完成步骤、files_dup发布屏障、ARM signed日志边界、normal/fault分流、roadmap状态及 `git diff --check`。这些仅证明文档一致性。

BOOT-3仍待实施；后续需保存真实host RED/GREEN、17组QEMU和双架构回归，并另写验证报告。现有BOOT-1/2验收不得替代本期测试。BOOT-4/5未随本次获批，未自动开始任何实现或Git提交。
