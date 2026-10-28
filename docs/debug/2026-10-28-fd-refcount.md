# fd_refcount 间歇性 rc_-1 定位（2026-10-28）

## 结论与边界

此次复现的失败来自 **get-vs-detach fixture 的并发覆盖编排与判据**。没有观察到文件引用计数错误、分配失败或 UAF；不能据此推导整个引用协议不存在其他缺陷。

两个问题同时存在：

1. present/absent 的握手只保证读者看到两种 slot 状态，没有保证后面的两个 10000 次压力循环同时运行。双核阶段跟踪两次均直接观察到 writer 进入压力循环时 `reader_done=1`。
2. `cross_cpu` 要求两个线程整个运行期的累计 CPU mask 完全不相交。正常迁移会改变这一结果，且集合不相交本身也不能证明时间重叠。这不是可靠的跨核并发证据。

因此，不能把历史失败一概写成“已经发生真实并发，只是迁移造成误报”。本次失败样本确实缺少压力循环重叠；需要修正 fixture，而不是删除断言或把相交条件改成“不相等”。以上是定位阶段结论：临时诊断当时已撤销，测试与生产实现保持原样。后续用户授权修复后的实现和验收见下文。

## 返回值路径

原始版本 `kernel/selftest/test_fd_refcount.c`：

- 176–177 行：`(reader_mask & writer_mask) == 0`，并要求两个 mask 非零。
- 187–190 行：该条件失败打印 `no cross-CPU overlap`，返回 `-1`。
- 301–311 行：两个子场景任一个失败即使 `test_fd_refcount()` 返回 `-1`。
- `kernel/sched/core.c:633` 将返回值交给 `selftest_record_late()`。
- `kernel/selftest/result.c:179` 将非零返回值格式化为 `reason=rc_-1`。

`rc_-1` 是测试返回值，不是某个 fd 系统调用返回的 errno。本次各轮 pin-vs-detach 均 PASS，失败发生在 get-vs-detach。

## 为什么调度时序影响结果

reader 在 80 行发布 `r3_reader_detached_sampled`，立即进入无主动让出的读循环；writer 在 114 行通过 `wait_flag()` 等待该标志，恢复后才开始写循环。`wait_flag()` 反复调用 `schedule()`，并不保证标志发布后 writer 会立刻恢复执行。

两个 worker 都通过普通 `kernel_thread()` 创建，未绑定 CPU。`kernel/sched/balance.c:165–184` 允许迁移不在 CPU 上且目标 CPU 符合 `allowed_cpus` 的任务。writer 初始 CPU 与压力循环 CPU 可以不同。CPU mask 又在初始握手和压力循环里累计，所以初始 CPU1、后续 CPU0 会得到 `writer=3`；reader 只在 CPU0 采样得到 `reader=1`，交集为 1，必然失败。

此外，`schedule()` 的返回契约是保持 IRQ 关闭（`kernel/sched/core.c` 返回路径有明确注释），fixture 的等待助手不恢复 IRQ。若 reader 在握手等待中调用过它，后续紧循环还可能缺少 timer 抢占机会。这是代码层面可见的放大因素；本次没有采集 RFLAGS，不将具体样本的 IRQ 状态写成实测结论。

## 实测阶段证据

起始 revision：`2770d2ca50321c856070938d5649b9a17858b05a`，工作树干净。

原始复现命令：

```sh
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest
```

首跑为 39 PASS / 1 FAIL，唯一失败：

```text
FAIL (no cross-CPU overlap reader=1 writer=3)
[selftest] fd_refcount pin-vs-detach... PASS (cross-CPU reader=2 holder=0)
[TEST] FAIL fd_refcount reason=rc_-1
```

临时阶段诊断记录每个阶段的 CPU、TSC 与 acquire 读取的 `reader_done`，不修改原有通过条件。诊断代码已撤销，临时 patch 在提交前删除；下方阶段数据及归档日志保留调查证据。阶段编号：

