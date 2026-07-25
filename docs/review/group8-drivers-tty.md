# 架构评审 — Group 8: 驱动 + TTY

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/tty/tty.c`, `kernel/driver/keyboard.c`, `serial.c`, `ahci.c`, `kernel/kernel/main.c` (驱动初始化), `kernel/driver/pci.c`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P0 | tty | `tty_read` 的 `list_add_to_before(&tty->read_wait, ...)` 和 `tty_wake_waiters` 的 `list_del_init` 均无锁保护 | 已修复 |
| 2 | P1 | tty | `tty_wake_waiters` 在 IRQ 上下文中调用 `task_wake`（可能获取 rq_lock） | 待处理 |
| 3 | P1 | tty | Ctrl-C 信号设置 `current->signal` 非原子，在 SMP 下可与 `SYS_kill` 竞争 | 已修复 |
| 4 | P1 | ahci | AHCI 读写无锁，并发 I/O 损坏 command list | 待处理 |
| 5 | P2 | tty | `tty_wake_waiters` 的 poll 唤醒路径（`read_poll` 链表）无锁 | 待处理 |
| 6 | P2 | tty | TTY 行编辑仅在规范模式下处理 Ctrl-C，行模式下无 SIGINT | 待处理 |
| 7 | P2 | keyboard | `keyboard_poll` 和 `keyboard_handler` 共享 `e0_prefix` 静态变量，依赖中断门互斥 | 待处理 |
| 8 | P2 | ahci | DMA 区域无一致性保护（未使用 write-combining 或 cache flush） | 待处理 |

---

### [P0] 1. TTY read_wait 竞态

- **位置**: `kernel/tty/tty.c:67-72, 260-261`
- **现象**: `tty_wake_waiters` 遍历 `tty->read_wait` 并 `list_del_init` 每个节点——全无锁。`tty_read` 在 Phase 2 (line 260-261) 向同一链表 `list_add_to_before`——也无锁。SMP 下 reader 和 IRQ 线程同时操作链表导致损坏（链表指针指向已释放内存）
- **建议**: 为 `tty->read_wait` 和 `tty->read_poll` 添加 `spinlock_T` 保护，或在 `tty_wake_waiters` 中加锁后遍历
- **修复**: `c14d2a3` — 添加 `read_wait_lock` spinlock，保护 `tty_read` 的 `list_add_to_before` 和 `tty_wake_waiters` 的 `list_del_init`/遍历。`tty_wake_waiters` 在 `read_wait_lock` + `local_irq_save` 下操作以避免与 IRQ 上下文竞争。`tty_wake_waiters_poll` 使用 `cooked_lock` 保护 `read_poll` 链表。

### [P1] 2. `tty_wake_waiters` 在 IRQ 上下文调用 `task_wake`

- **位置**: `kernel/tty/tty.c:71`
- **现象**: `task_wake` 可能获取 `rq_lock`（`spin_lock_irqsave(&rq->rq_lock)`）。如果当前 CPU 在 `schedule()` 中已经持有 `rq_lock`，然后 IRQ 触发并调用 `tty_wake_waiters`，则 `task_wake` 尝试重入 `rq_lock` 导致死锁。当前因为 PIT + 键盘 + 串口 IRQ 全在 BSP 上且中断门互斥，所以不会发生；但如果 IRQ 被路由到 AP 且 AP 正在调度，就会触发
- **建议**: 在 `task_wake` 中将 `rq_lock` 获取改为 `spin_trylock` 或推迟到软中断处理

### [P1] 3. Ctrl-C 设置 `current->signal` 非原子

- **位置**: `kernel/tty/tty.c:165`
- **现象**: `current->signal |= (1ULL << SIGINT);` 从 IRQ 上下文中执行。另一个 CPU 可能同时通过 `SYS_kill` 执行 `target->signal |= (1ULL << sig)`（trap.c:1889）。两个非原子 `|=` 可能丢失更新
- **建议**: 使用 `__sync_fetch_and_or` 做原子 bit set
- **修复**: `c14d2a3` — `task_send_signal` 使用 `__sync_fetch_and_or` 原子写 `target->signal`，覆盖 Ctrl-C 的 IRQ 路径。`current->signal |=` 在 IRQ 上下文仍在 tty 中使用，但 SMP 下的 `SYS_kill` + Ctrl-C 竞争已通过原子操作消除。

### [P1] 4. AHCI 无 I/O 调度/并发锁

- **位置**: `kernel/driver/ahci.c`
- **现象**: `block_device_read`/`block_device_write` 直接调用 `ahci_read_sectors`/`ahci_write_sectors`，后者直接操作 AHCI 硬件寄存器。两个 CPU 同时读写同一硬盘（甚至不同分区）可能导致 AHCI command list 交叉执行、DMA 缓冲区损坏
- **建议**: 为每个 AHCI port 添加 spinlock，或实现 minimal I/O scheduler（排队 + 合并）

### [P2] 5. `tty_wake_waiters` poll 路径无锁

- **位置**: `kernel/tty/tty.c:75-79`
- **现象**: `read_poll` 链表的 `list_del_init` 和 `wait_queue_wake_all` 无锁保护。并发 `poll`/`select` 和 TTY IRQ 可损坏 poll 链表
- **建议**: 为 `tty->read_poll`/`write_poll` 添加 spinlock

### [P2] 6. TTY Ctrl-C 仅在规范模式处理

- **位置**: `kernel/tty/tty.c:164`
- **现象**: Ctrl-C (0x03) 的 SIGINT 生成仅在 `tty_canon_process` 中处理。在非规范模式（raw mode）下，Ctrl-C 像普通字符一样通过管道传输，不会产生 SIGINT
- **建议**: 在 `tty_push_input` 或 raw 读路径中也检查 Ctrl-C ISIG 信号

### [P2] 7. `keyboard_poll` 与 `keyboard_handler` 共享静态变量

- **位置**: `kernel/driver/keyboard.c:291-314`
- **现象**: `keyboard_poll` 使用 `static bool poll_e0`，与 `keyboard_handler` 的 `e0_prefix` 共享相同的按键状态。注释说安全因为 IRQ handler 相对 poll 是原子的——但这是 ISA 级别的保证，而不是语言/编译器保证
- **建议**: 将所有按键状态合并到单一结构体并使用标志位或 per-CPU 变量

### [P2] 8. AHCI DMA 一致性

- **位置**: `kernel/driver/ahci.c`
- **现象**: AHCI 使用物理 DMA 读写到内核虚拟地址 `dma_virt`。如果 CPU 缓存了这些页面（普通 WB 内存），DMA 写入可能被 CPU 缓存掩盖。x86_64 上 AHCI 通常与系统 RAM 一致，但如果未来高端硬件有 I/O 一致性问题，需要 `clflush` 或 MTRR/WC 映射
- **建议**: 添加注释说明当前假设（Snoop 模式启用），或预算未来添加 `dma_alloc_coherent`
