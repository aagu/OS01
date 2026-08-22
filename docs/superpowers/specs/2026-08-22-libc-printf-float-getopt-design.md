# libc 兼容性修复设计（printf 浮点 + getopt）— 修订版 v5

- 日期：2026-08-22（v5 修订）
- 验收基线：`docs/applet-verification.md`（BusyBox Applet 验证报告 2026-08-18）
- 范围：仅修复代码中**已确证**的两个 libc 缺陷——`printf` 格式化器（无符号路径缺失 + 浮点缺失 + 大小保护缺失）与 `getopt`（逻辑错误 + 公开头多重定义）。
- 方针：最小范围、最低风险；不引入外部依赖（除非浮点部分明确选择成熟 dtoa）。

## 0. 修订说明（v1 → v2）

根据评审，v1 存在不成立断言与遗漏，本版修正：

- **P0**：`%lu`/`%u` 无符号格式化**并未被正确处理**——`vsprintf.c` 用 `va_arg(args, long)` 取 `%lu`、用 `va_arg(args, int)` 取 `%u`，再交给接收**有符号 `long`** 的 `number()`；高位为 1 的 `unsigned long` 被当负数参与除法/取余，`%u` 高位值被符号扩展。这很可能就是 `cksum` 异常根因。本版将“无符号路径”列为 **printf 修复的必做项**。
- **P1（入口点）**：v1 “只改 `vsprintf.c` 即覆盖 printf 家族”不成立。`vsnprintf.c`/`sprintf.c`/`snprintf.c` 直接调用 `vsprintf(buf, …)`，而 `vsprintf` 写入 `buf+4096` 无视调用者缓冲区大小；`snprintf` 的事后截断并不提供真实大小保护，`size<4096` 时越界。本版要求先抽取带 `end`/容量的核心格式化器，再让所有 wrapper 正确接入。
- **P1（getopt 行为）**：v1 仅描述 POSIX 短选项，遗漏 BusyBox 已依赖的 `optind=0` 完整复位、`+`/`-` optstring 前缀。重写可能回归 applet。本版明确这些契约。
- **P1（验收可复现）**：v1 把主验收设为 homeserver `/tmp` 的脚本（fsroot 副本还是 gitignored 产物）。本版要求把测试输入/期望入仓、写清打包与失败码。
- **P2（浮点契约）**：v1 允许 `%Lf`“按 double 近似”（UB）、`%a/%A` 行为“实现时定”，不能作为实现规格。本版固定为显式不支持且不读取该实参。
- **P2（浮点算法）**：朴素 `frac*=10` 不足以支撑正确 `%f/%e/%g`（舍入进位、极端值、`%g` 有效数字、`#` 语义）。本版要求采用成熟 double→decimal 或逐例验收的窄承诺。
- **公开头缺陷**：`libc/include/getopt.h` 将 `optarg/optind/opterr/optopt` 定义为实体（非 `extern`），多 TU 包含即多重定义。本版纳入 getopt 修复。

### 0.1 修订说明（v2 → v3）
- **P0（`ll` 修饰符）**：v2 §3.3 只覆盖 `l`，但 BusyBox `du` 用 `%llu`、`sum` 用 `%u %llu`，代码库还有大量 `%lld/%llx/%llo`。当前解析器只识别单个 qualifier，`%llu` 会被当作 `l` + 未知 `l` 原样输出/错位取参。本版加入连续 `ll` 解析、`long long`/`unsigned long long` 的 `va_arg` 与转换路径，以及 `du`/`sum` 真实回归用例。
- **P1（`vformatter` 接口）**：v2 的 `vformatter(char *buf, char *end, …)` 对 `snprintf`/`vasprintf` 不足——`NULL` 推进指针、`end=buf+size`、`str-buf` 算长度均有 UB；且 `printf.c`/`stdio_file.c` 按返回长度从固定 4 KiB 缓冲写出会越界读。本版改为“输出槽容量 + 独立总字符计数”接口，并明确各 wrapper 语义与 `vasprintf` 双 pass。
- **P1（getopt `-` 前缀缺 `optarg`）**：返回 `1` 时必须 `optarg = argv[optind]` 并递增 `optind`、复位 `optpos`（BusyBox `unzip` 在 `case 1` 直接使用 `optarg`）。补测试：连续多个非选项。
- **P2（成熟浮点未冻结）**：musl 路径与 D.M. Gay `dtoa` 非可互换单文件；`dtoa` 只做转换，`%f/%e/%g` 的 flags/宽度/精度/`#`/缓冲仍需自集成。本版冻结具体上游（musl `fmt_fp`）、版本、许可证、依赖与适配边界。
- **措辞修正**：v2 称 `%lu`“因负数除法错误”不准确——现有 `do_div` 用 `divq` 对位模式做无符号除法；真正缺陷是 `va_arg` 类型不匹配与 `%u` 符号扩展。类型正确的无符号路径仍是正确的修复方向。

