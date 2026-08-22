# OS01 优化路线图 v19

> **基准**: `16a78c3` (docs(roadmap): v18 — timer 重构完成)
> **日期**: 2026-08-18

标记: ✅ 已完成 | 🔒 P1 安全加固 | 🏗 P2 aarch64 适配 | 🖥 P3 GUI | 🔧 P4 硬件适配 | 📐 P5 ABI 扩展/兼容性

---

## 当前状态总览 (9 个 Phase 全部就绪)

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork, mmap/mprotect/munmap, demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、x86 平台源隔离、aarch64 dispatch 桩、SMP（percpu+GS-base+AP boot+负载均衡）、canary、hang detector、debug channels、kallsyms、FPU 保存、slab/PMM/softirq/timer SMP 加固 | ✅ |
| **Phase 3: 信号 + 调度** | arch 信号帧投递、Ctrl-C→SIGINT、per-CPU EEVDF rbtree 可运行队列、SMP 负载均衡 | ✅ |
| **Phase 4: 文件系统** | ext2 R/W、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+raw mode TTY）、9 applet、init（/etc/inittab 配置解析、4 阶段引导：SYSINIT→WAIT→ONCE→RESPAWN/ASKFIRST、fallback 硬编码默认）、libc (printf/malloc/string/syscall wrapper)、VT100 CSI 终端模拟器 | ✅ |
| **Phase 7: poll/select** | poll_table + 双队列级联唤醒、select/pselect 系统调用（SYS_select=50 + SYS_pselect6=51）、do_poll_core 共享轮询循环、pselect6 sigmask 原子 swap、requested-event-aware 注册（按请求方向唤醒，修复复合 flags + PTY 双注册容量）、per-poll timeout registry（修复 lost-wakeup + 并发 clobber）、systest 142/142 | ✅ |
| **Phase 8: 网络** | lwIP 2.2.1、E1000 + virtio-net、PCI/MSI-X、DHCP/DNS、TCP/UDP socket syscall（52–64）、poll/select 集成、BusyBox HTTP wget、E1000 RX ring 所有权串行化、DHCP ACD 关闭（确定性绑定）、自动化网络回归 harness（make test-network） | ✅ |
| **Phase 9: 时间系统** | clocksource + clockevent 双层抽象（kernel/time/）、TSC 频率校准（CPUID15h + RTC PIE 联合）、LAPIC 周期 tick 接管（掩 PIT + fallback）、CLOCK_MONOTONIC/REALTIME + nanosleep + poll/select 迁纳秒、内核 jiffies 频率 self-test、systest 150/150 | ✅ |

---

## 待实施路线图（v19 按 5 优先级）

> 优先级框架（用户确认，2026-08-18）：**P0 工程基础** → **P1 安全加固** → **P2 aarch64 适配** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### P0 工程基础（主线前铺垫）

| 项 | 内容 | 理由 |
|----|------|------|
| 文档同步 | `docs/syscall.md` 51→66、`docs/timer.md` 新架构、`pit-200hz-handoff.md` 状态 ✅（已提交 ba56d34） | 技术债清零 |
| applet 验证清单 | 52/52 编译进 busybox；28 个完整工作，15 个受 libc 缺口影响，3 个未编译，4 处 user-fault 崩溃。详见 `docs/applet-verification.md` ✅（已提交，2026-08-18） | 摸清家底 ✅ |

### 🔒 P1 安全加固

依赖链：`getrandom → AT_RANDOM → 用户栈 canary / ASLR`；UBSan/KASan 编译期独立。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| getrandom syscall | 内核安全随机数（熵池：TSC + RTC + 周期熵累积），LWIP_RAND/种子改用 | 独立 | Linux getrandom(2) |
| 用户栈 canary | libc `-fstack-protector-strong` + ELF 加载器 AT_RANDOM auxv 传种子（原 P1#5） | getrandom | |
| ASLR | mmap 基址随机化 + ET_DYN/PIE 加载随机化（原 P3#12） | getrandom | |
| UBSan + KASan | 内核编译期 instrument（原 P3#13） | 独立 | ArvernOS |
| syscall 边界审计 | copy_from_user 全路径核查 + TOCTOU（现仅 read/exec 防） | 独立 | |
| 堆加固 | malloc double-free/溢出检测 | 独立 | |
| NX 页 | 栈/堆不可执行 + mmap PROT_EXEC 审计 | 独立 | |

### 🏗 P2 aarch64 适配

前置：**rwlock/seqlock**（多核并发正确性，VFS/`/proc` 多核缩放，SMP 基础）。

已有基座：arch 抽象层 ✅、dispatch 桩 ✅、平台源隔离 ✅、aarch64 spinlock（ldxr/stlxr）✅、clocksource/clockevent 接口 hook ✅

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| rwlock/seqlock | VFS lookup + `/proc` read 多核缩放（原 P1#4 提为前置） | 独立 | |
| head.S + MMU | 启动入口 + TTBR0_EL1/页表 | 独立 | ArvernOS |
| GICv2 驱动 | 中断控制器 | head.S | opuntiaOS |
| Generic Timer | cntvct_el0 读数 + CNTP 周期定时器（clockevent hook 已预留） | head.S | opuntiaOS |
| 交叉编译链 | aarch64-linux-gnu-gcc + QEMU virt 平台 | 独立 | |
| SMP 验证 | percpu/GS 抽象已就绪，AP 启动 ARM 侧 | GIC | |
| 用户态 syscall ABI | `svc #0` 入口 + 参数传递 | 启动 | |

### 🖥 P3 GUI

已有基座：fb ✅、fb mmap ✅、terminal 双缓冲 + alt-screen ✅、键盘扫描码 ✅

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
| 作业控制 | setpgid/setsid/tcgetpgrp 真实现 + tty ISIG + SIGWINCH | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| symlink/readlink | VFS 软链接 + ext2 symlink（in-inode 快链接）（原 P1#6） | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget（原 P2#10） | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC（原 P2#11） | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch（原 P1#7） | libc | |
| **libc 完整性** | printf 补 `%f/%ld/%lu`、strtod、getopt、stdio 行读取；验收基线 `docs/applet-verification.md`（28/52 完整工作） | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线（原 P3#16） | 动态链接器 | cavOS |

### 依赖链总览

