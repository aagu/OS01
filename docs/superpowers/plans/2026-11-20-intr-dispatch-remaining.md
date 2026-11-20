# 中断/异常 dispatch 统一（剩余项）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal** — 在 aarch64 产品镜像交付正式 EL0 IRQ 返回抢占、8 位 ASID 和固定亲和的 SMP 用户调度。

**Architecture** — 继续使用公共事件路由和调度器。只在 aarch64 正式 EL0 IRQ 适配器尾部接调度。VM 保存任务态授权、CPU 局部硬件状态与 mm 标签。完成并发资源契约后扩大用户任务放置范围。

**Tech Stack** — C、AAPCS64 汇编、GNU Make、现有 hosttests 和 QEMU virt。

**Spec** — [2026-11-20-intr-dispatch-remaining-design.md](../specs/2026-11-20-intr-dispatch-remaining-design.md)（评审通过）

## 全局约束

- 顺序为 A → B → C。每阶段单独验证，使用正常调度、异常和 VM 入口。公共分派继续无状态，沿用 x86 IRQ/IST/softirq/信号及 aarch64 旧探针语义。保留寄存器布局、用户 ABI、SMP 启动接口。
- A、B 阶段保留产品单核门。C 阶段任务终身固定单核亲和，PID 1 固定 BSP，多任务可跨核并行。保留 BOOT-5 创建窗口和 AP 既有接纳顺序。
- B、C 阶段使用 4 KiB、40-bit PA、`TCR_EL1.A1=0, AS=0`，TTBR0[55:48]、CnP=0、标签 1–255、0 回退。修改或释放前完成规定的 TLBI。新增运行时日志走 `log_*`。
- 测试集中在真实 IRQ 切换、ASID 同 VA 隔离和双核用户调度三条主场景。hosttests 优先扩展既有夹具，新增宿主测试带 `@os01-test` 并运行 `make regen-test-catalog`，记录实际 RED 原因和 GREEN 证据。内核自测镜像与 `OS01_SYSTEST=1` 分开。
- 测试标记由 guest 根据实际观察发出。新增 runner 用已有 `ProcessSession` 和 `RunArchive`，只消费 make 传入路径，120 秒绝对期限。超时、fatal、进程自退、重复/缺失必需证据均失败。普通 ELF 审计新测试钩子及 fixture 符号。

## 评审重点

- A 的单核场景验证任务 A 暂停 VM 编辑时，任务 B 经 IRQ 返回处理信号，A 恢复原编辑与 TLS。
- B 的同 VA 场景使用原启动 identity 区间中的地址，验证非零标签、替换和撤销。ASID 池用一个宿主生命周期用例验证回退及失效后再用。
- C 的两核场景要求每核实际切换两个正式 EL0 任务，并钉住已经发现的两类竞态。COW 最后一引用在双核场景重放，两种孤儿交接交错由同一宿主夹具驱动。

## 文件与责任

