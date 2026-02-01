# 编译、运行和调试指南

本指南详细说明如何编译、运行和调试本 x86_64 操作系统项目。

## 环境设置

### 必要的依赖项

1. **编译工具链**
   * Clang/LLVM (用于编译内核)
   * GNU Make
   * ld.lld (链接器)

2. **构建依赖**
   * dosfstools (提供 mkfs.vfat)
   * mtools (提供 mmd 和 mcopy)

3. **运行和调试**
   * QEMU (用于模拟 x86_64 环境)
   * OVMF.fd (UEFI 固件，QEMU 模拟 UEFI 环境所需)

### 安装依赖项

#### Ubuntu/Debian 系统

```bash
# 安装编译工具和依赖
sudo apt update
sudo apt install clang llvm lld make dosfstools mtools qemu-system-x86

# 下载 OVMF.fd (如果需要)
wget https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O boot/uefi/OVMF.fd
```

#### Arch Linux 系统

```bash
# 安装编译工具和依赖
sudo pacman -S clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf

# 复制 OVMF.fd 到正确位置
sudo cp /usr/share/edk2/x64/OVMF.fd boot/uefi/
```

## 编译步骤

### 1. 克隆项目

```bash
git clone <项目仓库地址>
cd OS01
```

### 2. 编译整个项目

项目使用 Makefile 管理编译过程，执行以下命令编译整个项目：

```bash
make
```

此命令会执行以下操作：

1. **编译引导程序**：编译 UEFI 引导程序 BOOTX64.EFI
2. **安装头文件**：将内核头文件安装到 sysroot
3. **编译库**：编译 libc 库
4. **编译内核**：编译内核生成 kernel.bin
5. **创建磁盘镜像**：创建包含引导程序和内核的 FAT32 磁盘镜像 disk.img

### 3. 编译单个组件

#### 编译引导程序

```bash
make boot/uefi/BOOTX64.EFI
```

#### 编译内核

```bash
make kernel/kernel.bin
```

#### 安装头文件和库

```bash
make lib
```

## 运行步骤

### 1. 运行系统

使用以下命令运行编译好的操作系统：

```bash
make run
```

此命令会执行以下操作：

1. 确保磁盘镜像和 OVMF.fd 存在
2. 启动 QEMU 模拟器，加载 UEFI 固件和磁盘镜像
3. 将串口输出重定向到标准输出

### 2. QEMU 命令详解

```bash
qemu-system-x86_64 \
  -pflash boot/uefi/OVMF.fd \
  -hda disk.img \
  -m 2G \
  -serial stdio
```

参数说明：

* `-pflash boot/uefi/OVMF.fd`：使用 OVMF.fd 作为 UEFI 固件
* `-hda disk.img`：使用 disk.img 作为硬盘
* `-m 2G`：分配 2GB 内存
* `-serial stdio`：将串口输出重定向到标准输出

### 3. 自定义运行参数

如果需要自定义 QEMU 运行参数，可以直接修改 Makefile 中的 `run` 目标，或者在命令行中直接执行 QEMU 命令。

## 调试方法

### 1. 使用 QEMU 调试模式

项目提供了调试目标，使用以下命令启动调试模式：

```bash
make debug
```

此命令会执行以下操作：

1. 确保磁盘镜像和 OVMF.fd 存在
2. 启动 QEMU 模拟器，加载 UEFI 固件和磁盘镜像
3. 启用 GDB 远程调试服务器（监听端口 1234）
4. 暂停 CPU 执行，等待 GDB 连接

### 2. 使用 GDB 调试

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

### 3. 使用 VS Code 调试

项目支持使用 VS Code 进行调试，配置文件位于 `.vscode` 目录中。

#### 步骤：

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

### 4. 串口调试

系统使用串口进行调试输出，在运行时可以通过终端查看串口输出。

在代码中，可以使用以下函数输出调试信息：

```c
// 内核打印函数
color_printk(RED, BLACK, "Hello, World!\n");

// 串口打印函数
serial_printk("Debug message: %x\n", value);
```

## 配置文件

项目支持通过配置文件自定义系统行为，配置文件位于 `config/config.txt`。

