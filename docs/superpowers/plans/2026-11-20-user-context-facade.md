# 用户态进入与返回门面实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal**　将 x86_64 和 aarch64 的正式用户态首入与返回接入 `<arch/context.h>`，共用返回工作顺序。

**Architecture**　架构强定义准备新任务的保存 SP/PC，并在各自汇编中提供同名的原生恢复目标。共享 C 编排按阶段调用静态绑定的钩子，x86_64 的强 softirq 定义覆盖弱默认，aarch64 使用弱默认。原生适配器在原调用位置选择完整工作或仅信号阶段。

**Tech Stack**　freestanding C、预处理汇编、Clang/LLVM ELF 工具、Python unittest、QEMU UEFI。

**Spec**　[用户态进入与返回门面设计](../specs/2026-11-20-user-context-facade-design.md)，通过评审的 Git blob 为 `e3dafee4b048ad9b56b99d9a25dea7bc342d1b56`。

## 全局约束

- 保持 `thread_t`、`task_t`、`pt_regs` 布局与 32 KiB task stack。
- 保留现有 IRQ 屏蔽、栈归属、ACK/EOI、switch tail、活跃页表和 TLS 切换顺序。
- 恢复块保持寄存器、段和系统寄存器恢复顺序及 `iretq` / `eret` 指令。
- 共享代码不按架构编译分支。静态链接解析差异，只有 `arch_context_run_deferred_work` 提供弱默认。
- x86_64 syscall E2E 与内核自测分别从顶层发起。aarch64 普通 ELF、M4 和四种 user-runtime 验收使用各自的构建入口。

## 实施工作树

实施启动时按 `superpowers:using-git-worktrees` 从干净的基线创建隔离工作树，记录基线 SHA。若已评审的文档仍是工作区文件，只将这两份文档带入实施工作树。在该工作树运行 `regen-test-catalog`，先核对生成 diff 仅登记本项测试，再纳入本项提交。当前工作区的测试目录、生成物与其他改动由原工作保留。

- [ ] 在隔离工作树执行 `git submodule update --init --recursive`，通过 `git submodule status --recursive` 核对工作树使用基线固定版本。x86_64 sysroot 及双架构 UEFI 的源码依赖按 [构建指南](../../build/build.md) 准备。
- [ ] 记录 `make --version`，在本基线使用已核对的 GNU Make 4.4.1，确保 aarch64 profile 中导出的 `AARCH64_BOOT_BLOCK_BASE` 能进入 `$(shell ...)` 的校验。分别运行 `make PROFILE=x86_64-clang validate-profile` 和 `make PROFILE=aarch64-clang validate-profile`，两者应退出 0。实施前记录基线依赖或工具链失败，验收时用同配置区分改动前后的结果。

## 评审重点

| 条件 | 对应可验收结果 |
|---|---|
| 初次 spawn 或 fork 构造、子返回值和遗留 `PF_KTHREAD` 赋值 | 任务 1 的生产调用点与宿主 publish 证据 |
| x86_64 task-stack 上的 CPL0 返回、trap gate 实时 IF 与可变 RSP 对齐 | 任务 3 的共享编排和适配器审计，任务 4 的 BOOT-2 与 syscall 证据 |
| x86_64 IST/NMI/spurious 和 aarch64 EL1h 可恢复 uaccess | 任务 2 的同址恢复及错误路由负例，任务 4 的原有自测 |
| aarch64 IRQ 在 EOI 后才准入，调度后帧归属失配 | 任务 3 的拒绝阶段测试与调用链，任务 4 的真实 IRQ 抢占证据 |
| aarch64 M4 probe 以改写后的 EL1h 帧恢复，SVC 未知编号与缺页不加信号 | 任务 2/3 的路由检查，任务 4 的 M4 与运行期证据 |

## 文件归属

