---
title: OS01 中断与异常分派统一第一期
created: 2026-10-23
updated: 2026-10-23
type: spec
status: draft-for-review
tags: [osdev, aarch64, x86_64, interrupt, exception, arch]
---

# 中断与异常分派统一第一期：事件描述与返回契约

## 1. 意图与成功标准

本项目是 roadmap P2「中断/异常 dispatch 统一」及 Spec A 的第一个增量，为 aarch64 M4 用户态和后续启动入口统一建立可复用的 C 分派边界。实现必须让现有两种架构的入口实际经过共同分派函数，同时保留架构专属现场、控制器协议、故障恢复与终止机制。

交付后，aarch64 EL1h sync/IRQ 和 x86_64 的四类代表性同步异常进入共同 `arch_intr_dispatch()`；已有 EL1h 致命诊断、IRQ 返回、x86 用户缺页/COW/uaccess 与用户异常信号终止行为保持。第一期不宣称全部硬件中断入口、全部 x86 异常或 M4 已统一。

本文件以 2026-10-23 工作树、HEAD `abb89b03` 为调查基准。用户已选择上一轮推荐的项目并要求撰写 spec；下述接口细节、四类 x86 迁移范围及验收方案是本 spec 的设计提议，尚未经用户批准。本文不授权开始产品代码实现。

## 2. 已核实现状

| 区域 | 当前实现与约束 |
|---|---|
| aarch64 入口 | `kernel/arch/aarch64/intr/entry.S` 的 `exception_vectors` 为 16 槽、128 字节步长、2 KiB 对齐；offset 0x200 接 EL1h sync，0x280 接 EL1h IRQ。EL0、FIQ、SError 等其余槽仍是占位分支。 |
| aarch64 现场 | `kernel/include/arch/aarch64/regs.h` 定义 272 字节 `pt_regs_t`，包含 x0–x30、SP_EL0、ELR_EL1、SPSR_EL1；sync 汇编在进入 C 前另行快照 ESR/FAR。 |
| aarch64 sync | 直接调用 `aarch64_el1_sync_fatal(regs, esr, far)`，屏蔽 DAIF、CAS 抢首报、使用无锁早期输出、永久停机。不能依赖 per-CPU 初始化、GIC、Slab、调度器或普通日志。 |
| aarch64 IRQ | `el1_irq()` 调 `gic_dev_dispatch(gic_dev_current(), regs)`；GIC 后端读取 IAR、分派 INTID 并写 EOIR。TLB SGI 3 handler 不自行 EOI。 |
| x86 同步异常 | `kernel/arch/x86_64/intr/trap.c` 当前为 1133 行；存在用户异常信号映射、#GP safe-MSR fixup、#PF uaccess longjmp、VMA/demand/COW 与致命路径。旧 `docs/arch.md` 所称 3065 行不是当前工作量依据。 |
| x86 返回 | `entry.S` 有 `ret_from_intr` 的 softirq/调度/信号处理和独立异常返回标签；不同 stub 的实际跳转不能仅按注释推断。用户终止 helper 直接换到任务栈后调用 `do_exit()`。 |
| 既有 IRQ facade | `arch_irq_dispatch()` 属于 IRQ 控制器路径；x86 实现在 `intr/irq_hooks.c`。它不等于本 spec 的事件分派入口，也不能用 aarch64 向量槽编号冒充 GIC INTID。 |
| 内存前置 | M0–M3 已闭环，M3 并发 replace 缺陷已修复；aarch64 用户页表、EL0 异常和真实 uaccess 恢复仍属于后续 M4。 |

## 3. 方案比较与选择

1. **只加规范化解码 helper。** 改动最小，但真实入口仍各走各路，无法验收共同分派边界；不选。
2. **共同事件入口 + 每架构处理后端，有限迁移。** 共享层只决定事件通道，现有复杂处理继续由后端执行；能用真实 IRQ、可返回 fault 和致命 fault 验证。采用此方案。
3. **一次抽出 VMA/COW、用户信号和全部异常策略。** 会同时涉及 M4、调度/异常栈和内存语义重构，超出一个独立增量；后续单独设计。

本轮用户选择的是项目方向；这里将其收敛为方案 2，作为待审设计选择。与 roadmap 简写 `arch_intr_dispatch(vector, regs)` 相比，本 spec 使用带类型的事件结构，避免把 IDT vector、VBAR 槽、syndrome 与 INTID 混成一个整数。

