# Profile-only 构建与 UEFI Overlay 缩减设计

## 目标

在既有 GNU Make profile 构建体系上完成两项收尾：

1. 删除所有不通过 profile 的 OS01 组件构建路径。
2. 删除 posix-uefi 的 `0002-runtime-make-overlay.patch`，但保留 `0001-clang-int8.patch`。

完成后，根 Makefile 是唯一支持的入口；所有 OS01 构建均通过 `PROFILE` 和 `OS01_PROFILE_FILE` 获得配置。posix-uefi 仍由 profile-private adapter 复制并构建，但运行时 Makefile 不再被 OS01 patch 修改。

## 非目标

- 不修改或提交 posix-uefi 上游代码，不创建 fork，不提 PR。
- 不删除 `config/posix-uefi/0001-clang-int8.patch`。
- 不处理 Termux 的 `-lgcc` 链接问题。
- 不改变 x86_64、aarch64 的输出 ABI、UEFI 入口、kernel boot ABI 或公开根 Make target 名称。

## Profile-only 规则

`kernel/Makefile`、`libc/Makefile`、`user/Makefile`、`boot/uefi/Makefile`、`test/Makefile`、`kernel/arch/x86_64/make.config` 与 `kernel/arch/aarch64/make.config` 必须要求 `OS01_PROFILE_FILE` 存在；缺失时在解析期报错并提示从根目录调用，例如 `make PROFILE=x86_64-clang kernel.bin`。

删除各组件的 legacy 分支：

- `ARCH ?=` 默认选择和以 `build/$(ARCH)` 计算输出目录；
- 组件自行 include 根 `toolchain.mk`；
- 组件直接写项目根 `kernel.bin`、`disk.img`、旧 sysroot 或 source-tree archive；
- `test/Makefile` 写入 source-tree `test/build`，或由 root 以不带 profile 的 `make -C test` 调用；
- `test/Makefile` 的根级 focused target `build/test_poll_requested.elf`；它删除或迁为 `$(HOST_TEST_BUILD_DIR)/test_poll_requested.elf`。如需保留便利入口，只能由根 Makefile 以受控、profile-aware alias 提供；
- `boot/uefi/Makefile` 中复制 posix-uefi、`sed -i` 修改 runtime 和 legacy OVMF 构建路径。

根 Makefile 继续提供现有公开 target，并通过 profile 模块调用组件。组件的 `all`、`install`、`clean` 是根模块内部的实现入口，不再是独立受支持的开发接口。

`mk/targets/x86_64.mk` 定义 `OVMF_FIRMWARE ?= $(BUILD_DIR)/firmware/OVMF.fd`，以及 `OVMF_FIRMWARE_SOURCE ?= https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd`。root-owned firmware real-file rule 仅接受 HTTPS URL（下载到临时文件后原子 rename）或一个已存在的绝对本地文件路径（content-guarded copy）；其他值在下载前以清晰错误拒绝。`mk/components/run.mk` 使用该规则；全部 x86 QEMU 入口——`run`、`run-kvm`、`run-virtio`、`debug`——的前置条件和 QEMU `-drive` 参数都引用 `$(OVMF_FIRMWARE)`，不再以 `make -C boot/uefi OVMF.fd` 下载固件，也不读取 source-tree 的 firmware。

`test-phase-0`、`test-syscall`、`test-inittab`、`test-network` 均把 `$(OVMF_FIRMWARE)` 作为 capability-gated prerequisite，并在其 `tests/run_test.py` recipe 中显式传递 `OVMF_FIRMWARE="$(OVMF_FIRMWARE)"`（保留各自的 `DISK_IMG` 传递）。测试脚本要求此变量存在且指向可读文件；缺失或无效时在启动 QEMU 前清晰失败，绝不回退到 source-tree 路径。独立运行脚本的调用者也必须显式提供该变量。

新增 capability-gated 根 target `print-run-paths`，输出当前 profile 的绝对 `OVMF_FIRMWARE` 与 `NORMAL_IMAGE` 路径；现行手动 QEMU 文档先用 `make PROFILE=x86_64-clang print-run-paths` 获得这两条路径，再启动 QEMU。更新仍面向当前用户的构建文档，使它们以 root profile target 取得或使用 firmware，而不再要求写入 `boot/uefi/OVMF.fd`；历史设计记录不作追溯性改写。

