# aarch64 运行期跨核任务迁移 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal** 在正式 aarch64 SMP 产品构建中，已运行的 EL0 任务能由调度器跨核拉取，并在目的核继续使用原用户帧和私有 mm。

**Architecture** 只迁移停在已验证 EL0 IRQ 返回调度点、已完成 switch tail 的 runnable 任务。空闲与繁忙 CPU 复用正常 schedule/pick，在两把 rq 锁下交换所有权。TTBR0/TLS 由目的核正常 switch-in 安装，页表修改及回收仍由 ASID 广播和私有根独占协议保障。

**Tech Stack** C、AArch64 汇编、EEVDF、GIC/CNTP、GNU Make、宿主测试与 QEMU Python runner。

**Spec** [2026-11-21-aarch64-runtime-task-migration-design.md](../specs/2026-11-21-aarch64-runtime-task-migration-design.md)

## 全局约束

- PID 1 固定 CPU0，显式指定 CPU 的 aarch64 测试任务保留单核亲和。有效 CPU 须同时满足 `online`、`ipi_ready`、`scheduler_ok`；单核可用，运行期失败无 BSP fallback。
- 仅正式 EL0 IRQ 返回可标记续体。只有 `TASK_RUNNING/on_rq=1/on_cpu=0` 且已完成 VM/栈交接的任务可移动。所有新观测钩子和测试调度控制限于测试构建。
- 地址序 rq 双锁，已有 task-list → rq 次序。双 rq 锁内仅做队列事务，不引入 task-list、VM、ASID 锁或 IPI。保留 x86_64 调度策略和 aarch64 内核线程亲和。
- 单次最多拉一个正式用户任务。繁忙队列差至少为 2，额外空闲拉取每 8 个本地 idle tick 开放一次。
- 独占用户 ASID 1–255，ASID 0 耗尽回退；同一 Inner Shareable 域、私有 mm，无 CPU 热插拔。根 registry 不作多 CPU 引用计数。
- 新宿主测试用 `@os01-test`，运行 `make regen-test-catalog`；新内核日志按仓库日志规范。

## 重点复核

- quiesce 报忙/未注册时不会重新发布另一个 CPU 可能在用的根。Task 1 验证回滚和 fail-stop 分支。
- 单核/no-switch 与 IRQ 调度点返回后清掉安全标记，随后在 SVC 阻塞时不能以旧标记参与均衡。Task 2、Task 5 分别校验标记生命周期与实机交接。
- 并发 wake 命中旧 rq、旧源队列长度在等锁时变化、最重源全不可迁。Task 3 用真实移动路径和受控交错验证。
- fork/exec 的亲和在 mask 中存在离线或未 IPI ready 节点时仍遵守 one-snapshot 契约。Task 2 用 mock 拓扑，Task 5 用实际入口佐证。
- 第 8 tick 前被其它 IRQ 唤醒、同一禁止访问误走 uaccess 软件 precheck、旧 VA 分配重用 PA。Task 4、Task 5 分别拒绝这些假阳性。

## Task 1 · VM 编辑的失败闭合与 ASID 复核

**文件** `kernel/arch/aarch64/memory/user_vm.c`、`kernel/include/arch/aarch64/user_vm.h`、`hosttests/cases/test_user_vm.c`、`hosttests/mock/user_vm_runtime.{c,h}`。审计 `user_mm.c`、`user_asid.c`、`user_image.c`、`user_vm_fork.c`、`cpu/user_task.c` 内的 detach/lookup/quiesce/释放链。

**接口** 保留 `int aarch64_vm_edit_begin(mm_t *mm)` 与 `void aarch64_vm_edit_end(void)`。在宿主 shim 增加 `void user_vm_test_set_quiesce_error(int error)`，由已链接的 quiesce stub 使用，一次 reset 清零注入。

