# AArch64 M4 正式 SVC、异常分派和 uaccess 实施计划

> **执行方式**　用 `superpowers:executing-plans` 顺序实施，并按用户指令提交。

**目标**　正式 AArch64 用户任务经 `svc #0` 使用共享 syscall 表，并在自身 `mm` 下完成故障可恢复的用户复制。

**架构**　共用处理函数与表项，AArch64 只编入已支持的四项。低 EL 同步事件经公共分派确认来源，EL1 用户复制 abort 使用带任务和根校验的精确跳板。保留第 2 块的任务与启动契约。

**技术栈**　C、AArch64 汇编、Clang/LLD、GNU Make、现有宿主测试与 QEMU UEFI 自测。

**Spec**　[正式 SVC、异常分派和 uaccess](../specs/2026-11-12-aarch64-m4-svc-uaccess-design.md)

## 全局约束

- 首版为单核 BSP 上的正式用户任务。使用已发布、硬件安装的私有 `mm` 根。
- 保留 x86 原生及兼容 ABI，保留 AArch64 内部 immediate 和 M4 旧探针。
- 正式复制在 IRQ 屏蔽、不调度且不持锁的访存窗口执行。EL1 修复只处理两条精确用户指令。
- 普通 AArch64 产物含正式代码，自测专用入口及数据只进入旧自测变体。现有用户态启动契约保持原样。

## 审查重点

- 原生 AArch64 入口使用完整 x8。任务 4 验证高位污染、未知 immediate、未开放调用和返回帧。
- 错槽位、错模式、非标准帧或错误根不能进入用户处理。任务 2 用宿主负例验证。
- 内核侧访存或无窗口的 abort 继续致命。任务 3 覆盖纯匹配负例，任务 4 校验生产接线。
- 预检失败不能写入已映射前缀。任务 4 以跨未映射页哨兵验证。
- 真实 EL1 读写 abort 必须返回调用者并清窗。任务 4 核对故障标签、FAR、方向及回调，再执行成功复制。

## 任务 1　共用四个 syscall 表项及处理函数

文件为 `kernel/syscall/{dispatch,sys_native,sys_proc,sys_time}.c`、`kernel/include/arch/syscall.h`、`kernel/arch/x86_64/intr/syscall_frame.c`、`hosttests/mk/sched.mk` 和 `hosttests/cases/{test_syscall_dispatch,test_compat_linux,test_nofile_limits}.c`。

- [ ] 在 `test_syscall_dispatch.c` 为四个将提取的处理函数提供可区分的 stub。测试 x86 表仍覆盖原集合，空上下文和无处理函数仍返回 `-EINVAL`。增加同源 `dispatch.c` 的定向宿主配置，用 `OS01_SYSCALL_A64_SUBSET=1` 编译表并复用测试源，只断言 `SYS_exit`、`SYS_getpid`、`SYS_getppid`、`SYS_clock_gettime` 有处理函数，`SYS_read`、`SYS_getpeername`、越界和 `(1ULL << 32) | SYS_getpid` 均无处理函数。原表的子集断言应先失败。
- [ ] 在 `sys_native.c` 提取 `int64_t sys_native_{exit,getpid,getppid,clock_gettime}(syscall_ctx_t *ctx)`。时钟复用当前的时钟 ID 检查和 `copy_to_user_ft()` 语义。退出将 `(ctx->args[0] & 0xff) << 8` 交给 `_Noreturn void arch_syscall_exit(uint64_t status)`。x86 实现在 `syscall_frame.c`，保留写 `current->exit_code` 及调用 `do_exit()` 的行为。若 `do_exit()` 意外返回，屏蔽 IRQ 并停机，满足 `_Noreturn` 契约。AArch64 实现留到任务 3。
- [ ] 在 `dispatch.c` 只保留一份固定编号和名称的表，四项放在全量条件外。AArch64 或定向宿主子集配置隐藏其余项。更新 compat/nofile 宿主 stub，保留它们的记录和返回值。保留 `syscall_dispatch()` 的空／缺项错误及所有 x86 表项。用 `make PROFILE=x86_64-clang kernel.bin` 真实链接 x86 内核，并用 `make PROFILE=x86_64-clang test-syscall-boundary` 核对源审计，用文末定向命令运行全量、子集、compat 和 nofile 用例。AArch64 最终链接放在任务 3，届时其复制与退出接口齐备。

## 任务 2　正式低 EL 同步事件进入公共分派

文件为 `kernel/arch/aarch64/{cpu/user_task.c,intr/intr_decode.c,intr/dispatch.c,intr/trap.c}`、`kernel/include/arch/aarch64/{intr_decode,sync_fault,user_task}.h`、`hosttests/cases/{test_intr_decode_aarch64,test_intr_backend_aarch64}.c`、`hosttests/mk/intr.mk` 及 `qemutests/{intr_dispatch_audit,test_intr_dispatch_audit}.py`。

