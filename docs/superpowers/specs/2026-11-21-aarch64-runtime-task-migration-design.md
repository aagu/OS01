# aarch64 运行期跨核任务迁移 Spec

关联 [roadmap 的 P2 aarch64 适配](../../roadmap.md#-p2-aarch64-适配)与[当前用户态能力](../../arch.md)。本项完成后，普通 aarch64 EL0 进程可在已经执行过用户代码后，由调度器从一个就绪 CPU 的运行队列迁入另一个就绪 CPU，再从原用户帧继续执行。负载均衡在产品构建中生效。进程的 PID、内核栈、私有 mm、用户寄存器和 VM 授权语义随同一个任务延续。

## 范围与方案

- 运行期 VFS spawn 与 fork 保留轮转初始放置，亲和掩码取放置时的就绪 CPU 集合。集合来自现有 DTB/SMP admission，CPU 须同时满足 `online`、`ipi_ready`、`scheduler_ok`。初始目标必须属于同一集合。允许一个就绪 CPU 的产品镜像正常运行；禁止在运行期 admission 失败时回退到 BSP。
- 启动窗口的 PID 1 固定 CPU0，显式指定 CPU 的 aarch64 测试任务保留单核亲和。fork 产生的新私有 mm 子进程使用运行期策略，即使父进程固定。exec 原地换映像，保留任务亲和与当前归属。保留 x86_64 调度语义、aarch64 内核线程亲和及存量测试入口。
- 首期采用 **EL0 IRQ 返回的可迁移调度点**，使用现有 EEVDF 拉取路径。扩大到任意阻塞的 EL1 续体会要求全量审查可能跨 `schedule()` 持有的 CPU-local 快照与资源。只在 fork/spawn 时分散初始负载则无法处理运行后出现的负载差异。主动推走正在执行的任务需要额外的停核运行协议，本期使用已脱离 CPU 的队列节点。
- 支持当前 coherent、同一 Inner Shareable 域的 aarch64 SMP profile。CPU 热插拔、共享 mm 线程、用户亲和 ABI、拓扑感知均衡及 EL0 FP/SIMD 剩余项各自独立。

## 调度点与所有权

1. 只有正式 EL0 IRQ dispatch 完成、EOI 和 IRQ-depth leave 已结束、DAIF.I 仍置位的 `arch_context_reschedule_return()` 调用可以标记可迁移续体。先校验真实用户帧、当前 CPU 归属和掩码成员资格、硬件 TTBR0 与私有 mm 一致，并排除活跃 VM edit。标记在进入 `schedule()` 前写入任务，恢复后在重新开 IRQ 前清除。标记仅描述本次续体，不能从 fork/exec 继承。第一次运行前、SVC（含 yield）、阻塞 syscall、暂停的 VM edit/COW/exec、EL1 IRQ、退出及 idle 续体均不提供此标记。
2. 标记本身不构成所有权交接。迁移须同时满足源队列中的 `TASK_RUNNING`、`on_rq=1`、`on_cpu=0`、`task->cpu==src`、目标亲和位与两端就绪。仍在源栈上、被 pick 预约、阻塞、退出、PID 1 或固定亲和的任务不能迁移。锁内先检查生命周期和 `on_cpu`，再读续体标记。源核保存栈/DAIF 续体、TLS、已有 FP 保存区并离开旧 TTBR0，切到下一任务栈后由 `task_finish_switch()` 在源 rq 锁内 release 清除 `on_cpu`。同一任务的下一次 pick 只能在该交接后生效。
3. 拉取方保持本地 IRQ 屏蔽，按全局地址顺序取得两把 rq 锁，锁内重查负载、队列和资格，然后从源 dequeue，release 更新 `task->cpu`，按既有目标 `min_vruntime` 下限修正 `vruntime` 并用 enqueue 重算 deadline。两锁释放前完成事务；拒绝迁移时不修改任务或队列。现有运行队列与 `need_resched` 为通知事实来源。拉取由目的核自己执行，直接进入本地 pick，不需要向自己发 SGI。正常远端 wake 仍按其发布时捕获的目标通知。迁移与 wake 并发时，旧 CPU 快照的读者取得旧 rq 锁后须重查并重试。持有 `task_list_lock` 的路径沿用 task-list → rq 顺序；均衡不取 task-list/VM/ASID 锁，不在 rq 锁内送 IPI、编辑页表或清 TLB。
4. 按本地队列长度与远端队列长度拉取。空队列可拉一个合格任务，繁忙 CPU 在远端至少多两个排队项时可拉取，单次最多一个 aarch64 正式用户任务。候选源按预估队列长度排序，同长度按 CPU ID 升序；最忙队列没有合格任务时继续查其余满足负载差的就绪源。锁外负载读只作提示，锁内决策为准；失败时下次机会重试。就绪且本地 tick 已启动的空闲 CPU 每八次本地 tick 获得一次额外拉取机会，机会可以合并。该机会由 idle 路径消费，使用原有 WFI 屏蔽/检查协议；保留 AP 普通 idle tick 不发布 `need_resched` 的约定和现有远端 SGI 唤醒。拉取入口复用正常 pick 路径，不增加 IRQ handler 内调度，也不要求从只运行一个任务的源核强制搬走当前任务。

锁外队列长度提示用与写端匹配的原子访问，目标 `min_vruntime` 的迁移快照与写端由 rq 锁串行化。aarch64 用户拉取不重置源 `min_vruntime`，空树时源核仍可能正在运行别的任务。对 `task->cpu` 的远端无锁观察使用原子读，例如 procfs 的 LastCPU 可报告排队归属。task-list 锁只保证观察期间的任务寿命。

## TTBR0、ASID 与 VM

- `task->cpu` 表示调度归属，可能领先于硬件安装。迁移不转移任何 per-CPU live TTBR0 缓存、IRQ depth、GIC 状态、uaccess window 或 CPU 统计。目的核通过现有 `arch_task_switch()` 在 IRQ 屏蔽下安装目标私有 mm 的 TTBR0（root PA + ASID）并恢复任务 TLS，然后切栈。目的核在调度恢复后重新取 CPU-local 状态；源核的 CPU、IRQ context 或 observer 快照不能当作目的核数据使用。任务拥有的用户帧、栈、TLS、FP 保存区和 VM 字段都由交接同步。
- 每个用户 mm 的根至多在一个 CPU live。源核硬件换走旧根并撤销其 registry 项后，才允许通过 `on_cpu=0` 发布可迁移性。目的核先 publish 根、再写 TTBR0 与同步、再允许执行用户代码或 uaccess。源/目的 live shadow 各由本核维护；ASID 0 空根及既有本地全刷边界照旧。根 registry 的幂等发布不是跨核引用计数。
- 继续用全局 8-bit ASID 池（独占用户 tags 1–255，池耗尽时用户 mm 仍可用 tag 0）。同 mm 迁移保留 root/tag。对非零 tag，迁移不单独 TLBI，旧核可缓存同一 mm 的非全局翻译；用户 tag 0 按原 switch 路径在离开/安装核分别完成本地全刷。撤销/修改前的 quiesce 与最终回收在同一 Inner Shareable 域内广播 `aside1is`（非零 tag）或 `vmalle1is`（tag 0），序列为 `dsb ishst; tlbi; dsb ish; isb`，完成后才改写 detached 页表、释放页或归还非零 ASID。审计 fork/COW、mmap/mprotect/munmap、demand/uaccess fault、exec 成功/失败及退出/回收的原有调用顺序，以硬件 detach 和根 registry 的检查为准。参考 [Arm Memory Management §8](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf)。
- `aarch64_user_frame_is_valid()` 与正式 uaccess 接受多位掩码下的当前 CPU 成员资格，保留现有真实任务、CPU admission、已安装根和 ASID、frame 与 mm 校验。VM 的 native/fault auth、edit TLS/DAIF 快照仍属于任务。正在编辑的任务即使开 IRQ 并发生同核切换也继续在空根上恢复 edit，直到原 `edit_end` 重装用户根。跨核返回后新一轮授权依据目的核实际硬件状态。
- `aarch64_vm_edit_begin()` 仅在 detach 与 quiesce 都成功后进入可修改状态。quiesce 返回错误时 IRQ 仍屏蔽，恢复用户根、入口 TLS/DAIF，清临时 edit 快照，传递错误且保持 `edit_active=false`。若根仍被其他 CPU 使用等情况令独占不变量失效，走原 fatal gate，禁止发布第二个 live 根。销毁/exec 清理同样以 detach 和同步失效完成为回收前置。

## 验收

- 宿主测试直接执行实际调度代码，覆盖正常转移与队列/CPU/vruntime/重新 pick，一台 CPU、固定亲和、未就绪/未接收 IPI、退出/阻塞、on_cpu/预约、无安全点、VM edit、一条最重但全不可迁队列后的有效捐献源，以及 wake 锁定旧 rq 后重试。验证空闲与繁忙阈值及两把锁下的负载重查。在宿主条件下对 idle cadence 验证第 8 tick 才开放一次机会，合并后可重新开放，普通 AP idle tick 不发布 `need_resched`。宿主与运行期用例共同验证 spawn/fork 按同一个就绪快照取得初始核与完整掩码，空集合拒绝发布，PID 1 与显式目标保持单核，exec 保留掩码。
- 新增独立 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=migration` 双核用例。测试变体沿用 user-runtime 构建与 `AARCH64_EL0_TEST` 的互斥，从嵌入的 ELF 将 fixture 写入 `/tmp` 的 tmpfs，由生产 VFS spawn 创建父进程并由其正式 fork 生成迁移样本，迁移后 exec 也从 VFS 加载。可控竞争和空闲窗口通过测试同步制造，生产放置、拉取与提交负责改变任务归属。记录同一 PID 先在 CPU A 实际执行 EL0，再从已保存的 IRQ 返回点迁入并在 CPU B 实际执行 EL0。检查源核 detach 先于转移、源/目的从未并行执行该任务，目标根和 ASID、用户帧/栈及 TLS 随任务恢复。覆盖两个方向并返回曾使用过该根的 CPU。至少一个 BSP→AP 样本须记录 AP 进入 WFI 协议、阈值前空闲循环无拉取、第八个实际 CNTP tick 发布机会、idle 消费、commit/pick 顺序；窗口内无远端 SGI、AP work 或测试直接调用调度。
- 在一核从已知 VA 访问旧图样后，迁到另一核撤销旧映射、以不同 PA 重建同一 VA 并改变权限。记录前后 PA 并要求不同。迁回旧核后由 EL0 直接 load 校验新图样，再执行该 VA 的禁止访问并观察匹配 PID、CPU、FAR 和访问类型的权限异常。权限异常可放在独立的终结场景。另验证合法和不可读 uaccess 的成功与 `-EFAULT` 返回，保留另一常驻 mm 的同 VA 独立数据。对同 PID/root/ASID 记录真实 VM edit 的 TLBI 完成先于页表变更/释放，迁移本身不计作 edit TLBI。检验带 COW 的 fork、迁移后 exec/退出/wait 回收，以及 roots/pages/ASID 计数回基线。
- 运行包含 VM edit 暂停覆盖的存量用例，检查 edit resume 仍在原核；该禁止路径另由宿主候选测试钉死。以 VM 状态机宿主故障注入验证 quiesce 失败时恢复状态并拒绝编辑，以 ASID 池耗尽回归及产物反汇编核对 tag 0 本地全刷、两条广播指令和屏障顺序。QEMU 证据对缺项、错误顺序、重复和超时 fail closed。
- 跑既有 aarch64 `irq`、`asid`、`smp`、`product` user-runtime、AP sched-wake 与 sched-tick 回归，再跑 `make test-host`、`make test-static`、x86_64 phase-0。既有 `smp` 用例的 COW barrier 与 waitpid 依赖父子各占一核，测试可在该场景内禁止其子进程被拉取，仍须检查 fork 的原始落点、远端唤醒、COW 双核竞争和 waitpid。所有新观测钩子与测试调度控制限于测试构建；文档同步更新架构、调度和测试说明。以这些验证结果作为将 roadmap 项标记完成的依据。