## 4. 范围与明确不做的部分

本期必须迁移：

- aarch64 已接通的 EL1h sync 和 EL1h IRQ C 落点。
- x86 #DE（0）、#UD（6）、#GP（13）、#PF（14）的 C 入口；覆盖用户信号终止、safe-MSR fixup、缺页恢复三种控制流。
- 共同分派、纯事件解码及对应 host/QEMU 验证，双架构构建输入与调用链审计。

本期不接通 EL0/SVC，不新增 aarch64 可恢复缺页、uaccess、用户页表、ASID、COW 或 signal frame；不实现上下文切换、统一 SMP、统一 `kernel_main`、GICv2 Phase 2；不统一两种 `pt_regs_t` 布局。不改 x86 syscall 分派、NMI、#DF、#MC 和其余异常入口；不迁移 x86 外设 IRQ/LAPIC timer/IPI/softirq 的汇编入口。不把 IRQ controller 注册表或 ACK/EOI 协议合并。

禁止为使接口看起来完整而加入返回成功的 aarch64 恢复 stub，或将未知异常、未知 syndrome、无效故障地址当成可恢复缺页。

## 5. 接口与文件职责

### 5.1 共同接口

新增 `kernel/include/intr/dispatch.h` 与对应 `kernel/intr/dispatch.c`，定义下列语义；最终声明须按此契约实现：

```c
enum intr_kind { INTR_SYNC, INTR_IRQ };
enum intr_origin { INTR_ORIGIN_KERNEL, INTR_ORIGIN_USER,
                   INTR_ORIGIN_UNKNOWN };
enum intr_reason { INTR_REASON_OTHER, INTR_REASON_MEMORY,
                   INTR_REASON_ARITHMETIC, INTR_REASON_ILLEGAL,
                   INTR_REASON_PROTECTION };
enum intr_access { INTR_ACCESS_UNKNOWN, INTR_ACCESS_READ,
                   INTR_ACCESS_WRITE, INTR_ACCESS_EXEC };

struct intr_event {
    enum intr_kind kind;
    enum intr_origin origin;
    enum intr_reason reason;
    enum intr_access access;
    bool fault_addr_valid;
    uint64_t fault_addr;
    uint64_t native_vector;
    uint64_t native_status;
    uint64_t native_address;
};

void arch_intr_dispatch(const struct intr_event *event, pt_regs_t *regs);
```

公共头包含所需标准类型头和 `arch/regs.h`，不定义 ISA 寄存器、PTE 位或控制器类型。`pt_regs_t` 仅作为传递给当前架构后端的现场指针；共同实现不得读取其成员。不同架构独立编译，并无跨 ISA 二进制结构 ABI。

事件在入口适配器的栈上完整初始化，生命周期仅覆盖本次调用。无效地址规范为 `fault_addr_valid=false, fault_addr=0`；地址 0 也可能是有效故障地址，不能用数值替代有效位。字段是只读快照，后端不得保存指针、异步排队或通过全局/per-CPU 单槽暂存。支持已有嵌套入口，不引入共享可变分派状态。

`native_vector/native_status/native_address` 是架构私有的不透明载荷，共同代码只能透传。同步事件中分别为 x86 vector/error code/CR2 快照（非 #PF 的 native_address 为 0），或 aarch64 VBAR 槽 offset/ESR/FAR 原始快照。native_address 保存诊断所需原始值，即使 FAR 无效也不丢失；其存在不赋予 fault_addr 有效性。aarch64 IRQ 为 `native_vector=0x280, native_status=0, native_address=0`，所有 fault 字段取 OTHER/UNKNOWN/无效；INTID 由 GIC 后端读 IAR 后确定，不进入这些字段。`reason` 表示硬件类别，不表示已经满足恢复条件。

### 5.2 架构后端

新增 `kernel/include/arch/intr.h`，声明：

```c
void arch_intr_handle_sync(const struct intr_event *, pt_regs_t *);
void arch_intr_handle_irq(const struct intr_event *, pt_regs_t *);
__attribute__((noreturn))
void arch_intr_bad_event(const struct intr_event *, pt_regs_t *);
```

每架构必须提供强定义；缺失后端应链接失败，不提供 weak no-op。源与头保持目录对称：后端可位于 `kernel/arch/<arch>/intr/dispatch.c`，架构专属解码声明位于 `kernel/include/arch/<arch>/intr_decode.h`；公共 dispatcher 不包含这些私有头。

