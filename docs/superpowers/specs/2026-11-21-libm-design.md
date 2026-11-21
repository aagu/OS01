# libc 最小真实 libm

日期：2026-11-21。状态：设计评审通过；仅设计，不表示已实施。

## 目标、依赖与交付边界

用真实的 musl 1.2.5 基础代数集替换 sysroot 里的**空桩** `libm.a`，让 `-lm` 链接到真正会算的归档，并建立两层验证（宿主对照 glibc + guest 实跑）。

交付边界：

- **16 个符号**：`fabs/copysign/floor/ceil/trunc/round/fmod/sqrt` 及各自的 `f` 变体。不含超越函数（`sin/cos/exp/log/pow/atan/atan2`）——本次不做；`docs/roadmap.md` 的「libm 归档」开放项按此收窄。
- **仅用户态**。内核不链 libm（见「内核边界」）。`libm.a` 不进 `libk.a`。
- **不含 long double 变体**（`fabs*l` 等）。C99 不要求 `l` 变体；x86_64 的 `long double` 是 x87 80-bit、aarch64 是 binary128 quad，引入它们会把 x87/compiler-rt 软浮点拖进用户态。

本次**取代** [D01 Doom 前置 libc 补充](2026-11-10-doom-01-libc-design.md) 里的既有决定：该设计要求「fabs 采用现有 math.h 内静态内联位操作实现，不引入 libm archive」。本次把 `fabs` 提升为 `libm.a` 的真实符号，并按 musl 格式改写 `math.h`。

## 来源与许可

musl 1.2.5 官方归档 `musl-1.2.5.tar.gz`，SHA256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`，与 `thirdpart/musl-libc-algorithms.manifest` 钉死的值逐字节一致。MIT 许可，`thirdpart/musl-libc-algorithms/COPYRIGHT` 已在库内。

沿用 `thirdpart/musl-libc-algorithms.manifest` 已建立的 vendoring 纪律：vendored 文件清单、适配 diff 全量记录、补丁单独评审。

## 文件布局

```
libc/math/fabs.c        fabsf.c       copysign.c    copysignf.c
libc/math/trunc.c       truncf.c      floorf.c      ceilf.c
libc/math/fmod.c        fmodf.c       sqrt.c        sqrtf.c
libc/math/sqrt_data.c                            （__rsqrt_tab[128] 表）
libc/math/floor.c       ceil.c       round.c       roundf.c     （移植，见适配）
libc/math/musl_libm.h                                         （私有头）
```

私有头 `libc/math/musl_libm.h` **放在源目录，不进 `libc/include/`**，因此不会被 `install-headers` 发布进 sysroot —— musl 的 `asuint64/asdouble/FORCE_EVAL/predict_false` 不应出现在用户态头文件里。`#include "musl_libm.h"` 按同目录相对包含解析，宿主测试编译同一批源时不需要额外 `-I`。

## 适配 diff

musl 原版依赖 musl 运行时环境，OS01 不具备的部分逐条适配并记录：

1. `#include "libm.h"` → `#include "musl_libm.h"`。私有头提供 `asuint64/asdouble/asuint/asfloat`、`FORCE_EVAL`、`predict_false/predict_true`。不引入 musl 的 `libm.h`（它带 `long double` union、`fp_arch.h`、`WANT_ROUNDING` 等 musl 内部机制）。
2. `isnan(y)` → `y != y`。OS01 的 `math.h` 不提供 `isnan` 宏（改写后提供的是 clang builtin 宏，但 vendored 源不依赖它）。
3. `__math_invalid(x)` → 返回 NaN 位模式常量。musl 用它触发 `FE_INVALID`；OS01 没有 fenv，不实现异常。
4. `FENV_SUPPORT` → 0。musl 的 `sqrt` 在 `FENV_SUPPORT=1` 时会用一次 `y + t` 的浮点加法去制造舍入模式与 inexact 效果；关掉后 `sqrt` 变成纯整数路径。
5. 删除 `FORCE_EVAL(x + 0x1p120f)` 的 inexact 触发。OS01 不实现 FP 异常标志，保留它只会引入浮点指令。删掉后 `floor/ceil/trunc` 是纯整数位运算。
6. `floor/ceil`（double）与 `round/roundf` 改为纯整数位运算实现。musl 原版用 `1/EPS` 的 toint 技巧，其正确性依赖 `FLT_EVAL_METHOD`：x86_64 上 `FLT_EVAL_METHOD==2`，musl 会取 `double_t = long double`、`EPS = LDBL_EPSILON`——那会把 x87 拖进用户态。改用 musl **自己 float 版的位运算风格**移植到 double（musl 的 `floorf/ceilf` 本来就是纯位运算），语义不变：`floor`/`ceil` 精确，`round` 保持「半远离零」。
7. `fmod/fmodf` 的 NaN/无穷路径 `return (x*y)/(x*y);` → 返回 NaN 位模式常量。musl 用一次浮点乘除制造 quiet NaN 并触发 `FE_INVALID`；OS01 没有 fenv，这条路径是全部 16 个函数里**唯一的浮点算术**，去掉后 `fmod` 变成纯整数。返回值语义不变（quiet NaN）。
8. 去掉 `hidden` 可见性标注。

