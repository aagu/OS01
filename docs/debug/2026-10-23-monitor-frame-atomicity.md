# monitor 帧被串口诊断输出撕裂

2026-10-23，闭环 roadmap monitor parked #14。

## 现象

monitor QEMU 官方矩阵的 2 核用例超时：runner 等待 `dual-core monitor
frames` 至总截止。guest 侧实际打印了 8 帧，但 `parse_frames()` 只识别出
7 帧——第 8 帧的标题被撕成了两半：

```text
OS01 mo[term-present] version=1 begin_ns=... rect_calls=6
nitor
CPU total: --
```

`[term-present]` 是 terminal 显示层写往同一块串口的诊断记录
（`user/terminal/terminal_display.c::emit_present_record`）。

## 根因

控制台是共享的串口线，但各写入方的**原子性承诺**不同，而且 monitor 的
stdout 并**不是** `/dev/tty`：

- `config/inittab` 只有一条 `tty1:respawn:/bin/terminal`。terminal 应用
  `fork` ash 并 `dup2(slave, 0/1/2)`（`user/terminal/terminal.c:224`），
  所以 ash 及其派生的 `/bin/monitor` 的 fd 0/1/2 是 **PTY 从端**。
- terminal 应用读 PTY 主端（`read(pty_fd, buf, 2048)`，
  `user/terminal/terminal.c:441`）并把读到的整块
  `write(serial_fd, buf, n)`（同文件 `:446`）转发到 `/dev/serial`。
  **转发的块边界是读边界，不是行边界。**
- terminal 自己的诊断记录（`[term-present]`、`[term-probe]`）也是写
  `/dev/serial`（`kernel/fs/devfs.c::serial_write` 整块持 `serial_lock`，
  一次 `write()` 是原子的）。
- 而 monitor 的渲染器 `user/monitor/render.c::tw_putc` 用
  `fprintf(out, "%c", c)` **逐字节**写，OS01 的 libc 无缓冲
  （`libc/stdio/stdio_file.c::vfprintf` → `write_all`，单字符格式即
  单字节 `write()`）。

于是：标题 `OS01 monitor` 是 12 次独立 `write()` 进 PTY 环形缓冲
（`PIPE_SIZE` = 512，`kernel/include/fs/file.h:12`），terminal 应用把它们
读成**若干个小块**并逐块转发；terminal 自己的诊断记录随时可能插在两次
转发之间，把标题撕成 `OS01 mo` + `nitor`。

`parse_frames()` 只接受整行 `OS01 monitor`
（`qemutests/monitor_test.py::parse_frames`），于是这一帧整个丢失。

> 注：`kernel/sched/core.c:557` 给 init 任务接的 `/dev/tty` 不是 monitor
> 的输出路径。monitor 走的是 PTY → terminal 中继 → `/dev/serial`。
> 这个区别决定了保证的层次（见下）。

## 修改

### guest 侧（减少撕裂机会）

`user/monitor/render.c` 的有界写入器改为**按行缓冲**：`struct tw` 增加
512 字节行缓冲，`tw_putc`/`tw_raw` 追加进缓冲，`tw_newline` 用**一次**
`fprintf(out, "%s", line)` 冲刷整行。

- 一次 `fprintf("%s", ...)` = 一次 `write()`，整行**一次**进入 PTY 环形
  缓冲。terminal 中继因此把整行读成一块，再用一次 `write(serial_fd, n)`
  发出（该路径整块持 `serial_lock`，是原子的）。**「读到标题前缀」这种
  撕裂在结构上不再可能。**
- 512 字节的上限覆盖最长的进程行（comm 最长 63 字节，转义后最多 4 字节
  一个，加行前缀、`(expired)` 标记和 ANSI 属性约 300 字节）。溢出时冲刷
  而非丢弃字节——当前行型不会触发（host 回归钉死了这个上界）。
- 缓冲内容不含 NUL（comm 已转义、转义序列可打印），故 `"%s"` 恰好发出
  缓冲的字节。
- 未改 libc、未改内核、未改 monitor 的输出内容与 ANSI 语义。

**这一层保证的边界**：线上（wire）原子性仍受中继块边界约束——
`pipe_write_internal` 在环形缓冲接近满时会把一次写**分段提交**
（`kernel/fs/file.c:925`），串口线上的逐字节写者
（`kernel/driver/fb.c:92` 的 `fb_write`、panic 路径）也仍能撕行。所以
runner 侧的剥离是端到端保证，guest 侧是消除主要撕裂来源。

### runner 侧（端到端保证）

`qemutests/monitor_test.py::parse_frames` 在切行前先剥掉 terminal 的诊断
记录——**连同其行尾换行**，使两侧残片重新接合。

前缀按完整记录形态匹配（`[term-present] version=<n>` /
`[term-probe] wakes/s=<n>`），避免把 comm 里含有字面量 `[term-present]`
的进程行误当成记录。**已知边界**：comm 里含有完整记录前缀
（`[term-present] version=1`）的进程行仍会被剥掉——这是接受的取舍，
替代方案是让撕裂重新吃掉整帧；monitor 自身的门禁不依赖某个进程行必须
在表内。

