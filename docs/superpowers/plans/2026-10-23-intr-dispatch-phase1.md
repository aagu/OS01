# Interrupt Dispatch Phase One Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 aarch64 EL1h sync/IRQ 和 x86 #DE/#UD/#GP/#PF 经过共同事件分派入口，保持原有恢复、终止和 IRQ 协议。

**Architecture:** 栈上事件携带规范化分类与原始寄存器快照；共同 dispatcher 只检查参数并选择 sync/IRQ 后端。架构后端拥有现有 handler、现场修改、longjmp、任务退出和 GIC EOI。纯解码与硬件操作分文件，使 host 测试能够链接生产代码。

**Tech Stack:** freestanding C、x86_64/AArch64 汇编、Clang/LLD、Make profile、现有 hosttests、自测协议 v1、Python unittest 与 QEMU。

**Spec:** [2026-10-23-intr-dispatch-phase1-design.md](../specs/2026-10-23-intr-dispatch-phase1-design.md)，已提交于 `7471981`；子代理第二轮评审 approve，用户随后要求提交并写 plan。

## Global Constraints

- “本期不改汇编 frame 布局、IDT gate/IST 设置或返回标签。”
- “共同实现不得读取其成员。”此处指 `pt_regs_t`；公共头只通过 `arch/regs.h` 获得类型。
- “每架构必须提供强定义；缺失后端应链接失败，不提供 weak no-op。”
- “无效地址规范为 `fault_addr_valid=false, fault_addr=0`”；`native_address` 始终保留原始 FAR。
- “不能在调用后补必需清理、IRQ ACK 或恢复 IRQ 状态。”共同调用链不拥有需正常返回才能释放的资源。
- “共同入口、解码、aarch64 sync 适配与 bad-event 整条早期可达链禁止分配、锁、普通日志、current/per-CPU 访问、栈 canary 依赖及启动状态 gate。”
- aarch64 向量表为 16 槽、128 字节步长、2 KiB 对齐；sync `0x200`，IRQ `0x280`，现场 272 字节。
- 不新增 EL0/SVC、aarch64 可恢复缺页、用户页表、ASID/COW、上下文切换、统一 SMP/kernel_main 或 x86 IRQ 迁移。
- 新源/头按仓库目录对称规范配对；所有测试入口从仓库根调用，产物留在 profile build 目录。不要直接运行 `make -C hosttests` 绕过 profile。
- 不更改共享结构 ABI；若意外需要改动 task/percpu/pt_regs 等结构，先重新审设计，并遵守 `make clean` 要求。普通新增日志用 `log_*`，自测标记仅在自测构建中发布。
- `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` 不可同时设置 `KERNEL_SELFTEST=1`；内核自测单独运行。

## Review Focus

1. 无效 FAR 非零、有效故障地址为 0：规范化有效性与原始快照不能互相覆盖；Task 1 钉死。
2. 坏 kind、NULL、载荷来源矛盾和未迁移向量：必须进入无依赖 nonreturn bad-event；Tasks 2–4 钉死。
3. 实际 #PF 与 prepare 拒绝都可能返回 `-EFAULT`：只有硬件 fault→dispatcher→longjmp 和现场恢复可算恢复通过；Task 5 钉死。
4. GIC spurious 与普通中断需要不同 EOI 行为：不能预读 IAR、二次 ACK 或吞掉 SGI shootdown；Tasks 3、7 钉死。
5. #GP fixup 返回、任务栈切换退出和异常栈返回路径不能混用：保持旧 RIP/信号/跳转语义，且早期调用链无 canary；Tasks 4、6、7 钉死。

## 文件分解与执行约定