### 0.2 补充修订（v3 → v3.x，本轮）
- **P0（musl `fmt_fp` 集成边界）**：v3 误将 `fmt_fp` 描述为接收 `char *` 缓冲的渲染器；实际 musl 1.2.5 的 `fmt_fp(FILE *f, long double, int w, int p, int fl, int t)` 通过同文件 `out`/`pad` 与内部 `FILE/__fwritex` 输出。**（此方案在 v4 被弃用——`fmt_fp` 的 long-double/`math.h` 依赖过重，改为自包含 `double`-only 实现，见 §0.3 / §3.4。）**
- **P1（`vformatter` NUL 契约）**：`cap` 定义为**完整目标数组大小（含 NUL）**，核心最多保存 `cap-1` 字符；`cap>0` 时所有字符串 wrapper 都终止。二进制输出（`printf`/`vfprintf`）走独立的非 NUL sink。
- **P1（`sprintf`/`vasprintf` 容量与溢出）**：`sprintf` 沿用历史 4 KiB 截断上限（与 `vsprintf` 一致，非“调用者保证”）；`vasprintf` 补齐错误路径（`total==SIZE_MAX`、`total+1` 溢出、超出 `int` 返回范围 → 返回 `-1` 且 `*strp=NULL`）。
- **P2（`%Lf` 自相矛盾）**：改为 `%lf == %f`；`%Lf/%Le/%Lg` 统一原样输出且不消费实参；§5.3 增加三种 `%L` 测试。

### 0.3 补充修订（v4，本轮）
- **P0（浮点依赖面）**：v3 冻结的 musl `fmt_fp` 移植在实践中会引入 `long double` 运算及 `math.h`/`float.h`（`signbit`/`isfinite`/`frexpl`/`LDBL_*`）依赖，而 OS01 libc 当前两者皆无、也无实现。改为**放弃 `fmt_fp` 移植**，采用**自包含 `double`-only 浮点格式化器**（零 `math.h`/`long double` 依赖）：位级 `signbit`/`isfinite` 宏 + 最小 `float.h`，全部基于 `double` 的 64 位位模式与 `uint64_t` 整数运算，无运行时 helper 符号。
- **P1（printf NUL 写 stdout）**：`printf`/`vfprintf` 改用 `min(total, 4095)` 写出（绝不把 `buf[4095]` 的终止 NUL 发出去）；新增长度 ≥4096 的字节级回归，断言输出无嵌入 NUL。
- **P2（浮点 flags 测试）**：§5.3 增加 `%#.0f`、`%+08.2f`、`%-10.2e`、`%#.5g` 等 flag 用例。

### 0.4 补充修订（v4.x，本轮）
- **P0（跨 TU 调用）**：`floatconv` 不再标 `static`；改为 `libc/stdio/floatconv.c` 提供非 `static` 内部函数 `floatconv_render`，并新增私有头 `libc/stdio/floatconv.h`（不安装到公开 `include/`）由 `vsprintf.c` 包含调用。
- **P0（uint64_t 不足）**：引入**定容 bigint**（80×`uint32_t`≈2560 bit，覆盖任意有限 `double` 十进制展开至 ~770 位），并明确支持上界：所有有限 `double`；`precision` 上限 `FLOATCONV_MAX_PREC`（如 100），超出钳制到上限（文档化、不崩溃）。
- **P1（`DBL_MANT_DIG` 错误）**：修正为 **53**（52 fraction + 1 隐含位）；`float.h` 加编译期 `#error` 断言 binary64（`DBL_MANT_DIG==53 && DBL_MAX_EXP==1024 && sizeof(double)==8`），并配单测。
- **P1（舍入规则）**：明确默认 **round-to-nearest, ties-to-even**（不实现 `fenv`）；验收串固定 `0.5→"0"`、`2.5→"2"`、`-0.5→"-0"`、`3.5→"4"` 等。