| 阶段 | 文件 | 职责 |
|---|---|---|
| A | `kernel/arch/aarch64/intr/{trap.c,syscall.c,gic_driver.c}`、`kernel/arch/aarch64/cpu/{user_task.c,task_arch.c}`、`kernel/arch/aarch64/memory/{user_vm.c,uaccess.c}`、对应 `kernel/include/arch/aarch64/` 公共头 | IRQ 尾部、任务 VM 状态、局部 copy 窗口、切根/TLS |
| B | `kernel/arch/aarch64/memory/{user_asid.c,user_mm.c,user_image.c,page_table.c}`、`kernel/include/arch/aarch64/{user_asid.h,user_mm.h,page_table.h,mmu.h}`、`kernel/arch/aarch64/cpu/{user_task.c,sched_boot.c,task_arch.c}` | 标签、根生命周期、叶属性与 TLBI |
| C | `kernel/arch/aarch64/{cpu/ap_sched.c,cpu/user_task.c,sched/exec.c,memory/vmm_gate.c,memory/user_vm_policy.c,memory/user_vm_fork.c,memory/user_elf.c}`、`kernel/sched/{core.c,kthread.c,wait.c}` 与对应头、`kernel/sched/signal.c`、`kernel/syscall/{sys_native.c,sys_fs.c}`、`kernel/fs/file.c`、`kernel/include/fs/file.h`、`kernel/init/userspace.c` | 资源交接、共享信号和文件位置、跨核映像、ready/放置、产品启动 |
| 验收 | `kernel/selftest/test_aarch64_user_runtime.c`、`kernel/include/selftest/aarch64_user_runtime.h`、`kernel/selftest/fixtures/aarch64_user_runtime.S`、`qemutests/aarch64_user_runtime.py`、`qemutests/test_aarch64_user_runtime.py`、`mk/components/run.mk`、`mk/profiles/aarch64-clang.mk`、`mk/project.mk`、`kernel/Makefile` | 共享 fixture、观察与隔离构建。沿用 M4 fixture 链接脚本 |
| 回归 | `hosttests/cases/` 中现有 VM、页表、switch complete 和 VMM gate 夹具及其 mock/link group。`docs/{roadmap.md,arch.md,build/build.md,changelog.md}` | 定向回归与闭环文档 |

## A1. 任务拥有 VM 授权与 TLS

**接口**

- `bool aarch64_vm_task_editing(const struct task_struct *task)`，供 switch 路径判定是否安装内核根。
- `void aarch64_user_switch_mm(struct task_struct *prev, const struct task_struct *next)` 替换现有单参数版本。调用者 `switch_to()` 保持本地 IRQ 屏蔽，由同一次调用完整执行保存 TLS → 切根 → 恢复 TLS。
- 延用 `aarch64_vm_native_enter/leave`、`aarch64_vm_edit_begin/end`、现有 fault 与 uaccess 接口。

- [ ] 在 `hosttests/cases/test_user_vm.c` 的事务夹具中先加一个串行交错用例。A 保存授权并 detach，切换 mock current 到 B，B enter/edit/end/leave，再切回 A。断言 `aarch64_vm_task_editing(A)` 为真、B 为假，A 的 edit end 恢复 A root、TLS、IRQ。IRQ 深度非零时 `edit_begin()` 返回 `-EBUSY`。沿用相应 link group 并更新 mock 对 current 的标识。
- [ ] 运行 `make test-host`，记录事务状态接口缺失或交错破坏旧全局状态的失败。
- [ ] 在 `aarch64_user_task` 的私有状态中定义每任务的授权与 edit 快照，初始化所有 spawn/fork 构造路径。绑定 current、mm、帧。IRQ 深度和 copy window 使用 arch 私有 `NR_CPUS` 状态，通过已安装的本核身份访问。无窗口的早期 abort 仍可安全走 fatal。退出由任务生命周期销毁状态。exec 成功后清旧映像的授权，失败沿原事务退出。准备路径更新必要 mock。
- [ ] `switch_to()` 在切根前保存 prev TLS，在切根完成后恢复 next TLS（内核或暂停 edit 使用清零的寄存器，原 edit TLS 在任务快照里）。从通用 install-root 移出 TLS 清零副作用，fresh spawn 的保存字段初始化为零，成功 exec 重置保存字段和硬件寄存器。fork 时父根已 detach，子任务从父 edit 快照继承 TLS，首次入用户态保留 switch 已装好的值。edit begin 保存入口 TLS、卸载用户根并显式清硬件 TLS，edit end 恢复入口值。FP 沿用现有 fork 快照。
- [ ] 运行 `make test-host` 和 `make PROFILE=aarch64-clang test-aarch64-audit`。执行 A2 末尾的 M4 回归。提交 A1 的实现和相应测试。

## A2. 正式 IRQ 返回与一条单核验收

**接口**

- `void aarch64_user_irq_return(pt_regs_t *regs)`，只从 `aarch64_el0_irq()` 的正式分支调用。
- `test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=irq`。本任务引入 mode 和 `AARCH64_USER_RUNTIME_TEST=irq` 构建选择器，后续复用 runner/fixture。

