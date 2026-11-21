# 多架构异常诊断与内核栈回溯实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal** — 让 x86_64 和 AArch64 的致命内核异常通过 `kernel/core/trace.c` 打印从保存 PC 开始、最多 4 帧的符号化回溯。

**Architecture** — 沿用 `intr_event`、架构分派后端和原生 `pt_regs`。共用符号查询、帧链约束和紧急输出。架构代码验证栈所有权、解析寄存器并保护两次帧字读取。致命路径决定是否回溯和停机。

**Tech Stack** — freestanding C、x86_64/AArch64 汇编、两阶段 GNU Make 链接、LLVM 工具链、hosttests、Python 标准库和现有 QEMU harness。

**Spec** — [多架构异常诊断与内核栈回溯设计](../specs/2026-11-21-multiarch-exception-trace-design.md)。

## 全局约束

- 保持 `arch_intr_dispatch()` 无状态，维持 IDT/VBAR、`pt_regs`、信号帧及原有 IRQ/syscall/用户异常和非诊断 fixup 的分派、语义与返回策略。
- 首帧打印原始异常 PC，最多 4 帧，单段内核栈。用户或未知来源与 PC-only 模式不读帧内存。返回地址的符号查询回退 x86_64 1 字节、AArch64 4 字节，展示原始返回地址。
- 致命报告和早期读取恢复路径不依赖分配、锁、常规 logger、格式化库、早期 canary 或未经校验的当前任务指针。保留 x86_64 #BP/#NMI、AArch64 EL1h/SError 的现有 fatal 主记录。共享 IST 完整性以 spec 的范围为准。
- 内核保留帧指针。测试函数保留自己的调用帧。隔离镜像包含探针，普通 ELF 无探针符号。真实读故障在测试专用映射触发，执行栈保持完整。
- 新宿主文件以 `@os01-test` 注册，新 runner 单测带 `TEST_MANIFEST`，执行 `make regen-test-catalog` 并提交实际变化的生成物。每项先记录预期失败，再记录通过的命令与输出。syscall E2E 单独构建。

## 评审重点

1. 真正的第二字读取故障恢复后，两个输出哨兵都保留，x86_64 没有执行返回工作。任务 2、4。
2. AArch64 步长正确但属于邻核的高半区 boot SP，或者 AP 低身份映射 SP，均保持 PC-only。任务 4。
3. 坏 FP 恰在区间末尾、算术接近 `UINTPTR_MAX` 或与当前帧重叠时，共享层不读取该记录。任务 3。
4. 两段间的地址、同址别名和最后一个符号应有确定结果。最终 ELF 地址和嵌入表逐项一致。任务 1。
5. x86_64 终止报告由锁无关路径输出。普通 #PF 的返回路径不消费其他适配器的返回寄存器。任务 2、3。

## 文件与职责

| 任务 | 主要文件 | 职责 |
| --- | --- | --- |
| 1 | `kernel/core/kallsyms.c`、新增 `kernel/core/trace_symbols.c` 和 `kernel/include/core/trace_symbols.h`、两架构 `linker.ld`、`kernel/Makefile` | 可执行范围、生成器、纯符号查询、双架构链接 |
| 2 | 新增 `kernel/include/arch/unwind.h`、`kernel/include/arch/x86_64/unwind.h`、`kernel/arch/x86_64/intr/{unwind.c,unwind_read.S}`，修改 x86_64 `intr/{entry.S,trap.c,fault_probe.c}`、`kernel/core/main.c`、构建选择器和审计 | 公共帧读取接口、x86 精确读取恢复、真实读取失败用例 |
| 3 | `kernel/core/trace.c`、`kernel/include/core/trace.h`、x86_64 `intr/{unwind.c,trap.c,fault_probe.c}`、x86 runner | 共享遍历和早期输出、现场与栈归属、x86 fatal 迁移 |
| 4 | 新增 `kernel/include/arch/aarch64/unwind.h`、`kernel/arch/aarch64/intr/{unwind.c,unwind_read.S,trace_probe.c}`，修改 AArch64 `intr/trap.c`、`cpu/{task_arch.c,ap_sched.c}`、`boot/main.c`、`kernel/Makefile` 及 AArch64 runner | AArch64 读取恢复、启动和任务栈、原生 fatal 迁移 |
| 回归 | `hosttests/mk/intr.mk`、定向宿主用例、`qemutests/intr_dispatch_audit.py` 和其单测、`qemutests/exception_trace.py` 和其单测、`mk/project.mk`、两个 profile 和 `mk/components/run.mk` | 生产对象联测、恢复分流、隔离变体、首帧与实际链接证据 |

