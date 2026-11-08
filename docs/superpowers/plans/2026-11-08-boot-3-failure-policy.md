# BOOT-3 Boot Failure Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 注册错误不被吞掉，必需能力失败停止启动，可选失败可诊断降级，PID1只在标准fd与继承准备成功后发布。

**Architecture:** 保留void phase/percpu执行API，内部设置失败屏障；注册返回负errno并锁存首错，双架构使用共同initcall遍历。关键VFS/TTY/PTY/stdio/spawn边界直接检查；默认fatal关闭本地IRQ、输出跨架构兼容记录后不返回。

**Tech Stack:** freestanding C、x86_64/AArch64、Clang/LLVM、已有VFS/devfs与files引用协议、C hosttests、Python unittest、QEMU/UEFI与既有profile/harness。

**Spec:** [BOOT-3 R3设计](../specs/2026-11-08-boot-3-failure-policy-design.md)，`boot3_spec_review` 已 APPROVE。基准 `1212913c`，实际实施前重核对HEAD/AGENTS.md。本文R2，独立 reviewer `boot3_plan_review` 已 APPROVE；见[评审记录](../reports/2026-11-08-boot-3-doc-review.md)。本轮仅文档，所有checkbox是未来实施步骤。

## Global Constraints

- 保持BOOT-1 CPU生命周期和BOOT-2 IRQ/校准编排；不增加BOOT-4 DAG/phase枚举/动态表，不实施BOOT-5全面task_init拆分。
- 不改变UAPI、boot_context、percpu/task/thread/pt_regs布局；subsys内部entry可增加result/mask，不是跨边界ABI。
- register失败一律属于完整性错误；OPTIONAL只影响init失败，不允许损坏的注册表继续启动。
- subsys_status保持0/1/-1和未知-999；clocksource inactive仍是合法降级；PCI普通可选probe失败、DEVICE_UNSAFE及既有不可返回fatal协议保留。
- 默认x86 VFS/TTY/PTY/stdio/init必需；/boot、/tmp、/proc可降级；ARM无VFS/用户态不得被新能力门误拦。
- fatal不依赖x86 kpanic；signed字段预格式化为固定栈缓冲字符串，经log_*的%s输出，支持INT64_MIN，不分配内存。
- 已发布节点/IRQ/AP资源不因启动fatal而回收；私有stdio失败按file_put/files_unpin/node_put协议回滚，不持锁drop refs。
- 不提供启动重试/运行期注册；spawn成功已enqueue，不做事后unpublish；负spawn停止后续服务，正常init仍首先取PID1。
- KERNEL_SELFTEST传空值关闭，syscall顶层OS01_SYSTEST=1且不混kernel-selftest；所有构建从根make入口，产物profile/variant隔离。
- 本次只产出文档，不执行下述任务，不把其他任务已完成的BOOT-1/2验证当本期验收。

## Review Focus

1. wrapper返回0掩盖registration失败，OPTIONAL错误或late注册覆盖首因（Task1真实框架/latch/adapter测试）。
2. ARM formatter不支持signed格式、INT64_MIN取负溢出、logger替身掩盖错误（Task1真实ARM logger与默认fatal）。
3. TTY分配失败注册空节点、节点EEXIST伪成功、partial节点回收产生UAF（Task2真实helper/资源保留）。
4. fd安装引用所有权、files_dup失败仍publish、PID1并行先后与负spawn被误判（Task3真实file/exec/task_init接线）。
5. fatal发生在SMP前后却用相同禁止标记、fixture直接panic/改spawn成功结果伪造证据、QEMU静默通过（Task4分阶段严格parser/process/ELF）。

## 文件与责任

