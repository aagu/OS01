# BOOT-1 spec / plan 文档评审记录

日期：2026-11-07  
代码基准：`64d23013`。本次只编写文档；无生产代码修改、构建或 QEMU 验收。

## 交付与最终结论

| 文档 | 最终版本 | 独立 reviewer | 结论 |
|------|----------|---------------|------|
| [CPU 生命周期与 SMP 启动握手 spec](../specs/2026-11-07-boot-1-cpu-lifecycle-design.md) | R2 | `boot1_spec_review` | **APPROVE** |
| [BOOT-1 实施 plan](../plans/2026-11-07-boot-1-cpu-lifecycle.md) | R2 | `boot1_plan_review` | **APPROVE** |

两名 reviewer 均使用无会话历史的独立子 agent；只读评审文档并按需核对 CodeGraph、生产源码和构建/测试入口。主 agent 负责写作、处理反馈和修订，再由 reviewer 复核。批准表示文档可用于后续实施，不表示 BOOT-1 已修复或测试已通过。

## Spec 评审与修订

1. R1 送审：核对 x86 percpu 构造、AP online/TSC/IPI/scheduler 握手、trampoline 复用和超时资源保留；核对 ARM boot ACK、runtime online、accepted mask 与 shootdown 的区别。
2. reviewer 认为协议整体可批准，建议准确描述 percpu 初始化前提。主 agent 将“CPU 尚未运行”改为“runtime 状态未发布且不存在并发 runtime 读者”，补充 ARM 现有强制 probe 等待全部 ipi_ready_count 的排他证明，以及未来移除 probe 必须保留等价屏障的要求。审查中对迟到 AP memset 的初始并发疑虑已被现有 gate 排除，不记录为已证实缺陷，也未据此扩大为 BSP 统一预构造工程。
3. reviewer 重读 R2 后明确 **APPROVE**，确认构造前提与 gate 证明已解决措辞问题，没有剩余阻断项。
4. 主 agent 将整体成功观测格式具体化为 `[boot-1] complete discovered=<N> ready=<N>`（包含 BSP，单核无需 AP ready 行）；reviewer 再次确认 **APPROVE**，认为仅明确观测格式，未改变协议。

保留的关键设计裁决：5000000 是轮询预算而非墙钟秒数；x86 超时停止 BSP 并保留已暴露资源；不要求停止所有 AP 才能保证资源有效；ARM 未接纳但 IPI 已就绪的 CPU 仍参与共享页表 shootdown。

## Plan 评审与修订

1. 在 spec 获批后编写 R1，拆为五个任务：构造/发布与握手、并发消费者、x86 故障矩阵、ARM 资格验证、最终验收与文档。
2. `boot1_plan_review` 第一轮结论 **REQUEST_CHANGES**，指出一个 P2 可执行性问题：construction 与尚无生产实现的 wait binary 若共同作为聚合目标 prerequisite，构建会在 recipe 前失败，无法取得计划要求的真实构造行为 RED。
3. 主 agent 修订 R2：根入口明确 `test-boot-lifecycle-host CASE=construction|wait|all`；construction 不依赖 wait；先保存真实 percpu.c 的 online=1 断言 RED，再单列接口缺失证据，最终 all 验证 GREEN；写明 root CASE 校验及 host 子目标分派。
4. reviewer 复读后明确 **APPROVE**，确认 P2 已解决，未发现其他阻断问题。

主 agent 另在自检中将 ARM probe 的泛称成功标记改为真实 `SHOOTDOWN-PROBE: OK`，并告知 reviewer；最终批准针对这一精确格式。

## 本轮检查与后续边界

文档检查覆盖：Markdown 本地链接、spec/plan 互引、五个实施任务与未勾选步骤、构造/等待独立 RED 路径、roadmap 状态及 `git diff --check`。这些检查只证明文档一致性。

生产实现未开始，plan 全部步骤保持未完成。后续实施需重新核对工作区和 AGENTS.md，执行设计要求的正常/失败矩阵，并另写验证报告；只有实际验收通过后才关闭 roadmap 的 BOOT-1。BOOT-2～5 未随本次评审获批，也未开始实施。
