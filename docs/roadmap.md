# OS01 优化路线图 v28

> **基准**: `09264e4` (worktree-startup-unification, 2026-09-16)
> **日期**: 2026-09-16

标记: ✅ 已完成 | 🔒 P1 安全加固 | 🏗 P2 aarch64 适配 | 🖥 P3 GUI | 🔧 P4 硬件适配 | 📐 P5 ABI 扩展/兼容性

---

## 当前状态总览（9 个 Phase 全部就绪 ✅）

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork、mmap/mprotect/munmap、demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、SMP（percpu+GS-base+AP boot+负载均衡）、canary、debug channels、kallsyms、slab/PMM/softirq/timer SMP 加固、profile 化 GNU Make 构建体系、aarch64 UEFI bootloader 统一、**v25 arch-cleanup**：weak-default + strong-override 模式覆盖 7 子系统（PMM/log/PGD/PUD/PMD/PTE/E820/pt_regs_t/arch_irq/rtc/SUBSYS_INITCALL/kernel_thread_entry/UEFI 残留） | ✅ |
| **Phase 3: 信号 + 调度** | arch 信号帧投递、进程组/会话（setpgid/setsid/getpgid/getsid 67-70）、tty 行规程（VINTR/VQUIT→SIGINT/SIGQUIT）、SYS_kill 支持 pid=0/-pid/-1、per-CPU EEVDF rbtree 可运行队列、SMP 负载均衡 | ✅ |
| **Phase 4: 文件系统** | ext2 R/W、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+行规程 TTY）、52 applet（见 `docs/applet-verification.md`）、init（/etc/inittab 配置解析、4 阶段引导）、libc、VT100 CSI 终端模拟器 | ✅ |
| **Phase 7: poll/select** | poll_table + 双队列级联唤醒、select/pselect、do_poll_core 共享、pselect6 sigmask 原子 swap、requested-event-aware 注册、per-poll timeout registry | ✅ |
| **Phase 8: 网络** | lwIP 2.2.1、E1000 + virtio-net、PCI/MSI-X、DHCP/DNS、TCP/UDP socket、poll/select 集成、BusyBox HTTP wget、自动化网络回归 harness | ✅ |
| **Phase 9: 时间系统** | clocksource + clockevent 双层抽象、TSC 频率校准、LAPIC 周期 tick 接管、CLOCK_MONOTONIC/REALTIME + nanosleep + poll/select 迁纳秒 | ✅ |

> 各 Phase 的实施细节、commit 记录、决策与经验见「相关文档」与 `docs/changelog.md`。

---

## 待实施路线图（按 5 优先级）

> 优先级框架（用户确认，2026-08-18）：**P0 工程基础** → **P1 安全加固** → **P2 aarch64 适配** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### ✅ P0 工程基础（全部完成）

| 项 | 内容 |
|----|------|
| 文档同步、`applet-verification.md` 52/52 验证清单、roadmap 瘦身、GNU Make profile 重构、profile-only UEFI 简化、自托管 compiler runtime、aarch64 UEFI bootloader 统一、aarch64 UEFI 固件修复、目录重构 P1-P6、PMM/sched 稳定性系列、文档学习层 | 全部 ✅（详见 `docs/changelog.md` 2026-08-23 ~ 2026-09-16） |

### 🔒 P1 安全加固

**已完成**：getrandom、x86_64 内核栈保护、统一用户态启动方式、syscall 边界审计、exec 软链接跟随、**用户栈 canary（worktree `feat/user-stack-canary`，commits `00a98e6`..`ae472a6`，8 commits：canary_smash RED → libc SSP GREEN → canary_dump + 43/44 熵守门 → `test-user-canary` 7 步 → SKIP_STRIP busybox 符号守门 → at_random selftest RED → AT_RANDOM auxv + selftest + getauxval → LWIP_RAND 接内核 ChaCha20 + hosttest）**。详见 `docs/changelog.md`。

依赖链：`getrandom ✅ → 统一用户态启动方式 ✅ → 用户栈 canary ✅ + AT_RANDOM ✅ → ASLR`；UBSan/KASan 编译期独立。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 用户栈 canary | ✅ 完成（见上） | | |
| ASLR | mmap 基址随机化 + ET_DYN/PIE 加载随机化 | getrandom ✅ | |
| UBSan + KASan | 内核编译期 instrument | 独立 | ArvernOS |
| 堆加固 | malloc double-free/溢出检测 | 独立 | |
| NX 页 | 栈/堆不可执行 + mmap PROT_EXEC 审计 | 独立 | |

### 🏗 P2 aarch64 适配

**已完成**：PMM arch-neutral、log API 统一、PGD/PUD/PMD/PTE 层级、E820 拆分、pt_regs_t facade、arch_irq hooks、rtc split、SUBSYS_INITCALL、kernel_thread_entry、UEFI 残留排除、aarch64 页表原语。详见 `docs/changelog.md` v25 + `docs/arch.md` weak-default 矩阵。

