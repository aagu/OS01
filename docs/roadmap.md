# OS01 优化路线图 v3

> **基准**: `8038274` (setjmp fix + getppid test fix)
> **日期**: 2026-07-05
> **来源**: v2 路线图 + 本周信号系统实现进展

标记: ✅ 已完成 | 🔴 P0 本周 | 🟡 P1 本月 | 🟢 P2 下月 | 🔵 P3 远期

---

## 自 v1 以来的进展 (2026-06-27 → 2026-07-05)

| 项目 | 状态 |
|------|------|
| CPU idle (`hlt()`) | ✅ |
| 信号传递引擎 + Ctrl-C → SIGINT | ✅ |
| /dev/random (rdtsc 熵源) | ✅ |
| 优先级调度 (max-counter pass) | ✅ |
| FPU/SSE 状态保存 | ✅ |
| ACPI shutdown (FADT PM1a_CNT_BLK) | ✅ |
| procfs (`/proc/meminfo`, `/proc/self/status`, `/proc/<pid>/status`) | ✅ |
| blocker framework (通用阻塞/等待框架) | ✅ |
| fork_mm_copy fix | ✅ |
| 模块化 debug 日志 (`debug_<channel>()` 宏) | ✅ |
| 内置 selftest 框架 (slab, vfs, procfs, spinlock, pm) | ✅ |
| Hang detector (watchdog per CPU, 500ms threshold) | ✅ |
| 内核级 strace (`DEBUG_CHANNELS=syscall`) | ✅ |
| systest 系统测试 (70 passed, 0 failed, 33 test funcs, 43 syscalls) | ✅ |
| `sigprocmask` syscall (完整实现，含 Linux ABI 映射) | ✅ |
| 构建系统升级 (`-MMD` 自动依赖 + 分离 build 目录 + wildcard 源码) | ✅ |
| **信号 handler 用户态投递** (sigframe→trampoline→sigreturn 完整闭环) | ✅ |
| **`sigreturn` trampoline** (`sigreturn_trampoline.S` + busybox 链接) | ✅ |
| **`rt_sigaction` SA_RESTORER** (libc signal/sigaction 自动设置 restorer) | ✅ |
| **setjmp `-fomit-frame-pointer` 修复** (`[RSP]` 替代 `[RBP+8]`) | ✅ |
| **getpid() syscall wrapper 修复** (不再 hardcode return 1) | ✅ |
| **busybox ash 正常启动** (`#` shell 交互就绪) | ✅ |

---

## Phase 1: 质量基础设施 🔴

> 没有测试和诊断能力，复杂功能会引入难以追踪的 bug。
> 这一 Phase 是六个项目分析后的最大启示 — Tilck 的测试体系是其代码质量的核心保障。

### 1.1 测试体系升级 [借鉴 Tilck]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | 系统测试 (syscall 级) | systest.elf 70 passed, 0 failed, 33 tests 覆盖 43 syscall |
| 🔴 P0 | Mock 框架完善 | host 上编译测试非 arch 内核代码 (Tilck `mocking.h` 模式) |
| ✅ | 单元测试扩展 | 已有 `test/` + selftest 框架覆盖 slab/vfs/procfs/spinlock |

### 1.2 模块化调试日志 [借鉴 ArvernOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | DEBUG channel 机制 | `debug_<channel>()` 宏 + `DEBUG_CHANNELS=` 运行时开关已就绪 |
| 🔴 P0 | 日志级别 (ERR/WARN/INFO/DEBUG) | 替代无差别的 `printk`，编译时可屏蔽低优先级 |
| ✅ | stacktrace 符号解析 | `kernel/kallsyms.c` → `nm` 生成符号表 → `backtrace()` 自动符号名+偏移 |

### 1.3 内核诊断 [借鉴 Tilck + cavOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | Hang detector | 检测调度停滞 (500ms) → dump 所有 task 状态 + 调用栈 |
| ✅ | 内核级 strace | `DEBUG_CHANNELS=syscall` — pid + 参数 + 返回值 |
| 🟢 P3 | Debug panel TUI | Tilck 内置交互式 debug 面板 (高级功能，可延后) |

---

## Phase 2: 内存管理升级 🔴

> COW fork 是性能刚需。当前 `fork_mm_copy` 急切复制 2MB 页，fork 延迟 ~100ms → 目标 ~1ms。

### 2.1 Copy-On-Write Fork

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🔴 P0 | 4KB 页面支持 | 当前仅 2MB 大页，COW 场景浪费严重。需拆分 page table 层级 |
| 🔴 P0 | `fork_mm_copy` → COW | `mm_t.refcount` 已存在，父/子共享页表，标记只读 |
| 🟡 P1 | COW fault handler | `do_page_fault` 检测 Write-Cause → 分配新 4KB 页 → 复制 → remap |

