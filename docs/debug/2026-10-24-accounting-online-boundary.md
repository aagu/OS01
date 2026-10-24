# monitor parked ⑦：CPU online 边界调查（2026-10-24）

## 结论

`sched_accounting_read()` 的生产行为正确，原问题是宿主 fixture 缺失。
本次只补测试，不改内核实现、结构体或 ABI。

- `cpu >= NR_CPUS` 或输出指针为空：返回 `-EINVAL`。
- `num_cpus <= cpu < NR_CPUS`：返回 0，`online=0`。
- `cpu < num_cpus`：`online` 来自 `percpu_data[cpu].online`，不能仅由
  CPU 已枚举或计时器已启动来推断。
- 读取不清空已有 busy/idle 计数、epoch 或 tick rate。
  `valid` 按既有契约表示已初始化、计时器启动且 scheduler ready，
  与 `online` 分别报告。本次不引入 CPU hotplug 语义。

生产代码见 `kernel/sched/accounting.c::sched_accounting_read()`；
计时器有效性契约见 `kernel/include/sched/accounting.h`。

## 覆盖与有效性验证

在 `hosttests/cases/test_monitor_accounting.c` 新增两个测试，链接真实
`accounting_production.o`，只通过已有 runtime fixture 控制 CPU 状态：

1. 使用最后一个合法槽 `NR_CPUS-1`，显式 stop/start 计时器后注入
   3 个 busy tick、2 个 idle tick。分别设置 `num_cpus=1`、
   `NR_CPUS-1`、`NR_CPUS`，验证范围外、恰好等于边界、重新进入范围内
   的返回值与 online 状态，并逐项验证计数、epoch、频率及 valid。
   范围外时刻意保留 per-CPU online=1，以捕捉忽略 `num_cpus` 的回归。
2. 对范围内 CPU 将实际 online 标志从 1 切为 0 再恢复为 1，验证读取
   跟随该标志，同时保持已有记账。测试后恢复控制状态，避免污染后续测试。

现有实现本来正确，因此不把本次描述为“旧实现复现 bug”。为证明新增
测试能捕捉对应错误，在 `/tmp` 中复制生产源，沿用宿主构建的编译选项和
真实测试对象，分别编译并运行三个错误版本；未改动工作区生产源码：

| 实现 | 结果 |
|------|------|
| 原始生产实现 | 120/120 断言通过，退出 0 |
| 删除 `cpu < num_cpus` 检查，直接取 per-CPU online | 2 条断言失败，退出 1 |
| 边界改为 `cpu <= num_cpus` | 1 条断言失败，退出 1 |
| 范围内一律返回 online=1 | 1 条断言失败，退出 1 |

## 最终验证

- `make PROFILE=x86_64-clang test-monitor-host CASE=accounting`：通过，
  12 个测试、120 条断言（此前 10 个测试、84 条断言）。
- `make PROFILE=x86_64-clang test-monitor-host`：七组全部通过。
- `make PROFILE=x86_64-clang test-host`：退出 0，完整宿主二进制、PMM
  reservation、kernel-selftest runner 与 user build 检查通过。
- `git diff --check`：通过。

日志分别为 `/tmp/os01-parked-accounting-online.log`、
`/tmp/os01-parked-accounting-monitor.log`、`/tmp/os01-parked-accounting-host.log`；
错误版本日志为 `/tmp/os01-parked-accounting-{ignore_num_cpus,inclusive_boundary,ignore_percpu_flag}.log`。
本次未运行 QEMU；新增覆盖针对宿主可控制的 CPU 枚举边界，不声称完成
CPU 热插拔或严格绑核验证（parked ⑥ 仍保留）。