所有 x86 QEMU 入口直接使用 profile 的 `$(NORMAL_IMAGE)`（或测试的 variant image），项目根 `disk.img` 仅是默认 profile 的兼容 copy，不能作为这些入口的输入。只有 `PROFILE == DEFAULT_PROFILE` 定义该 real-file copy rule；非默认 rootfs profile 的同名公开 `disk.img` target 必须是 phony 的 profile-local alias，只依赖 `$(NORMAL_IMAGE)`，绝不创建、更新或以根 `disk.img` 的已有时间戳满足自己。这保证将来出现另一个 rootfs profile 时，不会从或向默认 profile 的兼容文件串扰。

`clean` 不得再递归调用任何 component Makefile：它在 root 层取得 profile publish lock、检查 generation lease、删除该 profile 的 `build/<profile>/`，并且仅在默认 profile 时删除根兼容 artifacts。这样 profile-only component 的解析期要求不会破坏 `make PROFILE=x86_64-clang run`、测试或 `make PROFILE=x86_64-clang clean`。

宿主单测也属于 OS01 构建：profile 定义 `HOST_TEST_BUILD_DIR := $(BUILD_DIR)/host-test`（以及按需的 host compiler contract）；`test/Makefile` include profile 后仅以该目录作为对象和二进制输出。根 `test` target 在 capability gate 后通过 `os01_submake` 调用它，不能直写 `make -C test run`。因此 profile clean 删除 host tests，且 `make -C test` 在解析期失败；host-test 不使用 target triple 或 sysroot，但同样服从 profile 输出隔离。

`toolchain.mk` 删除；其中仍需要的 Clang/LLVM 检测只保留于 `mk/toolchains/clang.mk`，并由 profile include。迁移面向用户的 [`docs/build/toolchain.md`](../../build/toolchain.md)、[`docs/build.md`](../../build.md)、[`docs/build-run-debug.md`](../../build-run-debug.md)、[`docs/boot.md`](../../boot.md) 和仓库 [`AGENTS.md`](../../../AGENTS.md) 中的现行操作说明：删除 standalone `toolchain.mk`、`make -C boot/uefi` / `make -C kernel` 和 source-tree `boot/uefi/OVMF.fd` 的用法。所有现行构建示例使用 `make PROFILE=<name> <target>`；手动 QEMU 示例使用由 profile build 目录解析出的 firmware/image 路径。历史 specs/plans/report 不作追溯性改写。

## 保留的 int8 兼容 patch

保留 `config/posix-uefi/0001-clang-int8.patch`。其将 posix-uefi fallback 的 `int8_t` 从 plain `char` 改为 `signed char`，与 Clang（包括 Termux Clang）`<stdint.h>` 的类型一致。

此 patch 是对已固定 submodule revision 的本地兼容层；adapter 继续把它作为 runtime receipt 的输入，并仅在 profile-private runtime copy 上应用。不得通过调整 OS01 的 `<stdint.h>` include 顺序、伪造 `_STDINT_H` guard 或全局宏来规避该 patch。

## 删除 runtime Makefile overlay

删除 `config/posix-uefi/0002-runtime-make-overlay.patch`。其两项行为改为 adapter 调用约定：

| 旧 overlay 行为 | 替代机制 |
| --- | --- |
| `CFLAGS += -DUEFI_NO_UTF8` | adapter 传递专用 `UEFI_RUNTIME_CFLAGS=-DUEFI_NO_UTF8` 给 boot wrapper；wrapper 只在启动外层 posix-uefi Make 时执行 `env CFLAGS="$(UEFI_RUNTIME_CFLAGS)" ... $(MAKE)`。该值不是通用 `os01_submake` 的 `CFLAGS` override；上游的 `CFLAGS +=` 会在外层及 `uefi/` 内层追加此环境值，且不会以命令行变量覆盖既有 COFF flags。 |
| 内层 `make ... OUTDIR=` | adapter 传递专用 `UEFI_RUNTIME_MAKE="$(MAKE) OUTDIR="` 给 boot wrapper；wrapper 在同一外层 runtime 调用中以 `env MAKE="$(UEFI_RUNTIME_MAKE)"` 启动上游 Make。上游 `uefi/libuefi.a` recipe 的 `$(MAKE)` 因而执行带 `OUTDIR=` 的内层 Make，而外层应用对象继续使用 profile 的 `OUTDIR`。 |

在删除 patch 前，必须以 profile-private runtime copy 做探针：x86_64 与 aarch64 分别构建 EFI，保存 runtime 外层与 `uefi/` 内层的实际命令日志，并确认两类 compile command 都含 `-DUEFI_NO_UTF8`；确认 runtime archive 对象仍在 runtime 目录、应用对象仍在 profile UEFI output 目录、且 `-j1` 的既有上游限制仍被 adapter 保持。探针失败时，不删除 `0002`；应记录上游 Make 的变量传播原因并返回设计评审，不以新 patch 或 source-tree `sed` 替代。