| event | 位置 |
| --- | --- |
| 0 | reader 初次读取 present slot 前 |
| 1 | reader 压力循环开始 |
| 2 | reader 压力循环结束，发布 done 前 |
| 3 | writer 首次分配、发布 slot 前 |
| 4 | writer 最后一次握手等待结束，压力循环开始 |
| 5 | writer 压力循环内部检测到 CPU 改变；未触发时记录全零 |
| 6 | writer 压力循环结束 |

双核第一轮阶段跟踪（按阶段发生顺序整理）：

| event | TSC | CPU | reader_done |
| --- | ---: | ---: | ---: |
| 3：writer 首次发布前 | 6684553680 | 1 | 0 |
| 0：reader 首次读取前 | 6684701880 | 0 | 0 |
| 1：reader 压力开始 | 6699314790 | 0 | 0 |
| 2：reader 压力结束 | 6701111700 | 0 | 0 |
| 4：writer 压力开始 | 6701221320 | 0 | **1** |
| 6：writer 压力结束 | 6725813220 | 0 | 1 |

最关键的证据不是跨 CPU 的 TSC 比较，而是 event 4 acquire 读到 `reader_done=1`：reader 在结束循环并 unpin 后才 release 发布该标志，因此 writer 压力循环确定晚于 reader 完成。writer 从 event 3 的 CPU1 到 event 4 的 CPU0 也直接证实了迁移；event 5 没触发是因为迁移发生在压力循环之前。

第二次双核跟踪得到相同时序：reader 压力结束 TSC=7157814510，writer 压力开始 TSC=7157935860、CPU0、`reader_done=1`，最终仍为 `reader=1 writer=3`。

另一次独立的临时操作窗口诊断在读操作与 close 周围关闭本地 IRQ、发布 active CPU 标记，观察到 `live_overlap reader=0 writer=0`、`present=1 absent=1 writer_err=0`。零计数本身不能证明完全没有窗口重叠；以上阶段跟踪提供了更直接的循环完成顺序证据。

## 完整运行记录

归档根目录为 `build/x86_64-clang/logs/tests/kernel-selftest/`，各子目录均有 `stdout.log` 与 `result.json`。所有失败轮次均为 get-vs-detach 的 `reader=1 writer=3`，完整套件 39/40；通过轮次为 40/40。

| run_id | SMP | 源码状态 | 结果 |
| --- | ---: | --- | --- |
| `20261028T151016Z-9d2192080bc144ad809598e05aa49687` | 2 | 干净基线 | FAIL |
| `20261028T151104Z-b417d0330cde42cb923dc1b5f7c52915` | 2 | 临时 active 窗口诊断 | FAIL |
| `20261028T151201Z-36448d328f484ac881255bd6d7606958` | 2 | 临时阶段跟踪 | FAIL |
| `20261028T151231Z-7a67e37c0ac44d87a492d4d018f10e05` | 2 | 临时阶段跟踪 | FAIL |
| `20261028T151235Z-c6d178b6f0c84489bab47b2a47c1f97b` | 4 | 临时阶段跟踪 | PASS |
| `20261028T151239Z-e4ededb3a22c49198ccf9709ebc86959` | 4 | 临时阶段跟踪 | PASS |
| `20261028T151242Z-ea06870a1c8f4b26aa255e7d11200cf4` | 8 | 临时阶段跟踪 | PASS |
| `20261028T151325Z-3443d72e57004c1ca10f80a9e39e6ed9` | 4 | 已撤销诊断并重建 | FAIL |

阶段跟踪的两次四核、一次八核记录中，writer 压力开始时 reader 尚未完成，循环区间在不同 CPU 上有时间重叠。诊断会改变时序，这些 PASS 不能当作原始配置的稳定性证明。撤销诊断后四核再次失败，也说明本次没有修复问题。

