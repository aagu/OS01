# OS01 用户态 libc

本文描述 OS01 **用户态** libc（`libc.a`）当前提供的接口、语义边界与已知限制，以及
如何运行它的验证。

> 内核侧的 `libk.a` 是 libc 的子集（kernel-only 的那一半），它的接口边界见
> [架构总览](../architecture.md)。本文只讲用户态。

- 公开头：`libc/include/`（`<stdio.h>`、`<stdlib.h>`、`<unistd.h>`、`<time.h>`、`<dirent.h>`…）
- 生产实现：`libc/{stdio,stdlib,unistd,time,ctype,string,net,errno}/`
- 归档：`build/<profile>/staging/libc/usr/lib/libc.a`（用户态）、`build/<profile>/lib/lib/libk.a`（内核侧）

---

## 1. 当前覆盖的接口组

下表是计划覆盖表（`docs/superpowers/plans/2026-11-08-libc-completion.md`）的落地状态。
每一行都有 **host 组**（宿主上跑生产对象）的证据；**guest 证据**（QEMU 里跑真实 guest）
覆盖其中 10 组（alloc / env / stdio / integer / float / scan / time / path-exec / dir /
append）外加 shutdown（由 guest 的 nettest 用例验收）—— 详见验收报告 §4.1。

| 组 | 交付任务 | 接口 | host 组用例 | 提交 |
|---|---|---|---|---|
| api | T1 | 全部公开头的自包含/单定义/C linkage 契约 | `test_libc_api`（5 例） | `eb530767`、`bd7dd393`、`e8faf7eb` |
| alloc | T2 | `malloc`/`calloc`/`realloc`/`free`/`reallocarray`/`posix_memalign`/`aligned_alloc` | `test_libc_alloc`（7 例） | `9777ead1`、`e727a705`、`de424d15` |
| env | T3 | `getenv`/`setenv`/`unsetenv`/`putenv`/`clearenv` | `test_libc_environ`（9 例） | `bc9bfbb3`、`41675a69` |
| shutdown | T4 | `shutdown` 接真实 syscall | `test_libc_shutdown`（3 例） | `fa26649f` |
| stream | T5 | `feof`/`ferror`/`clearerr`/`ungetc`（含 unlocked 版）与标准流生命周期 | `test_libc_stream_state`（13 例 / 324 断言） | `cec5e02c` |
| append | T6 | 内核真实 `O_APPEND` 原子追加（跨独立 fd/节点） | `test_vfs_append`（9 例） | `8f1f00d6` |
| io | T7 | `fopen`/`fdopen`/`fclose`/`fread`/`fwrite` + 真实 `fcntl` | `test_libc_stdio_io`（13 例） | `df385814` |
| line | T8 | `fgetc`/`getc`/`putc`/`fileno`/`fgets`/`getdelim`/`getline`（含 unlocked 版） | `test_libc_line`（11 例） | `3fd09345`、`a37cd43f` |
| format | T9 | `dprintf`/`vdprintf`/`asprintf` + `fseek`/`fseeko`/`ftell`/`ftello`/`rewind`/`freopen` | `test_libc_stdio_format`（10 例） | `286b95d2`、`a37cd43f` |
| integer | T10 | `strtol`/`strtoul`/`strtoll`/`strtoull`/`atoi`/`atol`/`atoll`/`strtoimax`/`strtoumax` | `test_libc_integer`（7 例） | `606fc3c0` |
| float | T11 | `strtod`/`strtof`/`strtold`/`atof`（按目标精度） | `test_libc_float_parse`（3 例） | `7815e7ac` |
| scan | T12 | `sscanf`/`vsscanf`（宽度、长度修饰符、`%n`、扫描集） | `test_libc_scan`（13 例） | `825141c2`、`1d0ebc97` |
| time | T13 | `gmtime`/`localtime`/`_r`、`mktime`、`strftime`、`strptime`、`asctime`/`ctime`/`_r`、`tzset`、`timegm`、`difftime`、`clock` | `test_libc_time`（10 例） | `4dcf0693`、`b7c33ba6` |
| path | T14 | `realpath` + `execl`/`execlp`/`execle`（`execvp` 的 PATH 搜索） | `test_libc_path`（24 例） | `7aeb732a`、`5f2d8936`、`77061c8d` |
| dir | T15 | `opendir`/`readdir`/`closedir`/`dirfd`/`fdopendir`/`scandir`/`alphasort` | `test_libc_dir`（19 例） | `785e676f`、`22d2d5c3` |