适配后，全部 16 个函数的函数体**不产生浮点算术指令**（只有位操作与内存搬运；`sqrt` 是定点 Goldschmidt 迭代 + 位重建，`fmod` 是整数长除）。这条性质是「内核侧理论上可链」的前提，见「内核边界」。

## `libc/include/math.h` 改写为 musl 格式

现文件只有 `math_fp_signbit`/`math_fp_isfinite` 两个静态内联与 `fabs` 内联。改写后：

- 16 个函数声明（musl 书写风格：`double      fabs(double);`）。
- `INFINITY`/`NAN`/`HUGE_VAL`/`HUGE_VALF`。`INFINITY`/`NAN` 的替换列表必须与 `libc/include/float.h:54-55` 里的**字面一致**，避免宏重定义告警。
- `MATH_ERRNO`/`MATH_ERREXCEPT`/`math_errhandling` = `MATH_ERRNO`（OS01 无 fenv，只承诺 errno 语义）。
- `FP_NAN/FP_INFINITE/FP_ZERO/FP_SUBNORMAL/FP_NORMAL`。
- 分类宏 `isnan/isinf/isfinite/isnormal/signbit/fpclassify`：用 clang builtin 实现（`__builtin_isnan` 等），**不用** musl 的 `__fpclassify*` 辅助函数——那会引入新的链接符号，且 musl 的 `sizeof(x)` 分派写法在传入 int 时会落到 `__fpclassifyl`。

后果与连带修改：

- `fabs` 不再是头内内联 → 调用者必须链 `-lm`。第一方代码无 `fabs()` 调用者（`libc/stdlib/floatscan.c` 用自己的 `fs_fabs`），无现存程序被打断；BusyBox 本来就带 `-lm`。
- **`hosttests/cases/test_libc_float_parse.c` 的 Layer 4 契约变更**：该测试断言「`fabs` 的实现在头里」，且已用 `-Dfabs=os01_fabs` 前缀改名防止宿主 libm 插桩。改写后实现来自 `libm.a`，该 TU 需链接 vendored 的 fabs 对象。断言意图（**宿主 glibc 的 `fabs` 绝不能顶替**）完全保留，实现来源从「头内内联」变为「归档真实符号」。同一文件对 `signbit()/isfinite()` 的使用不变——改写后的 `math.h` 仍提供这两个宏。

## `fs_fabs` 收敛为纯整数位运算

`fs_fabs` 是浮点 ABI 门面（`libc/include/libc_arch/<arch>/float_abi.h`）的一部分，服务于 `floatscan.c`/`strtod.c` 的**扫描器工作类型** `fs_float_t`（x86_64 = `long double`，aarch64 = `double`）。

**不改成调用 `libm.a` 的 `fabs` 符号**，两个硬理由：

1. x86_64 上 `fs_float_t` 是 x87 80-bit `long double`，换成 double 版 `fabs` 会砍掉 strtod 的工作精度，`hosttests/data/floatscan_cases.h` 里的 80-bit 位模式断言会直接失败。
2. `floatscan.o`/`strtod.o` 一旦引用归档里的 `fabs`，`libc.a` 就反向依赖 `libm.a` → 所有只链 `-lc` 的用户程序（`init.elf`、`terminal.elf` …）链接失败。

**改为纯整数位运算实现**，与 musl 的 `fabs` 同一套语义：

- x86_64：清 80-bit 格式的符号位（`ux.i.se &= 0x7fff`），与既有 `fs_copysignl` 的整数访问方式一致。
- aarch64：清 bit63（`ux.i &= 0x7fffffffffffffffULL`）。

语义与 `__builtin_fabsl`/`__builtin_fabs` 完全一致（都是清符号位，NaN 载荷保留），因此浮点解析的现有断言不受影响。

## 构建集成