共同入口只验证非 NULL 的 event/regs 和 kind，然后调用对应后端。非法 kind 或 NULL 参数调用 `arch_intr_bad_event()`，不打印、不继续返回。其他字段组合由架构后端验证；已迁移事件的向量不在白名单、同步入口声称未知 origin、或载荷与来源不一致时进入 bad-event，禁止默认返回。纯解码函数可以报告 UNKNOWN，入口必须据此停机；保留 raw 值供诊断。

bad-event 是内部契约损坏路径：使用 `arch_early_puts()` 输出固定前缀 `[intr-dispatch] FATAL bad-event`，屏蔽本核中断并永久停机；aarch64 屏蔽完整 DAIF。不解引用可能为空的参数，不访问 current/per-CPU，不调用普通 logger。它不替代正常同步异常原有诊断。

x86 IRQ 后端本期仅作为链接完整性的 fail-closed 后端：生产中没有调用者，调用即 bad-event；不能路由到 `arch_irq_dispatch()` 冒充已经覆盖所有 IRQ。aarch64 IRQ 后端则调用现有 GIC 分派器。

共同入口、解码、aarch64 sync 适配与 bad-event 整条早期可达链禁止分配、锁、普通日志、current/per-CPU 访问、栈 canary 依赖及启动状态 gate。相应函数须按现有早期致命路径应用 `no_stack_protector`，并审计最终对象中的调用与编译器插桩。IRQ 后端及原 x86 业务 handler 按其原有运行期契约执行。

## 6. 解码规则与真实调用链

### 6.1 x86_64

迁移四个公开 `do_*` C 落点为薄入口：快照现场 → 构造事件 → `arch_intr_dispatch()`。原业务体改为架构私有 handler，由 `arch_intr_handle_sync()` 按原生向量显式 switch 调用；禁止后端重新调用公开入口形成递归。其他 `do_*` 不经共同入口。

异常来源按保存的 CS 特权级确定：CPL 0 为 KERNEL、CPL 3 为 USER、其余 UNKNOWN。#PF 来源不按 error code 的 U/S 位取代 CS。#PF 的 CR2 在 C 落点最前端读取一次，先于任何可能导致二次 fault 的普通 helper；解码和原业务体共用该快照，原业务体不得重读 CR2。error code 保留原值，PTE 查询和 COW 写入仍留在 x86 后端。

| vector | reason | access / 地址 |
|---|---|---|
| #DE 0 | ARITHMETIC | UNKNOWN / 无效 |
| #UD 6 | ILLEGAL | UNKNOWN / 无效 |
| #GP 13 | PROTECTION | UNKNOWN / 无效；不能拿 CR2 当 #GP 地址 |
| #PF 14 | MEMORY | I/D=1 为 EXEC，否则 W/R=1 为 WRITE，否则 READ；CR2 有效 |

#GP safe-MSR fixup 必须先于原有用户终止/内核 fatal 路径，保持现场 RIP 改写。#PF uaccess `fault_jmp` 判断、VMA 权限、heap absent-leaf 禁止 demand-map、COW 引用计数、失败释放和 TLB 行为按现有执行顺序保留。拆分只传快照，不将这些策略搬到公共层。

本期不改汇编 frame 布局、IDT gate/IST 设置或返回标签。实现前后须逐个审计四个 stub 的真实跳转和原 handler 的所有 return/nonreturn 分支。异常栈上的 current 获取、换任务栈退出仍由现有 x86 helper 拥有；共同入口不得运行 softirq、schedule 或 signal delivery。

### 6.2 aarch64

EL1h sync 汇编保持完整现场和 ESR/FAR 参数快照，C 调用目标改为新的 `aarch64_el1_sync(regs, esr, far)` 适配器；声明 `noreturn, no_stack_protector`。适配器构造事件并进入共同入口；后端调用原 `aarch64_el1_sync_fatal()`。共同入口按整体定义可返回，但该适配器中若它意外返回，必须直接执行 bad-event 停机；汇编保留现有 DAIF+WFI 兜底，不能增加 `eret`。

来源根据所接向量与保存的 SPSR 模式交叉检查：本期只接受 EL1h（M[4:0]=0b00101）。纯解码器可将 EL0t 标为 USER，其他模式 UNKNOWN；EL1h 入口收到非 EL1h 帧必须 bad-event，不能藉此宣称 EL0 已接通。IRQ 适配器同样校验保存模式。