### 0.5 补充修订（v5，本轮）
- **P0（`#if sizeof` 非法）**：预处理器无法解析 `sizeof`；拆为：`#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024` → `#error`，`sizeof(double)==8` 改 C 级 `_Static_assert`。并补**位模式测试**（`(uint64_t)1.0 == 0x3FF0000000000000`、`0.5 == 0x3FE0000000000000`）校验真实 binary64 编码，避免“自证宏”假阳性。
- **P1（缺 `DBL_MAX`/`DBL_MIN`）**：`float.h` 补充 `DBL_MAX`（≈1.7976931348623157e308）、`DBL_MIN`（最小正规数，≈2.2250738585072014e-308），使 §5.3 的 `DBL_MAX`/`DBL_MIN` 用例可编译。
- **P1（不可靠的中点用例）**：`%.1f(2.05)→"2.0"` 因 `2.05` 非精确二进制中点而不可靠；改为精确中点 `2.25→"2.2"`、`2.75→"2.8"`。

## 1. 背景与上下文（修正后的根因归类）

| # | 根因 | 核查结论 | 本方案处理 |
|---|------|----------|-----------|
| 1 | `printf` 浮点缺失 | `vsprintf.c` 无浮点分支，`%f` 落入 `default` 原样输出 | ✅ |
| 1b | `printf` 无符号缺失（P0） | `%lu` 用 `va_arg(long)`、`%u` 用 `va_arg(int)` 交给有符号 `number()` → 高位值错误/`cksum` 异常 | ✅ |
| 1c | `printf` 大小保护缺失（P1） | `snprintf`/`sprintf` 经 `vsprintf` 无界写 `buf+4096` | ✅ |
| 4 | `getopt` 逻辑错误 | `optpos` 粘连/带参处理错误（`-d:`/`-f2`） | ✅ |
| 4b | `getopt.h` 多重定义 | 全局量定义为实体非 `extern` | ✅ |
| 2 | `strtod` | 代码对 `"0.1"` 正确，非代码确证缺陷 | ❌ 转调查 |
| 3 | `getcwd`/`pwd` | libc 侧正常 → 疑内核 `SYS_getcwd` | ❌ 转内核任务 |
| 5 | stdio 行读取/seek | `fgets/fseek/getline` 存在性待确认 | ❌ 转调查 |
| 6 | user-fault 崩溃（固定 RIP） | 需 `addr2line` 定位 | ❌ 转调查 |

## 2. 入口点与改动面（修正）

- **printf 家族汇聚于 `vsprintf.c`，但当前 wrapper 直接调 `vsprintf(buf,…)`：**
  - `printf.c` → `vsprintf`
  - `vfprintf`/`fprintf`（`stdio_file.c`）→ `vsprintf`
  - `sprintf.c` → `sprintf`/`snprintf` 均调 `vsprintf(buf, …)`
  - `vsnprintf.c` → `vsprintf(b, …)`（仅事后截断）
  - `vasprintf.c` → `vsnprintf(NULL,0)` → `vsprintf(dummy)`
  - **后果**：`vsprintf` 内部 `end = buf + 4096`，对调用者真实缓冲区大小一无所知；当调用者 `size < 4096`（典型 `snprintf` 小缓冲）即越界写。浮点使单行输出更长，越界风险放大。
  - **设计**：抽取 `vformatter(dst, cap, fmt, ap)`（`cap` = 完整目标数组大小含 NUL，受 `cap` 约束），所有 wrapper 以正确 `cap` 接入（见 §3.2）。修复后“改核心器即覆盖 printf 家族”的断言才成立。
- **`getopt`**：仅 `libc/unistd/getopt.c` + `libc/include/getopt.h`；全局量定义在 `.c`，头文件需改 `extern`。

## 3. 设计 1：printf 核心器重构 + 无符号路径 + 浮点

### 3.1 现有缺陷（细化）
- 无符号（真正根因是 `va_arg` 类型不匹配）：`%lu` → `va_arg(args, long)` 把 8 字节按**有符号 long** 解读，`number(long …)` 收到高位为 1 的负数；`do_div` 本身用 `divq` 对位模式做无符号除法，但传入的 `num` 是**有符号 long**，且 `%u` 走 `va_arg(args, int)` 会把 `> INT_MAX` 的值符号扩展成 64 位负数，二者都导致无符号值被错误解释。`%x/%o` 已用 `unsigned int`/`unsigned long`，但 `%lu/%u` 未走无符号路径。修正方向：改用类型正确的 `unsigned long` / `unsigned int` `va_arg` + 无符号 `number` 路径。
- `ll` 修饰符缺失：`%llu/%lld/%llx/%llo` 中的连续 `ll` 未被识别（解析器只认单个 qualifier），会被当作 `l` + 未知 `l` 原样输出或错位取参，破坏 BusyBox `du`(`%llu`)/`sum`(`%u %llu`) 等验收。
- 大小保护：见 §2。

