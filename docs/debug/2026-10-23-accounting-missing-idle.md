# 缺少 idle 身份时的 CPU tick 误记账

2026-10-23，基于 `3ddb277` 处理 roadmap monitor parked #1。

## 可达性与根因

核实当前 x86 启动顺序：BSP 在 `task_init()` 中先把
`percpu_data[0].idle` 指向 init idle，随后才发布 `scheduler_ok=1`；AP
由 BSP 在启动前分配并设置 idle，`ap_entry()` 之后才发布 scheduler_ok。
因此，本项是正常启动顺序已保证的防御性边界，不声称观测到线上假 100%
CPU，也不把既有正常 QEMU 启动归因于此问题。

`kernel/sched/accounting.c::sched_account_tick()` 原先只检查 accounting
ready、CPU 范围、scheduler_ok 和 tick source valid。分类逻辑为
`idle && cur == idle` 才算 idle，其余全算 busy；runtime 则在
`cur && cur != idle` 时增加。idle 指针为空时，无论 current 是谁都落入
busy 分支，连尚未发布其身份的真正 idle task 也会增加任务 runtime。

根因是“无法确定 idle 身份”被当成了“确定为非 idle”。未来若修改启动
顺序或增加阶段性 idle 缺失状态，这个失败方向会制造 CPU 与任务负载。

## 修改

读出本 CPU 的 idle 后，若为空立即返回，不获取 usage 锁、不改 CPU
计数、不改任务 runtime。已存在的基线、epoch、tick rate 和 source valid
保持不变；恢复 idle 后继续正常采样，不凭空重启计时器生命周期。

已确认 idle 非空，后续指针分类可直接写 `cur == idle`。公开热路径注释
及 monitor spec §2.2 同步明确缺失 idle 时丢弃样本。没有改变结构布局、
ABI、在线状态判定或 `sched_accounting_read()` 的 valid 语义，也未增加
interrupt 热路径日志或分配。

## 红绿回归

沿用 hosttests 的真实 `accounting.c` 编译 seam，测试只控制 cpu_id、
percpu idle/scheduler 状态和 current：

1. 先产生 3 busy + 2 idle，获取已有非零基线。
2. 设置 idle=NULL，分别让 busy task、真正 idle task 和 NULL current
   各经历 10 个 tick。要求原计数、两个任务 runtime、epoch、tick rate
   保持不变；热路径不获取 usage 锁。
3. 重新发布同一个 idle 指针，产生 1 busy + 1 idle；要求 counters 各增 1、
   busy runtime 增 1、idle runtime 不变，epoch 与 rate 连续。
4. 全程确认分配/释放计数不变。测试复用 CPU 0，但以操作前后差分断言，
   不依赖静态 usage 数组能够被重置。

旧实现 84 条断言中 7 条失败：缺失 idle 的窗口把 busy 增加 30、busy
任务 runtime 从 3 增至 13、真实 idle runtime 从 0 增至 10，且恢复后的
计数也带着这些误记增量。修复后 10 个注册用例、84 条断言全部通过。

独立评审未发现待修正项；以 `-O1` + AddressSanitizer/UndefinedBehaviorSanitizer
编译真实生产函数及 fixture，84 条断言通过，无 sanitizer 报告。完整七组
monitor host 测试也通过。

## 验证

```sh
make PROFILE=x86_64-clang test-monitor-host CASE=accounting
make PROFILE=x86_64-clang test-monitor-host
make PROFILE=x86_64-clang test-qemu SUITE=monitor
make PROFILE=x86_64-clang test-host
```

完整 `test-host` 返回 0（全部 host binaries、PMM boot reservation、43 项
selftest runner 测试和 7 项 user build 测试通过）。官方 QEMU monitor
矩阵中 1 核与 8 核 PASS，2 核因下述串口帧标题交错而等待超时，因此整条
QEMU 命令返回非零，不能宣称完整 QEMU 矩阵通过。缺失 idle 的条件由 host
控制真实生产函数直接覆盖，不在实际启动代码中加入制造此状态的开关。

运行环境备注：沙箱内 QEMU `-snapshot` 两次在进入 UEFI 前失败，原因是
无法向只读 `/var/tmp` 创建临时文件；设置 TMPDIR 也未改变其访问位置。
之后通过测试所需权限运行相同官方入口，未修改 QEMU 参数或内核代码来
绕过该环境问题。这两次启动失败不计作 guest 记账回归结果。


## 回归中发现的独立 runner 问题

权限正常的官方 monitor 运行归档于
`build/x86_64-clang/logs/tests/monitor/20261023T134622Z-14e632cbdc92425f8f6ea4e091294489/`。
2 核 stdout 中首帧标题实际为：

```text
OS01 mo[term-present] version=1 begin_ns=... rect_calls=6
nitor
CPU total: --
```

后续七帧正常输出，但 `parse_frames()` 只识别完整行 `OS01 monitor`，所以
runner 等不到要求的 8 帧。离线对同一 transcript 删除 `[term-present]` /
`[term-probe]` 诊断文本后，识别帧数从 7 恢复为 8；这只是根因分析，未
修改正式 runner、未改变帧数门槛，也未把离线修复结果计为 QEMU PASS。
因为等待还没有完成，末尾 shell-alive 命令尚未发送，其缺失不能证明
shell 死亡。此独立问题另列为 monitor parked #14。