```
P1: getrandom → AT_RANDOM → canary / ASLR
P2: rwlock → aarch64 SMP；timer hook ✅ → CNTP
P3: fb ✅ → 2D API → 字体 → Window Server；PS/2 鼠标并行
P4: USB 栈 → 真机启动；NVMe / HPET / ACPI 独立
P5: ELF ✅ → ld.so → 共享 libc → apk/musl；futex ✅ → clone → pthread
    socket ✅ → AF_UNIX；mbedTLS ✅ → HTTPS
```

---

## Tetris 游戏实施路线图 ✅ P0（已完成）

> 状态：**已完成**（2026-08-16）。提交 `79f1179`（framebuffer + alt-screen 协议）、`d8e5c05`（UX：慢速重力 + 种子 RNG + 消行闪烁）、`ae0cc04`（消行白残留修复）。QEMU 手工 `exec /bin/tetris` 可玩，退出终端内容恢复。

### 目标

OS01 上可玩的俄罗斯方块：用户态 `user/tetris.c`，**手工启动**（不进 inittab），fb 像素渲染 + `/dev/keyboard` 原始扫描码输入，退出后终端内容自动恢复（Linux alt screen 协议）。

**明确不做**：serial 渲染后端（38400 baud 下增量重绘虽可行，但优先级低，已讨论砍掉，收敛范围）。

### 架构决策（已确认）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 31 | tetris 输入 | `/dev/keyboard` 原始扫描码（E0 48/4B/4D/50 方向键） | tty TCSETS 是 no-op，raw 模式改造不值；扫描码自解析 E0 前缀 + release 位 |
| 32 | tetris 渲染 | fb mmap 像素块（20×10 网格 × 30px = 600×300） | fb_mmap 是 per-VMA，多进程共享；FBIOSURRENDER 只停内核 console 不排他 |
| 33 | 终端恢复协议 | Linux alt screen：`\e[?1049h` / `\e[?1049l` | 标准化（vim/less 可复用）；terminal.elf 零轮询零 waitpid |
| 34 | terminal.elf 双缓冲 | offscreen 主缓冲 + alt 缓冲 | 退出后内容 100% 一致、恢复零重绘成本 |
| 35 | 游戏生命周期 | 手工 `exec /bin/tetris` | 不进 inittab |
| 36 | serial 后端 | ❌ 不并入 | 增量 ANSI 虽可行（38400 baud ≈ 38 帧/s 上限），但串口只读单字节无 poll，投入产出比低 |

### 关键现状事实（已核实）

- `/dev/keyboard` ring 存**含 0xE0 前缀的原始 PS/2 流**（release 位 0x80 也入 ring）；`keyboard_devfs_read` **非阻塞**、`keyboard_ops` **无 .poll** → devfs_poll 无回调时默认 always-ready（poll 空转）
- `devfs_ops` 含 `.poll` 槽位；poll 基建（poll_table/poll_wait/fd_poll）完备
- `tty_read` 四阶段阻塞协议（drain → 非空返回 → 挂 read_wait 双检 → schedule）是 keyboard 改造的现成范本
- tty termios **假**：TCGETS 硬编码谎报 `ICANON|ECHO|ISIG`，TCSETS **no-op**；实际 raw 搬运（行编辑靠用户态 busybox ash FEATURE_EDITING）
- PTY termios **真存储但语义未实现**（pipe 读路径不看 c_lflag）——本次范围外
- fb_mmap per-VMA、FBIOSURRENDER 非排他；terminal.elf 无离屏缓冲、VT100 无 `?1049h/l`
- 方向键扫描码（Set 1 + E0）：UP=E0 48、LEFT=E0 4B、RIGHT=E0 4D、DOWN=E0 50
- `Makefile` 无头文件依赖 → **改 tty_t 结构体必须 `make clean`**（AGENTS.md）

### 分步实现

| Step | Commit 主题 | 文件 | 内容 | 验证 |
|------|------------|------|------|------|
| **1** | `feat(driver): keyboard poll support`（~31 行） | `kernel/driver/keyboard.c` + `kernel/kernel/main.c` | scancode wait queue（spinlock+list）；`keyboard_handler` push ring 后 wake；`keyboard_poll_dev()`：ring 非空→POLLIN，否则 poll_wait；`keyboard_ops.poll` 挂上（main.c 1 行） | `make clean && make`；QEMU 内测试程序 `poll(/dev/keyboard)` 阻塞等键立即返回 |
| **2** | `fix(tty): make termios honest`（~60 行） | `kernel/include/kernel/tty.h` + `kernel/tty/tty.c` | tty_t 加 `struct termios term`；默认 raw（`c_lflag=0, ICRNL, OPOST\|ONLCR, VMIN=1`）；TCGETS 返回真值 / TCSETS 真存储；tty_read 尊重 ICANON（攒行等 `\n`）+ ECHO 回显。**不做**：ISIG/pgrp（TODO）、OPOST 输出转换（无消费者） | `make clean && make`（结构体变更！）；QEMU 回归 terminal/ash；小程序 TCSETS 切换 raw/canonical 验证行为差异 |
| **3** | `feat(terminal): alt screen double buffer`（~80 行） | `user/terminal.c` | offscreen 主缓冲 + alt 缓冲；put_glyph 写当前缓冲；CSI 解析器加 `?1049h`（保存+清屏+切 alt）/ `?1049l`（切回主缓冲全量重绘） | 测试程序发 `\e[?1049h` 画图 → `\e[?1049l`，原终端内容完整恢复 |
| **4** | `feat(applets): tetris game`（~400 行） | `user/tetris.c`（新）+ `Makefile` | 游戏逻辑（7 Tetromino、4 旋转、碰撞、消行、计分、等级加速）；输入解析（E0 前缀 + release 位 → 归一化 K_LEFT/RIGHT/DOWN/UP/ROTATE/DROP）；渲染 fb 像素块 + **20×10 逻辑屏脏矩形 diff**（防闪烁）；主循环 `poll(/dev/keyboard, 500ms 超时=下落 tick)`；`\e[?1049h` 进入 / `\e[?1049l` 退出 | QEMU 手工 `exec /bin/tetris` 可玩 |
| **5** | 集成验证（无代码） | — | 全量 `make clean && make`；启动 → terminal/ash 正常 → 玩 tetris → 退出终端恢复；systest 回归 | QEMU 实跑 |

### 实施顺序与依赖

```
Step 1 (keyboard poll) ──┐
                         ├──→ Step 4 (tetris 游戏，依赖 1 的 poll + 3 的 alt screen)
Step 2 (tty termios)  ──┤    Step 4 内部：游戏逻辑可先于渲染写（无依赖）
                         │
Step 3 (terminal 双缓冲) ─┘

Step 5 (集成验证) ──→ 所有 Step 完成后
```

