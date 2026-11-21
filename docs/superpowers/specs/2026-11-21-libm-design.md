# libc 最小真实 libm

日期：2026-11-21。状态：设计评审修订版（v3）；仅设计，不表示已实施。

## 目标、依赖与交付边界

用真实的 musl 1.2.5 基础代数集替换 sysroot 里的**空桩** `libm.a`，让 `-lm` 链接到真正会算的归档，并建立两层验证（宿主对照 glibc + guest 实跑）。

交付边界：

- **16 个符号**：`fabs/copysign/floor/ceil/trunc/round/fmod/sqrt` 及各自的 `f` 变体。不含超越函数（`sin/cos/exp/log/pow/atan/atan2`）——本次不做；`docs/roadmap.md` 的「libm 归档」开放项按此收窄。
- **不交付 long double 变体**。C99 §7.12.7 列出的 `fabsl/sqrtl/floorl/ceill/truncl/roundl/fmodl/copysignl` 本次**明确排除**，`math.h` 不声明它们。理由：x86_64 的 `long double` 是 x87 80-bit、aarch64 是 binary128 quad，前者要 x87，后者要 compiler-rt 软浮点（`__addtf3` 等），两者都会把 OS01 当前没有的依赖拖进来。
- **仅用户态**。内核不链 libm（见「内核边界」）。`libm.a` 不进 `libk.a`。
- 本次**取代** [D01 Doom 前置 libc 补充](2026-11-10-doom-01-libc-design.md) 里的既有决定：该设计要求「fabs 采用现有 math.h 内静态内联位操作实现，不引入 libm archive」。本次把 `fabs` 提升为 `libm.a` 的真实符号，并按 musl 格式改写 `math.h`。

## 来源与许可

musl 1.2.5 官方归档 `musl-1.2.5.tar.gz`，SHA256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`，与 `thirdpart/musl-libc-algorithms.manifest` 钉死的值逐字节一致。MIT 许可，`thirdpart/musl-libc-algorithms/COPYRIGHT` 已在库内。

沿用 `thirdpart/musl-libc-algorithms.manifest` 已建立的 vendoring 纪律：vendored 文件清单、适配 diff 全量记录、补丁单独评审。

## 文件布局

```
libc/math/fabs.c        fabsf.c       copysign.c    copysignf.c
libc/math/trunc.c       truncf.c      floorf.c      ceilf.c
libc/math/fmod.c        fmodf.c       sqrt.c        sqrtf.c
libc/math/sqrt_data.c                            （__rsqrt_tab[128] 表）
libc/math/sqrt_data.h                            （__rsqrt_tab 的声明）
libc/math/floor.c       ceil.c        round.c       roundf.c   （移植，见适配）
libc/math/musl_libm.h                               （私有头）
```

`sqrt_data.h` 必须与 `sqrt_data.c` 同目录：`sqrt.c` 用 `#include "sqrt_data.h"` 取 `__rsqrt_tab` 的声明，漏掉它会变成隐式声明并丢掉表的外部链接属性。

私有头 `libc/math/musl_libm.h` **放在源目录，不进 `libc/include/`**，因此不会被 `install-headers` 发布进 sysroot —— musl 的 `asuint64/asdouble/FORCE_EVAL/predict_false` 不应出现在用户态头文件里。`#include "musl_libm.h"` 按同目录相对包含解析，宿主测试编译同一批源时不需要额外 `-I`。

## 适配 diff

musl 原版依赖 musl 运行时环境，OS01 不具备的部分逐条适配并记录：

