# OS01 GNU Make 构建系统重构设计

## 状态与目标

本设计只重构 OS01 的 GNU Make 构建体系。不会引入或迁移至 CMake、Ninja、Meson 或其他构建工具。

目标是把当前分散于根 Makefile、`toolchain.mk` 和各组件 Makefile 中的隐式约定，整理为可理解、可检查、可增量构建的产物图。重构后应清楚地区分宿主工具、目标工具链、目标 sysroot、内核/用户态/UEFI 产物、镜像和 QEMU 运行；同时保留现有的开发者入口：`make`、`make kernel.bin`、`make disk.img`、`make run`、`make run-aarch64-uefi` 与 `make test`。

## 非目标

- 不切换构建系统或要求安装新的构建工具。
- 不在此设计中解决 Termux 的 `-lgcc` 链接兼容问题；本次保持现有链接参数与成功 profile 的行为，`-lgcc` 到 compiler runtime archive 的替换、发现和验证将由后续独立的工具链规格处理。
- 不修改内核、libc、BusyBox 或 posix-uefi 的源码 ABI。
- 不将 BusyBox 或 posix-uefi 改写为非 Make 构建。
- 不引入联网下载、远程缓存或生成式配置步骤。

## 设计原则

1. 根 Makefile 是唯一的用户入口和跨组件依赖图所有者；组件 Makefile 只构建自己的产物。
2. 依赖由实际文件或表示完整输出树的 stamp 文件表达；`.PHONY` 只用于用户入口和清理操作。
3. 宿主路径与目标路径绝不共用变量或名称。任何组件均不得从父 Make 的环境中隐式取得 `CFLAGS`、`LDFLAGS`、`PREFIX` 或 sysroot。
4. 每个 profile 完整定义目标架构、能力集、编译器、链接器、运行时库、sysroot、UEFI 固件和 QEMU；OS01 自有的递归 Make 显式包含同一份 profile 配置，第三方上游 Make 由 adapter 隔离。
5. 外部项目适配的输入、补丁和输出必须进入依赖图；禁止在第三方工作树内留下构建修改或共享临时文件。
6. 每个迁移阶段保持既有 x86_64 构建和 aarch64 UEFI 启动可用。

## 文件布局与职责

```text
Makefile                         稳定用户入口、包含目标图
mk/project.mk                    源码根、build 根、公共辅助函数
mk/profiles/<profile>.mk         一份完整的 host + target 配置
mk/toolchains/clang.mk           Clang/LLVM 的通用发现与校验
mk/targets/x86_64.mk             x86_64 的链接、UEFI、QEMU 参数
mk/targets/aarch64.mk            aarch64 的链接、UEFI、QEMU 参数
mk/components/sysroot.mk         headers、libc、libk、mbedTLS 产物规则
mk/components/kernel.mk          kernel artifact 规则
mk/components/user.mk            用户程序与 BusyBox artifact 规则
mk/components/uefi.mk            UEFI runtime adapter 与 EFI artifact 规则
mk/components/image.mk           rootfs manifest、staging、磁盘镜像规则
mk/components/run.mk             run/debug/test 入口规则
kernel/Makefile、libc/Makefile、user/Makefile、boot/uefi/Makefile
                                 组件内部编译规则；消费显式配置文件
```

根 Makefile 不再保存 BusyBox、mbedTLS、镜像拷贝或目标架构的具体 recipe。它仅包含 `mk/` 下的模块，并定义用户可见 target 的别名和 capability 校验；`mk/components/run.mk` 提供这些别名的 recipe。`mk/components/*.mk` 是跨组件依赖的唯一位置；组件目录内的 Makefile 不得直接调用其它组件。

## Profile 与工具链契约

调用格式为：

```text
make PROFILE=x86_64-clang image
make PROFILE=aarch64-clang run-aarch64-uefi
```

未指定时 `PROFILE=x86_64-clang`。profile 文件包含目标架构文件和工具链文件，并定义下列语义变量：

