# libc 兼容性修复设计（printf 浮点 + getopt）— 修订版 v2

- 日期：2026-08-22（v2 修订）
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
  - **设计**：抽取 `vformatter(buf, end, fmt, ap)`（受 `end` 约束），所有 wrapper 以正确 `end` 接入（见 §3.2）。修复后“改核心器即覆盖 printf 家族”的断言才成立。
- **`getopt`**：仅 `libc/unistd/getopt.c` + `libc/include/getopt.h`；全局量定义在 `.c`，头文件需改 `extern`。

## 3. 设计 1：printf 核心器重构 + 无符号路径 + 浮点

### 3.1 现有缺陷（细化）
- 无符号（真正根因是 `va_arg` 类型不匹配）：`%lu` → `va_arg(args, long)` 把 8 字节按**有符号 long** 解读，`number(long …)` 收到高位为 1 的负数；`do_div` 本身用 `divq` 对位模式做无符号除法，但传入的 `num` 是**有符号 long**，且 `%u` 走 `va_arg(args, int)` 会把 `> INT_MAX` 的值符号扩展成 64 位负数，二者都导致无符号值被错误解释。`%x/%o` 已用 `unsigned int`/`unsigned long`，但 `%lu/%u` 未走无符号路径。修正方向：改用类型正确的 `unsigned long` / `unsigned int` `va_arg` + 无符号 `number` 路径。
- `ll` 修饰符缺失：`%llu/%lld/%llx/%llo` 中的连续 `ll` 未被识别（解析器只认单个 qualifier），会被当作 `l` + 未知 `l` 原样输出或错位取参，破坏 BusyBox `du`(`%llu`)/`sum`(`%u %llu`) 等验收。
- 大小保护：见 §2。

### 3.2 步骤 A：抽取核心格式化器 `vformatter`（解决 P1，v3 接口）
- 接口固定为**“输出槽容量 + 独立总字符计数”**，避免 NULL 推进指针、`end=buf+size`、`str-buf` 算长度等 UB：
  `static size_t vformatter(char *dst, size_t cap, const char *fmt, va_list ap)`
  - 内部维护 `total`（本应生成的字符总数）与 `pos`（已写入 `dst` 的字节数）。
  - `dst==NULL && cap==0`：只累计 `total`，不写。
  - 写入时仅当 `pos < cap` 才真正写 `dst[pos]`；每次成功自增 `pos` 与 `total`。
  - 返回 `total`（C 标准 `snprintf` 语义）。
- wrapper 接入：
  - `vsnprintf`/`snprintf`：`vformatter(buf, size, …)`；`size>0` 时保证 `buf[min(total,size-1)]` 处写 `'\0'`，返回 `total`（即便被截断）。`size==0` 时 `vformatter(NULL,0,…)` 仅计长度。
  - `sprintf`：`vformatter(buf, SIZE_MAX_or_4K, …)`，调用者保证缓冲足够（文档注明 ≥4K 或自担风险）。
  - `vsprintf`：保留 4 KiB 上限语义，等价于 `vformatter(buf, 4096, …)`。
  - `vasprintf`：**双 pass**——先 `va_copy(ap2, ap)` 调 `vformatter(NULL, 0, …, ap2)` 得 `total`；分配 `total+1`；再用**原始 `ap`** 调 `vformatter(buf, total+1, …, ap)` 填充。`va_end(ap2)`。绝不用同一 `va_list` 格式化两次。
