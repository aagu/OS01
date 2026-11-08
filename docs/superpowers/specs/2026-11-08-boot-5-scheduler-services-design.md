# BOOT-5 调度启动与系统服务解耦设计

日期：2026-11-08；版本 R2；独立 reviewer `boot5_spec_review` 已 APPROVE。仅文档，未实施。见[评审记录](../reports/2026-11-08-boot-5-doc-review.md)。

## 1. 目标与基线

基线 `f610e665`，BOOT-1～4已有实现与验收报告。当前公共main在BOOT-4 runtime检查点之后打开creation window、初始化futex/console，再进入x86 task_init。该函数承担TSS/IST、init_mm、idle、stdio、init、网络、调度发布、VFS bootstrap结束、thermal、自测及永久idle。

目标：公共用户态启动策略与架构机制分离；首次可运行用户任务之前结束全局VFS锁豁免；init仍首先获得PID1；网络/thermal/自测保留各自合法时机。ARM保持无用户态产品配置和已有EL1调度handoff，不用空成功hook伪装共同策略。

不改变UAPI/boot_context/percpu/task/thread/pt_regs布局、BOOT-1 admission/online/IPI语义、BOOT-2 IRQ/校准、BOOT-4阶段/依赖与检查点。不并行探测、不更改init supervision/网络DHCP策略、不新增EL0支持，不全面改造spawn/fork或VFS锁体系。

## 2. 现状、风险及方案

`spawn_user_task()`直接选择CPU、发布TASK_RUNNING、入队并通知。x86 AP此时可能已scheduler_ok=1，PID1可能在BSP仍执行net_lwip_init时访问VFS；当前vfs_end_bootstrap却在BSP sched_boot_start之后，vfs_sched_ready用普通全局int读取，进程可跳过namespace/FS mutex。此处是可达风险分析，本轮不声称已实测竞态。

选择“提前关闭VFS bootstrap + 保留现有AP资格 + 公共策略流水线”。不暂停全部AP，不把所有任务钉在BSP，不为首进程增加隐藏task状态。延迟所有任务发布要重写spawn/服务constructor并改变UP网络契约；只搬文件则保留风险。首次用户任务允许在已合格AP上先于BSP scheduler_ok运行，但必须晚于VFS runtime发布；不能把BSP调度就绪等同全系统已可运行。

## 3. 职责、目录与公共接口

新增启动策略子目录及对称公开头：`kernel/init/` ↔ `kernel/include/init/`，先成对确定后建文件。

- `init/userspace.c` ↔ `include/init/userspace.h`：`void kernel_start_userspace(void) __attribute__((noreturn))`，唯一用户态策略入口。仅OS01_HAS_USERSPACE产品编译/调用，不保留task_init别名。
- `init/services.c` ↔ `include/init/services.h`：`void boot_services_prepare(void)`执行现有net_lwip_init；`void boot_services_start(void)`执行thermal并warn降级。接口名字表示启动尝试，不承诺网络ONLINE/worker一定成功，不新建服务注册器。
- `selftest/boot_late.c` ↔ `include/selftest/boot_late.h`：`void selftest_boot_creation_probe(void)`和`void selftest_boot_run_late(void)`。x86用户态profile启用OS01_SELFTEST时执行现有probe/九个late case及单一END；非自测头提供inline空操作，无运行成功日志。ARM继续自身既有selftest路径，不链接x86 late实现。
- x86 `cpu/sched_boot.c`仅保留强机制hook `void arch_sched_boot_prepare(void)`（既有arch_task_init_platform TSS/IST）与 `void arch_sched_boot_idle(void) __attribute__((noreturn))`（原hlt/need_resched/schedule/IRQ-enable循环），声明在公共 `include/arch/boot.h` 的USERSPACE门内。ARM无对应空实现；ARM0/0无这些引用。
- `sched/boot.c`/`include/sched/boot.h`仍只负责creation window、idle元数据、scheduler_ok发布；不引入VFS、lwIP、thermal或late selftest依赖。既有ARM caller及0→2关闭路径不变。

原task_init声明/定义/调用删除，main在runtime/post-SMP自测后调用kernel_start_userspace。main的creation_begin/futex/console也移到该入口原相对顺序，ARM仍走arch_start_kernel_idle。init_mm通用segment/VMA元数据初始化放userspace内部helper，不挪动早期CR3保存；init_task/AP idle任务链及CPU0 runqueue绝不清空。

## 4. 用户态启动顺序与失败边界

公共入口在BSP合法init_task_union栈上，BOOT-4 runtime已核验、BSP online/IPI/tick有效；AP可能已运行idle。顺序为：