| 位置 | 职责 |
|---|---|
| `kernel/include/arch/context.h`、`kernel/include/sched/task.h`、`kernel/include/arch/aarch64/user_task.h` | 声明公共交接点，收回原生内部符号的公共声明 |
| `kernel/arch/x86_64/cpu/task_arch.c`、`kernel/arch/x86_64/sched/{exec,fork}.c`、`kernel/arch/aarch64/cpu/user_task.c` | 准备首次线程继续点，保留原生帧构造和发布顺序 |
| `kernel/arch/{x86_64,aarch64}/intr/entry.S` | 提供强恢复标签，保留原生恢复块和特殊返回 |
| `kernel/sched/context_return.c`、`kernel/Makefile` | 实现并链接公共编排及弱默认 |
| `kernel/arch/{x86_64,aarch64}/intr/trap.c`、`kernel/arch/x86_64/intr/context_return.c`、`kernel/arch/aarch64/intr/syscall.c`、`kernel/arch/aarch64/cpu/user_task.c`、`kernel/include/intr/softirq.h`、`kernel/intr/softirq.c` | 接入现有返回点，提供架构强定义及 softirq 声明 |
| `hosttests/cases/test_boot_{spawn_failure,services_spawn,services_vfs}.c`、`qemutests/test_context_facade.py`、`qemutests/{intr_dispatch_audit,test_intr_dispatch_audit}.py` | 宿主发布、门面编译/链接及真实 ELF 路由回归 |
| `config/test-catalog.json`、`mk/generated/test-harness-modules.mk` | 由目录生成工具注册新 harness |
| `docs/{arch,roadmap,changelog}.md` | 记录实施落点和实测验收 |

## 任务 1　首次调度准备

**文件**　公共与两个架构的首次调度文件、三个既有 host case、新 harness 及其生成物。

**接口**　`void arch_task_prepare_user_entry(task_t *task, struct pt_regs *frame)`。

- [ ] 新建 `qemutests/test_context_facade.py`，使用 `TEST_MANIFEST`（`id=test_context_facade`，`layer=harness`，`subsystem=test-framework`，`tier=pr`，`timing=strict`，`scenarios=[]`）。参照 `test_boot_facade_headers.py` 使用 `clang -target <triple> -ffreestanding -Werror -Ikernel/include -fsyntax-only -x c -`，测试 `test_prepare_hook_header` 在 `x86_64-unknown-none` 和 `aarch64-none-elf` 下仅包含 `<arch/context.h>`，以函数指针赋值断言精确签名。测试 `test_production_prepare_callers` 检查 `spawn_user_task`、`do_fork` 和 `user_task_prepare` 函数体在帧填充后调用门面。检查 x86_64 的子帧 `rax=0` 先于调用，`PF_KTHREAD` 分支仍写入原保存 RSP/PC，aarch64 的子帧 `x0=0` 及 fresh-image 初始化均先于调用。检查公共 `sched/task.h` 不再声明 `ret_from_intr`。
- [ ] 运行 `python3 -m unittest qemutests.test_context_facade`，确认当前基线在新声明/调用点断言处失败。
- [ ] 在 `arch/context.h` 声明帧前向类型与门面函数。在 `kernel/arch/x86_64/cpu/task_arch.c` 增加强定义和局部 `extern void ret_from_intr(void)`，写 `task->thread->rsp = (uintptr_t)frame`、`task->thread->rip = (uintptr_t)ret_from_intr`。删除公共原声明。让 `exec.c::spawn_user_task` 传其已初始化的 `regs`。在 `fork.c::do_fork` 用同一个 `child_regs` 完成复制、写零与交接。只在非 `PF_KTHREAD` 分支调用门面，后者保留保存 RSP 为子帧、保存 RIP 为 `regs->rip`。RSP0/FS/GS 和入队仍由原调用者处理。
- [ ] 在 `kernel/arch/aarch64/cpu/user_task.c` 将 `aarch64_user_first_entry` 改为 `static`，收回 `aarch64_el0_restore` 的 C 声明并使用 `struct pt_regs *` 参数和 `noreturn`。实现强首入准备函数，写 `task->thread->sp = (uintptr_t)frame`、`task->thread->x30 = (uintptr_t)aarch64_user_first_entry`。在 `user_task_prepare` 使用一个 `pt_regs_t *` 构造新帧或复制子帧，完成后调用门面。首入 C 继续点仍先执行 `arch_aarch64_entry_prepare()`，取得当前帧、交付信号、调用恢复适配器。信号调用的编排迁移留给任务 3。
- [ ] 三个现有 host case 各自以门面替身替换其 `ret_from_intr` stub。替身将保存 RSP 设为帧地址、RIP 指向各用例内部的不可执行测试继续点。`test_boot_services_spawn.c` 增加观测并断言 CPU0 正常放行的 spawn 在入队前调用一次，`frame == (void *)((uintptr_t)task + STACK_SIZE - sizeof(pt_regs_t))`、`frame->rip == 0x400000`、`(frame->cs & 3) == 3`，以及入队时保存 RSP 与传入帧一致。测试替身继续让锁内资格复查失败触发原释放断言。
- [ ] 运行 `python3 -m unittest qemutests.test_context_facade`、`make regen-test-catalog`、`make check-test-catalog`、`make PROFILE=x86_64-clang test-host` 与 `make PROFILE=aarch64-clang test-aarch64-audit`。预期新模块已生成注册，三个 host case 均 PASS，两侧首入门面强定义进入正确构建，原 aarch64 普通 ELF 审计通过。
- [ ] 评审并提交本任务文件，提交说明 `refactor(sched): prepare user entry through arch context`。

