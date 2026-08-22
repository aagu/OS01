# OS01 全面架构评审计划

> **日期**: 2026-07-25
> **范围**: 全面架构评审 — 问题清单 + 优先级
> **基准**: `0916bf8` (HEAD)

## 评审框架

### 评审维度 (每子系统 5 维)

| # | 维度 | 检查内容 |
|---|------|---------|
| 1 | **接口设计** | 模块边界、公开 API 一致性、头文件暴露控制、依赖方向 |
| 2 | **并发正确性** | 锁粒度/范围、ABBA 死锁、`spin_lock_irqsave` 使用、per-CPU 安全 |
| 3 | **错误处理** | 分配失败路径、syscall 返回值检查、资源泄露 (fd/页/锁) |
| 4 | **架构抽象** | x86_64 特有代码隔离度、`#ifdef` 节制、aarch64 桩完备性 |
| 5 | **风险模式** | 该子系统特有的易错模式是否在代码中复现 |

### 优先级定义

| 级别 | 含义 | 响应 |
|------|------|------|
| **P0** | 潜在 crash / 数据损坏 / 安全漏洞 | 建议立即修复 |
| **P1** | 设计缺陷 / 可维护性瓶颈 / 架构不一致 | 建议本迭代内处理 |
| **P2** | 风格 / 文档 / 可读性 | 建议择机处理 |

### 输出格式

每条问题：

```
[P0/P1/P2] [子系统] 问题简述

- 位置: 文件名:行号
- 现象: 代码片段 + 潜在后果
- 建议: 修复方向 / 重构思路
```

### 最终产物

1. 按顺序逐组检查，每组交付独立问题清单
2. 全部完成后汇总跨模块关联问题分析

## 评审范围与顺序 (10 组)

### Group 1: Boot + 内核入口
- 文件: `kernel/arch/x86_64/head.S`, `entry.S`, `trampoline.S`, `kernel/main.c`, GDT/IDT/TSS
- 关注点: 启动序列正确性、页表初始化、GDT/IDT 布局、TSS IST 栈、bootinfo ABI (LLP64 vs LP64)

### Group 2: 内存管理
- 文件: `kernel/memory/pmm.c`, `slab.c`, `vmm.c`, `vma.c`, `kernel/arch/x86_64/trap.c` (#PF handler), `kernel/include/kernel/vmm.h`
- 关注点: PMM 三区管理、Slab 递归初始化、VMM 页表操作、VMA 查找/拆分、COW 引用计数、4KB subpage pool、TLB shootdown

### Group 3: 中断 + 时钟 + SMP
- 文件: `kernel/apic/*.c`, `kernel/intr/*.c`, `kernel/pic/*.c`, `kernel/time/*.c`, `kernel/sched/smp.c`, `kernel/percpu/*.c`, `kernel/arch/x86_64/trampoline.S`
- 关注点: APIC/IOAPIC/LAPIC 初始化顺序、IRQ 向量分配、PIT/LAPIC timer 共存、IPI 广播、SMP bringup 8 阶段、percpu GS base

### Group 4: 调度
- 文件: `kernel/sched/task.c`, `kernel/sched/schedule.c`, `kernel/sched/eevdf.c`
- 关注点: EEVDF rbtree 操作、vruntime 累积、deadline 选择、task state 机、fork/exec/exit/spawn、blocker framework、idle task

### Group 5: 信号
- 文件: `kernel/sched/signal.c`, `kernel/arch/x86_64/trap.c` (do_signal_delivery), `kernel/arch/x86_64/entry.S` (sigreturn)
- 关注点: 信号投递 CPL 检查、handler 栈布局、sigreturn 恢复、Ctrl-C → SIGINT 路径、signal mask 原子性

### Group 6: 同步原语
- 文件: `kernel/futex.c`, `kernel/mutex.c`, `kernel/completion.c`
- 关注点: futex hash table、mutex 实现、completion 使用场景

### Group 7: 文件系统 + I/O
- 文件: `kernel/fs/*.c`, `kernel/block/*.c`
- 关注点: VFS 层抽象、ext2/FAT32/tmpfs/devfs/procfs 实现、pipe、poll/select、GPT 分区解析、block device 层

### Group 8: 驱动 + TTY
- 文件: `kernel/driver/*.c`, `kernel/tty/*.c`
- 关注点: AHCI SATA、PS/2 键盘、16550 串口、VT100 CSI 终端模拟器、TTY cooked/raw mode、ioctl

### Group 9: 用户态 + libc
- 文件: `user/*.c`, `user/crt0.S`, `user/sigreturn_trampoline.S`, `libc/**/*.c`
- 关注点: syscall wrapper ABI、crt0 初始化、libc (printf/malloc/string/stdio/time/rbtree)、busybox 集成

### Group 10: 构建系统
- 文件: `Makefile`, `kernel/Makefile`, `libc/Makefile`, `user/Makefile`, `test/Makefile`, `tools/mkdisk`, linker script, kallsyms
- 关注点: 头文件依赖缺失 (`make clean` 问题)、多架构构建、交叉编译工具链、测试框架

## 执行方式

每组评审步骤:
1. 用 codegraph 获取该组关键符号的源代码 + 调用关系
2. 按 5 维度逐文件/逐功能检查
3. 产出问题清单
