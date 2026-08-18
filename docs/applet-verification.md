# BusyBox Applet 验证报告（2026-08-18）

> 目的：逐个验证 `config/busybox.config` 启用的 applet 在 OS01 上能否运行。
> 方法：自定义 inittab `once:` 执行验证脚本 → QEMU（-smp 1, -serial file）→ 串口镜像收集。
> 环境：homeserver QEMU TCG，busybox v1.36.1 静态链接 OS01 libc，root 为 ext2 R/W。

## 结论（TL;DR）

- ✅ **52/52 配置的 applet 全部编译进 busybox**，ELF 加载与执行无缺失 syscall 导致的失败
- ⚠️ 但约 **15 个 applet 功能异常**，根因是 **OS01 libc 兼容性缺口**（非 applet 自身、非 syscall 缺失）：
  `printf` 缺 `%f/%ld/%lu`、`strtod` 不支持、`getcwd` 异常、`getopt` 解析问题、stdio 行读取疑点
- ❌ 未编译 applet：`sort`（config 缺 CONFIG_SORT）、`[`（LEFTBRACKET 未启用）、`touch`（不在 config）
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

### ⚠️ B 级 — 功能异常（libc 兼容性，15 个）

| applet | 现象 | 疑似根因 |
|--------|------|---------|
| seq | 输出空 | busybox 用 `printf("%.0f")` → OS01 printf 无 `%f` |
| du | 字面 `%lu\t/etc` | **printf 缺 `%lu`**（实锤：格式符原样输出） |
| cksum | 值 = 2^64-1876（溢出） | 同上 `%lu` / 算法 |
| sum | 输出空 | printf 格式问题 |
| printf | `%d`→`%ld` 字面、`%f` 字面 | **printf 缺 `%ld/%f`** |
| sleep | `0.1` → invalid number | **strtod 不支持/有 bug**（整数 OK） |
| pwd | 输出空 | getcwd 异常 |
| dirname | `/a/b/c` → `/`（应 `/a/b`） | 字符串/路径处理 |
| tail | `-2` 输出空 | stdio/lseek 疑点（`-c` 正常） |
| tac | 输出空 | stdio 行读取疑点 |
| cut | `-d: -f2` 报 invalid number / 空 | getopt/strtoull 解析 |
| paste | `-sd,` 被当文件名 `-` | getopt 解析 |
| expand/unexpand | 输出空 | stdio/字符分类疑点 |
| nl | 缩进有、内容缺 | printf 格式疑点 |
| tr | `tr a x` 偶发空（`tr '\n' ','` 正常） | 待确认 |

### ❌ C 级 — 未编译 / 未配置（3 个）

| applet | 状态 |
|--------|------|
| sort | config 只有 `FEATURE_SORT_BIG`，缺 `CONFIG_SORT` → applet not found (rc=127) |
| [ | `LEFTBRACKET` 未启用（ash 无内置 test，`[` 需外部 applet） |
| touch | 不在 config（chmod 测试因此无法完整验证） |

### 💥 崩溃（内核 user-fault 杀任务）

- RIP `0x41CD38` ×3：cut/nl/expand 相关子进程
- RIP `0x421B48` ×1：sum 相关子进程
- 固定 RIP = 同一 libc 函数缺陷（非随机崩溃），需 addr2line 定位后修复

## 根因归纳（OS01 libc 缺口 → P5 ABI 关联）

| # | libc 缺口 | 影响 applet | 工作量估计 |
|---|----------|------------|-----------|
| 1 | `printf` 缺 `%f` / `%ld` / `%lu` / `%x` 等格式 | seq, du, cksum, sum, printf, nl, du | 半天~1 天 |
| 2 | `strtod` 不支持/损坏（sleep 0.1 失败） | sleep(FANCY), awk(未来) | 半天 |
| 3 | `getcwd`/pwd 路径异常 | pwd, $PWD 相关 | 半天 |
| 4 | `getopt` 短选项解析 | cut, paste（-d/-f/-s） | 半天 |
| 5 | stdio 行读取/seek（fgets/fseek） | tail, tac, expand | 半天~1 天 |
| 6 | user-fault 崩溃函数（RIP 固定） | cut/nl/expand/sum | 需 addr2line 定位 |

> 建议：P5 ABI 里新增「**libc 完整性：printf/strtod/getopt/stdio**」任务，
> 本报告为验收基线（修完后重跑 R1/R2 脚本应全绿）。

## 验证脚本

- `/tmp/applet_verify.sh`（R1 冒烟 52 个 --help + 功能）
- `/tmp/applet_verify_r2.sh`（R2 精确验证 libc 疑点）
- 脚本已存于 homeserver `/tmp/`（fsroot 副本为 gitignored 构建产物）
