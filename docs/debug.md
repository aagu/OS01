# 调试内核代码

本项目支持多种调试方法，包括使用 QEMU 调试模式、GDB 调试、VS Code 调试和串口调试等。

## 1. 使用 QEMU 调试模式

项目提供了调试目标，使用以下命令启动调试模式：

```bash
make debug
```

此命令会执行以下操作：

1. 确保磁盘镜像和 OVMF.fd 存在
2. 启动 QEMU 模拟器，加载 UEFI 固件和磁盘镜像
3. 启用 GDB 远程调试服务器（监听端口 1234）
4. 暂停 CPU 执行，等待 GDB 连接

## 2. 使用 GDB 调试

在另一个终端中，使用 GDB 连接到 QEMU 调试服务器：

```bash
gdb kernel/kernel.bin
```

然后在 GDB 中执行以下命令：

```gdb
# 连接到 QEMU 调试服务器
target remote localhost:1234

# 设置断点
break kernel_main

# 继续执行
continue

# 查看内存
x/10i $pc

# 查看寄存器
info registers

# 单步执行
step

# 继续执行直到下一个断点
continue
```

## 3. 使用 VS Code 调试

本项目可以在 VS Code 中调试代码，参考了 osdev Wiki 中的[这篇](https://wiki.osdev.org/User:TheCool1Kevin/VSCode_Debug)教程。

### 步骤：

1. **安装 VS Code**：从 [VS Code 官网](https://code.visualstudio.com/) 下载并安装

2. **安装扩展**：安装以下扩展
   * C/C++ 扩展
   * Cortex-Debug 扩展（可选）

3. **启动调试**：
   * 执行 `make debug` 启动 QEMU 调试模式
   * 在 VS Code 中按 `F5` 启动调试
   * VS Code 会自动连接到 QEMU 调试服务器

4. **调试功能**：
   * 设置断点
   * 单步执行
   * 查看变量和内存
   * 查看调用栈

VS Code 调试的配置可以查看 `.vscode` 文件夹。

## 4. 串口调试

系统使用串口进行调试输出，在运行时可以通过终端查看串口输出。

在代码中，可以使用以下函数输出调试信息：

```c
// 内核打印函数
color_printk(RED, BLACK, "Hello, World!\n");

// 串口打印函数
serial_printk("Debug message: %x\n", value);
```

## 5. 调试技巧

### 5.1 设置断点

在代码中设置断点可以帮助你了解程序的执行流程：

```c
// 在 GDB 中设置断点
break kernel_main
break pmm_init
break vmm_init
```

### 5.2 查看内存

查看内存内容可以帮助你了解数据的存储情况：

```gdb
// 查看指定地址的内存
x/10x 0x100000

// 查看寄存器指向的内存
x/10i $pc
```

### 5.3 查看调用栈

查看调用栈可以帮助你了解函数的调用关系：

```gdb
// 查看调用栈
backtrace
```

### 5.4 单步执行

单步执行可以帮助你了解程序的详细执行流程：

```gdb
// 单步执行，进入函数
step

// 单步执行，不进入函数
next

// 继续执行直到下一个断点
continue
```

## 6. 常见调试问题和解决方案

### 6.1 GDB 无法连接到 QEMU

**解决方案**：确保 QEMU 以调试模式启动，并且没有防火墙阻止端口 1234。

### 6.2 断点不生效

**解决方案**：确保断点设置在正确的函数或地址上，并且代码已经被编译到内核中。

### 6.3 调试信息不完整

**解决方案**：确保内核编译时包含了调试信息，检查 Makefile 中的编译选项是否包含 `-g`。

### 6.4 串口输出乱码

**解决方案**：检查串口设置是否正确，确保波特率设置为 38400。

## 7. 调试工具推荐

1. **GDB**：功能强大的命令行调试工具
2. **VS Code**：图形化调试界面，易于使用
3. **QEMU**：提供硬件模拟和调试支持
4. **Serial Terminal**：用于查看串口输出

通过以上调试方法，你可以更有效地开发和调试操作系统内核代码，快速定位和解决问题。