# x86_64 init PID 1:延迟启动 lwIP tcpip 内核线程

- 日期:2026-10-31
- 状态:v3 已通过独立子 agent 评审(APPROVE);不代表实现已完成
- 代码基线:`058a360b`(工作区另有 monitor 相关未提交修改,与本设计无关)
- 范围:设计文档。改动集中、无结构体变更,评估可直接据此实施,不另写实施计划

## 1. 目标与动机

让用户态 `/bin/init` 在**所有构建变体**下都是 PID 1,方法是把 lwIP 网络栈的启动
(即 `tcpip_init()` 创建 tcpip 内核线程)从 `kernel_main` 迁移到 `task_init()` 内
`spawn_user_task("/bin/init")` 之后。引导创建窗口(boot creation window)的开闭
点不动,只重排窗口内服务线程与 init 的先后。

现状是三重缺陷:

1. **PID 语义错误**:tcpip_thread 先消费 PID 1,init 是 PID 2
   (`kernel/arch/x86_64/cpu/sched_boot.c:100` 注释明确承认)。
2. **优雅关机路径死亡**:`user/poweroff.c:14`、`user/halt.c:14`、
   `user/reboot.c:14` 均硬编码 `kill(1, ...)`,而 PID 1 是 PF_KTHREAD,
   `task_send_signal()` 对内核线程返回 `-EPERM`
   (`kernel/sched/signal.c:27`),init 的 `shutdown_handler` 永远不触发,
   poweroff 只能走"等超时后被一起杀掉"的兜底。
3. **KERNEL_SELFTEST 变体同样错误**:`sched_boot_probe_create()`
   (`kernel/core/main.c:212`)在窗口内创建的探针线程在正常构建语义之外
   又先消费一个 PID,该变体下 init 是 PID 3。

设计参照 Linux 的"先定身份,再干活"模型:`rest_init()` 先创建 kernel_init
(PID 1)再创建 kthreadd(PID 2),驱动与网络栈初始化全部发生在 PID 1 还是
内核线程的阶段,之后同一任务 `execve("/sbin/init")`,任务身份不变;一切
后续内核线程由 kthreadd 代生。OS01 采纳其**顺序内核**(init 先拿 PID,
引导服务线程殿后),不采纳 kthreadd 孵化器(见 §3.2)。

## 2. 当前代码事实