| 单元 | 文件 | 责任 |
|------|------|------|
| 跨架构失败边界 | 新增 `kernel/core/boot_failure.c`、`kernel/include/core/boot_failure.h`；修改 `kernel/Makefile` ARM白名单 | 无堆signed格式、fatal/warn与永久halt |
| 框架 | 修改 `kernel/subsys/subsys.c`、`kernel/include/subsys/subsys.h` | 验证/latch、共同iterator、必需屏障、optional raw结果/percpu mask |
| 注册接线 | 修改双架构 `platform/subsys.c`、`kernel/include/arch/subsys.h`注释、x86 `smp/subsys_percpu.c`；所有SUBSYS_INITCALL int wrapper（timer/clocksource/PIT/serial/keyboard/mouse/APIC/PIC/LAPIC/device/thermal/故障fixture） | 真实传播错误；两架构复用共同iterator，不改调用顺序 |
| FS/终端 | 修改 `kernel/fs/boot.c`、`tmpfs.c`、`procfs.c`及对应头、`kernel/tty/boot.c`、`pty.c`及boot.h；main错误检查 | root归因、optional mount warn、有效TTY/PTY能力 |
| stdio/spawn | 新增 `kernel/sched/boot_stdio.c`、`kernel/include/sched/boot_stdio.h`；修改x86 `cpu/sched_boot.c`、`kernel/sched/exec.c` | 私有fd表事务、task_init检查、files_dup失败发布屏障 |
| Host | 新增 `hosttests/cases/test_boot_failure_subsys.c`、`test_boot_failure_log.c`、`test_boot_failure_mounts.c`、`test_boot_failure_terminal.c`、`test_boot_stdio.c`、`test_boot_spawn_failure.c`、`test_boot_task_init.c`；`hosttests/mock/boot_failure/`边界；更新host mk/旧mock | 真实对象的行为与引用/调用顺序；ARM formatter链 |
| QEMU fixture/runner | 新增 `kernel/selftest/boot_failure_fixture.c`、`kernel/include/selftest/boot_failure_fixture.h`、`qemutests/boot_failure.py`、`qemutests/test_boot_failure.py` | 精确失败点、17组证据与严格runner |
| 构建/登记 | 修改mk/project.mk、双profile、kernel/Makefile、mk/components/run.mk、catalog/scenarios/CI及枚举约束测试 | selector、隔离、子make、inventory/CI/证据归档 |
| 文档 | subsys/boot/fs/sched/build/arch/roadmap/changelog相应章节；新 `docs/superpowers/reports/2026-11-08-boot-3-validation.md` | 契约、真实门禁与关闭条件 |

新增源/头路径成对；boot_stdio是BOOT-3必要的局部事务，不搬动网络/thermal/调度编排。fixture从普通selftest wildcard排除，仅测试变体链接；ARM必须显式收集boot_failure.c，无需链接panic.c。

## Task 1: 框架完整性、必需屏障与跨架构记录

**Files:** 失败边界/框架/注册接线行；两个subsys/log host binary及mk注册；根focused入口。

**Interfaces:** 新增 `void boot_failure_stop(const char *stage, const char *component, int64_t rc, int phase, int cpu)`（noreturn）及 `void boot_failure_warn(const char *stage, const char *component, int64_t rc, int phase, int cpu)`，分别输出spec的FATAL/WARN同字段行。固定栈缓冲至少24字节/字段，用unsigned幅值处理INT64_MIN，只向logger传%s。新增 `void subsys_register_initcalls(subsys_initcall_t *begin, subsys_initcall_t *end)`、`int subsys_result(const char *name)`、`int subsys_percpu_result(const char *name, uint64_t *failed_mask)`、`int subsys_require_ready(const char *name)`；raw查询未注册-ENOENT/未执行-EAGAIN，成功0，失败原始非零rc；percpu mask输出失败位、pending/unknown时置0，允许mask=NULL。require将legacy正rc转-EIO，raw查询不丢原值。

根 `test-boot-failure-host CASE=subsys|log|mounts|terminal|stdio|spawn|task-init|all`，默认all；各CASE只构建运行自身binary，未来接口/其它组缺失不得阻断当前行为RED。subsys_entry增加raw_result；percpu entry增加raw_result/failed_mask，initialized仍0/1/-1。注册名称必须在整个启动期保持有效，latch保存安全静态名称（null/empty用固定占位）；不引入字符串堆分配。