- Step 1/2/3 **互不依赖**，可并行推进；Step 4 依赖 1+3
- Step 1+2 为**同一批内核修改**，但**两个独立 commit**（主题不同：driver vs tty）
- 每步独立验证，**通了再报进展**（QEMU 实证，非静态分析）

### 风险 / 注意

| 风险 | 缓解 |
|------|------|
| tty_t 结构体变更（sizeof 变化） | Step 2 必须 `make clean && make`，旧 .o 会静默崩 |
| keyboard ring 满丢键（RING 256，连按风险） | 一般够用；游戏输入事件率低；如丢键再加 ring 大小 |
| poll 唤醒丢失 | 参照 tty_wake_waiters 双检模式 + `this_cpu()->need_resched=1` |
| 主循环被信号中断 | poll/nanosleep EINTR 处理（`errno==EINTR → continue`） |
| 脏矩形 diff 复杂度 | V1 每 tick 全量 diff（20×10 数组比较，~200 次 memcmp，开销可忽略） |

### 完成后（可选项，不阻塞）

- tty ISIG/pgrp 真实现（Step 2 留的 TODO）→ 内核层 Ctrl-C 信号
- serial 后端 `tetris -serial`（增量 ANSI，QEMU -nographic 直玩）

---

## nanosleep 修复路线图 ✅ P0（已完成）

> 状态：**已完成**（2026-08-17）。commit `2faccbc` — 唤醒走 blocker（`wakeup_jiffies` + `BLOCKER_NANOSLEEP`）+ 掩码感知信号唤醒 + `-EINTR`/rem + `CLOCK_MONOTONIC`。systest 150/150。
> 连带发现的 PIT 200Hz（jiffies 2x）为 QEMU TCG artifact——✅ **已由 timer 重构根治**（08-18，LAPIC tick 接管，见「Timer 重构实施总结」），不再阻塞任何后续工作。证据链归档 `docs/pit-200hz-analysis.md`。

### 背景（tetris 开发中实证发现）

`SYS_nanosleep`（kernel/arch/x86_64/trap.c:1862）睡眠实现无唤醒源：

```c
uint64_t target = jiffies + ticks;
while (jiffies < target) {
    current->state = TASK_INTERRUPTIBLE;   // 标记睡眠
    schedule();                              // dequeue 后不重新入队
    ...
}
```

schedule()（kernel/sched/task.c:541-547）对 INTERRUPTIBLE 任务 dequeue 后**不重新入队**；
nanosleep **没有注册任何唤醒机制**（无 wait queue、无 timer 回调、无 wake 路径），
PIT tick 不唤醒普通睡眠任务 → 无 signal 任务**永久睡死**。

QEMU 实证：
- tetris game-over 后 `nanosleep(1s)` 永久卡死（无 `\e[?1049l`、init 不 respawn）
- busybox `sleep 1` **0.05s 瞬间返回**（pending signal 假醒，根本没睡）
- 与 poll bug 同源（1ef8e1f 已修）：睡眠/超时机制缺唤醒注册；nanosleep 比 poll 更严重（连 wq 都没有）

### 修复方案

| 方案 | 改动 | 代价 |
|------|------|------|
| A 最小可用 | nanosleep 循环**不置 INTERRUPTIBLE**（保持 RUNNING）→ schedule 轮询，每 tick 调度回来检查 jiffies | 10ms 粒度空转；信号打断靠循环检查 |
| B 事件驱动（推荐） | task 加 `wakeup_jiffies` 字段 + PIT tick 扫描唤醒到期的 INTERRUPTIBLE 任务（复用 poll timeout registry 思路，~30 行） | 干净、真睡眠；需处理多核并发唤醒 |

### 分步实现（TDD 门禁）

1. **Step 1 复现测试**（RED）：systest 加 nanosleep case——`nanosleep(100ms)` 断言实际睡眠时长 ≥80ms。当前实现：无 signal 时睡死（systest 超时失败）；有 signal 时假醒（时长断言失败）
2. **Step 2 GREEN**：方案 B 优先（wakeup_jiffies + PIT 扫描）；方案 A 作 fallback
3. **Step 3 回归**：systest 全量 + host 全绿 + QEMU 实证——`sleep 1` 真睡 ~1s（非 0.05s）；tetris game-over 后正常退出恢复终端
4. **Step 4 commit**：`fix(sched): nanosleep with real wakeup`（独立 commit）

### 连带问题（待查，不阻塞）

- **signal 假醒**：busybox `sleep 1` 0.05s 返回——pending signal 来源待确认（疑似 fork 复制父进程 pending SIGCHLD / exec 未清 signal）。修复后 sleep 应真睡 1 秒，假醒自然消除

---

## Phase 解析

### 已完成的 P0 基座

#### 1. EEVDF 公平调度器 [借鉴 Tilck]

**状态:** 已完成。调度器使用 per-CPU rbtree 可运行队列，按 eligibility 和 virtual deadline 选择任务，并配套 SMP 负载均衡。

| 任务 | 工作量 | 说明 |
|------|--------|------|
| rbtree 可运行队列 | ✅ | 替代 O(n) 链表扫描 |
| vruntime 累积 + min_vruntime | ✅ | per-CPU 时间线 |
| eligibility + deadline 选择 | ✅ | `pick_eevdf()` O(log n) |
| **收益:** | | 反饿死 + 响应时间可预测 + 高负载下不减速 |

#### 2. lwIP 网络栈 + E1000 驱动 [借鉴 cavOS]

**状态:** 已于 `7416f40` 合并到主线。具备 lwIP 2.2.1、E1000/virtio-net、DHCP/DNS、TCP/UDP socket、poll/select 网络就绪通知和 HTTP wget。

| 任务 | 工作量 | 借鉴 |
|------|--------|------|
| lwIP 移植到内核 | ✅ | tcpip_thread + OS01 sys_arch |
| E1000/virtio-net 驱动 | ✅ | PCI、MSI-X、IRQ RX 缓冲 |
| socket syscall 层 | ✅ | TCP/UDP、DNS、shutdown、部分读缓存 |
| poll/select 网络集成 | ✅ | `FD_SOCKET` readiness 回调 |
| HTTPS/TLS | ⏳ | mbedTLS 已引入，尚未集成到 BusyBox wget |