## 任务 1 — 统一可执行范围与符号表

**接口**

- `struct trace_text_range { uintptr_t start, end; };`。由 `__trace_text_start/end` 和 `__trace_ltext_start/end` 描述两个半开区间。AArch64 第二段以相同开始/结束值表示空段，既有链接符号保留。
- `bool trace_symbol_lookup(uintptr_t query_pc, const char **name, uint64_t *offset);`。成功提交输出，失败不改输出。另提供 `bool trace_kernel_text_contains(uintptr_t pc);`，只查段范围。两个入口都无 canary 依赖。

- [ ] 新增 `hosttests/cases/test_trace_symbols.c`，链接生产 `trace_symbols.c`。夹具以小型 ELF 汇编符号数据提供两个有间隙的范围及小型表。用例 `test_trace_symbol_boundaries` 断言精确首符号偏移 0、末符号能解析到段末前 1、段末和间隙查找失败、第二段的第一个符号之前不会借用第一段的尾符号。`test_trace_symbol_empty` 和 `test_trace_symbol_missing` 分别在同一联编规则的独立 fixture ELF 中检查计数为零和弱表缺失。测试目录只登记主用例，由其测试夹具运行两个附属 ELF，复用同份生产查询对象。新增带 `TEST_MANIFEST` 的 `qemutests/test_kallsyms_trace.py`，测试排序、空段、别名与无终端哨兵。预期旧生成器缺共用符号及新查询入口。
- [ ] 运行 `make regen-test-catalog`、`make test-host`、`python3 -m unittest qemutests.test_kallsyms_trace`，记录对应的 RED。
- [ ] 在两个链接脚本增加共用边界。在 `kernel/core/kallsyms.c` 沿用 `HOST_NM -n` 输入，接收区间内的文本符号，排除范围标记、Arm mapping symbols 和不适合作为源码名字的内部生成项。别名按外部文本符号、局部文本符号、弱符号的优先级选择，同级按名称确定。地址、名称偏移和计数使用固定位宽及现有 ELF 符号名。C 端将 `kallsyms_names` 声明为字符数组。查询通过下界和同段检查限制候选，计数为 0 或任一弱表未定义时直接回退。
- [ ] 在 `kernel/Makefile` 的 AArch64 清单加入 `core/trace_symbols.c`。x86_64 旧 `lookup_kallsyms()` 暂时用新查询输出旧格式，原调用点等任务 3 一起迁移。新增 `qemutests/kallsyms_trace_audit.py --elf <kernel.elf> --nm <nm> --generator <kallsyms>`，用 ELF64 LOAD 映射定位表字节，读取四个表符号及区间，把最终 ELF 的 nm 输出交给同一构建生成器，逐项比对地址和名称，并拒绝空主段或空表。
- [ ] 运行宿主和生成器用例。运行 `make PROFILE=x86_64-clang kernel.bin`、`make PROFILE=aarch64-clang aarch64-uefi-kernel`，用最终 ELF 路径运行新增审计，确认至少 `kernel_main` 和正式同步分派函数可查。分别审查两阶段链接产物，记录实测地址。提交本任务。

## 任务 2 — 实现 x86_64 两字读取与 #PF 原生恢复

**接口**

- `struct unwind_frame { uintptr_t next_fp, return_pc; };`。
- `bool arch_unwind_read_frame(uintptr_t fp, struct unwind_frame *out);`。输入地址至少字对齐，使用中断前的架构帧指针解码规则。只有两次读取成功才写 `out`。协议在公共 `arch/unwind.h` 中定义，包装器和读桩由架构实现。
- `bool x86_unwind_fixup(struct pt_regs *regs, uint64_t error, uintptr_t cr2);`。命中改 RIP。
- x86_64 `int do_page_fault(struct pt_regs *regs, uint64_t error_code)`。返回 1 表示专用恢复，0 表示执行现有 #PF 后续返回工作。仅 `FUNC == do_page_fault` 的汇编路径读取 EAX。