- [ ] **Step 1 — 写RED与准备声明。** 先添加所需新声明/内部字段以便编译，不实现行为；subsys测试先只调用旧register/init/status API，mock掉logger和noreturn fatal边界，验证required失败后哨兵仍运行的旧行为RED、optional status不为成功、percpu失败不标成功。独立追加duplicate/invalid/full64与16表/吞错wrapper/latch优先/late注册/重复dispatch/正rc及percpu部分mask；新API缺失链接失败单列。新log binary链接真实boot_failure.c和真实ARM runtime/log_impl.c，仅替换UART/kputs/kputu/kputx与IRQ/halt，捕获完整行，验证INT64_MIN、负rc、phase/cpu=-1、mixed字符串和WARN，不把logger换成stub。默认fatal在host由第一次halt以noreturn测试陷阱跳出，证明关IRQ→格式完整→halt，非mock整个stop函数。
- [ ] **Step 2 — 独立RED接线。** 先接根CASE=subsys，只构建旧API行为子集，运行 `make PROFILE=x86_64-clang test-boot-failure-host CASE=subsys`，保存required未阻断的断言RED；再接新API用例并单列链接失败。log组在生产函数最小空stub可载入后运行CASE=log记录输出/次序断言RED。hosttests/mk/sched.mk注册两个binary为sched/protocol-or-legacy，私有boot3-*对象和depfiles；根未知CASE必须非零，不能原样透传未经校验的目标。
- [ ] **Step 3 — 实现共同屏障。** register验证/同表unique/已知flags，返回-EINVAL/-EEXIST/-ENOSPC并锁存首错，latch后拒绝新注册；不覆盖旧entry。共同iterator逐项检查callback和latch，null条目fatal，优先latch首因（无latch的callback非零用component=initcall并额外index诊断）。callback按原链接顺序，立即停止下一项。首次phase调用封BSP表，percpu首次dispatch封percpu表；所有phase/dispatch前及callback后查latch。required非零调用stop；optional调用warn并保存原值，重复不重试；percpu部分失败完成后initialized=-1。共同helper不拿锁调用callback。两架构void arch_register_subsys改调用共同iterator；所有int wrapper真实返回register结果，multi-register首错立即return，APIC注册不提前kpanic吞掉统一归因。void arch_register_subsys_percpu保留接口，若register失败可直接stop或由随后dispatch的latch先检屏障捕获，不允许先执行任一percpu callback。保留APIC init/DEVICE_UNSAFE等低层不返回fatal。ARM显式加core/boot_failure.c，新增optional诊断经warn helper避免%d格式。
- [ ] **Step 4 — GREEN/兼容性。** 跑subsys/log CASE，补host两架构adapter调用共同helper的实际链接或精确接线契约；验证ARM formatter真实输出不是字面%d且参数不移位。`make PROFILE=x86_64-clang test-host`、test-static、image；`make PROFILE=aarch64-clang aarch64-uefi`和MODE=smp。核对BOOT-2 phase3/4/5/6时机与late percpu注册不变，现有mock/include声明适配新字段但不修改共享ABI。
- [ ] **Step 5 — Commit。** `fix(boot): enforce subsystem failure barriers and registration integrity`。

## Task 2: 根挂载、TTY/PTY必需能力与可选挂载降级

**Files:** FS/终端行、kernel/core/main.c；mounts/terminal host与既有test_device_boot/mocks及fs mk。

**Interfaces:** `fs_boot_prepare()` int和`fs_boot_mounts()` void保留；内部root helper改返回int实际rc，必需错误用stop。`int tmpfs_init(void)`、`int procfs_init(void)`返回实际挂载结果；register_fs的-EEXIST在已登记类型时允许继续mount。`tty_boot_init()` int、`pty_init()` int保留，`int tty_boot_register_devfs(tty_t *console)`以显式非NULL对象注册（头内前向声明按真实tty_t定义选include，不能造不兼容tag）。main按stage=fs/tty/pty检查非零，默认用户态能力不再仅log后继续。