| 代码位置 | 现状 | 本期影响 |
| --- | --- | --- |
| `kernel/core/main.c:220` | `net_lwip_init()` 在 `task_init()`(229 行)之前调用 | tcpip_thread 成为第一个 `alloc_pid()` 消费者,拿走 PID 1 |
| `kernel/sched/core.c:159` | `pid_counter = 1`,`alloc_pid()` 原子取旧值递增 | 先创建先得 1;无预留机制 |
| `kernel/net/sys_arch.c:476` | `sys_thread_new()` → `create_kthread()` | tcpip_thread 走 kthread 路径分配 PID |
| `kernel/sched/kthread.c:148` | `kthread_alloc_and_publish()` 内 `alloc_pid()` | 同上 |
| `kernel/sched/exec.c:399,557` | `spawn_user_task()` 内 `alloc_pid()`;首次调用设置 `user_init_task` / `user_init_pid` | init 的 PID 取决于调用时序;reparent 靠指针与 PID 解耦 |
| `kernel/arch/x86_64/smp/smp.c:160,184` | AP idle 任务显式 `tsk->pid = 0`;AP 在 `ap_entry()` 内置 `online = 1` 并立即发布 `cpu->scheduler_ok = 1` | AP idle 不消耗 `pid_counter`;SMP 下 `x86_64_boot_aps()` 返回后 AP 已 `scheduler_ok == 1`,仅 BSP(CPU0)`scheduler_ok == 0` |
| `kernel/core/main.c:199` / `kernel/sched/boot.c:79` | 窗口由 `sched_boot_creation_begin()` 打开,由 `task_init` 内 `sched_boot_start()` 永久关闭(先置 `boot_creation_state = 2`,再发布 `percpu_data[0].scheduler_ok = 1`) | `spawn_user_task`(106 行)与窗口关闭(115 行)之间窗口仍开、BSP `scheduler_ok == 0`,是合法插入点 |
| `kernel/sched/kthread.c:57-69,94-104` | UP 下无核 `scheduler_ok`,窗口打开时 kthread 放置回退 CPU0;SMP 下 AP 已 `scheduler_ok == 1`,由 `sched_pick_cpu_mask()` 直接选 AP | 迁移前后 UP/SMP 两条放置路径均保持不变(见 2026-10-30 aarch64 调度 spec §8.1) |
| `kernel/net/lwip.c:195-255` | `net_lwip_start()`:先 `tcpip_init()` 建线程,同一调用者再跑 `netif_add` + `dhcp_start` 循环,末尾置 `s_adapters_finished = true` 并与 `tcpip_init_done_cb` 的 `s_tcpip_core_ready` 汇合发布 `NET_ONLINE` | UP 下依赖 BSP `scheduler_ok == 0`(timer tick 不触发 `schedule()`,循环与 `tcpip_thread` 天然串行);SMP 下迁移前后同处 post-AP、pre-BSP-scheduler 区间,并发事实不变 |
| `kernel/ipc/mbox.c`(头注释) | `sys_mbox_wake()` 在 `tcpip_init()` 创建 core mailbox 前是 no-op | NIC 早到的 RX 不依赖栈已启动 |
| `kernel/selftest/test_sched_boot_probe.c:31` | 探针线程在 `kernel_main` 窗口内创建 | selftest 变体下先于 init 消费 PID |
| `user/nettest.c:30-36` | `wait_for_dhcp()` 轮询 `SYS_getifaddr` 自适应等待 | 网络栈启动先后不产生固定 sleep 依赖 |
| `config/inittab.systest` | systest 是 init 按 `once` 动作 fork 的**子进程** | getpid 断言不能放 systest;回归防护放 E2E/selftest(§5) |
| `kernel/arch/aarch64/` | 不引用 `net_lwip_init`(已核实) | 本设计纯属 x86_64,无 aarch64 影响 |

## 3. 范围与方案选择

| 方案 | 结论 | 理由 |
| --- | --- | --- |
| A. 调用点迁移:`net_lwip_init()` 移入 `task_init`,`spawn_user_task` 之后、`sched_boot_start()` 之前 | **采用** | 集中改动;迁移前后 `net_lwip_start()` 全程处于"引导窗口打开且 BSP `scheduler_ok == 0`"区间,UP 下保持串行回退 CPU0,SMP 下保持原有 post-AP 放置语义 |
| B. `sched_boot_start()`(`scheduler_ok = 1`)之后由专门 net-start kthread 或直接在 BSP 启动(仿 thermal worker,`sched_boot.c:126`) | 拒绝 | 一旦关闭引导窗口并置 `percpu_data[0].scheduler_ok = 1`:(1)破坏 UP 下引导窗口覆盖 `net_lwip_init()` 的既定契约;(2)UP 下 BSP 在 `netif_add`/`dhcp_start` 循环期间可被 timer tick 抢占切入刚创建的 `tcpip_thread`,凭空引入原本在 UP 下不存在的重入竞争(lwIP 未开 core locking) |
| C. 惰性启动:首次网络 syscall 时 `net_lwip_start()` | 拒绝 | 需要 syscall 上下文的一次性初始化 + 跨 CPU 并发防抖 + 失败语义;唯一收益(无网卡不建线程)已被 `net_lwip_start()` 开头 `count == 0` 分支覆盖(`lwip.c:182`) |
| D. kthreadd 式孵化器 | 后续备选 | 收益在内核服务线程成规模后;当前窗口机制已承担"未就绪创建"职责 |

### 3.1 纳入