同步类别按 ESR.EC：instruction/data abort 为 MEMORY；PC alignment 为 PROTECTION；其余保守为 OTHER。instruction abort 的访问类型为 EXEC，data abort 只有在 syndrome 确认 WnR 可解释时解码 READ/WRITE，否则 UNKNOWN；cache-maintenance、外部 abort、未识别 FSC 等复杂情况一律 UNKNOWN，不据此恢复。第一期数据 abort 的 READ/WRITE 白名单仅为 translation fault FSC 0x04–0x07、access-flag fault 0x08–0x0b、permission fault 0x0c–0x0f，且 CM=0、S1PTW=0；其他保留 raw ESR。

地址有效性复用现有 `aarch64_sync_far_valid()` 真值源，不另建一份不一致的 FnV 规则；无效时仅将规范化字段置为 `fault_addr_valid=false, fault_addr=0`，`native_address` 始终保留原始 FAR。后端以 `native_status/native_address` 中的原始 ESR/FAR 调用原 fatal 函数，保持既有 `far=n/a` 与首报协议。该 helper 的既有语义不在本期扩大或修正。

EL1h IRQ：`el1_irq(regs)` → 构造 IRQ 事件 → 共同入口 → aarch64 IRQ 后端 → 原 GIC dispatcher → 原 handler/EOI → 原汇编恢复与 `eret`。不能在共同层读 IAR、二次 EOI、提前开启 IRQ、改写 DAIF/SPSR 或插入调度点。GIC spurious 和未注册 INTID 的处理保留后端现状。

## 7. 返回、恢复和终止契约

本期采用 `void` 分派 API，不新增一个要求旧 handler 伪造结果的返回枚举。以下四种是控制流语义，不是四个都已在 aarch64 实现的功能：

| 语义 | 入口／后端义务 | 本期真实覆盖 |
|---|---|---|
| 正常返回 | 后端返回时，现场已可按原路径恢复；共同层不再次处理、不额外推进 PC | aarch64 IRQ；x86 已修复缺页等 |
| 故障恢复 | 原指令重试、修改返回 RIP，或跳转已注册恢复现场；恢复机制由架构拥有 | x86 #PF demand/COW、#GP safe-MSR；uaccess longjmp 可直接越过 dispatcher，不返回它 |
| 用户任务终止 | 按原映射记录信号，切至合法任务栈并进入 do_exit；不恢复故障用户帧 | x86 #DE/#UD/#GP/#PF 原有终止路径 |
| 内核致命停机 | 无返回；保留架构原诊断与本核停机，不承诺其他核全停 | aarch64 EL1h sync；x86 原有内核 fatal |

共同入口及适配器不持有锁、引用、堆内存或需清理的资源，使 x86 nonlocal longjmp/do_exit 绕过调用链时不泄漏。不能在调用后补必需清理、IRQ ACK 或恢复 IRQ 状态。事件不跨任务切换存活。

已有 x86 用户终止 helper 在没有合法用户 task 时可能返回；本期保留其既有行为并记录为独立风险，不把它当作新的成功恢复案例。若实施过程中证明此行为会被本次迁移新触发，应停止扩大改动并报告证据，而非顺手重写任务退出。正常 USER fault E2E 必须证明目标任务确实终止且父任务存活。

## 8. 早期诊断、日志与兼容性

aarch64 原 `[aarch64-sync] FATAL ...` 字段、顺序、宽度、大小写、首报 CAS、`far=n/a` 和停机语义逐字保持。正常事件不新增每次 IRQ/fault 日志，不依赖 malloc/vsnprintf。普通运行期新增日志遵守 `log_*`；早期致命路径沿用既有 lock-free early-print 契约，不扩大 operational log 豁免。

不改变 boot_context、UAPI、用户信号 frame、pt_regs、percpu/task 结构，不引入额外结构 ABI。若实施发现确需改变既有共享结构，应重新审设计并按仓库要求 clean rebuild。本项目新增事件结构仅为本次内核源码接口。

## 9. 验证与验收证据

本节是未来实现的验收要求，不代表撰写 spec 时已经执行测试。现有 target 必须复用；新增用例注册到现有 hosttests/test bucket，不另造旁路 runner 或未登记 make alias。