用例数是 **2026-11-09 实测**的（直接跑 `build/x86_64-clang/host-test/*.elf` 的汇总行）。

**本期不实现**（2026-11-09 用户决定）：POSIX 正则引擎（T16）与 glob/globfree 匹配。
见 §4。

---

## 2. 怎么跑验证

```bash
# 全部已交付的 host 组（15 组）
make PROFILE=x86_64-clang test-libc-host LIBC_CASE=all

# 单组（case 名单：api alloc env shutdown stream append io line format
#       integer float scan time path dir regex all）
make PROFILE=x86_64-clang test-libc-host LIBC_CASE=path

# 公开头契约审计（两个 profile）
python3 scripts/audit_libc_api.py --check-headers --include-dir libc/include
python3 scripts/audit_libc_api.py --profile aarch64-clang --check-headers --include-dir libc/include

# guest 端到端（用例表 119 条：118 条通过 + 1 条 SKIP；
#   108 条原有 + T17 的 10 条 libc 验收用例）
make OS01_SYSTEST=1 test-syscall

# RED 复现（这两个是"预期的失败"，退出码非零就是复现成功）
make PROFILE=x86_64-clang test-libc-path-red
make PROFILE=x86_64-clang test-libc-dir-red
```

`LIBC_CASE=regex` 是**已登记但未交付**的组：报错是
`LIBC_CASE='regex' is registered but not delivered in this stage.`，而不是
`unknown LIBC_CASE` —— 它不是拼写错误，是"这一组本期不存在"。

---

## 3. 语义要点（容易踩的地方）

**流的条数上限 32。** `OPEN_FILES_MAX = 32`（`libc/stdio/stdio_file.c`）：同一时刻最多
32 个普通流。超出的 `fopen` 失败。

**`execvp()` 的 PATH 语义。**
- `PATH` **未设置** → 搜 libc 自己的默认路径 `/bin`（POSIX 把默认值留给实现；glibc 用
  `confstr(_CS_PATH)`，本机是 `/bin:/usr/bin`）。
- `PATH` **设为空串** → 读作**一个空段**，即**当前目录**（glibc 同此：当前目录里的同名
  程序会被执行）。
- `PATH` 里的**空段** → 当前目录（`PATH=:/bin:` 会先试 `./prog`）。
- 候选是脚本（内核答 `ENOEXEC`）→ 交给 `/bin/sh`，并且交给 shell 的是**搜索到的候选
  完整路径**（不是裸名），调用方的 `argv[0]` 被丢弃 —— 与 glibc 同构。交给裸名会让
  shell 相对当前目录解析，脚本不在当前目录时找不到，更糟的是会执行当前目录里的另一个
  同名文件。

**目录条目集合。** 每个目录都以 `.` 和 `..` 开头（tmpfs 合成、ext2 存为真实条目），
所以 `readdir`/`scandir` 的条目数**包含这两个**。`d_off` 是**内核的 cookie**，libc 原样
抄写（OS01 内核写的是排序后位置 + 1）。

**`realpath()` 先解析符号链接再应用 `..`** —— 与 glibc 同构。`resolved == NULL` 时由
libc 分配缓冲区，**调用方负责 free**。

**时间。** 内核**没有 RTC**：`SYS_time` 返回开机计数，`CLOCK_REALTIME` 实为单调开机计数。
所以墙钟是不可信的，日历换算请用**固定时间戳**。`clock()`（CPU 时间）明确答"不可用"
（返回 -1），不假装有。

---

## 4. 已知限制