**收益:** 网络栈是从"玩具 OS"到"可用 OS"的最大一步。

下一步重点不是继续堆 socket API，而是把 DNS、TCP、UDP、DHCP 和 wget 的成功路径纳入稳定的 QEMU 自动化回归。

---

> 📌 **历史计划（v18 及之前结构）**：以下 P1/P2/P3 为旧优先级结构，已由 **v19 的 5 优先级框架**（P0 工程基础 → P1 安全加固 → P2 aarch64 → P3 GUI → P4 硬件 → P5 ABI）取代。内容保留作参考，实施项已映射进新结构。

### P1: 近期

#### 3. 多架构 aarch64

**状态:** 基础清理完成。ARCH 参数化、dispatch 头文件、task arch hook、aarch64 原子量/spinlock 桩和 x86 平台源构建隔离均已到位；aarch64 尚不能启动。

| 任务 | 借鉴 | 说明 |
|------|------|------|
| `arch/aarch64/spinlock.h` 实现 | ✅ | ldxr/stlxr 独占循环 |
| `arch/aarch64/linkage.h` | — | 随启动汇编补 ENTRY 宏 |
| `kernel/arch/aarch64/head.S` | ArvernOS | 启动入口 |
| 中断控制器 (GIC) | opuntiaOS | GICv2/v3 驱动 |
| 时钟/定时器 | opuntiaOS | Generic Timer |
| MMU 初始化 | ArvernOS | TTBR0_EL1 设置 |

#### 4. /proc 完善

| 任务 | 工作量 | 说明 |
|------|--------|------|
| `/proc/<pid>/fd/` | 半天 | 查看打开的文件描述符 |
| `/proc/<pid>/maps` | ✅ | 已实现并有 systest 覆盖 |
| `/proc/<pid>/status` 扩展 | 半天 | signal mask、ppid、utime/stime |

#### 5. rwlock / seqlock

| 任务 | 工作量 | 说明 |
|------|--------|------|
| rwlock 实现 | 半天 | 读共享/写独占 |
| VFS lookup 路径加锁 | 半天 | `/proc` 多核缩放 |

#### 6. 用户栈 canary

| 任务 | 工作量 | 说明 |
|------|--------|------|
| libc 编译 `-fstack-protector-strong` | 半天 | 用户态 SSP |
| ELF 加载器传递 canary 种子 | 半天 | AT_RANDOM aux vector |

---

### P2: 中期

#### 7. 更多 busybox applet

> poll/select ✅ → 阻塞条件已满足。

| 任务 | 阻塞项 | 说明 |
|------|--------|------|
| grep/sed | regex | 从 musl 移植 regcomp/regexec |
| find | glob/fnmatch | libc 新增 `glob()`/`fnmatch()` |
| awk | regex + FPU | regex + 浮点数解析 |
| vi | TTY termios + SIGWINCH | 完整终端控制 |

#### 8. aarch64 启动

见 P1 #3 的剩余工作。

#### 9. readlink/symlink

| 任务 | 说明 |
|------|------|
| readlink syscall/兼容层 | 当前仅有兼容 stub，补齐真实 VFS 语义 |
| VFS 软连接支持 | 跨文件系统路径解析 |
| ext2 symlink inode | 快速符号链接 (in-inode) |

#### 10. 动态链接器

| 任务 | 借鉴 | 说明 |
|------|------|------|
| ELF64 .interp 解析 | cavOS | 识别 PT_INTERP |
| ld.so 加载 | cavOS | 内核启动动态链接器 |

---

### P3: 远期

| # | 任务 | 借鉴 | 说明 |
|---|------|------|------|
| 12 | ASLR | — | 随机化加载基址 |
| 13 | UBSan + KASan | ArvernOS | 内核地址消毒 |
| 14 | 真机 USB 启动 | Tilck | 建立真实硬件验证路径 |
| 15 | GUI 框架 | opuntiaOS + HackOS | Window Server + Compositor |
| 16 | Alpine apk 用户态 | cavOS | musl 二进制包 |
| 17 | NVMe 驱动 | — | 替代 AHCI |

---

## select/pselect 实施总结

### 架构

```
              libc
              ┌──────────────────────────────┐
              │ select(nfds, r, w, e, tv)     │
              │ pselect(nfds, r, w, e, ts, m) │
              └──────────────────────────────┘
                       │ SYS_select=50 / SYS_pselect6=51
                       ▼
              kernel/fs/select.c
              ┌─────────────────────────────────────────┐
              │ do_select(nfds, r, w, e, tv)            │
              │   → fd_set → pollfd[nfds]               │
              │   → do_poll_core(pollfd, nfds, ms, &pt) │
              │   → revents → fd_set                    │
              │                                         │
              │ do_pselect6(nfds, r, w, e, ts, packed)  │
              │   → 解包 sigmask + swap + restore        │
              │   → 同上                                 │
              └─────────────────────────────────────────┘
                       │
                       ▼
              kernel/fs/poll.c
              ┌─────────────────────────────────────────┐
              │          do_poll_core() (共享)           │
              │ do_poll() = copy + do_poll_core + copy  │
              └─────────────────────────────────────────┘
```

- **poll_table_t 动态化**: `entries[16]` → `*entries + max_entries`，支持 select 的 1024 fd
- **do_poll_core 共享**: poll 和 select/pselect 共享轮询循环，接收内核侧 pollfd[]，不碰用户内存
- **do_select_common 去重**: select/pselect 共享 fd_set↔pollfd 转换 + 反向映射 + 写回逻辑
- **pselect6 sigmask 原子性**: goto-out 模式，所有错误路径恢复 blocked
- **fd_set 128 字节**: `kernel_fd_set { uint64_t __bits[16] }` 匹配 libc `long __fds_bits[16]`

### 文件变更