- [ ] 先接一条单核正式 ELF 场景。两任务各守自己的 GPR、NZCV、SP、FP、TLS 和数据哨兵，预热后执行有界计算循环，循环中无 SVC。在 `gic_driver.c` 仅对测试变体观察真实 CNTP INTID 30 和 `gic_eoi()` 后的帧，配对正式 IRQ 尾部 `schedule()` 与原任务复归，断言每个任务至少一个有效样本。后半段在 A 已进入编辑、启用 IRQ 的测试观察点调用 `blocker_wait()` 让出单核。B 在计时 IRQ 的 EOI 后被置一次 pending，handler 与 sigreturn 恢复哨兵。B 的 sigreturn 观察点发布完成条件并唤醒 A，A 续跑验证临时内核根和 edit end 后状态。用单一汇总 `[user-runtime] PASS case=irq cpu_mask=1` 结束。
- [ ] 在 aarch64 profile 将 selector 映射为 `kernel/user-runtime-irq`、`image/user-runtime-irq`，要求 `KERNEL_SELFTEST=1` 且与其他 variant 互斥，`mk/project.mk` 透传。通过 `kernel/Makefile` 条件编译 fixture 和内核观察代码，沿用 `test_aarch64_scheduler_run()` 所在的已完成 BSP/AP 接纳后测试入口，runner 接收明确的 ELF/镜像/固件/QEMU 路径。runner 单测只验收一份成功记录和一份重复必需记录的失败。给 `test_aarch64_user_runtime.py` 添加 harness `TEST_MANIFEST`（扫描器对新 `test_*.py` 强制要求），runner 作为 `test-aarch64` mode 的驱动，由 make 显式调用。运行 `make regen-test-catalog`、`make test-harness` 及该 mode，确认旧回路因无正式 IRQ switch 而失败。
- [ ] 修改 `aarch64_el0_irq()`，对设备分派计入 IRQ 深度并在返回后恢复。沿用 `el1_irq()` bracket。沿用活跃 legacy 探针在 EOI 后的 timer continuation。正式路径再调 `aarch64_user_irq_return()`。该函数验证拥有帧和本地可调度条件，acquire 读 `need_resched`，有请求时调一次 `schedule()`，保留 scheduler 的消费点。重新验证当前任务、原帧和当前根后投递信号。局部 IRQ 保持屏蔽直至汇编恢复，信号内部 VM 准备可临时开 IRQ。测试观察点只在该 variant 编译，普通路径的 GIC dispatch 次数、EOI 所有权与 sync 链不动。
- [ ] 运行 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=irq`，记录 PASS 及 EOI → switch → resume 样本。运行 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`，见原 28/28 和 failed=0 后有序退出。保留旧 `aarch64_el0_preempt` 探针与正式 IRQ **设备分派区间**不切任务的观察。执行 `make PROFILE=aarch64-clang test-aarch64-audit`。提交 A2。

## B. ASID、空根与失效边界

**接口**

- `uint8_t aarch64_asid_alloc(void)`。`void aarch64_asid_release(uint8_t asid)`，调用前保证根已卸载，内部完成失效再交还非零标签。
- `uint64_t aarch64_user_ttbr0_value(const struct mm_struct *mm)`，由正式 `user_mm` 记录中的 PA 和 ASID 编码。
- `void aarch64_user_install_mm(const struct mm_struct *mm)` 替换正式 `aarch64_user_install_root(uint64_t)`。`int aarch64_user_mm_quiesce(struct mm_struct *mm)` 在离线拆页前验证空闲并完成失效。旧探针接口沿用。

