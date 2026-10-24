# procfs legacy 节点读取容量边界调查

日期：2026-10-24。对应 roadmap monitor 遗留项 ④。

## 调查结论

`procfs_read()` 的 legacy 路径分配 4096 字节，并通过
`procfs_generate_node()` 生成 status/stat/thermal/meminfo。生成器复用
per-open snapshot 的生产者，最后只复制 `s->len` 个文本字节，不复制 NUL。
原判断 `s->len >= cap` 却要求调用方额外留一个字节，因此合法的等长目标
缓冲区会得到 `-EOVERFLOW`。这是字节读取和 C 字符串容量约定混用。

原 parked 项提出的“4096 字节 status 导致空读”当前不可达：
`procfs_fmt.c` 的 `emit()` 检查 snprintf 截断，成功时文本长度严格小于
formatter 容量；status 的生成容量就是 4096。超过该容量时整体失败，
不会返回截断文本。本次没有声称在正常 guest 中复现过该空读。

修复把目的缓冲区检查改为 `s->len > cap`，并在公开内核接口注释中明确
“仅复制文本字节、不补 NUL、等长容量足够”。无需扩大 legacy 缓冲区；
现有输出、per-open 快照和用户 ABI 均不改变。未来若生产者输出增长到
超过 legacy 容量，仍按原约定返回溢出，不能静默截断。

## 回归覆盖

`test_legacy_node_capacity_boundary` 对四类节点使用真实生产者：先读取
per-open snapshot 作为预期字节流，固定 fixture 时钟和数据源，再分别用
长度减一、等于长度、长度加一的目标容量调用 legacy 生成器。

- 短一个字节返回 `-EOVERFLOW`，整个目的数组保持不变。
- 等长成功，内容与 per-open 结果一致，前后 canary 不变。
- 多一个字节成功，空余字节不被写成 NUL，前后 canary 不变。
- 每次成功和失败都检查 snapshot 分配余额恢复。

旧实现：procfs 13 场景中该新场景失败，544 条断言中 8 条失败（四种节点
各一个返回值和一个字节内容失败）。修复后：13 场景、544 条断言全部通过。

## 验证

- `make PROFILE=x86_64-clang test-monitor-host CASE=procfs`：红绿验证通过。
- `make PROFILE=x86_64-clang test-monitor-host`：完整七组通过。
- `make PROFILE=x86_64-clang test-host`：全部宿主二进制通过；PMM boot
  reservation、43 项 kernel selftest runner 单测及 7 项 user build 单测通过。
- `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`：内核启动标记与
  shell prompt 检查通过。该启动检查不替代上述宿主容量边界回归。

日志：`/tmp/os01-parked-procfs-capacity-{red,green,monitor,host,qemu}.log`。