- [ ] **Step 1 — RED。** 真实fs_boot.c（沿用test_device_boot的真实boot对象边界）、tty/boot.c、pty.c host对象：root无disk/GPT不足/根mount负rc，断言无tmp/proc/后续TTY；/boot失败有效root仍继续；tmp失败仍尝试proc、proc失败仍返回有效root。TTY分配NULL不得register/bind，tty/tty0 -EEXIST/-ENOMEM都失败；第一次节点已发布、第二失败不free console、不绑定全局console。PTY ptmx冲突失败；不能只mock tty_boot_init返回值自证main。既有root fatal已正确用例记录GREEN，新的nullTTY/PTY冲突与吞错挂载需行为RED。
- [ ] **Step 2 — 确认RED。** 分别跑根CASE=mounts/terminal，组内只链接当前已有接口；先保存旧tty_alloc NULL仍注册节点、PTY EEXIST仍成功的行为断言。变更tmp/proc返回签名前可以先用现有void旧调用测试吞错诊断；新返回接口的声明/旧mock调整与行为RED分别记录。同步更新旧test_device_boot的fatal捕获从kpanic到stop边界，但其正确root/fallback测试不放松。
- [ ] **Step 3 — 实现。** root helper返回明确无盘/分区错误或实际mount rc，fs_boot_mounts停止于root必需失败，保留GPT/whole-FAT分支与/boot warn。tmp/proc注册/挂载返回码上送warn，类型已由vfs_init登记的正常EEXIST可接受。TTY先alloc，有效console传入注册helper，两节点都成功后再serial/keyboard/tty_set_dev_tty绑定；冲突不被视为成功，partial已发布对象保留。PTY注册返回EEXIST，不提供覆盖/动态注销。main必需失败直接stop；不改鼠标依赖检查或optional键盘/PCI probe政策。
- [ ] **Step 4 — GREEN。** focused mounts/terminal、test-host、x86 phase-0/inittab-phase/network，验证串口TTY不依赖keyboard硬件成功；检查所有lookup/partial对象引用与未使用free路径。ARM编译不拉入fs/tty依赖，默认HAS_VFS=0/HAS_USERSPACE=0照旧。main源码/host调用序列证明必需失败后没有SMP/PID1路径。
- [ ] **Step 5 — Commit。** `fix(boot): require root and terminal capabilities before userspace`。

## Task 3: 私有标准fd事务与初始spawn发布屏障

**Files:** boot_stdio源/头、x86 cpu/sched_boot.c、sched/exec.c；stdio/spawn/task-init三个host binary，相关file/lifecycle mocks与mk。

**Interfaces:** `int sched_boot_prepare_stdio(task_t *idle, const char **failed_component)`：有效显式目标且原files=NULL；成功一次附表返回0，失败不附表、输出静态component串（files/tty-node/file-0..2/fd-0..2/target）；函数不调用fatal。task_init收到负值以stage=stdio stop。三fd分别O_RDWR/O_WRONLY/O_WRONLY，目标soft limit；lookup独立引用在所有路径put；安装成功slot持有file，失败caller持有，私有表用files_unpin。spawn_user_task原int64接口不改，parent有files且dup失败return-ENOMEM且不publish；task_init负spawn立即stop，无boot probe/net/sched_start/thermal调用。

- [ ] **Step 1 — RED事务/继承/接线。** stdio host链接真实boot_stdio.c和真实file.c的分配/安装/引用函数（私有对象、ffunction-sections/gc-sections，替换外部网络/硬件边界），逐一失败files_alloc/tty lookup/三次file_alloc/三次fd_alloc_limit；精确验证引用平衡、目标未变、未错误覆盖已有files、soft<3返回EMFILE。exec host必须编译真实exec.c::spawn_user_task；外部VFS返回合法ELF节点、机器/alloc/mm边界受控，files_dup失败时检查-ENOMEM、lookup/raw/thread/mm释放、无task-list/enqueue/notify/user_init记录；原parent files=NULL语义不因新检查改变，不能只stub spawn。task-init host另编译真实x86 sched_boot.c，用arch/MM/stdio/spawn/net等边界追踪：stdio失败不spawn、spawn负返回不触发boot probe/net/sched_start/thermal，成功spawn=1到net边界后以noreturn测试陷阱结束，避免执行hlt循环。
- [ ] **Step 2 — 独立RED。** CASE=spawn先针对旧真实exec对象记录files_dup NULL仍走publish的行为RED；其后续VMM/ELF等边界用可完成的合法准备stubs，不能因无关映射stub先失败使测试无效。CASE=task-init先保持旧stdio代码/边界，捕获旧spawn负返回仍调用net的行为RED；CASE=stdio新声明+最小stub可加载后记录返回/引用断言，不以缺文件替代行为失败。三个binary分别注册，任务未实现的其它CASE不作前置项。
- [ ] **Step 3 — 实现。** boot_stdio在私有表安装准确0/1/2，仅完整成功附给显式idle；所有error分支按所有权drop refs，不直接files_free(ref1表)，不持锁drop。替换task_init原静默stdio块为helper+stop；释放原查找tty独立引用。exec的files_dup检查放在现有首次继承点后、FPU/map准备和PUBLISH之前，失败释放当前已分配私有对象并return-ENOMEM，不回滚PID、不重试、不改变其它发布逻辑。task_init检查spawn<0并stop，然后正常打印init-created pid=1。正返回!=1可fail invariant，但注明任务可能已发布，不能unpublish或将其描述为发布前屏障。保留其它服务顺序和VFS bootstrap时机。
- [ ] **Step 4 — GREEN。** 三CASE、test-host、files引用/NOFILE与process lifecycle回归、BOOT-1/2 host入口；真实x86单/双核phase-0、syscall与PID1 selftest（分开配置）。确认低内存继承失败无新task-list节点，成功AP可在kernel init-created前打印PID1；不引入虚假串口全序。root/newAPI签名更新后的调用端包括fixture与host mock均一致。
- [ ] **Step 5 — Commit。** `fix(sched): fail initial userspace startup on stdio inheritance errors`。