| 类别 | 文件 | 行数 | 说明 |
|------|------|------|------|
| **修改** | `kernel/include/kernel/poll.h` | +24/−8 | poll_table_t 动态化 + do_poll_core 声明 |
| **修改** | `kernel/fs/poll.c` | +60/−25 | 提取 do_poll_core() + 包装器重构 |
| **新增** | `kernel/include/kernel/select.h` | 73 | kernel_fd_set、sigset_t、pselect6_sigmask、原型 |
| **新增** | `kernel/fs/select.c` | 423 | do_select、do_pselect6、do_select_common、do_select_nofds |
| **修改** | `kernel/arch/x86_64/trap.c` | +15 | SYS_select + SYS_pselect6 dispatch + 移除 sigset_t |
| **修改** | `kernel/include/uapi/syscall.h` | +1 | SYS_pselect6=51 |
| **新增** | `test/include/kernel/select.h` | 73 | 镜像 |
| **修改** | `test/include/kernel/poll.h` | 镜像 | 同步动态化 |
| **新增** | `libc/include/sys/select.h` | 59 | FD_ZERO/SET/CLR/ISSET + select/pselect 声明 |
| **新增** | `libc/unistd/select.c` | 52 | libc wrapper + pselect6 打包 |
| **修改** | `libc/include/sys/syscall.h` | +1 | SYS_pselect6 |
| **修改** | `user/systest.c` | +258 | 10 测试组 (20 个断言) |
| **修改** | `tests/run_test.py` | +3 | QEMU AHCI 驱动参数 |

**总计: 11 commits, 14 files, +1042 / −33**

### systest 结果: 126/126 passed (poll 8/8 + select 20/20)

---

## SMP 负载均衡实施总结

### 架构

```
fork / spawn / kthread
  │
  ├── sched_pick_cpu() ──→ 选择 nr_running 最小的 CPU
  ├── spin_lock_irqsave(&percpu_data[tsk->cpu].rq_lock)
  ├── enqueue_task(tsk, ...)
  ├── spin_unlock_irqrestore(...)
  └── sched_notify_remote(tsk)   // IPI + need_resched

CPU N: schedule()
  │
  ├── update_curr / dequeue / zombie reap  (不变)
  ├── sched_balance(rq)                    ← NEW
  │     ├── 找到最忙 CPU（max nr_running, tiebreak min_vruntime）
  │     ├── 门控: idle 无条件 或 src.nr > local.nr + 1
  │     ├── count = max(1, (src - local) / 2)
  │     ├── 双锁 rq_locks（地址排序，单次 IRQ save）
  │     ├── rbtree 尾部窃取（最大 deadline → 最近不会运行）
  │     └── vruntime 规范化到目标 CPU 时间线
  ├── pick_eevdf(rq)                       (不变)
  └── switch_to                             (不变)
```

- **负载指标**: `nr_running` 为主，`min_vruntime` 为 tiebreaker
- **窃取方向**: rbtree 尾部（最大 deadline）— 这些任务刚用完时间片，源 CPU 近期不会调度
- **振荡防护**: 门控 `src > local + 1` ≥ 2 任务差距才迁移；count = `(diff)/2` 收敛而非过冲
- **Vruntime 规范化**: `t->vruntime = max(t->vruntime, rq->min_vruntime)` — 上限，非下限
- **Init 保护**: `sched_balance` 跳过 `pid == user_init_pid` 的任务，确保关机路径完整

### SMP 前置条件（6 项加固）

| 提交 | 说明 |
|------|------|
| `90da765` | PMM alloc_pages/free_pages 全局 spinlock |
| `052f3e6` | Slab kmalloc/kfree 递归 per-CPU spinlock |
| `7b1b52e` | softirq_status 原子操作 + timer_list_lock + AP TIMER_SIRQ |
| `29d129c` | fork_mm_copy flush_tlb() → tlb_shootdown() |
| `40ccd3d` | task_wake 重试模式（t->cpu/on_rq 迁移竞态） |
| `b3465ff` | AP 启动 3 个修复（wrmsr EDX:EAX、CR4 SSE、IST 栈重定向） |

### AP 启动 Bug 修复（验证中发现）

| Bug | 症状 | 修复 |
|-----|------|------|
| wrmsr EDX:EAX 分割 | AP #PF at ret_from_intr (GS base 错误) | trampoline.S 添加 `movq %rax,%rdx; shrq $32,%rdx` |
| CR4_OSFXSR\|OSXMMEXCPT 缺失 | AP 用户任务 #UD on movups/movaps (SSE) | trampoline.S `orl $(CR4_PAE\|CR4_OSFXSR\|CR4_OSXMMEXCPT)` |
| kill_current_user_task IST 栈重定向 | #PF at RIP=0 after task kill | iretq → direct RSP switch + call do_exit |

### 文件变更

| 类别 | 文件 | 说明 |
|------|------|------|
| **修改** | `kernel/sched/task.c` | sched_pick_cpu、sched_notify_remote、sched_balance、task_wake 重试、do_fork/spawn/schedule 集成、idle_task_resume、nr_running |
| **修改** | `kernel/include/kernel/percpu.h` | +`uint32_t nr_running` |
| **修改** | `kernel/memory/slab.c` | per-CPU 递归 slab_lock |
| **修改** | `kernel/memory/pmm.c` | pmm_lock for alloc_pages/free_pages |
| **修改** | `kernel/intr/softirq.c` | lock orq/andq atomic softirq_status |
| **修改** | `kernel/time/timer.c` | timer_lock + do_timer 解锁回调模式 |
| **修改** | `kernel/apic/lapic_timer.c` | AP TIMER_SIRQ + softirq.h include |
| **修改** | `kernel/arch/x86_64/trampoline.S` | wrmsr EDX:EAX + CR4 SSE bits |
| **修改** | `kernel/arch/x86_64/trap.c` | kill_current_user_task RSP switch |
| **新增** | `libc/rbtree/rbtree.c` | rbtree_last、rbtree_prev |
| **新增** | `user/smp_stress.c` | CPU-bound 多进程负载均衡验证 |

**总计: 15+ commits, 12 files, 126/126 systest pass (-smp 2)**

---

## Timer 重构实施总结（clocksource + clockevent）

> 目标：根治 QEMU TCG 下 PIT 200Hz 伪影（jiffies 2x），把精粒度时间与 tick 解耦，为 aarch64 预留时钟接口。前置：`docs/pit-200hz-handoff.md`（根因定位）。spec/plan 见 `docs/superpowers/specs/2026-08-17-timer-clocksource-clockevent-design.md`（v8 修订）。

### 架构