| 变量 | 含义 |
| --- | --- |
| `PROFILE` | host + target + toolchain 的唯一名称 |
| `BUILD_DIR` | `build/$(PROFILE)`；本 profile 的全部中间与最终输出 |
| `SYSROOT` | `$(BUILD_DIR)/sysroot`；唯一目标安装根 |
| `TARGET_INCLUDEDIR` | `$(SYSROOT)/usr/include` |
| `TARGET_LIBDIR` | `$(SYSROOT)/usr/lib` |
| `HOST_PREFIX` | 宿主软件前缀，仅供宿主工具探测 |
| `TARGET_TRIPLE` | 编译目标三元组 |
| `TARGET_CC`、`TARGET_LD`、`TARGET_AR` | 目标编译、链接、归档工具 |
| `TARGET_RUNTIME_ARCHIVE` | 直接调用链接器时所需的 compiler runtime archive |
| `UEFI_CC`、`UEFI_LD` | UEFI COFF 专用工具链命令 |
| `QEMU_BIN`、`UEFI_FIRMWARE` | profile 对应的模拟器与固件 |
| `KERNEL_BUILD_DIR`、`LIBC_BUILD_DIR`、`USER_BUILD_DIR` | 组件专有 object 输出目录 |
| `UEFI_BUILD_DIR`、`UEFI_RUNTIME_DIR` | EFI 输出和运行时副本目录 |
| `PROFILE_CAPABILITIES` | 此 profile 支持的 `kernel`、`userland`、`rootfs`、`uefi` 等能力 |

`HOST_PREFIX` 不能参与目标 C/C++ 的 include 或 library 搜索。目标编译仅使用 `TARGET_INCLUDEDIR`、`TARGET_LIBDIR` 和明确允许的 Clang resource directory。不得再使用语义含混的 `PREFIX`、`INCLUDEDIR`、`LIBDIR` 作为跨组件接口。`TARGET_*DIR` 永远表示最终 sysroot，install 规则不得写入这些路径。

组件不得从 `ARCH` 重新推导输出路径；必须消费 profile 导出的绝对 `*_BUILD_DIR` 和 artifact 路径。根模块也只引用这些同源路径。因此 `aarch64-clang` 和旧 `aarch64` 不会写入同一目录，且不会出现根规则等待 `build/$(PROFILE)`、组件却输出 `build/$(ARCH)` 的分裂。

OS01 自有递归 Make 通过 `OS01_PROFILE_FILE=$(abspath mk/profiles/$(PROFILE).mk)` 取得配置，并在组件 Makefile 顶部显式 `include`。根规则在构造子 Make flags 前清空 `MAKEOVERRIDES` 中的白名单外命令行变量；递归调用使用统一的 `os01_submake` 函数。其 recipe 行必须采用可执行形式：

```make
+env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
    MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
    -C <component> OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" <whitelisted-args>
```

行首的 `+` 是 GNU Make recipe 前缀，不属于 shell 命令；`$(MAKE)` 本身前不能再加 `+`。`OS01_SUBMAKEFLAGS` 只保留 GNU Make 选项及 jobserver authentication，不能含 `VAR=VALUE` 赋值，因此 GNU Make 保留 jobserver 文件描述符的继承。受控环境只重建 `PATH`、`HOME`、`TMPDIR`、净化后的 `MAKEFLAGS` 与白名单变量。这样 `CFLAGS`、`CPPFLAGS`、`CXXFLAGS`、`ASFLAGS`、`LDFLAGS`、`CC`、`LD`、`AR`、`PREFIX` 和任意未列环境变量均不能泄漏到子 Make。

允许显式传递的白名单为 `PROFILE`、profile 文件路径、已验证的 `CLANG`/`UEFI_CLANG`/LLVM 工具 override、QEMU override 和已验证的 OS01 功能开关；其他变量一律不可跨层传播。不得 `export CFLAGS` 或依赖调用 shell 已设定的 `CC`。BusyBox 和 posix-uefi 等上游 Makefile 不要求 include profile；adapter 也通过受控环境调用，只向它们传递其已支持的、由白名单派生的工具、flags 和输出目录参数。

