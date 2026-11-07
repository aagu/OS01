# BOOT-1：CPU 生命周期与 SMP 启动握手设计

日期：2026-11-07  
状态：R2，子 agent `boot1_spec_review` 已 APPROVE；未实施。评审过程见[文档评审记录](../reports/2026-11-07-boot-1-doc-review.md)。  
基准：`64d23013`；依据：[启动分析报告](../reports/2026-11-07-boot-sequence-analysis.md) §4 F1、[roadmap](../../roadmap.md) BOOT-1。

## 1. 目标与范围

消除“构造 percpu 数据即表示 CPU 已运行”的歧义，使 x86 BSP 只能通过 AP 自己发布的状态确认启动；在修复中保持 aarch64 的启动 ACK、IPI 服务与调度 admission 三种资格的区别。

本 spec 交付：公共 percpu 构造契约、双架构 online 发布点、并发读取规则、x86 两阶段启动等待及资源安全、针对性 host/QEMU 证据。BOOT-2 的早期 IRQ 放行、BOOT-3 的通用失败传播、BOOT-4 的公共启动阶段编排、BOOT-5 的 task_init 拆分不在范围内。

不增加统一 `arch_smp_boot_aps` API，不实现 CPU hotplug/offline，不改变拓扑排序、CPU 编号、APIC/MPIDR 映射、页表协议或用户 ABI；不重写 aarch64 PSCI 状态机和 admission 机制。

## 2. 已确认事实与尚未验证的影响

- `kernel/percpu/percpu.c::percpu_init()` 对所有被构造的 CPU 提前写 `online=1`；x86 BSP 在发 SIPI 之前就构造全部 enabled CPU。
- `kernel/arch/x86_64/smp/smp.c::smp_boot_aps()` 当前用同一 online 字段等 AP 响应，随后复用单份 trampoline data 启动下一个 AP。
- AP 的真实路径是本地 LAPIC/描述符表/TSS/idle 准备 → online → TSC 握手 → IRQ 放行 → ipi_ready → scheduler_ok。
- x86 等待失败当前会移除 idle 并释放其栈/线程；没有协议证明迟到 AP 不会继续使用它们。
- aarch64 有独立 `aarch64_boot_percpu_t.online/go` 启动协议；运行期 `percpu_t` 和冻结的 `aarch64_smp_accepted_mask()` 是另外两层状态。
- aarch64 未 ACK/迟到 AP 可以留在工作循环，IRQ/IPI 就绪后仍可参与共享页表 shootdown，但不得跨越冻结的调度 admission。

提前满足 x86 online 条件是源码可推导的协议错误。实际启动误判、迟到 AP 与资源回收的动态影响尚未复现；实施必须先记录原始基准并用针对性测试建立证据，不能将正常 boot 成功当作不存在窗口的证据。

## 3. 方案取舍

| 方案 | 取舍 |
|------|------|
| 只改 x86 等待另一个临时标志 | 改动小，但保留 online 被构造者提前发布的误导语义；其他消费者继续承担隐含约束 |
| 保留现有能力字段，构造与目标 CPU 发布分离 | 不改变 percpu 布局；已有 ipi_ready/scheduler_ok 仍承担各自语义；双架构发布点显式可审计 |
| 新增跨架构生命周期枚举、统一 AP transport 和降级接纳协议 | 范围远超 BOOT-1，还会侵入 aarch64 已验收的 PSCI/admission/cache 发布协议 |

本设计选择第二种。x86 在已发启动请求后的超时采用 fail-closed，不尝试安全性尚无证明的降级回收。此为设计者建议的行为取舍：以明确启动失败替代潜在迟到 CPU 使用已回收内存；不要求 aarch64 改掉现有 DEGRADED 行为。

## 4. 状态含义与不变量

不新增结构字段。`prepared` 是构造完成的概念条件，不另设持久布尔值；`num_cpus` 保持已发现逻辑 CPU 的遍历上界，不能解释为在线计数，不因启动失败压缩编号。