### 2.2 内存管理增强

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | `mmap`/`mprotect` syscall | COW 的前置依赖 + 解锁 busybox applet |
| 🟡 P1 | Slab OOM fallback | `alloc_pages()` 返回 NULL 时的优雅降级 |
| 🟡 P2 | Kernel heap frag 统计 | slab 内部碎片可视化 |

---

## Phase 3: 调度器升级 🟡

> 当前: O(n) 链表扫描 + max-counter 优先级。目标: O(log n) AVL 树 + vruntime 公平调度。

### 3.1 EEVDF 公平调度器 [借鉴 Tilck]

Tilck 实现了 Linux 6.6+ 同款 EEVDF 算法，有独立文档 (`docs/scheduler.md`)。

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | AVL/rbtree 可运行队列 | 替代 O(n) 扫描，Tilck 用 AVL 树按 (vruntime, tid) 排序 |
| 🟡 P1 | vruntime 累积 + min_vruntime | 子刻度精度 (`VRUNTIME_SCALE=16`)，整数运算达浮点精度 |
| 🟡 P1 | eligibility + deadline 选择 | `vruntime ≤ avg_vruntime` 且选 deadline 最早者 |
| 🟡 P2 | SMP 负载均衡 | CPU affinity → work stealing / push-pull 跨核迁移 |
| 🟡 P2 | SMP IRQ4 验证 | v1 遗留 — smp=2 时 IRQ 交付路径验证 |

### 3.2 EEVDF 核心公式 (来自 Tilck docs/scheduler.md)

```
V = mean(vruntime over runnable + curr)

Task i is eligible ⟺ vruntime_i ≤ V

Selection: among eligible tasks, pick argmin(deadline_i)
  where deadline_i = vruntime_i + slice

Equal-weight collapse (当前 Tilck 配置):
  w_i = 1 ∀i, slice = SCHED_LATENCY / N
```

---

## Phase 4: 文件系统增强 🟡

> FAT32 没有权限、符号链接、inode — 这是 UNIX 核心概念缺失。

### 4.1 ext2 支持 [借鉴 Aquila]

Aquila 的 ext2 只读驱动仅 221 行，是最佳范本。

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | ext2 只读驱动 | superblock、block group descriptor、inode 表、间接块 |
| 🟡 P2 | ext2 读写驱动 | inode 分配/释放、block bitmap、目录操作 |
| 🟡 P2 | VFS vnode refcounting | Aquila 的 `vref()`/`vrele()` 引用计数模式 |

### 4.2 /proc 完善

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | `/proc/<pid>/fd/` | 查看进程打开的文件描述符 |
| 🟡 P1 | `/proc/<pid>/maps` | 进程内存映射信息 |
| 🟡 P2 | sysfs 骨架 | 参考 Tilck 暴露 ACPI namespace + PCI 设备列表 |

### 4.3 缺失的 syscall

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | `sigprocmask` 完整实现 | Linux ABI 映射 (nr 14→42)，信号屏蔽字 atomic get/set，已解锁 busybox shell |
| 🟡 P1 | `mmap`/`mprotect` | COW 前置 + 解锁更多 busybox |
| 🟡 P1 | `readlink`/`symlink` | ext2 软硬链接的前置条件 |
| 🟡 P1 | `poll`/`select` | 多路 I/O，解锁 busybox 网络相关 applet |
| 🟡 P2 | `setitimer`/`alarm` | 用户态定时器 |

---

## Phase 5: 网络栈 🟡→🟢

> 当前 OS01 无任何网络能力。选用 lwIP 快速获得 TCP/IP，后期可替换自研模块。

### 5.1 lwIP 集成 [借鉴 cavOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | lwIP 移植到内核 | cavOS 的开箱即用方案，TCP/UDP/IP 协议栈无需从零写 |
| 🟡 P1 | NIC 驱动 (E1000) | QEMU 最广泛支持的虚拟网卡，Intel 文档齐全 |
| 🟡 P1 | socket syscall 层 | cavOS `UserSocket` + `lwip_send`/`lwip_recv` 封装模式 |
| 🟡 P2 | AF_UNIX sockets | 本地 IPC，cavOS 已实现 |

### 5.2 自研协议栈 [借鉴 ArvernOS]

ArvernOS 的自研网络栈每层约 1 个文件，清晰可读。

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟢 P2 | Ethernet → ARP → IPv4 → ICMP | 学习目的，模块化替换 lwIP 的对应层 |
| 🟢 P2 | UDP → DHCP → DNS | 无外部依赖，自包含 |
| 🟢 P3 | TCP | 最大挑战，lwIP 的 TCP 更实际 |

