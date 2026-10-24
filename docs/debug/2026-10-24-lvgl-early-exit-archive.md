# parked：test_lvgl 永久失败跳过 alt-screen 恢复（2026-10-24）

## 结论

该 parked 项已由历史迁移消除，roadmap 未同步删除。本次只归档调查，
不新增业务代码修复，也不将历史修复归因于本次工作。

原条目引用旧路径 `user/test_lvgl.c`；当前源为
`user/test_lvgl/test_lvgl.c`。

## 历史证据

两个提交均已通过 `git merge-base --is-ancestor <commit> HEAD` 确认
包含于当前主线：

- `7cefb01acd43d5ac0dc0d5f25f43ccce0f809e36`
  （2026-10-11，`feat(user): migrate fullscreen clients to explicit display ownership`）
  删除进入与退出 alt-screen 的 `write(...1049h...)` / `write(...1049l...)`，
  改为显式显示所有权。因此原条目的“跳过 alt-screen 恢复”前提已经消失。
- `acebb9dfb38b641fd8d40630ac7aff611b32f953`
  （2026-10-20，`refactor(test_lvgl): use gui platform lifecycle and poll step loop`）
  删除旧 `gfx_present_ok()` 调用后的直接 `return 1`，改用
  `gui_platform_step()` 状态和统一 `cleanup`。

## 当前控制流核查

对当前 `main()` 的错误退出逐项核查：

- 启动时钟查询失败：直接返回 1，但发生在平台创建前，`platform=NULL`，
  没有已获得的显示所有权或需恢复的 alt-screen。
- 平台创建失败、断言失败、smoke 超时或 step 失败：转入统一 cleanup。
- 初始屏幕帧、wait/watch-display 循环及默认展示阶段遇到
  `GUI_STATUS_LOST_OWNERSHIP` / `GUI_STATUS_DISPLAY_CHANGED` /
  `GUI_STATUS_ERROR`：报告诊断、设置非零返回值并进入 cleanup。
- 时钟/poll 错误同样进入 cleanup；正常退出也走同一清理出口。
- cleanup 在平台非空时调用 `gui_platform_destroy()`。
  该函数销毁仍挂接的输入、LVGL display/global state，释放仍拥有的
  gfx 所有权、关闭 gfx handle，并释放映射和平台对象。

这项结论针对旧条目中的普通错误返回路径。LVGL fatal assert 的
`_exit(1)` 与 SIGKILL 属于进程退出回收路径，不声称它们调用了应用的
cleanup；也不以本次核查关闭另一个独立 parked 项：guest `test_lvgl`
偶发 SIGSEGV。

## 本次验证

- `make PROFILE=x86_64-clang test-gui-host CASE=platform`：退出 0，
  `test_gui_platform` 通过。已有 fixture 覆盖创建回滚、EBUSY/EIO、
  EACCES 所有权丢失、ESTALE 显示变化、错误后 destroy 和重新 create。
- `make PROFILE=x86_64-clang test-gui-host CASE=lvgl`：退出 0，
  `test_gui_lvgl_integration` 通过，包含真实 LVGL 渲染、50 轮
  create/step/destroy、映射计数平衡和独立子进程池耗尽 fatal 验证。
- `git diff --check`：通过。

日志：`/tmp/os01-parked-lvgl-cleanup-platform.log` 与
`/tmp/os01-parked-lvgl-cleanup-lvgl.log`。

本次未运行新的 guest QEMU 验证；以上宿主测试验证平台层行为，不能代替
guest 显示恢复或应用故障路径的端到端注入。本项归档的直接依据是旧协议
及旧早退路径已从主线移除，且当前普通错误退出统一到现有生命周期接口。