### 3.2 步骤 A：抽取核心格式化器 `vformatter`（解决 P1，v3 接口）
- 接口契约：**`cap` = 完整目标数组大小（含 NUL 终止符）**。
  `static size_t vformatter(char *dst, size_t cap, const char *fmt, va_list ap)`
  - 内部维护 `total`（本应生成的字符总数）与 `pos`（已写入 `dst` 的可见字符数）。
  - 仅当 `pos < cap-1` 时才真正写 `dst[pos]`；每次成功自增 `pos` 与 `total`。
  - 当 `cap > 0` 时，无论是否被截断，**最后都写 `dst[min(total, cap-1)] = '\0'`**（核心负责终止，调用方无需再补）。
  - 当 `cap == 0`（含 `dst==NULL`）时：只累计 `total`，不写、不终止。
  - 返回 `total`（C 标准 `snprintf` 语义：本应写入的总长度，不含 NUL）。
- wrapper 接入：
  - `vsnprintf`/`snprintf`：`vformatter(buf, size, …)`；`size>0` 返回 `total`（截断亦同 `size==0` 走 `vformatter(NULL,0,…)` 仅计长度）。`cap>0` 已由核心保证 NUL。
  - `vsprintf`：**保留历史 4 KiB 上限**，`vformatter(buf, 4096, …)`——最多保存 4095 个可见字符 + NUL，与现状语义一致。
  - `sprintf`：与 `vsprintf` 同为 **4 KiB 固定截断上限**，`vformatter(buf, 4096, …)`；**不要求调用者保证缓冲大小**，超出部分按 4 KiB 截断（与 `vsprintf` 对称，标准契约明确）。
  - `vasprintf`：**双 pass**——`va_copy(ap2, ap)` 调 `vformatter(NULL, 0, …, ap2)` 得 `total`；检查错误（`total == SIZE_MAX`、或 `total + 1` 溢出 `SIZE_MAX`、或 `total > INT_MAX`）→ 返回 `-1` 且 `*strp = NULL`；否则分配 `total+1`，用**原始 `ap`** 调 `vformatter(buf, total+1, …, ap)` 填充（`cap>0` 保证 NUL），`va_end(ap2)`。绝不用同一 `va_list` 格式化两次。
- **二进制输出 sink（printf/vfprintf）**：`printf.c` / `vfprintf`（`stdio_file.c`）不依赖 NUL 终止。使用固定 **4096 字节**内部缓冲，`vformatter(buf, 4096, …)` 写入（最多 4095 可见字符，NUL 落在 `buf[4095]`）；`write()` 实际写出 **`min(total, 4095)`** 字节——**绝不把 `buf[4095]` 的终止 NUL 发出去**（修正此前 `min(total, 4096)` 会把 NUL 写入标准输出的问题）；超出 4095 字节按 4 KiB 截断语义丢弃。等价于“复用字符串缓冲但只写 `min(total, 4095)`”，杜绝嵌入 NUL。
- 此步为纯重构，行为不变，须通过现有 printf 测试不变。

### 3.3 步骤 B：无符号整数路径 + `ll` 修饰符（P0，必做）
- qualifier 解析支持**连续 `ll`**：逐个识别 `h`/`l`/`L`/`Z` 后，再检查下一个字符是否同为 `l` 以升级为 `ll`（即 `long long`）。
- 新增两个底层渲染器（均使用**无符号**运算，避免符号扩展/负数 division 问题）：
  - `number_longlong(char *dst, size_t cap, long long num, …)`（有符号，用于 `%d/%i` 的 `ll`）
  - `number_unsigned_longlong(char *dst, size_t cap, unsigned long long num, …)`（无符号，用于 `%u/%o/%x/%X` 的 `l`/`ll`）
  - 原有 `number()`（有符号 `long`）保留给 32/64 位有符号短修饰符。
- 分支修正（`va_arg` 类型必须与格式严格匹配）：
  - `%d/%i`：`int` / `long`（`l`）/ `long long`（`ll`）。
  - `%u`：`unsigned int` / `unsigned long`（`l`）/ `unsigned long long`（`ll`）。
  - `%o/%x/%X`：同上无符号族。
  - `%p`：保持 `unsigned long`（已是）。