**策略**: lwIP 先跑通 → 按需用自研模块替换 (先从 ARP/IP 开始)。

---

## Phase 6: 信号系统完善 🟡

> ~~当前内核态信号引擎正常 (SIGINT/SIGCHLD/SIG_DFL)，但用户态 handler 不工作。~~
> **Phase 6 全部完成！** 信号 handler 用户态投递链路完整闭环。

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | `sigreturn` trampoline | 用户栈帧: return addr → `sigreturn` syscall，恢复被中断上下文 |
| ✅ | `rt_sigaction` 完整实现 | SA_SIGINFO、sa_flags、SA_RESTORER 自动设置 |
| ✅ | `sigprocmask` 完整实现 | 已完成 — Linux ABI 兼容，信号屏蔽字 atomic get/set |
| ✅ | 信号 handler 用户态投递 | sigframe push → handler 执行 → trampoline → sigreturn → 恢复 |
| 🟢 P3 | `sigsuspend`/`sigpending` | 原子 sigmask + sleep（低频需求，延后） |

---

## Phase 7: 内核加固 🟡

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🔴 P0 | 内核栈 canary | 启用 `-fstack-protector` + 实现 `__stack_chk_guard` + `__stack_chk_fail` |
| 🟡 P1 | 用户栈 canary | 用户程序编译时启用 SSP |
| 🟡 P2 | ASLR 基础 | 随机化用户程序加载基址 |
| 🟡 P2 | rwlock / seqlock | 读多写少场景 (VFS lookup, /proc read) 优化 |
| 🟢 P3 | UBSan 集成 | ArvernOS 模式：`UBSAN=1` debug build 选项 |

---

## Phase 8: 用户态生态 🟡→🔵

### 8.1 更多 busybox applet

| 优先级 | 任务 | 阻塞项 |
|--------|------|--------|
| 🔴 P0 | grep/sed | regex (regcomp/regexec) — 需从 musl 移植或手写 |
| 🟡 P1 | find | glob/fnmatch |
| 🟡 P1 | awk | regex + 浮点数支持 |
| 🟡 P2 | vi | 完整 TTY termios + signal (SIGWINCH) |

### 8.2 动态链接 [借鉴 cavOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P2 | ELF64 动态链接器 | 共享 musl libc，减小二进制体积 |
| 🔵 P3 | Alpine apk 用户态 | cavOS 终极方案 — 直接安装 Alpine Linux musl 二进制包 |

cavOS 已做到的: `apk add` → bash, vim, gcc, python3, xorg-server, mpv, htop, netsurf, quake, thunar...

这需要 ~200+ Linux syscall 兼容，是长期目标。

### 8.3 GUI 框架 [借鉴 opuntiaOS + HackOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🔵 P3 | 可缩放字体渲染器 | HackOS 字体方案 |
| 🔵 P3 | Window Server + Compositor | opuntiaOS 架构: WindowManager + Compositor + IPC |
| 🔵 P3 | LibG / LibUI | 独立图形库 + UI 控件库 |

---

## Phase 9: 多架构 + 构建 🔵

| 优先级 | 任务 | 借鉴 | 说明 |
|--------|------|------|------|
| 🟢 P2 | 多架构抽象 (`arch/`) | ArvernOS | x86_64 → aarch64 (Raspberry Pi 3/4) |
| 🔵 P3 | 真机测试 (USB boot) | Tilck | "不能在真机上测试就别实现" |
| 🔵 P3 | clang-tidy / coverity | — | 静态分析 CI 集成 |
| 🔵 P3 | GDB 脚本增强 | Tilck | `tasklist`、`handles`、`vfs` 等调试 helper |

---

## 实施优先级总览

```
P0 (本周):
 1. COW fork + 4KB 页面       — 最大性能瓶颈，fork 100ms→1ms
 2. 内核栈 canary             — 低改造成本，防栈溢出
 3. mmap/mprotect             — COW 前置 + 解锁 busybox grep/sed
 4. 日志级别 (ERR/WARN/INFO/DEBUG) — 调试效率质变

P1 (本月):
 5. ext2 只读                 — UNIX 文件系统核心 (Aquila 221 行范本)  
 6. COW fault handler         — 完成 COW 闭环
 7. EEVDF 调度器              — O(n)→O(log n)，公平调度
 8. poll/select               — 多路 I/O，解锁 busybox
 9. SMP 负载均衡              — 多核利用
10. readlink/symlink          — ext2 前置
11. /proc/<pid>/fd/ + /proc/<pid>/maps

P2 (下月):
12. lwIP 网络栈 + E1000       — 第一个网络能力
13. ext2 读写
14. rwlock                    — 读多写少优化
15. 用户栈 canary
16. 更多 busybox applet (grep/sed/find)

P3 (远期):
17. 动态链接器
18. ASLR
19. 多架构 (aarch64)
20. UBSan
21. GUI 框架
22. Alpine apk 用户态
```

