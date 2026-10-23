# monitor host 分发的能力与 CASE 门禁

2026-10-23，基于 `b243972` 处理 roadmap monitor parked #13。

## 根因

`test-monitor-host` 把 `_test-monitor-host-run-*` 注册为不带能力判断的
前置目标，`require_capability(rootfs)` 则在主目标 recipe 内。GNU Make
先更新前置目标，再展开 recipe，因而不支持 rootfs 的 aarch64 profile
也会先启动 hosttests 子 make，最后才报能力不足。

未知 CASE 被直接拼成依赖名，Make 在主 recipe 之前就因缺少规则失败。
原有 shell case 里的友好报错分支永远到不了。`-j` 可并发启动合法前置
任务，`-n` 也会执行递归 make 配方，所以只把检查移到 recipe 首行无效。

## 修复

- 对 CASE 先去除两侧/重复空白。空值表示默认整组；否则必须恰好一个
  word，并用七个注册名称作精确白名单，不能让 `%` 充当 filter pattern。
- 仅在 profile 具有 rootfs 且 CASE 合法时生成前置目标。无效请求没有
  需要执行的子任务，然后在主 recipe 先给出能力错误或未知 CASE 提示。
- 错误不在 Makefile 无条件解析时触发，无关目标仍可使用自己的 CASE。
  此次未改变其他 focused host 入口或扩大为全项目 Makefile 重构。
- 注册列表同时决定调度与错误提示，不再另维护一份 shell case 枚举。

保留“省略 CASE 跑全部七组”的接口；`CASE=all` 原本不支持，仍会被拒绝。
纯空白规范为默认整组。没有新增公开 target、profile 能力或构建产物。

## 确定性回归

`qemutests/test_monitor_host_dispatch.py` 实际启动仓库根 Makefile，采用真实
x86_64/aarch64 profile；仅将递归 make 可执行程序替换成临时目录中的
记录器。记录器写 JSON 参数并返回成功，不运行 compiler 或 host binary。
因此可以直接观测拒绝请求是否越过了子 make 边界，而不是检查 Makefile
字符串或依赖现有构建缓存。

五个测试方法覆盖：

- aarch64 profile、空/合法/未知 CASE，普通、`-n`、`-j4`、`-n -j4`；
  都要求明确的能力错误，且记录器零次调用。
- 未知 CASE、`%`、`all`、多个合法 CASE、混合合法/未知 CASE；要求列明
  合法值、无 “No rule to make target”、且零次递归调用。
- 七个合法 CASE 各自恰好一次正确 submake，包含 `-n`。
- 省略、显式空值、纯空白的默认整组；普通和并行调度均恰好七组各一次。
- 无关 `print-run-paths` 目标不因其 CASE 不在 monitor 列表而失败。

父 make 会把命令行变量导出到环境，fixture 只继承 PATH/HOME/TMPDIR，
保证 profile 及 variant 测试不受调用者的构建标志污染。所有记录与临时
可执行程序均位于临时目录并在测试结束后删除。

旧代码运行当前回归：5 个测试方法中，共 27 个子场景失败。修复后全部通过。
新增模块已加入 `TEST_HARNESS_MODULES`，由现有 `test-harness` CI job 执行。

## 验证

```sh
python3 -m unittest qemutests.test_monitor_host_dispatch
make PROFILE=x86_64-clang test-monitor-host
make PROFILE=x86_64-clang test-harness
```

真实七组 monitor host 测试通过，完整 `test-harness` 返回 0：607 项测试，
0 失败、1 项跳过；包含新增的五个分发测试方法。独立评审无待修正项，
独立运行新增模块五项测试及 `git diff --check` 均通过。

本次没有运行额外 QEMU 启动：变更只影响 host make 入口的依赖选择与错误
分发，真实 monitor host 组和根 Make 回归直接覆盖相应路径。