1. sched_boot_creation_begin → futex_init → console_init，保留main原时机。
2. arch_sched_boot_prepare → 初始化init_mm通用segment/VMA元数据 → sched_boot_publish_idle。不重设CR3，不重新初始化task list/runqueue。
3. sched_boot_prepare_stdio(&init_task_union.task, &component)，失败BOOT-3 stage=stdio停止，保持引用事务；成功保留BOOT-3 stdio sentinel。
4. 在所有启动期namespace/挂载/TTY及stdio操作已经返回、无VFS/FS锁持有时，vfs_end_bootstrap；输出 `[boot-5] vfs-runtime`。不得等到spawn后才关闭。
5. 设置idle pgrp/session=1，按原BOOT-3 spawn enter/path/leave夹住spawn_user_task；负值stage=init/component=/bin/init停止，不probe、不net、不sched_start、不thermal、不late。正值!=1也停止但任务可能已发布，禁止事后unpublish/回滚PID。正常保留旧 `init: spawned user-space init, pid=1`，另输出 `[boot-5] init-created pid=1`。
6. selftest_boot_creation_probe → boot_services_prepare(net)。均在PID1之后、creation window仍open、BSP scheduler_ok=0。AP上的服务/用户任务可以已经执行；UP保持原netif_add/DHCP初始化的不可被BSP调度抢占时机。
7. idle state=TASK_RUNNING → sched_boot_start（先永久close window，后release BSP scheduler_ok=1），输出 `[boot-5] runtime-ready window=closed`。
8. boot_services_start(thermal) → selftest_boot_run_late → arch_sched_boot_idle，不返回。

VFS runtime比BSP scheduler_ok更早，二者分别表示锁策略与CPU执行资格。boot_services_prepare不做VFS namespace操作，也不以BSP scheduler_ok为前置；thermal必须在调度发布之后，失败只warn。没有网卡时保持NET_OFF/旧skip日志，不能阻塞shell或把网络缺失提升为fatal。

## 5. VFS bootstrap退出契约

保留 `void vfs_end_bootstrap(void)`，把既有vfs_bootstrap改为release写0/acquire读；新增只读 `bool vfs_bootstrap_active(void)`。vfs_init仅在系统最早期设1，生产中不得reopen；end重复调用幂等，但公共策略只有一个调用点。

vfs_sched_ready继续表示“bootstrap已结束且current为pid>0进程”，不能仅因BOOT-5将idle上下文纳入可睡眠mutex。ARM0/0不链接VFS。测试专用vfs_test_set_sched_ready仍可reset，但使用atomic且不得进入产品路径。

首次非idle任务publish之前必须满足bootstrap=false，且转换时没有正在进行的豁免namespace/FS锁区间。正常策略在stdio之后、spawn之前满足此点：此前只有idle/AP基础设施，没有普通PID任务。runtime后禁止从pid0 idle启动namespace/挂载操作；现有late自测的文件引用操作与在普通kthread里的FS操作不等同idle namespace操作，实施须审计所有调用者。不可在持锁mid-operation或IRQ中切换此门。

不新增“全局VFS所有访问都必须持一个大锁”的机制；本项保证已有vfs_ns_lock/ext2/fat/tmpfs使用的scheduler gate在进程首次进入时已生效。release/acquire只解决发布和可见性，不能补偿未受保护的idle访问。

## 6. 初始用户任务发布门与placement

spawn_user_task入口在VFS bootstrap仍active时return-EAGAIN，发生在lookup/分配/PID消耗之前。只改这个启动spawn路径；普通fork由已进入runtime的用户任务发起，不更改其API。本项不以事后日志代替发布前检查。

初始placement使用sched_pick_cpu_mask而不是无条件CPU0 fallback：目标必须在拓扑内、online acquire且scheduler_ok acquire。无合格目标时仅允许BSP调用者在creation window open且CPU0 online的条件下暂存到CPU0；UP保留此例外，pinned kthread不获得例外。没有合格CPU且例外不成立return-EAGAIN，不访问越界runqueue，不分配PID。

最终publication按task_list_lock→目标rq_lock统一持锁，重新检查目标资格（同一boot exception）及VFSruntime后，才list insert/enqueue，后续没有可失败步骤；失败释放全部私有task/thread/files/FPU/mm/用户映射（stack page由mm teardown拥有），不insert/enqueue/notify，不回滚已消费PID。锁外释放资源，顺序遵循现有files_unpin/mm销毁协议。不把原分开的list/rq临界区里先list再检查造成半发布。

允许CPU0排队任务在BSP发布后或被合格AP平衡取得；目标资格读取不保证CPU hotplug，此项目无CPU下线协议。本项不扩张为全scheduler placement重写。user_init_task/user_init_pid既有记录用途保留，不承诺内核init-created日志早于AP用户输出。

## 7. 自测与兼容契约

九个x86 late case保持原注册和执行顺序：kernel_mutex、kthread_self_reap、sched_boot_probe、init_pid、fd_refcount、bcache_smp_coherence、pgrp_signal、tty_vintr、x86_intr；END恰一次，task-tests-done marker保留。probe创建仍在PID1后/boot window内；真正执行和负向placement检查仍由原测试验证。

