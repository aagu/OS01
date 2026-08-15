# OS01 优化路线图 v14

> **基准**: `840f3a5` (architecture boundary cleanup)
> **日期**: 2026-08-15

标记: ✅ 已完成 | 🔴 P0 本迭代 | 🟡 P1 近期 | 🟢 P2 中期 | 🔵 P3 远期

---

## 当前状态总览 (8 个 Phase 全部就绪)

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork, mmap/mprotect/munmap, demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、x86 平台源隔离、aarch64 dispatch 桩、SMP（percpu+GS-base+AP boot+负载均衡）、canary、hang detector、debug channels、kallsyms、FPU 保存、slab/PMM/softirq/timer SMP 加固 | ✅ |
| **Phase 3: 信号 + 调度** | arch 信号帧投递、Ctrl-C→SIGINT、per-CPU EEVDF rbtree 可运行队列、SMP 负载均衡 | ✅ |
| **Phase 4: 文件系统** | ext2 R/W、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+raw mode TTY）、9 applet、init（/etc/inittab 配置解析、4 阶段引导：SYSINIT→WAIT→ONCE→RESPAWN/ASKFIRST、fallback 硬编码默认）、libc (printf/malloc/string/syscall wrapper)、VT100 CSI 终端模拟器 | ✅ |
| **Phase 7: poll/select** | poll_table + 双队列级联唤醒、select/pselect 系统调用（SYS_select=50 + SYS_pselect6=51）、do_poll_core 共享轮询循环、pselect6 sigmask 原子 swap、systest 126/126 | ✅ |
| **Phase 8: 网络** | lwIP 2.2.1、E1000 + virtio-net、PCI/MSI-X、DHCP/DNS、TCP/UDP socket syscall（52–64）、poll/select 集成、BusyBox HTTP wget | ✅ |

---

## 待实施优先级

```
P0 (本迭代):
 1. 网络回归测试              — TCP/UDP/DNS/DHCP/wget 纳入自动化 QEMU 测试
 2. /proc/<pid>/fd/           — 补齐进程文件描述符可观测性
 3. 任务退出/回收收敛         — wait 驱动回收，移除调度器与 reaper 双重职责

P1 (近期):
 4. rwlock/seqlock            — VFS 与 /proc 多核缩放
 5. 用户栈 canary             — libc SSP + ELF AT_RANDOM
 6. symlink/readlink          — 完整 VFS 软链接语义和 ext2 symlink
 7. 更多 BusyBox applet       — grep/sed/find，先补 regex/fnmatch

P2 (中期):
 8. aarch64 启动              — head.S + MMU + GIC + Generic Timer
 9. 动态链接器                — PT_INTERP + ld.so + 共享 libc
10. HTTPS/TLS                 — 集成现有 mbedTLS 子模块，BusyBox wget HTTPS
11. AF_UNIX/socketpair        — 本地 socket IPC

P3 (远期):
12. ASLR                      — 用户态安全
13. UBSan + KASan             — 运行时 bug 检测
14. 真机启动 (USB)            — 建立硬件验证路径
15. GUI 框架                  — Window Server + compositor
16. Alpine apk 用户态         — musl 二进制包兼容路线
17. NVMe 驱动                 — 替代 AHCI
```

**依赖链:**
```
  poll/select ✅ ──→ lwIP/socket ✅ ──→ 网络回归测试 ──→ HTTPS/TLS
       │
       └─────→ 更多 BusyBox applet ──→ 动态链接器
       
  ext2 R/W ✅ ──→ symlink/readlink
       
  rwlock ──→ VFS 多核缩放 (VFS lookup, /proc read)
```

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
| **修改** | `kernel/timer/timer.c` | timer_lock + do_timer 解锁回调模式 |
| **修改** | `kernel/apic/lapic_timer.c` | AP TIMER_SIRQ + softirq.h include |
| **修改** | `kernel/arch/x86_64/trampoline.S` | wrmsr EDX:EAX + CR4 SSE bits |
| **修改** | `kernel/arch/x86_64/trap.c` | kill_current_user_task RSP switch |
| **新增** | `libc/rbtree/rbtree.c` | rbtree_last、rbtree_prev |
| **新增** | `user/smp_stress.c` | CPU-bound 多进程负载均衡验证 |

**总计: 15+ commits, 12 files, 126/126 systest pass (-smp 2)**

---

## 已完成汇总 (截至 2026-08-15)

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

---

## 开源 OS 项目借鉴 — 下一步行动

| 项目 | 已经用到的 | 还可以拿来的 |
|------|----------|-------------|
| **Tilck** | EEVDF 调度思路、3 层测试、hang detector | EEVDF 代码结构、load balancing、GDB helper |
| **cavOS** | lwIP、socket syscall、E1000、动态链接与 Alpine apk 路线参考 | 动态链接器加载流程、用户态包兼容 |
| **Aquila** | **ext2 R/W 核心 (~221 行)**: inode/block alloc+free | — |
| **ArvernOS** | 多架构抽象思路、aarch64 dispatch 桩模式 | 分层日志系统、UBSan、aarch64 head.S/GIC/Generic Timer |
| **opuntiaOS** | devman 子系统注册框架 | GICv2 驱动、Generic Timer、Window Server GUI |
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