| 创建/修改 | 职责 |
|---|---|
| 新建 `kernel/include/intr/dispatch.h`、`kernel/intr/dispatch.c` | spec 完整事件结构、公共 `void arch_intr_dispatch(...)` |
| 新建 `kernel/include/arch/intr.h` | 自包含公共后端声明，包含 `intr/dispatch.h`，不形成反向包含循环 |
| 每架构新建 `kernel/include/arch/<arch>/intr_decode.h`、`kernel/arch/<arch>/intr/intr_decode.c` | 生产纯解码与返回 origin 的校验辅助 |
| 每架构新建 `kernel/include/arch/<arch>/intr_dispatch.h`、`kernel/arch/<arch>/intr/dispatch.c` | 私有 handler 声明、载荷校验、后端路由 |
| 每架构新建 `kernel/arch/<arch>/intr/bad_event.c` | 真实屏蔽中断、early-print、halt；与可 host 编译的后端分离 |
| 修改两架构 `intr/trap.c`、aarch64 `intr/entry.S` | 真实入口适配、原处理路径交接；x86 汇编只审计不修改 |
| 新建 `hosttests/cases/test_intr_decode_{x86,aarch64}.c`、`test_intr_dispatch.c`、`test_intr_backend_{x86,aarch64}.c` | 链接生产纯解码、共同入口与架构路由对象 |
| 新建 `hosttests/mock/intr_dispatch/aarch64/arch/regs.h` | 仅转包含生产 `arch/aarch64/regs.h`，让 host 编译 ARM 现场类型；禁止复制结构或覆盖宿主 libc 的 ISA 宏 |
| 修改 `hosttests/Makefile`、`kernel/Makefile` | 注册测试二进制、必要时添加 ARM host include 优先级、aarch64 共同源白名单 |
| 修改 `kernel/selftest/test_uaccess.c`、必要时 `test_msr_safe.c` | 真实 guest 恢复断言、原 safe-MSR 路径证据；不改生产 uaccess 实现 |
| 必要时修改 `user/systest.c` | 为用户 #GP 增加独立 child 的信号终止回归，若现有 suite 已有等价用例则复用 |
| 修改 `qemutests/check_kernel_selftest.py`、`run_kernel_selftest.py`、`test_kernel_selftest_result.py` | marker 与协议 PASS 联合 gate、严格正反例 |
| 新建 `qemutests/intr_dispatch_audit.py`、`test_intr_dispatch_audit.py`；修改 `mk/components/run.mk` | 可复跑对象/源码接线审计与静态/框架 bucket 注册 |
| 修改 `docs/arch.md`、`docs/roadmap.md`、`docs/changelog.md` | 最终覆盖范围和证据，保留 Spec A/M4 剩余状态 |

依赖顺序：Task 1 → Task 2 → Task 3 → Task 4 → Tasks 5–6 → Task 7。顺序交付，不需要并行修改共享头。执行前按 using-git-worktrees 技能选择隔离工作区；不要在写 plan 阶段创建实现 worktree 或开始代码。

任务内先加测试并运行，确认失败来自缺失的行为或链接符号，再实现、复跑并提交。新 host binary 全部加入 `TEST_BINS` 并使用已有 test framework 输出可辨认结果，通过根 `make PROFILE=x86_64-clang test-host` 调用；不新增裸运行旁路。本文给出的后续提交命令仅供实施者执行，本次只写 plan。

### Task 1: 定义事件契约并实现两个纯解码器

**Files:** 新建公共事件头、两组 `intr_decode.h/.c`、两份 decode host case 与 ARM regs 转包含头；修改 `hosttests/Makefile`。

**Interfaces:**
- Produces: spec §5.1 完整 `struct intr_event` 与 enums；声明 dispatcher 但本任务不接入任何产品入口。
- Produces: `void x86_intr_decode(struct intr_event *out, const pt_regs_t *regs, uint64_t vector, uint64_t error_code, uint64_t cr2)`。
- Produces: `void aarch64_intr_decode_sync(struct intr_event *out, const pt_regs_t *regs, uint64_t esr, uint64_t far)` 与 `void aarch64_intr_decode_irq(struct intr_event *out, const pt_regs_t *regs)`。
- 两解码器由非 NULL 适配器调用；不读取系统寄存器、不执行 handler。全量初始化成员，不要求初始化 padding。

