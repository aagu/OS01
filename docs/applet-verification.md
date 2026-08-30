# BusyBox Applet 验证报告（2026-08-18）

> 目的：逐个验证 `config/busybox.config` 启用的 applet 在 OS01 上能否运行。
> 方法：自定义 inittab `once:` 执行验证脚本 → QEMU（-smp 1, -serial file）→ 串口镜像收集。
> 环境：homeserver QEMU TCG，busybox v1.36.1 静态链接 OS01 libc，root 为 ext2 R/W。
> 更新（2026-08-22）：`CONFIG_FLOAT_DURATION=y` 已默认开启，busybox 用 `strtod` 的浮点时长路径已验证。
> 更新（2026-08-24，syscall 边界审计 Task 10）：见 §💥，**applet user-fault 属用户态 libc 缺陷，不在 syscall 边界审计作用域**（审计钩子只覆盖内核解引用用户指针，不覆盖用户态非法 RIP）。
> 更新（2026-08-30）：**B 类 9 个 applet 全部修复并实测通过**。根因：`realloc` 读错头部偏移致内容全丢（8/9）、
> `libgen` dirname/basename 桩、`strsep` 耗尽不置 NULL、`fclose` 哨兵崩溃、`fd_read` EOF 返回 -1。
> 见下方「§ 2026-08-30 修复记录」。systest 回归 **228/228**。

## 结论（TL;DR）

- ✅ **52/52 配置的 applet 全部编译进 busybox**，ELF 加载与执行无缺失 syscall 导致的失败
- ✅ **libc printf 格式族已修复**：`%f/%F/%e/%E/%g/%G` 浮点、`%ld/%lu` 及 `%x/%o` 整数全部正常 →
  `printf` / `seq` / `du` / `cksum` / `sum` 已恢复
- ✅ **libc `strtod` 已修复**：`sleep 0.1` 等浮点时长通过（`CONFIG_FLOAT_DURATION=y` 已默认开启）
- ✅ **2026-08-30：B 类 9 个 applet 全部修复**（原 ⚠️ ~9 个 libc 缺口）：`realloc` 丢内容、`libgen` 桩、
  `strsep` 耗尽不置 NULL、`fclose` 哨兵崩溃、`fd_read` EOF 返回 -1 → pwd / dirname / tail -n / tac /
  cut / paste / expand / unexpand / nl 全部 QEMU 真机实测通过，systest 回归 **228/228**。tr 经复查本就正常（旧报告误报）。
- ❌ 未编译 applet：`touch`（需文件时间戳）、`grep`（需 POSIX regex）——见 C 级 / Tier 2；`sort` 与 `[` 已于 2026-08-30 启用（见 C 级）。
- ✅ **user-fault 崩溃已修复**：`nl` 崩溃根因是 `fclose(stdin)` 解引用哨兵 `(FILE*)1`，见 §2026-08-30 修复记录。

## 验证结果分级

### ✅ A 级 — 完整工作（28 个）

| applet | 验证 | applet | 验证 |
|--------|------|--------|------|
| echo | `echo hello` → hello | kill | `kill -l` → 信号表 |
| printf | `%s` 正常 | cal | 输出 2026 日历 |
| basename | `/a/b/c` → c | mkdir/rmdir | 建/删目录 OK |
| ls / ls -l | 根目录/权限列 OK | cat | 文件读取 OK |
| uname | OS01 ... x86_64 | wc | `-c/-l/-w` 均 OK |
| tty | "not a tty"（合理） | cp/mv/rm | 文件操作 OK |
| which | /bin/busybox | crc32 | 输出校验和 OK |
| true/false | 退出码 OK | cmp/comm | 文件比较 OK |
| test | `-d/-f` OK | truncate | `-s 100` → 100 字节 |
| sync | OK | clear | OK |
| nohup | 后台执行 OK | yes | `yes x \| head -1` → x |
| head | `-2`/`-c` OK | tail | `-c` OK |

### ✅ B 级 — 功能异常（libc 兼容性，2026-08-30 已全部修复）

> 2026-08-22 重测（真实 QEMU 抓串口）：printf 格式族与 strtod 已恢复，下列表格区分「已恢复」与「仍异常」。
> 2026-08-30：仍异常的 9 个已全部修复，见下表。

**✅ 已恢复（6 个）— libc printf 格式族 / strtod 修复生效：**

| applet | 实测（QEMU 真实输出） | 结论 |
|--------|----------------------|------|
| printf | `3.1 2147483647 4294967295 ff`（`%.1f %ld %lu %x`）| 已恢复 |
| seq | `1 2 3`（`seq 1 3`）| 已恢复（原 `%.0f` 浮点路径）|
| du | `183 /bin/busybox`（`du -k`，无 `%lu` 字面）| 已恢复 |
| cksum | `3287646509 5`（`cksum`，校验正确）| 已恢复 |
| sum | `08403 1`（`sum`，正确）| 已恢复 |
| sleep | `sleep 0.1` → 成功（`CONFIG_FLOAT_DURATION=y` 走 `strtod`）| 已恢复 |

