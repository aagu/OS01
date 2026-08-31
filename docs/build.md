# 编译过程

本系统使用原生 Clang 交叉编译（`x86_64-unknown-none` 目标），无需单独的 GCC 交叉编译工具链。可重入（re-entrant）中断处理通过在 CFLAGS 中禁用 red zone 来保证。

> **x86_64 工具链覆盖变量（`CLANG=clang-N`、`LLVM_NM=`、`UEFI_CLANG=` 等）和验证矩阵**：参见 [`docs/build/toolchain.md`](build/toolchain.md)。

## 必要的依赖项

1. **编译工具链**
   * Clang/LLVM (用于编译内核，`x86_64-unknown-none` 目标)
   * GNU Make
   * ld.lld (链接器)

2. **构建依赖**
   * mkfs.vfat (from dosfstools)
   * mmd (from mtools)

3. **运行和调试**
   * QEMU (用于模拟 x86_64 环境，`qemu-system-x86_64`)
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
3. **编译库**：编译 libc 库（libk.a 供内核用，libc.a 供用户程序用）
4. **编译内核**：编译内核生成 `kernel.bin`（项目根目录），中间目标文件在 `build/x86_64/kernel/`
5. **编译用户程序**：编译 `init.elf`, `spin.elf`, `sigtest.elf`, `poweroff.elf`, `systest.elf` 等
6. **编译 BusyBox**：构建 `busybox.elf`（需初始化子模块 `git submodule update --init`）
7. **创建磁盘镜像**：`tools/mkdisk` 创建 GPT 双分区磁盘镜像 `disk.img`（FAT32 ESP + ext2 根文件系统）

### 3. 编译单个组件

#### 编译引导程序

```bash
make boot/uefi/BOOTX64.EFI
```

#### 编译内核

```bash
make kernel.bin
```

#### 安装头文件和库

```bash
make lib
```

#### 编译用户程序

```bash
make user
```

#### 只编译内核（不含磁盘镜像）

```bash
make kernel/kernel.bin
```

#### 内置自测试

```bash
make KERNEL_SELFTEST=1
```

在 kernel_main 中调用 `selftest_run_all()` 执行内置测试。

#### 运行测试

```bash
make test             # 运行 test/Makefile 中的测试
make test-phase-0     # 运行 tests/run_test.py phase-0
make test-syscall     # 使用 OS01_SYSTEST=1 编译并测试系统调用

> **⚠️ 注意事项**: `make test-syscall` 会自动设置 `OS01_SYSTEST=1`。如果**手动**构建 `kernel.bin` 或 `disk.img`（例如 `make run`），需要显式传入 `OS01_SYSTEST=1`，否则 init 不会启动 systest 而是启动 busybox shell。例：`make OS01_SYSTEST=1 run` / `make OS01_SYSTEST=1 disk.img`。
```

## 运行系统

使用以下命令运行编译好的操作系统：

```bash
make run
```

此命令会启动 QEMU 模拟器（`-M q35 -smp 2`），加载 UEFI 固件和磁盘镜像，并将串口输出重定向到标准输出。

### BusyBox

项目使用 BusyBox 提供用户空间工具。构建前需要初始化子模块：

```bash
git submodule update --init
```

BusyBox 配置位于 `config/busybox.config`，使用 `clang` 编译器构建。

### 磁盘镜像

`tools/mkdisk` 创建 GPT 双分区磁盘镜像：
- 分区 1：FAT32 ESP — 包含 BOOTX64.EFI 和 kernel.bin，挂载于 `/boot`
- 分区 2：ext2 根文件系统 — 包含用户程序，挂载于 `/`

Fallback 模式（无 GPT）时使用单一 FAT32 分区。

## 项目结构

### 主要目录

* `boot/` - 引导程序相关代码
  * `uefi/` - UEFI 引导程序
* `kernel/` - 内核代码
  * `kernel/` - 内核主文件（main.c, printk.c, panic.c, log.c 等）
  * `arch/x86_64/` - x86_64 架构代码（head.S, trap.c, subsys.c）
  * `intr/apic/` - APIC 子系统（acpi.c, lapic.c, ioapic.c, ipi.c, lapic_timer.c）
  * `block/` - 块设备层
  * `driver/` - 驱动程序（keyboard, serial, pit, rtc, ahci, pci）
  * `fs/` - 文件系统（VFS, FAT32, ext2, devfs, procfs, tmpfs）
  * `intr/` - 中断处理（irq.c, softirq.c, dispatch.c）
  * `memory/` - 内存管理（pmm, vmm, slab, vma, tlb）
  * `sched/` - 调度器（task.c, smp.c）
  * `subsys/` - 子系统注册框架
  * `time/` - 时钟源与定时器（clocksource + 软件定时器，subsys 名 `timer`）
  * `tty/` - TTY 子系统
  * `percpu/` - 每 CPU 数据结构
  * `intr/pic/` - 8259A PIC 控制器
* `libc/` - 系统库（libk 供内核，libc 供用户程序）
* `user/` - 用户空间程序（init, spin, sigtest, poweroff 等）
* `sysroot/` - 系统根目录
* `config/` - 配置文件（busybox.config, fsroot/）
* `test/` - 测试代码
* `tests/` - 测试脚本（run_test.py）
* `tools/` - 构建工具（mkdisk）
* `docs/` - 文档
* `thirdpart/` - 第三方依赖（posix-uefi, busybox-1.36.1）

### 编译产物

* `boot/uefi/BOOTX64.EFI` - UEFI 引导程序
* `kernel.bin` - 内核二进制文件（项目根目录）
* `kernel/kernel.elf` - 内核 ELF（含调试符号，供 GDB 使用）
* `build/x86_64/kernel/*.o` - 内核中间目标文件
* `build/x86_64/user/*.elf` - 用户程序 ELF
* `disk.img` - GPT 双分区磁盘镜像（FAT32 ESP + ext2 根文件系统）

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