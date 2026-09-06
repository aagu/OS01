# OS01 优化路线图 v23

> **基准**: `c64c854` (test: symlink systest cases — syscall matrix + exec via symlink + DT_LNK)
> **日期**: 2026-09-06
> **变更**: 同步 v22 之后的实际进展——**软链接支持落地**（12 commits `43588c8..c64c854`，4 新 syscall `symlink/readlink/lstat/fstatat` + VFS `__vfs_lookup_raw` mid-path 解析 + ext2 fast/long symlink storage + type-aware unlink rollback + libc POSIX wrappers + PF_LINUX_ABI table 4 项修复 + 67 systest case + 6 kernel selftest）；关闭 P1「exec 软链接跟随」+ P5「symlink/readlink」+ P5「exec symlink ABI ✅ 缓解」三项阻塞 roadmap 项。Parked follow-up：`__vfs_lookup_raw` 的 `consumed` 路径需以 mount prefix 播种（潜在 bug，仅在非根 mount + 相对 target 时触发）。

标记: ✅ 已完成 | 🔒 P1 安全加固 | 🏗 P2 aarch64 适配 | 🖥 P3 GUI | 🔧 P4 硬件适配 | 📐 P5 ABI 扩展/兼容性

---

## 当前状态总览（9 个 Phase 全部就绪 ✅）

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork, mmap/mprotect/munmap, demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、x86 平台源隔离、aarch64 dispatch 桩、SMP（percpu+GS-base+AP boot+负载均衡）、canary、hang detector、debug channels、kallsyms、FPU 保存、slab/PMM/softirq/timer SMP 加固、**profile 化 GNU Make 构建体系**（`make PROFILE=<name>` + 自管 sysroot generation + host-tool mkdisk + make help capability 标签）、**profile-only UEFI overlay 简化**（x86 固件 per-profile + UEFI runtime 补丁删除）、**自托管 compiler runtime**（udivti3 + provider-keyed archive + kernel link publication + kernel runtime validation targets）、**aarch64 UEFI bootloader 统一**（x86_64+aarch64 共享 `boot/uefi/main.c` + `boot_context` handoff ABI） | ✅ |
| **Phase 3: 信号 + 调度** | arch 信号帧投递、进程组/会话（setpgid/setsid/getpgid/getsid 67-70）、tty 行规程（VINTR/VQUIT→SIGINT/SIGQUIT）、SYS_kill 支持 pid=0/-pid/-1、per-CPU EEVDF rbtree 可运行队列、SMP 负载均衡 | ✅ |
| **Phase 4: 文件系统** | ext2 R/W、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+行规程 TTY）、52 applet（见 `docs/applet-verification.md`）、init（/etc/inittab 配置解析、4 阶段引导）、libc、VT100 CSI 终端模拟器 | ✅ |
| **Phase 7: poll/select** | poll_table + 双队列级联唤醒、select/pselect、do_poll_core 共享、pselect6 sigmask 原子 swap、requested-event-aware 注册、per-poll timeout registry | ✅ |
| **Phase 8: 网络** | lwIP 2.2.1、E1000 + virtio-net、PCI/MSI-X、DHCP/DNS、TCP/UDP socket、poll/select 集成、BusyBox HTTP wget、自动化网络回归 harness | ✅ |
| **Phase 9: 时间系统** | clocksource + clockevent 双层抽象、TSC 频率校准、LAPIC 周期 tick 接管、CLOCK_MONOTONIC/REALTIME + nanosleep + poll/select 迁纳秒 | ✅ |

> 各 Phase 的实施细节、commit 记录、决策与经验见下方「相关文档」。

---

## 待实施路线图（v22 按 5 优先级）

> 优先级框架（用户确认，2026-08-18）：**P0 工程基础** → **P1 安全加固** → **P2 aarch64 适配** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### P0 工程基础（✅ 已完成）

