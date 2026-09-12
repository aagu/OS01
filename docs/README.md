# OS01 文档索引

> 按"层"组织。**首次阅读请按下面的顺序**，每层读完后用对应的验证手段自测。

## 第 0 层 · 全局认知（30 分钟）

| 文档 | 内容 |
|------|------|
| [README.md](../README.md) | 项目是什么、quick start、构建旗标 |
| [structure.md](structure.md) | 目录结构逐目录说明（读代码前先看这个） |
| [roadmap.md](roadmap.md) | 各 phase 做到哪了、已完成/进行中清单 |

## 第 1 层 · 启动与架构骨架（1-2 小时）

推荐顺序：**先纵向走通一条线，再横向展开。**

1. [architecture.md](architecture.md) — 启动链、内存布局、初始化序列（全局骨架图）
2. [boot.md](boot.md) — UEFI 引导细节、`boot_context` v2 ABI（LLP64/LP64 陷阱）
3. [arch.md](arch.md) — arch-neutral facade + per-arch 强覆盖模式（weak default / strong override 是全库最重要的模式）

**自测**：能不看文档说出 `UEFI → BOOTX64.EFI → boot_context @ 0x60000 → kernel.bin @ 0x100000 → head.S → kernel_main` 这条链上每一步发生了什么。

## 第 2 层 · 核心机制（每篇 1-2 小时，按兴趣排序）

| 主题 | 文档 | 核心源文件 |
|------|------|-----------|
| 内存 | [memory.md](memory.md) + [cow-mmap.md](cow-mmap.md)，源码导读见 [vfs-memory-reading-guide.md](vfs-memory-reading-guide.md) | `kernel/memory/`（pmm.c, vmm.c, slab.c, vma.c） |
| 中断 | [interrupt.md](interrupt.md)，源码导读见 [tty-intr-reading-guide.md](tty-intr-reading-guide.md) | `kernel/intr/`（irq.c, dispatch.c, apic/, pic/） |
| SMP | [smp.md](smp.md) | `kernel/arch/x86_64/smp.c`, trampoline.S |
| 调度 | [scheduler.md](scheduler.md) + [scheduler-complexity.md](scheduler-complexity.md)，源码导读见 [scheduler-reading-guide.md](scheduler-reading-guide.md) | `kernel/sched/task.c`（EEVDF） |
| 系统调用 | [syscall.md](syscall.md)，源码导读见 [trap-reading-guide.md](trap-reading-guide.md) | `kernel/include/uapi/syscall.h`, `kernel/arch/x86_64/trap.c` |
| 信号 | [signal.md](signal.md) | trap.c do_signal_delivery, libc sigreturn |
| 定时器 | [timer.md](timer.md) | `kernel/time/`（clocksource, tick, timer） |
| TTY | 源码导读见 [tty-intr-reading-guide.md](tty-intr-reading-guide.md) | `kernel/tty/`（tty.c, canon.c, console.c, pty.c） |

**自测**：任选一条用户态→内核路径（如 `kill()` 触发 SIGINT），能在源码里走完全程。

## 第 3 层 · I/O 与子系统

| 主题 | 文档 |
|------|------|
| 文件系统 | [filesystem.md](filesystem.md)（VFS, FAT32, ext2, devfs, procfs, tmpfs） |
| TTY/PTY/终端 | AGENTS.md "Interactive shell" 一节 + `kernel/tty/`（tty.c, canon.c, console.c, pty.c） |
| 驱动 | [driver.md](driver.md)（keyboard, serial, ahci, pci, e1000, virtio-net, fb） |
| I/O 多路复用 | [io-multiplexing.md](io-multiplexing.md)（poll/select） |
| 网络 | [network.md](network.md) + [lwip-debugging-experience.md](lwip-debugging-experience.md)（lwIP） |
| 子系统注册框架 | [subsys.md](subsys.md)（`SUBSYS_INITCALL()` + phase 顺序） |
| GUI | [gui.md](gui.md) |

## 第 4 层 · 构建 / 调试 / 测试（动手前必读）

| 文档 | 何时读 |
|------|--------|
| [build.md](build.md) + [build/toolchain.md](build/toolchain.md) | 改 Makefile / 加 profile 前 |
| [build-run-debug.md](build-run-debug.md) | 第一次跑 QEMU / GDB |
| [debug.md](debug.md) + [log.md](log.md) | 加打印、开 DEBUG_CHANNELS |
| [debugging/](debugging/) | 最近的真实排障记录（读别人的排障过程是最好的学习材料） |
| [applet-verification.md](applet-verification.md) | 改 busybox 集成前 |

## 横向材料

- [decisions.md](decisions.md) — 架构决策记录（为什么这么做）
- [changelog.md](changelog.md) — 版本历史
- [review/](review/) — 2026-07 架构评审（10 组问题清单，部分已修，读思路不看状态）
- [superpowers/](superpowers/) — 历史设计 spec + 实施 plan（**过程档案，路径引用以当前代码为准**）
- [references.md](references.md) — 外部参考

## AI 编码时代的阅读纪律

1. **specs/plans 是"当时想怎么做"，代码是"实际怎么做"**——两者冲突时以代码为准，并回写文档
2. 每个子系统入口：`kernel/<subsys>/` 源码 + `kernel/include/<subsys>/` 公开头 + docs 对应 md，三处对称（这是 P4 重构后的硬约定，见 AGENTS.md）
3. 评审报告里标"待处理"的问题，读代码时留意，别把已知坑当新发现
