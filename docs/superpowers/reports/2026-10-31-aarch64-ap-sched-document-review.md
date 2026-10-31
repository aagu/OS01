# aarch64 AP 调度文档评审记录

开始日期：2026-10-31；评审跨至2026-11-01（Asia/Shanghai）。代码基线：`e8412d1e`。

本轮范围为文档编写与独立子 agent 评审。禁止派发实施；文档 APPROVE 不代表代码实现、构建或 QEMU 验收已完成。

## 文档与顺序

1. spec：`../specs/2026-10-31-aarch64-ap-cooperative-scheduler-design.md`
2. P1 plan：`../plans/2026-10-31-aarch64-ap-sched-p1-cooperative.md`
3. P2 plan：`../plans/2026-10-31-aarch64-ap-sched-p2-wakeup.md`

各文档先完成作者自审，再提交独立评审；发现按实际源码验证后修订，再次送审。所有文档现已明确APPROVE。

## Spec

- R1：`spec_review` → **REQUEST_CHANGES**。五项发现均核对源码后接受：P1多核scanner竞争、ready/READY双边握手漏通知、资源表容量、atomic访问遗漏、共享emergency stack。
- 修订：P1仅BSP扫描且跳过AP；P2统一blocker锁协议；ap_work依赖既有ipi_ready前提并无条件kick；按CPU分批带代资源跟踪、溢出失败；P1完成完整atomic审计；fatal栈前无栈争抢单owner。另冻结accepted-mask getter。没有实施代码。
- R2：`spec_review` → **APPROVE**。五项阻塞发现均已解决，可编写P1/P2。附带落实项：scanner的t->cpu读取与schedule重复写也须遵守一致atomic/锁合同，已纳入P1 T1。
- P1编写时将既定双CPU fatal测试具体化为fatal-owner selector/独立变体；该小幅delta随P1送审。

## P1 plan

- R1：`p1_review` → **REQUEST_CHANGES**。六项发现接受：释放前hook不能证实回收完成、批次/实例归属未定、无预构建harness不可依赖ELF、T3/T4依赖T5顺序环、fatal缺第二CPU参与证据、UP/多核schema不一致。
- 修订：free_begin/token→kfree→free_complete，单活动批次/单调实例token，旧1000任务分4批保留；mock unittest与真实ELF审计分离；必要variant/runner接线随T3/T4交付；fatal强制双到达/单败者；完整schema按CPU数钉死。资源/fatal细化同步更新spec。
- R2：六项修订获确认，评审者要求补分配入口确切签名；已补原resource hook仅允许alloc=true，旧false调用一律替换token协议。
- R3：`p1_review` → **APPROVE**，含同步spec的资源/fatal/selector细化。批准仅覆盖文档可实施性。

## P2 plan

- R1：`p2_review` → **REQUEST_CHANGES**。四项发现接受：旧late probe固定seq冲突、P1拒绝测试需升级、hook接口/修改边界不全、路径基准混用。
- 修订：共存直接走真实tlb_shootdown及ACK代次，不调用旧late work probe；AP初始true blocker验证成功；列全wake-only hook签名/调用点/锁与IRQ限制/超时行为及相关TU；统一kernel-relative展开规则。另冻结idle_wait.S硬件边界，使host mock不需猜测内联汇编替换方式。
- R2：`p2_review` → **APPROVE**。四项发现全部解决，额外idle硬件等待边界亦获确认。

## 最终结论

| 文档 | 独立评审者 | 最终结论 |
|---|---|---|
| Spec | spec_review；细化由p1_review确认 | **APPROVE** |
| P1 plan | p1_review | **APPROVE** |
| P2 plan | p2_review | **APPROVE** |

已停止在文档交付阶段；没有派发实施agent，没有修改产品/测试/构建代码，没有运行产品验收。后续执行者为用户指定的Luna，需另行明确实施授权；先执行P1，产品验收后再核对P2。

文档校验：本地Markdown链接目标存在，代码围栏成对；最终文件摘要如下（不含本报告自身，避免自引用摘要）。批准后的更改仅为最终状态说明，不改变设计和实施合同。

收尾时检测到并行出现的提交 `ddca8b8d`，仅更新docs/roadmap.md与docs/changelog.md；产品源码相对评审基线e8412d1e无变化，故保留原评审基线。未覆盖该提交或修改这两个文档。

| 文件 | SHA256 |
|---|---|
| 2026-10-31-aarch64-ap-cooperative-scheduler-design.md | `6ea5186f93cc0c9f55ada89319070bf07e5d7d1c22f66f2213bc5230bdcc375e` |
| 2026-10-31-aarch64-ap-sched-p1-cooperative.md | `a1c3f4129e490fdbcbeae0fb52d009c0af6a996da7fb9778ac6479a6b72ca6c5` |
| 2026-10-31-aarch64-ap-sched-p2-wakeup.md | `ea781e4e6f1dd62c2d9838332ad446078588594a8afc3ffd1152f254b1d1904a` |