- **验收边界值**（写入 §5 用例）：
  - 无符号：`%lu`=`0xFFFFFFFFFFFFFFFF`、`0x8000000000000000`；`%lx`、`%lo`；`%u`=`0xFFFFFFFF`、`0x80000000`。
  - **`long long`**：`%lld`=`-9223372036854775808`（INT64_MIN）、`%llu`=`0xFFFFFFFFFFFFFFFF`、`%llx`、`%llo`。
  - **真实回归用例**：`du`（`%llu`，见 `thirdpart/busybox-1.36.1/coreutils/du.c:140`）、`sum`（`%u %llu`，见 `sum.c:85`）的输出格式须与预期一致。

### 3.4 步骤 C：浮点路径（明确契约，解决 P2）
- 支持 `double`：`%f/%F/%e/%E/%g/%G`。**`%lf` 等同 `%f`**（qualifier `l` 对 `double` 无额外语义）。
- **`%Lf/%Le/%Lg`（long double）：本期显式不支持，且统一处理**——**不调用 `va_arg(args, long double)`**（避免类型不匹配 UB），将 `%Lf`/`%Le`/`%Lg` 原样输出字面量（`%Lf`→`%Lf` 等），**不消费该实参**。三条 `%L` 形态行为一致，须分别测试（见 §5.3）。
- **`%a/%A`（十六进制浮点）：本期显式不支持**——原样输出 `%a`/`%A`。
- 算法（呼应 P2，已冻结具体方案，v4 定稿）：**不采用朴素 `frac*=10`**，亦**不移植 musl `fmt_fp`**（其 `long double`/`math.h` 依赖过重）。改为 **自包含 `double`-only 浮点格式化器**，零 `math.h`/`long double` 依赖：
  - **依赖面（满足 P0）**：
    - 新增 `libc/include/float.h`：定义 `FLT_RADIX=2`、`DBL_MANT_DIG=53`（**修正：53 = 52 显式 fraction 位 + 1 隐含整数位**）、`DBL_MAX_EXP=1024`、`DBL_MIN_EXP=-1021`，并补充 `DBL_MAX`（≈1.7976931348623157e308，位模式 `0x7FEFFFFFFFFFFFFF`）、`DBL_MIN`（**最小正规数**，≈2.2250738585072014e-308，位模式 `0x0010000000000000`）——§5.3 的 `DBL_MAX`/`DBL_MIN` 用例依赖这两个宏方可编译。
    - **编译期断言（拆分，满足 P0）**：预处理器层 `#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024` → `#error "need binary64 macros"`；`sizeof(double)` 是 C 表达式，**不能用于 `#if`**，改用 C 级 `_Static_assert(sizeof(double) == 8, "requires IEEE-754 binary64");`（或 typedef 数组断言兼容旧标准）。注意：手工定义宏后再 `#if` 检查仅为自证，故另加**位模式测试**校验真实编码：`union{double;uint64_t}` 断言 `(uint64_t)1.0 == 0x3FF0000000000000`、`(uint64_t)0.5 == 0x3FE0000000000000`，确认实现确为 binary64 而非仅宏正确。
    - 新增 `libc/include/math.h`（最小子集）：`signbit(x)`、`isfinite(x)` 为**位级宏**（`union{double;uint64_t}` 取符号/指数域），无 libm 调用、无 helper 符号。
    - **不引入 `frexpl`/`scalbnl` 或任何 `long double` 运算**；仅用 `double` 的 64 位位模式 + 整数/大整数运算。
  - **大整数支撑（满足 P0：纯 `uint64_t` 不足）**：`%f` 大指数（`DBL_MAX`≈1e308）、`%g`/高精度 `%.100f`、以及 10 的幂缩放中间值都超出 64 位，故**引入定容大整数（bigint）**——定长字数组（如 80×`uint32_t`≈2560 bit，足以容纳任意有限 `double` 的完整十进制展开至 ~770 位），提供 `init/adc/sub/mul_small/divmod_small/mul_pow10`。**明确支持上界**：
    - 支持**所有有限 `double`**；`precision p` 上限 `FLOATCONV_MAX_PREC`（如 100）。`p > FLOATCONV_MAX_PREC` 时**钳制 `p = MAX_PREC`**（文档化：超精度请求只输出到上限，不崩溃、不越界），确保 bigint/栈占用有界。
    - 非有限：`inf`/`nan` 走专用分支（文本 `inf`/`nan` + 可选符号），不经 bigint。
    - 全程栈上定容缓冲（如 768 字节 scratch），无动态分配。
  - **舍入规则（满足 P1）**：默认且仅保证 **round-to-nearest, ties-to-even**（不实现 `fenv`/`FLT_ROUNDS`，文档声明仅默认舍入模式）。验收串明确：
    - `%.0f`：`0.5`→`"0"`、`1.5`→`"2"`、`2.5`→`"2"`、`-0.5`→`"-0"`、`3.5`→`"4"`。
    - 任意精度 `%.1f`：`2.25`→`"2.2"`、`2.75`→`"2.8"`（精确二进制中点，ties-to-even）。
  - **实现位置与链接（满足 P0 跨 TU 调用）**：拆为两个文件：
    - `libc/stdio/floatconv.c`：提供**非 `static`** 的内部函数，如 `size_t floatconv_render(char *scratch, size_t scap, double d, int w, int p, int fl, int conv)`，生成“数字串+小数点+指数+符号”原始序列（不含宽度/填充），返回长度。
    - `libc/stdio/floatconv.h`：**私有内部头**（置于 `libc/stdio/`，**不安装到公开 `include/`**），声明该函数 + `FLOATCONV_MAX_PREC` 等宏；`vsprintf.c` 包含它以调用渲染器。
    - 两者均编译进 `libc.a`（遵循 `stdio/*.c` 自动发现）；`floatconv_render` 在 libc 内部可链接（非 `static`），但不在公开 API 导出。
  - **集成边界（避免两层 flags/width 重复）**：`vformatter` 在浮点转换符处调用 `floatconv_render` 得到核心数字串，再由 `vformatter` 现有 padding 逻辑统一处理前导零/空格、宽度、`#`、左对齐、零填充（浮点数字串复用 `number()` 风格填充），**渲染器内不做宽度/填充**。
  - 仍须满足 §3.4 全部逐例验收（含 §5.3 的 flags / 舍入 / 大值用例：`inf`/`nan`、负零、`%e` 指数归一、`%g` 有效数字与去尾随零、`#`/`+`/宽度/对齐/零填充语义）。