---

## 关键设计决策

| # | 决策 | 选择 | 替代方案 | 理由 |
|----|------|------|---------|------|
| 1 | 调度器 | EEVDF (Tilck 路线) | 维持 O(n) RR | Linux 6.6+ 生产级算法，公平性/延迟保障 |
| 2 | 网络栈 | lwIP (cavOS 路线) | 自研 (ArvernOS 路线) | 快速获得 TCP/IP，后期可替换自研模块 |
| 3 | 文件系统 | FAT32 + ext2 (Aquila 路线) | FAT32 only / ext4 | ext2 简洁 (221行)，UNIX 权限/inode 概念完整 |
| 4 | 测试 | Tilck 3 层 (unit+self+sys) | 仅手动测试 | 经过验证的模式，gtest host 测试非 arch 代码 |
| 5 | 用户态 | busybox → 动态链接 → Alpine apk | 全自研用户态 | cavOS 已验证可行，Alpine musl 与 OS01 哲学一致 |
| 6 | 调试 | 分级日志 + strace + hang detector | printk only | ArvernOS + cavOS + Tilck 三合一，已全部落地 |
| 7 | 信号系统 | sigframe→trampoline→sigreturn 完整闭环 | 仅内核态 SIG_DFL 处理 | 用户态 handler 是 UNIX 信号的核心语义，已完成闭环 |
| 8 | setjmp | `[RSP]` 相对寻址，不依赖 frame pointer | `[RBP+8]` (broken) | `-fomit-frame-pointer` 是 busybox 上游默认，libc 必须兼容 |

---

## 附录：开源 OS 项目分析摘要

> 完整分析见 `memory/opensource-os-analysis.md`。项目源码在 `/tmp/{HackOS,tilck,Aquila,ArvernOS,opuntiaOS,cavOS}`。

| 项目 | 语言 | 平台 | 亮点 | 对 OS01 核心价值 |
|------|------|------|------|-----------------|
| **Tilck** | C | i686+riscv64 | EEVDF调度器、3层测试、hang detector、Linux ABI兼容 | 调度器设计、测试体系、诊断工具 |
| **cavOS** | C | x86_64 | Alpine apk用户态、lwIP网络栈、内核strace、动态链接 | 网络栈、strace、用户态终极方案 |
| **Aquila** | C | x86 | ext2 R/W (221行)、完整VFS、6文件系统 | ext2 驱动范本 |
| **ArvernOS** | C | x86_64+aarch32+aarch64 | 多架构抽象、模块化日志、自研网络协议栈 | 多架构、debug日志、网络学习 |
| **opuntiaOS** | C/C++/ObjC | x86+ARMv7+ARM64 | Window Server GUI、Compositor、IPC框架 | GUI 架构远期参考 |
| **HackOS** | C | x86_64 | 可缩放字体、VESA图形、window manager框架 | 字体渲染参考 |

### 各项目技术栈对照

| 特性 | OS01 | Tilck | cavOS | Aquila | ArvernOS | opuntiaOS |
|------|------|-------|-------|--------|----------|-----------|
| 调度器 | RR+优先级 | EEVDF | 抢占式 | 多线程 | 抢占式 | 抢占式 |
| 文件系统 | FAT32, devfs, procfs | ramfs, devfs, FAT32, sysfs | FAT32, ext2, /proc, /sys, /dev | ext2, tmpfs, devfs, procfs, devpts | — | ext2, /dev, /proc |
| 网络 | ❌ | ❌ | lwIP TCP/IP | socket骨架 | 自研 ARP→UDP | local sockets |
| 测试 | 单元测试骨架 | gtest+self+sys | — | — | 单元测试 | benchtests |
| GUI | ❌ | FB console | Xorg | fbterm | ❌ | Window Server |
| 多架构 | x86_64 | i686+riscv64 | x86_64 | x86 | x86_64+aarch32/64 | x86+ARMv7+ARM64 |
| 用户态 | busybox(9) | busybox+vim+tcc | Alpine apk | aqbox+tcc+lua | homemade | C++ GUI apps |
| Syscall | ~45 (+ Linux ABI) | ~100 (Linux兼容) | ~200+ (Linux兼容) | POSIX | homemade | POSIX |

