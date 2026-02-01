# 项目结构

本系统参考osdev Wiki中[Meaty Skeleton](https://wiki.osdev.org/Meaty_Skeleton)的项目结构来组织文件。

* `boot` 系统启动相关代码
* `kernel` 系统内核相关代码
* `libc` 系统库函数
* `sysroot` 系统根目录，用于安装头文件和库
* `thirdpart` 第三方依赖库
* `toolchain` 工具链相关文件

`libc`内代码依据不同的编译指令，会编译为`libk`和`libc`两个库（后者暂未实现），`libk`和`libc`共享部分代码，前者由内核使用，后者供应用程序使用。

在`kernel`目录内，根据各文件的职责范围，大致分为以下几个子目录：

* `kernel` 内核主文件及一些不便分类的文件
* `intr` 中断处理相关
* `arch` 处理器体系架构相关
* `driver` 硬件驱动
* `include` 头文件，会和lib一起安装到sysroot中
* `memory` 内存管理相关
* `pic` pic控制器，包括8259和APIC（todo）
* `timer` 定时器相关

## 详细文件说明

### boot 目录
包含UEFI引导程序相关代码，使用posix-uefi编写，负责系统的启动和初始化工作。

### kernel 目录

#### kernel 子目录
* `main.c` - 内核主函数，系统启动的入口点
* `kallsyms.c` - 内核符号表相关
* `panic.c` - 内核 panic 处理
* `printk.c` - 内核打印函数
* `trace.c` - 内核跟踪功能

#### intr 子目录
* `irq.c` - 中断请求处理
* `softirq.c` - 软中断处理

#### arch 子目录
* `x86_64/trap.c` - x86_64 架构的陷阱处理

#### driver 子目录
* `keyboard.c` - 键盘驱动
* `pit.c` - 可编程间隔定时器驱动
* `rtc.c` - 实时时钟驱动
* `serial.c` - 串口驱动

#### memory 子目录
* `dump.c` - 内存转储功能
* `pmm.c` - 物理内存管理
* `slab.c` - 内存 slab 分配器
* `vmm.c` - 虚拟内存管理

#### pic 子目录
* `8259A.c` - 8259A PIC 控制器驱动

#### timer 子目录
* `timer.c` - 定时器实现

### include 目录
包含系统头文件，按功能分类组织：

* `device/` - 设备相关头文件
* `driver/` - 驱动相关头文件
* `kernel/` - 内核核心头文件
* `kernel/arch/x86_64/` - x86_64 架构特定头文件

### sysroot 目录
系统根目录，用于安装编译好的头文件和库文件，模拟完整的系统环境。

### thirdpart 目录
包含第三方依赖库，如 posix-uefi 等。

### toolchain 目录
包含工具链相关文件，如 QEMU 模拟器等。