```
┌─────────────────────────────────────────────────────┐
│ 精粒度消费者: clock_gettime / nanosleep / poll/select │ ← 纳秒时间戳
├─────────────────────────────────────────────────────┤
│ clocksource 层 (kernel/time/clocksource.c, arch无关) │ ← 单调纳秒
│   clocksource_read_ns() = (cycle+offset)*mult>>shift │
├─────────────────────────────────────────────────────┤
│ clockevent 层 (kernel/time/tick.c, arch无关)         │ ← tick 语义
│   tick_handler(): jiffies++ / poll超时纳秒扫描/need_resched
├─────────────────────────────────────────────────────┤
│ arch hook (kernel/include/kernel/arch/*.h)           │
│   arch_cycle_counter() [已有]  arch_cycle_freq() [新增]
│   arch_tick_start() [新增]  ← x86: LAPIC; aarch64: CNTP(预留)
└─────────────────────────────────────────────────────┘
粗粒度消费者: EEVDF、watchdog、kernel timer 轮、lwIP 粗超时、AHCI → jiffies（10ms/tick 不变）
```

### 关键决策

| 决策 | 选择 | 理由 |
|------|------|------|
| tick 源 | LAPIC 周期模式接管，PIT 掩蔽 + 未校准回退 | LVT 本地投递免疫 QEMU IOAPIC edge 伪影；PIT 先跑保证 boot 窗口有 tick |
| TSC 频率校准 | CPUID15h + RTC PIE 联合校准（RTC PIE 结果优先复用） | 250ms PIE 窗口比 TSC 窗口更稳；AP 复用 BSP 校准状态 |
| mult/shift | `__uint128_t` 中间值 + shift 上限放宽 | 高频 TSC（≥4.3GHz）需 shift≥35，64 位中间值会溢出（commit 496a210） |
| 时间语义 | 精粒度路径迁纳秒；调度器/watchdog/lwIP 粗超时保持 jiffies | 亚 tick 分辨率只对用户可感知路径有价值；避免重写已验证的 deadline 算术 |
| jiffies | 仍隐含 10ms/tick，修好 tick 源后自动恢复正确速率 | EEVDF 是 tick 粒度语义，不迁纳秒 |
| aarch64 | clocksource/clockevent 接口 hook 预留（cntvct_el0 + CNTP），不写 ARM 代码 | YAGNI，避免未验证代码 |

### 实现期发现

- **per-LAPIC DIV 必须写**：QEMU 双核实测 298Hz 定位——AP 复位后 DIV 为 ÷1，静态值 ÷2 折算导致 AP 200Hz。`lapic_timer_start` 必须在每个 CPU 写 `LAPIC_TIMER_DIV`（spec v8 门① 实证）。

### Commit 列表

| Commit | 内容 |
|--------|------|
| `dc25e97`~`41b3541` | spec/plan 迭代 v1→v8 + Hermes 外部评审归档（9 个 docs commit） |
| `de052f2` | clocksource + clockevent 双层抽象（TSC/cntvct_el0，接口层） |
| `4cc5779` | TSC 频率校准（CPUID15h + RTC PIE 联合校准，含超时） |
| `67f99e2` | BSP 切 LAPIC 周期 tick，先掩 PIT 再接管 + 握手采样 |
| `86f4ceb` | CLOCK_MONOTONIC/REALTIME + nanosleep + poll 迁纳秒 |
| `7a06a6e` | 内核 jiffies 频率 self-test + 验证门证据 + 回归 |
| `14db988` | 最终评审修复 — RTC PIE 窗口对齐/mult-shift 上界/aarch64 stub/CPUID 守卫/删死代码/IRQ-safe 锁 |
| `496a210` | compute_mult_shift 高频 TSC 溢出 — __uint128_t 中间值 |

### 验证

- systest **150/150**（含 jiffies 频率 self-test：QEMU 实测 tick 速率与期望值匹配）
- 回归：select/poll 超时、EEVDF 时间片、lwIP 超时、busybox `sleep 1` ≈ 1s（恢复真 10ms/tick）

---

## 已完成汇总 (截至 2026-08-18)

