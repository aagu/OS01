# 定时器系统

本系统实现了完整的定时器系统，包括硬件定时器（PIT）和软件定时器管理。

## 定时器系统架构

定时器系统采用分层设计：

1. **硬件定时器层**：使用可编程间隔定时器（PIT）提供时间基准
2. **时间计数层**：维护系统时间计数器 `jiffies`
3. **定时器管理层**：管理软件定时器的创建、添加、删除和执行
4. **软中断处理层**：使用软中断处理到期的定时器

## 核心组件

### 系统时间计数器

```c
uint64_t volatile jiffies;
```

`jiffies` 是系统时间计数器，每 10ms 递增一次（由 PIT 中断触发）。

### 定时器链表

```c
timer_t timer_list_head;
```

`timer_list_head` 是定时器链表的头节点，所有活跃的定时器都链接到这个链表中。

### 定时器结构体

```c
typedef struct timer {
    struct list_head list;
    void (*func)(void * data);
    void * data;
    uint64_t expire_jiffies;
} timer_t;
```

- `list` - 链表节点，用于链接定时器
- `func` - 定时器到期时执行的回调函数
- `data` - 传递给回调函数的数据
- `expire_jiffies` - 定时器到期的时间（`jiffies` 值）

## 硬件定时器（PIT）

### 功能

* 提供系统时间基准
* 触发定时器中断
* 递增系统时间计数器 `jiffies`

### 实现

位于 `kernel/driver/pit.c` 中，主要组件包括：

* `pit_init` - 初始化 PIT（APIC 可用时使用 IOAPIC，否则使用 PIC）
* `set_frequency` - 设置 PIT 频率
* `pit_handler` - PIT 中断处理函数（递增 jiffies、设置 need_resched、触发软中断）

### 初始化流程

1. 调用 `pit_init` 函数
2. 注册 PIT 中断（IRQ0）
3. 设置 PIT 频率为 100Hz（每秒 100 次中断）

### 中断处理流程

1. PIT 每 10ms 触发一次中断（仅 BSP；AP 使用 LAPIC 定时器做调度）
2. 调用 `pit_handler` 函数
3. 递增系统时间计数器 `jiffies`
4. 设置 `need_resched` 以便调度器在中断返回时切换任务
5. 如果有定时器到期，设置 `TIMER_SIRQ` 软中断
6. `do_timer` 在软中断上下文中遍历链表并执行回调

### LAPIC 定时器（每 CPU 调度时钟）

LAPIC 定时器为每个 CPU 提供独立的调度时钟（100Hz），位于 `kernel/apic/lapic_timer.c`：

* `lapic_timer_init` - 注册 IDT 门并校准（subsys Phase 4）
* `lapic_timer_calibrate` - 用 PIT 作为参考进行校准
* `lapic_timer_start` - 启动周期模式（percpu_init + GS 基址设置后调用）
* `lapic_timer_handler` - 设置 `need_resched` + 发送 EOI（不含 jiffies 递增）

#### 与 PIT 的关系

- **PIT**（仅 BSP）：唯一递增 `jiffies` 的源，触发软中断执行定时器回调，请求调度
- **LAPIC 定时器**（每 CPU）：仅请求调度，不递增 `jiffies`
- `jiffies` 是系统全局时间计数器，仅由 PIT 在 BSP 上维护

## 软件定时器管理

### 功能

* 创建和初始化定时器
* 添加定时器到链表
* 从链表中删除定时器
* 处理到期的定时器

### 实现

位于 `kernel/timer/timer.c` 中，主要函数包括：

* `init_timer` - 初始化定时器
* `create_timer` - 创建定时器
* `add_timer` - 添加定时器到链表
* `del_timer` - 从链表中删除定时器
* `do_timer` - 处理到期的定时器
* `timer_init` - 初始化定时器系统

### 初始化流程

在 `kernel_main` 的子系统框架中（subsys Phase 4）执行：