- [ ] **Step 1:** 为两个 host binary 编写命名用例并注册：`x86_vectors_and_cpl` 断言 0/6/13/14 对应 reason，CPL 0/3/1/2 → KERNEL/USER/UNKNOWN/UNKNOWN；`x86_pf_access_and_zero` 断言 I/D 优先于 W/R、CR2=0 仍 valid；非 PF `native_address=0`。
- [ ] **Step 2:** 添加 `a64_modes_and_classes`、`a64_far_validity`、`a64_abort_access`、`a64_irq_fields`。EC `0x20/0x21/0x24/0x25` 为 MEMORY；`0x22` 为 PROTECTION；其他 OTHER。FnV=1、FAR=`0x12345678` 时 normalized=false/0、raw 不变；PC alignment 不按 FnV。DATA FSC `0x04..0x0f` 且 CM/S1PTW=0 按 WnR，其余 UNKNOWN；IRQ raw=`0x280/0/0`。EL1h=`0x5`、EL0t=`0`；其他模式 UNKNOWN。用独立常量表生成期望值。
- [ ] **Step 3:** 运行 `make PROFILE=x86_64-clang test-host`，确认新测试因缺头/缺函数失败；记录首个预期错误。既有失败单列，不能误当 RED 证据。
- [ ] **Step 4:** 按 spec §6 实现纯函数。aarch64 调用现有 `aarch64_sync_far_valid()`；host ARM include shim 只转包含生产结构。x86 只解码收到的 CR2，不读取 CR2。标记纯函数 `no_stack_protector`。
- [ ] **Step 5:** 同命令复跑，要求两个新 binary 全部断言 PASS；检查 header 自包含、ARM/X86 测试使用正确生产现场类型。
- [ ] **Step 6:** 只暂存上述文件并提交 `feat(intr): define normalized events and native decoders`。

### Task 2: 共同入口及真实 bad-event 后端

**Files:** 新建 `kernel/include/arch/intr.h`、`kernel/intr/dispatch.c`、两架构 `intr/bad_event.c`、`hosttests/cases/test_intr_dispatch.c`；修改两 Makefile。

**Interfaces:**
- Consumes: Task 1 的 `struct intr_event` 与 `pt_regs_t`。
- Produces: `void arch_intr_dispatch(const struct intr_event *event, pt_regs_t *regs)`；`void arch_intr_handle_sync(const struct intr_event *, pt_regs_t *)`、`void arch_intr_handle_irq(...)`、`noreturn void arch_intr_bad_event(...)` 的强后端声明。
- 产品 backends 分别在 Tasks 3/4 接入；本任务不将共同 dispatcher 加入产品源清单，避免中间提交留下未定义后端。host 测试提供强 spy 定义。

- [ ] **Step 1:** 新增 `sync_routes_once`、`irq_routes_once`：spy 接收原 event/regs 指针，计数为 1，现场字节不变，无 ACK、调度或尾部副作用。新增 `null_and_invalid_kind_die`：NULL event、NULL regs、kind 超界进入 nonreturn spy，用 setjmp/longjmp 测试逃逸；不得令 bad-event 正常返回。
- [ ] **Step 2:** 增加 `nonlocal_escape_skips_tail`：sync spy 直接跳回测试恢复点，证明 dispatcher 无必需清理且未调用另一通道。运行根 `test-host` 确认测试 RED。
- [ ] **Step 3:** 实现仅检查参数/kind 的共同函数，附 `no_stack_protector`；公共代码不访问 regs 成员/原生载荷。bad_event.c 每架构各提供强 `noreturn,no_stack_protector`：先屏蔽中断（ARM 完整 DAIF），固定 early-print 前缀 `[intr-dispatch] FATAL bad-event`，永久 halt，不解引用参数。在同一任务的 Makefile 改动中，从 x86 `intr/*.c` 产品通配清单临时排除新增 `intr/dispatch.c`；ARM 也暂不加入该共同源。两架构 backends 完整后分别在 Tasks 3/4 启用，host 测试始终直接链接该生产源。
- [ ] **Step 4:** 根 `test-host` GREEN；对 dispatcher 对象确认未引入 current/日志/锁/分配依赖。真实停机不在宿主运行，后续 Task 6 对象审计覆盖。
- [ ] **Step 5:** 提交 `feat(intr): add stateless dispatcher and fatal contract guards`。

### Task 3: 接通 aarch64 EL1h sync/IRQ

**Files:** 新建 aarch64 私有 `intr_dispatch.h`、`intr/dispatch.c`、`test_intr_backend_aarch64.c`；修改 aarch64 `trap.c`、`entry.S` 与 `kernel/Makefile`、host Makefile。