- [ ] 加一个宿主 ASID pool/回收用例。取满 255 个非零且不重复，第 256 个为 0。释放一个非零，TLBI 完成回调内尝试重新分配仍获 0，完成后重新分配得到同一个编号。现有页表夹具增加用户叶 nG 断言，保留内核叶 global。先运行 `make regen-test-catalog` 和 `make test-host` 记录 RED。
- [ ] 在 `user_asid.c` 实现锁保护的 8-bit 池。失效完成前池位仍被占用，执行 TLBI 时不持池锁。标签在 `aarch64_user_mm_create()` 成功路径登记到既有 `user_mm_record_t`。`aarch64_user_mm_destroy()` 在 busy/leaf/metadata 校验通过后先使翻译失效，再摘注册、回收页表和归还标签。标签释放可复用已完成失效的生命周期步骤，禁止先公布空闲再失效。对于 `user_image_destroy()`，在 `user_vm_destroy()` 前调用 quiesce。ASID 0 退化路径用本核完整切换失效和 inner-shareable 全量撤销。用 IRQ-safe 锁序，TTBR0 解析不在锁内执行 TLBI。
- [ ] 建立仅普通高半区运行期使用的共享空 TTBR0 根。BSP 现有 `aarch64_user_runtime_init()` 在启动 probe 已结束后用 PMM 分配/清零并安装，根常驻。确认这之后所用的设备和内核地址均经 TTBR1。首次安装执行本核全量失效。AP 暂无正式用户执行，本任务承担 BSP 路径，C 将其复用于 AP scheduler 发布前。现有内存计数从运行期初始化后取基线。
- [ ] 修改 `page_table.c` 的用户叶编码（含 stash 恢复及设备叶），活跃低 VA 改为 all-ASID VA 操作（`dsb ishst → tlbi vaae1is → dsb ish → isb`）。高 VA 维持已有失效/共享 shootdown。审计 `arch_flush_tlb_page()` 低 VA 调用，统一进入 all-ASID 语义。保持已有 BBM。正式根换入前发布，换出后撤销，使用 PA+ASID 完整身份。非零干净切换只做寄存器写和必要屏障。VM edit 确保先装空根、按 ASID 完成失效，再开始编辑。更新 fork/exec/exit/VM/uaccess 的读根、安装与卸载调用，保留只取根 PA 的旧调用约定。
- [ ] 扩展同一 runner 的 `USER_RUNTIME_CASE=asid` 单核 variant。两个任务以 `MAP_FIXED` 将各自 `0x40000000` 放不同数据，IRQ 切换中验证数据与两个非零 ASID。检查两根干净切换无全刷。用一次同 VA 替换检查新内容。记录销毁的卸载 → 失效完成 → 首次释放（及再用）顺序。通过 `[user-runtime] PASS case=asid cpu_mask=1` 汇总。
- [ ] 运行 `make test-host`、`make PROFILE=aarch64-clang test-aarch64-audit`、`make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=asid` 和 A2 的 M4 回归。提交 B。

## C1. 跨核运行所需的资源交接

