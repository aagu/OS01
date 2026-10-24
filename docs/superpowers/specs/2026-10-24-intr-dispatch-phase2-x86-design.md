---
title: OS01 中断分派第二期：补齐现有 x86 入口
created: 2026-10-24
updated: 2026-10-24
type: spec
status: draft-for-review
tags: [osdev, x86_64, interrupt, exception, arch]
---

# 中断分派第二期：补齐现有 x86 入口

## 1. 目标与调查基线

用户在 phase1 实施后选择「补齐 x86 入口」。本期让当前实际安装的 x86 异常和硬件中断入口全部经过 `arch_intr_dispatch()`，保留原 handler、controller ACK、LAPIC EOI、异常现场和返回路径。这里的「全部」有明确清单：20 个已安装异常向量，以及 28 个硬件中断向量；不等于 CPU 的全部异常特性或 IDT 256 个向量都获得支持。

调查基线为 2026-10-24、HEAD `48992754`，工作树干净。phase1 已交付 aarch64 EL1h sync/IRQ、x86 #DE/#UD/#GP/#PF；此前 m3-probe wire 漂移已在该 HEAD 修复并正式验收。历史报告保留原失败，后续依据见 `docs/debug/2026-10-24-shootdown-probe-investigation.md`。

本文件为第二期待审 spec，不代表代码已实现或新矩阵已执行。aarch64 可恢复 abort、EL0/M4、调度器上下文切换和统一 kernel_main 仍留后续。

## 2. 方案选择

上一轮比较了「aarch64 EL1h 恢复」「补齐 x86 入口」「直接进入 M4」，用户选择第二项。在此方向下采用 **保留汇编 ABI、迁移 C 落点、按原生 vector 路由至现有业务末端** 的方案。

没有选择一次生成全新异常/IRQ stub 系统：现有 NMI 独立 stub、带/不带硬件 error code 的公共入口、LAPIC timer 手写 stub 与 spurious 直接 iret 的行为不同，统一重写会引入额外栈/返回风险。也不只迁移普通外设 IRQ 后宣称 x86 补齐：timer、IPI、spurious 和其余异常都在本期清单内。

## 3. 当前真实入口与目标覆盖清单

### 3.1 异常：现有 sys_vector_install 的 20 项

| vector | 现有 C 落点后缀（`do_`） | error code 来源 | 原业务语义 |
|---|---|---|---|
| 0 | divide_error | stub 传 0 | phase1 已接；用户 SIGFPE / 内核 fatal |
| 1 | debug | stub 传 0 | 原 fatal；本期不新增调试恢复 |
| 2 | nmi | NMI C 参数显式 0 | 原 fatal；独立 stub/返回分支 |
| 3 | int3 | stub 传 0 | 原 fatal，包含用户触发时的既有行为 |
| 4 | overflow | stub 传 0 | 用户 SIGFPE / 内核 fatal |
| 5 | bounds | stub 传 0 | 用户 SIGFPE / 内核 fatal |
| 6 | undefined_opcode | stub 传 0 | phase1 已接；用户 SIGILL / 内核 fatal |
| 7 | dev_not_available | stub 传 0 | 用户 SIGFPE / 内核 fatal |
| 8 | double_fault | CPU error code | 原 fatal；IST 3 专用栈 |
| 9 | coprocessor_segment_overrun | stub 传 0 | 用户 SIGFPE / 内核 fatal |
| 10 | invalid_TSS | CPU error code | 用户 SIGSEGV / 内核 fatal |
| 11 | segment_not_present | CPU error code | 用户 SIGBUS / 内核 fatal |
| 12 | stack_segment_fault | CPU error code | 用户 SIGBUS / 内核 fatal |
| 13 | general_protection | CPU error code | phase1 已接；safe-MSR fixup 优先 |
| 14 | page_fault | CPU error code | phase1 已接；CR2 单次快照、uaccess/VMA/COW |
| 16 | x87_FPU_error | stub 传 0 | 用户 SIGFPE / 内核 fatal |
| 17 | alignment_check | CPU error code | 用户 SIGBUS / 内核 fatal |
| 18 | machine_check | stub 传 0 | 原有用户 SIGBUS / 内核 fatal |
| 19 | SIMD_exception | stub 传 0 | 用户 SIGFPE / 内核 fatal |
| 20 | virtualization_exception | stub 传 0 | 用户 SIGBUS / 内核 fatal |

