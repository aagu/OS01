# resolution 客户端 ESTALE 用例确定性失败

2026-10-23，基于 master 合并提交 `4ac18f54`，处理 roadmap 中 GUI-2
总回归发现的 `_client_estale_case` parked 项。

## 复现与根因

同步已有隔离 worktree 到 master，因 GUI-2 带来结构变化，先 `make clean`
再构建 normal image。复用根 profile 的 OVMF 固件，创建私有 800×600 镜像，
用现有 `_boot_session()` 和原 `_client_estale_case()` 运行真实 guest。

原实现稳定报告：

```text
client_estale_no_diagnostic
client_estale_no_exit_status
```

串口上的关键顺序为：

```text
{ /bin/test_lvgl wait; echo RES_LVGL_EXIT=$?; } &
[TEST PASS] LVGL compatibility smoke test succeeded.
[test_lvgl] Interactive mode: Press 'q' or ESC to exit...
RES_LVGL_EXIT=0
setres 1280x720
resolution set to 1280x720
```

BusyBox `ash.c::forkchild()` 在禁用作业控制时把后台任务的 fd0 关闭并打开
`/dev/null`。`test_lvgl wait` 正确地在 stdin EOF 或挂断时退出，故 ready
文本只能证明初始化成功，不能证明随后仍有活着的测试客户端。退出标记
早于 case 的观察起点，后续自然既没有 ESTALE 诊断，也没有新的退出状态。

此外，`gui_platform_step()` 只在 `pending_present` 时调用 gfx_present；
这个 showcase 是静态 UI，单纯循环 step 不会保证下一次 PRESENT。因此
测试还必须显式产生重绘，才能观察旧 view 的 ESTALE。

曾用仅在临时探针中的静默 sleep 管道验证 stdin 生存期：客户端不再在
切换前退出，但切换后仍 exit 0，诊断探针也观察到 writer 提前结束。
PTY resize 向共享 pgrp 广播 SIGWINCH，BusyBox 的有界 sleep 会被中断且
不循环 EINTR。这个方案已移除，没有作为最终实现或豁免保留。

## 修改

`test_lvgl` 新增显式 `--watch-display` 测试模式：

- 沿用真实 onscreen GUI 创建、初始渲染和全部原有清理流程。
- 不读取 stdin，后台 `/dev/null` 不影响其存活，也不争抢 shell 输入。
- 每轮使 screen 失效，再调用现有 `gui_platform_step()`，以 250ms poll
  间隔等待。静态画面也产生真实 PRESENT；ESTALE 对应原有
  `GUI_STATUS_DISPLAY_CHANGED` 分支，打印诊断、释放资源、返回 1。
- 没有增加常驻 pipe producer。进程生命周期由显示错误和已有 QEMU
  session 总截止控制；wait、smoke、默认模式的逻辑保留。

`_start_client()` 改为后台运行这一模式，shell 仍负责打印实际退出码。
用 `mark()` 限定本次启动的 ready/exit，若同一窗口已观察到 exit 则启动
失败。原 case 要求的新 ESTALE 诊断、非零退出码及 shell 存活断言保留。

## 回归与验证

新增 host 用例覆盖旧 ready 不可重用、ready 后立即 exit 0/1/139 不可算启动
成功、旧 exit 不干扰新 ready、未 ready 的退出不可算启动成功。原 helper
在旧 ready 和立即退出断言上失败（4 条断言失败）；修复后整个
模块 34 项全部通过。

真实 QEMU 单独运行原 case，最终得到 `FAILURES []`，关键输出为：

```text
[test_lvgl] Watching display changes (stdin unused)...
resolution set to 1280x720
[test_lvgl] display mode changed, restart the app (errno=116); exiting
RES_LVGL_EXIT=1
```

完整 production suite 通过，包含表面/会话往返、空闲切换、CLI、ESTALE、
off-whitelist、三启动镜像隔离和无 BGA 分支。本机固件未提供 1280×960，
按已有 fallback 在 1600×1200 验证 off-whitelist，未跳过该检查。

另外在真实 guest 顺序执行 `test_lvgl wait </dev/null`、`test_lvgl smoke`、
`test_lvgl`，三者均 exit 0。独立代码评审无待修正项，独立运行 34 项
host 单测通过。

```sh
python3 -m unittest qemutests.test_resolution_switcher
make PROFILE=x86_64-clang test-qemu SUITE=resolution
make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution
make PROFILE=x86_64-clang test-host
```

`FB_RESOLUTION_TEST=1` 完整故障配置亦通过：snapshot/mismatch、terminal
ENOMEM、PTY 窗口通知、无 BGA、drain/mmap/rollback 和三启动隔离均执行。
完整 `test-host` 通过，包括所有 host binaries、PMM boot reservation、
43 项自测 runner 测试与 7 项 user build 测试，命令退出码为 0。

本项解决测试无法保持并驱动活客户端的问题；未修改 GUI 平台的空闲呈现
策略、kernel 信号实现、交互应用的 EOF 语义。另列 parked 的 LVGL 偶发
SIGSEGV 保留，不能因本次通过就声称其已修复。fault suite 当前不调度
`_client_eagain_case`（显示独占使 writer hold 与 GUI client 互斥），其
已有 parked 及 host 互斥验证约定未变。