- [ ] 在 `hosttests/cases/test_unwind_fixup_x86.c` 联编生产匹配器，提供两个载入标签及失败续点的符号替身。对 CPL0、错误码 0/P、正确 RIP 和保存 RDI 加相应偏移验证成功，且只有 RIP 改变。分别翻转 CPL、W/R、U/S、RSVD、I/D、PK、地址、RIP 或制造加 8 溢出，断言不修改帧。扩展 `qemutests/test_intr_dispatch_audit.py`，覆盖错误 FUNC 消费 EAX、命中恢复误入返回工作、普通 #PF 绕过返回工作三个反例。
- [ ] 运行 `make regen-test-catalog`、`make test-host`、`python3 -m unittest qemutests.test_intr_dispatch_audit`。先见新增恢复入口未实现或不满足预期分流。
- [ ] 实现 `x86_unwind_load_pair` 及两个标记的 qword 载入，保留 RDI 为待查地址。失败续点返回 false，成功时才把组合结果提交到输出。匹配器只接受错误码 `(error & ~UINT64_C(1)) == 0` 的读 #PF。#PF C 落点继续只在起始读取一次 CR2，先走精确修复，再进入原有事件路径。修改共享汇编 post-call 选择器，用独立 scratch 比较 FUNC，确认 #PF 后才比较完整 EAX。命中直接进 native restore，对其他入口保存原路由。
- [ ] 更新 `qemutests/intr_dispatch_audit.py` 的准确后缀及源级断言，保留其他 vector、syscall、frame 参数和 CR2 的现有校验。运行宿主测试、`make PROFILE=x86_64-clang kernel.bin`，再运行 `make PROFILE=x86_64-clang test-intr-dispatch-audit`。审计目标本身不构建 ELF。
- [ ] 在 `mk/project.mk`、两架构 profile、`kernel/Makefile` 及 `mk/components/run.mk` 延伸现有 `X86_INTR_FAULT_TEST`，增加 `trace-read`，路径 `kernel/intr-x86-fault-trace-read` 和 `image/intr-x86-fault-trace-read`。保持该选择器仅 x86、无 sys/selftest 混用。在 `fault_probe.c` 延伸现有 BSP 探针入口，在 BSP 探针的单核关 IRQ 区段使用测试专用低半区地址 `1ULL << PAGE_PGD_SHIFT`。先确认活动页表的该顶层项不存在，排除上级巨页和存量映射，再用 VMM/PMM 以 supervisor-only 方式映射第一页，确认第二页未映射。低层读取从第一页末尾 8 字节开始，绕过 walker 的帧对齐要求。先检查同页两个已知字成功，再检查跨页返回 false、两个输出仍是哨兵及返回工作计数未增长。使用测试变体限定的返回工作计数观察点。采样到读返回期间保持本地 IRQ 屏蔽。验证该变体的 armed 行记录真实 case 和构建 variant，只在全部通过后输出 `[exception-trace] PASS arch=x86_64 case=read` 并停机。
- [ ] 新增 `qemutests/exception_trace.py` 和带 manifest 的 `qemutests/test_exception_trace.py`。runner 只消费 make 明示的 profile、case、ELF、固件、磁盘及 QEMU 路径。沿用 `ProcessSession/RunArchive`、同类 BSP 启动配置和有界串口排空。QEMU 自行退出、到绝对时限仍无完整证据、缺失/重复记录、构建变体不符和意外 fatal 均失败。QEMU 停机由 runner 在完整证据及短排空后执行。接入 `make PROFILE=x86_64-clang test-qemu SUITE=intr-x86`，先查旧 #BP/#NMI 门，再执行 trace-read。验证 QEMU 真实跨页失败和通过证据，提交本任务。

## 任务 3 — 共享回溯与 x86_64 致命报告

**接口**