以上依据当前 `kernel/arch/x86_64/intr/trap.c::sys_vector_install()` 和 `entry.S`，不套用外部理想异常表。15、21–31 等未安装项不新增 handler；0x80 syscall 保持独立。NMI、#DF、#MC 进入既有 sync **分派通道**，这里的 kind 不表示它们在硬件层都属于同步可恢复 fault，不为它们新增用户恢复或 ACK。

### 3.2 硬件中断：当前 28 项

| vector | 现有入口与链 | ACK/EOI 所有者 | 返回 |
|---|---|---|---|
| 0x20–0x37 | 24 个 `IRQn_interrupt` → `arch_irq_dispatch` → irq_table | 当前 controller->ack(vector) | 原 ret_from_intr |
| 0x38 | `lapic_timer_stub` → `lapic_timer_handler` | 原 timer handler 的 lapic_eoi | 原 ret_from_intr |
| 0x40 | `_intr_stub_tlb` → `generic_intr_dispatch` → IPI handler | 原 TLB handler：flush→ack generation→lapic_eoi | 原 ret_from_intr |
| 0x41 | `_intr_stub_resched` → `generic_intr_dispatch` → IPI handler | 原 resched handler：need_resched→lapic_eoi | 原 ret_from_intr |
| 0xff | `lapic_spurious_stub` → 原直接 iretq | 无 EOI | 只恢复现场，不运行返回工作 |

`generic_intr_dispatch()` 和 handler 表实际定义在 `kernel/intr/vector_dispatch.c`，不是旧注释所写的 arch 子目录。现有生产 `DEFINE_INTR_STUB/REGISTER_INTR_HANDLER` 消费者只有 0x40/0x41。新 vector 的未来注册必须同步更新路由清单和审计；本期不把「表有 256 项」解释成任意 vector 已有支持。

## 4. 不变接口与架构私有增量

沿用 phase1 的 `struct intr_event`、枚举、`arch_intr_dispatch(const struct intr_event *, pt_regs_t *)` 和三个强后端声明。共同 dispatcher 不读取 regs 成员、不解释 vector、不 ACK、不调度，不需要新的 kind、通道 tag 或公共 payload 字段。aarch64 的解码、后端、汇编入口和诊断语义不变。

新增 x86 私有接口，均置于 `kernel/include/arch/x86_64/`，实现落在对应 `kernel/arch/x86_64/intr/`；入口适配器集中于新增的 `intr/irq_entry.c`（既有外设适配器可继续留在 x86 `irq_hooks.c`）：

```c
void x86_intr_decode_irq(struct intr_event *out, const pt_regs_t *regs,
                         uint64_t vector);
void x86_irq_entry(pt_regs_t *regs, uint64_t vector);
```

`x86_irq_entry()` 只用于当前 timer/spurious 新 C 落点；0x20–0x37 和 0x40/0x41 保留原公开入口名作为适配器。三个入口分别核验自己的向量族，避免一个外设适配器把 0x38 当普通 GSI 转发。它们构造栈上事件，调用共同分派，不拥有正常返回才能释放的资源。

公开符号 `generic_intr_dispatch()` 从公共 `kernel/intr/vector_dispatch.c` 移入 x86 `intr/irq_entry.c`；既有汇编与 `gate.h` 声明继续使用该符号，不需要改名。公共文件只保留注册表及原查表业务，后者重命名为中立 helper `void intr_dispatch_registered_vector(pt_regs_t *regs, uint64_t vector)`，通过新增配对头 `kernel/include/intr/vector_dispatch.h` 声明。它继续按原边界检查和传参调用已注册 handler，无 EOI、不重入共同 dispatcher，也不认识 x86 向量族。`x86_handle_registered_irq()` 留在 x86 架构目录，通过该中立头调用 helper。公共文件不得包含 x86 私有头或新增 x86 白名单；其既有仅供 x86 产品编译的源清单边界保持，不加入 aarch64 白名单，也不新增 ARM stub。

新增架构私有 `kernel/include/arch/x86_64/intr_vectors.h` 作为本期 vector 常量与成员清单的单一真值源，供 decoder/adapter/backend 使用；测试期望仍用独立常量，不能由该清单自动推导全部预期。实现可选 switch 或只读表，但不能用可变注册结果替代本期明确清单。

