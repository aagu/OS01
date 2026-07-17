# OS01 优化路线图 v8

> **基准**: `7258cac` (busybox ash editing + cursor blink complete — 38 commits, 49 files)
> **日期**: 2026-07-17

标记: ✅ 已完成 | 🔴 P0 本迭代 | 🟡 P1 本月 | 🟢 P2 下月 | 🔵 P3 远期

---

## 当前状态总览 (6 个 Phase 全部就绪)

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork, mmap/mprotect/munmap, demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、多架构清理（aarch64 桩到位）、SMP (percpu+GS-base)、canary、hang detector、debug channels、kallsyms、FPU 保存 | ✅ |
| **Phase 3: 信号 + 调度** | do_signal_delivery、Ctrl-C→SIGINT、systest 70/70、优先级 O(n) 调度器 | ✅ |
| **Phase 4: 文件系统** | ext2 只读、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+raw mode TTY）、9 applet、init、libc (printf/malloc/string/syscall wrapper)、VT100 CSI 终端模拟器 | ✅ |

---

## 待实施优先级

```
P0 (本迭代 — 本月):
 1. EEVDF 调度器              — O(n)→O(log n)，公平调度，反饿死
 2. poll/select               — 多路 I/O，解锁 busybox 网络 applet（v1 实现 FIONREAD + poll stub 已就绪，需升级为 wait queue）
 3. ext2 读写                 — 完成文件系统故事，/ 可写

P1 (本月):
 4. 多架构 aarch64 ✅ 清理     — 8 dispatch 头文件 + 7 aarch64 桩 + task.c 拆分（20 commits）
 5. /proc/<pid>/fd/ + maps   — 进程级调试
 6. 日志级别 ✅               — ERR/WARN/INFO/DEBUG，开发效率质变
 7. rwlock/seqlock            — VFS /proc 多核缩放
 8. lwIP 网络栈 + E1000       — 第一个网络能力
 9. 用户栈 canary             — 用户态加固

P2 (下月):
10. 更多 busybox applet      — grep/sed/find (依赖 poll/select + regex)
11. aarch64 启动              — head.S + GIC + Generic Timer（清理已就绪）
12. readlink/symlink          — ext2 写驱动的前置条件
13. 动态链接器                — 共享 libc，减小二进制体积

P3 (远期):
14. ASLR                      — 用户态安全
15. SMP 负载均衡              — push-pull / work stealing
16. UBSan + KASan            — 运行时 bug 检测
17. 真机启动 (USB)            — "不能在真机上测试就别实现"
18. 网络进阶 (AF_UNIX, socketpair)
19. GUI 框架 (参考 opuntiaOS + HackOS)
20. Alpine apk 用户态        — cavOS 路线，直接安装 musl 二进制包
21. NVMe 驱动                — 替代 AHCI
```

**依赖链:**
```
  poll/select ──→ 更多 busybox applet ──→ 动态链接器
       │
       └─────→ lwIP 网络栈 (socket syscall 层)
       
  ext2 只读 ──→ ext2 读写 ──→ readlink/symlink
       
  rwlock ──→ VFS 多核缩放 (VFS lookup, /proc read)
```

---

## Phase 解析

### P0: 本迭代 (预计 2-3 周)

> 做了这么久基础设施，是时候让系统有"操作系统"的感觉了。

#### 1. EEVDF 公平调度器 [借鉴 Tilck] 🟡 P1

**动机:** 当前 `schedule()` 扫描全局链表 O(n)，`max-counter + priority` 策略无公平性保证。EEVDF 是 Linux 6.6+ 的生产级算法。

| 任务 | 工作量 | 说明 |
|------|--------|------|
| AVL/rbtree 可运行队列 | 1-2 天 | 替代 O(n) 链表扫描 |
| vruntime 累积 + min_vruntime | 1 天 | 子刻度精度 |
| eligibility + deadline 选择 | 1 天 | `vruntime ≤ avg_vruntime` 且 deadline 最早 |
| **收益:** | | 反饿死 + 响应时间可预测 + 高负载下不减速 |

#### 2. poll/select 系统调用 🟡 P1

**动机:** 当前 `SYS_read`/`SYS_write` 阻塞 TTY/pipe 的方式不支持多路复用。busybox 的 ash 等待子进程、网络 applet、甚至简单的并发 I/O 都需要 poll/select。

