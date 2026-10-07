# 编译、运行与调试指南

> 本文档合并自 `docs/build.md`、`docs/build-run-debug.md`、`docs/build-system-harness.md`（2026-10-01）。
> `docs/build/toolchain.md` 保留为独立文档，覆盖 x86_64 toolchain override 契约。

本系统使用原生 Clang 交叉编译（`x86_64-unknown-none` 目标），无需单独的 GCC 交叉工具链。可重入（re-entrant）中断处理通过在 CFLAGS 中禁用 red zone 来保证。

构建体系自 2026-09-02 起按 **profile** 组织（GNU Make 重构，见 [`docs/superpowers/specs/2026-09-02-build-system-design.md`](../superpowers/specs/2026-09-02-build-system-design.md)）：每个 profile 完整定义目标架构、能力集、编译器、链接器、sysroot、UEFI 固件与 QEMU 参数。所有中间产物与最终产物都位于 `build/<profile>/` 下，不同 profile 互不污染。

> **x86_64 工具链覆盖变量**（`CLANG=clang-N`、`LLVM_NM=`、`UEFI_CLANG=` 等）、允许的开关白名单和受控环境规则：参见 [`toolchain.md`](toolchain.md)。

## 第 1 章 · 编译过程

### Profile 与用户入口

调用格式为 `make PROFILE=<name> <target>`；未指定时 `PROFILE=x86_64-clang`。

| Profile | 能力 | 产物 |
| --- | --- | --- |
| `x86_64-clang`（默认） | `kernel userland rootfs uefi` | `kernel.bin`、用户 ELF + BusyBox、`image/disk.img`、BOOTX64.EFI |
| `aarch64-clang` | `kernel uefi` | `kernel.elf`、BOOTAA64.EFI、64 MiB FAT `image/aarch64-uefi.img` |

**能力契约**：每个入口 target 都是能力感知的。在缺少对应能力的 profile 上执行会立即失败（解析期报错，不会等到编译）：

```text
make PROFILE=aarch64-clang run
make: *** PROFILE='aarch64-clang' lacks capability 'rootfs'.  Stop.
```

- `run` / `run-kvm` / `run-virtio` / `debug` / `test-*`（x86 E2E）需要 `rootfs`。
- `aarch64-uefi` / `aarch64-uefi-kernel` / `run-aarch64-uefi` 需要 `uefi`。
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
make print-run-paths  默认 profile 打印 firmware=/image= 绝对路径（手动 QEMU 用，见下文）
make validate         内核 ELF / EFI 产物验证 + profile 信息打印
make clean            清理指定 profile（默认 profile 还删除项目根兼容文件）
```

**Test entry points** — seven buckets (3 with flags, 4 fixed):

| Bucket | Purpose | Default |
| --- | --- | --- |
| `make test-qemu SUITE=phase-0\|systest\|inittab-phase\|network\|gfx\|resolution\|driver-model` | QEMU E2E against the matching variant image (or the normal image for `gfx`/`resolution`/`driver-model`) | `phase-0` |
| `make test-host` | hosttests + PMM boot reservation | — |
| `make test-static` | runtime / layout / canary / link-order audits (8 audits) | — |
| `make test-aarch64 MODE=smp\|no-ack\|gic-spi\|sync-fault\|m3-probe\|m1-*` | AArch64 PSCI / GIC / sync-fault harness | `smp` |
| `make test-contract PROFILE=x86_64-clang\|aarch64-clang` | CI build-contract check | `x86_64-clang` |
| `make test-kernel-selftest` | Boot selftest image + parse `[selftest]` markers | — |
| `make test-harness` | Python unittest framework regression (no QEMU, never discovers QEMU scripts) | — |

AArch64 M1：`MODE=m1-ram` 分别构建 normal/selftest 独立镜像并跑 16 组 RAM/CPU 矩阵。其余 `m1-*` MODE 构建 `AARCH64_M1_TEST=sparse|arena-exhaust|table-exhaust|ap-bad-root` 对应的隔离 `image/m1-<case>/` / `kernel/m1-<case>/` 变体；该旗要求 `KERNEL_SELFTEST=1`，拒绝与 sync-fault、weak-selftest 或 canary 混用。稀疏 map 仅由编译旗改写，不从环境变量注入。`make PROFILE=x86_64-clang test-m1-host [CASE=m1-layout|m1-reservation|m1-arena|m1-tree|m1-contract-x86|m1-install|m1-publish]` 为原生 focused 测试入口，省略 CASE 跑整组。

Resolution Switcher：`make PROFILE=x86_64-clang test-resolution-host [RES_CASE=uapi|state|writers|bga|ioctl|pty|terminal|clients|hooks|all]` 为分辨率切换器宿主聚焦测试分发入口，省略 `RES_CASE` 时默认 `all`（运行所有已注册 case）；未知或未注册 case 报错非零退出。端到端套件为 `make PROFILE=x86_64-clang test-qemu SUITE=resolution`（生产配置：QMP surface + `setres` 会话正常用例，含 800×600 往返、空闲后台切换、白名单外初态查询、三启动镜像隔离校验，以及 `-vga cirrus` 无 BGA 设备用例——SET/枚举返回 ENODEV 而信任的 GOP framebuffer、read/surrender 与串口会话仍存活）；`make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution` 使用隔离的 `resolution-test` 镜像运行 `/dev/fbtest` 故障子集（读回失配、排空超时、terminal-ENOMEM、SIGWINCH、raw mmap sticky、回滚失败）。QMP 连接失败或截图失败一律 FAIL，不跳过。结果与截图落在 `build/x86_64-clang/test-results/resolution/`。

Standalone (not bucketed): `test-syscall-repeat` (own harness),
`test-user-canary` (subset of test-static, distinct prereqs),
`test-pmm-boot-reservation` (subset of test-host). 详见第 3 章 §3 test bucket model。

### 必要的依赖项

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
   * QEMU（`qemu-system-x86_64`；aarch64 用 `qemu-system-aarch64`）
   * OVMF.fd（UEFI 固件；x86_64 profile 在首次使用时自动获取 profile 私有副本，见[固件与手动 QEMU](#固件与手动-qemuprint-run-paths)）

### 安装依赖项

#### Ubuntu/Debian 系统

```bash
sudo apt update
sudo apt install clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86