## 定位时提出的修复约束

使用现有 `kernel_thread_on_cpu()` 让 worker 在两个不同的在线、scheduler-ready CPU 上运行；增加有界的阶段握手，使实际 get/close 压力窗口有可验证的跨核重叠，并处理创建失败、超时、引用释放与 idle harness 的等待约束。单 CPU 应明确处理不具备跨核测试条件的情况。不能只绑定 CPU 就声称时间重叠，也不能仅靠放宽 mask 断言获得 PASS。

pin-vs-detach 的累计 reader mask 与 holder CPU 排斥检查（原始 280–281 行）存在相同判据缺口，但本次未复现该场景失败，不能据此声称它就是历史所有失败的原因。


## Fixture 修复

用户随后授权修复。只修改 `kernel/selftest/test_fd_refcount.c`，保留原始失败和上述阶段证据，不修改文件引用协议、调度器或 ABI。

- 选择 BSP reader CPU 和另一颗 online、scheduler-ready CPU；两个场景均通过 `kernel_thread_on_cpu()` 在发布 worker 前绑定。采样 mask 必须等于各自指定的单 CPU mask。单 CPU 明确 FAIL：`requires two scheduler-ready CPUs`，不声称具备跨核覆盖。
- get-vs-detach 初始 reader 持有 `files_get_file()` 返回的引用，直到远端 writer 关闭 slot。此后必须观察 slot 为空且 held 文件的引用数为 1，再 put；不是仅在不同时间观察 present/absent。
- reader 保持压力循环直到 writer 完成。writer 仍执行 10000 次分配/发布/关闭；每 16 次 close 后都要求 reader acquire 读取该批进度、执行一次真实 get/put，再 release 确认。最后一批也必须确认，PASS 必须具有完整 writer/reader 进度 10000，共 625 批。
- pin-vs-detach reader 持有首次成功取得的表引用，释放 holder 去执行 `do_exit()`，直到后续查找返回 NULL 后才检查 held 表仍有效并 unpin。两个 CPU 的归属分别核对。
- worker 等待为有界、可取消的跨 CPU 轮询，并开启 IRQ 让 timer deadline 生效；idle harness 单独通过 `schedule()` 等待完成，返回后重新开启 IRQ。整个自测结束时恢复入口 IRQ 状态。
- 在创建 R3 worker **之前**预留表引用，创建失败退回预留；worker timeout 或 abort 都在发布 done 前归还引用。部分创建失败通知已有 peer 退出；只有全部 worker 完成才释放 harness 引用，清理超时保留该引用并 FAIL。

这些证据保证跨核引用生命周期重叠和读操作贯穿每批写操作；不宣称每次 get 都命中非空 slot，或能证明 get/close 在锁临界区内同时执行。625 批内部的具体竞争交错仍是压力覆盖，初始 held-reference 阶段提供确定的生命周期验证。

独立源码评审检查了引用预留/释放、acquire/release 同步、idle 等待与 IRQ 契约，未发现阻断问题；首次评审指出的“只确认第一批”覆盖不足已加强为每 16 次确认，复审通过。


## 修复验收（x86_64-clang）

