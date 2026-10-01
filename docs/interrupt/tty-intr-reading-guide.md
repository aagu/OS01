# TTY + 中断底层源码导读（tty/ + intr/ + entry.S）

> 覆盖：键盘到 shell 的完整 I/O 链路，以及它脚下的中断分发层。参考手册见 docs/interrupt.md、AGENTS.md "Interactive shell" 一节。

## 文件清单

```
kernel/tty/    tty.c 481 | pty.c 260 | console.c 102 | canon.c（纯逻辑行规程）
kernel/intr/   irq.c 95 | dispatch.c | softirq.c | arch_irq_hooks.c
               apic/ (acpi, ioapic, ipi, lapic, lapic_timer) | pic/ (8259A.c)
kernel/arch/x86_64/ entry.S 326 | head.S 266 | switch.c 114
```

## 数据流全景（一条按键的旅程）

```
键盘 IRQ → apic/ioapic → arch_irq_dispatch (hook) → driver/keyboard.c
  → tty_push_input (tty.c L140)
  → canon.c 行规程（ICANON 回显/退格/行累积，纯逻辑可单测）
  → tty_read (L202) 阻塞等待者
shell 侧: read() → vfs → tty 层 → 拿到一行 → fork/exec
输出侧: printk → tty_write (L328) → console.c (VT100 CSI) → framebuffer / serial
PTY: terminal.c 把 pty.c 主从两端接到 framebuffer + busybox ash
```

## 推荐阅读顺序

**第 1 遍·中断骨架**（先理解"事件怎么到内核"）：
1. `irq.c` 注册路径：register_irq(gsi) / unregister_irq(uint32_t gsi)（9 月从 vector 改 gsi，与 arch_irq hooks 对齐）
2. `arch_irq_hooks.c` 弱默认 vs `arch/x86_64/intr/irq_hooks.c` 强覆盖（APIC→PIC ladder + 0x20+gsi 翻译 + do_IRQ）——weak default / strong override 模式最典型的实例
3. `dispatch.c` + softirq.c：硬中断上下文最小化，软中断做重活
4. entry.S `ENTRY(ret_from_intr)` (L64)：iretq 前 `get_current_task()` 查 need_resched → schedule()。**这是理解"抢占在哪发生"的关键 40 行**

**第 2 遍·TTY 层**：
1. `canon.c` 全文（~200 行，纯逻辑，ICANON 状态机：accumulate/read 两函数）
2. `tty.c` L140-370：push_input（上半段，IRQ 上下文）→ read（下半段，阻塞+wake）→ write → ioctl（termios）
3. `pty.c`：主从两端如何互为 tty；terminal.c 是最大的消费方
4. `console.c`：VT100 CSI 解析（光标移动/清屏），fb 直接写显存

**第 3 遍·信号联动**：Ctrl-C = keyboard IRQ → tty 层查 termios ISIG → 给前台 pgrp 发 SIGINT（docs/signal.md）→ trap.c 投递。读这条线把 tty 和 signal 两个子系统串起来。

## 已知坑

- `tty_read` 阻塞用的是 blocker 框架（sched），不是 sleep-wakeup 原语——唤醒配对看 tty_wake_waiters (L50)
- entry.S 每个异常 ENTRY 的压栈布局不同（有无 error_code），改动前逐个核对；`dev_not_available` 标注 "need rewrite"
- 注册中断用 gsi 不用 vector（旧文档/老代码里 RTC_PIE_IRQ_VEC 宏已删）
- canon.c 故意做成无副作用纯逻辑，为 hosttests 镜像测试服务——别往里加全局状态

## 自测问题

1. 为什么 do_IRQ 在强覆盖 hook 里而不是 8259A.c？多架构时 aarch64 的对应物是什么？
2. Ctrl-C 从键盘中断到 shell 收到 SIGINT，经过几层？哪一层查前台进程组？
3. ret_from_intr 里查 need_resched 用 `%gs:8`，这个偏移谁定义的？换 CPU 数量/字段顺序会怎样？