## 各项目详细深度分析

> 项目源码已 clone 至 `/tmp/{HackOS,tilck,Aquila,ArvernOS,opuntiaOS,cavOS}`，
> 完整尽职调查见 `memory/opensource-os-analysis.md`。
> 以下为以 OS01 为中心的提炼版。

---

### Tilck — 测试与调度器大师级范本

**规模**: 1,214 files / ~134K LOC (C + ASM + CMake)，i686+riscv64+ x86_64(build-only)

**三层测试体系** (最值得抄的部分):
- `tests/unit/` — gtest 在 host 上编译运行非 arch 内核代码 (mock `mocking.h` 模式)
- `tests/self/` — 内核内 selftest (link 进内核，QEMU 自动运行)
- `tests/system/` — 用户态通过 syscall 测试内核行为
- `tests/runners/lib/qemu.py` — Python QEMU 自动化框架，支持 sendkey/截屏/timeout
- CI: CircleCI，`debug_build` + `release_build` + clang + syscc 四条 pipeline
- 代码覆盖率: gcov → Codecov 上传

**EEVDF 调度器** (最值得研究的算法):
- CFS 风格 AVL 树可运行队列，键: `(vruntime, tid)`
- `VRUNTIME_SCALE=16` 子刻度精度 — 整数运算法则浮点精度
- `min_vruntime` 单调高水位 + `WAKEUP_VRUNTIME_BONUS` 反饿死
- `sum_vruntime_in_tree` 增量维护，用于 `avg_vruntime = (sum + curr.vruntime) / (N+1)`
- eligibility check: `vruntime_i ≤ avg_vruntime`，eligible 中选最小 deadline
- `SCHED_LATENCY_TICKS` / `MIN_GRANULARITY_TICKS` 动态切片
- 独立 `se_eevdf.c` selftest (3线程 × 0.5s，验证 slice 边界 + vruntime 推进)
- `se_fairness.c` selftest — 长时间公平性验证
- 完整 `docs/scheduler.md` 文档

**Linux ABI 兼容**:
- ~100 Linux syscall，直接运行未经修改的 busybox (static musl 编译)
- `gcc-musl` toolchain from bootlin.com — 无需自研用户态工具链
- `/bin/sh`, `vi` (vim-tiny), `tcc` compiler 均可运行

**模块化设计**:
- `modules/` 目录: acpi, console, debugpanel, e1000, fb, kb8042, pci, sb16, serial, sysfs, systests, tracing
- CMake `ExternalProject` 模式: kernel、gtests、modules 各自独立编译
- Kconfig 风格配置系统: `tilck_option()` → mconf 菜单式配置

**诊断工具**:
- Debug Panel TUI: 内置 `dp_*` 命令 (task list, memory stats, syscall trace)
- Hang detector: 检测调度停滞 → dump 所有 task + wait objects
- `KRN_TRACE_SYSCALLS` 编译选项 → 实时 syscall trace

**对 OS01 的直接可借鉴点**:
1. 测试框架 3 层架构（当前 OS01 只有 `tests/run_test.py` boot test）
2. AVL 树实现 (`kernel/bintree/`) → 可运行队列 + PID 管理
3. 模块编译系统 → OS01 可拆 `kernel/` 为独立模块
4. `mocking.h` host 测试模式 → OS01 `test/` 目录可扩展
5. `debugpanel` TUI → 替代 `serial_printk` 调试

---

### cavOS — 用户态终极方案 + 网络栈

**规模**: 763 files / ~164K LOC (C)，最大的分析对象

**Alpine apk 用户态** (最大亮点):
- 直接用 Alpine Linux 的 `apk` 包管理器 + musl libc
- 用户态工具: xorg-server, mpv, thunar, netsurf, quake, doom, htop, bash...
- 这意味着 ~200+ Linux syscall 需要兼容
- lwIP 集成: `src/kernel/networking/lwip/` — 完整的 TCP/IP 栈

**网络栈**:
- lwIP 作为内核网络栈 (非用户态库)
- E1000、RTL8139/RTL8169 驱动
- TCP、DHCP、Unix domain sockets
- 截图显示: HexChat IRC (TLS 加密)、NetSurf 浏览器

**内核 strace**:
- 实时打印每次 syscall 名称 + 参数
- 分组文件: `syscalls_clock.c`, `syscalls_env.c`, `syscalls_fs.c`, `syscalls_mem.c`, `syscalls_net.c`, `syscalls_proc.c`, `syscalls_sig.c`, `syscalls_poll.c`

**IPC 丰富度**:
- `pipe.c`, `futex.c`, `eventfd.c`, `signals.c`, `unixSocket.c`, `poll.c`
- 两路管道、AF_UNIX sockets、eventfd、futex (用户态同步原语)

