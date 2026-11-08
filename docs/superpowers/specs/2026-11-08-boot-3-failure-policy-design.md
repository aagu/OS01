# BOOT-3：启动失败传播与必需能力检查设计

日期：2026-11-08；版本 R3，独立子 agent `boot3_spec_review` 已 APPROVE；未实施。见[文档评审记录](../reports/2026-11-08-boot-3-doc-review.md)。
基准：`1212913c`（包含 BOOT-1/2 实现）。依据：[启动分析 F3](../reports/2026-11-07-boot-sequence-analysis.md)、[roadmap](../../roadmap.md)；前置：[BOOT-2](2026-11-08-boot-2-early-irq-time-design.md)及其[验证记录](../reports/2026-11-08-boot-2-validation.md)。

## 1. 目标与范围

让注册错误无法被 wrapper/iterator 吞掉，必需子系统返回失败后立即停止启动，可选失败仍有可查询的失败状态；在根文件系统、TTY/PTY、标准 fd 和 PID 1 创建处执行明确的能力检查。已存在的 BOOT-1/2 fatal、DEVICE_UNSAFE 等不可恢复路径保留协议，不强行把不返回函数变成可恢复错误。

不重排 BOOT-2 的控制器/percpu/校准/IRQ 时间线，不实现 BOOT-4 DAG、phase枚举或注册表动态扩容，不进行 BOOT-5 task_init 全面拆分；不改变 UAPI、boot_context、percpu/task/thread/pt_regs 布局、CPU admission、VFS bootstrap 锁协议或 PCI 可选设备探测策略。只支持现有默认 x86 用户态 profile 与 ARM kernel-only profile，不声称补齐任意特性门组合。

源码已确认：register wrapper多处忽略register_subsys结果；两架构arch iterator忽略initcall返回；非OPTIONAL失败只日志后继续；percpu回调即使失败仍标初始化成功；tty_alloc失败后仍注册空节点；PTY失败仅日志；stdio分配部分失败静默继续；spawn_user_task返回值被丢弃。根挂载已panic，不是本期新发现“未检查”的问题，本期补错误归因与测试。当前分析为静态核对，不声称故障已经复现。

## 2. 方案与取舍

| 方案 | 取舍 |
|------|------|
| 所有启动函数改返回大结果结构、调用者逐层检查 | 全面但侵入过大，新增遗漏检查点和大量无关签名迁移 |
| 只在main补几个panic | 无法阻止phase内后续消费者运行，也无法发现注册wrapper吞错 |
| 框架内强制失败屏障 + 关键能力边界检查 | 保留现有phase API和成功路径，错误在最小责任边界停止；可独立于BOOT-4/5实施 |

选第三种。框架void phase/percpu API保留，内部遇必需失败调用跨架构boot fatal；注册函数继续返回负errno，另外锁存首个注册错误，统一initcall iterator检查，wrapper即使错误地返回0也不能掩盖它。

## 3. 失败分类与能力政策

| 项 | 默认政策 | 成功/降级要求 |
|----|----------|----------------|
| 注册参数、重复名称、表满、initcall返回非零 | 注册表完整性错误，一律fatal；不受OPTIONAL影响 | 可选硬件缺失应在init/probe返回，不能伪装成注册失败 |
| 普通/每CPU init返回非零 | 非OPTIONAL立即fatal；OPTIONAL记录失败继续 | 保存原始返回码，不能标成功；不自动重试 |
| APIC/timer/PIT/LAPIC-stub等已设能力门 | 保留BOOT-1/2实际门控 | clocksource inactive=0仍为合法降级；不能把所有OPTIONAL timer概念升为硬件必需 |
| 设备模型/PCI协调器 | 保留既有DEVICE_UNSAFE和返回码策略 | 非root NIC/USB等普通probe失败不因本期被统一升级fatal |
| VFS/devfs与根 `/` | 现有HAS_VFS路径必需 | 根不可用在mount边界停止；不尝试后续TTY/SMP/PID1 |
| `/boot`、`/tmp`、`/proc` | 可降级 | 保持已有root选择/GPT与whole-disk FAT分支；挂载失败明确warn，不影响有效根和init路径 |
| TTY、`/dev/tty`、`/dev/tty0`、PTY `/dev/ptmx` | 默认x86用户态配置必需（含systest/nettest） | 节点存在不替代有效对象；EEXIST不自动当成功，不覆盖外来节点 |
| `/dev/keyboard`、mouse、fb/gfx等 | 保留既有后端返回及设备节点政策 | 不把PS/2 keyboard是否存在当console TTY分配成功条件；mouse仍依赖keyboard probe状态 |
| fd 0/1/2、初始 `/bin/init` | HAS_USERSPACE路径必需 | 三个fd全部就绪才附到idle并spawn；负spawn结果停止后续服务启动 |