## 5. 同步入口迁移与规范化规则

将余下 16 个 `do_*` 改为 phase1 同型薄入口：原生参数 → 纯解码 → 共同分派。原业务移为架构私有 `x86_handle_<原后缀>(pt_regs_t *, uint64_t error_code)`；phase1 四个私有签名不变，#PF 仍额外传 CR2。后端显式接纳 §3.1 的 20 项，禁止回调公开入口形成递归。

reason 规则确定如下：

- ARITHMETIC：0、4、5、7、9、16、19。
- ILLEGAL：6。
- PROTECTION：10、11、12、13、17、20。
- MEMORY：14。
- OTHER：1、2、3、8、18；其余未安装 vector 即使纯解码为 OTHER，后端也拒绝。

只有 #PF 有 access 和有效地址；其 read/write/exec 解码和原始 CR2 不变。其余事件 `access=UNKNOWN, fault_addr_valid=false, fault_addr=0, native_address=0`；`native_status` 为入口实际收到的 error_code，没有硬件 error code 的入口传 0。NMI 的 frame 占位槽历史上保存了寄存器值，不能从 `regs->errcode` 冒充硬件 error code；必须继续使用独立 stub 传入的 C 参数 0。

origin 继续按保存 CS 的 CPL 判断，CPL 0/3 为 KERNEL/USER，其他 UNKNOWN 后端拒绝；#PF U/S 位不与 CS 强制等价。分类不改变原信号或故障严重性，尤其不能将 #DB/#BP 改成可返回，也不能把 #MC 的原行为悄悄改成新的机器检查策略。

所有 gate 类型、DPL、IST 保持；NMI 独立 `RESTORE_ALL` 分支、普通异常原 `error_code→ret_from_intr` 分支保持。#DF 保持 IST 3，#PF 保持 IST 0。不改硬件 error-code 的 push/pop 约定，不用软件 `int $8` 代替真实 #DF 来证明该约定。

新增适配器、纯解码、校验 helper 及 backend 均不得引入栈 canary、锁、分配、普通日志、current/per-CPU 或初始化状态依赖。原业务末端保留现有诊断/栈切换/停机行为，包括现有不适合保证任意重入 NMI 场景的 logger/backtrace；本项目不宣称修复 NMI 重入、栈损坏或硬件 #MC 的全部可靠性。实施若发现新增调用深度超过当前异常栈预算，必须报告并调整设计，不能以关闭栈审计消除问题。

## 6. IRQ 事件与按 vector 路由

IRQ 解码设置 `kind=INTR_IRQ`；origin 同样来自 CS；reason=OTHER、access=UNKNOWN、地址无效且为 0，`native_vector=vector`，`native_status=0, native_address=0`。不能把 GSI、参数指针或 EOI 状态伪装成 `native_status`。

x86 IRQ backend 按保存 regs 和 raw vector 重解码，逐成员校验（禁止比较含 padding 的事件字节），并检查 origin 与 §3.2 白名单。合法 vector 路由至下列架构私有业务函数，签名均为 `void fn(pt_regs_t *, uint64_t vector)`：

| 族 | 业务函数 | 约束 |
|---|---|---|
| 0x20–0x37 | `x86_handle_device_irq` | 原 arch_irq_dispatch 的处理体；参数仍是 IDT vector，内部只在本族转换 GSI |
| 0x38 | `x86_handle_lapic_timer_irq` | 调原 lapic_timer_handler(regs, 0)，不重做 BSP/AP tick 计账 |
| 0x40/0x41 | `x86_handle_registered_irq` | x86 目录中的业务桥接，调用公共 intr_dispatch_registered_vector；透传 vector/param/regs |
| 0xff | `x86_handle_spurious_irq` | 空业务，只返回，不读写 APIC、不 EOI、不查普通 irq_table |

保持 `arch_irq_dispatch(regs, vector)` 名称给原 24 stub：它改成只接受 0x20–0x37 的事件适配器，私有业务体不再调用这个公开入口。迁入 x86 `intr/irq_entry.c` 的 `generic_intr_dispatch(regs, vector)` 同理只接受 0x40/0x41；表和中立查表 helper 留在原公共文件，不借此改注册/卸载或移动驱动模型。0x38/0xff 的 `x86_irq_entry` 在 dispatch 前也要验证它们的族。禁止通过任何适配器递归回自己；中立 helper 不得反调公开适配器。