- [ ] 在现有宿主解码与后端用例中添加 EL0 sync 槽 `0x400`、SVC 无 FAR、上下级 abort EC 与保存模式矛盾的拒绝断言。给 Data Abort 添加 bit 7 抑制读写分类和 bit 9 单独置位仍按 WnR 分类的用例。给正式帧匹配添加同栈错地址、mm 根与映像根不同、实际 TTBR0 与 mm 根不同的拒绝用例。更新已有 bit 9 和 EL0 槽断言。在宿主后端用真实纯匹配函数实现 admission stub，并加 `aarch64_user_task_sync()` spy。合法 EL0 事件调用 spy 一次，错误槽位、模式、帧和根均不调用。为手写构建规则补 matcher 头依赖。
- [ ] 将 FSC、WnR、S1PTW、CM、FnV 放到 `sync_fault.h` 供解码和复制修复共用。解码根据 SPSR 推导预期槽，EL1 适配器始终填真实 `0x200`，EL0 路由填真实 `0x400`。后端重解码比较，验证模式及 EC 的一致性后才分发。EL1 致命分支保留既有 fatal 和 bad-event。
- [ ] 在 `user_task.h` 放可由宿主用例直接覆盖的纯匹配函数 `aarch64_user_frame_matches(regs, expected_frame, mm_root_pa, image_root_pa, installed_root_pa)`。要求帧地址精确相等，EL0t，三个非零根 PA 一致。复用现有正式任务身份检查，并提供 `struct mm_struct *aarch64_user_current_mm(void)` 供入口和任务 3 使用。该 helper 先确认 BSP per-CPU 指针及 scheduler-ready，再读当前正式任务，要求 `PF_PROCESS`、所有者、`current->mm` 和映像一致，并核对硬件 TTBR0。它不取锁、不启用栈 canary，不依赖普通日志。
- [ ] 将正式 `aarch64_el0_sync_route()` 接到 `arch_intr_dispatch()`，后端通过正式帧验证后才调用 `void aarch64_user_task_sync(pt_regs_t *regs, const struct intr_event *event)`。保留事件的 access 和 FAR validity，用户诊断以 `fault_addr_valid` 判定能否报告 `fault_addr`。旧探针优先分支保留原 bool 返回，只有该分支可以使用旧 continuation。未识别或不一致的正式入口停机。保留现有内部 immediate 和 guard 退出。同步调度只把事件留在当前任务栈，不在全局保存事件地址。
- [ ] 更新普通产物正向审计和合成 fixture，将旧的直接 `aarch64_el0_sync_route → aarch64_user_task_sync` 边改为路由 → 公共分派 → 后端 → 正式处理，保留实际 VBAR 槽检查。运行两项宿主异常用例及 `python3 -m unittest qemutests.test_intr_dispatch_audit`。

## 任务 3　正式用户复制和 SVC #0 接线

文件为 `kernel/arch/aarch64/memory/{uaccess.c,user_copy.S}`、`kernel/include/arch/aarch64/uaccess.h`、`kernel/arch/aarch64/intr/{entry.S,trap.c,syscall.c}`、`kernel/include/arch/aarch64/syscall.h`、`kernel/arch/aarch64/cpu/user_task.c`、`kernel/Makefile` 和任务 2 的审计用例。

- [ ] 在既有 AArch64 宿主解码测试中覆盖共用的纯 abort 匹配。用实际的 bit 7、CM、FnV、错模式、错 EC/FSC、错 PC、WnR 与窗口方向不符、错操作数 FAR、越窗 FAR、无窗口和不同任务／mm／根作为拒绝条件。匹配器只比较入参，测试不得伪造硬件恢复证据。
- [ ] 在 `uaccess.c` 实现共享头已声明的 `syscall_check_user_range()` 和两个 `copy_*_ft_res()`。`n == 0` 直接成功。其余用 `arch_local_irq_save()` 获取正式 `mm`、从 `mm->pgdir` 取 `uint64_t root_pa = (uintptr_t)mm->pgdir`。用 `aarch64_pt_root_is_published(&root_pa)` 查登记，检查 `ssize_t` 和减法边界后，将 `(void *)Phy_To_Virt(root_pa)` 传给 `arch_user_range_accessible()` 校验全范围。只在全部查询锁已释放后打开 BSP 私有窗口，记录当前任务、`mm`、根 PA、方向和范围。复制返回后清除本次窗口并恢复 IRQ，再返回全长或 `-EFAULT`，失败回调至多一次。拒绝嵌套且保留外层窗口。
- [ ] 将 `user_copy.S` 的真实 LDTRB/STTRB、指令标签和故障跳板编入普通镜像。将 `aarch64_uaccess_fixup()` 改为先做纯 ESR/SPSR/PC/FAR 谓词。用 `SPSR_EL1 & 0x1f` 核对完整 M[4:0] 等于 EL1h，再匹配正式窗口与当前任务、mm、硬件根。正式修复不查发布表或页表。旧探针的复制和修复分支继续受测试宏及活动探针约束。内核缓冲区普通访存指令维持致命路径。
- [ ] 普通 `el1_sync_entry` 在 `aarch64_el1_sync()` 返回后恢复帧。该 C 入口只有命中修复点才返回，其他异常维持原有 fatal 或 bad-event。扩展早期链审计和合成 fixture，正向检查 fixup 与恢复边，将修复的早期可达 helper 纳入无锁、无栈 canary、无普通日志审计。正式用户运行期分支在验证来源后才接触运行期处理。
- [ ] 在 `intr/syscall.c` 实现 `void aarch64_user_syscall(pt_regs_t *regs)`。以完整 x8 做 `syscall_has_handler()` 检查，无处理函数则在入口写 x0 为 `-ENOSYS`。从 x0–x5 组装参数，设置 `ctx.arch_frame = regs` 和 `ctx.suppress_writeback = false`，调用共享分派后按 `suppress_writeback` 写 x0。由正式 `aarch64_user_task_sync()` 的 immediate 0 分支调用。AArch64 `arch_syscall_exit()` 在 `user_task.c` 转交既有 `aarch64_user_task_exit()`。将 `syscall/{dispatch,sys_native}.c` 加入普通 AArch64 白名单，构建普通与自测配置。
- [ ] 运行宿主匹配用例和普通构建静态审计。正向检查 `aarch64_user_task_sync → aarch64_user_syscall → syscall_dispatch`，接受编译器生成的已解析普通调用或尾调用边。审计普通产物中的复制符号与 EL1 修复／恢复边，并排除旧探针复制包装器 `aarch64_copy_from_user`、`aarch64_copy_to_user` 及新增 `aarch64_uaccess_test_copy`、`aarch64_user_svc_uaccess_test_hook` 和 fixture ELF 符号。