| 状态 | 含义 | 发布方 / 消费方 |
|------|------|----------------|
| prepared | self、CPU identity、runqueue/锁及启动需要的数据构造完毕 | `percpu_init` 调用完成；仅由启动代码依照调用顺序使用 |
| runtime online | 对应 CPU 已实际执行到定义的本地初始化完成点；不保证 IRQ/IPI 或任务调度可用 | BSP 为自身发布；AP 只能由目标 AP 发布；远端以 acquire 读取 |
| ipi_ready | 目标 CPU 的 IPI handler、路由及 IRQ 条件已满足 | 维持现有架构发布函数；shootdown 使用 online ∧ ipi_ready ∧ 非自身 |
| scheduler_ok | idle/执行栈和架构切换条件就绪，可以参与调度 | 维持现有 release 发布；调度使用 online ∧ scheduler_ok，并保留架构 admission |
| aarch64 boot ACK | AP 完成早期启动协议并向 BSP 报告 | boot slot 的 online；不能改成 runtime online 的别名 |
| aarch64 accepted mask | BSP 冻结的启动 ACK 接纳集合 | 仅按现有 PSCI 协议建立；调度不能因迟到 runtime online/ipi_ready 扩大此集合 |

必须满足：

1. `percpu_init()` 只准备数据，返回后 online=0、scheduler_ok=0；保留现有末尾 8 字节的初始化机制，不改 `ipi_ready/tlb_ack_gen` 偏移和一次发布协议。调用者仅在目标 CPU 尚未发布、且不存在并发 runtime 状态读者时构造一次，禁止运行期重初始化。x86 由 BSP 在发布 num_cpus 前构造全部发现 CPU；aarch64 保留 AP 自构造及下述既有读者启动 gate。
2. `sizeof(percpu_t)=144`、GS self/need_resched/CPU ID 偏移、末尾字段与汇编访问保持不变；不调整 UAPI/boot_context。
3. online 用 `__atomic_store_n(..., 1, __ATOMIC_RELEASE)` 发布；跨 CPU 读取用 `__atomic_load_n(..., __ATOMIC_ACQUIRE)`。不混用 plain load/store 和原子访问同一并发状态。
4. `ipi_ready=1 ⇒ online=1`；`scheduler_ok=1 ⇒ online=1`。aarch64 admitted AP 还必须在 accepted mask 中；BOOT-1 不为 runtime online 加 accepted 限制。
5. 已发布状态只从 0 到 1，不被 BSP 重置；没有 hotplug 或重试本轮 AP 启动协议。
6. x86 下一 AP 的 trampoline data 不得在当前 AP scheduler_ok 被 acquire 观察到之前覆盖。
7. 启动失败后不得发布整体成功、进入 task_init 或启动 PID 1；已可能被 AP 使用的栈、线程、task list 节点、TSS 和 trampoline data 均保持有效。

## 5. 双架构发布点

### 5.1 BSP

x86 `x86_64_boot_percpu()` 在 CPU0 的 percpu 数据、tss/tss_hw 和 GS 安装完成后 release 发布 CPU0 online。其余 CPU 只构造，online 保持 0。拓扑已将第一个条目当 BSP 的现有约定不在本期改造。

aarch64 `arch_boot_percpu()` 维持 install pointer → percpu_init → runtime online → num_cpus → smp_starting_enter 的现有顺序，将显式 online 发布保留为唯一 BSP 发布点。

### 5.2 x86 AP

保持原有准备步骤和 tick/accounting 归属。完成 LAPIC、GDT/IDT、selectors、idle/thread、TSS/TR、NMI 路由和本地 tick 尝试之后，AP release 发布 online。此发布发生在 TSC 握手之前，避免 BSP/AP 互等。

TSC 采样完成或按现有策略超时后，AP 开 IRQ，调用现有 `ipi_ready_publish_ap()`，最后 release 发布 scheduler_ok。IPI 发布函数应断言已 online；scheduler_ok 之前 idle 栈/TSS 必须有效。AP tick 启动失败的既有 accounting 行为保持，本期不宣称此失败模式已解决。

### 5.3 aarch64 AP

`secondary_idle()` 在 gic_cpu_init、runtime percpu 初始化及页表 AP 验证通过后，boot ACK 之前，显式 release 发布 runtime online。NO_ACK 注入仅抑制 boot ACK，不能抑制 runtime online，也不能强行使 AP 获得 scheduler_ok。