### 9.1 Host 与编译边界

- 对生产纯解码代码做表驱动测试：x86 四向量、CS 来源、#PF read/write/exec、地址 0；aarch64 同/低 EL abort、SPSR 模式、FnV、PC alignment、OTHER 类和复杂 data abort 保守 UNKNOWN。测试数据对照独立常量/期望表，不能以同一 helper 生成预期。
- 明确覆盖 FnV=1 且原始 FAR 非零的事件：规范化地址无效并归零，`native_address` 仍等于注入的原始 FAR，后端交给 fatal 的原始 ESR/FAR 不变，不能因规范化丢失诊断载荷。
- 使用 spy 架构后端测试生产 dispatcher：SYNC/IRQ 各调用一次，原 event/regs 指针原样透传；正常返回不改现场、不 ACK、不进入调度；NULL/坏 kind 进入 nonreturn bad-event。死亡路径用隔离进程或受控测试逃逸，不能把它 mock 成正常返回。
- 分别验证 x86 后端四项白名单、入口与私有 handler 无递归、未迁移向量 fail-closed；aarch64 sync 保留原 ESR/FAR 给 fatal，IRQ 恰一次调用 GIC。x86 IRQ 后端必须拒绝调用。
- 用 host 可控 nonlocal escape 验证分派链允许恢复路径跳出且没有尾部副作用；这只验证 C 边界，不能替代真实 guest 的 uaccess/IST 验证。
- 双架构编译、nm/反汇编及源码审计确认共同函数和三个后端符号的强定义唯一、实际入口调用经过共同函数；测试生产对象，不能只测影子实现。确认 aarch64 白名单包含新增共同源，早期调用链无 canary/普通 logger/调度依赖，未引入 x86 PTE/寄存器访问。

### 9.2 aarch64 QEMU

| 命令 | 必须证明 |
|---|---|
| `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 原独立变体：armed 后恰一次完整 FATAL、正确 EC/FAR、无 returned；证据出现后 runner 主动终止，静默超时失败。 |
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 既有 1/2/4 CPU × 3 次矩阵、timer/SGI、Slab/M3/multicore 均满足现有断言。 |
| `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 外设 SPI 仍正确分派、EOI，无中断风暴或漏中断。 |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 生产镜像跨核 shootdown probe 的 START→OK，不接受 SKIP/FAIL。 |
| `make PROFILE=aarch64-clang test-aarch64-audit` | M3 既有 aarch64 用户态符号排除 gate 继续通过。 |

正常镜像不得带破坏性注入。同步 fatal 仍能发生于 GIC/per-CPU 启动之前；共同入口接入不能新增 readiness 门槛。实际调用接线由目标对象/链接反汇编审计提供证据，无需给每次 IRQ 加日志。

### 9.3 x86_64 与回归结果处理