ARM的boot stack→idle stack、BSP tick验证、sched_boot_start→APrelease→IRQ/open→late/idle全过程不移到用户态策略。共享sched_boot_start不加USERSPACE/VFS门，以免破坏ARM。默认ARM产物无init/userspace/services、x86late、VFS/net新依赖。

BOOT-3故障fixture保持stdio/spawn窗口和fatal字段、旧runner识别的init成功行；BOOT-4 main序列host测试仅将原task_init入口替换成kernel_start_userspace，不降低三个检查点和feature gate断言。

## 8. 验证

### 8.1 Host与结构

- 真实userspace入口轨迹：成功全序、stdio/spawn失败无下游、unexpected PID非1、net无卡/thermal失败降级、late/END只一次；机制hook独立验证TSS参数与idle恢复IRQ，不mock整条策略当作实现。
- 真实VFS对象：bootstrap gate原子可见性、end幂等、pid>0启用/idle仍豁免；namespace入口实际取得/释放mutex。转换前无进行中操作由轨迹与受控并发测试验证，不在操作中强行翻转gate。
- 真实spawn与balance/boot对象：bootstrapactive在lookup之前拒绝；AP资格、UPfallback、window关闭、offline/越界；分配成功后最终locked recheck失败时无list/enqueue/notify且完整释放。保留BOOT-3 files_dup失败回归，不靠spawn stub证明publication安全。
- 编译/符号审计：x86策略不含TSS/HLT细节；机制文件不依赖fs/net/thermal/late；sched/boot无service依赖；ARM产品不链接USERSPACE入口或虚假hook；原task_init产品符号消失。新旧测试指向实际迁移后的源文件，不保留被mock替代的旧整函数。

### 8.2 新QEMU矩阵

新增x86 `make test-qemu SUITE=boot-services`，共8组：normal 1/2/4/8（4），overlap 2/4（2），no-net 1/2（2）。normal/no-net用普通产品snapshot，无fixture；no-net禁用默认NIC并显式确认无NIC，检查NET_OFF skip、PID1/终端/shell仍成功。三条生产BOOT-5 marker各一次、有BSP偏序vfs-runtime→init-created→runtime-ready，但用户PID1输出允许穿插其中。

overlap使用唯一隔离 `BOOT_SERVICES_TEST=overlap`，其余为空/none，拒绝其它fault、自测、systest/nettest混用。fixture在spawn前arm，在真实namespace或所选root FS（FAT/ext2）的lock wrapper成功取得mutex后，仅PID1且cpu!=0时采集bootstrap=false、vfs_sched_ready=true、BSP scheduler_ok=0、creation window=open，release发布观察；不得持namespace/FS锁打印或等待。BSP在spawn返回后、probe/net/sched_start之前用jiffies+200（2秒）有界等观察，acquire后打印 `[boot-5-test] overlap pid=1 cpu=<1..N-1> bootstrap=0 locked=1 bsp_scheduler_ok=0 window=open`；采样不符合不得伪造成功。PID1默认只读/etc/inittab，不保证进入namespace写锁，因此observer接在vfs_ns_lock及FAT/ext2实际mutex取得分支；不能只观察vfs_sched_ready返回true便声称locked=1。observer只在fixture编译，普通产物无身份/注入符号。fixture不创建线程、不耗PID，不通过sleep改变产物源码逻辑，等待BSP IRQ保持原状态。

runner验证overlap身份、真实锁观察及三marker/成功链；8组均检查原BOOT-4runtime检查点、init PID1、terminal/PTY shell。每QEMU120秒绝对期限，完整证据后至少1秒排空，由runner主动结束；timeout/静默/崩溃/自退、重复/错身份/marker乱序一律FAIL。归档显式ELF/image/firmware路径与hash、命令、原串口和结果JSON；普通image前后hash相同。解析负例纳入test-harness，构建selector必须经mk/project.mk受控白名单传到实际kernel/image子make。

### 8.3 回归与完成定义

BOOT-1 8组、BOOT-2 10组、BOOT-3 17组、BOOT-4双架构14组全部回归；test-host/static/harness；x86 phase-0/inittab-phase/network/systest与8核kernel-selftest分开；ARM smp/no-ack/m3-probe/sched-ap/sched-wake/sched-tick/logo。真实交互回归执行cat后Ctrl-C，确认前台cat收到SIGINT且terminal/ash存活，再执行唯一echo；按既有PTY链从serial stdio注入，未出现命令回显不能当成功。

KERNEL_SELFTEST关闭传空，不能传0。syscall/systest使用现行bucket：`make OS01_SYSTEST=1 test-qemu SUITE=systest`。本轮文档批准不代表实施/验收；实施报告必须列命令、rc、产物身份和未运行项，不以旧报告或静态检查代替8组新矩阵及并发证据。