**✅ 2026-08-30 已全部修复（9 个）— QEMU 真机复测通过（`-smp 1` + `-display gtk` + `-serial file`）：**

| applet | 修复前现象 | 根因（定位） | 修复 |
|--------|-----------|-------------|------|
| pwd | 输出空 | `realloc` 读错头部偏移（`ptr[-1]` 读到 is_free=0）→ copy=0 → `xrealloc_getcwd_or_warn` 收尾 shrink 丢内容 | `realloc` |
| dirname | `/a/b/c` → `/` | `libc/libgen/libgen.c` 的 `dirname()`/`basename()` 是桩 | 实现 POSIX 语义 |
| tail -n 2 | 输出空（NUL）+ 文件 rc=1 | `realloc` grow（tail.c:325 条件恒真）丢内容 → NUL；叠加 `fd_read` EOF 返回 -1 → read error | `realloc` + `fd_read` |
| tac | 输出空 | `bb_get_chunk_from_file` 收尾 `xrealloc(linebuf, idx+1)` shrink 丢整行 | `realloc` |
| cut | `-d: -f2` 输出 `b:c`（应 `b`）| `strsep` 耗尽时置 `*stringp=""` 而非 NULL → cut 解析 `-f2` 时 `ltok!="NULL"` → e=INT_MAX → `-fN` 变 `-fN-` | `strsep` |
| paste | 输出 `,`（内容丢） | realloc 丢内容 | `realloc` |
| expand/unexpand | 输出空 | realloc 丢内容 | `realloc` |
| nl | 缩进有、内容缺 + **user-fault 崩溃** | realloc 丢内容（缩进有内容缺）+ `fclose(stdin)` 解引用哨兵 `(FILE*)1` 崩溃 | `realloc` + `fclose` |
| tr | （报告误报） | 实测 `tr a x` → `xbc`、`tr '\n' ','` → `1,2,` 均正常，无需修复 | — |

### C 级 — 未编译 / 未配置（4 个，2026-08-30 启用 2 个）

