# 编译过程

本系统使用原生 Clang 交叉编译（`x86_64-unknown-none` 目标），无需单独的 GCC 交叉编译工具链。可重入（re-entrant）中断处理通过在 CFLAGS 中禁用 red zone 来保证。

构建体系自 2026-09-02 起按 **profile** 组织（GNU Make 重构，见 [`docs/superpowers/specs/2026-09-02-build-system-design.md`](superpowers/specs/2026-09-02-build-system-design.md)）：每个 profile 完整定义目标架构、能力集、编译器、链接器、sysroot、UEFI 固件与 QEMU 参数。所有中间产物与最终产物都位于 `build/<profile>/` 下，不同 profile 互不污染。

> **x86_64 工具链覆盖变量（`CLANG=clang-N`、`LLVM_NM=`、`UEFI_CLANG=` 等）、允许的开关白名单和受控环境规则**：参见 [`docs/build/toolchain.md`](build/toolchain.md)。

## Profile 与用户入口

调用格式为 `make PROFILE=<name> <target>`；未指定时 `PROFILE=x86_64-clang`。

| Profile | 能力 | 产物 |
| --- | --- | --- |
| `x86_64-clang`（默认） | `kernel userland rootfs uefi` | `kernel.bin`、用户 ELF + BusyBox、`image/disk.img`、BOOTX64.EFI |
| `aarch64-clang` | `kernel uefi-bringup` | `kernel.elf`、BOOTAA64.EFI、64 MiB FAT `image/aarch64-uefi.img` |

**能力契约**：每个入口 target 都是能力感知的。在缺少对应能力的 profile 上执行会立即失败（解析期报错，不会等到编译）：

```text
make PROFILE=aarch64-clang run
make: *** PROFILE='aarch64-clang' lacks capability 'rootfs'.  Stop.
```

- `run` / `run-kvm` / `run-virtio` / `debug` / `test-*`（x86 E2E）需要 `rootfs`。
- `aarch64-uefi` / `aarch64-uefi-kernel` / `run-aarch64-uefi` 需要 `uefi-bringup`。
- `lib` / `user` 需要 `userland`。
- `validate`（x86 内核 + UEFI 产物检查）与 `make test`（宿主测试）不依赖 rootfs。

**用户入口**（默认 profile 即用，其它 profile 以 `PROFILE=<name>` 前缀使用）：

```text
make                  默认 profile 的磁盘镜像（= build/<profile>/image/disk.img 的根目录兼容副本）
make kernel.bin       默认 profile 的内核（项目根兼容副本）
make disk.img         默认 profile 的磁盘镜像（项目根兼容副本）
make image            当前 profile 的磁盘镜像（镜像路径，见下文 variant）
make lib              默认 profile 的 sysroot 库（stamp）
make user             默认 profile 的用户 ELF 与 BusyBox
make run              默认 profile 启动 QEMU（-display gtk；无显示环境用 run_test.py 的方式串口验证）
make run-aarch64-uefi aarch64-clang profile 启动 QEMU（-display none -serial stdio）
make test             默认 profile 的宿主测试（test/Makefile）
make validate         内核 ELF / EFI 产物验证 + profile 信息打印
make clean            清理指定 profile（默认 profile 还删除项目根兼容文件）
```

## 必要的依赖项

1. **编译工具链**
   * Clang/LLVM (用于编译内核，`x86_64-unknown-none` 目标)
   * GNU Make
   * ld.lld (链接器)

2. **构建依赖**
   * mkfs.vfat (from dosfstools)
   * mmd (from mtools)
   * mke2fs / debugfs (from e2fsprogs)
   * aarch64 启动还需要 `mkfs.fat`/`mcopy` 与 `/usr/share/edk2/aarch64/QEMU_EFI.fd`（edk2-aarch64）

3. **运行和调试**
   * QEMU (用于模拟 x86_64 环境，`qemu-system-x86_64`；aarch64 用 `qemu-system-aarch64`)
   * OVMF.fd (UEFI 固件，QEMU 模拟 UEFI 环境所需)

## 安装依赖项

### Ubuntu/Debian 系统

```bash
# 安装编译工具和依赖
sudo apt update
sudo apt install clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86

# 下载 OVMF.fd (如果需要)
wget https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O boot/uefi/OVMF.fd
```

### Arch Linux 系统

```bash
# 安装编译工具和依赖
sudo pacman -S clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86_64 edk2-ovmf

# 复制 OVMF.fd 到正确位置
sudo cp /usr/share/edk2/x64/OVMF.fd boot/uefi/
```

## 编译步骤

### 1. 克隆项目

```bash
git clone <项目仓库地址>
cd OS01
git submodule update --init   # BusyBox 与 posix-uefi 子模块
```

### 2. 编译整个项目

```bash
make
```

等价于 `make PROFILE=x86_64-clang disk.img`。此命令会执行以下操作：

1. **发布 sysroot generation**：按 staging manifests 安装内核头文件、libc/libk、mbedTLS、compat-libs（libm.a/librt.a 桩），组装不可变 generation 并原子发布 `build/x86_64-clang/sysroot` 符号链接
2. **编译引导程序**：posix-uefi runtime adapter（受控拷贝 + patch）→ BOOTX64.EFI
3. **编译内核**：`artifacts/kernel.bin`（中间文件在 `build/x86_64-clang/kernel/`）
4. **编译用户程序**：`artifacts/user/*.elf`（init/spin/sigtest/.../nettest/tetris）
5. **编译 BusyBox**：`artifacts/user/busybox.elf`（adapter 拷贝到 `build/<profile>/thirdparty/busybox` 后构建）
6. **创建磁盘镜像**：`build/x86_64-clang/image/disk.img`（manifest 驱动，`tools/mkdisk` 生成 GPT 双分区镜像），并内容保护地复制到项目根 `disk.img`