- `net_lwip_init()` 调用点迁移(§4.2)。
- `sched_boot_probe_create()` 同步迁移至同一区间:所有构建变体下 init 都是 PID 1。
- 注释与活文档同步:`kernel/core/main.c`、`kernel/arch/x86_64/cpu/sched_boot.c`、
  `kernel/sched/kthread.c`、`kernel/sched/boot.c`、`kernel/net/net.c`、
  `kernel/selftest/test_sched_boot_probe.c`、`kernel/selftest/selftest.c`、
  `kernel/include/selftest/sched_boot_probe.h`、`kernel/include/net/net.h`、
  `docs/architecture.md`、`docs/subsys/subsys.md`、`qemutests/test_entropy_facade.sh`。
- 回归防护:phase-0 E2E(`qemutests/run_test.py`)断言串口出现
  `init: running as PID 1`(`user/init.c:499` 已无条件打印)并同步更新
  `qemutests/test_run_test_harness.py` 桩数据;KERNEL_SELFTEST 增加 `init_pid`
  late case,断言 `user_init_task != NULL && user_init_task->pid == 1 && user_init_pid == 1`,
  并同步更新 `kernel/selftest/selftest.c` 与 `qemutests/test_kernel_selftest_result.py`。
- `docs/changelog.md` 记录。

### 3.2 不纳入

- kthreadd 式内核线程孵化器。
- lwIP core locking / netif API 重入安全改造(方案 B 的前置,本期不做)。
- `pid_counter` 预留或重排机制(语义不变)。
- `user/init.c` 的 PID 硬校验失败路径(维持只打印;防护由 E2E/selftest 承担)。
- 历史文档改写:2026-07-18 lwip spec/plan 等保持原样。

## 4. 设计

### 4.1 任务创建顺序规范

以下为规范条文,评审通过后对后续改动有约束力:

- **R1** x86_64 BSP 上,`task_init()` 内的 `spawn_user_task("/bin/init", NULL)` 必须是
  第一个 `alloc_pid()` 消费者。它之前禁止任何 `kernel_thread` /
  `kernel_thread_on_cpu` / `create_kthread` / `do_fork` / `spawn_user_task` 调用;
  引导窗口内的服务线程与自测探针线程一律排到该 `spawn_user_task` 之后。
- **R2** `net_lwip_init()` 必须在 `sched_boot_start()` 之前执行,即引导创建窗口仍处于
  打开状态(`sched_boot_creation_open() == true`)且 BSP(CPU0)尚未发布
  `scheduler_ok` 之前。这保证了:(a)UP 构建下 `create_kthread` 合法命中 CPU0 引导回退,
  且 `net_lwip_start()` 的 `netif_add`/`dhcp_start` 循环在 BSP 开启调度前跑完,
  不会被 timer tick 抢占切入 `tcpip_thread`;(b)SMP 构建下保持与迁移前相同的
  post-AP、pre-BSP-scheduler 窗口语义。禁止移到 `sched_boot_start()` 之后,
  除非先为 lwIP 启用 core locking 并完成 netif API 重入审计。
- **R3** 引导窗口开闭点不动:`kernel_main` 的 `sched_boot_creation_begin()` 与
  `task_init` 内的 `sched_boot_start()` 位置保持;本设计只重排窗口内创建的服务
  线程相对 `spawn_user_task` 的先后。窗口语义(见 2026-10-30 aarch64 调度 spec §8.1)不变。
- **R4** `user_init_task` / `user_init_pid` 记录机制保持不变;孤儿 reparent 继续与 PID 解耦。

### 4.2 具体改动