- `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 验证正常启动。
- `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` 验证 syscall 与用户故障终止，必须包括 #DE→SIGFPE、非法访存→SIGSEGV、#UD→SIGILL，以及 mmap/fork/COW、保护页和用户指针边界；不同时设置 `KERNEL_SELFTEST=1`。
- `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest` 单独覆盖下述真实 uaccess 非局部恢复及已有 safe-MSR probe；先确认测试实际注册并运行，不能以总 PASS 代替对应路径证据。
- `make PROFILE=x86_64-clang test-host` 与 `make PROFILE=x86_64-clang test-static` 完成 host/静态回归。

**真实 guest 非局部恢复是独立硬门槛。** 在现有 `uaccess` 内核 selftest 中补足以下用例，不新增生产恢复机制：

1. 选用测试独占的低地址页，保存其原映射/中间页表状态，在 live 页表中明确置为 absent 并完成所需 TLB 失效；预检确认不存在有效 leaf 或 huge/block 映射。不能仅凭固定地址“通常在空洞里”断言未映射。探针在单个受控 selftest 上下文执行，不持锁、不与其他 CPU 的该地址访问竞争。
2. 保存原 `addr_limit`、`fault_jmp`、`fault_cleanup`、`fault_cleanup_arg`，将探针地址置于当前用户范围。在生产 `copy_from_user_ft_res()` 上读取该 absent 页，长度为非零，目标为有效内核缓冲；该生产路径没有 `prepare_user_write_range()` 预检查，必须实际执行读取。复用已有 `strnlen_user()` unmapped 用例时也要求相同 absent 前置条件，但它不能替代 `_res` cleanup 恢复断言。
3. 必须经过真实 #PF → `do_page_fault` 薄入口 → `arch_intr_dispatch()` → x86 私有 #PF handler → `__builtin_longjmp`。源码与目标反汇编审计须确认探针调用生产 copy 函数、copy 的该失败返回仅能来自 setjmp 恢复分支、#PF 接线经过共同入口；不得直接调用 C handler、伪造返回值或以 host longjmp 测试代替 guest 路径。
4. 返回值必须是 `-EFAULT`，probe cleanup 计数恰一次；原 `fault_jmp`、`fault_cleanup`、`fault_cleanup_arg` 已恢复。随后显式完成一次有效读取，断言成功且不调用 cleanup，确认任务能够继续执行。恢复 `addr_limit`、原映射与测试页表资源；包括断言失败在内的退出均须清理，不能遗留 live 页表改动。
5. 在上述断言与清理均成功后，通过已有 selftest 协议输出精确子用例标记 `[selftest] uaccess: dispatch longjmp restore OK`。现有 kernel-selftest runner 对该标记恰一次及 `uaccess` terminal PASS 作联合检查；未出现、重复、只有总体 PASS、异常退出或超时均失败。该标记仅属于自测协议，不新增生产 IRQ/fault 日志。

现有 `test_uaccess.c` 的 `copy_to_user_ft_res()` unmapped 输出用例可在 prepare 阶段直接返回 `-EFAULT`，即使执行了 cleanup 也不能充当上述证据。已有 `strnlen_user()` unmapped 用例具备真实 #PF/longjmp 路径，但仅凭旧注释或旧总 PASS 不能证明新分派链和 `_res` 状态恢复，必须满足本节的具体断言。

QEMU 验证保存命令、profile/variant、源码 revision、guest 日志与退出状态。已知基线失败须在未改基线同环境复现并单列，不能悄悄降低断言、固定旧测试总数或以日志中单个 PASS 代替全套结果。尤其 syscall NIC 配置可能影响 UDP setup，应按现有 harness 能力在相同网络配置比较。

## 10. 预计变更位置与交付边界

| 位置 | 责任 |
|---|---|
| `kernel/include/intr/dispatch.h` / `kernel/intr/dispatch.c` | 公共事件与无状态共同入口 |
| `kernel/include/arch/intr.h` | 强后端声明 |
| `kernel/include/arch/<arch>/intr_decode.h` / `kernel/arch/<arch>/intr/dispatch.c` | 纯解码、明确后端路由和 bad-event |
| `kernel/arch/x86_64/intr/trap.c` | 四入口适配、原业务私有化与 CR2 单次快照 |
| `kernel/arch/aarch64/intr/trap.c` / `entry.S` | sync/IRQ 适配、sync C 目标调整及 nonreturn 兜底 |
| `kernel/include/arch/aarch64/sync_fault.h` | 复用现有 FAR 判断；如需声明调整不能改变原判断真值 |
| `kernel/Makefile` / `hosttests/Makefile` / `hosttests/cases/` | 双架构源接入及有注册的 host 用例 |
| `kernel/selftest/` / `qemutests/` | 复用并补足可观测的恢复/异常验收路径 |
| `docs/roadmap.md` / `docs/arch.md` / `docs/changelog.md` | 实施闭环后记录第一期覆盖范围，保留剩余 Spec A/M4 状态 |

本次仅新增 spec，不修改 roadmap 完成状态。后续实施可按「公共契约与解码 → aarch64 两路径 → x86 四路径 → 全回归」形成可审查增量，但最终交付必须具备全部已约定路径，不能只交 header 或 host mock。

## 11. 交给实施者的选择与后续项目

实施者可决定私有 helper 名称、纯解码函数在私有源内还是独立源、host fixture 组织、反汇编审计脚本细节；不得改变本期入口覆盖、强链接/无早期依赖、信号映射、控制流、GIC EOI 归属或验收强度。

后续需单独设计：剩余 x86 异常和 IRQ 接入、EL0 sync/IRQ/SVC、可恢复 aarch64 abort/uaccess、架构中立 VM fault resolver、上下文切换与返回用户态、统一 SMP 和 kernel_main。本期四类控制流契约供这些项目沿用；若未来需要显式 disposition 返回值，应在真实调用者和后端机制确定后再演进，不预先加入无实现的 action enum。