profile 同时声明头文件 ABI 规则，而不把规则隐藏在全局 `CFLAGS`：x86 kernel 的 target `<stdint.h>` 强制包含和 `__CLANG_STDINT_H` guard 是 kernel profile flags；UEFI 的 Clang/resource-header 优先级与 posix-uefi 类型兼容补丁属于 UEFI adapter。每项规则必须标明适用组件与 target/host header 来源，并由编译期 typedef 冲突检查覆盖。

链接规则必须标记为 Clang driver 或直接 `ld.lld` 模式，且同一规则不得混用两种模式。本次只集中其定义位置，不改变现有 `-lgcc` 参数或 Termux 运行时库选择；后续工具链规格才会定义并启用 `TARGET_RUNTIME_ARCHIVE` 取代 `-lgcc`。

## 产物图

```text
x86_64 profile:
kernel headers ──> sysroot headers ──┐
libc/libk ───────> sysroot libraries ├─> kernel.bin
mbedTLS ─────────> sysroot library   ├─> user/*.elf / busybox.elf
UEFI runtime ────> BOOTX64.EFI        └─> disk.img ─> run / test

aarch64 UEFI bring-up profile:
kernel.elf + BOOTAA64.EFI + firmware ─> aarch64-uefi.img ─> run-aarch64-uefi
```

`x86_64-clang` 声明 `kernel,userland,rootfs,uefi` 能力。`aarch64-clang` 当前只声明 `kernel,uefi-bringup`：它生成只包含 `kernel.elf` 和 `BOOTAA64.EFI` 的 FAT 镜像，不能依赖 libc、mbedTLS、BusyBox、通用 rootfs 或 x86 测试目标。根模块在解析时根据 capability 拒绝不支持的 target，而非尝试套用 x86 图。

每项跨组件依赖使用稳定、profile 专有的路径：

| 逻辑产物 | 路径 |
| --- | --- |
| 内核头文件安装 | `$(BUILD_DIR)/stamps/kernel-headers.stamp` |
| libc/libk 安装 | `$(BUILD_DIR)/stamps/libc-install.stamp` |
| mbedTLS 安装 | `$(BUILD_DIR)/stamps/mbedtls-install.stamp` |
| 内核 | `$(BUILD_DIR)/artifacts/kernel.bin` |
| 用户程序 | `$(BUILD_DIR)/artifacts/user/<name>.elf` |
| BusyBox | `$(BUILD_DIR)/artifacts/user/busybox.elf` |
| UEFI runtime | `$(BUILD_DIR)/stamps/uefi-runtime.stamp` |
| EFI app | `$(BUILD_DIR)/artifacts/uefi/BOOTX64.EFI` 或 `BOOTAA64.EFI` |
| 磁盘镜像 | `$(BUILD_DIR)/image/disk.img` |

stamp 的 recipe 必须先生成完整输出树，再用原子 `mv` 写入 stamp；stamp 依赖其所有源码、配置和工具链配置文件。单个 archive、ELF、EFI 和 image 直接作为依赖，不能用一个伪 `lib` target 代替。组件的 install target 必须在成功后验证预期文件确实存在。

同一 sysroot 永远只有 `sysroot.mk` 一个 writer。install 调用强制传入 `INSTALL_ROOT=$(BUILD_DIR)/staging/<component>`；组件只允许写 `INSTALL_INCLUDEDIR=$(INSTALL_ROOT)/usr/include` 与 `INSTALL_LIBDIR=$(INSTALL_ROOT)/usr/lib`，不得写 `TARGET_*DIR`。每个 staging tree 同时生成受管路径 manifest。

所有 install stamps 完成后，`sysroot.mk` 从空的 `$(BUILD_DIR)/sysroot-generations/<generation>/` 按 manifests 串行组装和验证完整 sysroot；发生同路径冲突即失败。成功后以原子替换 `$(BUILD_DIR)/sysroot` 的符号链接来发布该 generation，并写入 sysroot stamp。每个 artifact recipe 在其 sysroot prerequisite 完成后只解析一次该符号链接，获得不可变绝对路径 `SYSROOT_GENERATION_DIR`，并把该路径显式传给子 Make 和编译/链接 flags；同一次 artifact 构建不得再次解析 `SYSROOT`。