1. `kernel/core/main.c`:
   - 移除顶部 `#include <net/net.h>`(若 `main.c` 不再引用其他符号)。
   - `183` 行:`selftest_begin_run(7)` 改为 `selftest_begin_run(8)`(配合第 6 条新增 `init_pid` late case),同步更新 `178-182` 行注释中的 "seven scheduled cases" 为 "eight scheduled cases"。
   - `188-199` 行:标题注释与窗口注释改写(保留 `sched_boot_creation_begin()` 打开窗口的语义说明,将 `net_lwip_init()` 与 `sched_boot_probe_create()` 的调用点说明指向 `task_init()` in `sched_boot.c`)。
   - `201-220` 行:删除 `#ifdef OS01_SELFTEST` 下的 `sched_boot_probe_create()` 块及 `net_lwip_init()` 调用,迁入 `sched_boot.c`。
2. `kernel/arch/x86_64/cpu/sched_boot.c`:
   - 顶部增加 `#include <net/net.h>`(及 `#ifdef OS01_SELFTEST` 下的 `#include <selftest/sched_boot_probe.h>` 或局部 `extern`)。
   - 在 `spawn_user_task("/bin/init", NULL)`(`106` 行)之后、`current->state = TASK_RUNNING`(`112` 行)与 `sched_boot_start()`(`115` 行)之前的区间内,插入 `#ifdef OS01_SELFTEST` 的 `sched_boot_probe_create()` 与 `net_lwip_init()`;两者相对顺序不作规定(建议先 `sched_boot_probe_create()` 再 `net_lwip_init()`,或反之)。
   - 更新文件头步骤注释(`9-23` 行,在步骤 5 与 6 之间反映引导窗口内 `sched_boot_probe_create` / `net_lwip_init` 的调用)、`65-68` 行注释(去掉"`tcpip_thread from net_lwip_init()` 在 `task_init()` 前加入链表"的过时举例,仅保留 `AP idle tasks from smp_boot_aps()`)、`100-104` 行注释(改写为"`pid_counter` 从 1 开始,`spawn_user_task` 作为首个 `alloc_pid()` 消费者使 user init 恒为 PID 1;`net_lwip_init()` 随后在同一引导窗口内创建 `tcpip_thread`")以及 `156-163` 行 `test_sched_boot_probe` 注释("created in `kernel_main`" 改为 "created in `task_init` after `spawn_user_task` and before `sched_boot_start`")。
3. 其余注释与活文档同步:
   - `kernel/sched/kthread.c:47-56`、`kernel/sched/boot.c:52-57`:校验关于 `net_lwip_init()` 创建 `tcpip_thread` 的注释(`boot.c:55-57` 中 `sched_boot_publish_idle()` 运行时 `tcpip_thread` 尚未入队,更新表述为不重置既有链表/运行队列以保护 `smp_boot_aps()` 已挂入的 AP 任务及引导期队列不变)。
   - `kernel/selftest/test_sched_boot_probe.c:4-8` 与 `kernel/include/selftest/sched_boot_probe.h:9-15`:将 "runs from `kernel_main` inside the legal pre-publication window" 更新为在 `task_init()` 内(`spawn_user_task` 之后、`sched_boot_start()` 之前)的同一合法预发布窗口内运行;其被测不变量(BSP `scheduler_ok == 0`、窗口打开)与 `test_sched_boot_probe()` 断言完全不变。
   - `kernel/include/net/net.h:5` 与 `kernel/net/net.c:18`:`net.h:5` 注释 `// Stage B: Post-SMP, pre-task_init` 更新为 `// Stage B: Post-SMP, inside task_init() after user init spawn and before sched_boot_start()`;`net.c:18` 注释中的调用点 `(kernel/core/main.c)` 更新为 `(kernel/arch/x86_64/cpu/sched_boot.c)`。
   - `docs/architecture.md:170-176` 与 `docs/subsys/subsys.md:196-206`:更新启动序列说明与伪代码,体现 `net_lwip_init()` 位于 `task_init()` 内 `spawn_user_task` 之后、`sched_boot_start()` 之前。
   - `qemutests/test_entropy_facade.sh:25`:注释中 `init (PID 2)` 更正为 `init (PID 1)`。
