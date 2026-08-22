# libc 兼容性修复设计（printf 浮点 + getopt）

- 日期：2026-08-22
- 验收基线：`docs/applet-verification.md`（BusyBox Applet 验证报告 2026-08-18）
- 范围：仅修复代码中**已确证**的两个 libc 缺陷——`printf` 缺浮点格式、以及 `getopt` 逻辑错误。
- 方针：最小范围、最低风险；不引入外部依赖（除非明确选择 fallback）。

## 1. 背景与上下文

`docs/applet-verification.md` 报告约 15 个 busybox applet 因 OS01 libc 兼容性缺口而功能异常。报告列出的 6 类根因中，经代码核查：

| # | 根因 | 核查结论 | 本方案处理 |
|---|------|----------|-----------|
| 1 | `printf` 缺 `%f/%ld/%lu/%x` | `libc/stdio/vsprintf.c` 确无浮点路径；`%f` 落入 `default` 分支原样输出。整数 `%ld/%lu` 实际已被 qualifier 处理（疑为浮点问题级联/误报），但浮点是**确证缺失** | ✅ 本方案 |
| 4 | `getopt` 短选项解析 | `libc/unistd/getopt.c` `optpos` 处理对粘连参数/带参选项有误（`-d:`/`-f2`） | ✅ 本方案 |
| 2 | `strtod` | `libc/stdlib/strtod.c` 对 `"0.1"` 逻辑正确，**非代码确证缺陷** | ❌ 转调查 |
| 3 | `getcwd`/`pwd` | `libc/unistd/getcwd.c` libc 侧正常 → 疑为内核 `SYS_getcwd` 缺陷 | ❌ 转内核任务 |
| 5 | stdio 行读取/seek | `fgets/fseek/getline` 存在性待确认，先调查 | ❌ 转调查 |
| 6 | user-fault 崩溃（固定 RIP） | 需 `addr2line` 定位具体 libc 函数 | ❌ 转调查 |

**本方案只承诺 #1（浮点）与 #4（getopt）。** 其余 4 项列为后续调查/单独任务，不纳入本实现。

## 2. 入口点分析（改动面很小）

- `printf` 浮点：所有格式化函数都汇聚到 `libc/stdio/vsprintf.c`：
  - `printf.c` → `vsprintf`
  - `vfprintf`/`fprintf`（stdio_file.c）→ `vsprintf`
  - `sprintf.c` → `vsprintf`
  - `vsnprintf.c` → `vsprintf`；`vasprintf.c` → `vsnprintf` → `vsprintf`
  - 因此**只改 `vsprintf.c` 即修复全部 printf 家族**，且**不影响内核 `printk`**（内核 printk 是独立文件，且内核用 `-mno-sse` 编译，与本文无关）。
- `getopt`：仅 `libc/unistd/getopt.c`；全局变量 `optarg/optind/opterr/optopt` 同文件。

## 3. 设计 1：`printf` 浮点格式

### 3.1 现状
`vsprintf.c` 的 `vsprintf()` 主循环已正确解析 flags（`-+ #0`）、field width、`*`、`precision`、qualifier（`h/l/L/Z`），并对 `%c/%s/%o/%p/%x/%X/%d/%i/%u/%n/%%` 分支处理。浮点分支完全缺失。

### 3.2 方案 A（推荐）：就地扩展 `vsprintf.c`
在 `vsprintf()` 的 `switch(*fmt)` 中新增 `%f/%F/%e/%E/%g/%G` 分支（及可选 `%a/%A`）。实现要点：

1. 新增辅助函数（如 `static char *float_to_str(...)`）将 `double` 转为十进制字符串：
   - 取符号、处理 `inf`/`nan`、负零。
   - 整数部分：复用现有 `number()` 生成（或其内部逻辑）。
   - 小数部分：以 `.precision`（默认 6）位、通过反复 `frac *= 10` 取整数位生成；注意舍入（最后一位四舍五入）。
   - `%e/%E`：归一化指数形式（`[-]d.ddd e±dd`）；`%g/%G`：按值大小在 `%f`/`%e` 间选择、去尾随零。
