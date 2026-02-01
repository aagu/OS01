# 编译过程

本系统需要使用交叉编译工具，[为什么需要交叉编译](https://wiki.osdev.org/Why_do_I_need_a_Cross_Compiler)？简单来讲，就是去除系统自带编译工具的各种“优化”选项，因为我们在编译内核，针对应用程序编译所做的优化，可能适得其反。

## 必要的依赖项

1. **编译工具链**
   * Clang/LLVM (用于编译内核)
   * GNU Make
   * ld.lld (链接器)

2. **构建依赖**
   * mkfs.vfat (from dosfstools)
   * mmd (from mtools)

3. **运行和调试**
   * QEMU (用于模拟 x86_64 环境)
   * OVMF.fd (UEFI 固件，QEMU 模拟 UEFI 环境所需)

## 安装依赖项

### Ubuntu/Debian 系统

```bash
# 安装编译工具和依赖
sudo apt update
sudo apt install clang llvm lld make dosfstools mtools qemu-system-x86

# 下载 OVMF.fd (如果需要)
wget https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O boot/uefi/OVMF.fd
```

### Arch Linux 系统

```bash
# 安装编译工具和依赖
sudo pacman -S clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf

# 复制 OVMF.fd 到正确位置
sudo cp /usr/share/edk2/x64/OVMF.fd boot/uefi/
```

## 如何获取交叉编译工具

具体构建流程参考[这里](https://wiki.osdev.org/GCC_Cross-Compiler)，需要注意的是，本系统为64位，因此需要编译x86_64-elf工具链，而非i686-elf。此外，强烈建议参照[这个](https://wiki.osdev.org/Libgcc_without_red_zone)说明编译没有red zone的libgcc，否则在中断处理时可能遇到大麻烦。

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

## 运行系统

使用以下命令运行编译好的操作系统：

```bash
make run
```

此命令会启动 QEMU 模拟器，加载 UEFI 固件和磁盘镜像，并将串口输出重定向到标准输出。

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