4. `qemutests/run_test.py`(phase-0)与 `qemutests/test_run_test_harness.py`:
   - `user/init.c:493-499` 先打印 `OS01 Init v1.0 (PID 1)`,随后调用 `SYS_getpid` 打印 `init: running as PID 1`。若 `test_boot()` 仅 `read_until("OS01 Init v1.0")` 就立即匹配正则,下一行 `init: running as PID 1` 可能尚未从串口收齐。因此将 `qemutests/run_test.py:533` 的 `read_until("OS01 Init v1.0", timeout=25)` 推进为 `read_until("init: running as PID 1", timeout=25)`,并在 `548-560` 行的 `markers` 有序正则末尾追加 `r".*init: running as PID 1"`。
   - 同步更新 `qemutests/test_run_test_harness.py` 中 `Phase0SuiteTests` 的 `CHUNKS_POSITIVE`(`444-451` 行)与 `CHUNKS_LATE_PANIC`(`469-478` 行),在 `b"OS01 Init v1.0\n"` 后加入 `b"init: running as PID 1\n"`,确保宿主 harness 单测通过。
5. `kernel/selftest/` 与 `qemutests/test_kernel_selftest_result.py`:
   - 新增 late case `init_pid`:检查 `user_init_task != NULL && user_init_task->pid == 1 && user_init_pid == 1`(定义可放在 `kernel/selftest/test_sched_boot_probe.c`,需 `#include <sched/internal.h>` 以引用 `user_init_task` / `user_init_pid`,并通过 `extern int test_init_pid(void);` 暴露)。
   - `kernel/selftest/selftest.c:225-243`:在 `#if !defined(__aarch64__)` 的 late case 注册段增加 `selftest_register_late("init_pid");`,顺带将 `225-228` 行上方注释中的 `kernel/sched/core.c` 更新为 `kernel/arch/x86_64/cpu/sched_boot.c`。
   - `kernel/arch/x86_64/cpu/sched_boot.c`:在 late selftest 段调用 `selftest_record_late("init_pid", test_init_pid());`。
   - `qemutests/test_kernel_selftest_result.py:807-814`:`test_end_run_follows_every_late_record` 对 `sched_boot.c` 中 `selftest_record_late(` 调用次数的断言从 `7` 更新为 `8`,并同步注释。

### 4.3 PID 分配结果

| 构建变体 | init | 后续 |
| --- | --- | --- |
| 正常构建 | **1** | `tcpip_thread` = 2;thermal worker 及以后依次 |
| `KERNEL_SELFTEST=1` | **1** | `probe` / `tcpip_thread` = 2 / 3(相对顺序不规定) |
| 无网卡(QEMU 不配 NIC) | **1** | `net_lwip_start()` 走 `count == 0` 直接返回,不建 `tcpip_thread` |

### 4.4 安全性论证(不变量)

- **窗口仍开且放置路径不变**:插入点位于 `sched_boot_creation_begin()`(`kernel_main`)与
  `sched_boot_start()`(`task_init`)之间。
  - **UP(`-smp 1`)**:此时 `percpu_data[0].scheduler_ok == 0`,无任何核 `scheduler_ok`,`kthread_place_unpinned()`(`kthread.c:57-69`)通过引导窗口回退到 CPU0,与迁移前完全一致。
  - **SMP(`-smp >= 2`)**:AP 已在 `x86_64_boot_aps()` → `ap_entry()`(`smp.c:160`)中发布 `scheduler_ok = 1`。`spawn_user_task` 与 `create_kthread` 均通过 `sched_pick_cpu_mask()` 挑选 `online && scheduler_ok` 的 AP 并发送 `IPI_VECTOR_RESCHED`。迁移前后它们同处于 post-AP、pre-BSP-scheduler 区间。