## 任务 2　原生帧恢复门面

**文件**　`kernel/include/arch/context.h`、两个 `intr/entry.S`、两个中断 ELF 审计文件、`qemutests/test_context_facade.py`。

**接口**　汇编 `ARCH_CONTEXT_RESTORE_FRAME` 展开为强文本符号 `arch_context_restore_frame`。入口 RSP/SP 指向原生帧，控制流以分支交接。

- [ ] 在 `test_context_facade.py` 添加 `test_restore_symbol_assembler_only`。两 target 以 `-x assembler-with-cpp` 预处理仅包含门面头并引用 `ARCH_CONTEXT_RESTORE_FRAME` 的探针，断言展开为 `arch_context_restore_frame`，无 C 声明；两 target 的 C 编译探针断言该汇编宏不可见。源检查分别断言 x86_64 工作尾部和 aarch64 首入、lower-EL IRQ、lower-EL sync true 使用门面。probe false、EL1h sync、x86_64 IST/restore-only 仍走原本地标签。
- [ ] 扩充 `qemutests/test_intr_dispatch_audit.py` 的两份正例 ELF fixture，加入新强恢复标签、首入适配器及生产交接边。新增负例 `test_restore_alias_at_wrong_address`、`test_el0_first_restore_without_installing_sp`、`test_el0_irq_wrong_restore_target`、`test_el0_sync_wrong_restore_condition`、`test_aarch64_restore_frame_with_missing_gpr`。分别移动一个同址标签，删除 `mov sp, x0`，改 IRQ 目标、把 sync 的 `cbnz w0` 改为 `cbz w0`、删除一对 GPR 恢复，断言审计返回相应违规。保留原 restore-only 与 EL1 sync mutation 的拒绝语义。
- [ ] 运行 `python3 -m unittest qemutests.test_context_facade qemutests.test_intr_dispatch_audit`，确认新断言能区别旧实现或未检查的 mutation。
- [ ] 在 `<arch/context.h>` 的汇编部分定义 `#define ARCH_CONTEXT_RESTORE_FRAME arch_context_restore_frame`。在 x86_64 的 `RESTORE_ALL` / `x86_intr_restore_only` 同址处增加 `ENTRY(ARCH_CONTEXT_RESTORE_FRAME)`，把两个 `check_signal` 正常出口及 BOOT-2 收尾改用门面。aarch64 在 `restore_pt_regs` 同址处增加全局标签，从 `aarch64_el0_restore` 的 `mov sp, x0`、`el0_irq_entry` 及 `cbnz w0` 分支到它。保留 EL1h IRQ fallthrough、可恢复 EL1h sync 与 probe false 到旧标签的分支，保留同址的原生指令序列。两份 `entry.S` 直接包含门面头。
- [ ] 扩展 `qemutests/intr_dispatch_audit.py` 的 disassembly 白名单和返回验证。两个 ELF 都要求 `arch_task_prepare_user_entry` 和 `arch_context_restore_frame` 各有唯一全局强文本定义。验证 x86_64 三个恢复名同址、aarch64 两个恢复名同址，按已核实的地址匹配所有相关恢复边。取反汇编时允许同一个已验证地址的任一标签成为块名，原 NMI、spurious、IST selector、错误跳转等检查继续严格。x86_64 证明 `spawn_user_task` 和 `do_fork` 可达首入准备函数，并证明 `ret_from_exception` 直接恢复。aarch64 证明 `aarch64_user_first_entry` 调用/尾跳首入适配器，首入适配器设置 SP 后分支，EL0 IRQ 调用 C 后分支，EL0 sync 由 `cbnz w0` 分支，EL1 sync 的恢复边仍在。检查 aarch64 公共恢复块从偏移 248/256/264 恢复 SP_EL0/ELR_EL1/SPSR_EL1，从原位恢复 x30 与 x28…x0，最后 `add sp, sp, #0x110`、`eret`。匹配真实 LLVM 操作数表示，并拒绝额外工作调用。
- [ ] 重跑上面的 Python 测试。执行 `make PROFILE=x86_64-clang kernel.bin` 后执行 `make PROFILE=x86_64-clang test-intr-dispatch-audit`。执行 `make PROFILE=aarch64-clang test-aarch64-audit`。预期两份普通 ELF 的中断审计均通过，已注册的负例均拒绝。核对 C 构造器的首入继续点赋值与真实 ELF 的本机首入恢复边。
- [ ] 评审并提交本任务文件，提交说明 `refactor(arch): expose native context restore entry`。