1. `#include "libm.h"` → `#include "musl_libm.h"`。私有头提供 `asuint64/asdouble/asuint/asfloat`、`FORCE_EVAL`、`predict_false/predict_true`。不引入 musl 的 `libm.h`（它带 `long double` union、`fp_arch.h`、`WANT_ROUNDING` 等 musl 内部机制）。
2. `isnan(y)` → **整数位检测**（指数全 1 且尾数非 0）。`y != y` 在 x86_64 会生成 `ucomisd` 比较指令，破坏「无浮点算术」性质。
3. `__math_invalid(x)` → 返回固定 quiet NaN 位模式。
4. **错误路径按「errno 契约」拆开**：musl 把「NaN 输入」与「域错误」合并在同一条返回 NaN 的路径上（`fmod` 的 `uy.i<<1 == 0 || isnan(y) || ex == 0x7ff`），OS01 必须按契约表的顺序分别判定，见「errno 契约」。
5. `FENV_SUPPORT` → 0。musl 的 `sqrt` 在 `FENV_SUPPORT=1` 时会用一次 `y + t` 的浮点加法去制造舍入模式与 inexact 效果。
6. 删除 `FORCE_EVAL(x + 0x1p120f)` 的 inexact 触发。OS01 不实现 FP 异常标志，保留它只会引入浮点指令。
7. `floor/ceil`（double）与 `round/roundf` → 纯整数位运算实现（详见「移植理由」）。
8. `fmod/fmodf` 的 NaN 路径 `return (x*y)/(x*y);` → 返回固定 quiet NaN。这是 `fmod` 里唯一的浮点算术。
9. `fmod/fmodf` 的带符号零路径 `return 0*x;` → 整数构造带符号零（保留 x 的符号位，值位清零）。`0*x` 是浮点乘法。
10. `sqrt/sqrtf` 的次正规数归一化 → **整数重建**（公式见下节，这是 v2 的错误修正）。
11. NaN 返回值统一为**固定** quiet NaN：double `0x7ff8000000000000`、float `0x7fc00000`。不依赖各架构的硬件默认 NaN（x86 的无效运算默认 NaN 带负号，ARM 的不带），保证跨架构结果一致。
12. 去掉 `hidden` 可见性标注。

适配后，全部 16 个函数的函数体**不产生浮点算术指令**（只有位操作与内存搬运；`sqrt` 是定点 Goldschmidt 迭代 + 位重建，`fmod` 是整数长除）。这条性质是「内核侧理论上可链」的前提，见「内核边界」。

### 次正规数归一化（v2 公式的修正）

musl 用一次浮点乘法做次正规数归一化：`ix = asuint64(x * 0x1p52);`（`sqrtf` 是 `x * 0x1p23f`）。**v2 给出的整数公式是错的**：它保留了尾数位不变并固定指数域为 1，对 `0x0000000000000002` 得到 `0x0010000000000002`，而正确的浮点结果是 `0x0020000000000000`——指数与尾数都错，后续 Goldschmidt 迭代会对错误的值开方。

正确规则（按尾数最高置位重建指数与尾数）：

- binary64：`m = ix & 0xfffffffffffff`（52 位尾数字段），`p` = `m` 的最高置位位置（0…51）；
  `frac = (m - (1ULL << p)) << (52 - p)`；`biased = p + 1`；归一化位模式 = `(biased << 52) | frac`。
- binary32：`m = ix & 0x7fffff`（23 位），`p` = 最高置位位置（0…22）；
  `frac = (m - (1ULL << p)) << (23 - p)`；`biased = p + 1`；位模式 = `(biased << 23) | frac`。

归一化后 musl 的 `top` 仍按原逻辑处理（`top = ix >> 52; top -= 52;`），因为归一化后的位模式与浮点乘法的结果逐位相同。

该公式已实测：最小次正规数（`m=1`）、次小（`m=2`）、最大次正规数（`m=2^52-1`）、`m=2^51` 边界与 20 万次随机次正规数抽样，全部与 `x * 0x1p52` 的浮点结果**逐位一致**；v2 公式在上述每个用例上都失配。

### 移植理由（事实更正）

原稿把 `floor/ceil/round` 的整数移植理由写成「x86_64 上 `FLT_EVAL_METHOD==2`，musl 会取 `double_t = long double`」。**这条不成立**：本机 clang 配 OS01 头文件实测 `__FLT_EVAL_METHOD__ == 0`（x86_64 与 aarch64 目标编译均为 0），musl 原版会走 `EPS = DBL_EPSILON` 的纯 double 分支，其 toint 技巧本身成立。