- [ ] 针对退出交接，扩展 `test_switch_complete.c` 及其 fixture。写一个受控用例依次安排① 父先重挂，后到的子发布 ZOMBIE 后走自收割。② 子先发布 ZOMBIE，父重挂发生在子 switch tail 最终清 `on_cpu` 前，最终责任只归 tail。保留 fixture 原有清 `on_cpu` 后毒化对象的检查。若现有 fixture 缺少实际状态机，联编下述生产 helper，不在测试中重写状态机。运行 `make test-host` 记录 RED。
- [ ] 在 `kernel/sched/core.c`、`kthread.c`、`kernel/include/sched/{task.h,internal.h}` 和 `hosttests/mock/{blocker_runtime,sched_notify_test_runtime,switch_complete_test_runtime}.h` 接入 `bool task_finish_switch(task_t *prev)`，返回已领取的 self-reap 责任。统一 `task_list_lock → rq_lock` 序。尾部在清 `on_cpu` 前捕获资源指针，在锁内确定责任。尾部只有领到责任时才根据捕获的资源指针取 free-begin token 和释放资源。保留清 `on_cpu` 后无 prev 解引用的既有观察。增加 `void task_publish_process_exit(task_t *self, task_t *reparent_to, uint64_t status, list_t *reap_head)`，锁内原子确定 ZOMBIE、重挂与收割去向，已离核的孤儿进 `reap_head`，仍在核的孤儿由 tail 收割。在子已可收割后唤醒等待中的父任务。在 aarch64 进程退出中摘下 files 引用并在锁外 unpin，清旧根/映像后使用新的 exit helper，再释放 `reap_head`。审核 x86 switch tail 兼容和既有 waitpid 可见顺序。
- [ ] 修正 `user_vm_policy.c` 的 `cow_private_page()` 和 `user_vm_prepare_copy()` 拷贝提交路径。将旧叶切换且失效完成后才 release 旧 PA。`last=true` 时释放旧 PA，唯一引用升级分支继续保留旧 PA。更新 `user_vm_fork.c` 的发布注释，核查失败回滚仍独占子根且发布成功后只释放父侧暂存数据。用既有 VM 假后端模拟一次 refs>1 决策后归还最后引用，断言旧页释放一次而新内容保留。
- [ ] 在 `sched/signal.c` 的 PID/进程组发送及 `sys_native.c` 的广播路径用 release fetch-or 更新 pending。保留 task_list 保护目标生命周期与现有唤醒协议。等待侧对 pending 使用原子读，与 aarch64 IRQ/SVC 的 fetch-and 消费配对。核查共享调度、poll、mutex 和 x86 投递涉及的实际读改写，必要处使用原子操作且维持原来的掩码与交付语义。复用 M4 信号和 x86 syscall 回归。
- [ ] 在启用多核前审计共享文件引用和时间路径。fork 的 fd 表持有同一 `file_t`，用已有 `mutex_t` 序列化共享 `FD_VFS` 的隐式文件位置操作（read/write/append/lseek/getdents）。锁随 open-file 初始化，持引用、线程上下文取锁，覆盖 offset 取样到提交，延用短读写及用户复制失败时的提交规则。锁序为 open-file → VFS 实现的锁，append 仍由后端确定 EOF 并更新 offset。pipe/PTY 和无位置设备沿用自身同步。复用文件读写宿主回归，并核查 QEMU virtio-blk 的轮询完成与本地 IRQ、BSP-only 全局计时。确认这两条实际路径可被 AP 的 syscall 使用。
- [ ] `aarch64_elf_sync_icache()` 使用 inner-shareable I-cache 同步，新代码写入遵循 clean to PoU、完成、`ic ialluis`、完成、目标核上下文同步。扩及 `user_vm_policy.c` 中从 backing 新填可执行页和既有 resident 首次转为可执行的路径。fork 子映像沿用已完成同步的文本，审计其可执行叶处理，普通 kernel 使用原有 ARM 配置。
- [ ] 运行 `make test-host`、`make PROFILE=aarch64-clang test-static CASE=sched-common-compile` 和 A2 的 M4 回归。记录完整，提交 C1。用户放置门仍保持单核，C2 一并开启并验证。

## C2. AP 接纳、2 核验收和产品启动

**接口**

- `int aarch64_user_pick_cpu(void)`，运行期返回轮转选中的 ready CPU，boot-5 特例返回 0，无合法目标返回负 errno。
- `void aarch64_user_runtime_init_cpu(void)`，复用 BSP 初始化的空根，在本核准备 TTBR0/FPEN/IRQ 局部状态。启用前发布共享空根。
- `test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=smp|product`。smp 是隔离 variant，product 用普通镜像。