| 任务 | 工作量 | 说明 |
|------|--------|------|
| 内核 poll_table 框架 | 半天 | `poll_wait(fd, wait_queue)` 注册机制 |
| TTY poll 支持 | 半天 | tty_poll(): readable/writable/hangup |
| pipe poll 支持 | 半天 | pipe_poll() |
| 通用 busybox ash 依赖 | 半天 | ash 内置 `pump` + `G.zombie` wait 模式 |

**收益:** 解锁 busybox ash 的 `wait` 回显、网络相关 applet 依赖。

#### 3. ext2 读写驱动 🟡 P2

**动机:** 当前 ext2 只读，`/` 挂了但不可写。要完成文件系统故事，需要 inode/block 分配。

| 任务 | 工作量 | 借鉴 |
|------|--------|------|
| block bitmap 分配/释放 | 半天 | Aquila (221 行) |
| inode bitmap 分配 | 半天 | Aquila |
| 目录操作 (mkdir/rmdir) | 1 天 | link/unlink/rename |
| ext2_write/truncate | 1 天 | VFS write path |

**收益:** `/` 可写，系统调用具备持久化能力。

---

### P1: 本月

#### 4. /proc 完善 🟡 P1

| 任务 | 工作量 | 说明 |
|------|--------|------|
| `/proc/<pid>/fd/` | 半天 | 查看打开的文件描述符 (ls -l /proc/1/fd) |
| `/proc/<pid>/maps` | 半天 | 查看 VMA 布局 |
| `/proc/<pid>/status` 扩展 | 半天 | 增加 signal mask、ppid、utime/stime |

**收益:** 调试效率提升，busybox `ps`/`top` 的基础数据。

#### 5. 日志级别系统 ✅

**动机:** 当前 `serial_printk`/`color_printk` 无级别概念。开 DEBUG 看流水淹没过筛，关 DEBUG 关键错误丢失。

| 任务 | 工作量 | 借鉴 |
|------|--------|------|
| `log_impl(LEVEL, fmt, ...)` 宏 | 半天 | ArvernOS 分层日志 |
| `log_set_level()` / 编译期过滤 | 半天 | `KERN_ERR=3 KERN_WARN=4 KERN_INFO=6 KERN_DEBUG=7` |
| 已有 printk 迁移 | 1 天 | `printk(KERN_INFO ...)` → 逐步替换 |

**收益:** 调试效率质变 — 已知问题 vs 存疑警告快速区分。

#### 6. rwlock / seqlock 🟡 P2

| 任务 | 工作量 | 说明 |
|------|--------|------|
| rwlock 实现 | 半天 | 读共享/写独占，基于 spinlock 的简单实现 |
| VFS lookup 路径加锁 | 半天 | 读时取 rlock，写 (mount/unlink) 时取 wlock |
| seqlock (备选) | 半天 | 读不阻塞写，适合 /proc read |

**收益:** VFS 多核路径缩放，`make -smp 2` 下 /proc 并发读不冲突。

#### 7. lwIP 网络栈 + E1000 驱动 🟡 P1

**动机:** 最大的"缺失功能"。网络解锁 telnet/ssh/http/nslookup 等无限可能性。

| 任务 | 工作量 | 借鉴 |
|------|--------|------|
| lwIP 移植到内核 | 1 天 | cavOS 的开箱即用方案 |
| E1000 PCI NIC 驱动 | 2 天 | QEMU `e1000` 虚拟网卡 |
| socket syscall 层 | 1-2 天 | `UserSocket` + `lwip_send`/`lwip_recv` |
| poll/select 网络集成 | 半天 | socket poll 回调 |

**收益:** 网络栈是从"玩具 OS"到"可用 OS"的最大一步。

#### 8. 用户栈 canary 🟡 P1

| 任务 | 工作量 | 说明 |
|------|--------|------|
| libc 编译 `-fstack-protector-strong` | 半天 | 用户态 SSP |
| ELF 加载器传递 canary 种子 | 半天 | AT_RANDOM aux vector |

**收益:** 用户程序栈溢出检测。

---

### P2: 下月

#### 9. 更多 busybox applet 🟢 P2

> 依赖 poll/select + regex (regcomp/regexec) 就绪。

| 任务 | 阻塞项 | 说明 |
|------|--------|------|
| grep/sed | regex | 从 musl 移植 regcomp/regexec，或手写简单模式 |
| find | glob/fnmatch | libc 新增 `glob()`/`fnmatch()` |
| awk | regex + FPU | regex + 浮点数解析 |
| vi | TTY termios + SIGWINCH | 完整终端控制 |

#### 10. 多架构 aarch64 🟡 P1