本次仍做整数移植，理由改为：

1. **消除浮点算术**——第 2/6/8/9/10 条适配的前提，也是「内核侧理论上可链」这条性质的来源。
2. **不依赖 excess precision 语义**——toint 技巧的正确性依赖「中间和的 ulp 恰为 1」这一浮点表示事实；位运算实现不依赖编译器对表达式的求值精度约定，行为跨架构确定。
3. **不引入 long double**——整数实现避免 `double_t`/`float_t` 与 `LDBL_EPSILON` 出现在 vendored 源码里。

## `libc/include/math.h` 改写为 musl 格式

现文件只有 `math_fp_signbit`/`math_fp_isfinite` 两个静态内联与 `fabs` 内联。改写后：

- 16 个函数声明（musl 书写风格：`double      fabs(double);`）。
- `INFINITY`/`NAN`/`HUGE_VAL`/`HUGE_VALF`。`INFINITY`/`NAN` 的替换列表必须与 `libc/include/float.h:54-55` 里的**字面一致**，避免宏重定义告警。
- `MATH_ERRNO`/`MATH_ERREXCEPT`/`math_errhandling`。**只声明 `MATH_ERRNO`**（OS01 没有 fenv，不实现 `MATH_ERREXCEPT`），并按 C99 §7.12.1 让这个承诺成立——见下节。
- `FP_NAN/FP_INFINITE/FP_ZERO/FP_SUBNORMAL/FP_NORMAL`。
- 分类宏 `isnan/isinf/isfinite/isnormal/signbit/fpclassify`：用 clang builtin 实现（`__builtin_isnan` 等），**不用** musl 的 `__fpclassify*` 辅助函数——那会引入新的链接符号，且 musl 的 `sizeof(x)` 分派写法在传入 int 时会落到 `__fpclassifyl`。

### errno 契约

`math_errhandling = MATH_ERRNO` 意味着「实现对域错误与范围错误设置 `errno`」。C99 §7.12.1 要求域错误置 `errno = EDOM`。本次 16 个函数中只有两个函数有域错误，且**分支顺序必须写死**（先判 NaN，再判域错误）：

`sqrt(x)` / `sqrtf(x)`：

| 序 | 条件 | 结果 | `errno` |
|---|---|---|---|
| 1 | `x` 是 NaN | quiet NaN（固定位模式） | 不变 |
| 2 | `x < 0`（含 `-inf`） | quiet NaN（固定位模式） | `EDOM` |
| 3 | `x == ±0` | `±0` | 不变 |
| 4 | `x == +inf` | `+inf` | 不变 |
| 5 | 其余有限正数 | `sqrt(x)` | 不变 |

`fmod(x, y)` / `fmodf(x, y)`：

| 序 | 条件 | 结果 | `errno` |
|---|---|---|---|
| 1 | `x` 或 `y` 是 NaN | quiet NaN（固定位模式） | 不变 |
| 2 | `x` 是 `±inf`（含 `y` 也是 `±inf`） | quiet NaN（固定位模式） | `EDOM` |
| 3 | `y == 0`（`x` 有限） | quiet NaN（固定位模式） | `EDOM` |
| 4 | `y` 是 `±inf`（`x` 有限） | `x`（原值，含符号） | 不变 |
| 5 | 其余有限 | 精确余数 | 不变 |

依据（本机 glibc 实测）：`fmod(±inf, 2)` 返回 NaN 并置 `EDOM`；`fmod(NaN, 0)` 与 `fmod(inf, NaN)` 保留原 `errno`；`fmod(2.0, inf) == 2.0` 不置 `errno`。

- **范围错误**：本次 16 个函数全部精确（`sqrt` 正确舍入、`fmod` 精确取余），不产生范围错误，因此不设 `ERANGE`。
- `errno` 在 OS01 用户态 libc 里是普通全局（`libc/errno/errno.c`：`#if !defined(__is_libk) int errno = 0; #endif`），libm 设置它与其它 libc 函数无差别；`libm.a` 不进 `libk`，`errno` 也不在 `libk` 里，两侧一致。