未知 vector、错族调用、越界或 NULL 现场属于内部契约错误，进入现有 `arch_intr_bad_event()`，不得变成伪中断成功；NULL 必须在解码访问 regs 前被拒绝。对于合法族内「没有注册 handler」保持旧行为：外设仍在 controller 非空时 ACK；IPI 注册表为空时保留原无 handler 返回/诊断路径且不 EOI，不新造一个兜底 ACK。后者是已有行为，不承诺从未注册的真实 IPI 中断风暴自动恢复。

新适配器不主动开 IRQ。外设业务体保留原 `arch_local_irq_disable()` 及 handler→controller ACK 顺序；timer/IPI 的 EOI 仍由原 handler 执行。共同层和 backend 路由函数一律不补 EOI。PIC slave 路径仍允许一次 controller ACK 调用产生 slave+master 两次端口写，不能拿“恰一次 ACK”错误限定硬件端口写次数。

## 7. LAPIC spurious 的必要入口增量

0xff 是本期唯一必须扩展保存现场的既有入口：当前 stub 只有 `iretq`，不存在可传给共同入口的 `pt_regs_t`。新 stub 必须按既有 `INTR_SAVE_ALL` 契约建立完整现场，传 `(regs, 0xff)` 给 `x86_irq_entry`，然后只恢复现场并 `iretq`。

可在 x86 `entry.S` 导出私有 `x86_intr_restore_only` 标签，转入现有 `RESTORE_ALL`，供 spurious stub 的合成返回地址使用；不改变恢复代码、现场偏移或 IRQ/异常既有跳转。不能直接用当前 `DEFINE_INTR_STUB`，因为它固定进入 `ret_from_intr`，会为 spurious 新增 softirq/schedule/signal 返回工作，改变原语义。

保留 IDT 0xff 的 interrupt gate、IST 0。扩展入口必须完整保存/恢复全部 GPR、DS/ES 和 CPU 返回现场，不能只保存 C ABI 的 callee-saved 寄存器。spurious 路径不得执行 LAPIC EOI，也不得读 LAPIC ISR 来“确认”后才返回。本期不重做 PIC IRQ7/15 真伪中断识别。

## 8. 软件返回工作与 syscall 边界

`ret_from_intr` 仍是已有普通 IRQ/异常返回工作入口；softirq 不是新增的 CPU vector，不制造 `INTR_IRQ` 二次事件。`do_softirq()`、调度、信号投递的顺序和 CPL guard 保持；`arch_intr_dispatch()` 返回时不得再调用这些工作，避免一次中断重复执行。正常 IRQ 能打断 ring0/ring3，两种 origin 均为合法。

0x80 `system_call` / `do_system_call`、兼容 syscall 表和 signal frame 不迁移；syscall 是业务 ABI 入口，不能因补齐硬件入口而走本期 IRQ 白名单。其他尚未安装的新 ISA 异常不建 stub、不改 CPU feature enable。

## 9. 验证：从生产对象到真实 guest

### 9.1 host 与目标对象

- 链接生产 decoder/backend/adapters，表驱动覆盖 20 异常 vector、28 IRQ vector、CPL 0/3/UNKNOWN、错误 kind/分类/地址/raw payload、vector 空洞与 0x80。增加 kernel CS+#PF U/S=1 保持 KERNEL、IRQ user CS 合法、#PF 地址0有效与 IRQ 地址0无效的区分。
- 每个新增 sync 后端分支用独立 spy 证明只调用正确私有 handler 一次、参数原样传递；#DB/#BP/#NMI/#DF/#MC 的 fatal 末端只能用 nonreturn 测试逃逸，不能伪装成成功恢复。现有 phase1 #PF/GP 业务不靠 mock 证明完整正确性。
- 生产外设业务与 spy controller/handler 验证正常、无 handler、有/无 controller 和 slave ACK 顺序；timer/IPI 分别验证 EOI 所有者、TLB generation 与 resched 标记。测试 backend 到生产业务，不能只测 spy backend 却声称真实 ACK 已验证。
- 保留 phase1 对象审计，扩展为清单驱动的 20 个异常和 28 个硬件入口接线核对，包含 NMI 的直接 C call/独立恢复、普通 FUNC 槽间接调用、24 Build_IRQ 的 jmp/合成返回、timer 的 call/ret_from_intr、IPI 宏展开与 spurious 的 restore-only。支持实际 x86 `-mcmodel=large` movabs/间接 call/tail jump；未知边、错误目标、遗漏新安装 vector 均失败。
- 真实 ELF/反汇编核对 gate/IST/DPL 与 error-code frame，比较关键 push/pop/槽偏移及返回路径；runtime IDT 安装也需源码和运行期快照/fixture 证明，不能仅凭 symbol 存在宣称 IDT 正确。静态审计不负责解析任意设备函数指针，动态 controller/handler 边由生产 host 测试补齐。
- 禁止用遍历 vector 计数替代逐项调用链；禁止只在测试中直接调用 C adapter 冒充 IDT 硬件入口已验证。spurious 保存/恢复的寄存器 sentinel 与零 EOI 必须有对应验证。