## Task 4: 隔离启动失败矩阵与严格分阶段runner

**Files:** fixture/runner、main/关键分配与注册/挂载边界的测试接线；构建/登记/CI行；test_boot_failure Python模块和既有枚举/隔离/CI约束测试。

**Interfaces:** selector `BOOT_FAILURE_TEST=none|required-subsys|optional-subsys|registration|root|tty|pty|stdio|init|child-fds`，默认空/none。有效选择派生 `boot-failure-<case>` kernel/image及单独受控宏；x86以外拒绝这些用户态fixture（ARM共享框架由host/回归验证）。root在受控子make传宏并按有效宏而非仅顶层变量派生隔离路径；拒绝混用BOOT-1/2/X86_INTR/其它fault、selftest/systest/nettest等模式，KERNEL_SELFTEST传空。runner CLI `--case <normal或上述case> --cpus N --firmware PATH --image PATH --kernel-elf PATH --qemu PATH --build-dir PATH --profile x86_64-clang --timeout 90`，纯函数 `check_evidence(text: str, case: str, cpus: int) -> tuple[bool, list[str]]`；reuse ProcessSession/RunArchive/RunReport。

- [ ] **Step 1 — RED parser/process/build。** normal成功日志不含fixture身份/注入/能力哨兵，出现任一这些fault记录即拒绝；只使用生产init-created、PID1/shell成功链与归档ELF/image hash证明产物身份。optional及各fatal case另用含variant/injected的合成日志；缺失/重复/截断/错case/component/rc/phase/cpu、fatal后哨兵、错误BOOt1 complete禁令、PID1先于init-created、stale前一组证据、silent/timeout/自行退出均有负例。正常partial order接受跨核用户PID1早于kernel init-created；fatal后最少1秒观察且runner自停。build契约覆盖错误selector、aarch64拒绝、有效宏绕过top变量仍隔离、冲突旗拒绝、normal ELF无fixture符号/身份串、normal镜像hash不变；模块可导入后以具体行为失败/NotImplemented记录RED。
- [ ] **Step 2 — 注入与观测接口。** fixture只提供target site故障决策与哨兵，不直接调用stop、不对已成功spawn改返回。`bool boot3_test_fail_site(const char *site)`匹配当前case且一次命中，记录 `[boot-3-test] injected=<case> site=<site>`；`const char *boot3_test_init_path(void)`仅init case给不存在ELF路径，否则/bin/init；`void boot3_test_stdio_enter(void)`/`void boot3_test_stdio_leave(void)`围住stdio helper的私有构造区（每个出口先leave再drop refs），file_alloc仅在该window第二次调用的calloc边界注入NULL；`void boot3_test_spawn_enter(void)`/`void boot3_test_spawn_leave(void)`在初始spawn调用边界限定child-fds window，file.c仅在该window给files_dup的calloc注入NULL。case身份 `[boot-3-test] variant=boot-failure-<case>` 在registration前early串口就绪后打印。新增模块接入TEST_HARNESS_MODULES、root suite/白名单/help、catalog/scenarios；先跑Python模块保存行为RED。
- [ ] **Step 3 — 实现真实故障点。** required/optional专用phase5 callback返回-EIO且在同一注册callback顺序注册后哨兵，名字boot3-required/boot3-optional；registration callback注册同表boot3-duplicate两次，iterator检测首错-EEXIST且不进phase3。root仅给vfs_mount_fs目标/的实际调用返回-EIO；tty在tty_alloc的calloc边界给NULL；pty只给ptmx的devfs登记返回-ENOMEM；stdio只给boot_stdio第二次file_alloc的calloc返回NULL；init真实spawn缺文件，当前rc=-1保留；child-fds在真实files_dup calloc失败，stage=init/component=/bin/init/rc=-ENOMEM。每个注入必须实际命中一次，已有allocator失败/返回处理执行，不影响普通image或其它分配。partial资源策略按spec。隔离哨兵记录root-ready、terminal-ready、stdio-ready，以及spawn后服务入口（probe/net/BSP sched_start/thermal）是否被到达；optional后哨兵读真实status=-1/raw=-EIO才输出继续。普通image只新增init-created成功记录，哨兵不编入。
- [ ] **Step 4 — GREEN/17组与归档。** 根 `make PROFILE=x86_64-clang test-qemu SUITE=boot-failure`串行构建/执行normal1/2/4，required1/2，registration1，root1，tty1，pty1，stdio1/2，init1/2，optional1/2，child-fds1/2。先normal image取hash，fault用私有镜像/normal用snapshot；配置virtio-rng、90秒单组，CI whole matrix预算≥30分钟。registration必须无phase3/context；required无自身后哨兵/root；root/tty/pty无后续SMP complete/PID1；stdio/init/child-fds允许先前BOOT-1 complete但无init-created/PID1/后续服务哨兵。fatal元数据精确component和rc，只有percpu故障才cpu>=0（本矩阵没有percpu QEMU case，其行为由host负责）。optional有真实失败status/raw、后哨兵和root/terminal/stdio能力哨兵，再有init-created/PID1/shell；normal只要求生产成功链和ELF/image hash，明确禁止fault身份/注入/fixture哨兵。仅故障变体要求唯一variant身份/注入证据，所有组都要完整日志和RunReport。运行module、test-harness、catalog/scenario --check、normal ELF/镜像隔离审计，不能以lack-of-log或timeout替代停止证据。
- [ ] **Step 5 — Commit。** `test(boot): exercise required and optional startup failure policies`。

