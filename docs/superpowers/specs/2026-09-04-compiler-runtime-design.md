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

消费者不接触 provider 或 archive 路径，改为请求链接输入和构建前置：

```make
$(call runtime_prereqs,kernel)
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

profile 必须为每个 consumer 声明并规范化
`RUNTIME_TARGET_<consumer>`、`RUNTIME_CFLAGS_<consumer>`、
`RUNTIME_OBJECT_FORMAT_<consumer>` 与 `RUNTIME_ABI_<consumer>`。这些值、
provider、Clang identity/resource-dir 和 runtime 源摘要共同进入 variant
输出目录及 receipt。selfhosted kernel 因而使用 freestanding、
`-mno-red-zone`、no-stack-protector 等 kernel 约束；user/UEFI 不继承它们。

## Provider 接口

```make
RUNTIME_PROVIDER ?= selfhosted       # selfhosted | compiler-rt
RUNTIME_PREREQS_<consumer>           # provider/variant-keyed receipt 或 archive target
RUNTIME_INPUTS_<consumer>            # consumer 的唯一 runtime 链接输入
```

`RUNTIME_PROVIDER` 只接受两个精确值，未知值在 Make 解析时失败。默认
值由 profile/toolchain 定义，命令行可覆盖，以便 CI 比较。

每个消费 ELF 将 `runtime_prereqs` 列为正常 prerequisite，并将
`runtime_inputs` 追加在所有对象和静态库之后。receipt 的 key 包含 provider
及完整 variant，因此 provider、target/ABI flags、Clang identity 或 runtime
源变化都会重建 archive 并触发消费者重链；`make -j` 也不会在 archive 生成
前开始链接。

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
或 libk，以免再次生成 helper 或递归。limb 操作必须显式处理 carry/borrow，
禁止 shift-by-64。初版使用不溢出的二进制长除；未来仅当
`divisor.hi == 0`、`divisor.lo != 0` 且 `dividend.hi < divisor.lo` 才可使用
x86 `divq` 快路径，否则仍走长除，避免 `divq` 的 quotient-overflow fault。
除数为零执行显式 trap；该路径也必须通过对象未定义符号审计。

每个自托管 object（而非只检查 archive）在发布前必须经过
`llvm-nm -u` 检查：默认不允许任意未定义符号；若将来确有外部依赖，必须
使用按 variant 记录的精确 allowlist。另对
`^__(u|s|multi3|aeabi_|div|mod|ash|lsh|fix|float)` 等 runtime-helper 前缀
执行严格禁止检查，防止实现自身递归。

### compiler-rt

针对该 variant 的 Clang target 查询 runtime archive；查询仅负责发现候选，
不证明 archive 具有 consumer flags（例如 kernel `-mno-red-zone`）的等价
编译约束。可使用兼容接口：

```text
CLANG <variant target flags> -rtlib=compiler-rt -print-libgcc-file-name
```

候选必须是存在的 regular archive：ELF 接受 `libclang_rt.builtins.a` 或
`libclang_rt.builtins-<arch>.a`，COFF 接受 `clang_rt.builtins.lib` 或已由
profile 明确登记的工具链等价形式。路径与文件名均不得含 `libgcc`。
必须以选定 `llvm-ar` 枚举并在临时目录逐个提取 member，再以
`llvm-readobj --file-headers` 验证所有成员同为该 variant 的 object format
与 machine（ELF/EM_X86_64、ELF/EM_AARCH64、COFF/x64 或 COFF/ARM64）；
混合或不匹配即失败。

kernel variant 还必须拥有 profile 声明的 compiler-rt eligibility manifest，
记录 archive 摘要、Clang identity/resource-dir、目标 ABI 及无 red-zone 等
kernel 安全审计结论；没有该 manifest 时 `compiler-rt` 对 kernel 是明确
不支持的 provider，而不是风险回退。候选通过所有适用校验后才成为
`RUNTIME_INPUTS_<consumer>`。不生成 OS01 archive，也不允许降级为
selfhosted 或 GCC。

`TARGET_RUNTIME_ARCHIVE` 旧预留概念被收敛为 variant 私有的、已验证的
输入；不得以全局单一路径表达不同 ABI 的 runtime。

## 消费者接入

1. x86_64 kernel 首先接入。stage1 和 final raw link 都使用同一个
   `RUNTIME_INPUTS_kernel`，保证 kallsyms 双阶段一致。删除
   `KERNEL_RAW_LIBDIR` 中 GCC 目录发现以及两处 `-lgcc`。
2. userland 只有在其对象或 `libc.a` 出现 helper 未定义时接入。链接输入
   放在 `-lc` 后，不改变 libc 的所有权和 ABI。
3. UEFI 只有在 COFF 最终链接的实际未定义符号证明需要时接入。复制后的
   posix-uefi runtime overlay 必须在最终链接规则提供受控
   `OS01_RUNTIME_INPUTS` hook，并将其追加在 `$^` 之后（例如
   `LIBS += $(OS01_RUNTIME_INPUTS)`）；wrapper 显式传递此变量，不覆盖
   既有 `LIBS`/`-out:`。overlay/patch、最终命令和 COFF archive 进入 receipt。
   不得修改上游源码树或传入 ELF archive。x86_64 使用 COFF/MS x64 ABI；
   aarch64 使用 PE/COFF AArch64 UEFI ABI。
4. aarch64 以独立的 target/ABI variant 接入，不复用 x86 archive。
5. BusyBox 是独立 consumer；初期不宣称它已得到 runtime 覆盖。当其首度
   出现 helper 引用时，以 `runtime_prereqs,busybox`/`runtime_inputs,busybox`
   接入其私有构建 receipt，并在全部 BusyBox 静态库之后链接。

新增 helper 的规则：先在 runtime 组件添加独立实现与测试，再将其加入
selfhosted 允许清单。不得临时恢复 `-lgcc`；可显式选用 compiler-rt 作为
短期兼容后端。

## 审计与测试

新增 `make test-runtime`，它必须先构建所选 kernel variant，并审计实际的
stage1 与 final 链接输入，不能只依赖 host `make test`。初期覆盖 kernel
variant，后续 consumer 接入时扩展同一测试框架。

必须验证：

1. provider 值合法；compiler-rt archive 发现、regular-file、成员、格式、
   machine 与 kernel eligibility manifest 均合规。
2. selfhosted 链接含 `libos01-builtins.a`；compiler-rt 链接含选定 archive。
3. 每 build 写入 provider/variant-keyed link-command receipt；审计 kernel
   stage1（Clang driver）与 final（raw ld.lld）两条实际命令均恰好一次地在
   所有静态库之后包含同一 resolved runtime 输入，且命令、输入路径与产物
   均不含 `libgcc`。
4. selfhosted archive 无未定义 runtime helper；最终 kernel ELF 的
   `llvm-nm --undefined-only` 为空。
5. kernel ELF 保持静态、无 `INTERP`/`DYNAMIC`，并通过既有 `_start`、
   `kernel_main`、`_text` 符号和 EM_X86_64 验证。
6. `__udivti3` host 向量测试覆盖 0/1、最大 64/128 位值、高半非零、
   小于/等于/大于、整除/有余数及固定种子随机数据。期望商由不链接被测
   `__udivti3` 的独立生成器预先写入向量，避免 host `unsigned __int128 /`
   被测试定义 interpose。除零在子进程验证 trap/非零退出。
7. 增加目标对象/目标链接 smoke test，证明每个接入 variant 的 helper ABI
   与 provider 可实际解析；后续 UEFI/aarch64 接入时增加其 target-specific
   ABI 测试。
8. 增加静态库顺序负向测试：一份故意引用唯一符号的 consumer archive 与
   runtime archive，在 runtime 放于 consumer 前时必须链接失败，而放于全部
   consumer archive 后必须成功。这保护 kernel 的 `-lk + runtime` 与未来
   user 的 `-lc + runtime` 顺序。

每个 provider 都从新鲜 profile build 运行；默认 selfhosted 为必测，
compiler-rt 仅在该 profile 提供有效 eligibility manifest 时为必测，否则
应验证其清晰拒绝。kernel provider matrix 的顺序为：

```text
make clean && make RUNTIME_PROVIDER=selfhosted test-runtime validate-kernel test
make clean && make KERNEL_SELFTEST=1 test-kernel-selftest
make clean && make OS01_SYSTEST=1 test-syscall
```

若 profile 提供有效 kernel eligibility manifest，第二个、同样从 clean
开始的必测命令是：

```text
make clean && make RUNTIME_PROVIDER=compiler-rt test-runtime validate-kernel test
```

否则该命令是负向测试，必须在 provider 解析/eligibility 阶段以明确诊断
失败，且不得启动任何 GCC 回退。

`test-kernel-selftest` 是 x86 rootfs 的自动 QEMU 串口测试 target：先以
`KERNEL_SELFTEST=1` 构建专用镜像，再以受限时长启动 QEMU、捕获 serial log
并要求依次出现 `[selftest] running built-in tests...`、最终
`[selftest] N total: N passed, 0 failed`（N 为同一正整数）及
`[selftest] done`。超时、缺任一标记或任一失败计数均失败。它必须和
`test-syscall` 分开运行，且不能以仅构建镜像的 `make KERNEL_SELFTEST=1`
代替。

`make test` 是 host 测试，不能代替 `test-runtime`/`validate-kernel`。
后续 aarch64 接入时增加 `PROFILE=aarch64-clang` 的 profile-aware target，
明确构建其实际 kernel 路径并检查 ELF `EM_AARCH64` 与 UEFI COFF `ARM64`；
不能复用 x86-only validate。userland 接入时加入强制生成 `__udivti3` 的
fixture，在两 provider 下审计全部 user 链接并在 QEMU 运行。

## 迁移顺序

1. 建立 runtime 组件、variant 解析与 compiler-rt 发现校验。
2. 添加 `__udivti3`、host 向量测试及 archive 自检。
3. 将 x86 kernel 两个链接阶段改为唯一的 runtime 输入接口。
4. 删除 GCC 路径发现与全部 x86 kernel `-lgcc`。
5. 在 selfhosted 及每个已声明 eligible 的 compiler-rt provider 下执行
   `test-runtime`、clean build 与完整测试矩阵；同时运行 provider 发现的
   负向测试（无效 provider、空/失败查询、目录/非 archive、错误命名、
   `libgcc` 路径、错误或混合 member ABI/format），并确认没有 fallback。
6. 在构建文档记录默认值、覆写示例、诊断和新增 helper 政策。

## 错误处理原则

配置错误、archive 缺失、archive ABI/名称不匹配、未知 provider、runtime
对象递归依赖和任一未定义 helper 都必须在构建期失败，并明确指出
consumer、variant 与建议动作。禁止隐式搜寻主机 GCC、隐式 provider
切换或把 GCC 路径重新加入 `-L`。
