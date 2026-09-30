---
title: OS01 aarch64 EL1h 同步异常诊断设计
created: 2026-09-30
updated: 2026-09-30
type: spec
status: draft-for-review
tags: [osdev, aarch64, exception, diagnostics]
related: [os01-roadmap P2, 2026-09-17-aarch64-gic-phase1-design]
---

# OS01 aarch64 EL1h 同步异常诊断设计

## 1. 目标与边界

目标是让 QEMU `virt,gic-version=2` 上的 aarch64 内核在高半区 VBAR_EL1 生效后，遇到 **EL1h（current EL using SP_ELx）同步异常**时输出可定位的故障记录并停止，而不是停在向量表的 `b .`。这给后续页表、uaccess 和统一异常分发工作提供可观察的失败路径。本项交付诊断，不承诺异常恢复。

成功标准：受控的同 EL data abort 在串口输出一次包含异常类别、`ESR_EL1`、`ELR_EL1`、有效的 `FAR_EL1`、`SPSR_EL1` 和 MPIDR 标识的完整记录；该 CPU 不从异常返回。正常 aarch64 SMP/GIC/Timer 路径继续通过现有测试。x86_64 的中断和异常行为不变。

范围仅限 `kernel/arch/aarch64/intr/entry.S` 中 offset `0x200` 的 EL1h sync 槽，以及其直接调用的 aarch64 诊断函数。`head.S` 的 MMU 启动期 `boot_vectors` 保留现状；EL0 sync/IRQ、SP_EL0 sync、FIQ、SError、用户态信号、syscall、通用 `arch_intr_dispatch`、异常恢复和 GICv2 Phase 2 不在本项。后续实现不得把这些未处理入口误标为已支持。

## 2. 已核实的现状与方案选择

- `head.S` 的低地址 `boot_vectors` 在 MMU 切换窗口生效，已有独立 PL011 故障输出；`aarch64_main()` 在 `pl011_init()` 之后把 VBAR_EL1 指向高半区 `exception_vectors`。
- 高半区向量表仅 EL1h IRQ（offset `0x280`）进入 `el1_irq_entry`，它保存 31 个 GPR、`SP_EL0`、`ELR_EL1`、`SPSR_EL1` 到 272 字节 `pt_regs_t`。EL1h sync（offset `0x200`）仍是 `b .`。
- x86_64 与 aarch64 已共享 `kernel/include/arch/early_print.h`、`arch/cpu.h`、`arch/irq.h` 三个 facade。`kernel/core/stack_chk.c::__stack_chk_fail()` 已建立「屏蔽中断 → `arch_early_puts` 无锁输出 → `arch_cpu_halt` 永不返回」的跨架构致命路径模式，带 `noreturn`、`no_stack_protector`、`cold` 属性；本项复用这组接口与约束。aarch64 `arch_early_putc`/`arch_early_puts` 最终走轮询 PL011，无锁、无分配。
- x86_64 的 `kpanic()` 使用全局缓冲、`vsprintf`、`serial_printk` 和调度相关头；x86_64 的 `trap.c` 还承担用户态 fault 恢复及信号路径。这两者不满足 aarch64 当前启动期异常入口的最小依赖约束，因此不直接复用其函数或搬运其寄存器解码。`log_err` 同样走普通格式化路径。PL011 MMIO 地址 `0x09000000` 是现有 QEMU virt 启动契约。

比较过两种落点：

1. **选用：复用跨架构致命路径 facade，aarch64 专用地解码现场。** EL1h sync 专用入口保存现场，读取 syndrome，调用只使用 `arch_early_putc`/`arch_early_puts`、`arch_local_irq_disable`、`arch_cpu_halt` 的 `noreturn` 诊断函数。参照 `__stack_chk_fail` 的属性与调用约束，但不复制串口驱动或引入第二个早期输出接口。它不依赖调度器、GIC、内存分配或普通日志锁，能在 `aarch64_main()` 安装高半区 VBAR 后立即工作。
2. **暂缓：把所有异常统一转发给通用 panic/`arch_intr_dispatch`。** 这需要先定义跨 ISA 的异常类别、恢复语义、嵌套故障和 EL0 语义，会把一个可验证的诊断项扩成 roadmap Spec A 的完整设计。后续可在保留本项输出契约的前提下接入。

## 3. 入口和现场契约

`exception_vectors + 0x200` 只能放一个到 `el1_sync_entry` 的分支，保持 16 槽、每槽 128 字节、表基址 2 KiB 对齐。EL1h IRQ 槽及其保存/恢复/`eret` 路径不得改变。