## Task 5: 双架构验收与文档收尾

**Files:** 文档责任行及实际gate/CI修订。报告路径 `docs/superpowers/reports/2026-11-08-boot-3-validation.md`。

**Interfaces:** 消费前四任务；报告列spec各条能力/错误路径→代码/测试/原始证据，原始与最终commit、命令/return code/serial/result路径及未覆盖项。

- [ ] **Step 1 — 自审覆盖。** 注册所有wrapper的真实返回与首错latch；required/optional/percpu状态和重复行为；ARM真实signed记录；root与三种optional mount；TTY/PTY冲突/partial资源；stdio全失败点及child继承；task_init负返回不继续。确认没把clocksource inactive、PCI普通probe或ARM无VFS当fatal，BOOT-1/2调用顺序不变。
- [ ] **Step 2 — 最终门禁。** `make PROFILE=x86_64-clang test-host`、test-static、test-harness、test-qemu SUITE=boot-failure/boot-lifecycle/boot-irq-time；QEMU_SMP=1/2分别phase-0、inittab-phase、network；`make PROFILE=x86_64-clang OS01_SYSTEST=1 KERNEL_SELFTEST= test-syscall`，另跑 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`。ARM aarch64-uefi、MODE=smp/no-ack/m3-probe/sched-ap/sched-wake/sched-tick（no-ack按既有显式注入build，后恢复NO_ACK=0），catalog/scenario检查；记录完整命令不把环境缺失当PASS。
- [ ] **Step 3 — 写验收与契约。** 更新subsys.md注册完整性/optional/percpu/void fatal语义，boot/fs/sched文档记录root/TTY/PTY/stdin继承政策；arch.md记录ARM logger/fatal的%s边界；build.md新增focused CASE与17组套件。说明低层非返回fatal仍保留原协议、无全局stop-world、无重试/热注册、partial节点只保留无注销。
- [ ] **Step 4 — 状态与差异检查。** `git diff --check`、本地链接/新增源头对称/测试登记/最终证据一致性。只有全部实际门禁完成才关闭BOOT-3、更新changelog/roadmap；文档评审本轮只标已批准待实施。BOOT-4/5仍未完成，不改其它任务成果归属。
- [ ] **Step 5 — Commit。** `docs(boot): record BOOT-3 failure contracts and validation`。不自动合入master或开始BOOT-4。

## 本轮交付边界

只写spec、再写plan并经独立子agent评审到APPROVE。以上步骤均未执行，生产实现和QEMU验收需后续授权实施；本轮检查仅证明文档一致性，不声称代码修复完成。
