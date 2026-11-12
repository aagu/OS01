# AArch64 M4 正式用户任务与 ELF 装载实施计划

> **执行方式**　用 `superpowers:executing-plans` 按顺序实施。每项验证沿用当前 profile 和现有测试入口。

**目标**　由真实 BSP 调度器启动两个各自拥有私有 `mm` 的 EL0 任务，验证主动让出、ELF 替换及退出回收。

**架构**　内存 ELF 构造器生成未安装的映像，通用调度器发布任务，AArch64 切换入口管理安装状态。用户异常保存在当前任务的内核栈。替换映像只在构造成功后提交。

**技术栈**　C、AArch64 汇编、Clang/LLD、GNU Make、现有宿主和 QEMU UEFI 测试。

**Spec**　[正式用户任务与 ELF 装载](../specs/2026-11-12-aarch64-m4-user-task-design.md)

## 全局约束

- QEMU virt Cortex-A53、单核 BSP、ASID 0、静态内存 ELF 和 4 KiB RAM 页。
- 真实用户任务使用 `PF_PROCESS`，拥有 `task->mm` 和内核栈。M4 探针仍走原入口。
- 叶子修改和释放发生在根切离后，并且本地 IRQ 开启。IRQ 不做调度。
- 生产入口编入普通 AArch64，启动开关和旧内核线程语义保持现有契约。

## 审查重点

- 恶意或截断 ELF 必须在映射前拒绝。任务 1 复用布局测试并在任务 4 从真实 SVC 覆盖错误映像。
- 已装载一页后失败须回滚所有新资源。任务 4 注入一次失败并核对旧映像和 PMM 基线。
- 让出时保存真实任务栈，返回前安装正确根。任务 4 逐 PID 核对让出、恢复及同 VA 的独立数据。
- 退出须先切回内核根，释放工作由下一个任务栈接手。任务 4 观察真实 switch tail 和后续内核线程。
- 普通镜像须连通低 EL 向量和正式装载入口。任务 4 审计普通产物，并排除探针内容。

## 任务 1　构造独立用户映像

文件为 `kernel/arch/aarch64/memory/{user_elf,user_image}.c`、`kernel/include/arch/aarch64/{user_elf,user_image}.h`、`kernel/Makefile` 和必要的 ELF 头文件。

- [ ] 提取探针 loader 与正式 loader 共用的 AArch64 ELF 检查和指令缓存步骤。在 `user_elf.c` 分离生产 helper 与现有探针入口。公共预检先将文件头复制到对齐的局部对象并检查程序头范围，再用 slab 分配对齐的程序头数组。最多 32 项，装载者在成功或失败时均释放。早期探针已在 PMM 初始化 slab 后运行。普通产物受 512 B 栈帧审计约束，程序头数组和映射清单都放在堆上。给正式入口启用 TLS 段拒绝，保留旧探针已承诺的格式及权限检查。将 `fs/elf_layout.c` 加入普通 AArch64 的白名单。
- [ ] 在 `user_image.h` 声明不透明的 `struct aarch64_user_image` 和 `int aarch64_user_image_load(const void *bytes, size_t size, struct aarch64_user_image **out)`、`void aarch64_user_image_destroy(struct aarch64_user_image *image)`、`mm_t *aarch64_user_image_mm(const struct aarch64_user_image *image)`、`void aarch64_user_image_init_regs(const struct aarch64_user_image *image, pt_regs_t *regs)`。成功构造时交付映像，失败清空输出。构造和销毁从本地 IRQ 开启的 EL1 线程上下文调用。
- [ ] 用第一块的 `aarch64_user_mm_create/destroy` 和 M3 接口建立映像。`mm->pgdir` 存放根 PA。M3 与 `aarch64_user_mm_root_busy/owns_root` 接收 `Phy_To_Virt((uintptr_t)mm->pgdir)`，安装和发布接收根 PA。在私有结构里记录最多 16 个装载页和 1 个栈页的 VA/PA。先清零再拷贝，使用精确语义权限掩码。栈顶为 `0x20401000`，`0x203ff000` 保持未映射。首次帧初始化为零，填 PC、SP 和 `SPSR_EL1=0x340`。先查或检查根是否 busy，再按逆序解映射，验证返回 PA 后才释放。
- [ ] 编译普通 AArch64 产物并运行已有 `test_elf_layout.elf`。复用的探针 loader 在任务 4 同次 QEMU 回归。

## 任务 2　从调度器进入并恢复正式 EL0 任务

文件为 `kernel/sched/kthread.c`、`kernel/include/sched/internal.h`、`kernel/arch/aarch64/cpu/{task_arch,user_task}.c`、`kernel/include/arch/aarch64/{context,user_task}.h`、`kernel/arch/aarch64/intr/{entry.S,trap.c,intr_decode.c,dispatch.c}`、`kernel/arch/aarch64/memory/vmm_gate.c` 及对应头文件、`kernel/arch/aarch64/{intr/gic.c,platform/pl011.c}`。