## 任务 3　链接选择的共享返回工作

**文件**　`kernel/include/arch/context.h`、`kernel/sched/context_return.c`、`kernel/Makefile`、`kernel/arch/x86_64/intr/{context_return.c,entry.S,trap.c}`、`kernel/arch/aarch64/{cpu/user_task.c,intr/trap.c,intr/syscall.c}`、`kernel/include/arch/aarch64/user_task.h`、`kernel/include/intr/softirq.h`、`kernel/intr/softirq.c`、门面 harness 和中断审计/fixture。

**接口**

```c
#define ARCH_CONTEXT_RETURN_FULL 0

enum context_return_mode {
    CONTEXT_RETURN_FULL = ARCH_CONTEXT_RETURN_FULL,
    CONTEXT_RETURN_SIGNAL_ONLY = 1,
};

void context_run_return_work(struct pt_regs *frame, enum context_return_mode mode);
void arch_context_run_deferred_work(void);
bool arch_context_reschedule_return(struct pt_regs *frame);
void arch_context_deliver_return_signals(struct pt_regs *frame,
                                         enum context_return_mode mode);
```

- [ ] 给 `test_context_facade.py` 增加链接用例。原生 C 编译器以 `-O2 -Werror -Ikernel/include` 编译真实 `kernel/sched/context_return.c` 和临时 fixture。fixture 的重调度与信号实现记录阶段、帧指针和 mode，分别链接带或不带强 `arch_context_run_deferred_work` 的目标。断言下表的顺序与实参。另移除任一必需钩子的 fixture 定义，断言程序链接失败并报告缺失符号。双架构汇编/C 探针分别断言 FULL 常量值为 0 且和枚举一致。

  | 场景 | 期望顺序 |
  |---|---|
  | 强覆盖，FULL，通过 | deferred → reschedule(frame) → signal(frame, FULL) |
  | 强覆盖，FULL，拒绝 | deferred → reschedule(frame) |
  | 弱默认，FULL，通过 | reschedule(frame) → signal(frame, FULL) |
  | 强覆盖，SIGNAL_ONLY | signal(frame, SIGNAL_ONLY) |

