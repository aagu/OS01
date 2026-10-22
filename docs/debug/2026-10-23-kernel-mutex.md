# kernel_mutex parked 调查与修复

2026-10-23。基于 `13000cb` 的独立工作树，避免与并行 GUI 合并混用构建。

## 确认的问题

原 parked 记录称宿主高负载时 `kernel_mutex` 自测的 10M 次 `schedule()`
预算耗尽；旧现场没有 owner、waitqueue 或线程状态采样，无法证明当时
究竟是进展慢还是永久睡眠。本次不将历史事件排他归因于某一个缺陷。

源码调查发现生产 `mutex_lock()` 和 `mutex_lock_interruptible()` 都存在：

1. 等待者 CAS 失败。
2. 持有者清空 owner 并 wake-one；此时队列为空。
3. 等待者才通过 `wait_queue_sleep()` 入队，置 INTERRUPTIBLE 并调度。

此时 owner 已为 0，没有未来 unlock，因此等待者可能永久睡眠。
增加自测自旋次数不能修复这个协议窗口。

Host 回归链接真实 `kernel/sync/mutex.c` 和 `kernel/sync/wait.c`，仅替换
宿主无法运行的 scheduler/IRQ/atomic substrate。在 sleep/arm 边界插入
真实 `mutex_unlock()`，确定性构造上述时序。旧实现两个获取接口均出现
owner=0 + TASK_INTERRUPTIBLE；测试在无未来唤醒者的 schedule 处截获失败，
无需靠宿主随机调度或超时猜测。

此外，旧自测有两个独立的观测问题：

- `create_kthread()` 会在发布后按 PID 查找任务；工作线程若已自回收，
  查找会返回 NULL，虽然创建本身成功。自测不需要任务指针。
- `shared_counter == 2000` 出现在最后一次 unlock 之前，不能据此断言两个
  worker 已完成，也不能作为后续栈回收自测的同步边界。

## 修复

Mutex 保留首次 CAS 快路径；失败后关闭本地 IRQ，arm 等待队列，然后再次
CAS，再决定是否调度。关闭 IRQ 防止任务在 arm 与重查之间被本地抢占并
真正睡眠。wake-before-arm 被二次 CAS 捕获，wake-after-arm 则将状态置为
RUNNING，后续 schedule 不会重新覆盖状态。

持 waitqueue 锁执行 disarm，避免与另一 CPU wake-one 删除同一个节点。
interruptible 取消时额外 wake-one，避免取消者消费了 unlock 的唯一通知后
留下另一位等待者睡在空闲锁上。所有返回路径恢复调用者 IRQ 状态。

内核自测使用 `kernel_thread()` 的 PID 返回值；两个 worker 在最后一次
unlock 后用 release RMW 增加完成数，观察方 acquire 读到 2 后才读取计数。
等待按 500 jiffies 截止，并在 schedule 后启用 IRQ 以推进计时。worker 在
临界区每 32 次主动调度，覆盖同 CPU 与多 CPU 的锁竞争，而非只测快路径。
超时打印 done、owner 和经过的 jiffies，不与运行中的 worker 竞争读 counter。

## 回归入口

```sh
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
```

新增 `test_mutex_handshake` 纳入常规 host runner，不依赖手工编译。
测试用例覆盖普通/interruptible 入队前 unlock、入队后 wake、竞争后获得锁、
arm 后信号、已有 pending signal、取消时转交通知和初始 IRQ 关闭的恢复。

验证结果：

- 最终确定性 host 回归：8 cases / 38 assertions 全通过。两个入队前 unlock
  case 在旧实现上已观测到失败；取消后下一位等待者的回归也经历单独红绿验证。
- `make PROFILE=x86_64-clang test-host` 完整通过（含新增 mutex 回归、43 项
  kernel runner 单测、PMM 预留与 user build 单测）。
- `make PROFILE=x86_64-clang test-static` 完整通过。
- QEMU 1/2/4/8 vCPU 各一次，以及 4 vCPU 限制在同一个宿主 CPU 上运行三次：
  `kernel_mutex` **7/7 PASS**，计数均为 2000；`tty_vintr` 也 **7/7 PASS**，
  每次均输出 END。该矩阵完整套件仅 **1/7 PASS**（8 CPU）；其余 6 次唯一
  失败均为已知 `fd_refcount rc_-1`。不能将矩阵描述为完整全绿。
- 单宿主 CPU 压力测试通过临时 QEMU 包装脚本在 exec 前调用
  `os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})`，使 QEMU 线程继承
  单 CPU 限制；没有修改 runner 的 PASS/FAIL 判定或放宽 75 秒截止。
- 独立代码评审完成，无待修正确认项。评审建议的 schedule 时 IRQ 必须关闭、
  初始关闭 IRQ 的恢复检查已加入 host 回归。

矩阵本地证据在 `build/x86_64-clang/logs/tests/kernel-selftest/`，范围为
`20261022T160423Z-6a06fed4c59d423b9097cc20ab93db38` 至
`20261022T160451Z-cff6eec1800a421dac46342a36cc26e4`（目录名为 UTC；本地已是 10 月 23 日）。

本次关闭此 parked 自测加固项；历史宿主负载失败不作为已证明的单一根因
结论。若再次出现，新的完成数/owner/elapsed 诊断用于继续区分停滞与预算
耗尽。syscall systest 未与内核自测合跑。