- [ ] 从 `kthread_alloc_and_publish()` 提取通用栈／线程分配、身份初始化和发布函数 `sched_alloc_and_publish_task()`。签名为 `int sched_alloc_and_publish_task(unsigned cpu, bool pinned, uint64_t flags, const char *name, int (*prepare)(task_t *, void *), void (*discard)(task_t *, void *), void *arg, task_t **out_task, int *out_pid)`。prepare 失败自行清理，discard 只在 prepare 成功后发布失败时运行。正式用户创建方保留映像及私有对象所有权直到发布成功，失败由创建方释放。仍按 task-list → runqueue 加锁并在锁下重查 CPU 准入。只让原内核线程使用原 boot fallback。保留其文件/FPU 资源处理和观察点。抽取 `_Noreturn void task_exit_self_reap(uint64_t status)` 为标记、摘链和永久切出的公共尾部，`kthread_exit()` 保留自己的调用者检查及文件清理。
- [ ] 定义 `struct aarch64_user_elf_source { const void *bytes; size_t size; };` 和 `int aarch64_user_task_spawn(const struct aarch64_user_elf_source *catalog, size_t count, size_t index, const char *name)`。返回 PID 或负 errno。通过单核运行期和 scheduler-ready 检查后构造映像，再使用通用发布函数。AArch64 `thread_t` 末尾增加用户任务所有者指针，持有映像和目录引用，更新 `AARCH64_THREAD_SIZE` 及布局校验。父指针使用常驻 BSP idle。任何发布失败都释放映像、私有状态和线程资源。
- [ ] 用 `stack_top - PT_REGS_SIZE` 作为首次帧及首入 SP。首入 `aarch64_user_first_entry` 调用 `arch_aarch64_entry_prepare()` 后跳到共享 `restore_pt_regs`。确保 switch tail 和首入调用使用帧下方的栈空间。现有 `pt_regs_t` 布局保持不变。
- [ ] 在 `arch_aarch64_bsp_handoff()` 的调度就绪发布前调用 `aarch64_user_runtime_init(void)`，保存内核 TTBR0 并显式设置 FP 陷入。让 `arch_task_switch()` 在下一栈的 switch tail 前调用 `aarch64_user_switch_mm(const task_t *next)` 处理真实用户任务相关的切换。共用内部的 IRQ-masked 安装函数，供任务 3 替换和退出使用。读取实际安装状态，按 spec 的发布、写 TTBR0、全 TLBI、取消发布顺序操作。把 `aarch64_pt_root_unpublish_local()` 提升为运行期接口。清除两个 EL0 线程指针寄存器。
- [ ] 普通镜像使用 GIC、PL011 的高地址别名。同步向量保留现有入口，直到任务 3 实现正式同步处理。EL0 IRQ 的槽位解码和来源检查编入普通构建。正式分支复用 CPU、scheduler-ready、当前用户任务、EL0t 和实际 TTBR0 校验，拒绝未识别的来源。GIC dispatcher 仍是唯一 IAR/EOI 所有者。旧 probe timer callback 继续由测试宏和活动探针检查约束。
- [ ] 先编译链接普通和 EL0 自测配置，确认本任务可独立构建。运行现有定向宿主入口 `test-kthread-ctor`、`test-switch-complete`、`test-sched-layout-offsets`。必要时同步更新受影响 mock 的声明。第一块的根门控和页表调用审计继续通过。

## 任务 3　接入协作 SVC、映像替换和退出

文件为 `kernel/arch/aarch64/cpu/user_task.c`、`kernel/include/arch/aarch64/user_task.h` 和上一任务的低 EL 路由。

- [ ] 接通低 EL 同步向量及 `bool aarch64_el0_sync_route(pt_regs_t *regs, uint64_t esr, uint64_t far)`。只有已确认活动的旧探针回调返回 false，汇编才进入 `.Lel0_done` 并展开其旧栈帧。正式分支先确认 CPU 和调度状态可读，再验证当前任务、EL0t 和实际 TTBR0。普通和自测构建中，未识别的入口均停机，不能借用探针 continuation。定义 `void aarch64_user_task_sync(pt_regs_t *regs, uint64_t esr, uint64_t far)`，正常返回由路由交给共享恢复路径，退出分支不返回。只对 AArch64 SVC EC 解码三个 immediate。yield 调用 `schedule()` 后写 x0=0，未知 SVC 写 `-ENOSYS`，其余帧保持原样。非 SVC 同步异常走当前任务的故障退出，用 `aarch64_sync_far_valid()` 判定 FAR 是否有效，用 `log_err` 记录 ESR/FAR，状态为 `(uint64_t)-EFAULT`。
- [ ] exec 用 x0 作为目录索引。检查完整索引范围后开 IRQ，调用映像构造器，再关 IRQ。失败只改旧帧 x0。成功在IRQ 屏蔽区发布并安装新根、撤销旧根、更新所有者及 `task->mm`，再用新映像初始化异常帧。清除 TLS 寄存器。随后开 IRQ 销毁旧映像，关 IRQ 返回。这个路径没有调度点。
- [ ] 退出用 x0 的原始值。关 IRQ 切回内核 TTBR0，完成 TLBI 并取消发布，清除任务映像和 `mm` 引用。开 IRQ 释放映像及任务私有状态，进入 `task_exit_self_reap()`。退出路径不会继续 ERET，下一栈的 switch tail 回收线程和内核栈。
- [ ] 构建普通和自测变体，确认两者共用正式入口。任务 4 通过真实 EL0 SVC 验证语义。