**状态:** 基础清理完成（`docs/superpowers/specs/2026-07-14-multi-arch-cleanup-design.md`）。ARCH 参数化 + 8 个 dispatch 头文件 + task.c 拆分 + 7 个 aarch64 桩到位。`make ARCH=aarch64` 所有通用代码已通过 `#include` 路径到达 aarch64 桩。

**剩余工作:**

| 任务 | 借鉴 | 说明 |
|------|------|------|
| `arch/aarch64/spinlock.h` 实现 | — | ldxr/stlxr 独占循环（当前 `#error`） |
| `arch/aarch64/linkage.h` | — | ENTRY/SYMBOL_NAME 汇编宏 |
| `kernel/arch/aarch64/head.S` | ArvernOS | 启动入口（链接器 `_start`） |
| 中断控制器 (GIC) | opuntiaOS | GICv2/v3 驱动 |
| 时钟/定时器 | opuntiaOS | Generic Timer |
| MMU 初始化 | ArvernOS | TTBR0_EL1 设置，higher-half 映射 |
| **子系统框架已就绪** | OS01 | `arch/aarch64/subsys.c` + `subsys_percpu.c` |

**本次清理详情 (20 commits, 36 files, +457/−143):**
- Build: `ARCH ?= x86_64`, `OBJFORMAT` 变量, `arch/aarch64/make.config`
- Dispatch 头文件: `arch/spinlock.h`, `arch/gate.h`（新增）；`arch/cpu.h`, `arch/irq.h`, `arch/atomic.h`, `arch/mmu.h`, `arch/thread.h`, `arch/cache.h`, `arch/segment.h`（aarch64 桩替换 `#error`）
- Include 迁移: 15 个 spinlock 站点 + mutex.c/futex.c 修复 + `asmlinkage` 删除
- task.c 拆分: `__switch_to` → `arch/x86_64/switch.c`, `kernel_thread_func` → `thread_entry.S`, `arch_task_init_platform()` → `task_arch.c`（task.c 0 处 `arch/x86_64/` 直接引用）
- test/include/ 镜像同步 + `arch_virt_to_phys` 字面量替换

#### 11. readlink/symlink 🟢 P2

| 任务 | 说明 |
|------|------|
| `SYS_readlink` | Linux ABI 89 |
| VFS 软连接支持 | 跨文件系统路径解析 |
| ext2 symlink inode | 快速符号链接 (in-inode) |

#### 12. 动态链接器 🟢 P2

| 任务 | 借鉴 | 说明 |
|------|------|------|
| ELF64 .interp 解析 | cavOS | 识别 PT_INTERP |
| ld.so 加载 | cavOS | 内核启动动态链接器 |
| libc.so 共享 | cavOS | 所有 busybox applet 共享一份 libc |

**收益:** 二进制体积从几十 MB 降到几 MB。

---

### P3: 远期

| # | 任务 | 借鉴 | 说明 |
|---|------|------|------|
| 13 | ASLR | — | 随机化加载基址，`brk` 随机化 |
| 14 | SMP 负载均衡 | Tilck EEVDF | CPU affinity → push-pull / work stealing |
| 15 | UBSan + KASan | ArvernOS | `UBSAN=1` debug build，内核地址消毒 |
| 16 | 真机 USB 启动 | Tilck | "不能在真机上测试就别实现" |
| 17 | AF_UNIX sockets | cavOS | 本地 IPC，socketpair |
| 18 | GUI 框架 | opuntiaOS + HackOS | Window Server + Compositor + 可缩放字体 |
| 19 | Alpine apk 用户态 | cavOS | 直接安装 Alpine Linux musl 二进制包 |
| 20 | NVMe 驱动 | — | 替代 AHCI |

---

## 开源 OS 项目借鉴 — 下一步行动

| 项目 | 已经用到的 | 还可以拿来的 |
|------|----------|-------------|
| **Tilck** | EEVDF 调度思路、3 层测试 (unit+self+sys)、hang detector | EEVDF 代码结构、load balancing、`mocking.h` 测试框架、GDB helper 脚本 |
| **cavOS** | lwIP 网络栈参考、动态链接、Alpine apk 路线 | socket syscall 层实现细节、E1000 驱动、动态链接器加载流程 |
| **Aquila** | — | **ext2 R/W 核心 (~221 行)**: inode/block alloc+free 可直抄 |
| **ArvernOS** | 多架构抽象思路、aarch64 dispatch 桩模式 | 分层日志系统、UBSan 集成方式、aarch64 head.S/GIC/Generic Timer |
| **opuntiaOS** | devman 子系统注册框架 | GICv2 驱动、Generic Timer 驱动、Window Server GUI 架构 |
| **HackOS** | — | 可缩放字体渲染器、VESA 图形模式 |