- `enum trace_mode { TRACE_FULL, TRACE_PC_ONLY };`。`void trace_exception(const struct pt_regs *regs, enum trace_mode mode);`。
- `struct unwind_cursor { uintptr_t pc, sp, fp, stack_low, stack_high; size_t frame_align; bool kernel_origin; };`。`void arch_unwind_init(struct unwind_cursor *cursor, const struct pt_regs *regs, bool allow_walk);`。始终初始化来源和 PC，无可信范围时保持 `stack_low == stack_high`。
- `bool arch_unwind_return_query_pc(uintptr_t ret, uintptr_t *query_pc);`。返回 false 时既不提交结果也不打印该后续帧。
- x86_64 架构私有 `_Noreturn void x86_fatal_exception(const char *name, unsigned vector, const struct pt_regs *regs, uint64_t error, enum trace_mode mode);`。#PF 可通过私有包装附加 CR2。对外没有新增通用异常策略接口。

- [ ] 新增 `hosttests/cases/test_exception_trace.c`，链接生产 `core/trace.c` 与替身钩子，用带计数的假读取器和捕获的 UART 校验第一帧、4 帧上限、用户/未知来源和 PC-only 的零次读取。有返回 PC 且 `next_fp=0` 时仍输出该返回帧。另断言错位、`fp < sp`、跨上界、接近 `UINTPTR_MAX`、循环和重叠只截断，后续返回地址落在段外不输出伪帧。运行 `make regen-test-catalog`、`make test-host` 记录 RED。
- [ ] 在 `core/trace.c` 实现公共遍历和原始 PC 输出。切换到 `arch_early_putc/puts`，保留标题和字段，结束统一输出 `====================== End Kernel Stack Backtrace ======================` 独立换行，使 runner 区分已完成回溯和首帧后的阻塞。首帧只在可信内核来源时查符号。两字记录需完整在同一范围内。非零下一 FP 至少越过当前两字记录，所有后续访问仍检查同一栈。调用任务 1 的纯符号接口。移除 `backtrace()` 及旧打印式 `lookup_kallsyms()` 时一次迁移所有现有调用点。
- [ ] 在 x86_64 `unwind.c` 使用保存的 `CS/RIP/RSP/RBP`。先按 `IA32_GS_BASE` 确认 `percpu_data[]` 的槽范围/步长，再 acquire 读取 `online`。只由可证明是 BSP 的本核槽接受静态 `init_task_union`。其余已发布的 BSP/AP 槽分别确认 `TSS64_Table`、`init_tss[cpu]`，只采用符合任务栈几何的 `rsp0`。未知 CPU 或未发布阶段在此实现中只打印 PC。x86_64 按机器字对齐，按已启用的 canonical VA 规则拒绝危险地址，保存的 RSP 和候选范围不匹配时保持空范围。由同版本可信任务栈判断保护内核 #PF 的普通 uaccess 恢复检查，保持其已有命中语义。给 x86_64 宿主栈夹具覆盖未发布槽、AP 的保存 SP 落在 BSP union、IST 当前 RSP 与保存 RSP 不同及无效 TSS。其中 AP 错位、未发布槽和无效 TSS 只退化到 PC。
- [ ] 在 x86_64 `trap.c` 把终止分支汇入私有报告入口。先屏蔽 IRQ，做一次性先发仲裁，经早期串口打印完整原格式主行和共享回溯。对 #PF 保留 `PF-KRN:` 和 CR2，移除不受保护的任务清单和栈转储。对 #DF、NMI、#MC 只请求 PC。原向量处理及用户路径上的普通日志继续使用 `log_*`。在 `kernel/Makefile` 给内核 C 保留帧指针，Release 反汇编核对叶函数要求和 512 字节栈预算。
- [ ] 让现有 x86_64 #BP 探针包含三个分立的有帧函数。保留原 resume 标记，使 runner 以配对变体 ELF 的 resume 地址同时核对主记录 RIP 和首帧地址，下一两层符号按内到外固定顺序，最后等到回溯结束行。NMI runner 再校验一帧、无恢复标记，沿用当前 fatal 行协议。按任务 2 的同一路线增加 `X86_INTR_FAULT_TEST=trace-bad-fp`。更新 `mk/project.mk`、两 profile、`kernel/Makefile` 的选择与源过滤、`kernel/core/main.c` 的探针入口门、`mk/components/run.mk` 的构建和 runner 调用。该变体由独立汇编探针设置出界 RBP 后触发既有 #BP，配对 ELF 的 resume 标签用于验证原 PC。runner 要求唯一 armed 行、预期变体、恰好一帧和结束行，重复 fatal 或达到绝对时限为失败。
- [ ] 运行 `make test-host`、`make test-harness`、`make PROFILE=x86_64-clang test-static`、`make PROFILE=x86_64-clang test-qemu SUITE=intr-x86`。检查实际 #BP 多层、NMI 单帧及坏 FP 单帧。另运行 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`，从串口归档核对唯一 `[selftest] uaccess: dispatch longjmp restore OK`。该标记证明生产读取触发真实 #PF、返回 -EFAULT、cleanup 恰好一次且随后可成功读取。通过后提交本任务。