其它已存在的不可返回panic仍可在更低层先发生，不要求重复BOOT-3标记。策略不增加动态配置语言；未来无PTY/串口专用init profile需单独定义，不用解析inittab猜测内核能力。

## 4. 跨架构 fatal 边界

新增启动/致命路径单元 `kernel/core/boot_failure.c` 与 `kernel/include/core/boot_failure.h`，默认实现不依赖x86 kpanic：先关本地IRQ，log_err唯一完整记录

`[boot-3] FATAL stage=<register|subsys|percpu|fs|tty|pty|stdio|init> component=<name> rc=<signed> phase=<3..6|-1> cpu=<id|-1>`

随后永久arch_cpu_halt循环，不返回。日志可用前不得调用；当前两架构注册/phase执行均已过早期串口初始化。ARM实际log_impl只支持%s/%u等而不支持%d/%ld，因此fatal和新增可选失败记录必须在此单元用固定栈缓冲将signed rc/phase/cpu转为十进制字符串，只以%s交给log_err/log_warn；不依赖snprintf/堆分配，不改ARM全局formatter。rc接口用int64_t，覆盖INT64_MIN时以unsigned幅值计算避免有符号取负溢出；phase/cpu也转为字符串，不能在可选失败warn里继续使用不支持的signed格式。phase/cpu不适用用-1；rc保留原始非零值，包括legacy正返回。函数标noreturn，ARM显式加入kernel whitelist，不能因ARM未链接panic.c而丢失fatal语义。

fatal调用点不得持有task_list_lock、files锁、rq锁或注册表锁；回调在锁外执行。本期停止的是启动CPU，不保证已经启动的其它AP被全局停止；不释放可能被AP/IRQ/已发布节点使用的资源，不声称全局panic停机协议已实现。

## 5. 注册完整性和子系统失败屏障

### 5.1 注册

register_subsys / register_subsys_percpu 验证非空非空串名称、非空函数、合法phase（单次3–6）、已知flag（只OPTIONAL）；同一表内名称唯一，BSP表与percpu表各自命名空间。参数错-EINVAL、重复-EEXIST、满表-ENOSPC，固定64/16容量保持。失败不新增/覆盖条目，锁存第一次错误的name/rc/phase/class；后续错误不得覆盖首因。已锁存错误后不继续接受新注册。

新增公共initcall遍历入口，供两架构iterator使用：逐项调用并立即检查返回值和注册错误latch；两者都异常时优先首个真实注册错误，否则component=initcall并附槽位诊断。null函数条目也是错误。遇错误不执行下一个initcall，尚未到init阶段即停止。所有生产wrapper改为真实传播返回值，多注册wrapper在第一错误处返回；APIC注册wrapper改成return错误以便统一报告，init wrapper现有不可返回fatal不强制迁移。

任何phase/percpu执行前也检查latch，因此手动注册路径不能绕过屏障。初始化开始后封闭BSP注册；percpu表在其dispatch开始后封闭，x86在SMP后注册percpu的既有顺序不变。晚注册返回错误并锁存，当前callback返回后立即检查，不允许吞错继续执行；不提供运行期热注册。

### 5.2 执行与查询

单次init：0未执行、1成功、-1失败的subsys_status语义及未知-999保持。保存原始result供新查询接口使用；可选失败明确保持status=-1，后续phase执行不会重试；非OPTIONAL失败在当前回调后fatal，不执行同phase下一个或后续phase。

percpu dispatcher仍在BSP按原num_cpus遍历回调，不冒称回调运行在目标CPU。非OPTIONAL失败立即停止；OPTIONAL失败保存首个rc与failed_cpu_mask、继续其它CPU，最终entry为失败而非成功；重复dispatch不重试。只对callback给出的cpu编号归因，不改变BOOT-1在线条件或BOOT-4执行位置。

当前消费者继续用subsys_status==1检查能力。新增显式require查询：未注册-ENOENT、未执行-EAGAIN、成功0、失败保留负rc（legacy正rc映射-EIO用于返回查询，原始值仍可诊断）。只有消费者本身需要该能力时才提升为fatal，不能给所有OPTIONAL一刀切。鼠标现有keyboard依赖检查保留，失败keyboard不能导致mouse误标成功。不开DAG拓扑排序。