| 项 | 内容 | 状态 |
|----|------|------|
| 文档同步 | `docs/syscall.md` 71 syscall（0..70）、`docs/timer.md` 新架构、`pit-200hz-handoff.md` 状态 | ✅（ba56d34） |
| applet 验证清单 | 52/52 编译进 busybox；详见 `docs/applet-verification.md` | ✅（2026-08-18） |
| roadmap 瘦身 | 已完成内容迁出到 `docs/` 专题文档（见下） | ✅（2026-08-23） |
| GNU Make profile 重构 | profile 化构建入口（`make PROFILE=<name>`）+ profile-aware Make 接口 + 自管 sysroot generation（原子发布）+ UEFI/image 产物按 profile 隔离 + host-tool mkdisk + capability-aware 入口校验（缺少能力的 profile 在解析期 fail-fast）+ profile-only cleanup contract + make help capability 标签 | ✅（e567394，15 commits `26920ad..e567394`，2026-09-02） |
| profile-only UEFI overlay 简化 | x86 UEFI 固件 per-profile（不再用运行时 overlay patch）+ host test 按 profile 隔离 + 所有组件强制声明 profile + profile-only UEFI cleanup contract | ✅（T1–T5 landed，2026-09-03，T6 docs/wget flag 待办） |
| 自托管 compiler runtime | udivti3 实现 + provider-keyed selfhosted archive + provider 构建不变量硬化 + 内核链接 compiler runtime + kernel link publication 加固 + compiler-rt eligibility 验证 + kernel runtime validation targets + syscall/selftest suite 隔离 + variant link paths + root `make sysroot` 入口 | ✅（多 commits 2026-09-04/05，详见 `runtime/` + `docs/build.md`） |
| aarch64 UEFI bootloader 统一 | x86_64 + aarch64 共享 `boot/uefi/main.c` + arch 分发 + `boot_context` handoff ABI + boot_context 头部偏移断言 | ✅（commits `af166bc`..`06e6127`，merge `06e6127`） |
| aarch64 UEFI 固件修复 | firmware 截断 64MiB 适配 QEMU pflash（`11aa6ed`）+ aarch64 UEFI 默认 URL 下载（`bad8825`）+ aarch64 也显式传 clang+lld 到 posix-uefi（`25872d1`） | ✅（2026-09-03） |

### 🔒 P1 安全加固

依赖链：`getrandom → AT_RANDOM → 用户栈 canary / ASLR`；UBSan/KASan 编译期独立。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| getrandom syscall ✅ | **已完成**（SYS_getrandom=66，ChaCha20 池 + RDRAND/RDSEED 熵源 + 周期 reseed，`/dev/urandom` 同源）。详见 `docs/syscall.md`。`LWIP_RAND`/AT_RANDOM 种子改用仍待做 | 独立 | Linux getrandom(2) |
| 用户栈 canary | libc `-fstack-protector-strong` + ELF 加载器 AT_RANDOM auxv 传种子（原 P1#5） | getrandom | |
| ASLR | mmap 基址随机化 + ET_DYN/PIE 加载随机化（原 P3#12） | getrandom | |
| UBSan + KASan | 内核编译期 instrument（原 P3#13） | 独立 | ArvernOS |
| syscall 边界审计 ✅ | **已完成**（2026-08-24，commits `a1ad1b9`..`80eab1a`，11 commits）。详见下文「Syscall 边界审计实施总结」 | 独立 | |
| 堆加固 | malloc double-free/溢出检测 | 独立 | |
| NX 页 | 栈/堆不可执行 + mmap PROT_EXEC 审计 | 独立 | |
| **exec 软链接跟随** ✅ | **已完成**（2026-09-06，commits `43588c8`..`c64c854`）。`__vfs_lookup_raw` 早返中段/末段 symlink；`vfs_lookup_at` fold walk + splice + restart（≤ MAXSYMLINKS=8）；NOFOLLOW 仅禁止末段（POSIX）；sys_stat/open/chdir/exec 均已改走 `vfs_lookup_at`。spec §5.3 v3/v4/v5 修复全部应用。Parked follow-up：`__vfs_lookup_raw` 的 `consumed` 路径需以 mount prefix 播种（潜在 bug，仅在非根 mount + 相对 target 时触发）。 | 独立 | Linux do_filp_open |

