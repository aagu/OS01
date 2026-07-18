# OS01 优化路线图 v9

> **基准**: `1aaf2ee` (poll/select 完成 — 18 commits, 19 files, +875/−114)
> **日期**: 2026-07-18

标记: ✅ 已完成 | 🔴 P0 本迭代 | 🟡 P1 本月 | 🟢 P2 下月 | 🔵 P3 远期

---

## 当前状态总览 (7 个 Phase 全部就绪)

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork, mmap/mprotect/munmap, demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、多架构清理（aarch64 桩到位）、SMP (percpu+GS-base)、canary、hang detector、debug channels、kallsyms、FPU 保存 | ✅ |
| **Phase 3: 信号 + 调度** | do_signal_delivery、Ctrl-C→SIGINT、systest 78/78、优先级 O(n) 调度器 | ✅ |
| **Phase 4: 文件系统** | ext2 只读、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+raw mode TTY）、9 applet、init、libc (printf/malloc/string/syscall wrapper)、VT100 CSI 终端模拟器 | ✅ |
| **Phase 7: poll/select** | poll_table + 双队列级联唤醒、pipe wait queue 重构、TTY poll、devfs poll 转发、PIT 超时、systest 78/78 | ✅ |

---

## 待实施优先级

```
P0 (本迭代 — 本月):
 1. EEVDF 调度器              — O(n)→O(log n)，公平调度，反饿死
 2. ext2 读写                 — 完成文件系统故事，/ 可写

P1 (本月):
 3. lwIP 网络栈 + E1000       — 第一个网络能力 (socket poll 回调已就绪)
 4. 多架构 aarch64 ✅ 清理     — 8 dispatch 头文件 + 7 aarch64 桩 + task.c 拆分（20 commits）
 5. /proc/<pid>/fd/ + maps   — 进程级调试
 6. 日志级别 ✅               — ERR/WARN/INFO/DEBUG，开发效率质变
 7. rwlock/seqlock            — VFS /proc 多核缩放
 8. 用户栈 canary             — 用户态加固

P2 (下月):
 9. 更多 busybox applet      — grep/sed/find (依赖 poll/select ✅ + regex)
10. select() 系统调用          — fd_set → pollfd 适配层 (do_poll 已就绪)
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
  poll/select ✅ ──→ 更多 busybox applet ──→ 动态链接器
       │
       └─────→ lwIP 网络栈 (socket poll 回调 ✅)
       
  ext2 只读 ──→ ext2 读写 ──→ readlink/symlink
       
  rwlock ──→ VFS 多核缩放 (VFS lookup, /proc read)
```

---

## Phase 解析

### P0: 本迭代 (预计 2-3 周)

#### 1. EEVDF 公平调度器 [借鉴 Tilck] 🟡 P1

**动机:** 当前 `schedule()` 扫描全局链表 O(n)，`max-counter + priority` 策略无公平性保证。EEVDF 是 Linux 6.6+ 的生产级算法。

| 任务 | 工作量 | 说明 |
|------|--------|------|
| AVL/rbtree 可运行队列 | 1-2 天 | 替代 O(n) 链表扫描 |
| vruntime 累积 + min_vruntime | 1 天 | 子刻度精度 |
| eligibility + deadline 选择 | 1 天 | `vruntime ≤ avg_vruntime` 且 deadline 最早 |
| **收益:** | | 反饿死 + 响应时间可预测 + 高负载下不减速 |

#### 2. ext2 读写驱动 🟡 P2

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

#### 3. lwIP 网络栈 + E1000 驱动 🟡 P1

**动机:** 最大的"缺失功能"。网络解锁 telnet/ssh/http/nslookup 等无限可能性。poll/select 就绪为 socket poll 提供了直接的支持。

| 任务 | 工作量 | 借鉴 |
|------|--------|------|
| lwIP 移植到内核 | 1 天 | cavOS 的开箱即用方案 |
| E1000 PCI NIC 驱动 | 2 天 | QEMU `e1000` 虚拟网卡 |
| socket syscall 层 | 1-2 天 | `UserSocket` + `lwip_send`/`lwip_recv` |
| poll/select 网络集成 | 半天 | socket poll 回调 — `fd_poll(FD_SOCKET)` 直接复用 |

**收益:** 网络栈是从"玩具 OS"到"可用 OS"的最大一步。

#### 4. 多架构 aarch64 🟡 P1

**状态:** 基础清理完成。ARCH 参数化 + 8 个 dispatch 头文件 + task.c 拆分 + 7 个 aarch64 桩到位。

| 任务 | 借鉴 | 说明 |
|------|------|------|
| `arch/aarch64/spinlock.h` 实现 | — | ldxr/stlxr 独占循环 |
| `arch/aarch64/linkage.h` | — | ENTRY 汇编宏 |
| `kernel/arch/aarch64/head.S` | ArvernOS | 启动入口 |
| 中断控制器 (GIC) | opuntiaOS | GICv2/v3 驱动 |
| 时钟/定时器 | opuntiaOS | Generic Timer |
| MMU 初始化 | ArvernOS | TTBR0_EL1 设置 |