generation 名称采用 profile 私有、单调递增且不复用的 build invocation id；id 只能在持有 publish lock 时分配。`sysroot.mk` 使用 `mkdir` 原子获取 profile publish lock；锁路径位于 `build/.locks/$(PROFILE)/`，不在可被 profile clean 删除的 `$(BUILD_DIR)` 内。同一 profile 的 publish 和 clean 均持有该锁，不能并行；遗留锁必须由显式诊断/解锁 target 处理，不能由普通构建静默覆盖。

artifact recipe 在持有该锁时原子完成三件事：读取当前 `sysroot` 链接、解析不可变 `SYSROOT_GENERATION_DIR`、在该 generation 建立以 generation + invocation/PID + artifact 命名的 profile 私有 read lease；随后释放锁并只使用已固定的绝对路径。

该“取锁 → 创建 lease → 消费 sysroot → 删除 lease”区段必须由单一 shell 覆盖整个 artifact 编译或子 Make 调用：使用 `.ONESHELL` 的局部规则，或一个以 `+$(SHELL) -ec '...'` 启动的单一 recipe 行；不得把 `trap`、编译和子 Make 分置于默认多行 recipe。shell 的 `trap` 在退出时以原子 `rmdir` 删除其 lease。publish 与 `make clean` 都在持锁状态检查 leases；clean 发现任一 lease 即失败，不删除任何 generation。generation 不自动回收，只有无 lease 且持锁的 `make clean` 才能删除，因此不会移除仍被 artifact recipe 使用的 generation。这样删除或重命名 header/archive 时不会遗留在下一代 sysroot，且消费者始终看到一致视图。不得让平行递归 Make 同时写入 `TARGET_INCLUDEDIR` 或 `TARGET_LIBDIR`。

## 组件边界

`sysroot.mk` 负责按顺序安装 kernel headers、libc/libk 和 mbedTLS。其 recipe 仅调用对应组件 Makefile，且传递 `PROFILE`、组件专有 build directory、`INSTALL_ROOT` 和 profile 文件路径；它是唯一可将 staging tree 合并到 `SYSROOT` 的模块。kernel、user、BusyBox、UEFI 均只依赖 sysroot 的具体 stamp 或 archive，不能自行触发 `make -C libc install`。仅具有 `userland` 能力的 profile 包含这些规则。

`kernel.mk` 负责内核所需的 headers/libraries 与架构专有 linker script；它输出 `artifacts/kernel.bin`。`user.mk` 负责所有用户 ELF 和 BusyBox，使用同一个 sysroot，而 BusyBox 原生构建仍封装在一个 output-stamp recipe 内。`image.mk` 只消费 artifacts，绝不回调 kernel、libc 或 user 构建。

所有生成的 archive、generated assembly、嵌入 binary、配置文件、第三方副本和临时 object 都必须位于相应的 `*_BUILD_DIR` 或 `$(BUILD_DIR)/staging/`。例如 libc archives 不再落在 `libc/`，trampoline binary 不再落在 `kernel/arch/`，不得使用共享 `/tmp` 作为构建目录。源目录仅保留版本控制的输入文件；允许的根 `kernel.bin`/`disk.img` 兼容物由根 Make 从 profile artifact 单向生成。

### BusyBox 与 mbedTLS adapters

`user.mk` 定义 BusyBox adapter：根据版本控制的 BusyBox 输入 manifest 复制源码至 `$(BUILD_DIR)/thirdparty/busybox/`，在该副本中放入 `crt0.S`、sigreturn trampoline、配置和 Kbuild overlay，然后调用其原生 Make。adapter stamp 依赖 manifest、BusyBox 输入清单/固定 revision、overlay、libc/libk stamp、profile 和工具链身份，最终输出 `artifacts/user/busybox.elf`。BusyBox 不得写入 `thirdpart/busybox-1.36.1/`。