| # | 限制 | 说明 |
|---|---|---|
| 1 | **POSIX 正则未实现** | `libc/include/regex.h` 仍是 T1 的**内联桩**：`regex_t` 就是 `int`，`regcomp()` 恒返回 0（"成功"）但什么都不编译，`regexec()` 恒返回 0（REG_NOMATCH），`regerror()` 只返回缓冲区长度，`regfree()` 是空操作。**2026-11-09 用户决定本期不实现**（T16 推迟到后续周期）。它不是"声明无定义"，但确实属于"假成功"，用它的代码会得到**静默错误的结果**。 |
| 2 | **glob / globfree 未实现** | 计划已排除（连同为它增加的 fnmatch 改造）。`glob.h`/`fnmatch.h` 存在但不属于本轮覆盖范围。 |
| 3 | **只有 C locale** | 没有 `setlocale`，没有多字节/宽字符接口（无 `wchar_t`、无 `mbstowcs`）。`strftime` 的文本与 `%c`/`%x`/`%X` 都是 C locale 形态。 |
| 4 | **TZ 规则不按日期重算** | POSIX TZ 解析器支持 `UTC0`、`<±hh:mm>`、`Jn`、`n`、`Mm.w.d` 形式；同一进程里 `setenv("TZ", …)`/`unsetenv("TZ")` 会刷新缓存。但**换算规则不会按 broken-down time 的日期重新计算** —— 用同一 TZ 解析不同日期的 DST 边界时，偏移取自上次刷新的缓存。 |
| 5 | **`PATH_MAX` 用户程序看不到** | `libc/include/limits.h` 里有 `PATH_MAX 256`（含终止 NUL），并与内核 `VFS_NAME_MAX` 编译期绑定 + host 审计比对。但**用户态程序以 `-ffreestanding` 编译**，clang 自带的 resource `<limits.h>` 会屏蔽项目头 ⇒ 程序里 `PATH_MAX` **不可见**（hosted 的 host 测试组可见）。用户程序暂时只能写死 256。 |
| 6 | **同时最多 32 个普通流** | 见 §3。 |
| 7 | **`clock()` 无 CPU 时间** | 见 §3。 |
| 8 | **1/2/4 字节有符号扫描目标越界** | `sscanf` 往 `int`/`short` 等目标写超范围值时，OS01 **饱和**到 `INT_MIN`/`INT_MAX`；glibc 写 0/-1。C17 7.21.6.2p10 下两者都是 UB。8 字节有符号目标两者一致。 |
| 9 | **`scandir()` 的比较器转型** | 把 `int(*)(const struct dirent **, const struct dirent **)` 转型成 `qsort` 要求的 `int(*)(const void *, const void *)` 是 C 的 UB；本平台 ABI 下实测正确（glibc 内部是同一套写法）。 |
| 10 | **`long double` 是 x87 80 位** | `strtold` 保留 double 之外的位（guest 里 `0.1L != (long double)0.1`）。 |
| 11 | **整数转换越界答饱和值 + ERANGE** | `strtol`/`strtoll` 等越界时饱和到目标边界并置 `errno = ERANGE`（POSIX 要求）。 |

---

## 5. 消费者

libc 的真实消费者都在本仓库里，且**必须走真实库能力**：

- **桌面**（`user/desktop/`）：时钟/日期/Files 视图分别用 `localtime`/`strftime` 与
  `opendir`/`scandir`（Files 打开失败时**保留旧视图**，依赖 `fdopendir` 失败不关 fd 的
  语义）。
- **BusyBox**（`thirdpart/busybox-1.36.1/`）：子模块**未被改写**；`HAVE_GETLINE` 由 BusyBox
  自己的配置决定，libc 侧的 `getline` 是真实现（不再需要 fallback）。
- **内核侧 `libk.a`**：只提供内核需要的子集，**不**要求用户态的 `posix_memalign`/
  `aligned_alloc`/正则/浮点扫描接口（审计脚本的 `LIBK_NOT_REQUIRED`）。
  `realpath` 在 libk 里**保留为历史上的空壳**（恒答 `NULL`）：用户态的真实实现会调用
  `getcwd`/`lstat`/`readlink` 这些**用户 syscall**，不能进内核归档。审计脚本把
  "libk 不许丢掉已支持的导出"当作冻结契约，所以两个 profile 的 libk 都继续导出这个符号，
  但它只是壳（`llvm-nm --print-size`：libk 24 字节 / 用户 `libc.a` 2409 字节）。

---

## 6. 证据

- 逐任务的实施报告：`.superpowers/sdd/2026-11-08-libc-completion/task-*-report.md`
- 批审记录：同目录的 `review-*.md`
- 验收报告：[`docs/superpowers/reports/2026-11-08-libc-completion-validation.md`](../superpowers/reports/2026-11-08-libc-completion-validation.md)
- 计划与规格：[`docs/superpowers/plans/2026-11-08-libc-completion.md`](../superpowers/plans/2026-11-08-libc-completion.md)、
  [`docs/superpowers/specs/2026-11-08-libc-completion-design.md`](../superpowers/specs/2026-11-08-libc-completion-design.md)