**Xorg GUI**:
- `src/kernel/graphical/fb.c` — /dev/fb0 暴露 framebuffer
- PSF1 字体终端
- xorg-server 直接运行在 cavOS 上

**动态链接**:
- ELF64 动态链接器 + musl libc 共享库

**构建**:
- Limine bootloader (现代 UEFI/BIOS 双支持)
- `tools/toolchain/` 自建工具链脚本

**对 OS01 的直接可借鉴点**:
1. lwIP 集成方式 — 直接嵌入内核而非用户态库
2. Linux syscall 兼容层组织 (`syscalls_*.c` 分组)
3. 内核 strace 实现
4. Alpine apk 用户态路线图 — OS01 的 P3 终极目标

---

### Aquila — ext2 驱动最佳范本

**规模**: 482 files / ~47K LOC (C + asm)，x86 32-bit

**ext2 文件系统** (核心价值):
- `kernel/fs/ext2/` 仅 1,201 行 (7 个文件)
  - `ext2.c` (221 行) — superblock/挂载
  - `inode.c` (185 行) — inode 读/写/分配
  - `vops.c` (336 行) — VFS 操作绑定 (read/write/lookup/truncate)
  - `dentry.c` (131 行) — 目录项操作
  - `block.c` (97 行) — 块读写 + 间接块
  - `super.c` (6 行) — superblock 释放
  - `ext2.h` (225 行) — 结构体定义
- R/W 支持: inode 分配/释放、block bitmap、间接块
- 逐块边界对齐读取 — 减少 `kmalloc` 临时缓冲区

**VFS 架构**:
- `vnode_t` 核心: ino, mode, nlink, ctime/atime/mtime, refcount
- `vref()`/`vrele()` 引用计数
- `vcache` (vnode cache) — 路径解析缓存

**6 文件系统**: ext2, tmpfs, devfs, procfs, devpts, initramfs (CPIO)

**用户态**:
- aqbox (类 busybox)、fbterm (framebuffer 终端)、kilo (编辑器)、tcc (C 编译器)、lua
- newlib-3.0.0 C 库

**POSIX 信号**: `kernel/sys/signal.c` + `kernel/include/sys/signal.h`

**对 OS01 的直接可借鉴点**:
1. ext2 驱动 — 直接参考 `kernel/fs/ext2/` 代码结构
2. VFS vnode refcounting 模式
3. VFS + 多个文件系统挂载的完整实现
4. devpts (伪终端) 实现

---

### ArvernOS — 多架构抽象 + 模块化日志

**规模**: 1,671 files / ~25K LOC (C)，最精炼的项目

**多架构支持**:
- Tier 1: x86_64 (generic)
- Tier 2: aarch32 (Raspberry Pi 2), aarch64 (Raspberry Pi 3)
- `src/kernel/arch/x86_64/`, `aarch32/`, `aarch64/` 三个目录
- `arch/` 接口: mmu, proc, logging, register

**模块化调试日志** (最值得抄):
- `ENABLE_CONFIG_DEBUG=1`, `ENABLE_CORE_DEBUG=1`, `ENABLE_FS_DEBUG=1`, `ENABLE_MMU_DEBUG=1`, `ENABLE_NET_DEBUG=1`, `ENABLE_PROC_DEBUG=1`, `ENABLE_SYS_DEBUG=1`, `ENABLE_USERLAND_DEBUG=1`
- `make run-debug ENABLE_FS_DEBUG=1` — 命令行控制，无需重新编译
- 日志输出到 `COM1` 串口 → `./log/` 文件
- `DEBUG` / `INFO` / `WARN` / `ERROR` 四级

**自研网络协议栈**:
- `src/kernel/net/`: arp.c, dhcp.c, dns.c, ethernet.c, icmpv4.c, ipv4.c, net.c, ntp.c, udp.c
- 总共仅 1,067 行 — 教学级实现
- UDP + DHCP + DNS + NTP 完整链路

**文档**:
- Doxygen 生成完整 API 文档 (`docs/` 已生成 HTML)
- 每个子系统有 README

**构建**:
- Docker 开发环境 (`Dockerfile` + `willdurand/arvernos-toolchain` image)
- CircleCI + 单元测试
- clang-format 自动格式化 (`make fmt`)

**用户态**:
- homemade libc/libk (共享代码)
- shell, cat, clear, date, hostname, meminfo, init 等 ~10 个程序
- 配置系统: INI 格式解析 (`inish.ebnf` 语法定义 + `inish.c` 解析器)