2. 生成的数字串交给现有 `number()` 风格的填充/对齐/符号/ZEROPAD 逻辑输出，以复用 flags/width/precision 语义。
3. qualifier：`%Lf` 按 `long double` 处理（本实现可先按 `double` 近似或显式不支持）；`%f` 默认 `double`。
4. `%a/%A`（十六进制浮点）作为 stretch：实现或显式“不支持→原样输出 `%a`”以避免静默错误结果（二选一，实现时定）。

**正确性与范围权衡**：busybox 的实际需求很窄——`seq` 用 `%.0f`，`du`/`cksum`/`sum` 用 `%lu`/`%.0f`，`printf` 测试用 `%f`。因此聚焦“整数/固定小数/科学计数正确 + 默认 6 位精度 + 符号/宽度/精度 flags”即可，不强求 IEEE-754 最短舍入。

### 3.3 方案 B（fallback，更低风险）：引入成熟 `dtoa`
采用 musl 的浮点 printf 路径或 D.M. Gay 的 `dtoa.c`。舍入更正确，但引入外部代码 + 许可证审查，与仓库“自包含”风格相悖。**仅在评审要求零风险时采用。**

**推荐 A**：改动面小、可针对 busybox 用例验证，且不引入外部依赖。

## 4. 设计 2：`getopt` 重写

### 4.1 现状缺陷
`libc/unistd/getopt.c` 在 `arg[optpos+1] != '\0'` 时仅 `optpos++`，未把剩余字符作为 `optarg`；且 `optpos` 复位与“粘连短选项 + 带参”组合（如 `cut -d: -f2`、`paste -sd,`）处理错误，导致 busybox 解析异常。

### 4.2 方案 A（推荐）：就地重写 `getopt`
实现符合 POSIX 的短选项语义：
- 粘连短选项：`-abc` 逐个返回 a、b、c（`optpos` 推进）。
- 带参选项的粘连值：`-dVAL`（optstring `d:`）→ `optarg="VAL"`；`-f2`（f 带参）→ `optarg="2"`。
- 独立参数：`-d VAL` → 取下一 argv 为 `optarg`，`optind++`。
- `--` 终止符：遇到 `--` 后整体返回 -1，不再解析。
- 错误语义：optstring 首字符 `:` 时缺参返回 `:` 而非 `?`；未知选项返回 `?` 并置 `optopt`。
- 全局状态 `optarg/optind/opterr/optopt`、`static optpos` 复位逻辑正确。
- 不实现 `getopt_long`（busybox 这些 applet 仅用短选项；如后续需要再补）。

### 4.3 方案 B（fallback）：采用 musl `getopt`
约 100 行、久经考验、MIT。风险最低，但外部代码。

**推荐 A**：树内自包含、易针对 `cut`/`paste` 失败用例回归验证。

## 5. 测试与验收

- **增量单测（新增）**：在 `user/` 下新增一个小测试程序（或内核 selftest），覆盖：
  - `printf`：`%f`/`%e`/`%g` 带符号/精度/宽度；`%.0f` 整数；`%a` 行为（无论实现或显式原样）。
  - `getopt`：`-abc`、`-d: -f2`、`-d VAL`、`--` 终止、未知选项/缺参的错误码。
- **主验收 = 文档 R1/R2 脚本**：重新构建 busybox + 重跑 `applet_verify.sh` / `applet_verify_r2.sh`，目标 `seq/du/cksum/sum/printf/nl/cut/paste` 转绿。
- **不改动** busybox config（`sort`/`[`/`touch` 仍不在范围）。

## 6. 风险

- **浮点 ABI**：libc 以默认 flags 编译（无 `-mno-sse`），`double` 通过 XMM 传递，与 busybox（同样无 `-mno-sse`）一致，ABI 兼容。`va_arg(ap, double)` 可用。
- **舍入精度**：方案 A 不追求最短舍入，仅保证 busybox 用例正确；极端值可能多/少一位，不影响验收。

## 7. 明确不在范围（后续任务）

1. `strtod`：代码对 `"0.1"` 正确，先调查链接/封装层再决定是否改。
2. `stdio` 行读取/seek：先确认 `fgets/fseek/getline` 是否存在/正确，再修 `tail/tac/expand`。
3. `pwd`/`getcwd`：疑为内核 `SYS_getcwd` 缺陷，单独立内核任务。
4. 固定 RIP 的 user-fault 崩溃：需 `addr2line` 定位 `cut/nl/expand/sum` 对应的 libc 函数，单独调查。
