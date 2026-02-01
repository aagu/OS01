# 中断处理系统

本系统实现了完整的 x86_64 架构中断处理系统，包括处理器异常处理、外部设备中断和软中断。

## 中断系统架构

### 中断类型

1. **处理器异常（陷阱）**：由处理器内部事件触发，如除零错误、页错误等
2. **外部设备中断**：由外部硬件设备触发，如键盘、定时器等
3. **软中断**：由软件触发的中断

### 中断处理流程

1. **中断门设置**：通过 `set_intr_gate`、`set_trap_gate` 和 `set_system_gate` 函数设置中断门
2. **中断触发**：处理器检测到中断信号
3. **现场保存**：自动保存处理器状态到栈中
4. **中断处理**：跳转到相应的中断处理函数
5. **中断响应**：处理具体的中断事件
6. **现场恢复**：从栈中恢复处理器状态
7. **返回中断点**：继续执行被中断的程序

## 核心组件

### 中断描述符表（IDT）

系统使用中断描述符表（IDT）来管理中断处理函数。IDT 是一个包含中断门、陷阱门和系统门的表，每个表项指向一个中断处理函数。

### 中断处理函数

#### 处理器异常处理

位于 `kernel/arch/x86_64/trap.c` 中，处理各种处理器异常：

* `do_divide_error` - 除零错误
* `do_debug` - 调试异常
* `do_nmi` - 非屏蔽中断
* `do_int3` - 断点中断
* `do_overflow` - 溢出异常
* `do_bounds` - 边界检查异常
* `do_undefined_opcode` - 未定义指令
* `do_dev_not_available` - 设备不可用
* `do_double_fault` - 双重故障
* `do_coprocessor_segment_overrun` - 协处理器段溢出
* `do_invalid_TSS` - 无效 TSS
* `do_segment_not_present` - 段不存在
* `do_stack_segment_fault` - 栈段故障
* `do_general_protection` - 通用保护故障
* `do_page_fault` - 页错误
* `do_x87_FPU_error` - x87 FPU 错误
* `do_alignment_check` - 对齐检查
* `do_machine_check` - 机器检查
* `do_SIMD_exception` - SIMD 异常
* `do_virtualization_exception` - 虚拟化异常

#### 外部中断处理

位于 `kernel/intr/irq.c` 中，处理外部设备中断：

* `register_irq` - 注册中断处理函数
* `unregister_irq` - 注销中断处理函数
* `irq_install` - 安装中断门

#### 软中断处理

位于 `kernel/intr/softirq.c` 中，处理软中断。

## 中断注册与管理

### 注册中断处理函数

使用 `register_irq` 函数注册外部设备中断处理函数：

```c
int32_t register_irq(uint64_t nr, void * arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs),
        uint64_t parameter,
        hw_int_controller_t * controller,
        const char * irq_name);
```

参数说明：
* `nr` - 中断号
* `arg` - 传递给控制器的参数
* `handler` - 中断处理函数
* `parameter` - 传递给处理函数的参数
* `controller` - 硬件中断控制器
* `irq_name` - 中断名称

### 中断控制器

系统使用 `hw_int_controller_t` 结构体表示硬件中断控制器：

```c
typedef struct hw_int_type
{
    void (*enable)(uint64_t irq);
    void (*disable)(uint64_t irq);
    uint64_t (*install)(uint64_t irq, void* arg);
    void (*uninstall)(uint64_t irq);
    void (*ack)(uint64_t irq);
} hw_int_controller_t;
```

### 中断描述符

系统使用 `irq_desc_t` 结构体描述中断：

```c
typedef struct irq_desc
{
    hw_int_controller_t* controller;
    char* irq_name;
    uint64_t parameter;
    void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs);
    uint64_t flags;
} irq_desc_t;
```

## 中断初始化

### 系统初始化流程

1. 在 `kernel_main` 中调用 `sys_vector_install()` 安装系统向量（处理器异常处理）
2. 调用 `irq_install()` 安装外部中断门
3. 调用 `pic_init()` 初始化 PIC 控制器
4. 调用各设备驱动的初始化函数，注册相应的中断处理函数

### 示例：定时器中断初始化

```c
// 在 timer_init() 中
register_irq(IRQ0, NULL, timer_irq_handler, 0, &pic_controller, "timer");
```

## 中断处理示例

### 定时器中断处理

1. 定时器触发中断（IRQ0）
2. 处理器跳转到 `IRQ0x20_interrupt` 函数
3. 保存现场，调用 `do_IRQ` 函数
4. `do_IRQ` 查找并调用 `timer_irq_handler` 函数
5. 处理定时器事件，如更新系统时间、触发定时器回调等
6. 发送 EOI（End Of Interrupt）信号给 PIC
7. 恢复现场，返回中断点

### 键盘中断处理

1. 键盘按键触发中断（IRQ1）
2. 处理器跳转到 `IRQ0x21_interrupt` 函数
3. 保存现场，调用 `do_IRQ` 函数
4. `do_IRQ` 查找并调用 `keyboard_irq_handler` 函数
5. 读取键盘扫描码，转换为 ASCII 字符
6. 处理键盘事件，如将字符加入输入缓冲区
7. 发送 EOI 信号给 PIC
8. 恢复现场，返回中断点

## 注意事项

1. **中断处理函数应尽量简短**：中断处理函数执行时间过长会影响系统响应速度
2. **避免在中断处理函数中使用阻塞操作**：中断处理函数中不应调用可能阻塞的函数
3. **正确处理中断嵌套**：确保中断控制器正确处理嵌套中断
4. **及时发送 EOI 信号**：处理完中断后应及时发送 EOI 信号，否则会阻止后续中断

## 代码结构

* `kernel/intr/irq.c` - 外部中断处理
* `kernel/intr/softirq.c` - 软中断处理
* `kernel/arch/x86_64/trap.c` - 处理器异常处理
* `kernel/include/kernel/interrupt.h` - 中断相关头文件
* `kernel/include/kernel/arch/x86_64/trap.h` - x86_64 架构陷阱相关头文件
