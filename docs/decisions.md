# 设计决策总账（Key Design Decisions）

> 归集 OS01 演进过程中的关键架构与实现决策。每条带编号，供各子系统文档交叉引用。
> 原始记录来自 `docs/roadmap.md` 的「关键设计决策」表，按主题分区整理，便于检索。

---

## 内存与 COW

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 1 | COW 粒度 | 4KB-only V1（2MB 推迟） | 2MB split 子页风险过高；4KB COW 已捕获 fork+exec 主要收益 |
| 2 | COW 引用计数 | `subpage_pool.cow_count[512]` | 无需改 struct Page |
| 8 | COW PTE 标记 | `PAGE_COW` (bit 10) | bit 9 已被 `PAGE_PROTNONE` 占用 |
| 9 | COW 并发保护 | `subpage_lock` (已有 spinlock) | 无需新增锁 |

## 调度与 SMP

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 3 | 调度器 | EEVDF (Tilck 路线) | Linux 6.6+ 生产级算法 |
| 24 | SMP 负载均衡指标 | `nr_running` 为主 + `min_vruntime` tiebreaker | 避免 min_vruntime 单调递增语义导致的虚假"最忙"判断（1 CPU-bound vs 10 interactive） |
| 25 | SMP 窃取方向 | rbtree 尾部（最大 deadline） | CPU-bound 任务高 vruntime→自然迁移到空闲核；交互式任务留在源核 |
| 26 | SMP 振荡防护 | 门控 `src > local + 1` + count = `(diff)/2` | O(log N) 收敛，无 2:1↔1:2 ping-pong |
| 27 | SMP 前置条件 | 6 项全局无锁代码加固 | slab/PMM/softirq/timer/fork_mm_copy/task_wake — AP 运行用户程序前必须 SMP 安全 |
| 30 | 任务回收机制（**候选，暂缓**） | wait 驱动回收：do_waitpid 直接收割，删 schedule() 内 zombie reaper 扫描 + deferred_free | 贴 Linux 语义；当前 reaper 是 double-book 竞态温床；触发条件：频繁改任务退出/父子语义或加 exit_group 时实施。详见 `docs/scheduler-complexity.md` §3 🔴 |

## 网络与文件系统

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 4 | 网络栈 | 内核态 lwIP 2.2.1 + 自有 socket syscall 层 | TCP/UDP/DHCP/DNS 与 poll/select 已贯通；下一步补自动化网络回归和 TLS |
| 5 | 文件系统 | FAT32 + ext2 R/W + tmpfs + devfs + procfs | ext2 读写完整，UNIX 权限完整 |
| 11 | 磁盘布局 | GPT 双分区 (FAT32 ESP + ext2 root) | UEFI 标准，内核自解析 GPT |