- **并发与互斥前提不退化**:
  - **UP 下**:`net_lwip_start()` 的 `tcpip_init` + `netif_add`/`dhcp_start` 循环全程在 BSP `scheduler_ok == 0` 下执行,即便 timer tick 到来,`schedule()` 首行见 `!rq->scheduler_ok` 立即返回,`tcpip_thread` 在 `sched_boot_start()` 之前绝不可能抢占运行。方案 A 保留了这条 UP 互斥前提,而方案 B 会破坏它。
  - **SMP 下**:迁移前 `net_lwip_init()` 在 `x86_64_boot_aps()` 之后调用时 AP 就已 `scheduler_ok == 1`,迁移到 `task_init` 内 `sched_boot_start()` 之前并未改变 AP 的可调度状态;相反,由于 `spawn_user_task("/bin/init")` 先向 AP 投递了一个可运行任务,在双核(`-smp 2`)下 `init` 先占用 CPU1,进一步降低了 `tcpip_thread` 与 BSP `netif_add` 循环并发交错的概率。
- **DHCP 与 `NET_ONLINE` 发布语义不变**:`net_lwip_start()` 通过 `s_tcpip_core_ready`(`tcpip_init_done_cb` 置位)与 `s_adapters_finished`(循环末尾置位)双标志原子汇合发布 `NET_ONLINE`(`lwip.c:98-121`),不依赖谁先到达;`wait_for_dhcp()` 轮询无感。
- **早到 RX 安全**:NIC IRQ 路径的 `sys_mbox_wake()` 在 `tcpip_init()` 创建 `g_tcpip_mbox` 之前是显式 no-op;栈晚启动毫秒级时间不丢包不崩溃。
- **`files_t` 继承差异已审计且良性**:迁移前 `net_lwip_init()` 与 `sched_boot_probe_create()` 在 `kernel_main` 中执行时 `current`(idle)的 `files == NULL`;迁移到 `sched_boot.c:106` 之后,`current->files` 已在 `78-98` 行挂载了 `/dev/tty` 的 fd 0/1/2,因此 `arch_kthread_prepare()`(`task_arch.c:97-103`)会为 `tcpip_thread` 和 `probe` 线程执行 `files_dup(current->files)`。`probe` 线程退出时经 `kthread_exit()` → `arch_kthread_drop_files()` 完整释放引用,不影響 `test_fd_refcount` 或 `test_kthread_self_reap` 的基线计数;常驻的 `tcpip_thread` 与后续 `arch_thermal_start_worker()` 创建的 thermal 内核线程一样持有一份 `files_t` 副本,不访问 fd 表,完全良性。
- **中间阶段无网络依赖**:`kernel_main` 中原位于 `net_lwip_init()` 之后的 `futex_init()`、`console_init()`,以及 `task_init` 在 `spawn_user_task` 之前的段(TSS、`init_mm`、`sched_boot_publish_idle`、fd 表初始化)与 `spawn_user_task` 本身均不触及网络。用户态 `/bin/init` 即使在 SMP 下于 AP 上提前几十微秒开始执行,其初期仅做 `printf`、`getpid`、`signal` 与读取 `/etc/inittab`,不发起 socket 调用(且即便发起,`do_socket` 也有 `!net_service_ready()` 返回 `-ENETDOWN` 的完备保护)。

## 5. 测试与验收

- `make run`(正常构建,`DEBUG_CHANNELS=task`):串口出现
  `init: spawned user-space init, pid=1`、`init: running as PID 1` 以及
  `net: stack online with 1 active adapter(s)`(注:SMP 下用户态 `init` 与内核 `net: stack online` 跨核并发输出,不强求这两行的固定先后顺序)。
- 优雅关机回归(按 AGENTS.md headless 注入方式):注入 `/bin/poweroff`,
  预期 `kill(1, SIGUSR2)` 返回 0,串口出现 init 的 `init: received signal 12, shutting down...` 日志(此前因 PID 1 是 `PF_KTHREAD` 返回 `-EPERM`,只能走 `poweroff: init unreachable, forcing power-off...` 兜底)。
- `make OS01_SYSTEST=1 test-syscall`:228 项全部通过(systest 是 init 的
  子进程,时序不变)。