**对 OS01 的直接可借鉴点**:
1. `ENABLE_*_DEBUG=1` 通道机制 — OS01 的 debug 日志基础设施
2. `arch/` 多架构接口抽象 — 为将来 aarch64 做准备
3. 自研网络栈的教学价值 — 1,067 行完整 ARP→UDP→DHCP
4. `tools/fix-stacktrace.py` — addr2line 符号解析脚本
5. ini 配置解析器 → OS01 的 `/etc/init.ini` 启动配置

---

### opuntiaOS — GUI 框架远期参考

**规模**: 1,197 files / ~70K LOC (C/C++/ObjC)，x86+ARMv7+ARM64

**完整 GUI 栈**:
- Window Server (Compositor): `userland/servers/window_server/`
  - Desktop + Mobile 双 target
  - WindowFrame, IPC 通道
- LibG (图形库): 2D 渲染、字体、图像
- LibUI (UI 控件库): button, label, text field, scroll view
- LibFoundation: NSObject-like 基础类

**多架构**: x86, x86_64, ARMv7, ARM64, riscv64 — 5 个架构

**自定义 bootloader**:
- `boot/libboot/` 共享代码
- `boot/x86/`, `boot/x86_64/`, `boot/arm32/`, `boot/arm64/`, `boot/riscv64/`
- 内核校验 (security) + 自定义 device tree

**构建系统**:
- CMake 多级构建 (`build/kernel/`, `build/libs/`, `build/userland/`)
- Python 构建脚本 + 配置系统
- clang-format-16 强制格式化

**LibC + LibCxx + LibObjC**:
- 三语运行时支持 + POSIX 兼容层

**对 OS01 的直接可借鉴点**:
1. Window Server + Compositor 架构设计 — P3 GUI 目标
2. 多架构 bootloader 共享代码模式
3. LibG/LibUI 分层设计
4. 设备树 (device tree) 引导方式

---

### HackOS — 字体渲染 + 双引导

**规模**: 482 files / ~43K LOC (C + C++)，x86_64

**双引导 (BIOS + UEFI)**:
- `boot/BIOS/` — 3 阶段: MBR → stage_1 → stage_2
  - stage_2 中切换到 long mode 并设置 identity paging
- `boot/UEFI/` — gnu-efi 链
  - 单个 Makefile 切换 `TARGET_BIOS=true/false`

**可缩放字体渲染器**:
- `kernel/src/lib/font/font.h` — 字体数据
- `kernel/src/drivers/screen/` — framebuffer + 屏幕驱动
- VESA 图形模式 (1080p)
- 字体文件加载 (PSF 格式)

**ACPI 支持**:
- `kernel/src/acpi/`: rsdp, fadt, dsdt, madt + apic/madt
- PCI 枚举 (`pci.cpp`, `pci_descriptor.cpp`)

**用户态**:
- `libs/libc/` — 自研 libc (C++)，包含 syscall 接口
- crt0, crti, crtn 启动文件
- 数据结构: bitmap, vector, link (链表)

**构建**:
- GNU Make 多级
- 内核有两个 linker script: `link_BIOS.ld` + `link_EFI.ld`
- 内核 `.cpp` 文件 — C++ 编写 (较少见)

**对 OS01 的直接可借鉴点**:
1. PSF 字体加载 + framebuffer 渲染管线
2. 双 linker script 模式 (BIOS/EFI)
3. ACPI PCI 枚举代码 (MADT 已实现，PCI 是下一步)
4. C++ 内核编写风格参考 (异常处理、bitmap 类)

---

## 优先级对照矩阵

| 功能 | 最佳参考 | 复杂度 | 对 OS01 影响 | 建议优先级 |
|------|---------|--------|-------------|-----------|
| 测试框架升级 | Tilck | 中 | 高 — 质量保障基石 | 🔴 P0 (单元测试 ✅) |
| 模块化 debug 日志 | ArvernOS | 低 | 高 — 调试效率质变 | 🔴 P0 (channel ✅, 级别/符号待完成) |
| Hang detector | Tilck | 低 | 高 — 防死锁 | ✅ 已完成 |
| ext2 只读驱动 | Aquila | 中 | 高 — UNIX 文件系统核心 | 🟡 P1 |
| EEVDF 调度器 | Tilck | 高 | 高 — 生产级调度 | 🟡 P1 |
| 内核 strace | cavOS | 低 | 中 — syscall 调试 | ✅ 已完成 |
| mmap/mprotect | Aquila | 中 | 高 — COW 前置依赖 | 🟡 P1 |
| lwIP 网络栈 | cavOS | 高 | 高 — 首个网络能力 | 🟢 P2 |
| 自研网络栈 (教学) | ArvernOS | 中 | 中 — 理解协议 | 🟢 P2 |
| 多架构抽象 | ArvernOS | 高 | 中 — 扩展性 | 🟢 P2 |
| 动态链接器 | cavOS | 高 | 中 — 用户态基础 | 🔵 P3 |
| GUI 框架 | opuntiaOS | 极高 | 低 — 远期目标 | 🔵 P3 |
| 可缩放字体 | HackOS | 中 | 低 — GUI 前置 | 🔵 P3 |
| Alpine apk 用户态 | cavOS | 极高 | 极高 — 终极目标 | 🔵 P3 |