---

## 已完成汇总 (截至 2026-07-15)

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
| arch 通用头文件迁移 (io/cpu/irq/mmu/atomic/percpu) | 2 天 | 07-12 |
| 子系统注册框架 | 半天 | 07-12 |
| 日志级别系统 (log_err/log_warn/log_info/log_debug) | 5 天 | 07-12 |
| **多架构清理收尾** (8 dispatch + 7 aarch64 桩 + task.c 拆分 + 15 spinlock 迁移) | 1 天 | 07-15 |
| **busybox ash 方向键+行编辑** (putchar_at + VT100 CSI 终端 + keyboard VT100 映射 + tty_ioctl TCGETS/TCSETS + FIONREAD poll) | 2 天 | 07-17 |
| arch/x86_64 头文件引用清理 (gate.h/asm.h/cpu.h) | 1 小时 | 07-17 |

---

## 关键设计决策

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 1 | COW 粒度 | 4KB-only V1（2MB 推迟） | 2MB 区段无 VMA 覆盖 + split 子页不在 subpage_pool → 风险过高；4KB COW 已捕获 fork+exec 主要收益 |
| 2 | COW 引用计数 | `subpage_pool.cow_count[512]` | 4KB 页来自 alloc_4k_page → 在 pool 中；无需改 struct Page |
| 3 | 调度器 | EEVDF (Tilck 路线) | Linux 6.6+ 生产级算法 |
| 4 | 网络栈 | lwIP (cavOS 路线) | 快速获得 TCP/IP，后期可替换自研模块 |
| 5 | 文件系统 | FAT32 + ext2 (只读) + tmpfs + devfs + procfs | ext2 只读 370 行，UNIX 权限/inode 完整；tmpfs 430 行，供 /tmp 可写存储 |
| 6 | 测试 | Tilck 3 层 (unit+self+sys) | 经过验证的模式 |
| 7 | 用户态 | busybox → 动态链接 → Alpine apk | cavOS 已验证可行 |
| 8 | COW PTE 标记 | `PAGE_COW` (bit 10) | bit 9 已被 `PAGE_PROTNONE` 占用 |
| 9 | COW 并发保护 | `subpage_lock` (已有 spinlock) | 保护 cow_count RMW + alloc/free 位图，无需新增锁 |
| 10 | 内核栈 canary | 全局 `__stack_chk_guard` (rdtsc 种子) + `-fstack-protector-strong` | clang 在 `-ffreestanding -fpie` 下生成 RIP-relative 全局引用，无需 TLS/FS |
| 11 | 磁盘布局 | GPT 双分区 (FAT32 ESP + ext2 root) | UEFI 标准分区表，`/boot` 和 `/` 分离，内核自解析 GPT |
| 12 | 子系统注册模式 | 运行时 `register_subsys()` (与 softirq 同风格) | 不引入 ELF section 依赖；各 arch 写自己的 `subsys.c` |
| 13 | 多架构 dispatch | `arch/*.h` 用 `#ifdef __x86_64__`/`#elif __aarch64__` dispatch 头文件 | 最小变更：ISP 在 include 层解决，不改 spinlock_T 等已有接口；`interrupt.h`/`task.h` 等通用头通过 dispatch 路由到正确架构 |
| 14 | VT100 CSI 终端模拟器 | console_putchar() state machine：`kernel/tty/console.c` (~330 行)，隔离 framebuffer 底层 | "framebuffer 只画像素，终端管理在 tty 层之上"；光标闪烁由 PIT 100Hz 回调驱动下划线反显 |
| 15 | TTY ioctl (TCGETS/TCSETS) | tty_ioctl() 映射 TTY_L_ICANON/ECHO ↔ POSIX termios c_lflag，ISIG 强制启用 | 让 tcsetattr 真正工作，ash 可切 raw mode；dev_tty 从 devfs.c 迁移到 tty.c 统一管理 |
| 16 | 键盘 VT100 转义序列 | ext_scancode_tbl 改为 int，K_UP/DOWN/LEFT/RIGHT (0x100-0x105)，raw mode 下发 ESC [ A/B/C/D 序列 | canonical 模式静默丢弃；Ctrl+方向键 v1 不处理 |