- **验收边界值**：`%.0f` 整数化、`%f/%e/%g` 正/负/零/inf/nan、ties-to-even 舍入（`0.5→0`、`2.5→2`、`-0.5→-0`）、大值（如 `1e20`、`DBL_MAX` 的 `%f` 经 bigint）、`%g` 与 `%f` 切换阈值、超 `FLOATCONV_MAX_PREC` 精度的钳制行为。

### 3.5 方案选择（综合 P2，v4 定稿）
- **推荐（已冻结）**：§3.2 重构 + §3.3 无符号/`ll` 路径（必做，自实现）；浮点采用 **自包含 `double`-only 浮点格式化器**（`libc/stdio/floatconv.c` + 私有 `floatconv.h`，零 `math.h`/`long double` 依赖，定容 bigint 支撑，见 §3.4 依赖面），填充复用 `vformatter` 现有逻辑。
- **备选（已弃用）**：musl `fmt_fp` 因 long-double/`math.h` 依赖过重已排除；D.M. Gay `dtoa` 同样需自行集成 flags/缓冲且可能触碰 long double，亦不采用。
- 内核 `printk` 不受影响（独立文件，且内核 `-mno-sse` 编译，与本文无关）。

## 4. 设计 2：getopt 重写 + 头文件修复

### 4.1 现状缺陷（扩展）
- `optpos` 粘连/带参错误（`-d:`/`-f2`）。
- `getopt.h` 把 `optarg/optind/opterr/optopt` 定义为实体（非 `extern`）→ 多 TU 包含多重定义（clang 默认 `-fno-common`）。