1. `timer`（必需）→ `timer_init()` — 初始化 jiffies、链表头、注册软中断
2. `pit`（可选）→ `pit_init()` — PIT 100Hz，使用 IOAPIC 或 PIC
3. `lapic-timer`（可选）→ `lapic_timer_init()` — 注册门 + 校准，后续 per-CPU 启动

`register_subsys` 在 `kernel/arch/x86_64/subsys.c` 中定义。`subsys_init_all()` 按 Phase 顺序依次执行。具体流程：

1. 调用 `timer_init` 函数
2. 初始化系统时间计数器 `jiffies` 为 0
3. 初始化定时器链表头
4. 注册定时器软中断处理函数 `do_timer`

### 定时器管理流程

#### 创建定时器

```c
timer_t *timer = create_timer(callback_function, callback_data, expire_time);
```

#### 添加定时器

```c
add_timer(timer);
```

#### 处理到期的定时器

1. PIT 中断触发，递增 `jiffies`
2. 检查定时器链表，判断是否有定时器到期
3. 如果有定时器到期，设置定时器软中断
4. 软中断处理函数 `do_timer` 执行以下操作：
   - 遍历定时器链表
   - 找到所有到期的定时器
   - 从链表中删除到期的定时器
   - 执行定时器的回调函数
   - 继续检查下一个定时器

## 定时器使用示例

### 基本用法

```c
// 定义定时器回调函数
void timer_callback(void *data) {
    color_printk(GREEN, BLACK, "Timer expired! Data: %p\n", data);
    // 执行定时任务
}

// 创建并添加定时器
void setup_timer() {
    // 创建一个 5 秒后到期的定时器
    timer_t *timer = create_timer(timer_callback, NULL, 50); // 50 * 10ms = 500ms
    add_timer(timer);
}
```

### 一次性定时器

```c
void one_shot_timer(void *data) {
    color_printk(GREEN, BLACK, "One-shot timer executed!\n");
    // 定时器执行完成后会自动删除
}

void create_one_shot_timer() {
    timer_t *timer = create_timer(one_shot_timer, NULL, 100); // 1 秒后执行
    add_timer(timer);
}
```

### 周期性定时器

```c
void periodic_timer(void *data) {
    color_printk(GREEN, BLACK, "Periodic timer executed!\n");
    
    // 重新创建并添加定时器，实现周期性执行
    timer_t *timer = create_timer(periodic_timer, NULL, 50); // 500ms 后再次执行
    add_timer(timer);
}

void create_periodic_timer() {
    timer_t *timer = create_timer(periodic_timer, NULL, 50); // 500ms 后首次执行
    add_timer(timer);
}
```

## 定时器链表管理

### 链表结构

定时器链表是一个按到期时间排序的双向链表：

- 链表头：`timer_list_head`
- 链表节点：每个 `timer_t` 结构体中的 `list` 字段
- 排序方式：按 `expire_jiffies` 值从小到大排序

### 添加定时器流程

1. 遍历定时器链表
2. 找到第一个到期时间大于当前定时器的节点
3. 将当前定时器插入到该节点之前
4. 保持链表的排序顺序

### 删除定时器流程

1. 从链表中删除定时器节点
2. 保持链表的完整性

## 软中断处理

定时器系统使用软中断来处理到期的定时器，这样可以：

1. 减少硬中断处理时间
2. 提高系统响应速度
3. 允许在处理定时器时执行更复杂的操作

### 软中断注册

```c
register_softirq(0, &do_timer, NULL);
```

### 软中断触发

当有定时器到期时，在 PIT 中断处理函数中设置软中断状态：

```c
set_softirq_status(TIMER_SIRQ);
```

## 时间管理

### 时间单位

- **jiffies**：系统全局时间计数器，每 10ms 递增一次（仅由 PIT/BSP 维护）
- **毫秒**：1 毫秒 = 0.1 jiffies
- **秒**：1 秒 = 100 jiffies

### 时间转换