保留 AP 自构造的并发前提：当前多核 `arch_boot_aps()` 无条件执行 `aarch64_shootdown_probe()`，probe 在读取 runtime online/开始页表变更之前，等待 ipi_ready_count 达到 dtb_cpu_count；各 AP 的构造先于它们各自 IPI 发布。调度/admission 初始化在 arch_boot_aps 返回之后，单核不涉及 AP。这条 gate 是现有“无并发读者时 memset”的证明，不是本期发现的现有 race。实施须审计 probe 前 tick/其他路径不读远端 online，并补契约测试；未来移除 probe 时必须保留等价的生产就绪屏障，不能直接删除这个依赖。为消除这个隐含 gate 而将全部 ARM percpu 构造迁至 BSP 属于后续阶段编排选择，本期不做。

保持 boot slot cache clean、PSCI 返回码、2 秒 ACK deadline、frozen accepted mask、go command、GIC target、IRQ + ISB → ipi_ready_count、AP work loop 和调度 release gate 的现有顺序。禁止以 accepted mask 代替 shootdown 的 runtime online ∧ ipi_ready；未接纳但 IPI 已就绪的 AP 仍须刷新共享映射。

## 6. x86 BSP 两阶段等待与失败处理

每个目标按以下顺序串行启动：

1. 分配/构造 idle、thread 和 TSS，关联 percpu、task list，填充 trampoline data。online/scheduler_ok 必须仍为 0；遇到残留已发布状态视为启动协议错误，不清零掩盖它。
2. 按现有 INIT-SIPI-SIPI 顺序发送请求；不改变硬件延迟或向量。
3. 有界 acquire 等待目标 online；禁止将 prepared 或 topology count 当作 ACK。
4. BSP release 写 tsc_sync_go；AP acquire 读，AP release 写 tsc_sampled，BSP acquire 读；tsc_boot 在 sampled 发布之前写。采样超时仍按现有策略 offset=0 并告警，不是 CPU 启动成功证据。
5. 有界 acquire 等待 scheduler_ok；观察到之后必须同时满足 online、ipi_ready。再报告该 AP 启动完成，才可覆盖 trampoline data 启动下一个 CPU。

两个必需等待均使用 `X86_SMP_READY_SPIN_LIMIT=5000000u`；TSC 的 `SYNC_SPIN_LIMIT=1000000u` 保留。它们是有限轮询预算，不承诺毫秒/秒；日志不能称为 2s deadline。循环恰好最多检查 budget 次，每次先 acquire 检查再 pause；budget=0 不检查并返回失败；最后一次 pause 后的发布不算成功。

idle/thread 分配失败、残留状态、online 超时、scheduler_ok 超时、就绪条件矛盾：记录 `[boot-1] FATAL cpu=<id> stage=<prepare|online|scheduler|invariant>` 并进入现有 x86 fatal 路径，BSP 不继续启动。已发 SIPI 后不得 free/list_del/reuse 目标资源；不保证其他 AP 被全局停止，但它们只访问仍有效的内存。上线超时不再走原有“释放后继续”降级分支。分配失败前尚未发布给 AP 的局部内存可清理后 fatal。

全部发现 CPU 均满足对应启动契约后，BSP 仅发一次 `[boot-1] complete discovered=<N> ready=<N>`；N 包括 BSP，单核 N=1 时无需 AP ready 行。ready/complete 是启动能力观测，不替代已有 kernel-selftest 协议。

## 7. 消费者与内存序审计

审计生产代码的 runtime online 全部读取：`sched/balance.c`、`sched/kthread.c`、`memory/tlb.c`、架构 IPI/通知路径及诊断读取；并检索 selftest 的在线资格选择。改为 acquire，保持现有 scheduler_ok/ipi_ready acquire 及 allowed_cpus/admission 条件。普通拓扑遍历仍使用 num_cpus，不改为压缩的 online 数量。

`percpu_init()` 的 memset 必须发生在状态发布之前；不能用 volatile 或现有 `__sync_synchronize()` 加 plain 字段访问替代 release/acquire。x86 TSC 的 go/sampled 也纳入同一生产者/消费者审计。aarch64 boot slot 协议沿用原有 acquire/release API。