## 任务 4 — AArch64 的早期、任务栈和原生致命异常

**接口**

- 实现任务 2、3 的公共架构接口和私有 `bool aarch64_unwind_fixup(struct pt_regs *regs, uint64_t esr, uintptr_t far);`。
- `void aarch64_unwind_publish_task_stack(unsigned cpu, uintptr_t base);`。只由验证过栈几何的架构上下文边界调用，使用独立 `NR_CPUS` 原子槽，不修改 `percpu_t` 和任何原生帧。
- 新增 `make PROFILE=aarch64-clang test-aarch64 MODE=exception-trace [EXCEPTION_TRACE_CASE=all|read|bad-fp|task]`，默认 all。内部只向受控子构建传 `AARCH64_EXCEPTION_TRACE_TEST=read|bad-fp|task`，使用独立 `kernel/exception-trace-<case>/kernel.elf` 和 `image/exception-trace-<case>/aarch64-uefi.img`，由 `mk/targets/aarch64.mk` 现有变体函数推导。

- [ ] 在 `hosttests/cases/test_unwind_aarch64.c` 联编生产匹配器和栈范围判定。使用 `hosttests/mock/intr_dispatch/aarch64` 原生寄存器帧替身和输入已读取 TPIDR 值的范围 helper，将 AArch64 MRS 与读桩包装隔离在宿主不执行的适配部分。用专用寄存器替身断言同级 EL1h 的两个精确载入位置匹配，且只改 ELR。逐项否定 EL0、FnV、WnR、CM、S1PTW、外部中止、FAR 不符、未知 PC。栈范围另验证 TPIDR 无效、相邻槽、AP 低地址、过期任务基址以及栈几何的下边界。登记后运行 `make regen-test-catalog`、`make test-host`，记录 RED。
- [ ] 实现 `aarch64_unwind_load_pair`，保留 x0 为原读址，两个 LDR 成功后才写结果。EL1 同步入口先尝试精确恢复，命中用既有 `restore_pt_regs`，未命中按原顺序 uaccess、解码、分派。复用 `sync_fault.h` 的位定义，只接受 FSC 翻译 0x04–0x07、访问标志 0x09–0x0b 和权限 0x0d–0x0f。给早期可达链加明确的 canary 审计，新增 audit 断言保留原边，专用修复可正常返回向量恢复路径。
- [ ] 用 `SPSR_EL1`、`ELR_EL1`、`X29`、帧地址加 `PT_REGS_SIZE` 构造 EL1h 游标。以 TPIDR_EL1 验证本核槽址，只匹配本核 4 KiB 高半区启动栈范围。发布运行期栈基址到静态槽，调用处为 `arch_aarch64_bsp_handoff()`、`aarch64_ap_idle_handoff()` 及 `arch_task_switch()` 在验证后、实际换栈前。发布者核对 `align_up(base + sizeof(task_t) + 256, 16)` 和固定栈顶。切换窗口的 SP 与快照不符就结束于首帧。AArch64 按帧记录所需的机器字对齐读，返回地址查询要求 4 字节指令对齐。
- [ ] 在 AArch64 的 `kernel/Makefile` 清单加入 `core/trace.c`，核实两个架构的叶函数帧编译设置实际生效。在 `aarch64_el1_sync_fatal()` 原 fatal 行后加 FULL，在 `aarch64_serror_fatal()` 原 fatal 行后加 PC_ONLY。保留原子胜者、原始 ESR/FAR 和 park。
- [ ] 扩展现有 `MODE=sync-fault` 的启动探针，使现有已知 EL1h 缺页发生在三个保留帧的函数中，runner 先验原协议和配对变体 ELF 的故障指令标签，再验首帧地址、两个调用者和结束行。新增 `trace_probe.c`，从架构源码通配排除，仅在合法测试 selector 下显式加入。read 与 bad-fp 从 `arch_pre_subsys_selftest()` 进入，read 利用 `AARCH64_PT_SELFTEST_VA` 附近已确认空闲的双页，按现有页表自测的时机建立临时映射，复用任务 2 的两字与跨页哨兵协议。bad-fp 由独立汇编探针写入不能归属的 x29，再在唯一汇编标记处产生同级同步异常，验配对 ELF 的故障 PC、恰好一帧和结束行。task 在 `arch_aarch64_bsp_handoff()` 的 scheduler ready 与 AP release 后、通用 `OS01_SELFTEST` 默认分支之前，用 `kernel_thread_on_cpu(..., 0)` 创建独立任务并检查返回成功。实际调度后的线程先验证当前任务和 SP，再输出 armed，随后在三层可识别函数链的标记处触发同级同步异常。该例验证换栈发布及两个调用者。所有测试使用实际入口，探针挂钩及 ELF 符号限制在各自变体。
- [ ] 在 `mk/project.mk` 的 `OS01_SUBMAKE_ALLOWED` 透传 AArch64 selector，两个 profile 分别校验取值与架构，AArch64 mode 自行设置 `KERNEL_SELFTEST=1` 并拒绝并用其他会影响产物的 selector。由 AArch64 profile 设定 `KERNEL_VARIANT`，由 `kernel/Makefile` 加精确的编译宏和 fixture。沿用自测 manifest，把新变体的 marker 文件也放入对应镜像目录，避免重写普通自测目录。扩展 `qemutests/build_contract.sh` 检查 dry-run 产物路径和错误组合，并用正常 ELF 审计确认无新 fixture。扩展 make mode 包装与共用 runner、测试报告，校验 `[exception-trace] PASS arch=aarch64 case=read` 或对应 fatal 证据。运行 `make test-host`、`make test-harness`、`make PROFILE=aarch64-clang test-aarch64-audit`，在 `qemutests/aarch64_serror.py` 保留已有 fatal 校验，并扩展带 manifest 的既有 `test_aarch64_serror.py`，要求 EL1t、EL1h、EL0t 各自唯一一帧，其原始地址等于本次 fatal 的 ELR，随后出现唯一回溯结束行。EL0t 校验只报原址。另分别实跑 `MODE=sync-fault`、`MODE=serror`、`MODE=exception-trace`。提交本任务。