## 6. 文件系统与终端边界

### 6.1 文件系统

`fs_boot_prepare()`仍返回devfs准备错误，由main以stage=fs/component=devfs停止。VFS初始fs type登记缺失导致devfs/root挂载失败时可沿现有mount返回检测，不在本期重做整个VFS registry API。

根选择和fallback不变，但内部mount helper返回实际rc：无disk=-ENODEV、GPT有效但不足两分区=-EINVAL、ext2/FAT根mount失败保留实际负值。根失败先返回到fs_boot_mounts责任边界输出BOOT-3 fatal；不继续mount tmpfs/procfs或后续阶段。`/boot`失败仍warn；root成功后/tmp、/proc分别尝试，不因一个失败跳过另一个。

tmpfs_init/procfs_init改为返回实际登记/挂载错误，fs_boot_mounts对其warn降级。VFS init已登记这两种fs type，正常二次register的-EEXIST只表示类型已存在，应继续mount，不能让所有正常启动被误判失败。块设备节点发布失败保留既有诊断，不把“devfs节点不在”误当物理root不可读；显式root标识/磁盘选择改造另行规划。

### 6.2 TTY/PTY

tty_alloc失败立即返回-ENOMEM，不注册空private_data节点或绑定serial/keyboard。已有console对象后注册`tty`/`tty0`，任何错误（包括-EEXIST）传播；均成功后才发布console输入/输出绑定，helper直接接收有效console，不能通过keyboard_get_tty间接推断。默认keyboard硬件缺失时仍可建立串口console。

现devfs无注销API：首节点已发布、第二节点失败时不释放其console；返回main后立即fatal，保留有效资源而非伪造回滚。PTY registration失败传播并停止，不把-EEXIST当有效ptmx，不覆盖他人ops。启动初始化只调用一次，不引入重试/动态注销。

## 7. 标准 fd 与 PID 1

在现有x86 task_init的同一位置抽一个小的stdio准备helper（放sched子系统，源/头成对），不搬动服务/调度编排。对显式传入的BSP idle目标创建私有files表，查找有效`/dev/tty`，构造三个file，调用fd_alloc_limit验证准确安装0/1/2（soft limit使用目标idle，不依赖current隐含目标）。全部成功前不写idle->files；失败释放尚未安装的file、files_unpin私有表、vfs_node_put查找引用，不能双释放slot拥有的file。无锁持有时drop refs。成功后一次附表，放掉独立lookup引用。

每个失败必须有可定位component：files、tty-node、file-0/1/2或fd-0/1/2；ENOMEM/ENOENT/EMFILE等保留。拒绝覆盖目标已有files。task_init失败后不调用spawn_user_task。已安装的私有表回滚按现有ref协议，不直接files_free一个仍refcount=1的表。

stdio表成功附给idle仍不足以保证子进程继承：真实spawn_user_task中current->files非NULL但files_dup返回NULL时，必须在task-list/enqueue/user_init_task发布之前释放尚私有的raw stack、thread、mm及lookup引用并返回-ENOMEM，不能把NULL文件表当成功。仅补这个继承失败屏障，不全面重构exec/FPU；已分配的PID不回滚，本期失败即停止、不重试。原调用者本就无files时的语义保持。

spawn_user_task返回负值才作stage=init/component=/bin/init fatal，rc使用int64范围安全记录（当前errno范围）；不启动boot probe、lwIP、BSP sched_boot_start或thermal。保持init先分配PID1约束，正常返回应为1；如检查意外正PID不等于1，必须承认该任务可能已enqueue/在AP运行，不能声称“发布前阻止”。本期不改spawn的publish协议，不新增事后unpublish/kill回滚。

成功打印 `[boot-3] init-created pid=1`；此行可能晚于AP上的PID1用户输出，parser不得强制二者跨核先后。BOOT-1 complete在task_init之前，本期init/stdio fatal允许已经出现该行，不可作为统一禁止标记。

## 8. 验证与失败注入

### 8.1 Host