## 输入、隔离与验证

UEFI receipt 输入改为：posix-uefi gitlink、`0001-clang-int8.patch`、profile、UEFI 编译器身份、boot source hash，以及 `mk/components/uefi.mk`、`boot/uefi/Makefile` 的内容 hash 和最终展开的 `UEFI_RUNTIME_CFLAGS` / `UEFI_RUNTIME_MAKE` 值；不再含 `0002`。因此改变 runtime 环境变量、递归 Make 传递或 wrapper 启动方式时，adapter 必须重建 profile-private runtime，而不能复用带旧对象的 staged copy。adapter 继续拒绝未初始化或非干净的 posix-uefi submodule。

验收条件：

- `rg 'toolchain.mk|build/\$\(ARCH\)|Legacy standalone|ifndef OS01_PROFILE_FILE|test/build|build/test_poll_requested\.elf' kernel libc user boot/uefi test` 不再找到 component-level legacy 分支或 toolchain include（包括两个 kernel arch config）。
- `make -C kernel`, `make -C libc`, `make -C user`, `make -C boot/uefi` 与 `make -C test` 均在解析期失败，并提示使用根 Make + profile；传入不存在的 `OS01_PROFILE_FILE=/nonexistent` 时同样以清晰的 profile-file 错误失败。
- `docs/build/toolchain.md`、`docs/build.md`、`docs/build-run-debug.md`、`docs/boot.md` 与 `AGENTS.md` 不再把 `toolchain.mk`、component `make -C` 或 `boot/uefi/OVMF.fd` 描述为当前支持接口；历史 specs/plans/report 可保留其当时的命令记录。
- `make PROFILE=x86_64-clang run -n`、`run-kvm -n`、`run-virtio -n`、`debug -n`、`test-phase-0 -n`、`test-syscall -n`、`test-inittab -n`、`test-network -n` 与 `clean -n` 不包含未经 profile 的 `make -C` component 调用；所有 QEMU 命令与 `tests/run_test.py` 调用都使用同一个 `OVMF_FIRMWARE`，并显式依赖它。脚本缺少该环境变量时必须失败，不得回退到 source-tree firmware。
- `OVMF_FIRMWARE_SOURCE` 的 HTTPS 与绝对本地文件两条 fixture 分支都生成 profile-private firmware；无效 URL、相对路径或不存在的本地文件清晰失败。`make PROFILE=x86_64-clang print-run-paths` 输出该 profile 的绝对 firmware 和 normal image 路径。
- 对一个非默认 rootfs profile（在 fixture 中声明）运行相同的 dry-run 断言：QEMU 入口输入是该 profile 的 `NORMAL_IMAGE` 和 firmware，而不是根 `disk.img`；执行其 `disk.img` target 后，预置的根 `disk.img` 哈希不变，且实际输出只在该 profile build 目录。x86 E2E smoke 至少验证一次在 firmware 不预先存在时，target 会先生成 profile firmware 再启动 runner。
- `make PROFILE=x86_64-clang test` 的 host-test 对象/二进制（包括 focused poll test）只位于 `build/x86_64-clang/host-test`；`clean` 后该目录不存在，且整个过程不会创建或修改 `test/build` 或根级 `build/test_poll_requested.elf`。
- `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang disk.img validate` 成功。
- `make PROFILE=aarch64-clang clean && timeout 25 make PROFILE=aarch64-clang run-aarch64-uefi` 在预期 timeout 前输出 `aarch64 uefi handoff ok` 与 `phase1 boot ok`。
- `config/posix-uefi/0002-runtime-make-overlay.patch` 不存在，runtime receipt 与 adapter 无此引用；`0001-clang-int8.patch` 仍存在且被成功应用。
- 改变 `UEFI_RUNTIME_CFLAGS`、`UEFI_RUNTIME_MAKE`、`mk/components/uefi.mk` 或 `boot/uefi/Makefile` 的 fixture 输入时，receipt digest 变化且 profile-private runtime 被重建。
- posix-uefi submodule 与所有 OS01 source tree 在构建前后保持不变；全部 runtime、EFI 输出和下载的 x86 firmware 位于 `build/<profile>/`。

## 风险与回滚

profile-only 改动会破坏历史的 `make -C` 工作流，这是有意的不兼容变更；解析期报错必须给出根入口替代命令。删除 `0002` 的主要风险是上游 Make 对 `OUTDIR`、`MAKE` 或环境 `CFLAGS` 的递归传播与当前假设不一致。实施按两次独立提交进行：先删除 legacy 路径并验证，再在单独提交中探针和删除 `0002`；第二次提交若验证失败不合入，第一项不受影响。
