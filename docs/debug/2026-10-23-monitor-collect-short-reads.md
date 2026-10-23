# monitor collector 短读与零 TickHz 调查

2026-10-23，基于 `a4db6da` 处理 roadmap monitor parked #10。

## 发现与修复

`read_file_bounded()` 已循环累积短读，读到 EOF 才交给解析器；缓冲区额外
保留一个字节用于超长探测，恰好达到 cap 的文件仍需确认 EOF。因此没有
发现需要修改的短读累积缺陷，本次为这些路径补齐回归。

实际缺陷位于 `monitor_parse_stat()`：`TickHz` 只校验 uint32 数字语法和
范围，没有拒绝 0。非法文件会得到 `stat_valid=1`，继续进入模型；模型的
CPU tick 比例路径并不以正频率作为额外门槛，因此不能依赖下游兜底。

解析器现在要求 `TickHz > 0`，非法 stat 只影响本轮 CPU 指标，其余文件及
进程收集继续执行。没有硬编码当前内核的 100 Hz，测试同时确认 1 Hz 合法。
更新 collect 头文件中的有效性约定，没有改变公开结构布局。

## 回归设计与红绿证据

测试直接 host 编译生产 `user/monitor/collect.c`，沿用已有文件系统 shim：

- 四类 proc 文件分别按 1/2/7/31 字节短读，跨调用拼接数字、字段、换行和
  `Name` 的 `\\x0a` 转义。检查解析结果、多个 PID、复用同一 sample 的第二轮、
  文件描述符上限与最终分配余额。
- 每类文件分别测试 cap−1、cap、cap+1 长度，读取块大小为不限/1/7。
  fixture 是完整合法协议文本加空行；如果 reader 静默截断，解析仍会成功，
  因而能钉死超长探测，而不是靠无效协议偶然拒绝。越界只使对应指标无效，
  或只丢弃对应 PID，另一 PID 保留；每轮均检查 FD 和分配释放。
- `TickHz: 0` 在解析器返回 `-EINVAL`，收集器将 stat 标为无效但仍返回其余
  指标及进程；下一轮替换成合法 stat 后可恢复。

旧生产实现运行新增测试：12 cases 中 11 通过；零频率用例两条断言失败，
分别为解析结果 0 而非 `-EINVAL`、`stat_valid=1`。短读及边界用例原本通过。
最小正值校验后，12 cases / 1278 assertions 全部通过。

## 验证

```sh
make PROFILE=x86_64-clang test-monitor-host CASE=collect
make PROFILE=x86_64-clang test-monitor-host
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang image
```

完整七组 monitor 测试、完整 host 测试与 normal image 构建均 exit 0。
独立代码评审未发现待修正项；以 `-O1`、AddressSanitizer 和
UndefinedBehaviorSanitizer 重编生产 collector 与测试，12 cases /
1278 assertions 全通过，无 sanitizer 报告。

本次没有运行额外 QEMU 故障注入；短读与非法协议由 host fixture 驱动真实
collector 覆盖，用户态镜像构建验证 OS01 编译环境兼容性。
