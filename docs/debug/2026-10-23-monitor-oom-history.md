# monitor OOM 历史空点与第二次分配回滚

2026-10-23，基于 `5fe2066` 处理 roadmap monitor parked #9。

## 根因及约定

`user/monitor/monitor.c` 每次收集 sample 后调用 model_update，随后无论返回值
都渲染并计入一个 sampling round。`model.c` 先分配本轮进程显示行和差分
基线两个数组，分配失败会返回 `-ENOMEM`。原先这两个失败分支都在更新
历史环之前返回，所以 OOM 轮在图中完全消失；连续 OOM 期间，60 槽横轴
保持不动而不表示丢失的采样轮。

已有接口明确要求 OOM 时保留上一次提交的数据，以免部分提交破坏下一轮
CPU/进程差分。这一要求保留。原接口的“整个 model 完全不变”则需要收窄：
**数据显示快照和基线不变，历史记账仍推进**。头文件和 monitor spec §7
同步写明此区别，不把失败轮的 counters 当作已提交样本。

## 修改

两处分配失败进入共同 OOM 出口，为 total CPU 和 memory 历史各追加一个
`valid=false` 空点。第二次数组分配失败时仍先释放第一个临时数组，没有
新分配、外部 API 或结构布局变化。NULL 参数的 `-EINVAL` 路径不推进历史。

内存和 CPU 的旧显示快照保留，失败轮未提交其新值；历史也不复制这些旧值
为当前点。下一次成功更新仍从最后一次提交的 CPU/process baseline 差分，
使用真实时间间隔，不插值或伪造失败期间的 CPU 百分比。

## 确定性回归

沿用 hosttests 对真实 model.c 的 malloc/free 重命名 seam：在分配 1 或
分配 2 处分别注入失败。起点是有效 CPU=50%、进程=50%、内存=25% 的快照，
失败 sample 则含不同 tick_hz、epoch、PID 和内存数值，便于发现部分提交。

每个分配位置独立执行 62 次失败：

- 每次返回 ENOMEM，只有两条 history 允许变化；其余 model 字节、原数组指针
  和 pointed-to 行/基线内容保持不变。
- 余额始终为两个已提交数组：尤其第二次失败不能遗留临时行数组。
- 第一次失败追加空点，上一点仍有效且数值不变；连续 62 次失败后 60 槽
  环中全为空点，旧点正常被淘汰。
- 恢复 sample 位于 64 秒，上一基线位于 1 秒：3150 ticks / 63 秒 / 100Hz
  得到 50%，证明没有提交中间失败样本；恢复点前仍是空点，内存新值为 40%。
- destroy 后余额归零。

旧实现中两个新增用例均失败：history count 仍为 2（预期首个失败轮后为 3），
最新点仍是上一轮有效值，连续失败也不发生回绕。既有第二次分配回滚本来
正确，本次补齐其覆盖，没有将未发现的泄漏描述成已修复漏洞。

## 验证

```sh
make PROFILE=x86_64-clang test-monitor-host CASE=model
make PROFILE=x86_64-clang test-monitor-host
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang image
```

最终 model 专项：14 cases / 1164 assertions 全通过。完整七组 monitor
测试、完整 host 和用户态 normal image 构建均 exit 0。独立代码评审无待修正项，
并用生产 model.c 加故障注入测试完成 AddressSanitizer/UndefinedBehaviorSanitizer
验证，无内存或 UB 报告。

此次没有给实际 OS01 程序添加 OOM 注入开关；故障路径由 host 直接运行真实
model 代码覆盖。没有改变采样周期、收集器或内核指标接口，也没有清空数据
基线来制造历史空点。