修复前重新执行 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest`，仍为 get-vs-detach `reader=1 writer=3`、39/40 FAIL（命令输出 `/tmp/fd-refcount-red.log`）。修复后同一真实自测完成 RED→GREEN；没有通过删除或跳过必选 case 获得通过。

最终源码撤销全部故障注入并重建 selftest 镜像后，使用根 Make 目标执行首轮 SMP2；后续通过相同 `qemutests/run_kernel_selftest.py`、相同 firmware/image、512 MiB、75 秒预算，串行执行其余重复轮次。三种 SMP 配置各三轮，均为完整套件 40/40，无 SKIP；每轮确认 `held=1 overlap=1 closes=10000 batches=625` 和 pin-vs-detach 的 `held=1`。

| SMP | run_id | 完整套件 |
| --- | --- | --- |
| 2 | `20261028T152553Z-cef1feeeef48463d9448de3b59c50f67` | 40/40 PASS |
| 2 | `20261028T152557Z-41d39670b221442f8b8efaab159844c7` | 40/40 PASS |
| 2 | `20261028T152601Z-938f6f7cdaaf4306b53228d01b8312f1` | 40/40 PASS |
| 4 | `20261028T152604Z-676c6cd98e65415e88b089e575fed22f` | 40/40 PASS |
| 4 | `20261028T152608Z-83dc0723f58f4922a45ef6405804eae8` | 40/40 PASS |
| 4 | `20261028T152612Z-414c0d5096a7499baf117d9af6c065e3` | 40/40 PASS |
| 8 | `20261028T152616Z-04b5d96bea574621ab658b499a6b6d42` | 40/40 PASS |
| 8 | `20261028T152620Z-e50e3dd39a6e42578a50802a042e0d17` | 40/40 PASS |
| 8 | `20261028T152624Z-8ff103518c764df48e588ac7b6affb63` | 40/40 PASS |

故障注入在真实 QEMU 中逐项执行，每轮仅修改 fixture 一处，结束后恢复源码。四项创建错误用 `-1` 替换对应的 `kernel_thread_on_cpu()` 返回赋值；两项超时用例删除对应的 release 确认。所有六轮都只有 `fd_refcount` FAIL，39/40，且出现 `[selftest] task tests done`，证明错误不会留下无限等待阻断后续套件：

| 注入 | run_id | 关键结果 |
| --- | --- | --- |
| r3-reader-create | `20261028T152355Z-002bc0f102224a18b5f91d2ddf26041e` | 创建 reader 失败，writer 清理退出 |
| r3-writer-create | `20261028T152405Z-7f17ce7e396648f0b9bb0d583d3d52b4` | 创建 writer 失败，reader 清理退出 |
| r2-holder-create | `20261028T152415Z-ac5482de8f554ebfbdaf9a4d2a9f0ccd` | 创建 holder 失败，直接返回 |
| r2-reader-create | `20261028T152424Z-3dcb74a6791a445fa258dfa5b7e5c35b` | 创建 reader 失败，holder 退出 |
| r3-no-progress-ack | `20261028T152434Z-22178a9ebc0245fe9738875e8cdbdd3a` | 第一批 16 次 close 后 abort=1，不会误报 PASS |
| r2-no-pin-ack | `20261028T152448Z-45908dc5d6a341558d118952fa7fdbae` | holder 等待超时，abort=1，后续套件完成 |

单 CPU 负例：`20261028T152635Z-dd2dbbf517d542918d1bae17cd1cc9a2` 明确打印 `requires two scheduler-ready CPUs`。完整套件为 38/40，另一个失败是现有 `x86_intr` 的 `no online/ipi_ready target cpu`，未将该轮描述为全套仅 fd_refcount 失败。

附加检查：`make PROFILE=x86_64-clang test-host` 退出 0；`python3 -m unittest qemutests.test_kernel_selftest_result` 的 60 项通过。最终 fixture 编译无新增 warning。

执行过程曾错误地并行运行同 profile 的 static 与 QEMU 构建，遇到 sysroot 的临时 `poll.h` 缺失；相关 static 首跑 FAIL，早期故障注入脚本还可能读取上轮 archive，因此该批结果全部不计入以上验收。已改为串行构建，故障注入重跑逐轮要求生成一个全新 archive，并验证完整套件结束；上表仅列有效重跑证据。

串行重跑 `make PROFILE=x86_64-clang test-static` 退出 0（`/tmp/fd-refcount-test-static-serial.log`），修复前失败与作废结果均保留，不将重跑成功覆盖为“首跑通过”。独立评审复查最终的逐批进度握手后无阻断问题。修复涉及的活动源码没有结构体布局变更。