### 4.2 行为契约（覆盖 BusyBox 依赖，P1）
- **`optind == 0` 完整复位**：`optpos=1`、`optarg=NULL`、`optopt=0`（对应 BusyBox `GETOPT_RESET()` 将 `optind=0` 重置状态）。每次进入 `getopt` 若检测到 `optind==0` 即复位。
- **optstring 前缀 `+`**：遇到第一个非选项参数即停止置换、不再解析后续选项（等价 `POSIXLY_CORRECT`）。
- **optstring 前缀 `-`**：非选项参数以返回码 `1`（非 `'?'`）返回；**此时必须 `optarg = argv[optind]`、随后 `optind++` 且 `optpos=1`**（BusyBox `unzip` 在 `case 1` 直接使用 `optarg` 作为文件名）。即返回 `1` 与返回普通选项对称：调用方用 `optarg` 取该操作数。
- **每轮清空 `optarg`**：仅在确有参数时设置，否则置 `NULL`。
- 未知选项/缺参：返回 `'?'`（optstring 首字符为 `:` 时返回 `:`），并置 `optopt`。
- 粘连短选项 `-abc`、粘连带参 `-dVAL`/`-f2`、独立 `-d VAL`、`--` 终止：均须正确。
- 不支持 `getopt_long`（BusyBox 走 `getopt32` 自有逻辑；本文仅覆盖短选项 `getopt`）。

### 4.3 `getopt.h` 修复
- 头文件中 `optarg/optind/opterr/optopt` 改为 `extern` 声明；定义保留在 `getopt.c`（已存在）。避免多 TU 多重定义。

### 4.4 测试覆盖（P1）
- 重置发生在粘连选项中途（`optind=0` 后再解析剩余参数）。
- `+` 前缀：遇非选项即停。
- `-` 前缀：非选项返回 `1`，且 `optarg` 等于该操作数（`case 1` 能用 `optarg`）；**连续多个非选项**逐个正确返回 `1` 并推进 `optind`。
- 等价用例：`cut -d: -f2`、`paste -sd,`、`unzip` 风格的 `-` 前缀非选项取 `optarg`。

## 5. 测试与验收（可复现，P1）

### 5.1 仓库内受版本控制的测试
- 新增固定测试输入 + 期望输出到仓库（如 `test/libc/printf_float.tc`、`test/libc/getopt.tc`，或等价目录），明确预期输出串与失败退出码。
- 若采用 `user/` 小程序方式：该程序须落入 `user/Makefile`（加入 `PROGRAMS` 发现或显式目标）并与 systest/inittab 的实际入口接通（如 `config/inittab.systest` 的 `once:` 调用），而非仅留在 `/tmp`。

### 5.2 主验收 = R1/R2 脚本，但须入仓或提供等价可复现步骤
- 将 `applet_verify.sh` / `applet_verify_r2.sh` 或其等价用例纳入仓库固定文件；fsroot 脚本副本改为仓库内固定文件，写清如何打包进镜像、`/init` 或 systest 如何触发、串口收集方式。
- 目标：重跑后 `seq/du/cksum/sum/printf/nl/cut/paste` 转绿。

### 5.3 逐例边界用例（须全部列出并核对）
- **无符号**：`%lu`=`0xFFFFFFFFFFFFFFFF`、`0x8000000000000000`；`%lx`、`%lo`；`%u`=`0xFFFFFFFF`、`0x80000000`。
- **`long long`（v3 新增）**：`%lld`=`-9223372036854775808`（INT64_MIN）、`%llu`=`0xFFFFFFFFFFFFFFFF`、`%llx`、`%llo`。
- **`vformatter` 接口（v3 新增）**：
  - **NUL 契约**：`vsnprintf`/`vsprintf`/`sprintf`/`vasprintf` 在 `cap>0` 时输出均被 NUL 终止；`vformatter(buf, 4096, …)` 写满 4096 时第 4096 字节为 `'\0'`（最多 4095 可见字符），与历史语义一致；`cap==0`/`dst==NULL` 不写不终止。
  - `sprintf(buf, …)` 复用 **4 KiB 固定截断上限**（非“调用者保证”），超出部分截断；与 `vsprintf` 对称。
  - `snprintf(buf, 0, …)` 仅返回长度、不写；`snprintf(buf, 1, …)` 只写 `'\0'`、返回总长度。
  - 截断后返回值 = 本应生成的总长度（非 `size-1`）。
  - 输出超过 4096 字节时不越界、`total` 正确；`printf`/`vfprintf` 用独立非 NUL sink 输出，不依赖终止符。
  - `vasprintf` 用 `va_copy` 计数 pass + 原始 `va_list` 格式化 pass，验证两次调用结果等价；错误路径：`total==SIZE_MAX`、`total+1` 溢出、或 `total>INT_MAX` → 返回 `-1` 且 `*strp=NULL`，且不泄露部分分配。