- `libc/Makefile`：新增 `LIBM_C_SOURCES := $(wildcard math/*.c)`，`LIBM_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libm.a`，对象落在 `$(BUILD_DIR)/math/%.o`；`install-libs` 增加 `cp $(LIBM_ARCHIVE) $(INSTALL_ROOT)/usr/lib/libm.a`。libm **不进 libk**（libk 是内核子集，`aarch64` 还受 `-mgeneral-regs-only` 约束）。
- `mk/components/sysroot.mk`：**compat-libs 停止产出 `libm.a`**，只保留 `librt.a` 空桩。必须同步改：libc stage 与 compat-libs stage 若都声明 `usr/lib/libm.a`，generation assembler 的重复目标检查会报 `ERROR: duplicate destination in generation` 硬失败。compat-libs 的注释与阶段语义同步更新。
- 用户态链接：`user/Makefile` 的 `user_lib_dep` 已把 `-l%` 映射到 `$(SYSROOT)/usr/lib/lib%.a` → `-lm` 自动解析到新归档，无需改链接规则。
- BusyBox：`thirdpart/busybox-1.36.1/Makefile.flags:149` 的 `LDLIBS += m` 让最终链接带上 `-lm`（`LDLIBS = "m rt"`，见 `.busybox_unstripped.cmd`），`-L<sysroot>/usr/lib` 在最前 → 从此命中真实归档而不是宿主 glibc 的 libm。

## 内核边界：内核不链 libm

内核的浮点策略是显式禁用的：

- `kernel/arch/x86_64/make.config:20`：`-mno-80387 -mno-sse -mno-sse2 -mno-3dnow -mno-mmx`。
- `kernel/arch/aarch64/make.config:19`：`-mgeneral-regs-only`。
- `kernel/arch/x86_64/cpu/switch.c:179` 的注释写明意图：*"The kernel never uses FPU (-mno-sse -mno-80387), but user programs may."*

实测编译结论：

| 形态 | x86_64（`-mno-sse`） | aarch64（`-mgeneral-regs-only`） |
|---|---|---|
| `double f(double)` 按值 | 可编译（double 退化为内存传递） | 编译失败：`requires 'double' type support, but ABI 'aapcs' does not support it` |
| `void f(const double*, double*)` | 可编译 | 可编译 |

因此：

- **aarch64 内核永远不能链按值 double 签名的 libm**，这是编译期硬约束。
- x86_64 内核技术上可以链纯整数实现的 libm（double 走内存、不碰 xmm，不破坏用户态 FP 状态；内核的 FP 状态只在上下文切换时 FXSAVE/FRSTORE，系统调用入口不保存）。
- 但那会让两架构的内核侧 API 分裂（aarch64 必须是指针/位级），而内核当前**没有任何数学调用点**。

结论：**内核不链 libm，保持 FP-free 策略**。若将来内核确有数学需求，走「指针/位级 API」的单独立项，不在本次范围内。

## 测试与验收

### 宿主层（对照 glibc）

- 新增 `hosttests/cases/test_libc_math.c`（`@os01-test` 元数据 + `link: libc-math`）。
- `hosttests/mk/libc.mk` 新增规则：用 `HOST_CC` 编译**同一批** `libc/math/*.c`，加 `-Dfabs=os01_fabs -Dfloor=os01_floor …` 前缀改名，防止宿主 glibc 的 libm 插桩；注册 `LINK_GROUP_libc-math`。
- 比对基准是宿主 glibc 的同名函数（测试 TU 用宿主 `<math.h>`）。**逐位比对**（位模式完全一致），覆盖：±0、±inf、NaN、subnormal、±0.5、±2.5、±1.5、极大/极小、随机采样。
- `make regen-test-catalog` 重新生成测试目录。

已知需实测确认的点：musl 的 `sqrt` 正确舍入，预期与 glibc 逐位一致；若出现差异，以 musl 为准并在 manifest 记录容差，不放宽断言掩盖问题。

### guest 层（实跑）

- 新增 `user/systest/test_libm.c` systest 用例，`user/systest/app.mk` 增加 `APP_LDLIBS := -lm`。
- 该用例在 guest 内调用 16 个符号并断言硬编码的期望位模式，输出 PASS/FAIL。
- 运行入口：`make OS01_SYSTEST=1 test-syscall`（顶层调用，不与 `KERNEL_SELFTEST=1` 组合）。这同时验证 `-lm → $(SYSROOT)/usr/lib/libm.a` 的真实链接链路。

### 回归

- `make test-host` 全量。
- BusyBox 仍能链接并启动（`-lm` 从空桩变真实归档）。
- `make test-static` 静态审计。

## 文档更新

- `thirdpart/musl-libc-algorithms.manifest`：新增一节，记录 vendored 文件清单、8 条适配 diff、SHA256，以及 `fs_fabs` 与 musl `fabs` 同源的说明。
- `docs/roadmap.md`：「libm 归档」开放项从「无 libm archive」收窄为「已有基础代数集归档（16 符号），超越函数仍未实现」。
- `docs/build/build.md`：sysroot 发布阶段的 compat-libs 描述同步（只含 `librt.a` 桩）。

## 完成条件

`libm.a` 进入 sysroot 且内容真实；宿主测试逐位对照 glibc 全绿；systest 在 guest 内实跑全绿；BusyBox 与既有用户程序链接无回归；x86_64 与 aarch64 两个 profile 的 libc/libk 构建通过。