### 3. 编译单个组件

```bash
make kernel.bin      # 内核（默认 profile）
make lib             # sysroot 库（含 sysroot generation 发布）
make user            # 用户 ELF 与 BusyBox
make image           # 磁盘镜像（产物路径，不是项目根副本）
```

### 4. 运行测试

```bash
make test             # 宿主测试（test/Makefile）
make test-phase-0     # QEMU 启动 + shell 提示符（用普通镜像）
make test-syscall     # systest variant 镜像 + QEMU syscall E2E
make test-inittab     # inittab.test variant 镜像 + 相位派发验证
make test-network     # nettest variant 镜像 + 网络回归
```

测试镜像采用 **variant 隔离**：`test-syscall`/`test-network`/`test-inittab` 各自构建独立的变体镜像到 `build/x86_64-clang/image/<variant>/disk.img`，**绝不删除或覆盖普通 `disk.img`**，并记录/比对普通镜像的 sha256 以证明未被触碰。详见 [`docs/build-run-debug.md`](build-run-debug.md)。

## 输出路径（profile 布局）

`x86_64-clang` profile 的全部输出位于 `build/x86_64-clang/`：

| 路径 | 内容 |
| --- | --- |
| `build/<profile>/artifacts/kernel.bin` | 内核 |
| `build/<profile>/artifacts/user/<name>.elf`、`busybox.elf` | 用户程序（variant 时在 `artifacts/user/<variant>/`） |
| `build/<profile>/artifacts/uefi/BOOTX64.EFI` | EFI 应用 |
| `build/<profile>/image/disk.img` | 普通磁盘镜像 |
| `build/<profile>/image/systest/disk.img` 等 | 测试 variant 镜像 |
| `build/<profile>/sysroot` | 指向当前 generation 的符号链接 |
| `build/<profile>/sysroot-generations/<id>/` | 不可变 sysroot generation（唯一 writer 是 sysroot.mk） |
| `build/<profile>/kernel`、`user`、`libc`、`uefi`、`uefi-runtime`、`thirdparty/` | 组件专有中间产物 |
| `build/<profile>/staging/<component>/` | 组件安装暂存树 |
| `build/<profile>/host-tools/mkdisk` | 宿主构建工具 |
| `build/.locks/<profile>/publish` | 发布锁（在 build 目录之外，`clean` 不删除） |

项目根 `kernel.bin` 与 `disk.img` 是默认 profile 产物的**单向兼容副本**（内容保护：内容相同则不覆盖）。

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
* `config/` - 配置文件（busybox.config.in、rootfs.mk 镜像清单、inittab 模板、posix-uefi patch）
* `mk/` - 构建模块（project.mk、profiles/、targets/、toolchains/、components/）
* `test/` - 宿主测试代码
* `tests/` - E2E 测试脚本（run_test.py）
* `tools/` - 构建工具（mkdisk）
* `docs/` - 文档
* `thirdpart/` - 第三方依赖（posix-uefi, busybox-1.36.1, mbedtls）

### 编译产物

* `boot/uefi/BOOTX64.EFI` - UEFI 引导程序（`boot/uefi` 内部目标）
* `kernel.bin` - 内核二进制文件（项目根目录，默认 profile 兼容副本）
* `build/x86_64-clang/artifacts/kernel.bin` - 内核 artifact
* `build/x86_64-clang/kernel/kernel.elf` - 内核 ELF（含调试符号，供 GDB 使用）
* `build/x86_64-clang/artifacts/user/*.elf` - 用户程序 ELF
* `disk.img` - GPT 双分区磁盘镜像（项目根目录，默认 profile 兼容副本）
* `build/x86_64-clang/image/disk.img` - 磁盘镜像 artifact

## 常见问题和解决方案

### 1. 编译失败

#### 问题：找不到 clang 或其他编译工具

**解决方案**：确保已正确安装所有编译工具和依赖项。工具链可覆盖：`make CLANG=clang-22 kernel.bin`（详见 [`docs/build/toolchain.md`](build/toolchain.md)）。

#### 问题：缺少 OVMF.fd

**解决方案**：下载 OVMF.fd 并放置到 `boot/uefi/` 目录。

```bash
wget https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O boot/uefi/OVMF.fd
```

#### 问题：`PROFILE='...' lacks capability '...'`

**解决方案**：该 target 在当前 profile 下不可用（例如在 `aarch64-clang` 下运行 x86 的 `run`/`test-*`，或在 `x86_64-clang` 下运行 `aarch64-uefi`）。改用具备对应能力的 profile，或换用与 profile 匹配的 target。

### 2. 运行失败

#### 问题：QEMU 无法启动

**解决方案**：检查 QEMU 是否正确安装，以及 OVMF.fd 是否存在。

#### 问题：系统启动后无输出

**解决方案**：检查串口连接是否正确，确保 `serial_printk` 函数被正确调用。

## 开发技巧

### 1. 快速编译和测试

```bash
# 修改代码后
make disk.img
make run
```

### 2. 清理项目

```bash
make clean
```

此命令持有发布锁并确认没有 generation read lease 后，删除 `build/x86_64-clang/`（不会删除 `build/.locks/` 或其他 profile 的目录），并删除默认 profile 拥有的项目根兼容文件（`disk.img`、`kernel.bin`）。非默认 profile 的 `make PROFILE=<name> clean` 只删除该 profile 的 `build/<profile>/`，不动项目根兼容文件。
