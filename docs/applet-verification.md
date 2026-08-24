# BusyBox Applet 验证报告（2026-08-18）

> 目的：逐个验证 `config/busybox.config` 启用的 applet 在 OS01 上能否运行。
> 方法：自定义 inittab `once:` 执行验证脚本 → QEMU（-smp 1, -serial file）→ 串口镜像收集。
> 环境：homeserver QEMU TCG，busybox v1.36.1 静态链接 OS01 libc，root 为 ext2 R/W。
> 更新（2026-08-22）：`CONFIG_FLOAT_DURATION=y` 已默认开启，busybox 用 `strtod` 的浮点时长路径已验证。
> 更新（2026-08-24，syscall 边界审计 Task 10）：见 §💥，**applet user-fault 属用户态 libc 缺陷，不在 syscall 边界审计作用域**（审计钩子只覆盖内核解引用用户指针，不覆盖用户态非法 RIP）。

## 结论（TL;DR）

- ✅ **52/52 配置的 applet 全部编译进 busybox**，ELF 加载与执行无缺失 syscall 导致的失败
- ✅ **libc printf 格式族已修复**：`%f/%F/%e/%E/%g/%G` 浮点、`%ld/%lu` 及 `%x/%o` 整数全部正常 →
  `printf` / `seq` / `du` / `cksum` / `sum` 已恢复
- ✅ **libc `strtod` 已修复**：`sleep 0.1` 等浮点时长通过（`CONFIG_FLOAT_DURATION=y` 已默认开启）
- ⚠️ 仍有 **~9 个 applet 功能异常**，根因是 **OS01 libc 兼容性缺口**（非 applet 自身、非 syscall 缺失）：
  `getcwd` 异常、`getopt` 解析问题、stdio 行读取/seek 疑点、部分字符串/路径处理
- ❌ 未编译 applet：`sort`（config 缺 CONFIG_SORT）、`[`（LEFTBRACKET 未启用）、`touch`（不在 config）、
  `grep`（CONFIG_GREP 未设置，故 R1/R2 脚本里的 grep 判定为误报）
- 💥 特定路径触发内核 user-fault 杀任务（固定 RIP，指向 libc 内部函数缺陷）

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

### ⚠️ B 级 — 功能异常（libc 兼容性）

> 2026-08-22 重测（真实 QEMU 抓串口）：printf 格式族与 strtod 已恢复，下列表格区分「已恢复」与「仍异常」。

**✅ 已恢复（6 个）— libc printf 格式族 / strtod 修复生效：**

| applet | 实测（QEMU 真实输出） | 结论 |
|--------|----------------------|------|
| printf | `3.1 2147483647 4294967295 ff`（`%.1f %ld %lu %x`）| 已恢复 |
| seq | `1 2 3`（`seq 1 3`）| 已恢复（原 `%.0f` 浮点路径）|
| du | `183 /bin/busybox`（`du -k`，无 `%lu` 字面）| 已恢复 |
| cksum | `3287646509 5`（`cksum`，校验正确）| 已恢复 |
| sum | `08403 1`（`sum`，正确）| 已恢复 |
| sleep | `sleep 0.1` → 成功（`CONFIG_FLOAT_DURATION=y` 走 `strtod`）| 已恢复 |

**❌ 仍异常（9 个）— getopt / getcwd / stdio 行读取 + 崩溃：**

| applet | 现象（实测） | 疑似根因 |
|--------|-------------|---------|
| pwd | 输出空 | getcwd 异常 |
| dirname | `/a/b/c` → `/`（应 `/a/b`）| 字符串/路径处理 |
| tail | `-n 2` 输出空（`-c` 正常）| stdio/lseek 疑点 |
| tac | 输出空 | stdio 行读取疑点 |
| cut | `-d: -f2` 输出空 | getopt/strtoull 解析 |
| paste | `-sd,` 输出空 | getopt 解析 |
| expand/unexpand | 输出空 | stdio/字符分类疑点 |
| nl | 缩进有、内容缺 + **user fault 崩溃** | stdio 行读取 / 崩溃函数 |
| tr | `tr a x` 输出空（`tr '\n' ','` 正常）| 待确认 |

### ❌ C 级 — 未编译 / 未配置（4 个）

| applet | 状态 |
|--------|------|
| sort | config 只有 `FEATURE_SORT_BIG`，缺 `CONFIG_SORT` → applet not found (rc=127) |
| [ | `LEFTBRACKET` 未启用（ash 无内置 test，`[` 需外部 applet） |
| touch | 不在 config（chmod 测试因此无法完整验证） |
| grep | `CONFIG_GREP` 未设置 → R1/R2 脚本里依赖 `grep` 的判定为误报（RAW 输出本身仍可信）|

### 💥 崩溃（内核 user-fault 杀任务）

> 2026-08-22 重测：`sum` 崩溃已消失（printf 修复），`nl` 仍在同一类 libc 缺陷上崩溃。
> 2026-08-24 复查（syscall 边界审计 Task 10）：applet 崩溃均为**用户态 libc 函数缺陷**（`nl` RIP `0x41CE98`、stdio 行读取相关 libc 函数），是 user-mode #PF。audit 在 `do_page_fault` kernel-mode 分支（`!(regs->cs & 3)`）钩了 `__builtin_longjmp(current->fault_jmp, 1)` 用于恢复内核解引用用户指针；user-mode #PF 不进入此分支，仍走 `kill_current_user_task`（杀任务），**审计未触及此路径**。`nl` 仍按原行为崩溃，根因（libc stdio 缺陷）属于 P5 ABI「libc 完整性」工作流，与本次 syscall 边界审计解耦。

- RIP `0x41CE98`：`nl` 子进程（stdio 行读取相关 libc 函数缺陷，固定 RIP = 非随机）
- 需 `addr2line -e build/x86_64/user/busybox.elf 0x41CE98` 定位后修复

## 根因归纳（OS01 libc 缺口 → P5 ABI 关联）

| # | libc 缺口 | 影响 applet | 状态 |
|---|----------|------------|------|
| 1 | `printf` 缺 `%f` / `%ld` / `%lu` / `%x` 等格式 | seq, du, cksum, sum, printf, nl | ✅ 已修复 |
| 2 | `strtod` 不支持/损坏（sleep 0.1 失败） | sleep(FANCY), awk(未来) | ✅ 已修复（`CONFIG_FLOAT_DURATION=y`）|
| 3 | `getcwd`/pwd 路径异常 | pwd, $PWD 相关 | ❌ 未修复 |
| 4 | `getopt` 短选项解析 | cut, paste（-d/-f/-s） | ❌ 未修复 |
| 5 | stdio 行读取/seek（fgets/fseek） | tail, tac, expand, nl | ❌ 未修复 |
| 6 | user-fault 崩溃函数（RIP 固定） | nl（原 cut/nl/expand/sum）| ❌ 未修复（sum 已脱险，nl 仍在）|

> 建议：P5 ABI 里新增「**libc 完整性：printf/strtod/getopt/stdio**」任务，
> 本报告为验收基线（修完后重跑 R1/R2 脚本应全绿）。

## 验证脚本

- `/tmp/applet_verify.sh`（R1 冒烟 52 个 --help + 功能）
- `/tmp/applet_verify_r2.sh`（R2 精确验证 libc 疑点）
- 脚本已存于 homeserver `/tmp/`（fsroot 副本为 gitignored 构建产物）