# OVMF.fd 无需手动下载：x86_64 profile 首次使用时自动获取
# （OVMF_FIRMWARE_SOURCE，默认 https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd）
# 到 build/<profile>/firmware/OVMF.fd
```

#### Arch Linux 系统

```bash
sudo pacman -S clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86_64 edk2-ovmf

# 可选：改用发行版固件而不是默认下载
# （OVMF_FIRMWARE_SOURCE 只接受 https:// URL 或已存在的绝对本地文件路径）
make PROFILE=x86_64-clang disk.img OVMF_FIRMWARE_SOURCE=/usr/share/edk2/x64/OVMF.fd
```

### 编译步骤

#### 1. 克隆项目

```bash
git clone <项目仓库地址>
cd OS01
git submodule update --init   # BusyBox 与 posix-uefi 子模块
```

#### 2. 编译整个项目

```bash
make
```

等价于 `make PROFILE=x86_64-clang disk.img`。此命令会执行：

1. **发布 sysroot generation**：按 staging manifests 安装内核头文件、libc/libk、mbedTLS、compat-libs（libm.a/librt.a 桩），组装不可变 generation 并原子发布 `build/x86_64-clang/sysroot` 符号链接
2. **编译引导程序**：posix-uefi runtime adapter（受控拷贝 + patch）→ BOOTX64.EFI
3. **编译内核**：`artifacts/kernel.bin`（中间文件在 `build/x86_64-clang/kernel/`）
4. **编译用户程序**：`artifacts/user/*.elf`（init/spin/sigtest/.../nettest/tetris）
5. **编译 BusyBox**：`artifacts/user/busybox.elf`
6. **创建磁盘镜像**：`build/x86_64-clang/image/disk.img`（GPT 双分区），并内容保护地复制到项目根 `disk.img`

> **失效语义（增量构建）**：sysroot 以不可变 generation 发布。任一 sysroot 内容变化（内核/libc 头文件或库）会使 generation id 递增，内核、用户程序与 BusyBox 因 generation 变化而**整体重编译**（`-B`/digest），而非只重编依赖者——这是不可变 generation 设计的取舍。组件自身源码修改仍走 .d 依赖图只重编依赖者。

#### 3. 编译单个组件

```bash
make kernel.bin      # 内核（默认 profile）
make lib             # sysroot 库（含 sysroot generation 发布）
make user            # 用户 ELF 与 BusyBox
make image           # 磁盘镜像（产物路径，不是项目根副本）
```

#### 4. 固件与手动 QEMU（print-run-paths）

x86_64 profile 的 UEFI 固件是 **profile 私有** 的，位于 `build/<profile>/firmware/OVMF.fd`（不是源码树里的 `boot/uefi/OVMF.fd`）。首次使用时由根 Makefile 的固件规则从 `OVMF_FIRMWARE_SOURCE` 获取——只接受 `https://` URL（下载到临时文件后原子 rename）或已存在的**绝对**本地文件路径（内容保护拷贝），其它值在下载前报错：

```bash
make PROFILE=x86_64-clang disk.img OVMF_FIRMWARE_SOURCE=/abs/path/to/OVMF.fd
```

所有 x86 QEMU 入口（`run`/`run-kvm`/`run-virtio`/`debug`/`test-*`）都直接使用该 profile 固件与 profile 镜像（`build/<profile>/image/disk.img`），绝不读源码树固件，也绝不把项目根 `disk.img` 作为输入。

手动启动 QEMU 时先解析路径：

```bash
make PROFILE=x86_64-clang print-run-paths
#   firmware=/home/.../build/x86_64-clang/firmware/OVMF.fd
#   image=/home/.../build/x86_64-clang/image/disk.img
```

再把这两条路径分别填入 `-drive if=pflash,format=raw,readonly=on,file=<firmware>` 与 `-drive file=<image>,format=raw,...`。

### 输出路径（profile 布局）

`x86_64-clang` profile 的全部输出位于 `build/x86_64-clang/`：

| 路径 | 内容 |
| --- | --- |
| `build/<profile>/artifacts/kernel.bin` | 内核 |
| `build/<profile>/artifacts/user/<name>.elf`、`busybox.elf` | 用户程序（variant 时在 `artifacts/user/<variant>/`） |
| `build/<profile>/artifacts/uefi/BOOTX64.EFI` | EFI 应用 |
| `build/<profile>/image/disk.img` | 普通磁盘镜像 |
| `build/<profile>/image/systest/disk.img` 等 | 测试 variant 镜像 |
| `build/<profile>/sysroot` | 指向当前 generation 的符号链接 |
| `build/<profile>/sysroot-generations/<id>/` | 不可变 sysroot generation |
| `build/<profile>/kernel`、`user`、`libc`、`uefi`、`uefi-runtime`、`thirdparty/` | 组件专有中间产物 |
| `build/<profile>/staging/<component>/` | 组件安装暂存树 |
| `build/<profile>/host-tools/mkdisk` | 宿主构建工具 |
| `build/<profile>/host-test` | 宿主测试对象与二进制 |
| `build/.locks/<profile>/publish` | 发布锁（在 `build/<profile>/` 之外，`clean` 不删除） |

项目根 `kernel.bin` 与 `disk.img` 是默认 profile 产物的**单向兼容副本**（内容保护：内容相同则不覆盖）。

### 项目结构

#### 主要目录

* `boot/` - 引导程序相关代码
  * `uefi/` - UEFI 引导程序
* `kernel/` - 内核代码
  * `kernel/` - 内核主文件（main.c, printk.c, panic.c, log.c 等）
  * `arch/x86_64/` - x86_64 架构代码（head.S, trap.c, subsys.c）
  * `intr/apic/` - APIC 子系统（acpi.c, lapic.c, ioapic.c, ipi.c, lapic_timer.c）
  * `block/` - 块设备层
  * `driver/` - 驱动程序
  * `fs/` - 文件系统（VFS, FAT32, ext2, devfs, procfs, tmpfs）
  * `intr/` - 中断处理（irq.c, softirq.c, dispatch.c）
  * `memory/` - 内存管理
  * `sched/` - 调度器
  * `subsys/` - 子系统注册框架
  * `time/` - 时钟源与定时器
  * `tty/` - TTY 子系统
  * `percpu/` - 每 CPU 数据结构
* `libc/` - 系统库（libk 供内核，libc 供用户程序）
* `user/` - 用户空间程序
* `config/` - 配置文件（busybox.config.in、rootfs.mk、inittab 模板、posix-uefi patch）
* `mk/` - 构建模块（project.mk、profiles/、targets/、toolchains/、components/）
* `hosttests/` - 宿主测试代码
* `qemutests/` - E2E 测试脚本（run_test.py）
* `tools/` - 构建工具（mkdisk）
* `docs/` - 文档
* `thirdpart/` - 第三方依赖（posix-uefi, busybox-1.36.1, mbedtls）

#### 编译产物

* `build/<profile>/artifacts/uefi/BOOTX64.EFI` - UEFI 引导程序（aarch64 profile 为 BOOTAA64.EFI）
* `build/<profile>/firmware/OVMF.fd` - profile 私有 UEFI 固件（x86_64）
* `kernel.bin` - 内核二进制（项目根目录，默认 profile 兼容副本）
* `build/x86_64-clang/artifacts/kernel.bin` - 内核 artifact
* `build/x86_64-clang/kernel/kernel.elf` - 内核 ELF（含调试符号，供 GDB 使用）
* `build/x86_64-clang/artifacts/user/*.elf` - 用户程序 ELF
* `disk.img` - GPT 双分区磁盘镜像（项目根目录，默认 profile 兼容副本）
* `build/x86_64-clang/image/disk.img` - 磁盘镜像 artifact

### 常见问题和解决方案

#### 1. 编译失败

**问题：找不到 clang 或其他编译工具**

解决方案：确保已正确安装所有编译工具和依赖项。工具链可覆盖：`make CLANG=clang-22 kernel.bin`（详见 [`toolchain.md`](toolchain.md)）。

**问题：缺少固件（OVMF.fd）**

解决方案：无需手动下载。x86_64 profile 在首次使用 QEMU 入口时自动从 `OVMF_FIRMWARE_SOURCE` 获取固件到 `build/<profile>/firmware/OVMF.fd`；用 `make PROFILE=x86_64-clang print-run-paths` 查看当前固件/镜像路径。

**问题：`PROFILE='...' lacks capability '...'`**

解决方案：该 target 在当前 profile 下不可用。改用具备对应能力的 profile，或换用与 profile 匹配的 target。

#### 2. 运行失败

**问题：QEMU 无法启动**

解决方案：检查 QEMU 是否正确安装；profile 固件会自动获取，可用 `make PROFILE=x86_64-clang print-run-paths` 确认固件/镜像路径（首次 QEMU 运行会创建 `build/<profile>/firmware/OVMF.fd`）。

**问题：系统启动后无输出**

解决方案：检查串口连接是否正确，确保 `serial_printk` 函数被正确调用。

### 开发技巧

#### 快速编译和测试

```bash
make disk.img
make run
```

#### 清理项目

```bash
make clean
```

此命令持有发布锁并确认没有 generation read lease 后，删除 `build/x86_64-clang/`（不会删除 `build/.locks/` 或其他 profile 的目录），并删除默认 profile 拥有的项目根兼容文件（`disk.img`、`kernel.bin`）。非默认 profile 的 `make PROFILE=<name> clean` 只删除该 profile 的 `build/<profile>/`，不动项目根兼容文件。

### v25 增量：UEFI 残留排除（`mk/components/uefi.mk`）

**问题**：linked git worktree 的 `git -C posix-uefi status --porcelain` 用 submodule `config.worktree` 路径解析到 MAIN checkout（不在 worktree），与 find-based digest（跑在 OS01_ROOT 看 worktree 内容）不一致，导致前次 x86_64 build 残留 `*.o` / `*.a` / `*.lib` 经 `uefi/*.o` glob 进 aarch64 `BOOTAA64.EFI` ld.lld 链接产生 **duplicate-symbol + machine-type-mismatch**。

**修复**（`5dbc63d`）：

1. `find thirdpart/posix-uefi -type f` digest 用 `! -name "*.o" ! -name "*.a" ! -name "*.lib"` 排除
2. `cp -a "$(UEFI_RUNTIME_SOURCE)/." "$(UEFI_RUNTIME_DIR)/"` 后加 `find "$(UEFI_RUNTIME_DIR)" \( -name "*.o" -o -name "*.a" -o -name "*.lib" \) -delete`

干净 checkout 无行为变化。验证 selftest 21/21 + syscall 268/268 + aarch64 UEFI BOOTAA64.EFI ARM64 PE32+。

## 第 2 章 · 构建、运行与调试操作

> 本章翻译自 `docs/build-run-debug.md`（英文）。

### 1. 运行系统（x86_64）

```bash
make run
```

运行 QEMU（`-M q35 -smp 2`）+ OVMF 固件 + 磁盘镜像，串口输出到 stdio。能力说明：`run` 需要 `rootfs` 能力，所以 `make PROFILE=aarch64-clang run` 在 capability gate 处失败。

```bash
make run-kvm        # KVM 加速
make run-virtio     # virtio-net 代替 e1000e
```

### 2. 固件（profile 私有）

x86_64 profile 拥有位于 `build/<profile>/firmware/OVMF.fd` 的 UEFI 固件——绝不在源码树中。首次使用 root-owned 固件规则从 `OVMF_FIRMWARE_SOURCE`（默认 `https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd`）获取，只接受 `https://` URL 或已存在的**绝对**本地文件路径；其它值在任何下载/拷贝之前失败。

所有 x86 QEMU 入口（`run` / `run-kvm` / `run-virtio` / `debug` / `test-*`）使用此 profile 固件与 profile 镜像（`build/<profile>/image/disk.img`）——绝不读 `boot/uefi/OVMF.fd`，绝不用项目根 `disk.img`。

### 3. 调试

```bash
make debug
```

启动 QEMU 暂停 + GDB remote server 监听 :1234。另一个终端：

```bash
gdb build/x86_64-clang/kernel/kernel.elf
# target remote localhost:1234
# break kernel_main / continue / ...
```

`.vscode` 下的 VS Code 配置也驱动 `make debug`。

### 4. aarch64 UEFI 启动

```bash
make PROFILE=aarch64-clang run-aarch64-uefi
```

运行 `qemu-system-aarch64 -M virt -display none -serial stdio` + 64 MiB FAT 启动镜像（BOOTAA64.EFI + kernel.elf + 固件）。串口启动签名：`aarch64 uefi handoff ok`，接着 `phase1 boot ok`。`make PROFILE=aarch64-clang aarch64-uefi` 构建镜像 + 固件；`aarch64-uefi-kernel` 仅构建内核 ELF。三个目标都需要 `uefi` 能力（在 x86 profile 下干净失败）。

### 5. 测试

#### 宿主测试

```bash
make test-host
# `make test` 是 `make test-host` 的别名
```

运行 `hosttests/` 下的宿主测试套件（`Suites: 16 | Failed: 0`）+ PMM boot reservation。

#### QEMU E2E 测试（variant-isolated images）

每个 x86 E2E 目标将其镜像 **variant** 构建到独立目录，并通过 `DISK_IMG` 环境变量对那个精确镜像运行 `qemutests/run_test.py`。Variant 构建**绝不删除或覆盖普通镜像** `build/x86_64-clang/image/disk.img`：variant 构建前后记录普通镜像的 sha256（`image/normal.before` / `image/normal.after`）并比对，因此触碰普通镜像的 variant 构建会大声失败。

| 目标 | Variant 镜像 | 套件 |
| --- | --- | --- |
| `make test-qemu SUITE=phase-0` | 普通 `build/<profile>/image/disk.img` | 启动 + shell 提示符 |
| `make OS01_SYSTEST=1 test-qemu SUITE=systest` | `build/<profile>/image/systest/disk.img` | syscall E2E（`OS01_SYSTEST=1`） |
| `make INITTAB_FILE=config/inittab.test test-qemu SUITE=inittab-phase` | `build/<profile>/image/inittab-test/disk.img` | inittab 阶段派发（`INITTAB_FILE=config/inittab.test`） |
| `make OS01_NETTEST=1 test-qemu SUITE=network` | `build/<profile>/image/nettest/disk.img` | 网络回归（`OS01_NETTEST=1`） |
| `make test-qemu SUITE=gfx` | 普通 `build/<profile>/image/disk.img` | ring-3 gfx / terminal / desktop smoke |
| `make test-qemu SUITE=resolution` | 普通镜像（每次运行复制为私有副本） | 分辨率切换 surface/session 验收（QMP + `setres`） |
| `make FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution` | `build/<profile>/image/resolution-test/disk.img` | 隔离故障子集（`/dev/fbtest`） |
| `make test-qemu SUITE=driver-model` | 普通镜像（matrix harness 另建 fault 变体） | 驱动模型矩阵 |

systest variant 是 **compile-affecting** 的：`OS01_SYSTEST=1` 给 user CFLAGS 加 `-DOS01_SYSTEST`，所以 variant 的用户程序构建到独立对象/artifact 目录（`build/<profile>/user/systest`，`build/<profile>/artifacts/user/systest`），variant 镜像包含 systest 编译的 `init.elf`，启动 `/bin/systest` 而不是 BusyBox shell。另两个 variant（nettest、inittab-test）只改 inittab 文件和镜像目录，其用户二进制与普通构建共享。

也可以直接构建 variant 镜像：

```bash
make OS01_SYSTEST=1 image          # → build/x86_64-clang/image/systest/disk.img
make OS01_NETTEST=1 image          # → build/x86_64-clang/image/nettest/disk.img
make INITTAB_FILE=config/inittab.test image   # → .../image/inittab-test/disk.img
```

#### 测试框架（runner、归档、退出码与观察窗口）

宿主 C 测试、x86/aarch64 QEMU 套件与静态审计共用 `qemutests/harness/` 中的进程/结果层（`ProcessSession`、`RunArchive`、`parse_v1`）。runner 直接调用时的退出码统一为：

| 退出码 | 含义 |
| --- | --- |
| `0` | PASS — 有效完整结果 |
| `1` | FAIL 或 TIMEOUT — 测试未通过（含超时） |
| `2` | ERROR — 配置/运行环境错误（缺可执行文件、不可用输入、镜像/固件缺失等） |
| `130` | Ctrl-C（SIGINT） |

Make/CI 只保证「零 / 非零」，不要求透传具体数值；`result.json` 记录 runner 与子进程的真实返回码、细分状态与中断原因（spec §6.2）。框架级不变式：`status="ERROR"` ⟺ 退出 2，`status="FAIL"`/`"TIMEOUT"` ⟺ 退出 1，`status="PASS"` ⟺ 退出 0；Ctrl-C 在 130 单独成行。

**运行归档。** 每次运行（成功、失败与超时都保留）归档到：

```text
build/<profile>/logs/tests/<suite>/<UTC-time>-<unique-id>/
  stdout.log      # 宿主命令输出或 guest 串口输出
  stderr.log      # 宿主进程 / QEMU 诊断
  result.json     # 同目录临时文件 + 原子 rename 替换
```

生命周期由用户清理 / `make clean <profile>` 管理，不自动删除。

> **已记录偏差（启动失败归档只含 `result.json`）。** 归档目录在 `ProcessSession.start()` **之前**创建，而 `stdout.log`/`stderr.log` 只有 `Popen` 成功后才打开。因此**启动失败**（可执行文件缺失等）的归档只有 `result.json`，没有 `stdout.log`/`stderr.log`。这是刻意的：启动失败时本来就没有子进程输出可采集，且修正它需要改变 `ProcessSession` 语义（方案明确禁止）。受影响的运行路径包括 `run_static_audit.py` 与 `run_hosttests.py`。

**`result.json`（schema v1）** 至少记录：schema 版本；run ID；git revision 与 dirty 状态；profile 与 suite；请求选择条件与声明/实际 ID 集合（普通 v1 套件）；实际 argv；CPU 数与内存；工具版本；镜像/固件路径与 SHA-256（有输入时，区分运行前/后哈希）；UTC 开始时间与耗时；runner / child 返回码；是否受控停止；最终状态；计数单位；各用例状态/理由；日志路径。

**计数单位（count unit）。** `count_unit` 描述报告**实际包含**的内容。仅当该次运行发布了**逐用例记录**时用 `case`：协议 v1 的 systest 与内核自测（`declared_ids`/`observed_ids` 与逐用例结果齐备），以及迁移到 v1 的宿主二进制。旧格式/重复聚合套件用 `suite`：包括旧格式宿主二进制、`run_test.py` 的**全部**套件（它只把 v1 当作门控，不发布逐用例记录——`outcomes` 为空、`declared_ids` 为 null）与 `systest-repeat` 之类的重复套件。静态审计用 `audit`。断言数不等于用例数；旧格式适配器**绝不**编造 guest 用例记录。

**`CASE` 选择。** 目前只有 systest 支持按用例选择：`make OS01_SYSTEST=1 test-qemu SUITE=systest CASE=<id>`，`<id>` 必须匹配 `[A-Za-z0-9_.-]+`（`ID` 由 `user/systest.c --list` 提供），Make 写入私有 inittab 行并经现有 `INITTAB_FILE` 传入。宿主 runner 另有 `--binary ID` 选择单个 `TEST_BINS` 二进制。其它套件（`phase-0`、`gfx`、`resolution`、`driver-model`）**不支持** `CASE`。

**1 秒观察窗口与其边界。** 套件收到完整结果后继续观察 **1 秒**（`ProcessSession.observe`，共享 `_panic_in` 检测器），以捕获结果到达后窗口内到达的尾部异常（如 panic），随后由 runner 主动停止 QEMU、排空并回收。窗口只检查「已到达 / 窗口内」的异常，**不保证**检测无限延后的崩溃。

> **已记录偏差（窗口归属）。** 1 秒窗口施加于**自身拥有该延迟**的套件（普通 x86/aarch64 套件与内核自测）。预期 fatal/故障套件不把 1 秒窗口作为验收依据：它们按各自套件拥有的证据规则判定——例如 sync-fault 要求 armed/fatal 顺序唯一、寄存器字段完整，且证据成立后由 runner 停止仍存活的 QEMU；QEMU 自行退出则失败。

**实际生效配置。** 文档区分「Make 输入」与「实际生效值」：

| 套件 | 实际 QEMU CPU 来源 | 记录字段 |
| --- | --- | --- |
| `test-qemu SUITE=...`（x86） | `QEMU_SMP` 环境变量（默认 `1`；`run_test.py` 读取并用于 `-smp`） | `result.json.cpu_count` |
| `test-kernel-selftest` | `KERNEL_SELFTEST_SMP`（透传为 `--cpu`） | `cpu_count` |
| `test-aarch64 MODE=...` | 各 MODE 自带的 `--cpus` 列表 | 每个 (case, cpu) 一份报告 |
| 交互 `run` / `run-kvm` / `debug` | `SMP`（默认 `2`） | — |

> 说明：`test-qemu` 的 CPU 数取自 `QEMU_SMP` 环境变量（如 `QEMU_SMP=2 make test-qemu SUITE=phase-0`），这正是 `run_test.py` 实际读取并归档到 `cpu_count` 的值；Make 变量 `SMP` 目前只作用于交互式 `run`/`debug` 目标。

**CI。** `.github/workflows/ci.yml` 的 `harness` job 运行 `make test-harness`（纯 Python，无 QEMU、无构建前置）；`x86-checks` 在 PR 上跑 `phase-0`、`systest` 的 1/2 核与内核自测 8 核，`aarch64-checks` 跑 aarch64 SMP 冒烟，失败时上传 `build/*/logs/tests/**`；夜间 `schedule` 与手动 `workflow_dispatch` 的 `full-matrix` job 保留 1/2/4/8 核与 RAM/fault MODE。`test-contract` 保持在独立 `contract` job（它会 `make clean`）。

**耗时（实测）。** 本仓库开发机：`make test-harness`（15 模块）约 **4 分钟**（2026-10-07，429 tests）。QEMU 套件（`phase-0`/`systest`/`test-kernel-selftest`/`test-aarch64`）的耗时须在具备已构建镜像与 `thirdpart/*` submodule 的环境（CI）实测后再回填；本 worktree 无构建树，未实测。

### 6. 配置文件

系统行为通过 `config/` 配置（BusyBox 配置、`config/rootfs.mk` 磁盘镜像清单、inittab 模板 `config/inittab`、`config/inittab.systest`、`config/inittab.nettest`、`config/inittab.test`）。

## 第 3 章 · 构建系统 Harness

> 本章翻译自 `docs/build-system-harness.md`（英文）。本文档是 OS01 的 Makefile 构建、运行、调试、验证、测试、契约和维护入口的权威参考——`help` target、`AGENTS.md` 和 CI 脚本都以此为契约。Makefile 变更时，本文档与 `make help` 同步变更。

### 1. Capability gate（能力门控）

每个入口声明其需要的 profile 能力：`kernel`、`userland`、`rootfs`、`uefi`。门控由 `$(call require_capability,<cap>)` 在解析期强制执行，所以错误的目标调用会立即以 `PROFILE='<p>' lacks capability '<cap>'` 失败，而不是在链接时。

默认 `x86_64-clang` profile 声明 `kernel userland rootfs uefi`。
`aarch64-clang` profile 声明 `kernel uefi`。

### 2. 目标分类（target taxonomy）

| Bucket | 规范名 | 能力 | 用途 |
| --- | --- | --- | --- |
| Build artifact | `disk.img`、`kernel.bin`、`lib`、`user`、`image`、`sysroot` | profile-specific | 产生单个命名产物 |
| Run / Debug | `run`、`run-kvm`、`run-virtio`、`debug` | `rootfs` | 对已有镜像启动 QEMU |
| Bring-up | `aarch64-uefi`、`aarch64-uefi-kernel`、`run-aarch64-uefi` | `uefi` | AArch64 UEFI 启动链 |
| Validate | `validate`、`validate-kernel`、`validate-uefi`、`validate-profile` | `rootfs`（最后一个：`always`） | x86 产物健全性检查 + profile 检查 |
| Test | `test-qemu`、`test-host`、`test-static`、`test-kernel-selftest`、`test-aarch64`、`test-contract`（6 个 bucket；见 §3）+ `test-harness`（框架回归；不带任何 profile 能力；不启动 QEMU） | varies | 端到端和审计套件 |
| Inspection | `print-run-paths` | `rootfs` | 为外部 QEMU 调用打印解析后的路径 |
| Maintenance | `clean`、`unlock-profile` | always | 生命周期 |

### 3. Test bucket model（测试桶模型）

**6 个测试 bucket 目标** — 3 个带 flag，3 个不带：

| Bucket | Flag | 取值 | 运行内容 |
| --- | --- | --- | --- |
| `test-qemu` | `SUITE=` | `phase-0`、`systest`、`inittab-phase`、`network`、`gfx`、`resolution`、`driver-model` | `qemutests/run_test.py <SUITE>` 对匹配的 variant 镜像（`gfx`/`resolution`/`driver-model` 用普通镜像；`resolution` 每次只复制私有副本） |
| `test-aarch64` | `MODE=` | `smp`、`no-ack`、`gic-spi`、`sync-fault`、`m3-probe`、`m1-ram`、`m1-sparse`、`m1-arena-exhaust`、`m1-table-exhaust`、`m1-ap-bad-root` | `qemutests/aarch64_*.py` 之一 |
| `test-contract` | `PROFILE=` | `x86_64-clang`、`aarch64-clang` | `qemutests/build_contract.sh <PROFILE> <mode>` 按 profile mode 列表 |
| `test-host` | — | — | `os01_submake hosttests` + `pmm_boot_reservation_test.py` |
| `test-static` | — | — | 11 项静态审计（runtime_audit、stack_canary_audit、validate-kernel、runtime_link_order、kernel_runtime_link、kernel_layout、kernel_canary_contract、driver_model_boundary_audit、header_object、stack_frame、test-user-canary） |
| `test-kernel-selftest` | — | — | 启动 selftest 镜像 variant（`KERNEL_SELFTEST=1`）并解析 `[selftest]` 标记 |

**`test-harness`** 框架回归入口（上述 bucket 之一；`always` 能力；不启动 QEMU）：运行 `mk/components/run.mk` 中 `TEST_HARNESS_MODULES` 列表里的 Python `unittest` 模块。该列表在 Task 2 以单元素 `qemutests.test_gfx_runner` 起步，各任务以其新增的 `qemutests/test_*.py` 追加（Ruling 3）；Task 13 收口为 **15 个模块**，并补齐了此前**无任何 target 运行**的宿主自测（`test_driver_model_matrix`、`test_driver_model_boundary_audit`、`test_resolution_switcher`）以及针对本 workflow 的静态契约检查 `test_ci_workflow`。`test_lvgl_runner` **不**在列表内——它是会拉起 QEMU 的驱动脚本，不是 unittest 模块。`test_arch9_build_contract` 同样**不**在列表内：它的 fixture 会对 `kernel.bin` 执行真实的 `make -n`，需要有已准备好的 sysroot，因而无法在零环境依赖的 `make test-harness` 门控中运行（CI 的 `harness` job 在调用它之前不做任何构建）。它由 `test-contract` bucket 拥有：该 target **会**构建 sysroot，且在其 `x86_64-clang` 分支的 `build_contract.sh` 模式循环之后运行该模块，因此不再是无 target 调用的孤儿。recipe 始终为一次显式的 `python3 -m unittest $(TEST_HARNESS_MODULES)`：不做自动目录发现，不隐式启动 QEMU 脚本，不通过模块名推断 build 路径。fixture 以 fake `subprocess.Popen` 替换真实 QEMU（个别模块会以 `make -n` 探测 Make 契约，但**绝不**拉起 QEMU 进程），因此 `make test-harness` 在 PR/CI 中作为"零环境依赖"门控。

**独立的测试目标**（不归入任何 bucket，因为它们使用不同的 harness 或镜像 variant）：

| 独立目标 | 不归入 bucket 的原因 |
| --- | --- |
| `test-syscall-repeat` | 使用 `x86_64_systest_repeat.py`（与 `run_test.py` 不同）和普通镜像 variant |
| `test-user-canary` | `test-static` 的子集但可单独调用；前置条件（`USER_ARTIFACTS`、busybox、rootfs 清单）不同 |
| `test-pmm-boot-reservation` | `test-host` 的子步骤；可单独调用以便 PMM-only 调试 |

**Focused compatibility checks** 在 alias 窗口期间保留可见于 `make help`，保留其原始的窄行为：

| 目标 | 检查内容 |
| --- | --- |
| `test-kernel-layout` | 仅 x86 kernel ELF 布局 |
| `test-kernel-canary-contract` | 仅 kernel canary 编译旗标契约 |
| `test-aarch64-gic-spi` | 仅 PL011 RX 到 GIC SPI 注入 |
| `test-resolution-host` | 分辨率切换器宿主聚焦测试分发（`RES_CASE=`） |

### 4. Alias policy（别名策略）

Bucket 目标是规范名。**2026-09-26 cleanup 删除了所有转发别名**（`test`、`test-phase-0`、`test-syscall`、`test-inittab`、`test-network`、`test-aarch64-uefi-smp`、`test-aarch64-uefi-smp-no-ack`、`test-aarch64-gic-spi`、`test-build-contract-x86`、`test-build-contract-aarch64`）；CI 和所有调用方必须直接使用 bucket 目标。

**2026-10-06 例外（ARCH-9 Task 12）**：`test-syscall` 是**唯一**保留的转发别名，作为 `AGENTS.md` 行 60 + 70 的 exact-target 要求兼容入口。它的真实 recipe 是 `$(MAKE) OS01_SYSTEST=1 test-qemu SUITE=systest`，并在解析期有两个 gate：

* `OS01_SYSTEST` 必须为 `1`（systest variant 是这个 alias 唯一的 init 模式；普通 inittab 下不会以 PID 1 加载 `/bin/systest`）
* `KERNEL_SELFTEST` 必须**不**为 `1`（内核内自测在 boot 时 spawn kthreads，会干扰 systest 的 fork+exec+waitpid 测试）

新代码和 CI **不应**使用 `test-syscall`；用 `OS01_SYSTEST=1 test-qemu SUITE=systest`（bucket 目标）即可。本 alias 仅作为用户 AGENTS.md 的 exact-target 兼容垫片保留，未来若 AGENTS.md 撤回该要求，应在同一次 cleanup 中一并删除（参考 `task-12-report.md` 的撤销条件）。

新增其它转发别名**不**是替代品——它不能替代在新代码或 CI 中使用 bucket 目标。

**保留的 focused checks**：某些 `test-*` 名保留自己的原始 recipe（独立的针对性检查，不转发到 bucket）。为调试永久保留：

- `test-runtime` — runtime audit 子集（5 个 python 调用 + validate-kernel）
- `test-kernel-layout` — kernel.elf 在 `_end` 之后的保留布局审计
- `test-kernel-canary-contract` — kernel canary 编译旗标契约
- `test-user-canary` — 7 步 SSP/crt0 用户栈 canary 审计
- `test-pmm-boot-reservation` — PMM 启动期内存保留守卫
- `test-resolution-host` — 分辨率切换器宿主聚焦测试分发入口

### 5. 添加新 target

1. 决定 bucket（build / run / debug / bring-up / validate / test / inspection / maintenance）。
2. 选择 flag 值（如果有），确认 recipe 不是已有 recipe 的简单变体——如果是，合并到 bucket 并加别名。
3. 添加 gate（`require_capability`），选择已有 Make 变量用于路径和工具，写 recipe。
4. 如果 recipe 含 `$(MAKE)` 调用（variant 构建、sub-make），放独立 recipe 行，使 `make -n` 遵守 dry-run 契约——见 `run.mk` 行 263-266 的现有注释。
5. 在 `mk/components/run.mk` 的 `help` recipe 追加一行，匹配 bucket 的 printf 格式。
6. 如果 bucket 获得新值，更新本文档和 `AGENTS.md`。

### 6. 变量参考

| 变量 | 定义于 | 用于 |
| --- | --- | --- |
| `RUN_QEMU_BASE` | `mk/components/run.mk` | `run`、`run-kvm`、`run-virtio`、`debug` |
| `RUN_QEMU_DISK` | `mk/components/run.mk` | NIC 选择之后的公共 disk、RNG、memory、display 和 serial 参数 |
| `RUN_QEMU_FLAGS_run-kvm` / `RUN_QEMU_FLAGS_debug` | `mk/components/run.mk` | 插入共享 network/disk 参数之前的旗标 |
| `TEST_QEMU_FLAVOR_<suite>` / `TEST_QEMU_IMG_<suite>` | `mk/components/run.mk` | `test-qemu` per-SUITE 查找（Make 变量，不是 shell 变量） |
| `X86_CONTRACT_MODES` / `AARCH64_CONTRACT_MODES` | `mk/components/run.mk` | `test-contract` |
| `_test-contract-prep-x86` / `_test-contract-prep-aarch64` (private) | `mk/components/run.mk` | per-PROFILE pre-build 步骤（拆开使 `+env` 位于 recipe 行位置） |
| `TEST_AARCH64_EXTRA_<mode>` | `mk/components/run.mk` | `test-aarch64` per-MODE DTB 旗标 |
| `_test-aarch64-prep-<mode>` / `_test-aarch64-run-<mode>` (private) | `mk/components/run.mk` | per-MODE pre-build + python 调用（拆开使 `$(MAKE)` 和 python 位于独立 recipe 行） |

## 附录 A · 命令/Flag 速查

### 常用命令

```bash
# 构建
make                                # 默认 profile 完整构建
make kernel.bin                     # 默认 profile 内核
make lib user                       # 默认 profile 库与用户程序
make image                          # 当前 profile 磁盘镜像
make clean                          # 清理

# 运行与调试
make run                            # 默认 profile QEMU
make run-kvm                        # KVM 加速
make run-virtio                     # virtio-net
make debug                          # QEMU 暂停 + GDB :1234
make PROFILE=aarch64-clang run-aarch64-uefi  # aarch64 UEFI 启动

# 测试
make test-host                      # 宿主测试
make test-qemu SUITE=phase-0        # QEMU E2E（普通镜像）
make test-qemu SUITE=systest        # syscall E2E
make test-qemu SUITE=inittab-phase  # inittab 阶段派发
make test-qemu SUITE=network        # 网络回归
make test-qemu SUITE=resolution     # 分辨率切换 surface/session 验收
make FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution  # 隔离故障子集
make test-static                    # 8 项静态审计
make test-kernel-selftest           # 内核 selftest
make test-aarch64 MODE=smp          # aarch64 PSCI/SMP
make test-contract                  # CI 契约检查
make test-harness                   # Python unittest 框架回归（TEST_HARNESS_MODULES 列表；不启动 QEMU）

# 检查
make PROFILE=x86_64-clang print-run-paths  # 打印 firmware= / image= 路径
make validate                       # 内核 ELF / EFI 验证

# aarch64
make PROFILE=aarch64-clang aarch64-uefi         # aarch64 镜像 + 固件
make PROFILE=aarch64-clang aarch64-uefi-kernel  # aarch64 内核 ELF
```

### 常用 Flag/环境变量

| Flag/变量 | 用途 |
| --- | --- |
| `PROFILE=<name>` | 选择 profile（默认 `x86_64-clang`） |
| `SUITE=<name>` | `test-qemu` 的 variant（`phase-0`、`systest`、`inittab-phase`、`network`、`gfx`、`resolution`、`driver-model`） |
| `MODE=<name>` | `test-aarch64` 的 variant（`smp`、`no-ack`、`gic-spi`） |
| `OS01_SYSTEST=1` | 启用 systest variant（影响编译旗标） |
| `OS01_NETTEST=1` | 启用 nettest variant |
| `INITTAB_FILE=<path>` | 自定义 inittab 模板 |
| `DEBUG_CHANNELS=<list>` | 启用调试通道（`sched,vfs,mm` 等） |
| `KERNEL_SELFTEST=1` | 启用内核内自测 |
| `LOG_TARGET=<target>` | 日志输出目标（`serial`、`fb`、`both`） |
| `NDEBUG=1` | 编译期消除 `log_debug` |
| `OVMF_FIRMWARE_SOURCE=<url\|abs path>` | 自定义 OVMF 固件源 |
| `UEFI_RUNTIME_SOURCE=<path>` | aarch64 UEFI 运行时源 |
| `CLANG=<name>`、 `LLVM_NM=<path>`、 `UEFI_CLANG=<path>` | 工具链覆盖（详见 [`toolchain.md`](toolchain.md)） |
| `SMP=<n>` | QEMU SMP CPU 数（默认 2） |

## 来源映射

本文档由以下三份文档合并而成（2026-10-01）：

- `docs/build.md` → 第 1 章（编译过程）
- `docs/build-run-debug.md` → 第 2 章（构建、运行与调试操作）
- `docs/build-system-harness.md` → 第 3 章（构建系统 Harness）+ 第 2 章 §2-3（部分）

`docs/build/toolchain.md` 保持独立，覆盖 x86_64 toolchain override 契约。
