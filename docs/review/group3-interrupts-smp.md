# 架构评审 — Group 3: 中断 + 时钟 + SMP

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/intr/apic/lapic.c`, `ioapic.c`, `ipi.c`, `lapic_timer.c`, `kernel/intr/dispatch.c`, `softirq.c`, `irq.c`, `kernel/driver/pit.c`, `kernel/time/timer.c`, `kernel/arch/x86_64/smp.c`, `kernel/percpu/percpu.c`, `kernel/include/kernel/smp.h`, `kernel/include/kernel/ipi.h`, `kernel/include/kernel/softirq.h`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P1 | softirq | `softirq_status` 是全局变量，SMP 下多 CPU 同时处理会竞争 | 待处理 |
| 2 | P1 | softirq | `do_timer` (TIMER_SIRQ handler) 操作全局 timer list 无锁 | 待处理 |
| 3 | P1 | smp | `smp_boot_aps` 使用 `arch_processor_id==0` 判断跳过，合法 APIC ID 0 的 AP 会被跳过 | 待处理 |
| 4 | P1 | smp | `add_timer`/`del_timer`/`do_timer` 操作全局 timer list 无锁 | 待处理 |
| 5 | P2 | smp | `jiffies++` 在 PIT handler 中非原子，IPI 干扰理论风险 | 待处理 |
| 6 | P2 | smp | `ipi_send` ICR 等待超时后无恢复措施 | 待处理 |
| 7 | P2 | pit | `pit_handler` 在 IRQ 上下文调用 `serial_poll()` + `console_blink_tick()`，增加 tick 延迟 | 待处理 |
| 8 | P2 | lapic | LAPIC timer calibration busy-wait 依赖 PIT jiffies | 待处理 |
| 9 | P2 | smp | `vmm_map_page` 在 `kernel_map` 上的 `tlb_shootdown` 检查无锁保护 | 待处理 |
| 10 | P2 | pic | `pit_handler` 仍通过 PIC controller 接口注册，但已使用 IOAPIC | 待处理 |

---

### [P1] 1. `softirq_status` 全局变量 SMP 竞争

- **位置**: `kernel/intr/softirq.c:5`, `do_softirq:30-41`
- **现象**:
  ```c
  uint64_t softirq_status;    // 全局共享
  void do_softirq() {
      for(i = 0; i < 64 && softirq_status; i++) {
          if(softirq_status & (1 << i)) {
              softirq_vector[i].action(softirq_vector[i].data);
              softirq_status &= ~(1 << i);  // 非原子 RMW!
          }
      }
  }
  ```
  - 当前只有 BSP 的 PIT handler 设置 `softirq_status` (LAPIC timer handler 不设置)，所以实际无竞争
  - 但如果任何 AP 未来调用 `set_softirq_status`，两个 CPU 同时在 `do_softirq` 中 `&=` 会互相清除对方的 bit
- **建议**: 将 `softirq_status` 改为 per-CPU 变量，或添加 `this_cpu()->softirq_pending`

### [P1] 2. TIMER_SIRQ handler (`do_timer`) 操作全局 timer list 无锁

- **位置**: `kernel/time/timer.c:27-49` (do_timer), `58-68` (add_timer), `70-73` (del_timer)
- **现象**: `do_timer` 遍历 `timer_list_head` 链表并调用 timer 回调；`add_timer` 插入链表；均无锁保护。当前只从 BSP softirq 调用，但若未来任何 AP 调用 `add_timer`/`del_timer`，链表会损坏
- **建议**: 添加 `timer_lock` spinlock，在 `do_timer`/`add_timer`/`del_timer` 中持有

### [P1] 3. `smp_boot_aps` 使用 APIC ID==0 作为 skip 条件

- **位置**: `kernel/arch/x86_64/smp.c:182`
- **现象**:
  ```c
  for (uint32_t i = 0; i < num_cpus; i++) {
      if (i == 0) continue;         // skip BSP
      if (!percpu_data[i].arch_processor_id) continue;  // ❸ 错误！0 是合法 APIC ID
  ```
  - `percpu_init` 已通过 MADT `flags & 1` 过滤了 offline 的 CPU
  - 但 `arch_processor_id` 为 0 的 AP（合法但罕见）会被静默跳过
  - `percpu_data[i]` 在 `kernel_main` 的 percpu init loop 中对于 flags&1=0 的 entry 不会被初始化（memset 为零），所以 `!arch_processor_id` 也无法区分"未初始化"和"合法 APIC ID 0"
- **建议**: 改用 `percpu_data[i].online` 或专门的状态标志 `percpu_data[i].valid` 来判断 AP 是否应该启动：
  ```c
  if (i == 0 || !percpu_data[i].online) continue;
  ```
  或者在 `percpu_init` 中设置 `percpu_data[i].flags |= PERCPU_IS_AP`。

### [P1] 4. Timer list 操作无锁

- **位置**: `kernel/time/timer.c:27-73` 全局链表
- **现象**: 同上，#2 已说明。timer list 从初始化到过期回调均无锁保护
- **建议**: 添加全局 `timer_lock`，在 `do_timer`/`add_timer`/`del_timer`/`init_timer` 中加锁

### [P2] 5. `jiffies++` 在 PIT hardirq 中非原子

- **位置**: `kernel/driver/pit.c:27`
- **现象**: `jiffies++` 是读-改-写操作。如果 PIT IRQ 到达时 IPI handler 在同一 CPU 上同时读 `jiffies`（或者在 SMP 上一个 CPU 读 `jiffies` 而 BSP 写），值可能不一致。`jiffies` 是 `volatile uint64_t`，但 `volatile` 不保证原子性
- **建议**: 使用 `atomic_fetch_add` 或确保只有 BSP 写 `jiffies`（当前已是这样），并在 SMP 读取侧确认读一致

### [P2] 6. `ipi_send` ICR 等待超时后无恢复

- **位置**: `kernel/intr/apic/ipi.c:57-63`
- **现象**: 等待 ICR 清除 `ICR_STATUS_PENDING` 的超时循环只有 10000 次迭代。超时后打印 debug 消息但继续发送新 IPI。如果 ICR 实际仍忙，新写入会被忽略
- **建议**: 超时后等待更长时间或返回错误码让调用方重试

### [P2] 7. `pit_handler` 在 IRQ 上下文调用非紧急操作

- **位置**: `kernel/driver/pit.c:47,50`
- **现象**:
  ```c
  serial_poll();           // 轮询串口 — I/O 操作
  console_blink_tick();    // 终端光标闪烁 — 内存操作
  ```
  - `serial_poll()` 每次 tick 都要做 `inb` 检测串口状态。在重负载时 100 Hz 的 I/O 开销累积
  - `console_blink_tick()` 更新光标状态，通常只修改全局变量
- **建议**: 保留 `serial_poll()` 作为 IRQ fallback（注释说明用途），但考虑将 `console_blink_tick()` 移到软中断或仅在有 TTY 活动时触发

### [P2] 8. LAPIC timer calibration busy-wait

- **位置**: `kernel/intr/apic/lapic_timer.c:58-60`
- **现象**: `while (jiffies == start) arch_cpu_pause();` 在 calibration 中忙等 PIT 的 jiffies 递增。最多等待 10ms（一次 PIT tick）。只在启动时执行一次
- **建议**: 无紧迫修复，可考虑添加超时防止意外挂起

### [P2] 9. `vmm_map_page` 在 `kernel_map` 上的 `tlb_shootdown` 无锁

- **位置**: `kernel/memory/vmm.c:48-49`
- **现象**: `if (pagemap == kernel_map && num_cpus > 1) tlb_shootdown();` 假设单线程修改 `kernel_map`。如果两 CPU 同时 `vmm_map_page(kernel_map, ...)`（如同时分配内核内存），PTE 修改和 TLB shootdown 会交叉
- **建议**: 为 `kernel_map` 添加 `kernel_map_lock`

### [P2] 10. `pit_handler` 通过 PIC controller 接口注册但实际使用 IOAPIC

- **位置**: `kernel/driver/pit.c:16-23,56-63`
- **现象**:
  ```c
  hw_int_controller_t pit_controller = {
      .enable = pic_enable,
      .disable = pic_disable, ...
  };
  void pit_init() {
      hw_int_controller_t *ctrl = apic_available()
          ? get_ioapic_controller()
          : &pit_controller;
      register_irq(32, NULL, &pit_handler, 0, ctrl, "pit");
  ```
  - `get_ioapic_controller()` 返回 `ioapic_controller`（真实硬件操作）
  - `pit_controller` 仅在 APIC 不可用时使用（纯 PIC 模式）
  - 但 `pit_controller` 使用了 `pic_enable`/`pic_disable`，这些操作与 IOAPIC 模式无关
  - 代码逻辑正确（选择正确的 controller），但 `pit_controller` 结构体定义和 PIC 函数的引用可以删除以简化
- **建议**: 当 APIC 成为唯一目标时，移除 PIC fallback 代码和 `pit_controller` 结构体
