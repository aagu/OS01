# 驱动程序系统

本系统实现了多种硬件驱动程序，包括键盘、串口、定时器和实时时钟等。

## 驱动程序架构

系统的驱动程序架构采用分层设计：

1. **硬件访问层**：直接访问硬件寄存器，实现底层硬件操作
2. **驱动核心层**：实现驱动程序的核心逻辑
3. **中断处理层**：处理硬件中断
4. **接口层**：提供给内核其他部分使用的接口

## 核心驱动程序

### 键盘驱动

#### 功能

* 处理键盘中断
* 读取键盘扫描码
* 打印键盘按下信息

#### 实现

位于 `kernel/driver/keyboard.c` 中，主要组件包括：

* `keyboard_controller` - 键盘中断控制器
* `keyboard_handler` - 键盘中断处理函数
* `keyboard_init` - 键盘初始化函数

#### 初始化流程

1. 调用 `keyboard_init` 函数
2. 注册键盘中断（IRQ1）
3. 设置中断处理函数 `keyboard_handler`

#### 中断处理流程

1. 键盘按下时触发中断
2. 调用 `keyboard_handler` 函数
3. 读取键盘扫描码（端口 0x60）
4. 发送 EOI 信号（端口 0x20）
5. 打印键盘扫描码

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

* `keyboard_controller` - 键盘中断控制器
* `pit_controller` - PIT 中断控制器

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

在 `kernel_main` 函数中，驱动程序的初始化顺序如下：

1. **串口驱动**：`init_serial()` - 初始化串口，用于调试输出
2. **物理内存管理**：`pmm_init()` - 初始化物理内存管理
3. **虚拟内存管理**：`vmm_init()` - 初始化虚拟内存管理
4. **PIC 控制器**：`pic_init()` - 初始化 PIC 控制器
5. **定时器**：`timer_init()` - 初始化定时器系统
6. **PIT 驱动**：`pit_init()` - 初始化 PIT 定时器
7. **键盘驱动**：`keyboard_init()` - 初始化键盘

## 代码结构

### 驱动程序文件

* `kernel/driver/keyboard.c` - 键盘驱动
* `kernel/driver/serial.c` - 串口驱动
* `kernel/driver/pit.c` - PIT 定时器驱动
* `kernel/driver/rtc.c` - RTC 实时时钟驱动

### 驱动程序头文件

* `kernel/include/driver/keyboard.h` - 键盘驱动头文件
* `kernel/include/driver/serial.h` - 串口驱动头文件
* `kernel/include/driver/pit.h` - PIT 定时器驱动头文件
* `kernel/include/driver/rtc.h` - RTC 实时时钟驱动头文件

### 设备相关头文件

* `kernel/include/device/pic.h` - PIC 控制器头文件
* `kernel/include/device/timer.h` - 定时器设备头文件

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
#include <device/pic.h>
#include <kernel/interrupt.h>
#include <kernel/arch/x86_64/hw.h>

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

1. **扩展驱动程序**：添加更多硬件驱动，如硬盘、网络等
2. **驱动程序框架**：完善驱动程序框架，支持热插拔
3. **设备管理**：实现设备管理系统，统一管理硬件设备
4. **驱动程序抽象**：提供更高层次的驱动程序抽象，简化驱动开发
5. **性能优化**：优化驱动程序性能，提高系统响应速度