### 🏗 P2 aarch64 适配

前置：**rwlock/seqlock**（多核并发正确性，VFS/`/proc` 多核缩放，SMP 基础）。

已有基座：arch 抽象层 ✅、dispatch 桩 ✅、平台源隔离 ✅、aarch64 spinlock（ldxr/stlxr）✅、clocksource/clockevent 接口 hook ✅

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| rwlock/seqlock | 基础原语 ✅；VFS mount/lookup ✅；`/proc` 读路径未纳入本次范围（原 P1#4 提为前置） | 独立 | |
| head.S + MMU | 启动入口 + TTBR0_EL1/页表 | 独立 | ArvernOS |
| GICv2 驱动 | 中断控制器 | head.S | opuntiaOS |
| Generic Timer | cntvct_el0 读数 + CNTP 周期定时器（clockevent hook 已预留） | head.S | opuntiaOS |
| 交叉编译链 | aarch64-linux-gnu-gcc + QEMU virt 平台 | 独立 | |
| SMP 验证 | percpu/GS 抽象已就绪，AP 启动 ARM 侧 | GIC | |
| 用户态 syscall ABI | `svc #0` 入口 + 参数传递 | 启动 | |

### 🖥 P3 GUI

已有基座：fb ✅、fb mmap ✅、terminal 双缓冲 + alt-screen ✅、键盘扫描码 ✅。Tetris 游戏已落地（见 `docs/gui.md`）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| PS/2 鼠标驱动 | `/dev/mouse`，扩展 keyboard.c 的 PS/2 协议处理 | 独立 | |
| 2D 图形 API | fb 之上画线/矩形/位图 blit | 独立 | |
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API | HackOS |
| Window Server + compositor | 多窗口管理 + 合成（原 P3#15） | 字体/2D/鼠标 | opuntiaOS + HackOS |

### 🔧 P4 硬件适配

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| USB 驱动栈 | HID/存储/网络 | 独立 | |
| 真机启动 (USB) | 建立硬件验证路径（原 P3#14） | USB 存储 | Tilck |
| NVMe 驱动 | 替代 AHCI（原 P3#17） | 独立 | |
| HPET clocksource | 真实硬件跨平台时间源（timer spec 方案 B） | 独立 | |
| ACPI | 电源管理/关机 | 独立 | |