## 任务 4　一次单核 QEMU 验收

文件为 `kernel/selftest/test_aarch64_user_svc_uaccess.c`、`kernel/selftest/fixtures/aarch64_user_svc_uaccess.S`、`kernel/Makefile`、`kernel/selftest/{selftest,test_aarch64_scheduler}.c`、`kernel/include/arch/aarch64/boot.h`、`kernel/arch/aarch64/{intr/syscall.c,memory/uaccess.c}`、`kernel/include/selftest/aarch64_user_svc_uaccess.h` 及 `docs/build/build.md`。

- [ ] 复用第 2 块的 ELF 嵌入规则及链接布局。让 RW 段覆盖连续两页，紧邻第三页留空。正式任务用 `clock_gettime` 把 16 字节结构写到两页边界，检查结果及保护寄存器。向代码页写回和向 RW 段末尾跨越空页的写回均返回 `-EFAULT`，后者保持前缀哨兵。fixture 将 PID 和 PPID 返回值分别存入 x19、x20。测试观察点在该 PID 首次调用 `clock_gettime` 时，从保存帧读回两值，分别与创建返回的 PID 和常驻 BSP idle 的 PID 比较。验证未知 immediate、未开放编号及高位污染 x8 返回值。用原生 exit 退出。在 `test_aarch64_scheduler.c` 的 `aarch64_sched_observe_reap()` 入口增加测试专用 PID 过滤回调，放在既有 `reclaim_batch_active` 判断之前。该观察点由 `task_switch_complete()` 在 `task_finish_switch()` 和释放前调用，同时核对 `PF_SELF_REAP` 和编码后的 `exit_code`。复用第 2 块 guard fault 回归。
- [ ] 在正式 SVC 的 `aarch64_user_svc_uaccess_test_hook()` 测试分支只为已登记 fixture PID 执行一次内核复制自测。检查 copy-in 跨页的实际数据、零长度和溢出。对读未映射 guard 和写只读代码两次调用同一复制实现，`aarch64_uaccess_test_copy()` 仅跳过页权限预检。必须实际到达各自精确故障标签并进入修复，核对 FAR、方向、返回 `-EFAULT` 和各一次回调，再成功复制以确认清窗。测试旁路无生产入口。
- [ ] 以 `aarch64_user_svc_uaccess` 登记单个 late case，变体 late 期望从 14 改为 15。在调度器就绪后运行，用已有 PMM 4 KiB 基线和第 2 块的回收观察点确认退出清理。
- [ ] 预构建 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi`。执行 `timeout 20s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`。串口要求新增 PASS 及 `[TEST] END suite=kernel-selftest total=22 passed=22 failed=0 skipped=0`。普通镜像执行 `make PROFILE=aarch64-clang test-aarch64-audit`，运行 Python 合成审计和 x86 syscall 边界审计。更新构建文档，保留一条自测 PASS 及 suite END。

## 定向宿主命令

```sh
make -s -f Makefile -f - PROFILE=x86_64-clang m4-svc-host-check <<'MAKE'
.PHONY: m4-svc-host-check
m4-svc-host-check:
	$(call os01_submake,hosttests,test-syscall-dispatch test-compat-linux test-nofile-limits $(HOST_TEST_BUILD_DIR)/test_intr_decode_aarch64.elf $(HOST_TEST_BUILD_DIR)/test_intr_backend_aarch64.elf)
	$(HOST_TEST_BUILD_DIR)/test_intr_decode_aarch64.elf
	$(HOST_TEST_BUILD_DIR)/test_intr_backend_aarch64.elf
MAKE
```

任务 1 将子集二进制加入现有 `test-syscall-dispatch` 宿主入口。最终检查宿主命令退出码和 `git diff --check`。
