# monitor parked ⑧：快照锁范围可观测性（2026-10-24）

## 调查结论

`pmm_get_stats()` 和 `sched_accounting_read()` 的生产实现已经在正确的
irqsave 锁范围内汇总或复制计数，本次没有发现需要修改的内核错误。
缺口在宿主验证：PMM 的 spinlock shadow 完全不报告事件；accounting
已有 acquire 计数，但即使把解锁移到复制之前，原有断言仍可能通过。

本次在宿主 shadow 增加可选的弱观察钩子，并各增加一个链接真实生产对象
的测试。钩子、状态注入和断言均留在 `hosttests/`，不修改内核实现、
锁 ABI 或结构体。PMM 测试对象显式依赖所用 shadow 头，保证增量重编。

## 验证方式

PMM fixture 先通过真实 `alloc_pages()` 识别保护 zone 的锁，再验证
`pmm_get_stats()` 使用同一把锁、一次 acquire/release、无嵌套或失衡。
测试在三个时刻提供不同数据：

| 时刻 | zone 数 | managed / reserved / free（2 MiB 页数） |
|------|---------|----------------------------------------|
| 加锁前 | 1 | 3 / 1 / 2 |
| acquire 边界，模拟先前写者完成 | 2 | 5 / 3 / 2 |
| release 边界，模拟后续写者完成 | 1 | 3 / 0 / 3 |

真实函数必须返回中间状态的 `10485760 / 6291456 / 4194304` 字节。
该输入同时检查 zone 数与跨 zone 计数在锁内读取。NULL 输出另验证不取锁。
先写测试、保留原 PMM no-op shadow 时，新增场景的 8 条断言失败；
加上观察钩子后通过。

Accounting fixture 通过真实 tick writer 识别对应 CPU 的统计锁。
初始 busy/idle 为 3/2，acquire 边界通过真实 `sched_account_tick()`
增加 2/1，release 边界再增加 4/6。快照必须返回 5/3；随后普通读取必须
返回 9/9，epoch 不变，busy task runtime 为 9。注入期间关闭观察，避免
递归注入。这同时验证取锁身份、事件配对与计数复制范围，不能只靠 acquire
次数满足测试。

这些是确定性的边界交错模拟，shadow 本身仍不提供线程互斥，也不执行
真实 cli/sti。它们验证生产函数调用的锁与数据访问顺序，不替代 SMP
压力、硬件内存序或真实 IRQ 屏蔽验证。

## 错误注入验证

复制生产源到 `/tmp`，使用根 Make 门禁实际输出的编译参数与真实测试
对象，分别只替换目标快照函数。工作区生产源码和构建对象不变。

| 版本 | PMM 失败断言数 | Accounting 失败断言数 |
|------|----------------|-----------------------|
| 原始实现 | 0（59/59） | 0（138/138） |
| 删除 acquire/release | 5 | 8 |
| acquire 后立刻 release，再读计数 | 3 | 2 |
| 先读计数，再 acquire/release | 2 | 2 |
| 使用另一把锁 | 1 | 1 |

八个错误版本均退出 1；两个原始版本均退出 0。
复现脚本：`/tmp/os01-parked-snapshot-lock-mutations.py`。
各版本日志：`/tmp/os01-parked-{pmm,accounting}-lock-{original,missing_lock,early_unlock,late_lock,wrong_lock}.log`。

## 验证结果

- `make PROFILE=x86_64-clang test-monitor-host CASE=pmm`：6 个测试、59 条断言通过。
- `make PROFILE=x86_64-clang test-monitor-host CASE=accounting`：13 个测试、138 条断言通过。
- `make PROFILE=x86_64-clang test-monitor-host`：完整七组通过。
- `make PROFILE=x86_64-clang test-host`：退出 0，完整宿主二进制、PMM
  reservation 检查、kernel-selftest runner（43 项）及 user build（7 项）通过。
- `git diff --check`：通过。

完整 monitor 与 host 日志分别为
`/tmp/os01-parked-snapshot-lock-monitor.log` 和
`/tmp/os01-parked-snapshot-lock-host.log`。

本次无需运行 guest QEMU：内核源码未变，新增覆盖针对宿主锁边界模拟。
parked ⑥ 的严格绑核验证仍保留。