#### 5. /proc 完善 🟡 P1

| 任务 | 工作量 | 说明 |
|------|--------|------|
| `/proc/<pid>/fd/` | 半天 | 查看打开的文件描述符 |
| `/proc/<pid>/maps` | 半天 | 查看 VMA 布局 |
| `/proc/<pid>/status` 扩展 | 半天 | signal mask、ppid、utime/stime |

#### 6. rwlock / seqlock 🟡 P2

| 任务 | 工作量 | 说明 |
|------|--------|------|
| rwlock 实现 | 半天 | 读共享/写独占 |
| VFS lookup 路径加锁 | 半天 | `/proc` 多核缩放 |

#### 7. 用户栈 canary 🟡 P1

| 任务 | 工作量 | 说明 |
|------|--------|------|
| libc 编译 `-fstack-protector-strong` | 半天 | 用户态 SSP |
| ELF 加载器传递 canary 种子 | 半天 | AT_RANDOM aux vector |

---

### P2: 下月

#### 8. 更多 busybox applet 🟢 P2

> poll/select ✅ → 阻塞条件已满足。

| 任务 | 阻塞项 | 说明 |
|------|--------|------|
| grep/sed | regex | 从 musl 移植 regcomp/regexec |
| find | glob/fnmatch | libc 新增 `glob()`/`fnmatch()` |
| awk | regex + FPU | regex + 浮点数解析 |
| vi | TTY termios + SIGWINCH | 完整终端控制 |

#### 9. select() 系统调用 🟢 P2

| 任务 | 工作量 | 说明 |
|------|--------|------|
| do_select() 适配层 | 1 小时 | `fd_set → pollfd[] → do_poll() → revents → fd_set` |
| `sys_select.h` + libc | 半小时 | SYS_select(23) 已有编号，替换 -ENOSYS stub |

#### 10. aarch64 启动 🟢 P2

见 P1 #4 的剩余工作。

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

---

### P3: 远期

| # | 任务 | 借鉴 | 说明 |
|---|------|------|------|
| 13 | ASLR | — | 随机化加载基址 |
| 14 | SMP 负载均衡 | Tilck EEVDF | push-pull / work stealing |
| 15 | UBSan + KASan | ArvernOS | 内核地址消毒 |
| 16 | 真机 USB 启动 | Tilck | "不能在真机上测试就别实现" |
| 17 | AF_UNIX sockets | cavOS | 本地 IPC |
| 18 | GUI 框架 | opuntiaOS + HackOS | Window Server + Compositor |
| 19 | Alpine apk 用户态 | cavOS | musl 二进制包 |
| 20 | NVMe 驱动 | — | 替代 AHCI |

---

## poll/select 实施总结

### 架构

```
               libc poll(fds,n,to)
                      │ SYS_poll (48)
              ┌───────▼────────┐
              │   do_poll()    │  poll_table_t (栈)
              │  poll_table    │  .wq + .entries[16]
              └──┬───┬───┬─────┘
                 │   │   │
      ┌──────────┘   │   └──────────┐
      ▼              ▼              ▼
  ┌────────┐   ┌────────┐   ┌──────────┐
  │ pipe_t │   │ tty_t  │   │ devfs    │
  │.r_wait │   │.r_wait │   │.poll()   │
  │.r_poll │   │.r_poll │   │→tty_poll │
  │.w_wait │   │cooked  │   └──────────┘
  │.w_poll │   │_lock   │
  └────────┘   └────────┘
```

- **双队列**: `*_wait` (wait_queue_t, 直接阻塞 task) + `*_poll` (list_t, poll entry)
- **级联唤醒**: fd 就绪 → walk `*_poll` → `wake_all(e->poll_wq)` → 重扫所有 fd → 返回 revents
- **POSIX 合规**: POLLHUP/POLLERR 即使未请求也填入 revents；任意未阻塞信号中断返回 EINTR

### 文件变更

| 类别 | 文件 | 行数 |
|------|------|------|
| **新增** | `kernel/include/kernel/poll.h` | 82 |
| **新增** | `kernel/fs/poll.c` | 319 |
| **新增** | `libc/unistd/poll.c` | 16 |
| **修改** | `kernel/include/kernel/file.h` — pipe_t 4 新字段 | +8 |
| **修改** | `kernel/fs/file.c` — pipe wait queue + wake 路径 | +137/−50 |
| **修改** | `kernel/include/kernel/tty.h` — read_poll + tty_poll 原型 | +8 |
| **修改** | `kernel/tty/tty.c` — 双队列 wake + tty_poll() | +51 |
| **修改** | `kernel/fs/devfs.c` — poll 回调 + devfs_poll() | +67 |
| **修改** | `kernel/include/fs/devfs.h` — API + devfs_poll 声明 | +12 |
| **修改** | `kernel/arch/x86_64/trap.c` — syscall dispatch + names | +22 |
| **修改** | `kernel/include/uapi/syscall.h` — SYS_poll/ppoll/select | +5 |
| **修改** | `kernel/driver/pit.c` — poll 超时回调 | +10 |
| **修改** | `kernel/kernel/main.c` — devfs poll 参数 | +2 |
| **修改** | `libc/include/poll.h` — event flags 对齐内核 | +15 |
| **修改** | `libc/include/sys/syscall.h` — syscall 编号 | +3 |
| **修改** | `libc/unistd/busybox_stubs.c` — 删除假 poll | −39 |
| **修改** | `user/systest.c` — 8 poll 测试用例 | +86 |
| **修复** | `kernel/include/kernel/wait.h` — 消除循环 include | −1 |
| **修复** | `kernel/futex.c` — 显式 include task.h | +1 |
| **修复** | `Makefile` — run/run-kvm/debug 统一 AHCI | +14/−2 |