## 任务 4　一次单核 QEMU 验收与普通产物审计

文件为 `kernel/selftest/test_aarch64_user_task.c`、`kernel/selftest/fixtures/aarch64_user_task.S`、`kernel/selftest/{selftest,test_aarch64_scheduler}.c`、`kernel/include/arch/aarch64/boot.h`、`kernel/Makefile`、`qemutests/{intr_dispatch_audit,test_intr_dispatch_audit}.py`、`mk/components/run.mk` 和 `docs/build/build.md`。测试观察点还涉及 `kernel/arch/aarch64/intr/trap.c`、`kernel/arch/aarch64/cpu/{task_arch,user_task}.c` 和 `kernel/arch/aarch64/memory/user_image.c`。

- [ ] 复用静态 fixture ELF 的编译、链接、嵌入方式，生成同一 VA 布局的 A、B 和替换映像。用不同数据初值区分 A/B，另备一个头部无效的目录项。只在 `AARCH64_EL0_TEST` 构建这些映像和自测发起方。在现有 `aarch64_user_mm` 后记录 `aarch64_user_task_lifecycle`，只把这个变体的 late 期望加 1。
- [ ] 从已就绪 BSP 启动 A 和 B。用测试观察点按 PID 记录真实切换及 SVC 往返，确认 EL0 让出后数据、栈值、寄存器及继续 PC 正确。两份 fixture 在 exec 或 guard 访问前各做 64 次有界 yield。每次选中的 fixture yield 返回前在 IRQ 屏蔽状态写 `CNTP_TVAL_EL0=1`，令真实 CNTP 在恢复 EL0 后计入 vruntime。下一次 yield 前核对该 PID 已收到该轮 tick，区分相同 PID 的自选与切到对端。两个 fixture 都必须从实际切换中恢复并通过状态校验，否则本用例失败。以 `arch_intr_dispatch()` 前后 `aarch64_el0_timer_ticks()` 的增量确认真实 EL0 CNTP 和 GIC EOI 完成。IRQ 分派前后 PID 和切换计数应相同。
- [ ] 在 A 中依次执行无效 ELF 替换、一次性分配失败和成功替换。仅在测试变体为匹配 PID 的指定 exec 在成功映射一页后注入 `-ENOMEM`。失败后检查 errno、原 PID、根、继续 PC、寄存器和 PMM 计数。成功后检查 PID 不变、旧根归属消失、新 PC/SP 和清零帧。B 访问 guard，验证 Data Abort、翻译错误及 FAR。按 PID 在现有 reap 观察点释放前读取 `exit_code`，校验 A 和 B 的原始状态及真实资源回收，最后运行一个内核见证线程并确认 PMM 基线。
- [ ] 先执行 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi`，再执行 `timeout 20s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`，保留日志。确认 `[TEST] PASS aarch64_user_task_lifecycle` 和 `[TEST] END suite=kernel-selftest total=21 passed=21 failed=0 skipped=0` 后可结束 QEMU。更新构建文档中的总数与说明。
- [ ] 运行普通构建的 `make PROFILE=aarch64-clang test-aarch64-audit`。保持现有符号排除规则，并正向检查正式 loader、spawn 和根取消发布已链接。验证 VBAR +0x400/+0x480 的实际跳转及到同步路由和 IRQ 分派的可达路径。普通 ELF 排除旧探针入口、测试观察／故障注入符号和嵌入测试 ELF。更新 AArch64 合成审计 fixture 并增加缺失向量边、错误路由和探针泄漏的负例，运行 `python3 -m unittest qemutests.test_intr_dispatch_audit`。宿主验收通过下列命令运行受影响目标。

```sh
make -s -f Makefile -f - PROFILE=x86_64-clang m4-user-task-host-check <<'MAKE'
.PHONY: m4-user-task-host-check
m4-user-task-host-check:
	$(call os01_submake,hosttests,test-kthread-ctor test-switch-complete test-sched-layout-offsets test_smp_starting_gate test_vmm_caller_audit $(HOST_TEST_BUILD_DIR)/test_elf_layout.elf)
	$(HOST_TEST_BUILD_DIR)/test_elf_layout.elf
MAKE
```

各任务结束时检查 `git diff --check`。提交按用户指令进行。