真实subsys.c：必需/可选失败、正rc、同phase哨兵未执行、首错误latch/吞错wrapper、重复/空名/非法phase/flag/满表、BSP封闭与独立percpu注册时机；percpu可选部分失败mask/status、必需失败停止下一个CPU、重复dispatch不重试。真实两架构initcall adapter通过同一helper，不用复制iterator逻辑自证；fatal用测试链接替身longjmp捕获（测试替身noreturn），产品fatal本身由单独编译/行为边界验证。另一个host binary必须编译真实boot_failure.c与真实ARM runtime/log_impl.c，只替换UART/kputs/kputu/kputx和halt边界，捕获完整fatal/warn行，覆盖负rc、INT64_MIN、phase/cpu=-1和混合字符串参数顺序；不能以logger替身或仅ARM链接成功掩盖格式错误。

真实fs_boot/TTY/PTY与stdio helper：无root/GPT不足/mount错误阻断；/boot/tmp/proc降级仍继续；tty_alloc/节点冲突与pty注册失败；files/tty lookup/第0/1/2次file分配及fd安装失败，检查引用平衡、没有部分附表/后续spawn。真实exec.c在files_dup分配失败时必须返回-ENOMEM、无task-list/enqueue/notify/user_init记录且资源平衡；不得只mock spawn返回错误。真实task_init调用点用host边界捕获或精确集成契约确保spawn负返回后无服务调用；不只测试一个未被接线的check函数。

host使用生产对象，mock只替换外部分配/硬件/VFS边界，不复制policy；每组可单独构建/运行，声明准备与行为RED分开。新API缺失编译错误不能代替旧行为忽略错误的RED。已有正确root fatal/PCI可选策略可以直接记录GREEN基线。

### 8.2 隔离 QEMU

新增 `test-qemu SUITE=boot-failure`；normal 1/2/4，required-subsys 1/2，registration 1，root 1，tty 1，pty 1，stdio 1/2，init 1/2，optional-subsys 1/2，child-fds 1/2，共17组。KERNEL_SELFTEST传空值关闭；fault只有一个selector，独立kernel/image，普通镜像hash与ELF不受污染，保留virtio-rng和既有QEMU设备配置。

注入应驱动真实错误路径：required/optional在phase5注册专用无副作用测试回调返回-EIO，紧邻后哨兵证明停止/继续；registration制造同表重复名称并由统一iterator检测，必须在phase3之前停止；root让真实root mount调用收到受控错误而非直接boot_fail；tty在分配返回NULL边界、pty在ptmx注册返回错误边界、stdio在第2个file分配失败边界；init调用真实spawn路径加载专用不存在的ELF路径，不能先spawn成功再改返回值。child-fds在真实files_dup分配边界仅对初始spawn的继承失败注入，验证stage=init、component=/bin/init、rc=-ENOMEM且无发布；不能影响idle准备files_alloc或全局任意分配。外部ops故障由编译期fixture控制，不改变产品默认配置。

normal三组不含fixture身份、注入或能力哨兵，以生产init-created、PID1/shell成功链和归档ELF/image hash标识实际产物；若出现fault身份/注入行则拒绝。只有故障变体要求唯一variant身份、注入触发记录，fatal变体再检查预期component/rc；negative例必须看到唯一完整BOOT-3 fatal，观察≥1s，runner主动终止，不能静默/timeout/自行退出PASS。禁止标记按阶段定义：registration无phase3/context；required无其哨兵及后续root；root/tty/pty无后续SMP完成/PID1；stdio/init可有BOOT-1 complete，但无init-created/PID1/boot probe/lwIP/BSP scheduler发布后的续行。现有未输出的续行以隔离fixture的哨兵观测，不把缺少日志当唯一证明。

optional组有失败状态记录及后哨兵、root/terminal/stdio能力哨兵、init-created、PID1与shell；不能只“没panic”就PASS。normal不要求这些fixture哨兵，只要求生产成功链；跨核PID1先后不定按partial order判定。parser/process/ELF/变体隔离需缺失/重复/截断/错误rc/错误阶段/有fatal却继续等负例。归档serial、RunReport、CPU/profile/case/ELF/image身份。

### 8.3 回归与关闭条件

host/static/harness/catalog/scenario、BOOT-1八组、BOOT-2十组、phase-0、inittab-phase、network、syscall与8核kernel-selftest；ARM smp/no-ack/m3-probe/sched-ap/sched-wake/sched-tick，验证共享框架且不要求ARM VFS/用户态能力。独立ARM无userspace故障点的镜像仍须能链接boot fatal默认实现。

最终报告保存行为RED/GREEN和17组真实结果；所有正常/降级/停止条件可验证才关闭BOOT-3。只批准设计/plan不代表生产修复或实际验收完成。