## 合入前验收

- [ ] 对两个 profile 的 Release 普通 ELF 重跑符号、帧指针、栈预算和异常分派审计。清点 probe/test 符号都只出现在匹配的隔离镜像。用 `make PROFILE=x86_64-clang test-contract` 和 `make PROFILE=aarch64-clang test-contract` 执行新增变体路径及互斥检查。运行 `make test-host`、`make test-harness`、`make PROFILE=x86_64-clang test-static`、`make PROFILE=aarch64-clang test-aarch64-audit`。
- [ ] x86_64 运行 `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`、`make PROFILE=x86_64-clang test-qemu SUITE=intr-x86`、`make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall`，再独立运行 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`。
- [ ] AArch64 分别运行 `make PROFILE=aarch64-clang test-aarch64 MODE=<mode>`，覆盖 `sync-fault`、`serror`、`exception-trace`、`sched`、`sched-ap`、`user-runtime`。检查原生 fatal 主记录、双架构首帧地址、调用者顺序和恢复分流，在各自归档保存 ELF 身份、原始串口和实际命令。
- [ ] 验收通过后更新 `docs/arch.md` 和 `docs/build/build.md` 中的异常诊断与测试入口，注明单段回溯范围、早期串口输出与限制。提交文档及对应的测试目录生成物。每个实现任务独立提交，未通过的门附具体命令和日志。
