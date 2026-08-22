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
- 无符号：`%lu` → `va_arg(args, long)` 再传给 `number(long …)`，高位 1 的 `unsigned long` 被当负数，`do_div` 在负数上行为未定义；`%u` → `va_arg(args, int)`，值 `> INT_MAX` 被符号扩展为 64 位负数。`%x/%o` 已用 `unsigned int`/`unsigned long`，但 `%lu/%u` 未走无符号路径。
- 大小保护：见 §2。

### 3.2 步骤 A：抽取核心格式化器 `vformatter`（解决 P1）
- 从 `vsprintf()` 主循环抽出 `static char *vformatter(char *buf, char *end, const char *fmt, va_list ap)`，所有写操作受 `str < end` 约束（沿用现有检查）。
- wrapper 接入：
  - `vsprintf`：`end = buf + 4096`（保留 4K 上限语义）。
  - `vsnprintf`/`snprintf`：`end = buf + size`；`size==0` 时进入“仅算长度”模式（不写，返回需长度）。
  - `sprintf`：`end = buf + 4096`（调用者保证缓冲 ≥4K；文档注明）。
  - `vasprintf`：先 `vformatter` 算长度（核心器需支持 `buf==NULL` 的长度模式）→ 分配 → 再格式化。
- 此步为纯重构，行为不变，须通过现有 printf 测试不变。

### 3.3 步骤 B：无符号整数路径（P0，必做）
- 新增 `static char *number_unsigned(char *str, char *end, unsigned long num, int base, int size, int precision, int type)`，使用**无符号**运算（`do_div` 改无符号版）。
- 分支修正：
  - `%u`：`va_arg(args, unsigned int)` → `number_unsigned(…, (unsigned long)v, 10, …)`。
  - `%lu/%lo/%lX`：`va_arg(args, unsigned long)` → `number_unsigned`。
  - `%o/%x/%X`（无 `l`）：`va_arg(args, unsigned int)` → `number_unsigned`。
  - `%p`：保持 `unsigned long`（已是）。
  - `%d/%i`：保持有符号 `va_arg(args, int/long)` + `number(signed)`。
- **验收边界值**（写入 §5 用例）：`%lu` = `0xFFFFFFFFFFFFFFFF`、`0x8000000000000000`、`%lx`、`%lo`、`%u` = `0xFFFFFFFF`、`0x80000000`。目标：在 32/64 位无符号范围内正确无符号打印。

### 3.4 步骤 C：浮点路径（明确契约，解决 P2）
- 支持 `double`：`%f/%F/%e/%E/%g/%G`；qualifier `l`/`L` 对 `double` 无额外语义（`%lf == %f`）。
- **`%Lf`（long double）：本期显式不支持**——**不调用 `va_arg(args, long double)`**（避免类型不匹配 UB），原样输出 `%Lf`。
- **`%a/%A`（十六进制浮点）：本期显式不支持**——原样输出 `%a`/`%A`。
- 算法（呼应 P2）：**不采用朴素 `frac*=10`**。采用成熟 `double→decimal`（Musl 浮点 printf 路径或 D.M. Gay `dtoa`），保证：舍入（含小数向整数进位，如 `0.5→1`、`.999→1`）、正负/零/负零、`inf`/`nan`、`%e` 归一化指数、`%g` 有效数字与去尾随零、`#` 备用格式。
- **验收边界值**：`%.0f` 整数化、`%f/%e/%g` 正/负/零/inf/nan、`0.5`/`2.5` 四舍五入、大值（如 `1e20`）、`%g` 与 `%f` 切换阈值。

### 3.5 方案选择（综合 P2）
- **推荐**：§3.2 重构 + §3.3 无符号路径（必做，自实现）；浮点（§3.4）采用**成熟 dtoa（方案 B 的 dtoa 部分）**以保证正确性，而非朴素自实现。即“整体结构用方案 A + 浮点用成熟 dtoa”。
- **备选**：完全自实现浮点，但须满足 §3.4 全部逐例验收，否则回退到成熟 dtoa。
- 内核 `printk` 不受影响（独立文件，且内核 `-mno-sse` 编译，与本文无关）。

## 4. 设计 2：getopt 重写 + 头文件修复

### 4.1 现状缺陷（扩展）
- `optpos` 粘连/带参错误（`-d:`/`-f2`）。
- `getopt.h` 把 `optarg/optind/opterr/optopt` 定义为实体（非 `extern`）→ 多 TU 包含多重定义（clang 默认 `-fno-common`）。