`sysroot.mk` 定义 mbedTLS adapter：根据版本控制的输入 manifest 复制 mbedTLS 至 `$(BUILD_DIR)/thirdparty/mbedtls/`，在副本中应用 OS01 配置 overlay，并只在 `$(BUILD_DIR)/thirdparty/mbedtls-build/` 编译 objects。adapter 将 `libmbedtls.a` 安装到其 staging root，随后由 sysroot 单 writer 合并。mbedTLS 不得写入 `thirdpart/mbedtls/` 或共享 `/tmp`。

BusyBox 所需但项目尚未提供实现的 `libm.a`/`librt.a` stub 归 `sysroot.mk` 的 `compat-libs` 规则所有：它使用 profile 专有临时 object 写入 `$(BUILD_DIR)/staging/compat-libs/` 并生成 manifest；随后仅由 sysroot generation assembler 发布到最终 sysroot，并由独立 stamp 追踪。BusyBox 只依赖该 stamp，不创建或修改这些 archive。

### Rootfs manifest

新增版本控制的 `config/rootfs.mk`，用数据项描述每个镜像输入：artifact 源路径、镜像内目标路径、权限以及 BusyBox applet symlink。`image.mk` 从该 manifest 生成 `$(BUILD_DIR)/rootfs/`，再输出磁盘镜像。这样新增程序只改 manifest，不在根 Makefile 的长串 `cp` recipe 中插入逻辑。

## UEFI runtime adapter

`uefi.mk` 封装 posix-uefi：把固定来源复制到 `$(BUILD_DIR)/uefi-runtime/`，应用仓库中版本控制的 patch 或 overlay，再写入 `uefi-runtime.stamp`。runtime 的身份由版本控制的 `thirdpart/posix-uefi.manifest` 定义：来源路径、固定 submodule/vendor revision 或递归输入清单的内容 hash 三者至少具备其一；manifest 生成的输入列表本身是 stamp prerequisite。stamp 还依赖 patch/overlay、profile、UEFI 编译器身份和架构。

第三方源码目录视为只读；构建不可依赖或留下对该目录的 `sed` 修改。adapter 可以调用上游 Makefile，但其 `-j1` 限制、输入/输出目录和原因必须集中在 adapter 内。x86_64 与 aarch64 的 runtime objects 绝不共享。

## 用户入口与兼容性

下列入口保持有效，并由根 Make 映射至 profile 产物：

```text
make                         默认 profile 的 disk image
make kernel.bin              默认 profile 的内核
make disk.img                默认 profile 的磁盘镜像
make run                     默认 profile 启动 QEMU
make run-aarch64-uefi        aarch64-clang profile 启动 QEMU
make test                    默认 profile 的测试
make lib                     默认 userland profile 的 sysroot libraries
make user                    默认 userland profile 的用户 ELF 与 BusyBox
make clean                   清理指定 profile（默认 profile）的全部生成物
```

`lib` 只在具备 `userland` capability 的 profile 可用，输出相应 sysroot library stamps；`user` 同样只在该 capability 下可用，输出所有用户 artifacts。`clean` 取得 profile publish lock、确认不存在 generation read lease 后，仅删除指定 profile 的 build directory；只有 `PROFILE=$(DEFAULT_PROFILE)` 才额外删除由该 profile 拥有的项目根兼容 artifacts。因此它不影响其它 profile。同样保留 `run-kvm`、`run-virtio`、`debug`、`validate`、`test-phase-0`、`test-syscall`、`test-inittab`、`test-network`、`aarch64-uefi` 和 `aarch64-uefi-kernel`；每个 target 都须在 `run.mk` 定义其 profile capability 与输入 artifact。兼容的项目根 `kernel.bin`、`disk.img` 和测试所需镜像是始终存在的具体规则，依赖默认 profile 的 artifact，不能作为可选的复制或软链接措施。

现有开关保留为显式 Make 变量，例如 `LOG_TARGET`、`KERNEL_SELFTEST`、`OS01_SYSTEST`。根 Make 负责验证其取值并传入 profile/组件；子 Make 不读取未声明的环境开关。每种镜像模式是 profile variant，产物路径包含 variant 名称；例如 systest 镜像不得覆盖 normal `disk.img`。

## 分阶段迁移

### 阶段 0：基线