### 9.2 正常 guest 与受控入口探针

复用现有 `test-kernel-selftest` 和协议 v1，新增有明确选择/BEGIN/terminal PASS 的 x86-only 自测 case：

1. **LAPIC timer**：在 BSP/AP 上观察 timer 增量与返回后继续执行；区分 BSP 全局 tick 与 AP 计账，不能通过合成中断反复推进全局时钟造成双计账。保留原真实 timer 驱动来源。
2. **TLB IPI**：两个 online CPU 的受控 shootdown 验证目标 CPU 的失效及 ack generation；必须有真实 remote IPI，不能直接调用 handler 代替。遵守现有 tlb 串行化协议和 ipi_ready gate。
3. **resched IPI**：受控跨核发送并证明接收核 handler 执行与返回后继续工作；need_resched 可能在 ret_from_intr 被消耗，不能事后只查该位等于1。通过自测范围内的完成计数/后续任务进展证明，生产模式不加逐中断日志。
4. **0xff spurious**：受控内核软件 `int $0xff`（该 vector 无硬件 error code）真实进入新 stub，验证返回、可用 GPR/栈/RFLAGS sentinel 和无额外返回工作；它只证明该 IDT 路径，不声称制造了真正的 LAPIC 硬件 spurious。EOI 零次由生产 host 业务和对象审计佐证；若用测试钩子观测，须隔离自测且不计入正常 timer EOI。

外设真实验证包含 phase-0 启动/串口交互、磁盘读写及有 NIC 的 network suite，覆盖实际 IOAPIC 路径。PIC fallback 的端口/ACK 语义由生产 host 测试保证；若 QEMU 配置没有实际走 PIC，报告不得称其已做真 PIC E2E。

### 9.3 致命入口的隔离证据

新增 `test-qemu SUITE=intr-x86`，为本期破坏性异常探针使用独立 `intr-x86-fault` variant，至少包括 **kernel #BP** 和 **NMI** 两次独立单核启动。suite/variant/测试选择变量及冲突 gate 在后续 plan 中按现有框架落定；不得把探针混入正常或普通 selftest 镜像。

#BP 用内核单字节 `int3` 触发原 fatal，在该指令之后紧邻放置明确的续执行标签；runner 要求完整、可匹配的原 `do_int3(3)` 诊断，保存 RIP 必须等于该续执行标签地址（不是 int3 自身地址），且无注入后 returned。地址由对应变体 ELF 符号解析，不硬编码旧构建地址。

NMI 使用 QMP `inject-nmi`，在 guest 输出 armed 后发命令，核验 QMP 成功响应、原 `do_nmi(2)` 诊断及不返回；不以软件 `int $2` 冒充真实 NMI 递送。guest armed 后的等待/心跳路径必须不持普通 logger 或其他会被原 fatal 末端获取的锁，避免测试自身把 NMI 注入 logger 临界区。QMP 响应与串口字节没有统一顺序：允许排空此前已生成的串口数据，不能以宿主收到 QMP 响应的时刻划分心跳归属。判定以单核串口中的完整 NMI 诊断作为 guest 顺序边界，在其后保留有界观察窗口，若出现新的完整心跳/继续执行标记、二次 fatal 或 panic 即失败；注入前的已缓冲心跳不算 NMI 返回。观察窗口下限 1 秒、总截止由 runner 的有限超时控制；正反 fixture 须覆盖跨块诊断、QMP 响应前已到达诊断、注入前缓冲心跳与完整诊断后继续执行。#BP/NMI 两种 case 均在收到完整证据并完成观察后由 runner 主动结束 QEMU；仅超时或静默不算通过。