| applet | 状态 |
|--------|------|
| sort | ✅ 已启用（`CONFIG_SORT=y`；编译需 `atof`/`strverscmp`/`strptime`，已补入 libc）。实测 `sort`/`-u`/`-n`/`-V`（版本排序）/`-M`（月份排序）全部正确 |
| [ | ✅ 已启用（`CONFIG_TEST1`/`CONFIG_TEST2=y`；本版 busybox 的 `[` 符号是 TEST1 而非 LEFTBRACKET）。实测 `[ ]`/`[[ ]]` 及 shell 脚本内 `if [ ... ]` 判断全部正常 |
| touch | ❌ 未启用（Tier 2）：需 `utimensat`/`futimens` + 内核 mtime 支持（连 `utimes()` 都是 ENOSYS 桩）|
| grep | ❌ 未启用（Tier 2）：需真实 POSIX regex（`regcomp`/`regexec` 现为恒返回 0 的桩 → grep 会匹配每一行）|

> Tier 2（touch/grep）属 P5 ABI「libc 完整性」：文件时间戳族 + POSIX 正则引擎，需独立设计实现。

### 💥 崩溃（内核 user-fault 杀任务）

> 2026-08-22 重测：`sum` 崩溃已消失（printf 修复），`nl` 仍在同一类 libc 缺陷上崩溃。
> 2026-08-24 复查（syscall 边界审计 Task 10）：applet 崩溃均为**用户态 libc 函数缺陷**（`nl` RIP `0x41CE98`、stdio 行读取相关 libc 函数），是 user-mode #PF。audit 在 `do_page_fault` kernel-mode 分支（`!(regs->cs & 3)`）钩了 `__builtin_longjmp(current->fault_jmp, 1)` 用于恢复内核解引用用户指针；user-mode #PF 不进入此分支，仍走 `kill_current_user_task`（杀任务），**审计未触及此路径**。
> **2026-08-30：`nl` 崩溃已定位并修复。** addr2line 当前构建确认 RIP `0x41D048` = `fclose`（`libc/stdio/stdio_file.c:43`）：
> nl 读 stdin 时 `fopen_or_warn_stdin("-")` 返回哨兵 `(FILE*)1`，`fclose(stdin)` 里 `close(mf->fd)` 解引用地址 1 →
> user fault。修复：`fclose` 对 stdin/stdout/stderr 哨兵直接返回 0（与 `fwrite` 的既有特判一致）。
> 300 字符长行复测不再崩溃（300 字符触发 getc 路径多次 `xrealloc` 增长，修复后内容保留、循环正常退出）。
> （报告期 RIP `0x41CE98` 属旧构建，当前构建崩溃点为 `fclose`。）

## 根因归纳（OS01 libc 缺口 → P5 ABI 关联）

| # | libc 缺口 | 影响 applet | 状态 |
|---|----------|------------|------|
| 1 | `printf` 缺 `%f` / `%ld` / `%lu` / `%x` 等格式 | seq, du, cksum, sum, printf, nl | ✅ 已修复 |
| 2 | `strtod` 不支持/损坏（sleep 0.1 失败） | sleep(FANCY), awk(未来) | ✅ 已修复（`CONFIG_FLOAT_DURATION=y`）|
| 3 | `getcwd`/pwd 路径异常 | pwd, $PWD 相关 | ✅ 已修复（实为 realloc 丢内容，getcwd 本身正常）|
| 4 | `getopt` 短选项解析 | cut, paste（-d/-f/-s） | ✅ 已修复（实为 strsep 耗尽不置 NULL）|
| 5 | stdio 行读取/seek（fgets/fseek） | tail, tac, expand, nl | ✅ 已修复（实为 realloc 丢行内容）|
| 6 | user-fault 崩溃函数（RIP 固定） | nl（原 cut/nl/expand/sum）| ✅ 已修复（fclose 哨兵崩溃）|
| 7 | `dirname`/`basename` 桩 | dirname（basename 走 busybox 内部不受影响）| ✅ 已修复（libgen 实现）|
| 8 | `read()` EOF 返回 -1（应 0） | tail 文件输入 read error | ✅ 已修复（fd_read 区分 EOF/错误）|

> 建议：P5 ABI 里新增「**libc 完整性：printf/strtod/getopt/stdio**」任务，
> 本报告为验收基线（修完后重跑 R1/R2 脚本应全绿）。

## § 2026-08-30 修复记录

B 类 9 个 applet 的根因集中在一个核心缺陷（realloc）+ 三个独立缺陷，全部已修复并实测：

| 修复 | 文件 | 缺陷 |
|------|------|------|
| `realloc` | `libc/stdlib/malloc.c` | 读 `((size_t*)ptr)[-1]` 读到 `is_free`+padding（=0）而非头部 `size`（在 `ptr-HEADER_SIZE`）→ copy=0 → **每次 realloc 清空内容**。修复：`hdr->size - HEADER_SIZE`；libk 分支用新增 `ksize()`（`kernel/memory/slab.c`）查 slab 对象大小 |
| `strsep` | `libc/string/extras.c` | token 耗尽时置 `*stringp=""` 而非 `NULL` → 依赖 `==NULL` 判断的解析器出错（cut `-fN` 变 `-fN-`）|
| `fclose` | `libc/stdio/stdio_file.c` | 未处理 stdin/stdout/stderr 哨兵 `(FILE*)1/2/3`，`close(mf->fd)` 解引用地址 1 → user fault |
| `dirname/basename` | `libc/libgen/libgen.c` | 原为桩（恒返 `/` 与 `""`），实现 POSIX（SUSv4）语义 |
| `fd_read` | `kernel/fs/file.c` | `committed==0 → return -1` 把 EOF（vfs_read 返回 0）当错误 → `read()` 到 EOF 后返回 -1，libc 映射 errno=EPERM |

验证：QEMU 真机（`-smp 1` + `-display gtk`）复测 pwd=/、dirname=/a/b、tail -n 2=b\nc、tac=c\nb\na、
cut=b、paste=a,b、expand/unexpand/nl 正常、nl 300 字符长行不崩溃、tr=xbc；systest **228/228** 零回归。
测试程序（realloc_test/eof_test）已清理，环境已还原。

### C 级 Tier 1（2026-08-30 同日）：启用 sort 与 `[`

- `config/busybox.config`：`CONFIG_SORT=y`、`CONFIG_TEST1=y`（`[`）、`CONFIG_TEST2=y`（`[[`）
- sort 编译需要三个缺失的 libc 函数，已补实现：
  - `atof`（`libc/stdlib/strtod.c`，strtod 薄封装）
  - `strverscmp`（`libc/string/strverscmp.c`，版本号比较，sort -V）
  - `strptime`（`libc/time/strptime.c`，日期解析，sort -M 的 %b）
- `Makefile`：fsroot 增加 `sort` 符号链接
- 实测：`[ 1 -eq 1 ]`/`[[ abc == abc ]]`/shell 脚本 `if [ ... ]` 全通；`sort`/`-u`/`-n`/`-V`（v1.2<v1.9<v1.10）/`-M`（Jan<Mar<Dec）全对；systest **228/228** 零回归

## 验证脚本

- `/tmp/applet_verify.sh`（R1 冒烟 52 个 --help + 功能）
- `/tmp/applet_verify_r2.sh`（R2 精确验证 libc 疑点）
- 脚本已存于 homeserver `/tmp/`（fsroot 副本为 gitignored 构建产物）