monitor 自己的 `[MONITOR SPIN]` 记录不在剥离范围内
（`count_epoch_lines` / `parse_spin_epochs` 仍在原文上工作）。

剥离用的两个记录形态是对着发射方写的
（`user/terminal/terminal_display.c` 的 `"[term-present] version=1 …"`、
`user/terminal/terminal.c` 的 `"[term-probe] wakes/s=…"`）。若发射方改了
形态，剥离会静默失配、原始 bug 会带着全绿的测试回来——所以
`qemutests/test_monitor_runner.py` 把这两个形态对着发射方源码钉死。

## 红绿回归

### guest 侧

`hosttests/cases/test_monitor_render.c` 新增
`test_output_writes_whole_lines`：seam 统计每次输出的写调用数与「不是
恰好一整行」的调用数，要求**每次写调用恰好承载一整行**（一个 `\n` 且在
末尾），并要求调用数等于换行数。plain 与 ANSI 两种布局各测一遍。

旧实现（RED）：

```text
[FAIL] assert_eq(cap_split_calls=116, 0=0)     /* plain：116 次写全部不是整行 */
[FAIL] assert_eq(cap_split_calls=139, 0=0)     /* ANSI */
[FAIL] assert_eq(cap_calls=147, nlines=8)      /* 8 行输出用了 147 次 write() */
```

修复后（GREEN）：84 条断言全通过，ANSI 帧 8 行 = 8 次写。

另有 `test_longest_line_stays_one_write` 钉死 512 字节行缓冲的上界：用
63 个控制字节的 comm（转义后 252 字节，最长的一行）渲染，要求仍无拆分。
把 `TW_LINE_MAX` 临时改成 128 后该用例报 `cap_split_calls=2`（RED），
改回 512 后通过——即上界不是靠算术保证的。

### runner 侧

`qemutests/test_monitor_runner.py` 新增四项：

- `test_torn_header_recovers_the_frame`——标题被诊断记录劈开时仍应识别出
  那一帧（RED：`AssertionError: 0 != 1`；GREEN：1 帧，total 5500）。
- `test_diagnostic_between_lines_keeps_the_frame`——诊断记录插在帧的两行
  之间不丢帧。**注意：这一项旧实现也通过**，它是回归护栏，不是红绿用例。
- `test_monitor_spin_lines_are_not_stripped`——monitor 自己的
  `[MONITOR SPIN]` 记录不被剥离（断言 `_strip_diagnostics()` 的输出，
  而非 `count_epoch_lines()` 的原始文本——后者不经过剥离，钉不住这件事）。
- `test_strip_matches_the_terminal_record_emitters`——把剥离用的两个记录
  形态对着 `user/terminal/terminal_display.c` 与 `user/terminal/terminal.c`
  的发射源码钉死，避免发射方改形态后剥离静默失配。

## 验证

```sh
make PROFILE=x86_64-clang test-monitor-host CASE=render
make PROFILE=x86_64-clang test-monitor-host
make PROFILE=x86_64-clang test-qemu SUITE=monitor
make PROFILE=x86_64-clang test-host
```

官方 QEMU monitor 矩阵：**1 核 PASS、2 核 PASS、8 核 PASS**，整条命令
返回 0（2 核此前因标题交错超时）。

2 核 transcript 归档于
`build/x86_64-clang/logs/tests/monitor/<run>/cpu2/stdout.log`：其中确实
混入了 18 条 `[term-present]` 与 31 条 `[term-probe]` 记录，而 16 个
`OS01 monitor` 标题**全部完整**（没有任何 `OS01 mo` / `nitor` 残片）。
即交错场景真实发生，但帧没有再被撕开。

中继块边界仍可能在**行之间**落诊断记录（2 核 transcript 里就有 4 条落在
shell 提示符行内），这由 runner 侧的剥离吸收——即上面说的「guest 侧消除
主要撕裂来源、runner 侧是端到端保证」。

2 核本次仍出现一次 `spin migrated during window; retrying (attempt 1)`，
随后第 2 次尝试观察到稳定归属窗口并 PASS。那是 roadmap monitor parked #6
记录的绑核验证强度缺口（spin 归属采样粒度约 0.2 s），与本项的帧撕裂
无关，未因此改动。

## 未做

- 未改 libc 的无缓冲策略（stdout 逐字节写是全系统现状，改动面远大于本项）。
- 未改 `kernel/driver/fb.c` / panic 路径的逐字节串口写入：它们是内核侧
  的另一个问题，runner 侧的剥离已让 monitor 用例对这类撕裂免疫。
- 未改 terminal 中继（`user/terminal/terminal.c:441`）的块边界：让转发按
  行切分会改变 PTY 全部流量（含 shell 输出）的转发语义，且诊断记录仍
  是独立 fd 的独立写，行级转发也挡不住它——真正的端到端保证只能在
  消费者一侧，故留在 runner 侧。
- 未改 monitor 的输出内容、ANSI 语义或帧格式。