前置：rwlock/seqlock ✅。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| head.S + MMU | 启动入口 + TTBR0_EL1/页表（原语 ✅，下一步是把原语接到 aarch64 `main.c` 建立内核恒等映射 + user 页表） | 页表原语 ✅ | ArvernOS |
| GICv2 驱动 | 中断控制器 | head.S | opuntiaOS |
| Generic Timer | cntvct_el0 读数 + CNTP 周期定时器 | head.S | opuntiaOS |
| 交叉编译链 | aarch64-linux-gnu-gcc + QEMU virt 平台 | 独立 | |
| UEFI 启动链 | 共享 boot/uefi/main.c，aarch64 通过 PSCI 启动 AP；DTB handoff 副本固定 `[0x401e0000,0x401ff000)` | 独立 | |
| SMP 验证 (UEFI PSCI) | QEMU virt/Cortex-A53/GICv2 下 BSP+AP 独立栈+TPIDR+异常向量+每核 GIC interface；共享 spinlock 计数 1/2/4 核 ×3 验证；故障注入 `AARCH64_SMP_TEST_NO_ACK_CPU=1` 验证降级恢复 | GIC、UEFI 链 | |
| 统一 kernel_main | 单 `kernel_main` 按固定顺序调 `arch_early_init()` / `arch_late_init()` / `scheduler_init()`；aarch64 必须先 dtb 才能用 DTB 信息；init 顺序契约需明确 | PMM ✅, SMP ✅, 调度器 core, arch_irq ✅, rtc ✅, pt_regs_t ✅ | 长项 spec |
| 中断/异常 dispatch 统一 | `arch_intr_dispatch(vector, pt_regs*)` 单入口抽象；x86_64 IDT vs aarch64 VBAR_EL1 vector tables 各自封装；x86_64 IST stack vs aarch64 SP_EL1 切换。**最大代码量减少**：`x86_64/trap.c` 3065 行大部分是 x86 register decode；aarch64 `trap.c` 20 行（占位）。`arch_irq_dispatch` 已落地，剩余是 trap.c 内部 x86 register decode 的 arch 剥离 | arch_irq ✅, head.S ✅ | Linux do_IRQ |
| SMP 启动统一 | `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` 单入口；内部 aarch64 PSCI CPU_ON / x86_64 INIT-SIPI + trampoline 各自实现 | GIC ✅, 启动链 ✅ | opuntiaOS |
| 上下文切换统一 | `arch_task_switch(prev, next)` + `arch_thread_entry()`；aarch64 ret 到 user vs x86_64 sysret/iret | SMP 启动统一 | Tilck |
| 定时器统一 | 把两个 arch 的 timer driver 都注册到 `clockevent` 框架即可；aarch64 `time.c` 133 行 (Generic Timer)，x86_64 `time.c` 28 行 (LAPIC/HPET) | clocksource/clockevent hook ✅ | Linux tick |
| CPU 特性探测 | `arch_cpu_features()` 返回统一位图（has_fpu / has_virt / has_cache_coherency）；x86 CPUID vs aarch64 ID_AA64* 各实现一份 | 独立 | Linux cpufeature |

**距离单一 kernel_main 还差多远（粗估，一个人全职）**：~4–8 周（v25 已落地 arch_irq hooks + pt_regs_t facade + rtc split + kernel_thread_entry + driver initcall + PGD/PUD/PMD/PTE）。详见 `docs/arch.md` 末段 3 个 spec/plan 增量推进。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。靠 arch 抽象层封装，统一接口、不统一实现。

### 🖥 P3 GUI

已有基座：fb ✅、fb mmap ✅、terminal 双缓冲 + alt-screen ✅、键盘扫描码 ✅。Tetris 游戏已落地（见 `docs/gui.md`）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| PS/2 鼠标驱动 | `/dev/mouse`，扩展 keyboard.c 的 PS/2 协议处理 | 独立 | |
| 2D 图形 API | fb 之上画线/矩形/位图 blit | 独立 | |
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API | HackOS |
| Window Server + compositor | 多窗口管理 + 合成 | 字体/2D/鼠标 | opuntiaOS + HackOS |

### 🔧 P4 硬件适配

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| USB 驱动栈 | HID/存储/网络 | 独立 | |
| 真机启动 (USB) | 建立硬件验证路径 | USB 存储 | Tilck |
| NVMe 驱动 | 替代 AHCI | 独立 | |
| HPET clocksource | 真实硬件跨平台时间源 | 独立 | |
| ACPI | 电源管理/关机 | 独立 | |

### 📐 P5 ABI 扩展/兼容性