- [ ] 加入中断审计变异测试。两架构反转正确的 softirq 绑定类型或将必需钩子变弱均失败，替换共享编排到信号的边、x86_64 的 RSP 恢复、aarch64 的 IRQ shared 尾边均失败。增加 `test_el0_irq_work_before_dispatch` 与 `test_el0_irq_work_before_vm_leave`，分别把共享调用移到分派和退出 VM IRQ depth 之前，均须失败。静态变异将 x86_64 syscall、aarch64 首入/native syscall/yield 的 SIGNAL_ONLY 实参改成 FULL，或把正式 IRQ 的 FULL 改成 SIGNAL_ONLY，均须失败。x86_64 恢复适配器删 `cld`、错传 RDI/ESI 或插入 `cli`/`sti` 均须失败。将原 aarch64 原生 syscall 信号 mutation 对应到新的完整链，允许它指出具体失败的边。
- [ ] 运行 `python3 -m unittest qemutests.test_context_facade qemutests.test_intr_dispatch_audit`，预期返回工作 TU、真实交接边和符号校验仍缺失，新增测试给出对应失败。
- [ ] 在 `context_return.c` 实现公共函数。FULL 调用 deferred，再调用 reschedule，false 时立即返回。通过或 SIGNAL_ONLY 时调用 signals。默认 deferred 定义加 `__attribute__((weak))`，头中所有声明保持普通声明。x86_64 的 `sched/*.c` 已收集该文件。在 `kernel/Makefile` 的 aarch64 `KERNEL_C_SOURCES` 加入 `sched/context_return.c`。
- [ ] 在新建的 x86_64 `intr/context_return.c`（现有架构 intr wildcard 收集）提供三个强钩子。deferred 仅在 `softirq_status` 非零时调用 `do_softirq(void)`，在原有 `<intr/softirq.h>` 补原型并将定义写为 `(void)`。reschedule 以两次 GS 相对操作读/按需清除 `PERCPU_NEED_RESCHED_OFFSET`，然后按原条件调用一次 `schedule()` 并返回 true。保留每次访存的实时 GS 选择，在 inline asm 上用 `volatile` 和 `memory` clobber。signals 的 FULL 以 acquire 读取 `current->signal`，非零时调用原信号函数，返回非零才重读继续，CPL0 也进入原清理逻辑。SIGNAL_ONLY 在 `regs->cs & 3` 时调用一次。在 `intr/trap.c` 用 SIGNAL_ONLY 替换 `do_system_call` 的原尾部信号调用，保持写回及 suppress-writeback 条件。TTY 仍直接调用 `arch_do_signal_delivery(NULL)`。
- [ ] 将 x86_64 `ret_from_intr` 的原工作循环替换为 ABI 适配器。保留入口及自测计数，然后 `cld`、保存原帧指针至 RBX、设 RDI 为该帧、ESI 为 0、对齐 RSP 到 16 字节，`call context_run_return_work`。BOOT-2 的 `boot2_test_restore` 保持编译门控及标签，在同一对齐栈把 RBX 中的原帧传给 `boot2_test_irq_return`。从 RBX 恢复 RSP 后跳至门面。此区段没有 CLI/STI，不修改保存帧。删除旧 `TASK_SIGNAL_OFFSET` 和 `trap.c` 中只服务该硬编码的断言。IST selector、NMI、spurious 的直接恢复维持任务 2 的路径。
- [ ] 在 aarch64 `cpu/user_task.c` 将 `aarch64_user_irq_return` 转为强 `arch_context_reschedule_return`。保留全部帧、VM edit、CPU readiness 检查、acquire load、调度前后观察及续跑二次校验。所有提前返回改为 false，末尾校验成功返回 true。删除旧声明。将首入及内部 yield 的原信号调用替换为 SIGNAL_ONLY。保留首入准备顺序与 yield 的 x0 写回、测试 tick 重装位置。
- [ ] 在 aarch64 `intr/trap.c` 提供强 signals 钩子，忽略 mode 并一次调用原 `aarch64_do_signal_delivery`（声明在 `<arch/aarch64/syscall.h>`）。在同文件用 FULL 替换 `aarch64_el0_irq` 中原尾部调用，位置保持在 `arch_intr_dispatch`、`aarch64_vm_irq_leave` 及 legacy probe 分流之后。在 `intr/syscall.c` 用 SIGNAL_ONLY 替换已注册 syscall 的原尾部交付，保持 no-handler 的提前返回及 VM leave、writeback 顺序。确认其他 internal SVC、可恢复缺页、EL1h 及 probe 的工作范围维持现状。
- [ ] 扩充实际 ELF 审计。证明两侧的 `context_run_return_work`、reschedule、signals 各有一个强文本定义，x86_64 的 deferred 为 `T`，aarch64 为唯一 `W`。aarch64 弱边用 `weak_defs` 上已验证的地址识别，保留现有强边解析。证明编排实际调用三个阶段和信号钩子到原信号函数的边。使用现有 `x86_edges` 处理直接调用及大代码模型的 `movabs`/间接调用。x86_64 证实 `ret_from_intr` 按 `cld`、保留原帧、传 RDI/ESI（FULL=0）、RSP 对齐、C 调用、从 RBX 还原 RSP、原生恢复的顺序执行，且该适配器没有 CLI/STI。证实 syscall 的共享调用传 SIGNAL_ONLY=1。aarch64 核对每条共享调用前的 w1：首入、native syscall、yield 为 SIGNAL_ONLY=1，正式 IRQ 为 FULL=0。在普通 ELF 中要求 `aarch64_el0_irq` 的 `arch_intr_dispatch` → `aarch64_vm_irq_leave` → FULL 调用依序出现，保留 IRQ 后端的原 GIC/EOI 检查。按实际调用/尾跳前的参数赋值识别编译器形状（aarch64 允许零寄存器表示 0，两个架构都拒绝对调用参数的后续覆盖）。证明 native syscall 的 `context_run_return_work` → signals 钩子 → `aarch64_do_signal_delivery` 链。
- [ ] 重跑 Python 门面、中断审计与 `qemutests.test_boot_irq_time`。按任务 2 的真实普通 ELF 命令重跑双架构审计。预期新阶段顺序、强弱类型及错误路由 mutation 通过，两个实际 ELF 通过所有检查。
- [ ] 评审并提交本任务文件，提交说明 `refactor(sched): share return work across architectures`。