- **浮点 `%L` 三形态（v3 修正）**：`%Lf`/`%Le`/`%Lg` 均原样输出字面量（`%Lf`→`%Lf`）、不消费实参，三种形态分别验证。
- **真实回归（BusyBox）**：`du`（`%llu`）与 `sum`（`%u %llu`）的输出格式须与预期一致（见 §3.3）。
- **浮点**：`%.0f` 整数化；`%f/%e/%g` 正/负/零/inf/nan；`0.5`/`2.5` 四舍五入；`1e20` 大值；`%g`↔`%f` 切换。
- **浮点舍入（v4.x 新增，ties-to-even；v5 修正用例）**：`%.0f` 的 `0.5→"0"`、`1.5→"2"`、`2.5→"2"`、`-0.5→"-0"`、`3.5→"4"`（这些是有效的 ties-to-even 覆盖）；`%.1f` 改用**精确二进制中点**（非 `2.05` 这种不可靠的非精确中点）：`2.25→"2.2"`（2.25=9/4 精确，.25 位于偶数侧 2）、`2.75→"2.8"`（2.75=11/4 精确，.75 进位到偶数 8）。断言默认舍入模式为 round-to-nearest-even。
- **binary64 前提（v4.x 新增，v5 修正）**：预处理器 `#if DBL_MANT_DIG!=53 || DBL_MAX_EXP!=1024` → `#error`；C 级 `_Static_assert(sizeof(double)==8, "requires IEEE-754 binary64")`（`sizeof` 不用于 `#if`）；位模式单测校验真实编码：`(uint64_t)1.0==0x3FF0000000000000`、`(uint64_t)0.5==0x3FE0000000000000`。宏定义本身不构成充分证明。

- **浮点边界（v4.x 新增，bigint）**：`DBL_MAX`/`DBL_MIN` 的 `%f`/`%e` 经定容 bigint 正确展开；`%.100f` 等超 `FLOATCONV_MAX_PREC` 请求被钳制到上限且不崩溃、不越界（文档化行为）。
- **浮点 flags（v4 新增，覆盖 §3.4 承诺语义）**：`%#.0f`（强制小数点，如 `1`→`1.`）、`%+08.2f`（`+` 号 + 零填充 + 宽度，如 `3.14`→`+0003.14`）、`%-10.2e`（左对齐 + 宽度，如 `1.5e+00` 后补空格至 10 列）、`%#.5g`（`#` 保留尾随零/小数点后至少一位，如 `1.0`→`1.0000`）。这些用例用于捕获 musl 风格 flag 位映射错误或重复填充。
- **`printf`/`fprintf` 无嵌入 NUL（v4 新增）**：构造长度 ≥4096 的格式化输出（如 `printf("%s", 长串)` 或循环拼出 ≥4096 字节），做**字节级**断言：经 `write` 发出去的字节数 = 可见长度，且输出缓冲中**无任何字节为 `'\0'`**（即 `buf[4095]` 的终止符未被写出）。
- **自包含浮点依赖的编译/链接（v4 新增，回应 P0）**：`libc/include/float.h`、`libc/include/math.h`（仅位级宏）、`libc/stdio/floatconv.c` 必须随 libc 构建进 `libc.a`，且链接 busybox/测试程序时**无 `undefined reference`**（尤其不得引入 `frexpl`/`scalbnl`/任何 `__*_ld` 等 long-double helper 符号）；链接产物用 `nm`/`objdump` 核对无意外外部浮点符号。
- **getopt**：§4.4 全部用例（含 `-` 前缀连续非选项取 `optarg`）。

## 6. 风险
- 浮点改为**自包含 `double`-only 实现**（无外部代码、无许可证审查）；风险转移到自实现的正确性，须靠 §3.4/§5.3 的逐例验收（尤其舍入、长尾数、`%e`/`%g` 边界）。
- 核心格式化器抽取需同步改多个 wrapper（`vsprintf/vsnprintf/snprintf/sprintf/vasprintf`），回归面广——靠 §5 测试覆盖。
- `getopt.h` 改 `extern` 后须确认现有 `.c` 中确有单一定义（当前 `getopt.c` 已定义，满足）。

## 7. 明确不在范围（后续任务）
1. `strtod`：代码对 `"0.1"` 正确，先调查链接/封装层再决定是否改。
2. `stdio` 行读取/seek：先确认 `fgets/fseek/getline` 是否存在/正确，再修 `tail/tac/expand`。
3. `pwd`/`getcwd`：疑为内核 `SYS_getcwd` 缺陷，单独立内核任务。
4. 固定 RIP 的 user-fault 崩溃：需 `addr2line` 定位 `cut/nl/expand/sum` 对应的 libc 函数，单独调查。