- [ ] 在 `test_user_vm.c` 新增 `edit_quiesce_error_restores_context`，通过真实 `arch_syscall_user_vm(SYS_munmap, args)` 注入 `-EAGAIN`。断言 error 原样返回、根和双 TLS 恢复、DAIF 等于入口、`edit_active == false`、edit 四个快照字段归零、VMA/PA/PTE 不变，复位故障后同一授权可重试。新增 `edit_quiesce_busy_fails_stopped`，注入 `-EBUSY`，通过宿主 fatal stub（复用 gate 测试的 setjmp 模式）截获，断言没有第二次 publish、policy 未进入。对无有效 TTBR0 值的注册丢失也做相同断言。
- [ ] 跑 `make test-host`。红灯应能定位到上述新例，现有代码忽略 quiesce 返回值。
- [ ] 在 `aarch64_vm_edit_begin` 保存入口状态并 detach 后检查 quiesce。可以安全回滚的错误在 IRQ masked 下恢复原 mm/TLS，清 edit 快照，按原样恢复 DAIF 并返回错误。已经 detach 后的 `-EBUSY` 或查无有效注册走现有 `vmm_gate_violation()`，保持 fail-stop；禁止 IRQ enable 和调用 policy。沿成功路径原有 auth、edit 标记及 IRQ 行为。
- [ ] 核对 `aarch64_user_mm_destroy` 和 `aarch64_user_image_destroy` 在 revoke 后先完成失效再释放 PA/tag，fork 的父根编辑走同样的 quiesce 门。保持现有 ASID 0 与非零 tag 分支。跑 `make test-host`，包括 VM 和 `aarch64-user-asid` 的耗尽/释放回归。提交本任务。

## Task 2 · 就绪掩码与 EL0 IRQ 返回续体

**文件** `kernel/arch/aarch64/memory/vmm_gate.c`、`kernel/include/arch/aarch64/vmm_gate.h`、`kernel/arch/aarch64/cpu/user_task.c`、`kernel/arch/aarch64/memory/uaccess.c`、`kernel/arch/aarch64/sched/exec.c`、`kernel/sched/{kthread,balance}.c`、`kernel/include/sched/{internal,task}.h`、`hosttests/cases/test_smp_starting_gate.c` 及受共享签名影响的宿主 mock。

**接口** `int aarch64_user_pick_cpu(uint64_t *allowed_cpus)` 返回 CPU ID 或既有 errno，失败写 0；`int sched_alloc_and_publish_task(unsigned cpu, uint64_t allowed_cpus, uint64_t flags, const char *name, int (*prepare)(task_t *, void *), void (*discard)(task_t *, void *), void *arg, task_t **out_task, int *out_pid)`。新增任务字段 `bool irq_return_migratable`，在 fork/exec/首次进入及调度之外为 false。Task 3 只在源 rq 锁下通过 `on_cpu` 检查后读取该字段。