`el1_sync_entry` 在原异常栈上保存完整 `pt_regs_t`（31 个 GPR、`SP_EL0`、`ELR_EL1`、`SPSR_EL1`），沿用 `kernel/include/arch/aarch64/regs.h` 的偏移和大小，不改变结构体 ABI。进入 C 前读取 `ESR_EL1` 和 `FAR_EL1`；将 `struct pt_regs *`、两个 64 位寄存器值以 AAPCS64 参数传给 `aarch64_el1_sync_fatal(regs, esr, far)`。若寄存器先暂存到通用寄存器，必须在该通用寄存器被覆盖前保存原始值；异常现场中的 x0–x30 必须是异常发生瞬间的值。C 入口返回在设计上不可能；汇编仍须放不可返回的兜底停机循环，避免编译器或 ABI 错误导致 `eret`。

`SPSR_EL1`/`ELR_EL1` 必须在任何可能再次引发异常的 C/串口访问前快照。`ESR_EL1`/`FAR_EL1` 也必须在调用 C 前快照，避免之后的异常覆盖系统寄存器。入口不触碰 TTBR、页表或 GIC，不启用 IRQ。当前内核使用 EL1h 的 SP_ELx；本项不创建新的异常栈，因此要求进入异常时该栈仍可写且有至少 272 字节现场空间及诊断调用所需的 C 栈空间。栈已损坏/未映射导致的递归异常不属于成功诊断保证。

## 4. 诊断与停机契约

`aarch64_el1_sync_fatal` 声明为 `noreturn, no_stack_protector, cold`，与共享的 `__stack_chk_fail` 约束一致。它调用现有 `arch_local_irq_disable()`，再用 aarch64 的 `DAIFSet #0xf` 补全 D/A/F 掩码；输出只使用 `arch_early_putc`/`arch_early_puts` 或本文件内基于它们的无锁、无分配固定宽度十六进制助手；停机使用现有 `arch_cpu_halt()`。不得调用 `log_*`、`printk`、`kpanic`、`malloc`、VFS、调度器、GIC 或会等候锁的路径。屏蔽异常后尝试抢占全局首报标志：只有第一个故障 CPU 打印完整记录；其他 CPU 和同 CPU 的再次故障直接进入停机循环，不争抢串口。首报标志放在启动时清零的 BSS，使用已有 `arch_atomic_cas(&first_fault, 0, 1)`（`kernel/include/arch/atomic.h`），保证 SMP 下只有一个胜者，且不依赖尚未初始化的 per-CPU 状态、不引入新的原子实现。此处共享的是 fatal 输出/停机契约，ESR/MPIDR 的读取和解释仍属于 aarch64 arch 层。

`ec = (esr >> 26) & 0x3f`。当前 MPIDR_EL1 作为硬件 CPU 标识，日志字段名为 `mpidr`，不把它当逻辑 CPU ID，也不访问 TPIDR_EL1。固定输出为一行（CR 可由 PL011 驱动插入）：

```text
[aarch64-sync] FATAL mpidr=0x<16 hex> ec=0x<2 hex> esr=0x<16 hex> elr=0x<16 hex> spsr=0x<16 hex> far=<0x16 hex|n/a>
```

所有十六进制字母小写，固定宽度零填充；一行只输出一次，末尾换行。`FAR_EL1` 只在该异常类别定义了故障地址且有效时输出值：同/低 EL 的 instruction abort（EC `0x20`/`0x21`）、data abort（`0x24`/`0x25`）、PC alignment（`0x22`）、watchpoint（`0x34`/`0x35`）。对 instruction abort、data abort 或 watchpoint，只要该类 syndrome 的 `ESR_EL1.ISS.FnV` 位 10 为 1，就输出 `far=n/a`；PC alignment 不按 FnV 解释。其他类别（例如 BRK）输出 `n/a`，不把未定义/UNKNOWN 的 FAR 伪装成地址。诊断函数输出后永久停机，不能 `eret`、清除页表错误后继续运行或把异常归因给 GIC。

多 CPU 同时出错时允许另一个 CPU 的已在途普通日志与这一行交错，因为抢占首报标志不锁住普通日志；测试在受控单 CPU 故障场景要求该行完整。串口 MMIO 自身不可用、TX FIFO 永不腾空、异常栈不可写，以及在 `pl011_init()`/高半区 VBAR 安装之前的故障，都不属于本项保证；启动早期继续由 `boot_vectors` 负责。

## 5. 验证设计

新增**隔离的** `test-aarch64 MODE=sync-fault`，不能让默认 `MODE=smp` 或普通 selftest 镜像带入故障注入。该 mode 的 prep 子构建传 `KERNEL_SELFTEST=1 AARCH64_SYNC_FAULT_TEST=1`；前者保留现有 PMM/page-table smoke，后者只在 aarch64 profile 选择新的 `KERNEL_VARIANT=sync-fault`（优先于 `KERNEL_SELFTEST` 的 `selftest` 分支），在 `kernel/Makefile` 生成 `-DAARCH64_SYNC_FAULT_TEST=1`。`mk/project.mk` 的 `OS01_SUBMAKE_ALLOWED` 必须加入 `AARCH64_SYNC_FAULT_TEST`，让 `mk/components/image.mk` 的受控 `os01_submake` 把它传入内核子构建；根 make dry-run 与编译参数指纹检查必须能证实该宏实到 kernel 编译命令。独立 variant 产生独立的 kernel/image 路径，不复用普通 `selftest` 对象或 FAT 镜像；正常 mode 不设置该标志。