```c
// 将毫秒转换为 jiffies
uint64_t ms_to_jiffies(uint64_t ms) {
    return ms / 10;
}

// 将秒转换为 jiffies
uint64_t sec_to_jiffies(uint64_t sec) {
    return sec * 100;
}

// 将 jiffies 转换为毫秒
uint64_t jiffies_to_ms(uint64_t jif) {
    return jif * 10;
}

// 将 jiffies 转换为秒
uint64_t jiffies_to_sec(uint64_t jif) {
    return jif / 100;
}
```

## 代码结构

### 定时器核心

* `kernel/timer/timer.c` - 定时器系统核心实现（`timer_init`, `add_timer`, `del_timer`, `do_timer`）
* `kernel/include/device/timer.h` - 定时器相关头文件

### 硬件定时器

* `kernel/driver/pit.c` - PIT 定时器驱动（`pit_init`, `pit_handler`, `set_frequency`）
* `kernel/include/driver/pit.h` - PIT 定时器驱动头文件
* `kernel/apic/lapic_timer.c` - LAPIC 定时器（`lapic_timer_init`, `lapic_timer_start`, `lapic_timer_handler`）

### 子系统注册

* `kernel/arch/x86_64/subsys.c` - 所有定时器组件注册为 Phase 4 入口

## 性能优化

1. **链表排序**：定时器链表按到期时间排序，减少查找时间
2. **软中断处理**：使用软中断处理定时器，减少硬中断处理时间
3. **批量处理**：一次处理所有到期的定时器，提高效率
4. **内存管理**：使用 `calloc` 和 `free` 管理定时器内存

## 注意事项

1. **定时器回调函数应尽量简短**：回调函数执行时间过长会影响其他定时器的执行
2. **避免在回调函数中创建大量定时器**：可能导致链表过长，影响性能
3. **及时删除不需要的定时器**：避免内存泄漏和不必要的处理
4. **注意定时器的精度**：定时器的精度为 10ms，不适合需要高精度的场景

## 扩展定时器系统

### 提高定时器精度

当前系统已实现的定时方案：

1. **PIT**（100Hz）：系统 `jiffies` 时间源，10ms 精度
2. **LAPIC 定时器**（100Hz）：每 CPU 调度时钟，与 PIT 同频
3. **进一步优化**：可增加 PIT 频率，或将来引入 HPET

### 定时器类型

当前定时器类型（通过回调行为区分）：

1. **一次性定时器**：回调执行后自动从链表删除，不重新添加
2. **周期性定时器**：回调函数中重新调用 `create_timer` + `add_timer`

### 优化定时器管理

可以通过以下方式优化定时器管理：

1. **使用更高效的数据结构**：如时间轮（Time Wheel）或平衡二叉树，提高定时器管理效率
2. **批量处理定时器**：减少链表操作次数
3. **定时器合并**：合并接近的定时器，减少中断处理次数

## 故障排除

### 定时器不执行

可能的原因：

1. **定时器未添加到链表**：确保调用了 `add_timer` 函数
2. **定时器到期时间设置错误**：确保 `expire_jiffies` 设置正确
3. **软中断未触发**：检查 PIT 中断是否正常触发
4. **回调函数有问题**：检查回调函数是否有错误

### 定时器执行多次

可能的原因：

1. **定时器被多次添加**：确保每个定时器只调用一次 `add_timer`
2. **回调函数中重新添加了定时器**：检查回调函数是否意外重新添加了定时器

### 系统时间不准

可能的原因：

1. **PIT 频率设置错误**：检查 `set_frequency` 函数的参数
2. **中断处理延迟**：系统负载过高，导致中断处理延迟

## 总结

系统的定时器系统实现了以下功能：

1. **硬件定时器管理**：使用 PIT 提供系统时间基准
2. **软件定时器管理**：创建、添加、删除和执行定时器
3. **时间管理**：维护系统时间计数器 `jiffies`
4. **软中断处理**：使用软中断处理到期的定时器

定时器系统为操作系统提供了时间管理能力，支持各种定时任务的执行，是操作系统的重要组成部分。