**Interfaces:**
- Consumes: Task 1 ARM 解码器；Task 2 共同函数和 bad-event。
- Produces: ARM 强 sync/IRQ 后端；`noreturn,no_stack_protector void aarch64_el1_sync(pt_regs_t *regs, uint64_t esr, uint64_t far)`。
- 保留 `void el1_irq(struct pt_regs *regs)` 和旧 fatal 签名，私有头声明所需跨 TU 调用。

- [ ] **Step 1:** host 链接生产 ARM 解码、后端与共同 dispatcher，替换 fatal/GIC/bad-event 末端为 spy。`sync_fatal_preserves_raw` 断言 slot=0x200、原 ESR/FAR 原样送 fatal、无 GIC；`irq_gic_once` 断言 slot=0x280、GIC 只调用一次；`bad_mode_or_payload` 覆盖非 EL1h、UNKNOWN origin、错误 slot、kind/来源/解码字段不一致。expected-event 校验逐成员比较，不能用含 padding 的 memcmp。
- [ ] **Step 2:** `make PROFILE=x86_64-clang test-host` RED；实现后端的确定性校验：重解码纯载荷并逐成员比较，再调用既有末端。aarch64 sync 若 ESR.EC 声称 lower-EL instruction/data abort (`0x20/0x24`)，与已保存 EL1h 来源矛盾，进入 bad-event；其他未知 EC 走原 fatal，不假装恢复。
- [ ] **Step 3:** 在 trap.c 构造局部事件并调用共同入口；sync 适配若意外返回立即 bad-event。汇编仅改 `bl` 目标为 `aarch64_el1_sync`，保留保存、ESR/FAR 参数及 DAIF/WFI fallback；IRQ 原恢复/eret 不变。sync 后端及早期可达函数禁用 canary。
- [ ] **Step 4:** 把 `intr/dispatch.c` 加入 aarch64 共同源白名单；架构目录的 intr/*.c 由既有 wildcard 收集。保留 Task 2 的 x86 共同 dispatcher 临时排除，至 Task 4 同提交取消排除，避免缺 x86 后端破坏中间提交。
- [ ] **Step 5:** 根 `test-host` GREEN；运行 `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault`、`MODE=gic-spi` 和 `MODE=smp`。要求原 sync 证据完整无 returned，IRQ 既有断言不降级。反汇编确认 sync C 新目标以及 16 槽/步长/272 字节现场不变。
- [ ] **Step 6:** 提交 `feat(aarch64): route EL1h events through common dispatch`。

### Task 4: 迁移 x86 四个 C 入口，保留原业务与返回路径

**Files:** 新建 x86 私有 `intr_dispatch.h`、`intr/dispatch.c`、`test_intr_backend_x86.c`；修改 x86 `intr/trap.c`、kernel/host Makefile，必要时 `user/systest.c`；只读取 x86 `entry.S`、IDT/gate 安装代码。

**Interfaces:**
- Produces: x86 强 sync/IRQ 后端。公开 `do_divide_error/do_undefined_opcode/do_general_protection/do_page_fault(pt_regs_t *, uint64_t)` 保留。
- 私有业务交接：`void x86_handle_divide_error(pt_regs_t *, uint64_t)`、`x86_handle_undefined_opcode(...)`、`x86_handle_general_protection(...)`；`void x86_handle_page_fault(pt_regs_t *, uint64_t error_code, uint64_t cr2)`。声明只进 x86 私有头，因跨 TU 不用 static，但不成为公共 ABI。

- [ ] **Step 1:** 生产 x86 后端+共同 dispatcher 用 spy 私有业务测试：四个 vector 恰一次调用对应 handler、error 原样传递、#PF CR2 原样传递；错 origin、UNKNOWN、错误分类/载荷、未迁移 vector 和任何 IRQ 事件都 bad-event。真实非迁移入口仍不改。
- [ ] **Step 2:** 根 `test-host` RED；后端按四项显式 switch，检查由解码器重建的各成员；不把 CS origin 与 #PF U/S 强行判为相等，因为内核可访问用户地址。
- [ ] **Step 3:** 原业务体按上述签名移动到同一 trap.c 的私有 handler；四个公开函数改薄入口。#PF 在最前端读取一次 CR2，后续统一用事件快照；CR2 读取在所有可 fault helper 前，必要的编译器序约束在对象审计确认。除 #PF 外的薄入口和解码器不读取 CR2，#PF 私有业务体不重读。保留现有 #GP 内核 fatal 业务中的 CR2 诊断读取，但不得将它填入 #GP 事件的 `fault_addr` 或 `native_address`；非 #PF 事件仍为无效地址、raw address=0。
- [ ] **Step 4:** 取消 Task 2 设置、Task 3 保留的 x86 共同源临时排除。审计 `arch_msr_gp_fixup()` 仍先于原日志/用户分派，#PF fault_jmp 检查和全部 VMA/COW/引用计数/TLB/失败释放顺序保持。`kill_current_user_task` 的任务栈切换汇编保持，公共层不调用 current 或退出函数。
- [ ] **Step 5:** 根 `test-host` GREEN；运行 `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 与 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall`。按名字确认 #DE→SIGFPE、#UD→SIGILL、非法访存→SIGSEGV 和 COW/mmap 用例通过。用户 #GP 的真实路径若现有 suite 无用例，补一个独立 child 的受控用户特权指令（如 `cli`）触发 #GP，断言 waitpid 信号为 SIGSEGV、parent 继续执行，并注册 systest case；保持新用例的函数和表项在同一提交。
- [ ] **Step 6:** 对四 stub 做前后反汇编/源码记录：均应保持原 `error_code`→`ret_from_intr` 跳转，不按过时注释改成 `ret_from_exception`。提交 `refactor(x86): adapt representative faults to event dispatch`。

### Task 5: 增加真实 guest longjmp 与恢复状态硬断言

**Files:** 修改 `kernel/selftest/test_uaccess.c`；需要时修改 `kernel/selftest/test_msr_safe.c`，不修改生产 `memory/uaccess.c`。

**Interfaces:**
- Consumes: 已接通四个 x86 入口与生产 `copy_from_user_ft_res(void *, const void *, size_t, void (*)(void *), void *)`。
- Produces: 在已有 `uaccess` case 内的真实探针，成功且所有资源清理完成后精确输出 `[selftest] uaccess: dispatch longjmp restore OK`。

- [ ] **Step 1:** 添加 `test_uaccess_dispatch_longjmp_restore` 私有子用例（返回 int，纳入 `selftest_uaccess()` 返回值）。复用 ensure_pt/restore_pt 思路管理测试独占低地址页，保存父层/leaf 状态，移除 leaf、TLB 失效后确认 live walk 无有效映射。保存 addr_limit 和原三个恢复字段；测试不能持锁执行 fault。
- [ ] **Step 2:** 非零长度、真实 absent 用户源、有效内核 dst 调生产 `copy_from_user_ft_res`；要求 `-EFAULT`、计数 callback=1、old fault_jmp/cleanup/arg 全还原。所有观测值先记录，再走唯一清理出口，不能用早退 assert 遗留页表/addr_limit。反例禁止改用 output prepare 拒绝。
- [ ] **Step 3:** 在映射有效的测试源上再读同样非零长度，要求成功、字节正确、callback 不增加，原恢复字段仍还原；恢复映射/TLB/页表资源及 addr_limit 后才发精确 marker。测试 read 源和 cleanup 计数具有编译器可观察副作用。
- [ ] **Step 4:** 先用未接入共同分派的基线或临时断开的新接线运行，明确区分“旧 longjmp 功能通过”与“新 dispatcher 接线审计失败”；恢复实现后运行 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`。本任务的行为断言若在旧实现也通过，这是保真回归，RED 由 Task 6 接线审计与 marker gate 缺失提供，不能捏造功能 RED。
- [ ] **Step 5:** 检查 `msr_safe` case 实际执行 known_bad `arch_rdmsr_safe(0x800)`，返回 `-EIO` 且 sentinel 不变；不能只检查 fabricated regs。现有测试已覆盖则不改测试体。提交 `test(intr): prove guest fault recovery and state restoration`。

### Task 6: 将 marker 和生产对象接线纳入现有 gate

**Files:** 修改三份 kernel-selftest Python 文件；创建 audit 脚本及 unittest；修改 `mk/components/run.mk`，必要时 `docs/build/build.md` 记录已有 bucket 的新增检查。

**Interfaces:**
- Produces: `v1_failures(log, *, suite=SUITE, require_intr_dispatch=False)`；开启选项时额外要求 uaccess/msr_safe 被 SELECT、terminal PASS，以及精确恢复 marker 在 uaccess BEGIN 与 PASS 之间恰一次。普通协议解析仍用 parse_v1，不回退 legacy `failures()`。
- Produces: `run_kernel_selftest.py` 的真实 x86 gate 显式传 `require_intr_dispatch=True`。generic parse API 默认不强制 x86 特性，aarch64 harness 不受污染。
- Produces: `intr_dispatch_audit.py --profile PROFILE --elf ELF --llvm-nm TOOL --llvm-objdump TOOL --repo-root ROOT`，失败非零；通过输出 `[intr-dispatch-audit] OK`。

- [ ] **Step 1:** 在 `test_kernel_selftest_result.py` 添加命名正反例：完整 uaccess+msr_safe PASS/单 marker 成功；missing/duplicate/wrong-case marker、仅 legacy PASS、未 SELECT、FAIL、缺 END、marker 在 uaccess 区间外均失败。生产 runner 流式分块穿过 marker、marker 出现后后续 FAIL 的 fixture 也失败，不能见 marker 就停止。
- [ ] **Step 2:** 运行 `python3 -m unittest qemutests.test_kernel_selftest_result` 得到 RED；更新 v1 gate 和 runner。旧成功 runner fixtures 增添有效 uaccess/msr_safe 选择及 marker，不放宽旧失败 fixtures。复跑 GREEN，并运行 `make PROFILE=x86_64-clang test-harness`。
- [ ] **Step 3:** 新 audit unittest 用小型 disassembly/nm fixtures 覆盖缺失/重复/weak 后端、旧 sync 汇编调用目标、x86 薄入口绕过 dispatcher、#PF 链内 CR2 重读、aarch64 早期调用链含 `__stack_chk_fail`/普通日志/分配、意外变更 frame/gate/返回标签和跨 ISA 依赖。除 direct call/tail-branch 外，必须覆盖 x86 `-mcmodel=large` 的 `movabs` 载入符号绝对地址再间接 call/jump，以及四 stub 的 `leaq do_*`/`xchg` 写入 FUNC 槽、公共 `error_code` 从槽载入 `%rdx` 后 `callq *%rdx`。每种间接形态均有「ELF 符号解析为预期目标」「解析到错误目标」「目标不可解析」三类 fixture，后两类非零失败；未知格式不能直接判成功。单次 CR2 规则仅作用于 #PF 薄入口到私有业务处理的链，另加 #GP fatal 保留诊断读取仍通过的 fixture。
- [ ] **Step 4:** 实现审计：结合生产 ELF 符号表、实际指令数据流/现场 FUNC 槽传递与源码，逐条证明四个 stub → 公开 C 薄入口 → 共同函数 → 架构后端的边，包括绝对地址间接调用；需要证明的边无法解析则失败，不以“存在间接 call”或源文件出现函数名代替。全部强定义唯一、早期可达小调用链无禁依赖、#PF 链单次 CR2 快照、真实 copy 故障分支和 Task 5 路径存在。正常保留的 #GP fatal CR2 读取不在 #PF 单次快照计数范围。GIC 动态 handler 表保留既有控制器边界，由既有 GIC host 测试和明确的后端调用/EOI 归属检查证明，不要求静态解析任意设备 callback。保留审计输入和关键摘录到既有日志目录。
- [ ] **Step 5:** 把新 unittest 加入 `TEST_HARNESS_MODULES`。审计通过已有 `RUN_STATIC_AUDIT` 接入 x86 `test-static` 和 ARM `test-aarch64-audit`：x86 传既有 `$(LLVM_NM)`/`$(LLVM_OBJDUMP)` 与 `$(KERNEL_BUILD_DIR)/kernel.elf`；ARM 传既有 `$(AARCH64_NM)`，并在 `mk/components/run.mk` 新增可覆盖的 `AARCH64_OBJDUMP ?= llvm-objdump` 后传 `$(AARCH64_OBJDUMP)`，ELF 使用该 target 已有的 `$(BUILD_DIR)/artifacts/kernel.elf`。不引入 x86 toolchain include；保留 ARM 原有 vma/uaccess/fork 符号排除 gate。审计运行前检查两工具参数非空、可执行（裸程序名经 PATH 解析），缺失时配置错误非零，不可静默跳过。框架 fixtures 以假工具验证 x86/ARM 入参、覆盖值生效及空值/缺失工具失败，不依赖实际 kernel 构建或 QEMU。不创造新 root alias，不在 python 审计中启动 build/QEMU。
- [ ] **Step 6:** `make PROFILE=x86_64-clang test-harness`、`test-static`、`make PROFILE=aarch64-clang test-aarch64-audit` 全通过，并运行真实 x86 kernel-selftest 验证新 gate。提交 `test(intr): gate recovery evidence and native dispatch wiring`。

### Task 7: 完整验收、证据与范围收尾

**Files:** 修改 `docs/arch.md`、`docs/roadmap.md`、`docs/changelog.md`；新增 `docs/superpowers/reports/2026-10-23-intr-dispatch-phase1-validation.md`（实际执行跨日时采用当天日期）。

**Interfaces:** Consumes: Tasks 1–6；Produces: 可复跑测试报告、实际最终 revision、已完成/剩余范围，不新增运行期功能。

- [ ] **Step 1:** 记录最终树 revision、dirty 状态、compiler/QEMU/profile/variant 与测试日志位置。先做双架构正常构建：`make PROFILE=x86_64-clang kernel.bin` 与 `make PROFILE=aarch64-clang aarch64-uefi`；确认无破坏性注入 flag、无临时排除/调试插桩。
- [ ] **Step 2:** 运行 x86 `make PROFILE=x86_64-clang test-host`、`test-harness`、`test-static`、`test-qemu SUITE=phase-0`；分别运行 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` 与 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`。报告按 case 名检查 signal、COW、uaccess、safe-MSR，不固定旧测试总数。
- [ ] **Step 3:** 依次运行 `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault`、`MODE=smp`、`MODE=gic-spi`、`MODE=m3-probe` 和 `make PROFILE=aarch64-clang test-aarch64-audit`。要求 SMP 1/2/4×3，sync-fault 原字段/首报/无 returned；spurious 无 EOI、普通 GIC 完整 IAR 恰一次 EOI 由既有生产 GIC host tests和调用链审计共同证明；真实 SPI/SGI/Timer/shootdown 不退化。
- [ ] **Step 4:** 每个失败保留日志、命令和退出状态。环境基线问题在未改代码同配置重跑取证，尤其 UDP NIC；不降低断言、不以“预期超时”视为成功。验证失败须先修复/解释并复跑受影响 gate，不能将未通过矩阵写为已闭环。
- [ ] **Step 5:** 更新文档，明确已统一的六条入口（ARM 两条、x86 四条）；其余 x86 异常/IRQ、EL0、M4、上下文切换和 kernel_main 继续待实施。修正 arch.md 中与本次直接相关的过时行数/范围，不借机重写无关架构文档。
- [ ] **Step 6:** 自查 spec §1–11 每条要求对应本计划任务，`git diff --check` 通过；执行最终变更审阅，处理实质问题后只重跑受影响 gate。提交 `docs: record interrupt dispatch phase one validation`；实现者按所选执行技能完成最终代码审阅和交付，不自动合并/发布。

## 计划自审覆盖

| Spec | 执行任务 |
|---|---|
| §1–4 意图、有限迁移与不做范围 | Tasks 3–4、7 |
| §5 公共接口、事件生命周期、强定义、bad-event、早期依赖 | Tasks 1–3、6 |
| §6 解码、CR2/ESR/FAR 快照、真实接线 | Tasks 1、3–4、6 |
| §7 四种控制流与无尾部资源 | Tasks 2、4–6 |
| §8 原诊断与 ABI/log 边界 | Tasks 3–4、6–7 |
| §9.1 host/对象、§9.2 ARM、§9.3 x86/真实恢复 | Tasks 1–7，真实恢复特别见 Tasks 5–6 |
| §10–11 文件职责、实施选择和后续项目 | 文件表、Tasks 3–4、7 |

本计划只安排实施，不代表已执行 RED/GREEN、产品构建或 QEMU 验收。当前任务完成于写入和自审 plan；实现方式需用户选择后再开始。
