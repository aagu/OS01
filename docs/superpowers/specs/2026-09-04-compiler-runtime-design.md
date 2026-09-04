# Compiler Runtime 去 GCC `-lgcc` 依赖设计

## 状态与目标

本设计消除 OS01 目标产物对 GCC `libgcc`、主机 GCC 私有库目录和
`-lgcc` 链接选项的依赖。首个接入者是 x86_64 ELF 内核；设计同时为
x86_64 userland、x86_64 UEFI（COFF/MS ABI）、aarch64 kernel 和 aarch64
UEFI 提供可独立扩展的路径。

默认采用 OS01 自托管 builtin；Clang compiler-rt 是显式、经过验证的
兼容后端。绝不从一个后端静默回退到另一个后端，绝不回退到 GCC。

非目标：本次不改变 libc、BusyBox、posix-uefi 或 aarch64 的链接行为；
它们只有在实际出现 compiler helper 依赖时才接入此组件。

## 现状依据

x86_64 kernel 的 stage1 ELF 当前包含 `__udivti3`，并由 `-lgcc` 满足。
最终 raw `ld.lld` 链接还通过
`$(CLANG) -print-libgcc-file-name` 加入主机 GCC 的私有目录。这使构建
依赖宿主 GCC 布局，并且与 profile 工具链隔离目标冲突。

## 架构

新增顶层 runtime 组件，而非 `kernel/runtime`：

```text
runtime/
  include/os01/compiler_rt.h
  builtins/                 # 跨架构 helper 算法
  arch/x86_64/              # 架构或 ABI 专有实现（需要时）
  arch/aarch64/
  tests/                    # host vector / ABI tests
  Makefile
mk/components/runtime.mk    # provider 解析、variant 构建与发布
```

消费者不接触 provider 或 archive 路径，改为请求链接输入：

```make
$(call runtime_inputs,kernel)
$(call runtime_inputs,user)
$(call runtime_inputs,uefi)
```

每个 runtime variant 的身份为：

```text
<profile>/<consumer>/<target triple>/<object format>/<ABI>/<provider>
```

初始按 consumer 分开构建，即使 kernel 和 user 都是 x86_64 ELF/SysV。
这避免 kernel 的 `-mno-red-zone` 等约束泄漏到 userland；只有日后有明确
的字节码等价验证，才允许显式去重。COFF/MS ABI UEFI archive 绝不能与
ELF/SysV archive 混用。

## Provider 接口

```make
RUNTIME_PROVIDER ?= selfhosted       # selfhosted | compiler-rt
RUNTIME_INPUTS_<consumer>            # consumer 的唯一 runtime 链接输入
```

`RUNTIME_PROVIDER` 只接受两个精确值，未知值在 Make 解析时失败。默认
值由 profile/toolchain 定义，命令行可覆盖，以便 CI 比较。

### selfhosted

为每个 variant 产出 `libos01-builtins.a`。所有编译器 helper 仅在
`runtime/builtins/` 或 `runtime/arch/<arch>/` 实现并由该 archive 导出。
消费者在自身对象及依赖的静态库之后链接该 archive；例如 userland 的
顺序是 `crt0 + 程序对象 + -lc + runtime`，使 libc 的 helper 引用亦可
解析。

初始实现为 Clang builtin ABI 的：

```c
typedef unsigned __int128 u128;
u128 __udivti3(u128 dividend, u128 divisor);
```

函数体用 union 拆为两个 `uint64_t` 字，只能使用 64 位比较、加减、移位
和位测试。不得在该函数中对 `u128` 使用 `/`、`%`、`*`，不得调用 libc
或 libk，以免再次生成 helper 或递归。算法为二进制长除；除数高半为零
时可走 128÷64 快路径并使用 x86 `divq`。除数为零执行显式 trap，而不
产生静默结果。

每个自托管 archive 在发布前必须经过 `llvm-nm -u` 检查：若含任何
compiler runtime helper 未定义引用，构建失败。

### compiler-rt

