# 架构评审 — Group 6: 同步原语

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/futex.c`, `kernel/mutex.c`, `kernel/intr/wait.c`, `kernel/fs/file.c` (pipe_wake_*), `kernel/include/kernel/mutex.h`, `kernel/include/kernel/wait.h`, `kernel/include/kernel/arch/x86_64/spinlock.h`, `kernel/fs/poll.c`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P1 | mutex | `mutex_lock` CAS 失败后到加入 wq 之间有 lost-wakeup 窗口 | 待处理 |
| ~2~ | ~P1~ | ~futex~ | ~~`do_futex_wait` 加入 wq 后不重检查用户值~~ | ~~FALSE ALARM~~ |
| 2 | P1 | wait | `io_wait_node` 被 wait_queue/futex/tty 共用 — 一个 task 只能在一个队列上等待 | 待处理 |
| 3 | P2 | futex | `do_futex_wait` 的 val 比较在读物理页后、加锁前存在 TOCTOU | 待处理 |
| 4 | P2 | mutex | 不支持递归锁（同线程重入死锁） | 待处理 |
| 5 | P2 | futex | futex bucket 使用 `io_wait_node`，与 tty/pipe 的 wait queue 冲突 | 待处理 |
| 6 | P2 | spinlock | `spin_lock` 无调试模式（deadlock detection / lockdep） | 待处理 |
| 7 | P2 | mutex | `mutex_unlock` 唤醒了错误的等待者（wake_one 而应该是 wake_next_waiter） | 待处理 |

---

### [P1] 1. `mutex_lock` lost-wakeup 竞争

- **位置**: `kernel/mutex.c:12-17`
- **现象**: 标准模式:
  ```c
  while (CAS(&owner, 0, pid) == 0)   // ❶
      wait_queue_sleep(&m->wq);      // ❷ 加 wq → set INTERRUPTIBLE → schedule
  ```
  若 ❶ 失败（owner 被持有），然后 `wait_queue_sleep` 加入 wait queue 并设置 INTERRUPTIBLE。但如果 unlocker 的 `wait_queue_wake_one` 在 ❶ 之后、❷ 的 `list_add` 之前执行（通过 `wq->lock` 竞争），则会发现空列表，不做唤醒。随后 ❷ 加入列表并进入不可中断睡眠——即使 owner 已经变为 0，也无人唤醒此线程
- **建议**: 在 `wait_queue_sleep` 中加入双检模式：在加锁状态中添加 wq → 检查条件（owner==0）→ 如果条件满足，从 wq 移除并返回，不 sleep；否则 set INTERRUPTIBLE + unlock + schedule

### ~~[P1] 2. `do_futex_wait` 加入 wq 后不重检查值~~  FALSE ALARM

- **位置**: `kernel/futex.c:72-77`
- **原始审查**: 认为 `do_futex_wait` 在加入 wq 后、释放锁前应重检查用户值，避免丢失唤醒
- **复核结论**: **FALSE ALARM**。读 `*uaddr` 和 `list_add_to_before` 均在 `bucket->lock` 保护下原子完成。`futex_wake` 也获取同一 `bucket->lock`，因此：
  - 若值在锁外变化，waker 必看到 wq 中有任务 → 唤醒成功
  - 若 waker 看到空 wq（任务尚未加入），则锁内读到的值尚未变化 → 无丢失唤醒
  - 锁消除了检查和加入 wq 之间的竞争窗口
- **修正**: 此条应标记为 FALSE ALARM，不纳入问题清单。

### [P1] 2. `io_wait_node` 被多个子系统共用

- **位置**: `kernel/intr/wait.c:19`, `kernel/futex.c:73`, `kernel/tty/tty.c:261`
- **现象**: `task_t->io_wait_node` 是 task 内唯一的链表节点，同时被 wait_queue_sleep, futex, tty_read 等用于加入各自的等待队列。如果 tty_read 在 futex_wait 执行期间被调用（不会在当前实现中发生，因为 syscall 是互斥的），或一个 task 同时被加入多个 wait queue，链表会被破坏
- **建议**: 为每种等待机制分配独立的节点（或动态分配），或通过断言防止重复加入

### [P2] 3. `do_futex_wait` TOCTOU 窗口

- **位置**: `kernel/futex.c:56-70`
- **现象**: 验证地址和读用户值在**获取 bucket->lock 之前**完成。恶意用户空间可以在验证后、加锁前改变地址映射（例如 munmap + mmap 同一 VA 到不同物理页），导致读取的值与实际不同
- **建议**: 在 bucket->lock 下通过 `user_va_to_phys` 重新读取值（即在锁内再做一次 VA → PA 翻译）

### [P2] 4. `mutex_lock` 不支持递归

- **位置**: `kernel/mutex.c:12-17`
- **现象**: 如果同一 task 重入 `mutex_lock`（即持有锁时再次调用），`CAS` 比较 `owner==0` 失败（owner==当前 pid），导致死循环
- **建议**: 检查 `owner == current->pid` 并递增递归计数，或使用断言检测并 panic

### [P2] 5. futex bucket 与 pipe/tty wait queue 共享 `io_wait_node`

- **位置**: 同上 #3，futex 使用 `current->io_wait_node` 加入 bucket->wq。如果 task 正在 futex 上等待，然后 pipe 读也尝试用相同节点加入 pipe 的等待队列，链表被破坏
- **建议**: futex 使用自己的 `futex_wait_node` 字段，或阻塞期间禁用其他使用 `io_wait_node` 的路径

### [P2] 6. spinlock 无调试/死锁检测

- **位置**: `kernel/include/kernel/arch/x86_64/spinlock.h:18-33`
- **现象**: spinlock 实现是裸的 `lock decq` + 忙等。无 owner 追踪、无递归检测、无 lockdep 风格的依赖图
- **建议**: 在 `#ifdef DEBUG_SPINLOCK` 中增加 owner 记录、持有时间统计、递归检测

### [P2] 7. `mutex_unlock` 唤醒顺序问题

- **位置**: `kernel/mutex.c:24-28`
- **现象**: `mutex_unlock` 总是调用 `wait_queue_wake_one`（唤醒最早等待者）。但在某些场景下（例如 PI mutex），需要唤醒最高优先级的等待者。当前实现正确（FIFO），但无优先级继承
- **建议**: 添加注释说明当前 FIFO 行为，如需要优先级公平性则切换到优先级排序的等待队列
