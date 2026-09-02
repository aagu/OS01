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

`kernel/Makefile`、`libc/Makefile`、`user/Makefile`、`boot/uefi/Makefile` 与 `kernel/arch/x86_64/make.config` 必须要求 `OS01_PROFILE_FILE` 存在；缺失时在解析期报错并提示从根目录调用，例如 `make PROFILE=x86_64-clang kernel.bin`。

删除各组件的 legacy 分支：

- `ARCH ?=` 默认选择和以 `build/$(ARCH)` 计算输出目录；
- 组件自行 include 根 `toolchain.mk`；
- 组件直接写项目根 `kernel.bin`、`disk.img`、旧 sysroot 或 source-tree archive；
- `boot/uefi/Makefile` 中复制 posix-uefi、`sed -i` 修改 runtime 和 legacy OVMF 构建路径。

根 Makefile 继续提供现有公开 target，并通过 profile 模块调用组件。组件的 `all`、`install`、`clean` 是根模块内部的实现入口，不再是独立受支持的开发接口。

`toolchain.mk` 删除；其中仍需要的 Clang/LLVM 检测只保留于 `mk/toolchains/clang.mk`，并由 profile include。文档中的全部构建示例使用 `make PROFILE=<name> <target>`。

## 保留的 int8 兼容 patch

保留 `config/posix-uefi/0001-clang-int8.patch`。其将 posix-uefi fallback 的 `int8_t` 从 plain `char` 改为 `signed char`，与 Clang `<stdint.h>` 的类型一致。

此 patch 是对已固定 submodule revision 的本地兼容层；adapter 继续把它作为 runtime receipt 的输入，并仅在 profile-private runtime copy 上应用。不得通过调整 OS01 的 `<stdint.h>` include 顺序、伪造 `_STDINT_H` guard 或全局宏来规避该 patch。

## 删除 runtime Makefile overlay

删除 `config/posix-uefi/0002-runtime-make-overlay.patch`。其两项行为改为 adapter 调用约定：

| 旧 overlay 行为 | 替代机制 |
| --- | --- |
| `CFLAGS += -DUEFI_NO_UTF8` | adapter 以受控环境 `CFLAGS=-DUEFI_NO_UTF8` 启动外层 posix-uefi Make；环境变量会被上游的 `CFLAGS +=` 追加，不以命令行变量覆盖上游原有 COFF flags。 |
| 内层 `make ... OUTDIR=` | adapter 将外层 Make 的 `MAKE` 变量显式设为带 `OUTDIR=` 的递归命令，使上游 `uefi/libuefi.a` recipe 调用其内层 Make 时清空 `OUTDIR`，而外层应用对象仍使用 profile 的 `OUTDIR`。 |

在删除 patch 前，必须以 profile-private runtime copy 做探针：x86_64 与 aarch64 分别构建 EFI，确认 runtime archive 的对象仍在 runtime 目录、应用对象仍在 profile UEFI output 目录、且 `-j1` 的既有上游限制仍被 adapter 保持。探针失败时，不删除 `0002`；应记录上游 Make 的变量传播原因并返回设计评审，不以新 patch 或 source-tree `sed` 替代。

## 输入、隔离与验证

UEFI receipt 输入改为：posix-uefi gitlink、`0001-clang-int8.patch`、profile、UEFI 编译器身份、boot source hash 及 adapter invocation contract；不再含 `0002`。adapter 继续拒绝未初始化或非干净的 posix-uefi submodule。

验收条件：

- `rg 'ifndef OS01_PROFILE_FILE|else.*legacy|toolchain.mk' kernel libc user boot/uefi kernel/arch/x86_64` 不再找到 legacy 构建分支或 component-level toolchain include。
- `make -C kernel`, `make -C libc`, `make -C user` 与 `make -C boot/uefi` 均在解析期失败，并提示使用根 Make + profile。
- `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang disk.img validate` 成功。
- `make PROFILE=aarch64-clang clean && timeout 25 make PROFILE=aarch64-clang run-aarch64-uefi` 在预期 timeout 前输出 `aarch64 uefi handoff ok` 与 `phase1 boot ok`。
- `config/posix-uefi/0002-runtime-make-overlay.patch` 不存在，runtime receipt 与 adapter 无此引用；`0001-clang-int8.patch` 仍存在且被成功应用。
- posix-uefi submodule 与所有 OS01 source tree 在构建前后保持不变；全部 runtime 与 EFI 输出位于 `build/<profile>/`。

## 风险与回滚

profile-only 改动会破坏历史的 `make -C` 工作流，这是有意的不兼容变更；解析期报错必须给出根入口替代命令。删除 `0002` 的主要风险是上游 Make 对 `OUTDIR`、`MAKE` 或环境 `CFLAGS` 的递归传播与当前假设不一致。实施按两次独立提交进行：先删除 legacy 路径并验证，再在单独提交中探针和删除 `0002`；第二次提交若验证失败不合入，第一项不受影响。