针对该 variant 的 Clang target、object format 与 ABI flags 查询 runtime
archive。查询可通过兼容接口：

```text
CLANG <variant target flags> -rtlib=compiler-rt -print-libgcc-file-name
```

结果必须是存在的普通 archive，basename 必须匹配
`libclang_rt.builtins-*.a`，路径与文件名不得含 `libgcc`；否则失败。
它直接成为 `RUNTIME_INPUTS_<consumer>`，不生成 OS01 archive，也不允许
降级为 selfhosted 或 GCC。

`TARGET_RUNTIME_ARCHIVE` 旧预留概念被收敛为 variant 私有的、已验证的
输入；不得以全局单一路径表达不同 ABI 的 runtime。

## 消费者接入

1. x86_64 kernel 首先接入。stage1 和 final raw link 都使用同一个
   `RUNTIME_INPUTS_kernel`，保证 kallsyms 双阶段一致。删除
   `KERNEL_RAW_LIBDIR` 中 GCC 目录发现以及两处 `-lgcc`。
2. userland 只有在其对象或 `libc.a` 出现 helper 未定义时接入。链接输入
   放在 `-lc` 后，不改变 libc 的所有权和 ABI。
3. UEFI 只有在 COFF 最终链接的实际未定义符号证明需要时接入。由
   `mk/components/uefi.mk` wrapper 传入 COFF/MS ABI（或 AArch64 UEFI ABI）
   variant；不得修改 posix-uefi 源树或传入 ELF archive。
4. aarch64 以独立的 target/ABI variant 接入，不复用 x86 archive。

新增 helper 的规则：先在 runtime 组件添加独立实现与测试，再将其加入
selfhosted 允许清单。不得临时恢复 `-lgcc`；可显式选用 compiler-rt 作为
短期兼容后端。

## 审计与测试

新增 `make test-runtime`，分别在 `RUNTIME_PROVIDER=selfhosted` 与
`RUNTIME_PROVIDER=compiler-rt` 下运行。初期只覆盖 kernel variant，后续
consumer 接入时扩展同一测试框架。

必须验证：

1. provider 值合法；compiler-rt archive 存在且命名合规。
2. selfhosted 链接含 `libos01-builtins.a`；compiler-rt 链接含选定 archive。
3. 完整链接命令、输入 archive 路径及最终 ELF 都不含 `libgcc`。
4. selfhosted archive 无未定义 runtime helper；最终 kernel ELF 的
   `llvm-nm --undefined-only` 为空。
5. kernel ELF 保持静态、无 `INTERP`/`DYNAMIC`，并通过既有 `_start`、
   `kernel_main`、`_text` 符号和 EM_X86_64 验证。
6. `__udivti3` host 向量测试覆盖 0/1、最大 64/128 位值、高半非零、
   小于/等于/大于、整除/有余数及固定种子随机数据；期望值与宿主
   `unsigned __int128` 除法比较。除零在子进程验证 trap/非零退出。

完成前还须 clean build 两个 provider，并运行 `make test`。自托管默认
模式额外运行 `make KERNEL_SELFTEST=1` 与
`make OS01_SYSTEST=1 test-syscall`，两者必须分开执行。

## 迁移顺序

1. 建立 runtime 组件、variant 解析与 compiler-rt 发现校验。
2. 添加 `__udivti3`、host 向量测试及 archive 自检。
3. 将 x86 kernel 两个链接阶段改为唯一的 runtime 输入接口。
4. 删除 GCC 路径发现与全部 x86 kernel `-lgcc`。
5. 在两个 provider 下执行 `test-runtime`、clean build 与完整测试矩阵。
6. 在构建文档记录默认值、覆写示例、诊断和新增 helper 政策。

## 错误处理原则

配置错误、archive 缺失、archive ABI/名称不匹配、未知 provider、runtime
对象递归依赖和任一未定义 helper 都必须在构建期失败，并明确指出
consumer、variant 与建议动作。禁止隐式搜寻主机 GCC、隐式 provider
切换或把 GCC 路径重新加入 `-L`。