### 📐 P5 ABI 扩展/兼容性

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；`socket ✅ → AF_UNIX`；`mbedTLS ✅ → HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc（原 P2#9） | ELF ✅ | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构 | |
| clone/pthread | 线程模型 + pthread_create | futex ✅ | |
| readv/writev | scatter-gather I/O | 独立 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立 | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立 | |
| 作业控制（部分完成）| ✅ ~~setpgid/setsid/getpgid/getsid（67-70）~~、✅ ~~tcgetpgrp/tcsetpgrp 真实现~~、✅ ~~tty ISIG + VINTR/VQUIT~~、✅ ~~kill 支持 pid=0/-pid/-1~~；**剩余**：SIGWINCH 派发、SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs，需 busybox `CONFIG_ASH_JOB_CONTROL=y` 或自写 shell） | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| symlink/readlink ✅ | **已完成**（2026-09-06，commits `43588c8`..`c64c854`）。VFS 软链接 + ext2 symlink（fast ≤60B inline `i_block[0..59]` / long data block via `i_block[0]`）；4 新 syscall `SYS_symlink/readlink/lstat/fstatat`=71..74；libc `syscall3`/`syscall4`（r10 ABI for fstatat）；`vfs_getdents` `VFS_SYMLINK→DT_LNK`；67 systest + 6 kernel selftest。详见 `docs/superpowers/specs/2026-09-05-symlink-support-design.md` v5 | 独立 | |
| **exec symlink ABI** ✅ | **已完成**（2026-09-06）。busybox symlink 执行路径已可用：`__vfs_lookup_at(LOOKUP_FOLLOW)` 让 execve 跟随末段 symlink。**`b32e1e0` busybox 副本化构建期变通可在下次重构时切回符号链接**——本次未做（不在 T1-T13 范围内）；推荐在下一次 busybox rootfs 重构中验证并切回。 | exec 软链接跟随（P1）✅ | Linux |
| **sysroot 增量重编译** | 2026-09-02 build refactor 取舍：不可变 generation 发布 → 任一 sysroot 内容变化使内核/用户/BusyBox 整体重编译（-B/digest），非只重编依赖者（spec 209 未满足）。GNU Make 重构 T1–T7 已落地 `c1a64c5`/`9efb970` 的 provider-keyed archive + 原子发布，但头文件级增量重编仍未达成。refinement：generation 内 .d 路径相对化/软链引用，使头文件变化只重编依赖者 | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget（原 P2#10） | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC（原 P2#11） | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch（原 P1#7） | libc | |
| **libc 完整性** | ✅ printf `%f/%F/%e/%E/%g/%G` + `%ld/%lu` + `%x/%o`、strtod（小数/指数）、getopt 短选项（sleep/seq/du/cksum/sum 已恢复）；**仍缺**：stdio 行读取/seek（nl/tail/tac/expand 空 + nl user-fault 崩溃）、getcwd（pwd 空）、cut/paste 的 getopt 解析 | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线（原 P3#16） | 动态链接器 | cavOS |

### 依赖链总览

```
P1: getrandom ✅ → AT_RANDOM → canary / ASLR
P2: rwlock → aarch64 SMP；timer hook ✅ → CNTP
P3: fb ✅ → 2D API → 字体 → Window Server；PS/2 鼠标并行
P4: USB 栈 → 真机启动；NVMe / HPET / ACPI 独立
P5: ELF ✅ → ld.so → 共享 libc → apk/musl；futex ✅ → clone → pthread
    socket ✅ → AF_UNIX；mbedTLS ✅ → HTTPS
```

---

## 相关文档（已迁出内容）

| 文档 | 内容 |
|------|------|
| `docs/changelog.md` | 已完成工作按时间汇总（截至 2026-08-18） |
| `docs/decisions.md` | 46 条关键设计决策总账（按主题分区，供交叉引用） |
| `docs/references.md` | 开源 OS 项目借鉴表（已用 / 可拿） |
| `docs/syscall.md` | 71 syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 |
| `docs/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结（commit/验证） |
| `docs/smp.md` | SMP 架构 + 负载均衡实施总结（前置加固 / AP bug 修复 / 变更清单） |
| `docs/gui.md` | Tetris 游戏实施总结 + P3 GUI 路线图 |
| `docs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/network.md` | lwIP 网络栈 + 正确性加固实施总结 |
| `docs/scheduler.md` / `docs/scheduler-complexity.md` | EEVDF 调度器设计与复杂度评估 |
| `docs/applet-verification.md` | busybox applet 验证清单 |
| `docs/build.md` | GNU Make profile 化构建体系（2026-09-02 重构后）：profile 入口 / 能力契约 / 用户入口 / 工具链覆盖变量 / provider-keyed selfhosted archive / runtime validation targets / host-tool mkdisk / make help capability 标签 |
| `docs/boot.md` | x86_64 + aarch64 UEFI bootloader 统一 + `boot_context` handoff ABI + 架构中立生命周期 |
| `docs/architecture.md` / `docs/structure.md` / `docs/driver.md` / `docs/debug.md` / `docs/build-run-debug.md` / `docs/lwip-debugging-experience.md` | 系统整体架构 + 源码目录约定 + 驱动子系统 + 调试通道 + 端到端构建运行调试 + lwIP 调试经验 |
