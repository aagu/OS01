# parked：PTY 并发 resize fixture 初态与线程同步（2026-10-24）

## 结论与修复范围

发现两个宿主 fixture 问题，均位于
`hosttests/cases/test_pty_winsize.c::test_concurrent_resize_consistency`：

1. reader 只接受 A=`25,80,640,480` 或 B=`50,160,1280,960`，但启动
   worker 前未设置窗口。reader 若先执行，会把 `pty_alloc()` 的合法
   默认值 `25,80,0,0` 判为撕裂。writer 先创建不保证 writer 先执行。
2. `volatile bool stop` 被主线程和 reader 写、被两个 worker 读，
   没有同步，存在 C 数据竞争。volatile 不提供线程同步。

修复仅改宿主 fixture，内核源码与 ABI 未变：

- 在任何 worker 启动前，通过真实 master `TIOCSWINSZ` 发布 A 模式。
- 固定启动一个仅读一次的 reader，并 join 后验证结果；此时尚未创建
  writer，从执行顺序保证覆盖 reader-first，而非靠 sleep 猜调度。
- 进入并发阶段前复位控制状态。writer 等待 reader 成功验证一次快照
  后才开始 20,000 次写入。reader-ready 使用 release/acquire，stop
  使用原子 load/store；stop 只传递停止条件，不承担结果发布。
- reader 自有的失败计数、异常四元组和读取结果，以及 writer 自有的
  写入结果，均在 pthread join 后由主线程检查，无需 volatile。
- 检查线程创建/join 和每次 ioctl 的结果；断言 reader 实际读过、
  writer 完成全部 20,000 次写入。异常时打印实际窗口四元组。

本次不修改 A/B 判定条件，也不通过忽略默认窗口或降低写入次数规避失败。
reader-first 验证的是 fixture 准备好的初态，不宣称替代真实 SMP 压力测试。

## 复现与回归证据

先保留旧 fixture 的初始化与 volatile 标志，仅加入 reader-first 阶段：

`make PROFILE=x86_64-clang test-resolution-host RES_CASE=pty`

稳定失败于 reader-first 后的 `ctx.torn_count == 0` 断言（实际为 1）；
11 个场景中 1 个失败，152 条断言中 1 条失败。设置 A 初态并完成同步
修复后，同一根入口 11 个场景、163 条断言全部通过。

另将当前 fixture 复制到 `/tmp`，只将 stop 恢复为 volatile 普通访问，
保留初态设置和其他同步；与真实 `kernel/tty/pty.c`、`kernel/fs/file.c`
一起以 `-O1 -fsanitize=thread -fno-omit-frame-pointer` 编译和链接：

- volatile-stop 对照版：ThreadSanitizer 报告主线程 1 字节写与 reader
  线程 1 字节读之间的数据竞争，退出 66。
- 原子 stop 修复版：11 个场景、163 条断言通过，退出 0，未报告竞争。

临时源、对象和二进制位于 `/tmp/os01-parked-pty-initial-tsan/`；
日志为 `/tmp/os01-parked-pty-initial-{tsan,volatile-tsan}.log`。
此验证不更改工作区生产源或 Make 产物。

## 验证结果

- PTY 聚焦门禁连续运行 20 次，20/20 通过，每轮包含 reader-first 和
  20,000 次并发写入。
- `make PROFILE=x86_64-clang test-resolution-host`：完整九组与 profile
  contract 检查通过。
- `make PROFILE=x86_64-clang test-host`：退出 0，129 个宿主二进制、
  PMM reservation、kernel-selftest runner（43 项）及 user build（7 项）通过。
- `git diff --check` 通过。

红/绿及重复日志：`/tmp/os01-parked-pty-initial-{red,green,repeat}.log`；
完整分辨率日志：`/tmp/os01-parked-pty-initial-resolution.log`。
完整 host 日志：`/tmp/os01-parked-pty-initial-host.log`。

原始一次 host 失败没有记录窗口四元组，因此不能追溯断言该次失败一定
来自默认窗口；本次关闭的是已经确定复现并修复的 fixture 初态和同步
缺口。首次失败的原始记录保留在
`docs/debug/2026-10-24-fbtest-terminal-arm-validation.md`。
本次未运行 QEMU，独立的 ash 与 test_lvgl 用户态 SIGSEGV 项仍保留。
