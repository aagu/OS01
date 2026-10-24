# dispatch 第一期 shootdown-probe 遗留调查

日期：2026-10-24。调查基线：`61a5d15d952720251292fe5133efe3a17b7d716a`。

## 结论

本次复现确认，`MODE=m3-probe` 的验收失败来自内核与 runner 的串口标记
前缀不一致。对齐解析器后，同一内核镜像的真实双核 QEMU 探针通过。
此次调查没有发现必须修改 dispatch、GIC 或 TLB shootdown 实现的证据。
这不构成对所有 shootdown 并发情形的正确性证明。

初始调查阶段没有修改生产代码、runner 或 roadmap 的待办状态，也没有
提交。下述实验只在独立 Python 进程内替换四条正则；随后按用户要求进行
的正式修复及其证据见文末。

## 根因与测试盲区

- 内核 `kernel/include/arch/aarch64/boot/shootdown_probe.h` 的探针体输出
  `SHOOTDOWN-PROBE: START/OK/FAIL`。
- 单核跳过标记位于 `kernel/arch/aarch64/boot/main.c`，同样使用
  `SHOOTDOWN-PROBE: SKIP`；双核正式验收不接受 SKIP。
- `qemutests/aarch64_m3_probe.py` 的 `_START_RE`、`_OK_RE`、`_FAIL_RE`、
  `_SKIP_RE` 仍使用 `M3-SHOOTDOWN-PROBE:`。`run_case()` 用
  `probe_evidence()` 等待成功证据，因此真实 OK 不能结束等待。
- `045cba3f`（2026-10-05）清理里程碑命名时改了内核输出及 C host fixture，
  未同步 Python runner。该提交早于 dispatch 第一期。
- runner 自检与 `test_aarch64_harness.py` 的七项聚焦测试均通过，但它们
  使用 runner 自带的旧前缀 fixture，没有校验与真实生产者的协议一致性。
- runner 的四个 `_..._PREFIX` 常量当前没有引用；有效判定来自四条正则。
  只改这些常量不会修复问题。Makefile 注释也仍记录旧 wire 前缀。

## 本次实测

1. 原样执行 `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe`：
   根 make 退出 2，runner 退出 1，归档 `status=FAIL`，耗时约 90.50 秒，
   `timeout=true`、`ok_observed=false`。串口实际恰有 START 和 OK，均为
   新前缀。归档：
   `build/aarch64-clang/logs/tests/aarch64-m3-probe/20261024T115455Z-dd5d0ddf060b436c86eab220d156fba5/`。
2. 同一份串口日志离线对照：原解析器返回 False；仅对齐四条正则的前缀
   后返回 True。追加新前缀 FAIL、SKIP、重复 OK 均被拒绝。
3. 不改仓库文件，在独立进程内对齐四条正则，使用同一固件、镜像、
   `virt,gic-version=2,acpi=off`、两核 cortex-a53 和 diagnostic DTB 机制
   再跑真实 `run_case()`：退出 0，`result=PASS`、`timeout=false`、
   `ok_observed=true`。这只是实验 PASS，原样根目标仍未闭环。
   实验归档：
   `/tmp/os01-shootdown-probe-prefix-experiment/build/logs/tests/aarch64-m3-probe/20261024T115635Z-eae271cbb8f845b3aa65e9aec83d31a7/`。
4. `python3 qemutests/aarch64_m3_probe.py --self-test` 通过；
   `python3 -m unittest qemutests.test_aarch64_harness.M3ProbeEvidenceTests qemutests.test_aarch64_harness.M3ProbeLifecycleTests`
   七项通过，说明现有自检无法捕捉本次跨组件漂移。

原样日志：`/tmp/os01-shootdown-probe-investigation-live.log`。
实验脚本与日志：`/tmp/os01-shootdown-probe-prefix-experiment.py`、
`/tmp/os01-shootdown-probe-prefix-experiment.log`。

## 后续修复边界

建议保留当前内核的 `SHOOTDOWN-PROBE:` 命名，将 runner 的四条正则、
fixture、自检和相关 Makefile 注释一起对齐；删除未使用的旧前缀常量或
让正则确实由统一常量构造。不要只改 START/OK 而漏改 FAIL/SKIP。
增加独立于 runner 自带 fixture 的生产者协议回归，覆盖实际前缀、三种
换行、乱序、缺失、重复、近似标记及成功后出现 FAIL/SKIP 的拒绝行为。

最终必须在正式源文件上修复，并让原样根命令实际 PASS、无 SKIP/FAIL，
才能移除 roadmap 待办。阶段一历史失败报告应保留，仅新增后续闭环引用。

## 修复与正式验收

用户要求开始修复后，已完成以下修改：

- runner 四条正则、合成日志、自检及根 Makefile 注释统一为当前内核的
  `SHOOTDOWN-PROBE:`；移除四个无引用的旧前缀常量。
- 独立 wire fixture 覆盖 LF/CRLF/LF+CR、缺失、乱序、重复、旧前缀、
  近似及非行首标记，以及成功序列前后出现 FAIL/SKIP 的拒绝行为。
- Python 回归临时编译既有 C host fixture，执行真实共享 probe 函数体，
  将成功日志与 stale AP read 的失败日志交给解析器。失败日志移除额外
  START 后再追加到成功日志，确保拒绝原因是生产者的 FAIL 标记。
  编译/执行错误直接失败，不跳过；编译产物仅存在临时目录。

新增回归在旧 runner 上出现 5 次预期失败；修复后聚焦 11 项全通过。
`python3 qemutests/aarch64_m3_probe.py --self-test` 通过，完整
`python3 -m unittest qemutests.test_aarch64_harness` 为 **56 项通过**。

正式源文件上的原样命令：

```
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe
```

退出 **0**；归档 `status=PASS`、`runner_exit_code=0`，耗时约 **1.33 秒**，
`timeout=false`、`ok_observed=true`。另检查串口 START/OK 恰一次，
无 probe FAIL/SKIP。正式归档：
`build/aarch64-clang/logs/tests/aarch64-m3-probe/20261024T120108Z-b4baf77e74f24b159a44f042aabc8434/`。

日志：`/tmp/os01-shootdown-probe-fix-{red,green,harness,live}.log`。
roadmap 的这条遗留验收已关闭，其他 dispatch follow-on 保留。
阶段一历史失败报告未改写；本次未修改内核实现。