测试镜像在 `aarch64_main()` 完成 PL011 初始化、高半区 VBAR 安装、`aarch64_pt_smoke_test()` 成功及其 unmap 后，用与该 smoke 相同的有效活动页表根调用 `aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA, ...)`，明确要求 `AARCH64_PT_ENOENT`；若不是 ENOENT，输出 `[aarch64-sync-test] precondition FAIL` 并停机，不能继续注入。随后在 BSP 打印一次 `[aarch64-sync-test] armed`，以不可优化掉的 inline assembly `ldr` 读取 `AARCH64_PT_SELFTEST_VA`，其后保留仅在错误返回时可到达的 `[aarch64-sync-test] returned` 标记。仅从 1 CPU QEMU 运行该破坏性用例。测试目标 `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` 自动构建该隔离镜像、运行 QEMU 并判定结果，无需开发者手工清理普通镜像。

QEMU harness 以超时为失败，并要求：

1. `armed` 恰一次，随后恰一行 `[aarch64-sync] FATAL ...`；行内 `ec=0x25`、`far=0xffff800000000000`，`esr` 的 EC 位与 `ec` 一致，`elr`、`spsr` 均为 16 位十六进制字段。
2. 诊断后没有正常启动完成或 tick 标记，且该 CPU 不返回 faulting `ldr` 后续控制流；故障注入点紧随其后的 `[aarch64-sync-test] returned` 标记必须不存在。
3. 弱证据不得通过：缺失字段、错序、多条 FATAL、`far=n/a`、错误 EC、`precondition FAIL`，或只出现 `armed` 后静默超时均失败。对解析函数增加合成日志正反例；对 FAR 有效性判断的 host 侧测试覆盖 FnV=1 的 instruction abort、data abort、watchpoint 均输出 `n/a`，以及 PC alignment 不受 bit 10 影响。
4. 现有 `make PROFILE=aarch64-clang test-aarch64 MODE=smp` 与 `MODE=gic-spi` 通过，确保 IRQ、GIC、CNTP 和 IPI 不受影响；`make PROFILE=x86_64-clang test-static` 或等价静态边界检查确认 x86_64 构建输入未被故障宏污染。

测试构建所需的 profile 变体选择、Make capability gate 和 `make -n` 行为遵守 `docs/build-system-harness.md`；`sync-fault` 的新增说明同步写入该文档。测试不能把“QEMU 超时”本身当作成功证据，只能在完整诊断行已出现且 harness 主动终止 QEMU 时成功。

## 6. 文件职责与验收

| 文件 | 职责 |
|---|---|
| `kernel/arch/aarch64/intr/entry.S` | EL1h sync 槽、现场快照、调用 fatal 入口；保持 IRQ 路径原样 |
| `kernel/arch/aarch64/intr/trap.c` | aarch64 fatal 诊断、EC/FAR 有效性判断、首报与停机 |
| `kernel/include/arch/early_print.h`、`kernel/include/arch/cpu.h`、`kernel/include/arch/irq.h` | 复用已有 facade；本项不改变其接口或 x86_64 实现 |
| `kernel/include/arch/atomic.h` | 复用已有 `arch_atomic_cas` 实现首报；本项不改变接口 |
| `kernel/include/arch/aarch64/regs.h` | 只在确有需要时补共享常量或偏移断言；不改变 `pt_regs_t` 布局 |
| `kernel/arch/aarch64/boot/main.c` | 隔离测试变体的受控 data abort 注入 |
| `mk/project.mk`、`kernel/Makefile` | 放行测试变量到受控内核子构建并生成对应编译宏 |
| `mk/profiles/aarch64-clang.mk`、`mk/targets/aarch64.mk`、`mk/components/run.mk` | 独立变体和 `MODE=sync-fault` 构建/运行入口 |
| `qemutests/aarch64_uefi_smp.py` 或单独的 `qemutests/aarch64_sync_fault.py` | 复用现有固件/DTB/QEMU 路径，严格解析故障证据 |
| `docs/build-system-harness.md` | 记录新 MODE、隔离与验收命令 |

实施完成应同时满足：测试注入能稳定给出准确诊断；关闭注入后 SMP/GIC/Timer 现有测试通过；正常构建不含故障注入；改动不扩展到 EL0 或通用中断接口。roadmap 的“中断/异常 dispatch 统一”仍为进行中，本项只完成其 aarch64 EL1h 同步异常的诊断基础。