### 4.2 行为契约（覆盖 BusyBox 依赖，P1）
- **`optind == 0` 完整复位**：`optpos=1`、`optarg=NULL`、`optopt=0`（对应 BusyBox `GETOPT_RESET()` 将 `optind=0` 重置状态）。每次进入 `getopt` 若检测到 `optind==0` 即复位。
- **optstring 前缀 `+`**：遇到第一个非选项参数即停止置换、不再解析后续选项（等价 `POSIXLY_CORRECT`）。
- **optstring 前缀 `-`**：非选项参数以返回码 `1`（非 `'?'`）返回，调用方将其视为操作数。
- **每轮清空 `optarg`**：仅在确有参数时设置，否则置 `NULL`。
- 未知选项/缺参：返回 `'?'`（optstring 首字符为 `:` 时返回 `:`），并置 `optopt`。
- 粘连短选项 `-abc`、粘连带参 `-dVAL`/`-f2`、独立 `-d VAL`、`--` 终止：均须正确。
- 不支持 `getopt_long`（BusyBox 走 `getopt32` 自有逻辑；本文仅覆盖短选项 `getopt`）。

### 4.3 `getopt.h` 修复
- 头文件中 `optarg/optind/opterr/optopt` 改为 `extern` 声明；定义保留在 `getopt.c`（已存在）。避免多 TU 多重定义。

### 4.4 测试覆盖（P1）
- 重置发生在粘连选项中途（`optind=0` 后再解析剩余参数）。
- `+` 前缀：遇非选项即停。
- `-` 前缀：非选项返回 `1`。
- 等价用例：`cut -d: -f2`、`paste -sd,`。

## 5. 测试与验收（可复现，P1）

### 5.1 仓库内受版本控制的测试
- 新增固定测试输入 + 期望输出到仓库（如 `test/libc/printf_float.tc`、`test/libc/getopt.tc`，或等价目录），明确预期输出串与失败退出码。
- 若采用 `user/` 小程序方式：该程序须落入 `user/Makefile`（加入 `PROGRAMS` 发现或显式目标）并与 systest/inittab 的实际入口接通（如 `config/inittab.systest` 的 `once:` 调用），而非仅留在 `/tmp`。

### 5.2 主验收 = R1/R2 脚本，但须入仓或提供等价可复现步骤
- 将 `applet_verify.sh` / `applet_verify_r2.sh` 或其等价用例纳入仓库固定文件；fsroot 脚本副本改为仓库内固定文件，写清如何打包进镜像、`/init` 或 systest 如何触发、串口收集方式。
- 目标：重跑后 `seq/du/cksum/sum/printf/nl/cut/paste` 转绿。

### 5.3 逐例边界用例（须全部列出并核对）
- 无符号：`%lu`=`0xFFFFFFFFFFFFFFFF`、`0x8000000000000000`；`%lx`、`%lo`；`%u`=`0xFFFFFFFF`、`0x80000000`。
- 浮点：`%.0f` 整数化；`%f/%e/%g` 正/负/零/inf/nan；`0.5`/`2.5` 四舍五入；`1e20` 大值；`%g`↔`%f` 切换。
- getopt：§4.4 全部用例。

## 6. 风险
- 浮点采用成熟 dtoa 会引入外部代码 + 许可证审查（若选 B 路线）；若自实现须逐例验收（§3.4/§5.3）。
- 核心格式化器抽取需同步改多个 wrapper（`vsprintf/vsnprintf/snprintf/sprintf/vasprintf`），回归面广——靠 §5 测试覆盖。
- `getopt.h` 改 `extern` 后须确认现有 `.c` 中确有单一定义（当前 `getopt.c` 已定义，满足）。

## 7. 明确不在范围（后续任务）
1. `strtod`：代码对 `"0.1"` 正确，先调查链接/封装层再决定是否改。
2. `stdio` 行读取/seek：先确认 `fgets/fseek/getline` 是否存在/正确，再修 `tail/tac/expand`。
3. `pwd`/`getcwd`：疑为内核 `SYS_getcwd` 缺陷，单独立内核任务。
4. 固定 RIP 的 user-fault 崩溃：需 `addr2line` 定位 `cut/nl/expand/sum` 对应的 libc 函数，单独调查。