- [ ] 在现有 SMP gate 宿主例中加断言。boot CPU0 的返回为 CPU0/掩码 1；运行期两核依次返回 1、0，掩码均为 3；三项目标 gate 逐个拒绝置位；只剩 CPU1 ready 时返回 1/掩码 2；空 ready 集合为 `-EAGAIN` 和掩码 0。发布测试对缺初始 CPU 位、运行期 gate 失败断言无挂链且 prepare 成功的资源按原路径 discard。
- [ ] 跑 `make test-host`，预期新接口尚缺，早期编译失败。实现 picker 一次采集 ready mask 并在现有 CAS 光标循环中使用同一个 mask。调整通用 publisher 用显式掩码、在分配前检查 CPU 范围及位，在发布锁内复查初始目标 admission。aarch64 的 `PF_PROCESS` 路径同时要求 `ipi_ready`；只保留既有 BSP boot exception。BSP boot 分支在发布锁内要求该真实任务的 `allowed_cpus == 1`，否则执行同一 rollback。宿主发布测试故意传多位 boot mask 验证拒绝；普通产品启动以现有 `init-created pid=1` 和进入 shell 作为此发布门通过的证据。公开 kthread 包装用原 pinned 位或 `arch_kthread_default_mask()` 调用通用 publisher，保持原行为。
- [ ] 在当前 aarch64 `sched_balance` 的候选循环先加阶段性 `PF_PROCESS` veto，并以多位掩码的宿主候选用例证明无转移。先跑 `make test-host`，再启用下一步的正式多位掩码。Task 3 用完整生产条件取代这道护栏。
- [ ] 接通 VFS spawn、fork、`aarch64_user_task_spawn_on_cpu()`，分别传 picker mask、picker mask、显式单核位。exec 成功和失败都保留 mask。正式 frame gate 和 uaccess 把单核等式换成 mask 成员判定，保留 CPU/硬件根等校验。检查跨边界 task/thread 布局与 stack reserve。
- [ ] 在 `arch_context_reschedule_return` 的现有 `schedule()` 调用前置位 `irq_return_migratable`，返回即清，在 post-schedule test hook 之前清；IRQ 已屏蔽，且此前已通过真实 frame/非 edit/need gate。持久化观察参数只有 task、frame 与数值快照。恢复后重新取当前 CPU、验证同 task 和硬件根；不得使用缓存的源 `percpu_t *`。静态核实唯一生产置位在此路径，存量 `irq` 用例加 no-switch/切回即时清理与暂停 edit 不置位断言。
- [ ] 跑 `make test-host`、`make PROFILE=aarch64-clang test-static CASE=sched-common-compile` 和 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=irq`，后者仍须证明 IRQ 后用户寄存器与暂停 edit 恢复。提交本任务。

## Task 3 · aarch64 拉取事务及 wake 交错

**文件** `kernel/sched/{balance,core,fair,kthread}.c`、`kernel/fs/procfs_snap.c`、`hosttests/cases/test_sched_aarch64_migration.c`、`hosttests/mock/sched_notify_test_runtime.h`、`hosttests/mk/sched.mk` 和目录生成物。

**接口** 继续使用 `void sched_balance(percpu_t *rq)`，aarch64 分支以同一 TU 内 `static bool pull_aarch64_user_task(percpu_t *dst, percpu_t *src)` 实现每个源的一次事务，生产候选依赖 Task 2 的字段及 `bool aarch64_vm_task_editing(const struct task_struct *task)`。测试走同一分支，用独立 host 编译 selector，宿主不伪装为 ARM 指令目标。

- [ ] 用 `@os01-test` 新建宿主例，链接真实 balance/core/fair 与 rbtree。正例迁移 PID 42、mask=3、vruntime=5 从 CPU0 到 CPU1 的 floor=12，断言 CPU=1、计数 0/1、deadline 为 `12 + EEVDF_MIN_SLICE`，目的 pick 为同节点，源 floor 不变。反例逐个覆盖单核、idle/init、未 ready、窄 mask、源归属不符、blocked/zombie、`on_cpu=1` 与预约、未标记、正在 edit。负载 fixture 覆盖空 rq、busy 差 1/2、最重不可捐但次重可捐、同负载低 CPU ID 优先。
- [ ] 加可控交错。锁钩子在配对锁真正取得前让源负载跌到阈值以下，断言不移动；醒者在读到旧 CPU 后、拿锁前执行一次真实迁移，断言 wake 重试并锁住新 rq，节点只在目标队列一次。选择可进行确定性调度的 mock 锁，禁止在模型内同时把同一把锁宣称为已取得。跑 `make regen-test-catalog`、`make test-host`，预期正例在 Task 2 的保护下为零迁移。
- [ ] 在 aarch64 的 `sched_balance` 路径按原子 `nr_running` 快照选源（降序，CPU ID 升序）；遍历其余源用至多 `NR_CPUS` 的本栈候选。每源在 DAIF.I masked 后按 rq 锁地址排序加锁；以锁下实际长度重查 busy/idle 阈值和就绪，沿源 rbtree 从尾向前查。先查 `on_cpu=0`，再查安全标记和正式用户 VM 编辑条件。对合格候选执行 dequeue、release 更新 `cpu`、上调 `vruntime` 至目标 floor、enqueue，反序解锁。只移动一个；不向目的核发 SGI，不清源 floor。x86_64 的最大源、门限、批量、floor reset 保留。
- [ ] 统一 `nr_running` 的并发读写为 relaxed 原子操作，锁内检查仍以锁保证一致快照。aarch64 的调度写 `min_vruntime` 放在 rq 锁内（包括 self-pick 用尽时间片），分配期的锁外提示也用与写端匹配的原子读取，避免依赖锁外缓存。procfs LastCPU 以原子读报告归属。检查 `task_wake`、`blocker_state_snapshot` 已有重试，任何无锁快照不得直接用于操作另一个 CPU 的队列或在解锁后拿来解引用任务。
- [ ] 跑 `make test-host`、`make check-test-catalog`、`make PROFILE=aarch64-clang test-static CASE=sched-common-compile`。在专用 `smp` fixture 仅允许对测试的 COW/waitpid 样本抑制拉取，使用一个 test-only candidate veto，保持原有硬件双核观测。跑 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=smp`。提交本任务。

