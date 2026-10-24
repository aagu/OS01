# terminal-present 提前失败分支调查（2026-10-24）

## 结论与范围

roadmap Parked 的 `UnboundLocalError` 已定位并修复：真实驱动
`qemutests/terminal_present_test.py::_run_terminal_present()` 在入口初始化
`idle_records = []` 和 `idle_wallclock_ns = 0`。采样成功后仍由
`_run_monitor_window()` 的返回值覆盖。

baseline 缺失仍是验收失败，修复让驱动保留原始原因并写出 `summary.json`，
不会补造 baseline，也不会将未采样的空闲窗口视为通过。本次不涉及 guest
呈现代码，未执行完整像素与 30 秒性能验收。

## 根因与复现

这两个局部变量原先只在 monitor 采样返回时赋值，但多个更早的错误分支
已将它们传给 `_build_summary()`。因此原始诊断被二次异常覆盖，JSON
报告也无法写出。

新增回归走真实 `main()` → `_run_terminal_present()` → JSON 报告路径，
不使用 `run_with_fakes()` 简化入口。旧实现下四种场景均复现
`UnboundLocalError: cannot access local variable 'idle_records'`：

- baseline serial log 缺失。
- serial log 存在，但相邻 manifest 缺失。
- manifest 存在，但 JSON 损坏。
- capture 模式下 QEMU 启动抛出 `OSError`。

前三种使用真实临时文件；最后一种仅替换外部启动、版本探测与临时路径，
保留真实报告生成与序列化。每项检查返回码为 1、保留原始失败原因、
记录数为 0，且总体 gate 与 idle gate 都未通过。

`_verify_baseline()` 返回 hard failure 的分支也在赋值前引用这些变量，
同样受入口初始化保护；本次未单独注入 baseline 在检查期间消失的竞态。

## 验证

- 修复前：`python3 -m unittest qemutests.test_terminal_present_runner.DriverEarlyFailureTests`
  的 2 个测试、4 个场景均触发上述异常。
- 修复后：`python3 -m unittest qemutests.test_terminal_present_runner`
  共 28 项通过。
- 完整门禁：`make PROFILE=x86_64-clang test-harness` 返回 0，
  613 项、0 失败、1 跳过（556.601 秒）；同时通过 framework negative-contract
  前置检查。日志：`/tmp/os01-parked-terminal-present-harness.log`。
- 根入口负向验证：
  `make PROFILE=x86_64-clang test-qemu SUITE=terminal-present TERMINAL_PRESENT_BASELINE_LOG=/tmp/os01-parked-terminal-present-missing/serial.log`
  按预期失败（make 返回 2，套件返回失败），无 Python traceback；
  `build/x86_64-clang/test-results/terminal-present/20261024T024807Z-8facf1410f82/summary.json`
  保存 `evidence missing: baseline log ... not found`、`record_count=0`、
  `gate.ok=false`、`idle_window_ns=0`、`idle_window_ok=false`。
  此分支在启动 QEMU 前退出，不能当作 guest E2E 通过。
- `git diff --check` 通过。
