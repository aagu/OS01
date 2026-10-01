# 驱动程序系统

本系统实现了多种硬件驱动程序，包括键盘、PS/2 鼠标、串口、定时器和实时时钟等。

## 驱动程序架构

系统的驱动程序架构采用分层设计：

1. **硬件访问层**：直接访问硬件寄存器，实现底层硬件操作
2. **驱动核心层**：实现驱动程序的核心逻辑
3. **中断处理层**：处理硬件中断
4. **接口层**：提供给内核其他部分使用的接口

## 核心驱动程序

### 键盘驱动

#### 功能

* 处理键盘中断（PS/2 Scancode Set 1）
* 扫描码 → ASCII 转换（Shift/Ctrl/CapsLock 修饰键）
* **VT100 转义序列映射**：方向键（↑↓←→）、Home、End 在 raw mode 下发 ESC [ A/B/C/D 序列
* 通过 `keyboard_set_tty()` 推送字符到 TTY；raw mode 判断由 `kbd_tty->lflag & TTY_L_ICANON` 控制
* `/dev/keyboard` 可读取原始扫描码

#### 实现

位于 `kernel/driver/keyboard.c` 中，主要组件包括：

* `scancode_tbl[128]` — 基础扫描码→ASCII 映射表
* `ext_scancode_tbl[128]` — 扩展扫描码（E0 前缀）→ASCII 映射表，特殊键码 >0xFF（K_UP=0x100, K_DOWN=0x101, K_LEFT=0x102, K_RIGHT=0x103, K_HOME=0x104, K_END=0x105）
* `translate_and_push` — 扫描码转 ASCII 并推入 TTY；raw mode 展开 VT100 序列，canonical mode 丢弃
* `keyboard_handler` — IRQ1 中断处理
* `keyboard_poll` — task context 轮询（DevFS read 路径）

### i8042 共享控制器层

位于 `kernel/driver/i8042.c`。键盘（0x60/0x64 第一口）与 PS/2 鼠标（aux 口）
共用一个 8042 控制器，所有 `0x64` status 与 `0x60` data 读取必须发生在同一个
`spin_lock_irqsave(&i8042_lock)` 临界区内成对完成，按 status bit5（AUX）判定
字节归属，避免字节被错误的设备消费：

* `i8042_init` — 初始化控制器锁与命令字（keyboard.c/mouse.c 之前调用）
* `i8042_set_kbd_consumer` / `i8042_set_aux_consumer` — 注册两个字节消费者
  （在持锁的回调中执行，不得再访问 i8042 端口或睡眠）
* `i8042_pump` — 排空 output buffer 并按来源分发（IRQ1/IRQ12 handler 与
  task-context poll 共用的唯一入口）；有界排空，卡死的 status line 会放弃
  并交由下次 IRQ 重试
* `i8042_write_controller_cmd` / `i8042_read_command_byte` /
  `i8042_write_command_byte` — 控制器命令字事务（0xD4 前后保持端口访问串行化）
* `i8042_write_aux_byte` — 向 aux 口（鼠标）写命令字节，应答由 mouse 驱动
  通过 `i8042_pump()` 轮询收取

命令事务期间 aux 字节送入鼠标应答状态机，非 aux 字节照常送键盘，因此
键盘输入不会打断鼠标探测，鼠标应答也不会泄漏进 TTY。

### PS/2 鼠标驱动（/dev/mouse）

#### 功能

* 探测 aux 口鼠标并协商滚轮模式（magic knock：`0xF3 200/100/50` + `0xF2` 取 ID）
* IRQ12 数据流 → 3/4 字节包解析 → 归一化事件发布
* `/dev/mouse` 暴露固定 8 字节事件流，支持非阻塞读（`EAGAIN`）与 `poll`

#### 实现

* `kernel/driver/mouse.c` — 驱动主体（探测状态机、事件 ring、devfs read/poll）
* `kernel/driver/mouse_proto.c` — 纯 C 包解析器（含 9 位位移边界、Y 取反、
  溢出位、失步重新对齐；对 host 测试开放）
* `kernel/include/uapi/mouse.h` — 用户态事件 ABI（内核与用户程序共用）

#### 探测时序上限（`mouse_init`）

`mouse_init` 在 subsys Phase 6 以 OPTIONAL 注册，失败仅打 SKIP 日志、不注册
`/dev/mouse`。整个探测有硬性时间上限，保证无设备/无响应鼠标不拖慢启动：

| 阶段 | 预算 |
|------|------|
| 主动探测（0xFF 复位 → 0xAA/0x00 → 0xF6 → 滚轮协商 → 0xF4） | 400 ms |
| F5（disable reporting）有界回收尝试 | 最迟 460 ms |
| 回滚控制器写（command byte）与命令字恢复预留 | 各 20 ms |
| **总上限** | **500 ms** |

失败路径回滚顺序：清 command byte bit1（禁 aux IRQ）→ `unregister_irq(12)` →
摘除 aux consumer → 复位包解析状态机；不注册任何 devfs 节点。该路径由
host mock 测试覆盖（可控端口流模拟无响应设备，见
`.superpowers/sdd/2026-09-27-ps2-mouse-driver/task-7-report.md`）。

#### /dev/mouse 事件 ABI

事件固定 8 字节（`mouse_event_t`，`_Static_assert(sizeof == 8)`），一次读
`size < 8` 返回 `EINVAL`；`size ≥ 8` 时只返回整数个完整事件（非 8 倍数按
整事件截断）；无事件时返回 `EAGAIN`：

| 字段 | 类型 | 含义 |
|------|------|------|
| `buttons` | `uint8_t` | bit0=左键 bit1=右键 bit2=中键，其余位为 0 |
| `wheel` | `int8_t` | 滚轮增量（基础鼠标恒为 0） |
| `dx` | `int16_t` | 相对位移，正值向右 |
| `dy` | `int16_t` | 相对位移，正值向下 |
| `reserved` | `uint16_t` | 写 0，用户态忽略 |

内核侧事件 ring 容量 64，满时覆盖最旧事件并累计 `dropped_events` 计数。
`poll(POLLIN)` 空队列时挂入事件锁保护的等待链，发布事件时在锁内摘链唤醒。
用户态测试程序：`user/mousetest.c`（安装为 `/bin/mousetest`）。

### 串口驱动

#### 功能

* 初始化串口
* 读取串口数据
* 写入串口数据

#### 实现

位于 `kernel/driver/serial.c` 中，主要函数包括：

* `init_serial` - 初始化串口
* `serial_received` - 检查串口是否有数据
* `read_serial` - 读取串口数据
* `is_transmit_empty` - 检查串口发送缓冲区是否为空
* `write_serial` - 写入串口数据

#### 初始化流程

1. 禁用所有中断
2. 启用 DLAB（设置波特率除数）
3. 设置波特率为 38400
4. 配置数据格式：8 位数据，无校验，1 位停止位
5. 启用 FIFO，清除缓冲区，设置 14 字节阈值
6. 启用 IRQ，设置 RTS/DSR

#### 使用方法

```c
// 初始化串口
init_serial();

// 写入数据
write_serial('H');
write_serial('e');
write_serial('l');
write_serial('l');
write_serial('o');
write_serial('\n');

// 读取数据（阻塞）
char c = read_serial();
```

### 定时器驱动（PIT）

#### 功能

* 初始化可编程间隔定时器（PIT）
* 设置定时器频率
* 处理定时器中断
* 维护系统时间

#### 实现

位于 `kernel/driver/pit.c` 中，主要组件包括：

* `pit_controller` - PIT 中断控制器
* `pit_handler` - PIT 中断处理函数
* `pit_init` - PIT 初始化函数
* `set_frequency` - 设置 PIT 频率

#### 初始化流程

1. 调用 `pit_init` 函数
2. 注册 PIT 中断（IRQ0）
3. 设置中断处理函数 `pit_handler`
4. 设置 PIT 频率为 100Hz（每秒 100 次中断）

#### 中断处理流程

1. PIT 触发中断
2. 调用 `pit_handler` 函数
3. 增加系统时间计数器 `jiffies`
4. 检查定时器列表，触发到期的定时器
5. 设置定时器软中断

### 实时时钟驱动（RTC）

#### 功能

* 读取实时时钟数据
* 写入实时时钟数据
* 转换 BCD 格式数据

#### 实现

位于 `kernel/driver/rtc.c` 中，主要函数包括：

* `is_updating_rtc` - 检查 RTC 是否正在更新
* `get_rtc_register` - 读取 RTC 寄存器
* `set_rtc_register` - 写入 RTC 寄存器
* `rtc_read_datetime` - 读取 RTC 日期时间
* `rtc_write_datetime` - 写入 RTC 日期时间

#### 数据结构

```c
typedef struct {
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} datetime_t;
```

#### 使用方法

```c
// 读取 RTC 时间
datetime_t dt;
rtc_read_datetime(&dt);

// 写入 RTC 时间
datetime_t new_dt = {2024, 12, 31, 23, 59, 59};
rtc_write_datetime(&new_dt);
```

### AHCI SATA 驱动

位于 `kernel/driver/ahci.c`：

* `ahci_init` - PCI 扫描 SATA 控制器，启用总线主控和 MMIO
* `ahci_port_init` - 初始化端口（命令列表、FIS、命令表），发送 IDENTIFY
* `ahci_read_sectors` / `ahci_write_sectors` - 扇区级读写

初始化流程：
1. PCI 查找类=0x01（大容量存储）、子类=0x06（SATA）、编程接口=0x01（AHCI）
2. 启用总线主控和 MMIO，读取 ABAR（BAR5）
3. 映射 MMIO 空间，读版本和功能寄存器
4. BIOS/OS 所有权交接（BOH）
5. 初始化每个已实现的端口 → 注册为块设备

### 块设备层

位于 `kernel/block/blockdev.c`：

* `block_device_register` - 注册 AHCI 端口为块设备（hda, hdb, ...）
* `block_device_register_raw` - 无需 AHCI 包装的注册
* `block_device_read` / `block_device_write` - 扇区级读写（含边界检查）

### 帧缓冲驱动

帧缓冲在 `kernel/core/main.c` 中初始化：

* `frame_buffer_early_init` - 从 bootinfo 读取帧缓冲基址和分辨率
* `frame_buffer_init` - 将帧缓冲重映射到 `VIRT_FRAMEBUFFER_OFFSET`
* 在 devfs 中注册为 `/dev/fb`（`devfs_register_chrdev("fb", ...)`）

### gfx0 设备（`/dev/gfx0`，2026-09-30）

受限 present 设备，提供用户态 2D 图形 API（libgfx）的内核后端。文件位于 `kernel/driver/gfx.c`，ABI 定义在 `kernel/include/uapi/gfx.h`。

**职责**：

- 维护最多 16 项视图表（`g_gfx_table.slot[GFX_MAX_VIEWS=16]`），spinlock 保护分配 / 配置 / 释放
- `GFX_CREATE_VIEW`：校验矩形（减法形式溢出 `x ≤ fb_w && w ≤ fb_w - x`），分配视图槽
- `GFX_GET_INFO`：返回视图的 `width / height / stride / format`
- `GFX_PRESENT`：快照视图矩形 → 释放锁 → 逐行 `syscall_check_user_range` + `copy_from_user_ft` → `fb_write_row`；中途中断返回 `-EFAULT`，前面行可能已更新（spec §4）
- `release_file`：`file_put` 引用为 0 时清槽并释放视图结构
- **不提供 mmap 回调**：用户像素缓冲始终由用户提供，不通过 mmap 映射

**并发模型**：

- 视图表锁只覆盖分配 / 配置 / 释放；**不能跨 `syscall_check_user_range` 或 fault-tolerant copy 持有**（后者可能 longjmp，spinlock 持锁 longjmp 会泄漏）
- `copy_*_ft` 在 fault 时 longjmp 到调用方；调用方确保 spinlock 已释放
- `dup` / `fork` 共享 `file_t` 引用计数，最后 `file_put` 触发 release

**限制（spec §2 / §4 明示）**：

- 没有 owner / 委托 / compositor：任何进程可继续打开 `/dev/fb`，重叠视图按 present 顺序覆盖
- 无 vsync / page-flip / 硬件双缓冲
- 仅 RGB32 host-endian，无 alpha blend
- 视图表固定 16 项（超出需动态扩表）
- 关闭 path 不支持（仅 `file_put` 触发 release）

详见 `docs/gui/gui.md` 第 2 节、`.superpowers/sdd/2026-09-30-2d-graphics-api/`。

### TTY 驱动

位于 `kernel/tty/tty.c` + `kernel/tty/console.c`，头文件 `kernel/include/tty/tty.h` + `kernel/include/tty/console.h`：

* `tty_alloc` — 分配 TTY 实例，设置输出/回显回调
* `tty_push_input` — IRQ 上下文中推送字符（来自 keyboard/serial 处理程序）
* `tty_read` — 读取 TTY（规范模式行缓冲或 raw mode 逐字节；阻塞支持）
* `tty_write` — 写入 TTY 输出（路由到 output_char 回调，默认为 `tty_def_output → color_printk + serial`）
* `tty_ioctl` — 处理 TCGETS/TCSETS/TCSETSW/TCSETSF（ICANON/ECHO 映射，ISIG 强制启用）和 FIONREAD（可读字节数）
* `get_dev_tty` / `tty_set_dev_tty` — dev_tty 单例管理（从 devfs.c 迁移至 tty.c 统一管理）

#### 软件终端 (console.c)

新增 VT100 CSI 终端模拟器，位于 TTY write 路径和 framebuffer 之间：

* `console_putchar(char c)` — TTY 输出回调，CSI 状态机解析 VT100 序列并渲染字符
* `console_init()` — 初始化终端光标位置和状态（在 `kernel_main` 末尾调用）
* `console_blink_tick()` — PIT 100Hz 回调驱动光标下划线闪烁（800ms 周期，IRQ 上下文安全）

**支持的 CSI 序列 (v1):**
| 序列 | 语义 |
|------|------|
| `ESC [ D` / `ESC [ n D` | 光标左移 |
| `ESC [ C` / `ESC [ n C` | 光标右移 |
| `ESC [ K` | 清到行尾 |
| `ESC [ ?25h` / `ESC [ ?25l` | 显示/隐藏光标 |
| `ESC [ n ; m` | SGR 参数分隔（静默忽略） |

### 键盘 devfs 读处理

键盘驱动在 `kernel/core/main.c` 中注册到 devfs：

```c
devfs_register_chrdev("keyboard", NULL, keyboard_devfs_read, NULL);
```

`keyboard_devfs_read` 允许用户空间程序直接从 `/dev/keyboard` 读取键盘扫描码。

### 鼠标 devfs 注册

鼠标驱动同样在 `kernel/core/main.c` 中注册，但仅在探测成功时：

```c
if (subsys_status("mouse") == 1)
    devfs_register_chrdev("mouse", NULL, &mouse_ops);
```

`mouse_ops` 提供 `read`（8 字节事件流）与 `poll`（事件 ring 非空即 `POLLIN`）。

## 中断控制器

系统使用 `hw_int_controller_t` 结构体表示硬件中断控制器，为驱动程序提供统一的中断控制接口：

```c
typedef struct hw_int_type {
    void (*enable)(uint64_t irq);
    void (*disable)(uint64_t irq);
    uint64_t (*install)(uint64_t irq, void* arg);
    void (*uninstall)(uint64_t irq);
    void (*ack)(uint64_t irq);
} hw_int_controller_t;
```

### 预定义控制器

* `keyboard_controller` - 键盘中断控制器（APIC 可用时使用 IOAPIC）
* `pit_controller` - PIT 中断控制器（APIC 可用时使用 IOAPIC）

## 硬件访问

系统使用以下函数进行硬件访问：

* `inb` - 从 8 位端口读取数据
* `outb` - 向 8 位端口写入数据
* `inw` - 从 16 位端口读取数据
* `outw` - 向 16 位端口写入数据
* `inl` - 从 32 位端口读取数据
* `outl` - 向 32 位端口写入数据

这些函数定义在 `kernel/arch/x86_64/hw.h` 中。

## 驱动程序初始化流程

`kernel_main` 使用子系统框架（subsys）按 Phase 初始化驱动：

### Phase 1-2（硬编码在 kernel_main 中）

1. **串口硬件**：`init_serial()` - 初始化串口线配置（IER=0，无 IRQ）
2. **物理内存**：`pmm_init()` - 物理内存管理
3. **虚拟内存**：`vmm_init()` - 虚拟内存管理
4. **帧缓冲**：`frame_buffer_init()` - 将帧缓冲重映射到虚拟地址空间

### Phase 3-6（subsys_init_all，在 arch_register_subsys 中定义）

| Phase | 组件 | 函数 | 必需 |
|-------|------|------|------|
| 3 | APIC | `apic_init()` | 是 |
| 3 | PIC | `pic_init()` | 可选（无 APIC 时用）|
| 4 | 定时器 | `timer_init()` | 是 |
| 4 | PIT | `pit_init()` | 可选 |
| 4 | LAPIC 定时器 | `lapic_timer_init()` | 可选 |
| 5 | 键盘 | `keyboard_init()` | 可选 |
| 5 | 串口 IRQ | `init_serial_irq()` | 是 |
| 6 | AHCI | `ahci_init()` | 可选 |
| 6 | PS/2 鼠标 | `mouse_init()` | 可选（探测 500 ms 内放弃）|

### Phase 7-9（硬编码在 kernel_main 中）

7. **VFS/DevFS**：`vfs_init()` → `devfs_init()` → 注册设备（keyboard, fb, block）
8. **TTY**：`tty_alloc()` → 将 serial/keyboard IRQ 连接到 TTY
9. **Per-CPU + SMP**：`percpu_init()` → `smp_boot_aps()` → 每 CPU LAPIC 定时器启动
10. **调度器 + 用户空间**：`task_init()` → 加载 /init.elf

## 代码结构

### 驱动程序文件

* `kernel/driver/keyboard.c` - 键盘驱动
* `kernel/driver/i8042.c` - i8042 共享控制器层（键盘/鼠标字节分流）
* `kernel/driver/mouse.c` - PS/2 鼠标驱动
* `kernel/driver/mouse_proto.c` - PS/2 鼠标包解析器
* `kernel/driver/serial.c` - 串口驱动
* `kernel/driver/pit.c` - PIT 定时器驱动
* `kernel/driver/rtc.c` - RTC 实时时钟驱动
* `kernel/driver/ahci.c` - AHCI SATA 驱动
* `kernel/driver/pci.c` - PCI 配置空间访问

### 驱动程序头文件

* `kernel/include/driver/keyboard.h` - 键盘驱动头文件
* `kernel/include/driver/i8042.h` - i8042 控制器层头文件
* `kernel/include/driver/mouse.h` - PS/2 鼠标驱动头文件
* `kernel/include/driver/mouse_proto.h` - PS/2 包解析器头文件
* `kernel/include/uapi/mouse.h` - `/dev/mouse` 用户态事件 ABI（8 字节）
* `kernel/include/driver/serial.h` - 串口驱动头文件
* `kernel/include/driver/pit.h` - PIT 定时器驱动头文件
* `kernel/include/driver/rtc.h` - RTC 实时时钟驱动头文件
* `kernel/include/driver/ahci.h` - AHCI SATA 驱动头文件
* `kernel/include/driver/pci.h` - PCI 驱动头文件

### 设备相关头文件

* `kernel/include/intr/pic.h` - PIC/IOAPIC 控制器头文件
* `kernel/include/time/timer.h` - 定时器设备头文件
* `kernel/include/block/blockdev.h` - 块设备层头文件

## 扩展驱动程序

### 添加新驱动程序的步骤

1. **创建驱动文件**：在 `kernel/driver` 目录下创建新的驱动文件
2. **实现驱动逻辑**：实现驱动程序的核心逻辑
3. **创建中断控制器**：如果需要中断，创建中断控制器
4. **实现中断处理函数**：实现硬件中断处理函数
5. **创建初始化函数**：实现驱动程序的初始化函数
6. **注册中断**：在初始化函数中注册中断
7. **添加头文件**：在 `kernel/include/driver` 目录下创建对应的头文件
8. **初始化驱动**：在 `kernel_main` 函数中添加驱动初始化调用

### 驱动程序示例

以下是一个简单的驱动程序示例：

```c
// example_driver.c
#include <driver/example_driver.h>
#include <intr/pic.h>
#include <intr/interrupt.h>
#include <arch/x86_64/hw.h>

hw_int_controller_t example_controller = 
{
    .enable = pic_enable,
    .disable = pic_disable,
    .install = pic_install,
    .uninstall = pic_uninstall,
    .ack = pic_ack,
};

void example_handler(uint64_t nr, uint64_t parameter, pt_regs_t * regs)
{
    // 处理中断
    // 读取硬件状态
    // 发送 EOI 信号
}

void example_init()
{
    // 初始化硬件
    // 注册中断
    register_irq(IRQ_NUMBER, NULL, &example_handler, 0, &example_controller, "example");
}
```

## 注意事项

1. **中断处理函数应尽量简短**：中断处理函数执行时间过长会影响系统响应速度
2. **及时发送 EOI 信号**：处理完中断后应及时发送 EOI 信号，否则会阻止后续中断
3. **硬件访问的安全性**：访问硬件寄存器时应注意安全性，避免误操作
4. **错误处理**：应适当处理硬件错误，提高系统稳定性
5. **资源管理**：应合理管理硬件资源，避免资源泄漏

## 调试技巧

1. **串口调试**：使用串口驱动输出调试信息
2. **中断跟踪**：在中断处理函数中添加调试信息
3. **寄存器检查**：检查硬件寄存器状态
4. **时序分析**：分析硬件操作的时序要求
5. **模拟器调试**：使用 QEMU 等模拟器进行调试

## 未来计划

1. **扩展驱动程序**：添加更多硬件驱动，如 NVMe、网络等
2. **驱动程序框架**：完善驱动程序框架，支持热插拔
3. **设备管理**：实现设备管理系统，统一管理硬件设备
4. **驱动程序抽象**：提供更高层次的驱动程序抽象，简化驱动开发
5. **性能优化**：优化驱动程序性能，提高系统响应速度