## 8. 验证设计

### 8.1 Host（执行生产逻辑）

- 链接真实 percpu.c：构造 CPU1 后 online/scheduler_ok 为 0，identity/self/runqueue lock 有效；不会被启动等待当成在线。保留布局与末尾字段检查。
- x86 有界等待抽成 per-arch 小函数供真实 smp_boot_aps 调用，host 仅替换 pause/硬件设施。覆盖初始 ready、延迟发布、一直未发布、零预算和最后检查边界；禁止只测试重复实现的状态机。
- 扩展现有 TLB production-chain host 测试：offline+ipi_ready 不入目标，online+未 ipi_ready 不入目标，online+ipi_ready 入目标，即使 scheduler_ok=0。后者对应 aarch64 未接纳但仍服务 IPI 的 CPU。
- aarch64 原有 boot ACK/accepted mask/no-ack 测试保留；补 runtime online 显式发布顺序和所有 runtime online 并发访问审计。源码顺序检查只作辅助，不能代替实际 AP 执行证据。

### 8.2 x86 隔离 QEMU 场景

新增 `test-qemu SUITE=boot-lifecycle`，统一运行 normal、delay-online、no-online、no-scheduler 场景。故障只影响 CPU1；delay-online 在 AP 的真实 online 发布前 pause 100000 次，不改 BSP budget；no-online 永久停在发布前；no-scheduler 在 online/TSC/IPI 完成后停在 scheduler_ok 发布前。fault 镜像设置 KERNEL_SELFTEST=0，使用独立 kernel/image 变体与编译旗，不覆盖普通镜像或借环境变量注入运行期状态。

normal 跑 1/2/4/8 CPU，delay-online 跑 2/4 CPU；两个 fatal 场景各跑 2 CPU。普通生产路径由 BSP 在两阶段完成后发 log_info `[boot-1] ready cpu=<id> online=1 ipi_ready=1 scheduler_ok=1`，该行作为完成边界，不以 AP 串口输出排序代替 acquire 事实。

延迟场景需有编译旗身份与 AP delay 标记，并由 host 证明等待确实观测发布前状态；不要求 real QEMU 在固定轮数内一定被抢占到等待循环。fatal 场景必须看到唯一匹配 FATAL、至少 1 秒后观察窗口、无 CPU1 ready/后续 CPU 启动/用户 init/整体成功；runner 主动终止 QEMU。超时、静默、QEMU 自行退出均失败，不能被视为预期 fatal。

runner 使用现有 ProcessSession/RunArchive/RunReport，归档 profile、case、CPU 数、变体 ELF/镜像身份和串口证据。合成日志测试覆盖缺失、重复、乱序、截断标记及 fatal 后继续执行。正常镜像构建前后 hash 不变，正常 ELF 不含 fault 选择符或 fault marker 字符串。

### 8.3 aarch64 与既有回归

正常 SMP 1/2/4 核、no-ack CPU1、m3-probe、sched-ap、sched-wake、sched-tick；保留固件 diagnostic-DTB 的现有 harness 约定。no-ack 场景必须保持 DEGRADED 和冻结 accepted 集合，并证明 runtime online/IPI 参与不等于调度接纳。

x86 保留 phase-0 单/双核、网络、PID 1 与 kernel selftest（含实际 TLB IPI 路径）；syscall E2E 用顶层 `OS01_SYSTEST=1`，与 KERNEL_SELFTEST 分开运行。静态 layout 和双架构编译必须通过。BOOT-2 早期 IRQ 的未解决风险单列，不因本期正常启动通过声称已解决。

## 9. 交付与完成条件

设计与 plan、生产代码、host/QEMU 场景和 harness 元数据、SMP/percpu 文档与验证报告一起交付。每个行为变更先有 RED 证据、后有 GREEN；完整故障注入若受环境阻塞则保留记录，不能宣布 BOOT-1 完成。

本轮文档审核通过只表示设计/计划可供实施，不代表生产修复已完成。后续实施报告记录命令、日志路径、返回码与未覆盖边界；roadmap 的完成状态仅在实际验收后更新。