---

## 已完成 (2026-07-05)

| 项目 | 工作量 | 状态 |
|------|--------|------|
| 模块化 debug 日志 (`debug_<channel>()` 宏) | 1 天 | ✅ |
| 内置 selftest 框架 (5 subsystems) | 2 天 | ✅ |
| Hang detector (watchdog per CPU) | 半天 | ✅ |
| 内核级 strace (`DEBUG_CHANNELS=syscall`) | 1 天 | ✅ |
| `sigprocmask` 完整实现 (Linux ABI 映射) | 半天 | ✅ |
| stacktrace 符号解析 (`kallsyms` + `backtrace()`) | 已有 | ✅ |
| 构建系统升级 (`-MMD` + 分离 build 目录 + wildcard) | 1 天 | ✅ |
| 信号 handler 用户态投递 (sigframe→trampoline→sigreturn) | 2 天 | ✅ |
| `sigreturn` trampoline (`sigreturn_trampoline.S`) | 半天 | ✅ |
| `rt_sigaction` SA_RESTORER (libc auto-restorer) | 半天 | ✅ |
| systest 系统测试 (70 passed, 0 failed) | 1 天 | ✅ |
| setjmp `-fomit-frame-pointer` 修复 | 1 天 | ✅ |
| busybox ash `#` shell 正常交互 | — | ✅ |

## 下一个改进建议

### 🔴 P0: COW fork + 4KB 页面

**重要性**: 当前 `fork_mm_copy` 急切复制 2MB 页，fork+exec 模式中每次 fork 延迟 ~100ms 但 exec 立即丢弃。改用 COW 后共享页表+标记只读，延迟降到 ~1ms — **当前最大单点性能瓶颈**。

**借鉴**: Tilck COW fault handler + Linux `copy_page_range`

**工作量**: 3-5 天

**实现步骤**:
1. 实现 4KB 页面分配（当前仅 2MB 大页，PDE→PTE 映射）
2. `fork_mm_copy` 改为：共享 PML4/PDPT/PDE，标记页表只读，`mm_t.refcount++`
3. `do_page_fault` 检测 Write-Cause + COW 标记 → 分配新 4KB 页 → 复制 → remap R/W

**验证**: `fork(); exec();` 循环计时对比，70/70 systest 保持通过

### 🔴 P0: 内核栈 canary (`-fstack-protector`)

**借鉴**: Linux kernel `__stack_chk_guard` + `__stack_chk_fail`

**工作量**: ~30 分钟

**实现**:
1. 在 `-static` 链接时提供 `__stack_chk_guard` (随机值) 和 `__stack_chk_fail` (panic)
2. 添加 `-fstack-protector-strong` 到 kernel CFLAGS
3. 验证：写一个栈溢出测试 → canary 触发 → panic 而非静默破坏

**收益**: 防止栈缓冲区溢出导致的内核崩溃 — 当前最严重的一类 bug

### 🟡 P1: mmap/mprotect syscall

**借鉴**: Linux `mmap` + Aquila VMM

**工作量**: 1-2 天

**重要性**: COW 的前置依赖，同时解锁 busybox grep/sed/find 等核心 applet（它们依赖 mmap 做文件 I/O）

**实现**:
1. `SYS_mmap` — 匿名映射 (fd=-1) 和文件映射 (fd>=0)
2. `SYS_mprotect` — 页权限修改
3. Linux ABI 映射 (nr 9→mmap, nr 10→mprotect)

### 🟡 P1: 日志级别 (ERR/WARN/INFO/DEBUG)

**借鉴**: ArvernOS 四级日志 + Tilck DEBUG_PANEL

**工作量**: ~1 天

**实现**:
1. `log_level()` 宏族：`log_err()`, `log_warn()`, `log_info()`, `log_dbg()`
2. 全局 `LOG_LEVEL` 编译时开关 — 低于此级别的代码被 `if(0)` 消除
3. 渐进迁移现有 `printk` 调用 → 分级日志

**收益**: 调试效率质变 — 当前所有 `printk` 平等，无法按需开启/关闭