### 其它后果与连带修改

- `fabs` 不再是头内内联 → 调用者必须链 `-lm`。受影响的编译单元已逐一排查：
  - **Doom 引擎**：`thirdpart/doomgeneric/doomgeneric/v_video.c:868` 调用 `fabs(mouse_acceleration - 1)`；`v_video.c` 在 `thirdpart/doomgeneric-sources.mk` 的单元表里，`user/doom/engine.mk:81` 用 `-fno-builtin` 编译（编译器不会用 builtin 兜底），而 `user/doom/app.mk` 只链 `-lgui -lgfx`。移除头内实现后该引用需要归档符号 → **`user/doom/app.mk` 必须加 `-lm`**（`APP_LDLIBS := -lgui -lgfx -lm`）。
  - **BusyBox**：`fabs` 的真实调用点在 `networking/ntpd.c`，而 `config/busybox.config.in:949` 是 `# CONFIG_NTPD is not set` → 该调用不参与编译；BusyBox 仍带 `-lm`（`Makefile.flags:149` 的 `LDLIBS += m`），链接顺序上 `-lm` 在对象之后，解析到新归档。
  - **LVGL**：`config/lv_conf.h:1078` 是 `#define LV_USE_SVG 0` → `lv_svg_parser.c` 的 `sinf/cosf` 不参与编译。
  - **libc 自身**：`libc/stdlib/floatscan.c` 用的是自己的 `fs_fabs`，不是 `math.h` 的。
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

- `libc/Makefile`：新增 `LIBM_C_SOURCES := $(wildcard math/*.c)`，`LIBM_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libm.a`，对象落在 `$(BUILD_DIR)/math/%.o`；`install-libs` 增加 `cp $(LIBM_ARCHIVE) $(INSTALL_ROOT)/usr/lib/libm.a`。libm **不进 libk**（libk 是内核子集，`aarch64` 还受 `-mgeneral-regs-only` 约束）。`libc/math/*.c` 不得被加进 `LIBC_C_SOURCES`——那是 `libc.a` 的源清单。
- `mk/components/sysroot.mk`：compat-libs **停止产出 `libm.a`**，只保留 `librt.a` 空桩。
  - **必须同时清除 staging 里的旧桩**：compat-libs 的 manifest 是用 `find` 扫描整个 staging 目录生成的，已有构建目录里的旧 `usr/lib/libm.a` 会被重新写进 manifest，与 libc stage 的新归档撞成重复目标，发布器报 `ERROR: duplicate destination in generation` 硬失败。做法：在生成 manifest **之前** `rm -f $(STAGING_DIR)/compat-libs/usr/lib/libm.a`。
  - 验收必须覆盖这一点：**从已含旧空桩的构建目录跑一次增量发布**（当前 `build/x86_64-clang/staging/compat-libs/usr/lib/libm.a` 就是旧桩），必须成功发布新 generation，`sysroot/usr/lib/libm.a` 是新归档（不是空桩），且不报重复目标。
- 用户态链接：`user/Makefile` 的 `user_lib_dep` 已把 `-l%` 映射到 `$(SYSROOT)/usr/lib/lib%.a` → `-lm` 自动解析到新归档，无需改链接规则。
- `user/doom/app.mk`：`APP_LDLIBS := -lgui -lgfx -lm`（见「其它后果与连带修改」）。

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
- x86_64 内核技术上可以链纯整数实现的 libm（double 走内存、不碰 xmm，不破坏用户态 FP 状态；内核的 FP 状态只在上下文切换时 FXSAVE/FRSTOR，系统调用入口不保存）。
- 但那会让两架构的内核侧 API 分裂（aarch64 必须是指针/位级），而内核当前**没有任何数学调用点**。

结论：**内核不链 libm，保持 FP-free 策略**。若将来内核确有数学需求，走「指针/位级 API」的单独立项，不在本次范围内。

## 测试与验收