- `make KERNEL_SELFTEST=1 test-kernel-selftest`(与 systest 分开跑,既有
  规范):既有 late cases(含 `sched_boot_probe`)通过,证明窗口语义未破坏;新增 `init_pid` 通过。
- `make test-qemu SUITE=phase-0`:含新增 `init: running as PID 1` 断言通过。
- `make test-qemu SUITE=network`:UDP/TCP/DHCP/wget 用例通过(`wait_for_dhcp` 不超时)。
- `make test-host` 与 `make test-harness`(或 `python3 -m unittest qemutests.test_run_test_harness qemutests.test_kernel_selftest_result`):宿主单测与 harness 自测全部通过。

## 6. 风险与边界

- **回归风险**:后续有人在 `spawn_user_task` 之前又插入 kthread 创建。防护 = R1 条文 +
  phase-0 E2E 断言 + `init_pid` selftest 哨兵。
- **误移风险**:后来者把 `net_lwip_init()` 再"整理"到 `sched_boot_start()` 之后。
  防护 = R2 条文及调用点注释必须写明原因(UP 引导窗口回退与 `netif_add` 免抢占前提)。
- **SMP 理论竞争**(`netif_add`/`dhcp_start` 循环与 AP 上已启动的 `tcpip_thread`):
  迁移前后同处于 post-AP、pre-BSP-scheduler 区间,不引入任何新风险;若未来对 lwIP 做多网卡并发加固,则按 R2 启用 lwIP core locking。
- **回滚**:改动为单点调用迁移 + 注释/测试,git revert 单 commit 即可。

## 7. 参考

- `docs/superpowers/specs/2026-07-18-lwip-networking-design.md` — lwIP 接入设计
- `docs/superpowers/specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md` —
  引导创建窗口语义(§8.1)的权威定义
- Linux 对照:`init/main.c` `rest_init()`/`kernel_init()`、`kernel/kthread.c`
  kthreadd 孵化模型、PID 1 的 SIGNAL_UNKILLABLE 信号保护

## 8. 评审记录

- 第一轮(主 agent 初审 v1 → v2):识别并修正 4 项问题:(1)澄清 SMP 下 AP 在 `smp_boot_aps()` 已置 `scheduler_ok = 1`,将"所有 CPU `scheduler_ok == 0`"修正为"BSP(CPU0)`scheduler_ok == 0`"并区分 UP/SMP 论述;(2)补充 `tcpip_thread` 与 `probe` 线程在 `spawn_user_task` 后创建会继承 `current->files`(`/dev/tty` fd 0/1/2)的安全性审计;(3)补齐 `qemutests/test_kernel_selftest_result.py`(late case 计数 7→8)、`kernel/selftest/selftest.c`、`qemutests/test_run_test_harness.py`(`Phase0SuiteTests` 桩数据)及 `run_test.py` `read_until` 目标推进;(4)修正 §5 `DEBUG_CHANNELS=task` 与 SMP 下串口输出并发顺序说明,补齐 `sched_boot_probe.h`、`net.h`、`subsys.md` 等注释清单。
- 第二轮(独立子 agent 评审 v2 → v3,结论 `APPROVE`,0 阻塞项):逐行核对仓库全量引用与 `qemutests/`、`hosttests/` 源码扫描器,确认技术方案、UP/SMP 不变量与测试套件改动完备无误;同时采纳 3 条非阻塞完善建议写入 v3:(1)将 `kernel/net/net.c:18` 的调用点注释 `(kernel/core/main.c)` 纳入 §4.2 同步清单;(2)将 `kernel/selftest/selftest.c:225-228` 上方注释中的 `kernel/sched/core.c` 同步为 `kernel/arch/x86_64/cpu/sched_boot.c`;(3)注明 `test_init_pid()` 需 `#include <sched/internal.h>` 以访问 `user_init_task` / `user_init_pid`。