本期不强制制造真实硬件 #MC 或双重故障：#DF 的带 error-code 入口/IST3、#MC 的无 error-code 入口由生产对象、gate 现场和 host 路由证明；其真实硬件故障可恢复性/重入安全性不在交付保证内。报告明确分列「真实故障」「软件 vector 探针」「host 合成」「目标对象审计」，不能把这些证据混成全部硬件异常都经过 E2E。

### 9.4 必须运行的回归与失败处理

```text
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-harness
make PROFILE=x86_64-clang test-static
QEMU_SMP=1 make PROFILE=x86_64-clang test-qemu SUITE=phase-0
QEMU_SMP=2 make PROFILE=x86_64-clang test-qemu SUITE=phase-0
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall
make PROFILE=x86_64-clang OS01_NETTEST=1 test-qemu SUITE=network
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=4 test-kernel-selftest
make PROFILE=x86_64-clang test-qemu SUITE=intr-x86
make PROFILE=aarch64-clang test-aarch64-audit
make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe
```

`intr-x86` 是本 spec 新增的未来验收入口，目前未实现；其他命令复用现有 target，保留现有 protocol gate 和结果归档。phase1 的 uaccess 真 #PF→longjmp、safe-MSR 和用户信号 case 必须继续通过。`fd_refcount` 跨核 fixture 已有偶发失败，须保留首跑失败并同配置对照；不得为了本期通过删除它、降低断言或把迁移误报直接认定为本期产品回归。UDP/NIC 与其他环境限制如实记录，不以 SKIP 代替本期要求的真实 IRQ 证据。

## 10. 文件责任与完成条件

| 文件/区域 | 责任 |
|---|---|
| `kernel/include/arch/x86_64/intr_vectors.h` | vector 常量、现有覆盖白名单 |
| x86 `intr/intr_decode.c`、`intr_decode.h` | 20 异常分类、新 IRQ 解码 |
| x86 `intr/dispatch.c`、`intr_dispatch.h` | 强 backend、IRQ family 路由、私有业务交接 |
| x86 `intr/trap.c` | 16 个新增薄入口，原业务及 phase1 四路径保真 |
| x86 `intr/irq_hooks.c`、新增 x86 `intr/irq_entry.c` | 外设适配器及迁入架构目录的 generic 适配器，timer/spurious C 入口；保留公开符号，无递归 |
| `kernel/intr/vector_dispatch.c`、新增配对 `kernel/include/intr/vector_dispatch.h` | 仅保留中立注册表与 intr_dispatch_registered_vector 查表业务，不引入 x86 私有依赖，不改 ARM 源清单 |
| x86 `intr/apic/lapic_timer.c`、`lapic.c`、必要时 `ipi.c` | timer C 落点、新 spurious stub；保留 handler EOI |
| x86 `intr/entry.S`、`gate.h` | 只在 spurious 所需处增加 restore-only 接口/声明；原入口保存与 gate 规则不重写 |
| `kernel/selftest/`、host cases/mock、host/kernel Makefile | 生产路由/ACK 测试、跨核真实探针、自测专属观察 |
| `qemutests/intr_dispatch_audit.py` 及 fixture | 扩展接线/现场审计，保持 ARM 和 phase1 hard gate |
| QEMU runners、`mk/components/run.mk`、profile/variant/submake 配置 | 正常/破坏性测试隔离、QMP NMI、归档与严格判定 |
| 文档 | 报告覆盖清单及证据等级；后续 M4/ARM 恢复仍待实施 |

验收闭环需要同时具备：48 个已安装 x86 非 syscall vector 的完整接线；各族独立 ACK/返回语义；真实 timer/IPI/外设与隔离 fatal 证据；双架构回归及诚实记录的基线限制。不能以“IRQ backend 不再 fail-closed”或 host 数量增加代替交付。

实施者可决定私有函数的内部拆分、spy fixture 组织和清单的数据结构；本期覆盖清单、ACK 所有者、spurious restore-only、原信号/fatal 策略和测试证据等级不可自行扩大或缩减。spec 审批后再写逐任务 plan，不在本次开始产品实现。
