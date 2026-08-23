# 已完成工作汇总（Changelog）

> OS01 各阶段已完成工作的按时间汇总。最新在前（截至 2026-08-18）。
> 本表为历史完成记录，规划项见 `docs/roadmap.md`。

---

## 2026-08-17 ~ 08-18

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **timer 重构** (clocksource+clockevent 双层抽象 + TSC/RTC-PIE 联合校准 + LAPIC tick 接管掩 PIT + CLOCK_MONOTONIC/REALTIME/nanosleep/poll 迁纳秒 + jiffies self-test，systest 150/150，根治 PIT 200Hz) | 2 天 | 08-17~18 |

## 2026-08-16

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **网络回归 harness** (make test-network + OS01_TCP_ECHO_DELAY_MS delayed-reply + 20/20 no-delay + 10/10 delay250 cohort) | 1 天 | 08-16 |
| **DHCP ACD 关闭** (LWIP_DHCP_DOES_ACD_CHECK=0，消除 ~10.6s ACD 竞态导致的偶发不绑定) | 1 小时 | 08-16 |
| **requested-event-aware poll/select** (按请求方向注册/唤醒，修复复合 flags + PTY 双注册容量 + 时序敏感 select 断言) | 1 天 | 08-16 |
| **E1000 RX ring 所有权串行化** (tcpip 线程独占硬件 ring 消费，IRQ 仅 ack + wake) | 半天 | 08-16 |
| **俄罗斯方块游戏** (framebuffer 像素渲染 + 扫描码输入 + alt-screen 恢复 + UX) | 1 天 | 08-16 |

## 2026-08-15

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **per-poll timeout registry** (poll_timeout_head 链表 + PIT 扫描，修复 lost-wakeup + 并发 clobber) | 半天 | 08-15 |
| **keyboard poll 支持** (/dev/keyboard 扫描码 wait queue + keyboard_poll_dev) | 半天 | 08-15 |
| **tty termios 诚实化** (TCSETS 真存储 + raw 默认 + ICANON/ECHO) | 半天 | 08-15 |
| **terminal alt-screen 双缓冲** (?1049h/l) | 半天 | 08-15 |
| **/proc/<pid>/fd/ 观测性** (files_t 引用协议 pin/unpin + dup/dup2/fcntl 路由重构 + exit 路径 pin) | 1 天 | 08-15 |
| **arch 边界收紧** (x86 平台源选择、early task-state hook、公共 gate ABI、arch signal API、端口 I/O wrapper) | 1 天 | 08-15 |
| **网络正确性加固** (DNS 超时、端口字节序、部分读缓存、shutdown、UDP readiness、响应 hang) | 3 天 | 08-12~08-15 |
| **lwIP 网络栈合并** (E1000/virtio-net + PCI/MSI-X + DHCP/DNS + TCP/UDP socket + poll/select + HTTP wget) | 多迭代 | 08-15 |

## 2026-08-01

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **inittab 配置支持** (ACT_* 位掩码修复 + parse_inittab() open/read 解析器 + 3 套模板 + Makefile/build 集成 + test-inittab 相位派发验证) | 1 天 | 08-01 |

## 2026-07-25 ~ 07-29

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **SMP 负载均衡** (idle-steal + per-schedule pull + sched_pick_cpu + nr_running 指标 + 振荡防护 + 6 项 SMP 前置条件加固 + init 迁移保护 + idle→idle #PF 修复 + smp_stress 验证) | 2 天 | 07-29 |
| **EEVDF 调度器** (rbtree 可运行队列 + vruntime/deadline + pick_eevdf O(log n) + per-CPU TSS SMP 修复) | 2 天 | 07-25 |

## 2026-07-24

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **select/pselect syscall** (poll_table 动态化 + do_poll_core 提取 + 适配层 + pselect6 sigmask 原子性；当时 systest 118/118，当前 126/126) | 2 天 | 07-24 |

## 2026-07-19

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **ext2 读写** (alloc_block/inode、create/mkdir/rmdir/unlink/rename/truncate、selftest) | 2 天 | 07-19 |
| refactor: 固定数组→堆分配 (VFS name/cwd + mount_table + pipe buf + ext2 buf) | 1 天 | 07-19 |

## 2026-07-18

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **poll syscall** (poll_table + 双队列 wake + pipe/tty/devfs + PIT timeout) | 2 天 | 07-18 |
| Makefile QEMU targets 统一 AHCI (run/run-kvm/debug) | 10 分钟 | 07-18 |

## 2026-07-15 ~ 07-17

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **busybox ash 方向键+行编辑** (VT100 CSI + terminal + FIONREAD) | 2 天 | 07-17 |
| arch/x86_64 头文件引用清理 | 1 小时 | 07-17 |
| **多架构清理收尾** (8 dispatch + 7 aarch64 桩) | 1 天 | 07-15 |

## 2026-07-12

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 日志级别系统 | 5 天 | 07-12 |
| 子系统注册框架 | 半天 | 07-12 |
| arch 通用头文件迁移 | 2 天 | 07-12 |

## 2026-07-11

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 内核栈 canary (SSP) | 30 分钟 | 07-11 |
| SMP 栈溢出修复 (ext2 buf[256]→buf[4096]) | 30 分钟 | 07-11 |
| selftest 10/10 + systest 70/70 修复 | 2 小时 | 07-11 |
| disk.img GPT 双分区 + tools/mkdisk | 1 天 | 07-11 |
| ext2 只读 + GPT + tmpfs + /dev 块设备 | 1 天 | 07-11 |
| VFS mount point getdents 修复 | 半天 | 07-11 |
| **COW fork (4KB-only)** | 2 天 | 07-11 |
| 4KB 页面 + VMA + mmap/mprotect | 3 天 | 07-08 |

## 2026-07-02 ~ 07-05

| 项目 | 工作量 | 日期 |
|------|--------|------|
| systest 70/70 | 1 天 | 07-05 |
| 信号 handler 用户态投递 | 2 天 | 07-05 |
| busybox ash shell + 9 applet | 1 天 | 07-04 |
| FPU/SSE 状态保存 (fxrstor/fsave) | 半天 | 07-03 |
| do_signal_delivery (Ctrl-C→SIGINT) | 1 天 | 07-03 |
| syscall + signal 框架 (systest 43 syscall) | 2 天 | 07-02 |