### 配置示例

```txt
# 分辨率设置
resolution 1440x900

# 其他配置选项（可根据需要添加）
```

### 配置说明

* **resolution**：设置系统启动时的分辨率，格式为 `宽度x高度`

## 项目结构

### 主要目录

* `boot/` - 引导程序相关代码
  * `uefi/` - UEFI 引导程序
* `kernel/` - 内核代码
  * `arch/` - 架构相关代码
  * `driver/` - 驱动程序
  * `include/` - 头文件
  * `intr/` - 中断处理
  * `memory/` - 内存管理
  * `pic/` - PIC 控制器
  * `timer/` - 定时器
* `libc/` - 系统库
* `sysroot/` - 系统根目录
* `config/` - 配置文件
* `docs/` - 文档

### 编译产物

* `boot/uefi/BOOTX64.EFI` - UEFI 引导程序
* `kernel/kernel.bin` - 内核二进制文件
* `disk.img` - 包含引导程序和内核的磁盘镜像

## 常见问题和解决方案

### 1. 编译失败

#### 问题：找不到 clang 或其他编译工具

**解决方案**：确保已正确安装所有编译工具和依赖项。

#### 问题：缺少 OVMF.fd

**解决方案**：下载 OVMF.fd 并放置到 `boot/uefi/` 目录。

```bash
wget https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O boot/uefi/OVMF.fd
```

### 2. 运行失败

#### 问题：QEMU 无法启动

**解决方案**：检查 QEMU 是否正确安装，以及 OVMF.fd 是否存在。

#### 问题：系统启动后无输出

**解决方案**：检查串口连接是否正确，确保 `serial_printk` 函数被正确调用。

### 3. 调试问题

#### 问题：GDB 无法连接到 QEMU

**解决方案**：确保 QEMU 以调试模式启动，并且没有防火墙阻止端口 1234。

#### 问题：断点不生效

**解决方案**：确保断点设置在正确的函数或地址上，并且代码已经被编译到内核中。

## 开发技巧

### 1. 快速编译和测试

在开发过程中，可以使用以下命令快速编译和测试更改：

```bash
# 修改代码后
make kernel/kernel.bin disk.img
make run
```

### 2. 清理项目

使用以下命令清理项目：

```bash
make clean
```

此命令会删除所有编译产物，包括：

* `disk.img` - 磁盘镜像
* 引导程序编译产物
* 内核编译产物
* 库编译产物
* `sysroot/` - 系统根目录

### 3. 代码组织

* **引导程序**：位于 `boot/uefi/` 目录，负责系统启动和初始化
* **内核**：位于 `kernel/` 目录，实现操作系统核心功能
* **驱动**：位于 `kernel/driver/` 目录，实现硬件驱动
* **内存管理**：位于 `kernel/memory/` 目录，实现内存管理
* **中断处理**：位于 `kernel/intr/` 目录，实现中断处理

## 高级配置

### 1. 自定义编译选项

可以通过修改 Makefile 中的 `CFLAGS` 和 `LDFLAGS` 来自定义编译选项。

### 2. 自定义 QEMU 选项

可以通过修改 Makefile 中的 `run` 和 `debug` 目标来自定义 QEMU 选项。

例如，添加网络支持：

```makefile
run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -pflash boot/uefi/OVMF.fd -hda disk.img -m 2G -serial stdio -netdev user,id=net0 -device e1000,netdev=net0
```

### 3. 交叉编译

项目默认使用 Clang 进行编译，如果需要使用其他编译器，可以修改 Makefile 中的 `CC`、`LD` 等变量。

## 总结

本指南详细说明了如何编译、运行和调试本 x86_64 操作系统项目，包括：

1. **环境设置**：安装必要的依赖项
2. **编译步骤**：编译整个项目或单个组件
3. **运行步骤**：使用 QEMU 运行系统
4. **调试方法**：使用 GDB 或 VS Code 进行调试
5. **配置文件**：通过配置文件自定义系统行为
6. **常见问题**：解决编译和运行过程中可能遇到的问题

通过本指南，您应该能够成功编译、运行和调试本操作系统项目，并开始进行开发工作。