## 测试、用户态与抽象层

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 6 | 测试 | Tilck 3 层 (unit+self+sys) | 126/126 systest pass |
| 7 | 用户态 | busybox → 动态链接 → Alpine apk | cavOS 已验证可行 |
| 10 | 内核栈 canary | 全局 `__stack_chk_guard` (`arch_cycle_counter()` 种子) | clang 生成 RIP-relative全局引用，无需 TLS/FS |
| 12 | 子系统注册模式 | 运行时 `register_subsys()` | 不引入 ELF section 依赖 |
| 13 | 多架构 dispatch | `arch/*.h` 用 `#ifdef __x86_64__` / `#elif __aarch64__` | ISP 在 include 层解决 |
| 14 | VT100 CSI 终端模拟器 | `console_putchar()` state machine (~330 行) | framebuffer 与终端分离 |
| 15 | TTY ioctl (TCGETS/TCSETS) | tty_ioctl() 映射 ICANON/ECHO ↔ termios | ash 可切 raw mode |
| 16 | 键盘 VT100 转义序列 | K_UP/DOWN/LEFT/RIGHT → ESC [ A/B/C/D | canonical 模式静默丢弃 |
| 28 | inittab 格式 | `id:action:process`（3 字段，冒号分界，open+read 直接解析） | 去掉 runlevel；ACT_* 位掩码 0x01..0x80 与 run_actions() dispatch 兼容 |
| 29 | OS01_SYSTEST 切换 | Makefile cp 模板（config/inittab.systest），非编译期 #ifdef | 同一 init 二进制同时支持测试/正常模式 |

## I/O 多路复用（poll/select）

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 17 | poll/select 多路复用 | poll_table + 双队列 (task/poll 分离) + 级联唤醒 | POSIX 合规：POLLHUP/POLLERR 强制上报，EINTR on any signal |
| 18 | pipe 阻塞语义 | wait_queue_t 替代 busy-loop schedule() | 双队列：read_wait/write_wait (task) + read_poll/write_poll (poll entry) |
| 19 | poll 超时 | jiffies + PIT 100Hz callback → wake_all(poll_wq) | 级联唤醒路径无冗余扫描 |
| 20 | QEMU 设备模型 | `-drive` + `ahci/ide-hd` 替代 `-hda` | 与 GPT 分区表 + AHCI 驱动一致 |
| 21 | select/pselect 适配层 | `do_poll_core` 共享 + `do_select_common` 去重 | poll 和 select 共享轮询循环；select/pselect 共享 fd_set↔pollfd 转换 |
| 22 | poll_table 动态化 | `entries[]` → `*entries + max_entries` | select 支持 1024 fd，poll 保持 16 fd 不变 |
| 23 | pselect6 sigmask 原子性 | goto-out 模式 + `mask_swapped` 标志 | 所有错误路径恢复 blocked；nfds==0 路径也做 atomic swap |
| 38 | poll 注册方向语义 | requested & legal & unavailable 才注册（poll_requested_read/write 分类器）；readiness 只由 open mode 决定，绝不因 requested bits 创造就绪 | 修复复合 flags（O_RDWR 管道误报方向）、PTY 双注册容量（每 fd 2 槽）、socket POLLOUT 吞 POLLIN 注册 |
| 39 | poll 超时最终扫描 | 超时路径 cleanup 后 `poll_scan(kfds, nfds, NULL)` 只读扫描 | 超时返回前不恢复 current_poll_wq，避免最后一拍唤醒被丢 |
| 40 | DHCP ACD | `LWIP_DHCP_DOES_ACD_CHECK=0` | QEMU user-mode NAT 单 guest 无地址冲突可能；ACD ~10.6s PROBE/ANNOUNCE 竞态 nettest 10s 等待，偶发不绑定 |

## 定时器（Timer 重构）

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 41 | tick 源 | LAPIC 周期模式接管，PIT 掩蔽 + 未校准回退 | QEMU TCG IOAPIC edge 无沿检测 → PIT 200Hz 伪影；LAPIC LVT 本地投递免疫，PIT 先跑保证 boot 窗口 |
| 42 | 精粒度时间 | clocksource 纳秒（TSC, mult/shift）与 tick 解耦 | clock_gettime/nanosleep/poll/select 需亚 tick 分辨率；粗超时（EEVDF/watchdog/lwIP/AHCI）保持 jiffies |
| 43 | TSC 频率校准 | CPUID15h + RTC PIE 联合校准（PIE 结果优先复用，AP 复用 BSP 状态） | 250ms PIE 窗口比 TSC 窗口更稳；跨核只做握手采样粗偏移修正 |
| 44 | mult/shift 计算 | `__uint128_t` 中间值 + shift 上限放宽（>1GHz 需 shift>31） | 高频 TSC（≥4.3GHz）需 shift≥35，64 位中间值溢出（496a210 实证） |
| 45 | per-LAPIC DIV | `lapic_timer_start` 每 CPU 写 `LAPIC_TIMER_DIV` | AP 复位后 DIV=÷1，静态值 ÷2 折算 → AP 200Hz（QEMU 双核 298Hz 实证） |
| 46 | aarch64 时钟 | clocksource/clockevent 接口 hook 预留（cntvct_el0 + CNTP），不写 ARM 代码 | YAGNI，避免未验证代码；随 aarch64 启动实施 |

## Tetris / 图形

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 31 | tetris 输入 | `/dev/keyboard` 原始扫描码 | tty TCSETS no-op；扫描码自解析 E0 前缀 + release 位（方向键 E0 48/4B/4D/50） |
| 32 | tetris 渲染 | fb mmap 像素块（20×10 × 30px） | per-VMA 多进程共享；FBIOSURRENDER 只停内核 console |
| 33 | 终端恢复协议 | Linux alt screen `\e[?1049h/l` | 标准化、零轮询零 waitpid；vim/less 可复用 |
| 34 | terminal.elf 双缓冲 | offscreen 主缓冲 + alt 缓冲 | 退出后内容 100% 一致、恢复零重绘 |
| 35 | 游戏启动 | 手工 `exec /bin/tetris` | 不进 inittab |
| 36 | serial 渲染后端 | ❌ 不并入 | 38400 baud 增量重绘可行但投入产出比低；收敛范围 |
| 37 | nanosleep 修复 | B 事件驱动（wakeup_jiffies + PIT 扫描）优先；A 最小可用 fallback | 睡眠无唤醒源（tetris 卡死实证）；连带排查 signal 假醒（sleep 1 不睡） |