- [ ] 扩展 gate 现有宿主夹具。一次两核 setup 内断言 IPI 未全部 ready 时 mm 操作 `-EAGAIN`，BSP boot 窗口只可选 0，AP `scheduler_ok` 发布后两次运行期选核依次 1、0。用现有 runner parser 规则增加两核 fixture 数据要求，用 C 阶段启动前的配置观察 RED。
- [ ] 复用 `ap_sched.c::aarch64_ap_idle_handoff()` 既有 FPEN 位置，调用 runtime init CPU，在 scheduler_ok 发布前换空 TTBR0 并失效。将活动根和 IRQ/window 槽固定到当前 CPU，阶段 B 的共享空根由 BSP 先初始化。把用户 mm runtime check 改成 SMP 开始 → DTB 目标 IPI 全部就绪 → 本 CPU online/ipi_ready/scheduler_ok（BSP boot creation 特例）的顺序。扩容 registry 到 `2 * NR_CPUS + 8`，保持 publish/unpublish 的锁与顺序。给正式 EL0 校验与 copy precheck 补本核 readiness、单比特亲和及 PA/ASID 检查。旧 raw EL0 probe 仍限单核。
- [ ] 在 `vmm_gate.c` 实现选核策略并提供声明，正式 fork 与 VFS spawn 分别从 `user_task.c` 和 `sched/exec.c` 调用。轮转游标初始 `1`，在同一 ready 快照按 CPU ID 回绕，成功选择时以原子更新推进游标。将结果传给 `sched_alloc_and_publish_task(cpu, true, PF_PROCESS, ...)`，在现有锁内复核目标。fork 在发布准备完毕前不让远核见到对象。发布后只用已拷贝 PID 等值。exec 及唤醒维持原亲和。移除 `kernel/init/userspace.c` 的 aarch64 kernel-only 分支与废弃标签。
- [ ] 复用 fixture 的 2 核场景。在专用测试变体中复用嵌入 ELF 构造与 `sched_alloc_and_publish_task(..., true, ...)`，给夹具提供显式 CPU 的内部发布入口。把父任务及两个同伴分别固定到 CPU 0、0、1，夹具发布不推进轮转游标。父任务走正式 `SYS_fork`，断言首个子任务落到 CPU 1。按 CPU 记录真实 CNTP 的 EOI、两 PID 之间切换和恢复。父子对同一匿名 COW 页写不同哨兵，测试钩子在拷贝路径完成 memcpy 后、放弃旧引用前让二者都到达有上限的栅栏。检查旧 PA 只由最后一次 release 释放。子任务退出前等待观察点确认父以 `BLOCKER_WAITPID` 阻塞且已离核，随后验证父被唤醒并由 `waitpid` 获得子退出码。根和页回到回收基线。让栅栏以实际到达 CPU ID 区分两方。预期唯一 `[user-runtime] PASS case=smp cpu_mask=3`。其余 signal/exec/VM 组合继续复用旧 M4。多核普通镜像的 AP shell 路径验证跨核执行。
- [ ] 用同一 runner 从普通 `aarch64-uefi` 以 1/2/4/8 CPU 启动，使用现有 virt/GICv2、virtio 块设备和自动诊断 DTB 启动配方，省略 `-device ramfb` 以触发 `/bin/terminal` 的串口回落。要求 PID 1、inittab、`OS01 Shell` 和第一次 `$ `。2 核串口发送 `echo hi` 和 `exit`，匹配独立输出行 `hi` 与新 shell 的 banner/prompt。用 `llvm-nm` 确认正常产物无 `aarch64_user_runtime_test_*` 和 fixture 符号。运行 `make test-host`、`make test-harness`、`make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=smp` 和同命令的 `USER_RUNTIME_CASE=product`，提交 C2。

## 合入前门禁

- [ ] `make PROFILE=aarch64-clang test-aarch64-audit`，以及 `make PROFILE=aarch64-clang test-aarch64 MODE=<mode>` 分别执行 `smp`、`gic-spi`、`sync-fault`、`sched`、`sched-ap`、`sched-tick`、`sched-wake`。运行 A2 的完整 M4 单核门，按归档记录实际核数、条件与结果。
- [ ] 在 x86_64 执行 `make test-host`、`make test-static`、`make test-qemu SUITE=phase-0`、`make OS01_SYSTEST=1 test-syscall` 和独立的 `make KERNEL_SELFTEST=1 test-kernel-selftest`。审计共用 switch 收割与 IRQ dispatcher 无回归。相关失败先定位再启用 C 阶段。
- [ ] 实测通过后更新 `docs/roadmap.md` 对应 dispatch/M4 两条、`docs/arch.md` 的用户态与 IRQ/ASID 边界、`docs/build/build.md` 测试入口和 `docs/changelog.md`。记录固定亲和及验收范围，并链接证据。提交文档。