**已完成**：symlink/readlink、exec symlink ABI。详见 `docs/changelog.md`。

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；`socket ✅ → AF_UNIX`；`mbedTLS ✅ → HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc | ELF ✅ | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构 | |
| clone/pthread | 线程模型 + pthread_create | futex ✅ | |
| readv/writev | scatter-gather I/O | 独立 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立 | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立 | |
| 作业控制（部分完成）| setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅；**剩余**：SIGWINCH 派发、SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs） | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| sysroot 增量重编译 | genid 变化即 `-B` 全量重编；refinement：generation 内 .d 路径相对化/软链引用 | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch | libc | |
| libc 完整性 | printf `%f/%F/%e/%E/%g/%G` + `%ld/%lu` + `%x/%o`、strtod、getopt ✅；**仍缺**：stdio 行读取/seek、getcwd、cut/paste 的 getopt 解析 | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

### 依赖链总览

```
P1: getrandom ✅ → 统一用户态启动方式 ✅ → 用户栈 canary ✅ + AT_RANDOM ✅ → ASLR
P2: PMM ✅ + log ✅ + PGD/PUD/PMD/PTE ✅ + E820 ✅ + pt_regs_t ✅ + arch_irq ✅ + rtc ✅ + SUBSYS_INITCALL ✅ + kernel_thread_entry ✅ + UEFI 残留 ✅ + 页表原语 ✅
   → head.S + MMU → GICv2 → Generic Timer
   统一 kernel_main（interrupt/SMP/context-switch 三独立 spec，arch_irq 已落地收尾）
   rwlock ✅ → aarch64 SMP；timer hook ✅ → CNTP
P3: fb ✅ → 2D API → 字体 → Window Server；PS/2 鼠标并行
P4: USB 栈 → 真机启动；NVMe / HPET / ACPI 独立
P5: ELF ✅ → ld.so → 共享 libc → apk/musl；futex ✅ → clone → pthread
    socket ✅ → AF_UNIX；mbedTLS ✅ → HTTPS
```

### Parked（未闭环 follow-on，随时可拾起；2026-09-13 逐项对照代码核实）

| 项 | 状态 | 说明 |
|----|------|------|
| sysroot 头文件级增量重编 | **仍开放** | `mk/components/kernel.mk` 仍是 genid 变化即 `-B` 全量重编；refinement：generation 内 .d 路径相对化/软链引用，使头文件变化只重编依赖者 |
| `LWIP_RAND`/AT_RANDOM 种子 | **已完成**（2026-09-17，worktree `feat/user-stack-canary`） | `kernel/include/net/arch/cc.h` `LWIP_RAND()` 已替换为 `lwip_getrandom_u32()`（新文件 `kernel/net/lwip_sys_arch.c` 直接走 `get_random_bytes()` ChaCha20）；auxv 构造 `setup_user_stack()` 单站点已压 `AT_RANDOM(16B CSPRNG)` + `AT_PLATFORM("x86_64")`（commits `e35c763` + `ae472a6`） |

> 旧 Parked 项（devfs mount entry / PMM 非-RAM 类型 / `__vfs_lookup_raw` consumed 路径）已分别在 `0809100` / `beb351c` / `b0c95e3` 闭环，从 Parked 表移除。

---

## 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档索引（按 0-4 层组织，按阅读顺序排列） |
| `docs/architecture.md` | 启动链、内存布局、初始化序列（全局骨架图） |
| `docs/boot.md` | UEFI 引导 + `boot_context` v2 ABI + v25 E820 拆分 |
| `docs/arch.md` | arch-neutral facade + per-arch 强覆盖模式（weak default / strong override）+ v25 arch-cleanup 系列 7 子系统矩阵 |
| `docs/memory.md` + `docs/cow-mmap.md` | 物理/虚拟内存管理 + COW fork/mmap + v25 PMM arch-neutral + 页表层级统一 |
| `docs/interrupt.md` | 中断处理（do_IRQ、register_irq、IDT） |
| `docs/smp.md` | SMP 架构 + 负载均衡实施总结 |
| `docs/scheduler.md` + `docs/scheduler-complexity.md` | EEVDF 调度器设计与复杂度评估 |
| `docs/syscall.md` | 71+ syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 |
| `docs/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结 |
| `docs/gui.md` | Tetris 游戏实施总结 + P3 GUI 路线图 |
| `docs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/network.md` + `docs/lwip-debugging-experience.md` | lwIP 网络栈 + 正确性加固 |
| `docs/filesystem.md` | VFS, FAT32, ext2, devfs, procfs, tmpfs, GPT, block device |
| `docs/driver.md` | 驱动子系统（keyboard, serial, ahci, pci, e1000, virtio-net, fb） |
| `docs/subsys.md` | 子系统注册框架（`SUBSYS_INITCALL()` + phase 顺序） |
| `docs/log.md` | 日志级别、DEBUG_CHANNELS、LOG_TARGET、NDEBUG |
| `docs/build.md` | GNU Make profile 化构建体系 + v25 UEFI 残留排除 |
| `docs/build-run-debug.md` | 端到端构建运行调试 |
| `docs/debug.md` | 调试通道 |
| `docs/applet-verification.md` | busybox applet 验证清单 |
| `docs/decisions.md` | 关键设计决策总账 |
| `docs/structure.md` | 目录结构逐目录说明 |
| `docs/references.md` | 开源 OS 项目借鉴表 |
| `docs/changelog.md` | **历史完成记录**（按时间倒序，最新 2026-09-16） |
| `docs/superpowers/specs/` + `docs/superpowers/plans/` | 历史设计 spec + 实施 plan（过程档案） |