| 项目 | 工作量 | 日期 |
|------|--------|------|
| syscall + signal 框架 (systest 43 syscall) | 2 天 | 07-02 |
| do_signal_delivery (Ctrl-C→SIGINT) | 1 天 | 07-03 |
| FPU/SSE 状态保存 (fxrstor/fsave) | 半天 | 07-03 |
| busybox ash shell + 9 applet | 1 天 | 07-04 |
| 信号 handler 用户态投递 | 2 天 | 07-05 |
| systest 70/70 | 1 天 | 07-05 |
| 4KB 页面 + VMA + mmap/mprotect | 3 天 | 07-08 |
| **COW fork (4KB-only)** | 2 天 | 07-11 |
| VFS mount point getdents 修复 | 半天 | 07-11 |
| ext2 只读 + GPT + tmpfs + /dev 块设备 | 1 天 | 07-11 |
| disk.img GPT 双分区 + tools/mkdisk | 1 天 | 07-11 |
| selftest 10/10 + systest 70/70 修复 | 2 小时 | 07-11 |
| SMP 栈溢出修复 (ext2 buf[256]→buf[4096]) | 30 分钟 | 07-11 |
| 内核栈 canary (SSP) | 30 分钟 | 07-11 |
| arch 通用头文件迁移 | 2 天 | 07-12 |
| 子系统注册框架 | 半天 | 07-12 |
| 日志级别系统 | 5 天 | 07-12 |
| **多架构清理收尾** (8 dispatch + 7 aarch64 桩) | 1 天 | 07-15 |
| **busybox ash 方向键+行编辑** (VT100 CSI + terminal + FIONREAD) | 2 天 | 07-17 |
| arch/x86_64 头文件引用清理 | 1 小时 | 07-17 |
| **poll syscall** (poll_table + 双队列 wake + pipe/tty/devfs + PIT timeout) | 2 天 | 07-18 |
| Makefile QEMU targets 统一 AHCI (run/run-kvm/debug) | 10 分钟 | 07-18 |
| **ext2 读写** (alloc_block/inode、create/mkdir/rmdir/unlink/rename/truncate、selftest) | 2 天 | 07-19 |
| refactor: 固定数组→堆分配 (VFS name/cwd + mount_table + pipe buf + ext2 buf) | 1 天 | 07-19 |
| **select/pselect syscall** (poll_table 动态化 + do_poll_core 提取 + 适配层 + pselect6 sigmask 原子性；当时 systest 118/118，当前 126/126) | 2 天 | 07-24 |
| **EEVDF 调度器** (rbtree 可运行队列 + vruntime/deadline + pick_eevdf O(log n) + per-CPU TSS SMP 修复) | 2 天 | 07-25 |
| **SMP 负载均衡** (idle-steal + per-schedule pull + sched_pick_cpu + nr_running 指标 + 振荡防护 + 6 项 SMP 前置条件加固 + init 迁移保护 + idle→idle #PF 修复 + smp_stress 验证) | 2 天 | 07-29 |
| **inittab 配置支持** (ACT_* 位掩码修复 + parse_inittab() open/read 解析器 + 3 套模板 + Makefile/build 集成 + test-inittab 相位派发验证) | 1 天 | 08-01 |
| **lwIP 网络栈合并** (E1000/virtio-net + PCI/MSI-X + DHCP/DNS + TCP/UDP socket + poll/select + HTTP wget) | 多迭代 | 08-15 |
| **网络正确性加固** (DNS 超时、端口字节序、部分读缓存、shutdown、UDP readiness、响应 hang) | 3 天 | 08-12~08-15 |
| **arch 边界收紧** (x86 平台源选择、early task-state hook、公共 gate ABI、arch signal API、端口 I/O wrapper) | 1 天 | 08-15 |
| **/proc/<pid>/fd/ 观测性** (files_t 引用协议 pin/unpin + dup/dup2/fcntl 路由重构 + exit 路径 pin) | 1 天 | 08-15 |
| **per-poll timeout registry** (poll_timeout_head 链表 + PIT 扫描，修复 lost-wakeup + 并发 clobber) | 半天 | 08-15 |
| **keyboard poll 支持** (/dev/keyboard 扫描码 wait queue + keyboard_poll_dev) | 半天 | 08-15 |
| **tty termios 诚实化** (TCSETS 真存储 + raw 默认 + ICANON/ECHO) | 半天 | 08-15 |
| **terminal alt-screen 双缓冲** (?1049h/l) | 半天 | 08-15 |
| **俄罗斯方块游戏** (framebuffer 像素渲染 + 扫描码输入 + alt-screen 恢复 + UX) | 1 天 | 08-16 |
| **E1000 RX ring 所有权串行化** (tcpip 线程独占硬件 ring 消费，IRQ 仅 ack + wake) | 半天 | 08-16 |
| **requested-event-aware poll/select** (按请求方向注册/唤醒，修复复合 flags + PTY 双注册容量 + 时序敏感 select 断言) | 1 天 | 08-16 |
| **DHCP ACD 关闭** (LWIP_DHCP_DOES_ACD_CHECK=0，消除 ~10.6s ACD 竞态导致的偶发不绑定) | 1 小时 | 08-16 |
| **网络回归 harness** (make test-network + OS01_TCP_ECHO_DELAY_MS delayed-reply + 20/20 no-delay + 10/10 delay250 cohort) | 1 天 | 08-16 |
| **timer 重构** (clocksource+clockevent 双层抽象 + TSC/RTC-PIE 联合校准 + LAPIC tick 接管掩 PIT + CLOCK_MONOTONIC/REALTIME/nanosleep/poll 迁纳秒 + jiffies self-test，systest 150/150，根治 PIT 200Hz) | 2 天 | 08-17~18 |

---

## 开源 OS 项目借鉴 — 下一步行动

| 项目 | 已经用到的 | 还可以拿来的 |
|------|----------|-------------|
| **Tilck** | EEVDF 调度思路、3 层测试、hang detector | EEVDF 代码结构、load balancing、GDB helper |
| **cavOS** | lwIP、socket syscall、E1000、动态链接与 Alpine apk 路线参考 | 动态链接器加载流程、用户态包兼容 |
| **Aquila** | **ext2 R/W 核心 (~221 行)**: inode/block alloc+free | — |
| **ArvernOS** | 多架构抽象思路、aarch64 dispatch 桩模式 | 分层日志系统、UBSan、aarch64 head.S/GIC/Generic Timer |
| **opuntiaOS** | devman 子系统注册框架 | GICv2 驱动、Generic Timer（clocksource/clockevent 接口已预留）、Window Server GUI |
| **HackOS** | — | 可缩放字体渲染器、VESA 图形模式 |

---

## 关键设计决策

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 1 | COW 粒度 | 4KB-only V1（2MB 推迟） | 2MB split 子页风险过高；4KB COW 已捕获 fork+exec 主要收益 |
| 2 | COW 引用计数 | `subpage_pool.cow_count[512]` | 无需改 struct Page |
| 3 | 调度器 | EEVDF (Tilck 路线) | Linux 6.6+ 生产级算法 |
| 4 | 网络栈 | 内核态 lwIP 2.2.1 + 自有 socket syscall 层 | TCP/UDP/DHCP/DNS 与 poll/select 已贯通；下一步补自动化网络回归和 TLS |
| 5 | 文件系统 | FAT32 + ext2 R/W + tmpfs + devfs + procfs | ext2 读写完整，UNIX 权限完整 |
| 6 | 测试 | Tilck 3 层 (unit+self+sys) | 126/126 systest pass |
| 7 | 用户态 | busybox → 动态链接 → Alpine apk | cavOS 已验证可行 |
| 8 | COW PTE 标记 | `PAGE_COW` (bit 10) | bit 9 已被 `PAGE_PROTNONE` 占用 |
| 9 | COW 并发保护 | `subpage_lock` (已有 spinlock) | 无需新增锁 |
| 10 | 内核栈 canary | 全局 `__stack_chk_guard` (`arch_cycle_counter()` 种子) | clang 生成 RIP-relative全局引用，无需 TLS/FS |
| 11 | 磁盘布局 | GPT 双分区 (FAT32 ESP + ext2 root) | UEFI 标准，内核自解析 GPT |
| 12 | 子系统注册模式 | 运行时 `register_subsys()` | 不引入 ELF section 依赖 |
| 13 | 多架构 dispatch | `arch/*.h` 用 `#ifdef __x86_64__` / `#elif __aarch64__` | ISP 在 include 层解决 |
| 14 | VT100 CSI 终端模拟器 | `console_putchar()` state machine (~330 行) | framebuffer 与终端分离 |
| 15 | TTY ioctl (TCGETS/TCSETS) | tty_ioctl() 映射 ICANON/ECHO ↔ termios | ash 可切 raw mode |
| 16 | 键盘 VT100 转义序列 | K_UP/DOWN/LEFT/RIGHT → ESC [ A/B/C/D | canonical 模式静默丢弃 |
| 17 | poll/select 多路复用 | poll_table + 双队列 (task/poll 分离) + 级联唤醒 | POSIX 合规：POLLHUP/POLLERR 强制上报，EINTR on any signal |
| 18 | pipe 阻塞语义 | wait_queue_t 替代 busy-loop schedule() | 双队列：read_wait/write_wait (task) + read_poll/write_poll (poll entry) |
| 19 | poll 超时 | jiffies + PIT 100Hz callback → wake_all(poll_wq) | 级联唤醒路径无冗余扫描 |
| 20 | QEMU 设备模型 | `-drive` + `ahci/ide-hd` 替代 `-hda` | 与 GPT 分区表 + AHCI 驱动一致 |
| 21 | select/pselect 适配层 | `do_poll_core` 共享 + `do_select_common` 去重 | poll 和 select 共享轮询循环；select/pselect 共享 fd_set↔pollfd 转换 |
| 22 | poll_table 动态化 | `entries[]` → `*entries + max_entries` | select 支持 1024 fd，poll 保持 16 fd 不变 |
| 23 | pselect6 sigmask 原子性 | goto-out 模式 + `mask_swapped` 标志 | 所有错误路径恢复 blocked；nfds==0 路径也做 atomic swap |
| 24 | SMP 负载均衡指标 | `nr_running` 为主 + `min_vruntime` tiebreaker | 避免 min_vruntime 单调递增语义导致的虚假"最忙"判断（1 CPU-bound vs 10 interactive） |
| 25 | SMP 窃取方向 | rbtree 尾部（最大 deadline） | CPU-bound 任务高 vruntime→自然迁移到空闲核；交互式任务留在源核 |
| 26 | SMP 振荡防护 | 门控 `src > local + 1` + count = `(diff)/2` | O(log N) 收敛，无 2:1↔1:2 ping-pong |
| 27 | SMP 前置条件 | 6 项全局无锁代码加固 | slab/PMM/softirq/timer/fork_mm_copy/task_wake — AP 运行用户程序前必须 SMP 安全 |
| 28 | inittab 格式 | `id:action:process`（3 字段，冒号分界，open+read 直接解析） | 去掉 runlevel；ACT_* 位掩码 0x01..0x80 与 run_actions() dispatch 兼容 |
| 29 | OS01_SYSTEST 切换 | Makefile cp 模板（config/inittab.systest），非编译期 #ifdef | 同一 init 二进制同时支持测试/正常模式 |
| 30 | 任务回收机制（**候选，暂缓**） | wait 驱动回收：do_waitpid 直接收割（`on_cpu==0` 检查 + list_del + kfree(thread/fpu/stack)），删 schedule() 内 zombie reaper 扫描 + deferred_free（df-kthread） | 贴 Linux 语义（release_task 在 wait4 路径同步回收，调度器零参与）；当前 reaper 是"调度器驱动 + 异步"组合，是 double-book 竞态（f58d1a1）的温床；孤儿由 do_exit reparent→init + init reap_children 兜底。触发条件：频繁改任务退出/父子语义或加 exit_group 时实施。详见 docs/scheduler-complexity.md §3 🔴 |
| 31 | tetris 输入 | `/dev/keyboard` 原始扫描码 | tty TCSETS no-op；扫描码自解析 E0 前缀 + release 位（方向键 E0 48/4B/4D/50） |
| 32 | tetris 渲染 | fb mmap 像素块（20×10 × 30px） | per-VMA 多进程共享；FBIOSURRENDER 只停内核 console |
| 33 | 终端恢复协议 | Linux alt screen `\e[?1049h/l` | 标准化、零轮询零 waitpid；vim/less 可复用 |
| 34 | terminal.elf 双缓冲 | offscreen 主缓冲 + alt 缓冲 | 退出后内容 100% 一致、恢复零重绘 |
| 35 | 游戏启动 | 手工 `exec /bin/tetris` | 不进 inittab |
| 36 | serial 渲染后端 | ❌ 不并入 | 38400 baud 增量重绘可行但投入产出比低；收敛范围 |
| 37 | nanosleep 修复 | B 事件驱动（wakeup_jiffies + PIT 扫描）优先；A 最小可用 fallback | 睡眠无唤醒源（tetris 卡死实证）；连带排查 signal 假醒（sleep 1 不睡） |
| 38 | poll 注册方向语义 | requested & legal & unavailable 才注册（poll_requested_read/write 分类器）；readiness 只由 open mode 决定，绝不因 requested bits 创造就绪 | 修复复合 flags（O_RDWR 管道误报方向）、PTY 双注册容量（每 fd 2 槽）、socket POLLOUT 吞 POLLIN 注册 |
| 39 | poll 超时最终扫描 | 超时路径 cleanup 后 `poll_scan(kfds, nfds, NULL)` 只读扫描 | 超时返回前不恢复 current_poll_wq，避免最后一拍唤醒被丢 |
| 40 | DHCP ACD | `LWIP_DHCP_DOES_ACD_CHECK=0` | QEMU user-mode NAT 单 guest 无地址冲突可能；ACD ~10.6s PROBE/ANNOUNCE 竞态 nettest 10s 等待，偶发不绑定 |
| 41 | tick 源 | LAPIC 周期模式接管，PIT 掩蔽 + 未校准回退 | QEMU TCG IOAPIC edge 无沿检测 → PIT 200Hz 伪影；LAPIC LVT 本地投递免疫，PIT 先跑保证 boot 窗口 |
| 42 | 精粒度时间 | clocksource 纳秒（TSC, mult/shift）与 tick 解耦 | clock_gettime/nanosleep/poll/select 需亚 tick 分辨率；粗超时（EEVDF/watchdog/lwIP/AHCI）保持 jiffies |
| 43 | TSC 频率校准 | CPUID15h + RTC PIE 联合校准（PIE 结果优先复用，AP 复用 BSP 状态） | 250ms PIE 窗口比 TSC 窗口更稳；跨核只做握手采样粗偏移修正 |
| 44 | mult/shift 计算 | `__uint128_t` 中间值 + shift 上限放宽（>1GHz 需 shift>31） | 高频 TSC（≥4.3GHz）需 shift≥35，64 位中间值溢出（496a210 实证） |
| 45 | per-LAPIC DIV | `lapic_timer_start` 每 CPU 写 `LAPIC_TIMER_DIV` | AP 复位后 DIV=÷1，静态值 ÷2 折算 → AP 200Hz（QEMU 双核 298Hz 实证） |
| 46 | aarch64 时钟 | clocksource/clockevent 接口 hook 预留（cntvct_el0 + CNTP），不写 ARM 代码 | YAGNI，避免未验证代码；随 aarch64 启动实施 |