- `printf.c` / `vfprintf`（`stdio_file.c`）：不再按返回长度从固定 4 KiB 缓冲直接 `write`。改为：用 `vformatter` 写入固定缓冲（容量 = 缓冲大小），`write` 的字节数取 `min(total, 容量)`；或实现分段输出（超出缓冲时分多次 `write`）。明确实现为“只写实际保存长度”，杜绝越界读。
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
- 支持 `double`：`%f/%F/%e/%E/%g/%G`；qualifier `l`/`L` 对 `double` 无额外语义（`%lf == %f`）。
- **`%Lf`（long double）：本期显式不支持**——**不调用 `va_arg(args, long double)`**（避免类型不匹配 UB），原样输出 `%Lf`。
- **`%a/%A`（十六进制浮点）：本期显式不支持**——原样输出 `%a`/`%A`。
- 算法（呼应 P2，已冻结具体上游）：**不采用朴素 `frac*=10`**。冻结采用 **musl libc 的 `fmt_fp` 浮点渲染器**（来自 musl `src/stdio/vfprintf.c` 内的 `fmt_fp`，配合其内部辅助 `shuint`/`__uintarith` 等）：
  - **版本/许可证**：pin musl 1.2.x，许可证 **MIT**（与仓库自包含风格兼容，需在实施前将确切文件与 commit 写入本 spec 的“依赖清单”小节）。
  - **职责边界**：`fmt_fp` 负责把 `double` 按 `%f/%e/%g` 的 flags/宽度/精度/`#` 渲染到缓冲；我们保留自己的 `vformatter` 作 flags/width/precision 解析与前导填充，仅在遇到浮点转换符时把（已解析的 flags、width、precision、`#`、符号、`double` 值、目标缓冲与剩余容量）交给 `fmt_fp`，由其完成数字串生成并回写。即“解析层自研 + 渲染层用 musl”，而非整体替换 `vformatter`。
  - **为何非 D.M. Gay `dtoa`**：`dtoa` 只做浮点→十进制字符串转换，仍需我们自己实现 `%f/%e/%g` 的 flags/宽度/精度/`#` 与缓冲输出集成；`fmt_fp` 已包含这部分，集成面更小、风险更低。
  - 仍须满足 §3.4 全部逐例验收（舍入进位、正负/零/负零、`inf`/`nan`、`%e` 指数归一化、`%g` 有效数字与去尾随零、`#` 语义）。
- **验收边界值**：`%.0f` 整数化、`%f/%e/%g` 正/负/零/inf/nan、`0.5`/`2.5` 四舍五入、大值（如 `1e20`）、`%g` 与 `%f` 切换阈值。

### 3.5 方案选择（综合 P2）
- **推荐（已冻结）**：§3.2 重构 + §3.3 无符号/`ll` 路径（必做，自实现）；浮点采用 **musl `fmt_fp`** 作为渲染层（§3.4 已定义集成边界与许可证/版本要求），解析层仍自研。
- **备选**：若 `fmt_fp` 适配成本超预估，回退到 D.M. Gay `dtoa` + 自研 `%f/%e/%g` flags/缓冲集成，但须先把 `dtoa` 的依赖清单与适配边界写入 spec。
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
  - `snprintf(buf, 0, …)` 仅返回长度、不写；`snprintf(buf, 1, …)` 只写 `'\0'`、返回总长度。
  - 截断后返回值 = 本应生成的总长度（非 `size-1`）。
  - 输出超过 4096 字节时不越界、`total` 正确、`vasprintf` 双 pass 结果一致。
  - `vasprintf` 用 `va_copy` 计数 pass + 原始 `va_list` 格式化 pass，验证两次调用结果等价。
- **真实回归（BusyBox）**：`du`（`%llu`）与 `sum`（`%u %llu`）的输出格式须与预期一致（见 §3.3）。
- **浮点**：`%.0f` 整数化；`%f/%e/%g` 正/负/零/inf/nan；`0.5`/`2.5` 四舍五入；`1e20` 大值；`%g`↔`%f` 切换。
- **getopt**：§4.4 全部用例（含 `-` 前缀连续非选项取 `optarg`）。

## 6. 风险
- 浮点采用成熟 dtoa 会引入外部代码 + 许可证审查（若选 B 路线）；若自实现须逐例验收（§3.4/§5.3）。
- 核心格式化器抽取需同步改多个 wrapper（`vsprintf/vsnprintf/snprintf/sprintf/vasprintf`），回归面广——靠 §5 测试覆盖。
- `getopt.h` 改 `extern` 后须确认现有 `.c` 中确有单一定义（当前 `getopt.c` 已定义，满足）。

## 7. 明确不在范围（后续任务）
1. `strtod`：代码对 `"0.1"` 正确，先调查链接/封装层再决定是否改。
2. `stdio` 行读取/seek：先确认 `fgets/fseek/getline` 是否存在/正确，再修 `tail/tac/expand`。
3. `pwd`/`getcwd`：疑为内核 `SYS_getcwd` 缺陷，单独立内核任务。
4. 固定 RIP 的 user-fault 崩溃：需 `addr2line` 定位 `cut/nl/expand/sum` 对应的 libc 函数，单独调查。