## 任务 4　验收与路线图闭环

**文件**　`docs/arch.md`、`docs/roadmap.md`、`docs/changelog.md`。

- [ ] 先运行 `make check-test-catalog`，再运行 `make PROFILE=x86_64-clang test-host`，最后运行 `make PROFILE=x86_64-clang test-harness`。宿主构建先产出 `test_mock_core_dup` 需要的 `libmock-core.a`。要求目录无漂移，门面与中断 fixture 全通过，宿主用例无失败，既有 `test_sched_layout_offsets` 与三个直接链接 `exec.c` 的目标仍通过。
- [ ] 运行 `make PROFILE=x86_64-clang test-static` 和 `make PROFILE=aarch64-clang test-aarch64-audit`。要求无审计失败，记录两个普通 ELF 中首次调度和物理恢复强符号、共享返回工作以及 x86_64 `T` / aarch64 `W` 的 deferred 证据。aarch64 M4 镜像使用下面的运行验收，不送入普通 ELF 的 legacy/test 符号排除检查。
- [ ] 分别运行以下 x86_64 目标，确认运行器退出 0、既有用例无失败。检查 `intr-x86` 的 #BP/NMI 返回、BOOT-2 的原帧配对，及 syscall 中的 fork/exec/waitpid 和 `sigreturn saved rax` 证据。独立记录内核自测结尾及 uaccess 恢复标记。

  ```bash
  make PROFILE=x86_64-clang test-qemu SUITE=intr-x86
  make PROFILE=x86_64-clang test-qemu SUITE=boot-irq-time
  make OS01_SYSTEST=1 test-syscall
  make KERNEL_SELFTEST=1 test-kernel-selftest
  ```

- [ ] 预构建并启动 aarch64 M4 独立镜像。保存本次串口日志，等待完整 END 后停 QEMU。若外部超时结束，仅在此前已捕获完整 PASS/END，且无 FAIL/fatal 时接受 QEMU 在成功后仍运行的超时。检查 `aarch64_el0`、`aarch64_el0_irq`、`aarch64_el0_preempt`、`aarch64_user_task_lifecycle`、`aarch64_user_svc_uaccess`、`aarch64_user_vm`、`aarch64_user_fork_cow`、`aarch64_user_exec`、`aarch64_user_signal` 全部 PASS，以及 `[TEST] END suite=kernel-selftest total=28 passed=28 failed=0 skipped=0`。

  ```bash
  make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi
  timeout 90s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi
  ```

- [ ] 分别运行四个 aarch64 运行期目标。前三个要求对应 `*-samples` 和 `[user-runtime] PASS case=...`，CPU mask 分别为 1、1、3，按原 runner 核对 EOI → switch → resume、ASID/根和 SMP 任务所有权。产品模式核对 1/2/4/8 核到达 `OS01 Shell`，2 核串口 `echo hi`、`exit` 后 respawn，以及产品 ELF 测试符号排除。

  ```bash
  make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=irq
  make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=asid
  make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=smp
  make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product
  ```

- [ ] 在所有门禁通过后更新 `docs/arch.md` 中的上下文门面及各处待办说明，写清共享阶段、强弱链接与原生特殊栈边界。按路线图约定从未完成表及相关剩余列表移除本项，并添加近期闭环索引。向 `docs/changelog.md` 记录实际通过的命令、结果和串口证据，链接 spec/plan。记录真实的失败或未运行项，未满足验收时保留路线图待办。
- [ ] 逐项对照 spec，确认两侧的首入、异常返回、原有特殊返回以及所有验收证据齐备。运行 `git diff --check`，评审并提交本任务文档，提交说明 `docs: record user context facade validation`。