记录默认 x86_64 和 aarch64 UEFI 的最终命令、产物格式和启动签名。为 kernel ELF、EFI 和镜像建立可重复的验证 target，不改变现有默认构建流程。

### 阶段 1：配置去歧义

引入 `mk/project.mk`、profile、toolchain 与 target 文件，但仍由现有组件 Makefile 编译。删除跨组件的 `PREFIX`/`INCLUDEDIR` 语义混用，改用 profile 专有的组件 build directory 与 `SYSROOT`。定义 x86_64 与 aarch64 bring-up 两张 capability 图；此阶段保持旧 target 名称。

### 阶段 2：真实 sysroot 依赖

引入 `sysroot.mk` 和 install stamps，移除根 Make 中的伪 `lib` 依赖。kernel、user 与 BusyBox 转为只依赖具体 sysroot 产物。验证干净构建、无改动重建和头文件修改后的最小重建。

### 阶段 3：UEFI 与镜像边界

实现 UEFI adapter、版本控制的 patch/overlay 和 `rootfs.mk` manifest。根 Make 删除 UEFI 的复制/`sed` recipe、BusyBox/mbedTLS 细节和磁盘文件清单。

### 阶段 4：清理与文档

删除被模块化规则取代的旧变量与重复 recipe，文档说明 profile、产物位置和 toolchain override。保留兼容入口至少一个发布周期。

## 验收与回归防护

- `make clean && make kernel.bin`、`make clean && make disk.img` 在默认 profile 成功；连续第二次 `make disk.img` 不执行无关编译。
- `make -j` 构建 x86 image 与 aarch64 UEFI image 时无共享 sysroot 双写、UEFI runtime 竞态或缺失副产物；profile/variant 切换后无 object、sysroot 或镜像污染。
- 并行构建两个 profile 时，源码目录、`thirdpart/`、`/tmp` 内不会出现新 archive、object、配置、复制的 runtime 或第三方构建修改；全部此类输出位于各自 `build/<profile>/`。
- 修改 `kernel/include/` 中一个头文件后，只有依赖该头文件的目标重新编译；修改 UEFI 源时不重建 libc/user。
- x86_64 kernel ELF 为 `EM_X86_64`、无动态解释器和未定义符号；EFI 为 COFF；`make run` 可由 QEMU/OVMF 启动。
- `make run-aarch64-uefi` 启动至既有 `aarch64 uefi handoff ok` 和 `phase1 boot ok` 签名。
- Termux profile 的 sysroot 必为 `build/<profile>/sysroot/usr`，不得产生 `sysroot/data/data/com.termux/files/usr` 这类把宿主前缀拼入目标根的路径。
- 修改 runtime manifest、runtime 输入、patch/overlay 或工具链身份时只使对应 UEFI runtime/EFI 失效；rootfs manifest 删除或重命名条目后，staging root 中不会遗留旧文件。
- 删除或重命名任一组件安装的 header/archive 后，下一代 sysroot 不包含旧路径；任何递归调用都无法从 shell 环境继承未列入白名单的编译、链接或前缀变量。
- BusyBox 与 mbedTLS 的 manifest、输入、overlay 或工具链身份变化只使对应 adapter/stamp 失效；其源目录在构建前后内容相同。
- 配置解析会打印 profile、target triple、sysroot、链接模式和运行时 archive；不支持的 profile 或工具链组合在编译前失败。
- CI 至少覆盖默认 Linux x86_64、aarch64 UEFI 交叉构建，以及一个 Termux 的配置/依赖图检查。

## 风险与决策

GNU Make 的递归构建不会自动得到完整全局依赖图，因此本设计将跨组件依赖集中在根 `mk/components/*.mk`，并要求子 Make 仅处理自身 outputs。该约束比把所有编译规则合并到单一 Makefile 风险低，也能保留现有外部项目的原生构建方式。

Make 的 include 文件是配置接口而非环境变量替代品。profile 语义一旦固定，Termux 的 runtime archive、Clang 标头或 UEFI 特殊规则可局部修复，不再迫使根 Make 或无关组件增加条件分支。