## Task 4 · 空闲 AP 的真实本地 tick 触发

**文件** `kernel/arch/aarch64/cpu/{sched_tick,idle}.c`、`kernel/include/arch/aarch64/sched_tick.h`、`kernel/selftest/test_aarch64_sched_tick.c`，`hosttests/cases/test_aarch64_sched_tick.c`、`hosttests/cases/test_aarch64_idle_wakeup.c`、对应 `hosttests/mock/` 与 `hosttests/mk/{sched,intr}.mk` 链接。

**接口** `bool aarch64_sched_tick_take_idle_balance(void)`，调用方 local IRQ masked，仅当前 CPU 的 idle continuation 消费机会。

- [ ] 扩展已链接生产 CNTP/tick/fair/accounting 的宿主 tick 用例与 idle wakeup 用例，mock 实际递送的 CNTP。断言尚未启动、任一 readiness 位关闭、没有第二个 ready 核、忙 tick、无第 8 次 idle 样本不会产生机会；连续 7 次 idle tick 返回 false，第 8 次后 take 返回 true 然后 false，未消费的两批合并一次，后续能再次产生；AP idle 的 `need_resched` 一直是 0。用真实 `arch_aarch64_idle_step()` 验证只被 SGI 唤醒的 WFI 迭代不会生成 cadence、已经发布的机会会被正常消费。跑 `make test-host`，预期缺接口或 cadence 断言失败。
- [ ] 在启动状态旁初始化 per-CPU idle tick 计数与 pending，实际 timer 调用仅在本核 online/ipi_ready/scheduler_ok 已发布且至少另一 CPU 同样 ready、current==idle 后累积 idle 样本，每 8 次置 pending；繁忙 tick 不累积。通过本地 IRQ 屏蔽保证这两个字段只由本核读写。不设置 AP `need_resched`，保留 BSP 现有每 tick 请求。idle 在原有 masked 本地工作检查时 take，pending 与原条件合并为正常 `schedule()` 的入场条件。现有 rq 检查、READY ap_work 和 WFI/SGI 防丢唤醒顺序保留。
- [ ] 跑 `make test-host`、`make check-test-catalog` 与 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake`、`make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick`。sched-tick 原有 AP idle tick 在没有 SGI 时不置 `need_resched` 的证据仍须通过。提交本任务。

## Task 5 · 双核迁移 fixture、证据与发布门禁

**文件** `mk/profiles/aarch64-clang.mk`、`mk/components/run.mk`、`kernel/Makefile`、`kernel/selftest/{selftest,test_aarch64_user_runtime,test_aarch64_user_migration}.c`、`kernel/include/selftest/aarch64_user_runtime.h`、`kernel/selftest/fixtures/aarch64_user_migration.S`、`qemutests/{aarch64_user_runtime,test_aarch64_user_runtime}.py`。测试钩子落在已有产生事实的 IRQ、调度事务、VM/ASID 路径。编辑测试注册与源码时保留旧 `irq/asid/smp/product` 分支。

**接口** `void test_aarch64_user_migration_run(void)`，`AARCH64_USER_RUNTIME_CASE_MIGRATION` 编译门，selftest ID `aarch64_user_runtime_migration`，runner `--case migration` 使用 2 vCPU 与既有 120 s 绝对期限。按 `[user-runtime] migration-event seq=<n> kind=<kind> ...` 输出证据。观察器用 IRQ-safe 原子 fetch-add 预约全局序号与预分配槽位，在各事实发生处记录值类型字段。等待所有观察者离开后按全局 seq 排序输出，溢出、空洞或重复判失败。

- [ ] 先扩展 runner 单测与选项。要求一条成功封套、正确的 selftest ID、阶段内严格序号。冻结下表的事件与交叉断言，对删行、重复、方向错、root/tag/PA 错配、借 SGI 通过、滞后 TLBI、软件 uaccess 冒充硬件 fault 逐项构造负例。跑 `python3 -m unittest qemutests.test_aarch64_user_runtime`，先明确缺 parser 的红灯，补 parser 后单测全绿。runner 审计新专用符号和普通产品镜像无这些符号，沿用 variant ELF/image 对应及既有 QEMU 异常退出判定。
- [ ] 在 profile/run 两侧注册 `migration`，沿既有 user-runtime 与 `AARCH64_EL0_TEST` 互斥构建。kernel Makefile 按对应变体链接 fixture ELF blob，复用 fixture linker script，注册一个 late case，并从 `test_aarch64_user_runtime_run()` 分派。用 `vfs_create_file/vfs_write` 将 ELF 写入 `/tmp/urt-migration` 和 `/tmp/urt-migration-exec`。校验完整写入、关闭引用并按路径清理。第一次运行 migration 目标应到达迁移验收，未接入的证据令其失败。
- [ ] 用正式 VFS spawn 启动，由父进程 native fork 创建两个被跟踪的子进程（先验证 VM/权限异常，再单独验证迁移后 exec）。第一次 spawn 在 CPU1，第一/第二次 fork 分别落 CPU0/CPU1。通过已验证 frame 的正式 syscall 观察 placement。父子用一页 `MAP_SHARED|MAP_ANONYMOUS` 控制页（VA `0x40002000`）通过 EL0/EL1 acquire/release 交换阶段标志，控制页只在父仍持有它时用于直接观察。父等待各子进程的真实 waitpid。主子进程只有两个明确 move ID（`1` 代表 0→1，`2` 代表 1→0）；第二子进程单独使用 move ID `3`（1→0）。测试候选门仅让当前样本通过，窗口开始后保持其放行，不能写任务 mask、`cpu`、标记、队列和生产空闲计数。
- [ ] 为两个源 CPU 各预先创建 pinned helper。helper 用本核 wait queue 阻塞，控制器在启动 workload 前确认已经切下；观察正常 tick 在目标无 syscall 的 EL0 循环耗尽 slice 后，在该核 timer EOI 的测试 hook 用 `wait_queue_wake_one()` 让该核 helper runnable。沿生产 IRQ-return→pick(helper)→switch tail 交接；使用等待了更长时间的 helper，并在锁下确认它的 deadline 序优先于目标重入队后的 deadline。若未选中 helper，本轮失败并报告两者的 deadline。helper 恢复后在 EL1 有界等待 commit 证据，确保源核在该间隙不重新 pick 目标。任何准备阶段的 SGI 在观察窗口前收齐；整个 rendezvous 不持 rq/VM 锁，使用独立 arch counter 期限。
- [ ] move 1 的 AP 窗口从已自然消费上一批机会、空闲计数为零且无 pending 的 pre-WFI 点开始。用 AP masked、无锁的 pre-WFI hook 等待 BSP helper 发布目标已 off-CPU 且可拉证据；期间本地 CNTP 不能被软件提前递送。确认 AP rq 为空、无本地 resched 或 AP work，再执行原 WFI 路径。窗口内目标始终可拉，从实际 CNTP tick 1 到 7 每次检查无消费/无调度并重返 WFI，第 8 拍由同一轮 idle 消费触发真实 schedule→commit→pick；观测实际 GIC dispatch，拒绝 SGI/其它 IRQ/AP work/测试主动调用 schedule。move 2/3 反向由同样已切下目标的 AP helper 留住源队列。迁移变体的 BSP 顶层等待直接循环 `arch_aarch64_idle_step()` 直到 worker 完成，worker 阻塞期间顶层不调用 `kthread_yield()`。记录 idle 内实际消费的 BSP 本地 CNTP 请求及其进入正常 schedule 后的 pick；反向窗口拒绝测试直接调度和 SGI/work 来源。准备或清理可以用正常 wake，不能计入验收窗口。
- [ ] 主样本从第一条 EL0 旧内容读取到回源检查始终使用同一非零 tag 和私有 root。父子的匿名共享映射在 VA `0x40000000` 保留旧 backing 及旧内容，另在 `0x40001000` 建私有页，子在暖旧 TLB 之前先验证 COW 隔离。子在 CPU0 读旧值，CPU1 `munmap` 后重建同一 VA 的匿名私有页，触发 demand-fill，写入新值后 `mprotect(PROT_READ)`。用生产页表 walker 区分 PA；父保留旧映射，验证旧值与旧 PA。旧核不运行 ASID 0 用户根，拒绝旧核本地全刷、任何广播全刷及无关的同 tag 失效从旧 load 到回源直接访问之间混入。对每次被观测的 edit，关联真实 TLBI 完成及后续变更。munmap 取 leaf 撤销、demand fault 取首次 leaf 安装、mprotect 取 leaf 权限变化。重新 mmap 仅登记 VMA，记录实际 VMA commit 与此时 leaf absent，下一次 demand fault 的 leaf 证据另行归属。
- [ ] 主子回 CPU0 后，用连续 EL0 load/store 先直接读新内容、再对只读 `0x40000000` 写入，中间没有 syscall 或额外 VM edit。观察 `EC=0x24, FSC=0x0f, FAR=0x40000000, WnR=1`。在 load 成功后通过保留 GPR 传递图样和阶段标志，由随后的 permission abort 采集；断言父的 waitpid 返回对应 PID、原始 `(int)-EFAULT`。另一个子进程迁移后用已成功以可写模式打开的 `/tmp/urt-migration-copy`，对有效缓冲区 `SYS_write` 返回请求字节数，对单独的 `PROT_NONE` 页 `SYS_write` 返回 `-EFAULT`，两次经过同一个真实 copy_from 路径。随后用正式 `SYS_exec` 载入 `/tmp/urt-migration-exec`，从新 ELF EL0 再入后以状态 42 退出，父 waitpid 返回该 PID、状态 `42 << 8`。回收所有 helper 和父子任务，关闭观察器，清理临时 VFS 文件，再比对 roots/pages/nonzero ASID 基线。ASID 用仅测试可见的池占用计数，在池锁下采样。
- [ ] 复核实现的完整证据和以下命令。反汇编取 product runner 同一 `build/aarch64-clang/artifacts/kernel.elf`（若 profile 路径被覆盖，取该次 runner 的 `--elf` 实参），验证 `aarch64_asid_tlbi` 的 `aside1is` 与 `vmalle1is` 各有 `dsb ishst` 前缀、`dsb ish; isb` 后缀；`aarch64_user_install_mm/aarch64_user_restore_kernel_root` 的 ASID 0 分支用了本地 `vmalle1`。保存反汇编和迁移串口日志。QEMU 不能替代该指令审查。

| 事件 | 字段与校验 |
|---|---|
| `spawn` ×1、`fork` ×2 | `pid cpu mask`，spawn=(CPU1,3)，fork=(CPU0,3)/(CPU1,3)，PID 各异；普通产品 PID 1 由 Task 2 发布门验收。现有 `smp` 用例给通过 `aarch64_user_task_spawn_on_cpu()` 创建的两个 EL0 companion 增加 `explicit_pin` 证据（`pid cpu mask`），要求在各自 CPU 首次真实进入 EL0，mask 分别为 1/2 |
| 每个 move 的九个事件 | 各携带 `move_id pid cpu root asid`，严格依序 `irq_save, hw_detach, root_unpublish, tail, commit, pick, hw_install, irq_resume, el0_after`，每类每 ID 恰一条。detach 对照 MRS，unpublish 在 registry 撤销后，tail/two-rq commit 在锁内记录。pick 验证预约 `on_cpu=1`，hw_install 发生在 publish、MRS 一致之后。irq_save/resume 对比同一 frame 地址、全部 GPR、SP、ELR、SPSR 与双 TLS；在信号派送前记录 resume。el0_after 从真实 lower-EL 再入读取未被 syscall 参数占用的 GPR/TLS sentinel。commit 另带 src/dst，tail 记录 on_cpu=0，配对每个 PID 的 live 区间禁止重叠 |
| move 1 的 `idle_zero, idle_tick, idle_check, idle_take` | `idle_zero` 一条含 `cpu rq_nr pending cadence`（1/0/0/0）并排在源 tail 后、第一 tick 前，前置 WFI 另记 `wfi_enter`。tick 恰 8 条，各带硬件 INTID=30 与索引 1–8；check 记录 1–7 均回 WFI；take 只一条在 tick 8 后，在同次 idle 检查和首个 commit/pick 前。窗口的 SGI/非 CNTP IRQ/AP work/主动调度计数必须全 0。move 2/3 各有 `bsp_tick_schedule` 一条，记录该 move ID 和实际 BSP CNTP 请求 |
| `old_load, remap, new_load, permission_abort` | 都与主 PID 关联，各恰一条并按表序；old load 由随后 SVC 在 CPU0 确认，new load 的 GPR 证据与随后的真实 permission abort 在 CPU0 一起采集，各带 `va pa data`，两 PA 不等、数据符合 fixture，remap 位于两次 move 之间、来自 CPU1。fault 验证上述原始 ESR 字段。父在 `parent_check` 记录该 VA 的旧 PA/值与独立 COW 页的父值，子用 `cow_check` 提交自己的值 |
| `edit_begin, tlbi_done, edit_leaf, edit_vma` | 各 `pid cpu root asid tx op`，为主 PID 的四个操作各一个不同 tx。每 tx 依序恰有 begin、实际屏障后的 done、该操作的变更。munmap/fault/mprotect 各用一条 `edit_leaf`，含撤销/安装/权限变化的前后 PTE；mmap 仅用 `edit_vma`，含 VMA commit 与 leaf absent。记录 `full_flush` 与同 tag TLBI 以便拒绝污染窗口，允许这四次自己的失效 |
| `copy_ok, copy_denied, exec_commit, exec_el0, wait_status, baseline, final` | 两条 copy 事件关联 move 3 的 PID 和目的 CPU，在 exec 前分别返回字节数和 `-EFAULT`。exec 两事件关联 move 3 的 PID 和目的 CPU，`old/new root` 不同、`old/new mask=3`，随后从新 entry 再入 EL1。两条 wait 记录各有 `child_pid returned_pid status`；baseline/final 各一条含 roots/pages/asids，相等。成功封套只能在全部证据、清理完成及 final 后输出 |

```sh
set -e
python3 -m unittest qemutests.test_aarch64_user_runtime
make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=migration
for runtime_case in irq asid smp product; do
    make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE="$runtime_case" || exit 1
done
make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake
make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick
make check-test-catalog
make test-host
make test-static
make PROFILE=aarch64-clang test-static CASE=sched-common-compile
make test-qemu SUITE=phase-0
"${LLVM_OBJDUMP:-llvm-objdump}" -d \
    --disassemble-symbols=aarch64_asid_tlbi,aarch64_user_install_mm,aarch64_user_restore_kernel_root \
    build/aarch64-clang/artifacts/kernel.elf
```

- [ ] 更新 `docs/arch.md`、`docs/sched/scheduler.md`、`docs/build/build.md` 的 user-runtime 测试说明，说明就绪掩码、迁移边界、TLBI 证据与测试命令。全部门禁通过后才在 `docs/roadmap.md` 标记迁移完成。提交本任务。