**总计: 18 commits, 19 files, +875 / −114**

### systest 结果: 78/78 passed (poll 8/8)

---

## 开源 OS 项目借鉴 — 下一步行动

| 项目 | 已经用到的 | 还可以拿来的 |
|------|----------|-------------|
| **Tilck** | EEVDF 调度思路、3 层测试、hang detector | EEVDF 代码结构、load balancing、GDB helper |
| **cavOS** | lwIP 网络栈参考、动态链接、Alpine apk 路线 | socket syscall 层、E1000 驱动、动态链接器加载流程 |
| **Aquila** | — | **ext2 R/W 核心 (~221 行)**: inode/block alloc+free |
| **ArvernOS** | 多架构抽象思路、aarch64 dispatch 桩模式 | 分层日志系统、UBSan、aarch64 head.S/GIC/Generic Timer |
| **opuntiaOS** | devman 子系统注册框架 | GICv2 驱动、Generic Timer、Window Server GUI |
| **HackOS** | — | 可缩放字体渲染器、VESA 图形模式 |

---

## 已完成汇总 (截至 2026-07-18)

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
| **poll/select syscall** (poll_table + 双队列 wake + pipe/tty/devfs + PIT timeout + systest 78/78) | 2 天 | 07-18 |
| Makefile QEMU targets 统一 AHCI (run/run-kvm/debug) | 10 分钟 | 07-18 |

---

## 关键设计决策

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 1 | COW 粒度 | 4KB-only V1（2MB 推迟） | 2MB split 子页风险过高；4KB COW 已捕获 fork+exec 主要收益 |
| 2 | COW 引用计数 | `subpage_pool.cow_count[512]` | 无需改 struct Page |
| 3 | 调度器 | EEVDF (Tilck 路线) | Linux 6.6+ 生产级算法 |
| 4 | 网络栈 | lwIP (cavOS 路线) | 快速获得 TCP/IP，socket poll 回调已就绪 |
| 5 | 文件系统 | FAT32 + ext2 (只读) + tmpfs + devfs + procfs | ext2 只读 370 行，UNIX 权限完整 |
| 6 | 测试 | Tilck 3 层 (unit+self+sys) | 78/78 systest pass |
| 7 | 用户态 | busybox → 动态链接 → Alpine apk | cavOS 已验证可行 |
| 8 | COW PTE 标记 | `PAGE_COW` (bit 10) | bit 9 已被 `PAGE_PROTNONE` 占用 |
| 9 | COW 并发保护 | `subpage_lock` (已有 spinlock) | 无需新增锁 |
| 10 | 内核栈 canary | 全局 `__stack_chk_guard` (rdtsc 种子) + `-fstack-protector-strong` | clang 生成 RIP-relative 全局引用，无需 TLS/FS |
| 11 | 磁盘布局 | GPT 双分区 (FAT32 ESP + ext2 root) | UEFI 标准，内核自解析 GPT |
| 12 | 子系统注册模式 | 运行时 `register_subsys()` | 不引入 ELF section 依赖 |
| 13 | 多架构 dispatch | `arch/*.h` 用 `#ifdef __x86_64__`/`#elif __aarch64__` | ISP 在 include 层解决 |
| 14 | VT100 CSI 终端模拟器 | `console_putchar()` state machine (~330 行) | framebuffer 与终端分离 |
| 15 | TTY ioctl (TCGETS/TCSETS) | tty_ioctl() 映射 ICANON/ECHO ↔ termios | ash 可切 raw mode |
| 16 | 键盘 VT100 转义序列 | K_UP/DOWN/LEFT/RIGHT → ESC [ A/B/C/D | canonical 模式静默丢弃 |
| 17 | poll/select 多路复用 | poll_table + 双队列 (task/poll 分离) + 级联唤醒 | POSIX 合规：POLLHUP/POLLERR 强制上报，EINTR on any signal |
| 18 | pipe 阻塞语义 | wait_queue_t 替代 busy-loop schedule() | 双队列：read_wait/write_wait (task) + read_poll/write_poll (poll entry) |
| 19 | poll 超时 | jiffies + PIT 100Hz callback → wake_all(poll_wq) | 级联唤醒路径无冗余扫描 |
| 20 | QEMU 设备模型 | `-drive` + `ahci/ide-hd` 替代 `-hda` | 与 GPT 分区表 + AHCI 驱动一致 |