### 宿主层（对照 glibc）

- 新增 `hosttests/cases/test_libc_math.c`（`@os01-test` 元数据 + `link: libc-math`）。
- `hosttests/mk/libc.mk` 新增规则：用 `HOST_CC` 编译**同一批** `libc/math/*.c`，加前缀改名防止宿主 glibc 的 libm 插桩；注册 `LINK_GROUP_libc-math`。
  - **16 个数学符号全量改名**：`-Dfabs=os01_fabs -Dcopysign=os01_copysign …`（含 `f` 变体），沿用仓库既有的 `OS01_LIBC_PREFIX` 约定。
  - **`errno` 必须符号隔离**：libm 对象按 OS01 的 `<errno.h>` 编译，引用的是**普通全局** `errno`；宿主的 glibc `<errno.h>` 里 `errno` 是 TLS 变量。两者混用会得到确切错误（已实测复现）：
    ```
    ld: errno: TLS definition in /usr/lib/libc.so.6 section .tbss mismatches non-TLS reference in <obj>.o
    ld: /usr/lib/libc.so.6: error adding symbols: bad value
    ```
    处理：libm 对象加 `-Derrno=os01_errno`，测试二进制提供**自己的普通全局** `int os01_errno = 0;`，断言只读写 `os01_errno`。宿主 glibc 的 TLS `errno` 与之无关（两者可共存且互不干扰，已实测：宿主 `errno=42` 与 `os01_errno=7` 同时成立）。仓库既有先例：`hosttests/mk/libc.mk` 的 time 组注释「the test provides the plain `int errno` the libc declares (the host's errno is a TLS symbol)」。
- 比对基准是宿主 glibc 的同名函数（测试 TU 用宿主 `<math.h>`）。**比较策略分三类**，不能一刀切逐位相等：

  | 类别 | 断言 |
  |---|---|
  | 有限值、±inf、±0 | 与 glibc **逐位一致** |
  | NaN 产生路径（`sqrt` 负数、`fmod` 除零、`fmod` 无穷被除数、NaN 输入） | 断言返回值**等于 OS01 的固定 quiet NaN 位模式**（double `0x7ff8000000000000`、float `0x7fc00000`）+ `errno` 期望；**不与 glibc 逐位比较** |
  | `fabs`/`copysign` | 额外断言符号位与载荷：`fabs` 清符号位且保留 NaN 载荷，`copysign` 只改符号位、保留载荷 |

  NaN 不能与 glibc 逐位比较的原因（已实测）：glibc 的 `fmod(1.0, 0.0)` 返回 `0xfff8000000000000`（**负** quiet NaN），而 `fmod(1.0, NaN)` 会把输入 NaN 的载荷带进结果；musl 两条路径都返回自己的默认 NaN；本次统一返回固定的正号 quiet NaN（适配第 11 条）。因此宿主断言要**直接对照 OS01 的常量位模式**，不能只靠 `isnan()` 做分类判断——分类判断无法发现「返回了 NaN 但位模式不对」。
- errno 断言：调用前把 `os01_errno` 置为哨兵值，域错误路径后应为 `EDOM`，其他路径后应保持不变。
- 次正规数用例：`sqrt`/`sqrtf` 必须覆盖最小次正规数、次小、最大次正规数与 `m=2^51` 边界（两个精度各一组），逐位比较期望值。
- `make regen-test-catalog` 重新生成测试目录。

### guest 层（实跑）

- 新增 `user/systest/test_libm.c`：**必须同时加入 `user/systest/app.mk` 的 `APP_SOURCES`**（该文件显式枚举源文件，不加就不会被编译，用例根本不存在）。
- `user/systest/app.mk` 增加 `APP_LDLIBS := -lm`（`user/Makefile` 的 `user_lib_dep` 会把 `-lm` 映射到 `$(SYSROOT)/usr/lib/libm.a`）。
- 在 `user/systest/systest.c` 的 `tests[]` 表注册**稳定用例 ID**：`{"64_libm", "libm", test_libm, 1, 0}`（required=1）。ID 选 `64_libm` 是因为 `55`–`63`、`65` 已占用，`64` 空缺。
- **用例必须实际调用全部 16 个函数并断言结果**（不接受「只注册不计算」的用例）：
  - 有限值：与硬编码的**期望位模式**逐位比较（不用浮点比较，避免容差掩盖错误）；
  - ±inf、±0：位模式比较；
  - 域错误路径（`sqrt` 负数、`fmod` 除零、`fmod` 无穷被除数）：断言返回固定 quiet NaN 位模式 **且** `errno == EDOM`；
  - NaN 输入路径：断言返回固定 quiet NaN 位模式 **且** `errno` 未被改动；
  - `fabs`/`copysign`：断言符号位与 NaN 载荷。
  - 16 个符号逐个点名，任何一个没被调用都算用例不完整。
- 验收证据：
  1. 单用例：`make OS01_SYSTEST=1 test-qemu SUITE=systest CASE=64_libm` → `mk/components/run.mk` 写 `config/inittab.systest.case`（`tty1:once:/bin/systest --case 64_libm`），guest 跑单个用例，host 侧 `parse_v1` 校验协议后打印 `PASS: case 64_libm passed`。
  2. 全量：`make OS01_SYSTEST=1 test-syscall` → trace 含 `[TEST] PASS 64_libm`，END 行 `failed=0`。
  3. 「全绿但没跑到」必须能被判失败：`parse_v1` 要求 SELECT/BEGIN/PASS 一一对应，用例缺失或被 SKIP 会让协议校验失败；因此验收以 **该 ID 的 PASS 记录** 为准，不以整体退出码为准。

### aarch64 验收

aarch64 没有 systest E2E 路径，guest 层验收只在 x86_64 进行。aarch64 的验收是**构建与安装产物**：

- `build/aarch64-clang/sysroot/usr/lib/libm.a` 存在，且 `llvm-nm` 导出全部 16 个符号（不导出 `fabs` 之外的名字，不与 `libc.a` 重复定义）。
- aarch64 的 `libc.a`/`libk.a` 构建不回归。

### 静态审计

- 对两个 profile 的 `libm` 对象做反汇编检查，确认没有浮点算术指令：x86_64 无 `addsd/subsd/mulsd/divsd/sqrtsd` 等 SSE 算术（`movsd` 之类的内存搬运允许），aarch64 无 `fadd/fsub/fmul/fdiv/fsqrt/fcvt`。这是「无浮点算术」性质的机器可验证证据，也是内核边界结论的前提。

### 回归

- `make test-host` 全量。
- **Doom 实际链接**：构建并链接 `doom.elf`，确认 `v_video.c` 的 `fabs` 解析到新的 `libm.a`（不是未定义符号，也不是宿主 glibc 的 libm）。这是 `fabs` 外移的主要回归面。
- BusyBox 仍能链接并启动（`-lm` 从空桩变真实归档）。
- `make test-static` 静态审计。

## 文档更新

- `thirdpart/musl-libc-algorithms.manifest`：新增一节，记录 vendored 文件清单、12 条适配 diff、SHA256，以及 `fs_fabs` 与 musl `fabs` 同源的说明。
- `docs/roadmap.md`：「libm 归档」开放项从「无 libm archive」收窄为「已有基础代数集归档（16 符号），超越函数仍未实现」。
- `docs/build/build.md`：sysroot 发布阶段的 compat-libs 描述同步（只含 `librt.a` 桩）。

## 完成条件

`libm.a` 进入 sysroot 且内容真实；宿主测试全绿（含 errno 隔离、NaN 固定位模式、次正规数用例）；systest 在 guest 内实跑并留下 `64_libm` 的 PASS 记录，且该用例确实调用了全部 16 个函数；**从已含旧空桩的构建目录增量发布成功**；Doom 与 BusyBox 链接无回归；x86_64 与 aarch64 两个 profile 的 libc/libk 构建通过，且 aarch64 的 sysroot 里有符号完整的 `libm.a`。
