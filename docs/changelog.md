# 已完成工作汇总（Changelog）

> OS01 各阶段已完成工作的按时间汇总。最新在前（截至 2026-10-24）。
> 本表为历史完成记录，规划项见 `docs/roadmap.md`。

---

## 2026-10-25

- feat(intr): **x86 当前安装入口第二期闭环**：20 条异常 + 28 条 IRQ 接入共同事件分派，x86 原生适配与中立注册表分离；保留 gate/返回与设备 ACK、timer/IPI EOI 归属。修复连续 gate 的 IST 编码约束、按 MADT type-4 配置 NMI，并在 AP descriptor/IST/TR 就绪后启用 NMI。新增 48-gate 运行时核对、真实 timer/TLB/resched、软件 `int 0xff` 恢复及隔离 `int3`/post-percpu QMP NMI 验收。矩阵、既有 `fd_refcount` 失败与证据边界见 [验证报告](superpowers/reports/2026-10-25-intr-dispatch-phase2-x86-validation.md)；ARM 语义、M4、上下文切换与统一 `kernel_main` 不计入本次完成。

## 2026-10-24

- fix(qemutests): **dispatch 第一期遗留 shootdown-probe 验收闭环**：runner 及 fixture 从旧 `M3-SHOOTDOWN-PROBE:` 对齐到内核实际 `SHOOTDOWN-PROBE:`，删除未使用的旧前缀常量。新增独立正反例及临时编译真实 probe 函数体的生产者协议回归；旧 runner 五次预期失败，修复后聚焦 11 项、完整 AArch64 harness 56 项全通过。正式 `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` 退出 0、START/OK 各一次、无 probe SKIP/FAIL；不改内核实现，保留原阶段一历史失败。详见 [调查与修复证据](debug/2026-10-24-shootdown-probe-investigation.md#修复与正式验收)。

- fix(build): **parked fbtest 私有头发布边界闭环**：测试 sysroot 原先同时发布内核 `driver/fb_test.h` 和测试 UAPI；私有头改为始终排除，`uapi/fb_test.h` 仍仅在测试配置发布。新增真实安装规则回归，覆盖干净安装、旧头残留、配置切换、manifest 和 UAPI 内容，并接入 `test-host`。旧规则三个 subtest 失败；修复后生产/测试镜像构建与实际 sysroot 检查、完整分辨率宿主九组和 `test-host` 全部通过。详见 [调查与验证](debug/2026-10-24-fbtest-private-header.md)。

- fix(procfs): **parked monitor ④ legacy 字节缓冲区等长边界闭环**：`procfs_generate_node()` 只复制文本字节，却用 `len >= cap` 拒绝恰好装满的目标；改为 `len > cap` 并明确不补 NUL 的接口约定。四类统计节点新增长度减一/等长/长度加一、canary、失败无写入与分配余额回归；旧实现 8 条断言失败，修复后 procfs 13 场景/544 断言、完整 monitor 七组、`test-host` 和 QEMU phase-0 通过。原条目所述 4096 字节 status 空读在当前 formatter 下不可达，本次未声称 guest 复现。详见 [调查与验证](debug/2026-10-24-procfs-legacy-capacity.md)。

- fix(fbtest): **parked 文件 ioctl/release 冷调用锁初始化防护闭环**：正常启动已通过 bootstrap/reset 初始化锁，但两个文件回调缺少独立入口保障。各补一条 `fb_test_lock_ensure()`；新增三个独立子进程从未预热状态执行 register ARM、terminal ARM 和 release，观察真实回调的初始化/取锁顺序与幂等性。旧实现三项失败，修复后 hooks 13 场景/136 断言、完整分辨率宿主九组、`test-host` 及测试配置 QEMU SMP 1/2 共 26 项正常路径协议检查通过。未声称复现正常启动死锁，也未扩大为并发首次初始化协议。详见 [调查与验证](debug/2026-10-24-fbtest-cold-lock.md)。

- test(pty): **parked 并发 resize fixture 初态误报与停止标志同步闭环**：worker 启动前通过真实 ioctl 设置合法 A 模式，新增固定 reader-first 阶段；旧 fixture 在该阶段稳定报 `torn_count=1`。跨线程 stop 改为原子访问，writer 等待 reader 首次有效读取，结果在 join 后检查；补线程/ioctl 返回值、实际读写进度及异常四元组诊断。修复后 11 场景/163 断言、聚焦门禁连续 20/20、完整分辨率宿主九组及 `test-host` 通过；ThreadSanitizer 对照恢复 volatile stop 后报数据竞争，修复版全绿。仅宿主测试修改，不声称解释原始未采集四元组的那次失败。详见 [调查与验证](debug/2026-10-24-pty-resize-fixture.md)。

- fix(fbtest): **parked terminal ARM 校验对称性闭环**：`fb_test_arm_terminal()` 原先接受零 token 并覆盖已有 pending 请求。与 register ARM 对齐：零 token 返回 EINVAL，锁内检测已占用 terminal 槽并返回 EBUSY，保留原请求及累计消费计数；仅测试配置，无 ABI 布局变化。新增两项真实 hooks 回归，旧实现 13 条断言失败，修复后 124/124 通过；完整分辨率宿主九组通过，完整 host 重试通过，测试配置 QEMU SMP 1/2 各 13 项协议验证通过。首轮 host 的 PTY fixture 失败和 guest ash SIGSEGV 均如实保留并独立登记 parked，不声称已修。详见 [调查与验证](debug/2026-10-24-fbtest-terminal-arm-validation.md)。

- docs(roadmap): **parked `test_lvgl` 早退跳过 alt-screen 恢复条目归档**：调查确认这是历史迁移后遗留的旧项。`7cefb01a` 已删除 alt-screen 协议，`acebb9df` 已移除运行期直接早退并统一到 `cleanup → gui_platform_destroy()`；当前唯一直接返回 1 发生在平台创建前。平台错误/生命周期宿主测试及真实 LVGL 渲染、50 轮生命周期、OOM fatal 测试通过。本次不改业务代码，不声称修复另一个独立的 guest SIGSEGV parked 项。详见 [调查记录](debug/2026-10-24-lvgl-early-exit-archive.md)。

- fix(pty): **parked T6 termios 读写同步闭环**：`TCSETS`/`TCSETSW` 原先无锁提交，主/从 `TCGETS` 直接把共享 termios 传给用户拷贝，存在跨代混合读取。写端用户拷贝后持 `state_lock` 提交；读端持锁生成本地快照、解锁后用户拷贝，weak master fallback 同步修复。新增确定性交错、锁范围与用户指针失败回归：旧实现 18 条断言失败，修复后 PTY 11 场景/149 断言全部通过；隔离 weak 路径的旧版本另有 5 条失败，修复后全绿。完整分辨率宿主九组、`test-host` 及 QEMU phase-0 启动检查通过。详见 [调查与验证](debug/2026-10-24-pty-termios-state-lock.md)。

- test(monitor): **parked ⑧ PMM/accounting 锁内快照验证闭环**：调查确认生产实现正确；PMM no-op shadow 新增可选 acquire/release 观察，accounting shadow 补 release 观察。新 fixture 用真实 allocator/tick writer 识别保护锁，在加锁前、持锁时与解锁后注入不同计数，验证快照的锁身份、配对及数据复制范围。两个路径的缺锁/提前解锁/延迟加锁/错锁共八个临时错误版本均被拦住；PMM 6 项/59 断言、accounting 13 项/138 断言、完整 monitor 七组及 `test-host` 全部通过。详见 [调查与验证](debug/2026-10-24-monitor-snapshot-lock-observation.md)。

- test(monitor): **parked ⑦ CPU online 边界 fixture 闭环**：调查确认 `sched_accounting_read()` 对合法但未枚举 CPU 槽返回 `online=0`，范围内读取实际 per-CPU online 标志，生产实现无需修改。新增范围外、等于 `num_cpus`、最后合法槽及 online 标志切换覆盖，验证计数/epoch/频率不被读取重置；三个临时错误版本均被新增测试拦住。accounting 12 项/120 断言、完整 monitor 七组及 `test-host` 全部通过。详见 [调查与验证](debug/2026-10-24-accounting-online-boundary.md)。

- fix(test-harness): **parked terminal-present 提前失败异常闭环**：`_run_terminal_present()` 的空闲窗口指标原先在采样后才赋值，baseline log 缺失、manifest 缺失/损坏及 QEMU 启动失败分支却提前引用，导致 `UnboundLocalError` 覆盖原始诊断。入口初始化为空记录与零时长，提前失败正常写出 JSON 并返回失败，不放宽 baseline 或性能门槛。新增真实驱动入口回归覆盖四种错误场景，旧实现四处复现，修复后 terminal-present 28 项通过，完整 `test-harness` 613 项、0 失败、1 跳过；根 Make 入口缺失 baseline 负向验证按预期失败并归档原因。未执行 guest 像素/性能验收。详见 [调查与验证](debug/2026-10-24-terminal-present-early-failure.md)。

- feat(intr): **中断/异常分派统一第一期主体闭环**（六条入口与其 gate 全部通过；`MODE=m3-probe` 一项既有失败未闭环）——分支 `feature/intr-dispatch-phase1`，基线 `abb89b03`；在 `master` 之上 8 个实现 commit `a20e10ee`/`fed4028b`/`5e085f34`/`4eff2550`/`6592b684`/`935213ca`/`204dfa68`/`f4a2bc04`，另有 spec/plan 文档 commit：新增 arch-neutral 无状态事件分派 `kernel/intr/dispatch.c::arch_intr_dispatch()` 与事件契约 `kernel/include/intr/dispatch.h`（`kind`/`origin`/`reason`/`access` + `fault_addr_valid`/`fault_addr` + `native_vector`/`native_status`/`native_address` 原始快照），每架构提供强定义后端 `arch_intr_handle_sync`/`arch_intr_handle_irq`/`arch_intr_bad_event`（无 weak no-op，缺后端即链接失败）。**已统一的六条入口**：aarch64 EL1h sync、aarch64 EL1h IRQ、x86 #DE/#UD/#GP/#PF。四种控制流（正常返回、故障恢复、用户任务终止、内核致命停机）语义不变，aarch64 原 `[aarch64-sync] FATAL` 字段/顺序/首报 CAS 与 `far=n/a` 逐字保持；x86 IRQ 后端 fail-closed（生产无调用者）。真实 guest 非局部恢复证据：内核 selftest 的 `uaccess` 用例在真实 #PF → 薄入口 → `arch_intr_dispatch()` → x86 私有 #PF handler → `__builtin_longjmp` 路径上恢复，输出精确标记 `[selftest] uaccess: dispatch longjmp restore OK`（恰一次）。**验收**：双架构构建 exit 0；x86 `test-host`（134 binary）/ `test-static` / `test-harness`（678 tests, OK, skipped=1）/ `test-qemu SUITE=phase-0` / `OS01_SYSTEST=1 test-syscall`（99 passed, 0 failed, 1 skipped）/ `KERNEL_SELFTEST=1 test-kernel-selftest`（39/39）全部退出码 0；用户信号 case 按名核对：`54b_fault_divzero`→SIGFPE(0x8)、`54c_fault_write_ro`→SIGSEGV(0xb)、`54d_fault_ud2`→SIGILL(0x4)、`54e_fault_gp`→SIGSEGV(0xb) 且父任务存活；COW/mmap case 全 PASS。aarch64：`MODE=sync-fault` 原字段 + 首报 + 无 returned（PASS）、`MODE=smp` 1/2/4 CPU × 3 次 9/9 PASS、`MODE=gic-spi` PASS、`MODE=m3-probe` **失败**（见下）、`test-aarch64-audit` `[audit] OK`；调用链审计对 x86 与 aarch64 两个真实 ELF 均判定 `OK`（四符号强定义唯一、入口确实经过共同函数、x86 IRQ fail-closed、#PF 链只读一次 CR2）。**已知失败（如实记录，未修）**：`make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` 超时失败——内核 `shootdown_probe.h` 打印的 wire 标记是 `SHOOTDOWN-PROBE:`，而 runner `qemutests/aarch64_m3_probe.py` 仍匹配 `M3-SHOOTDOWN-PROBE:`；该改名来自 `045cba3f`（2026-10-05，M 前缀清理），该 commit 是 `master` 与本阶段基线 `abb89b03` 的祖先，**属既有 test 侧命名漂移，不是本阶段的回归**；临时把 runner 前缀对齐到内核实际标记后同一配置重跑即 PASS（探针本身 `START`→`OK` 正常），随后已把 runner 还原，未在本次降低断言。**范围**：其余 x86 异常与 x86 IRQ 入口、EL0/SVC、aarch64 可恢复 abort/uaccess、用户页表、ASID/COW、上下文切换、统一 SMP、统一 `kernel_main` 仍待实施，Spec A 与 M4 状态不变。设计 [spec](superpowers/specs/2026-10-23-intr-dispatch-phase1-design.md)，实施 [plan](superpowers/plans/2026-10-23-intr-dispatch-phase1.md)，验证 [验证报告](superpowers/reports/2026-10-24-intr-dispatch-phase1-validation.md)。

## 2026-10-23

- fix(monitor): **parked monitor #14：monitor 帧被串口诊断输出撕裂闭环**：monitor 的 stdout 是 **PTY 从端**（`config/inittab` 只有 `tty1:respawn:/bin/terminal`，terminal 应用 `dup2(slave, 0/1/2)` 后 fork ash），字节由 terminal 应用按**读块**转发到 `/dev/serial`；而渲染器的 `tw_putc` 用 `fprintf("%c")` **逐字节**写，OS01 的 libc 无缓冲——标题 `OS01 monitor` 成了 12 次独立 `write()` 进 PTY 环，被中继读成若干小块逐块转发，terminal 自己的诊断记录（同一端口的独立写）插在两次转发之间就把标题撕成 `OS01 mo` + `nitor`；`parse_frames` 只认整行，于是该帧整个丢失，2 核用例等 `dual-core monitor frames` 至总截止。guest 侧改为**每行缓冲、一次 `fprintf("%s")` 冲刷**（整行一次进入 PTY 环，中继按整行转发，撕裂标题在结构上不再可能），512 字节行缓冲覆盖最长的进程行（comm 63 字节转义后约 300 字节）；runner 侧再剥离诊断记录（连同其行尾换行使残片重新接合，前缀按完整记录形态匹配）作为**端到端**保证——线上仍可能有行间撕裂（PTY 环接近满时 `pipe_write_internal` 分段提交，`fb.c` 的 `fb_write`、panic 路径仍逐字节写）。未改 libc、未改内核、未改 monitor 的输出内容与 ANSI 语义。红绿回归：host 侧要求每次写调用恰好承载一整行，旧实现 8 行输出用了 **147 次 write()**、plain/ANSI 各 116/139 次写全部不是整行（RED），修复后通过；另有用例钉死 512 字节上界（把缓冲临时改成 128 字节即报 `cap_split_calls=2`，RED）；runner 侧撕裂标题用例旧实现 `0 != 1`（RED），修复后 23 项通过，另有用例把剥离用的记录形态对着发射源码钉死，防止发射方改形态后剥离静默失配。官方 QEMU monitor 矩阵 **1/2/8 核全 PASS**（此前 2 核超时）；2 核 transcript 里确实混入 18 条 `[term-present]` + 31 条 `[term-probe]`，而 16 个 `OS01 monitor` 标题全部完整。2 核仍出现一次 spin 归属迁移重试（parked #6 的绑核验证强度缺口，与本项无关）。详见 [调查与验证](debug/2026-10-23-monitor-frame-atomicity.md)。

- refactor(vmm): **ARCH-7 移除 `#define mmap uint64_t*` 类型宏**：`kernel/include/memory/vmm.h` 改为 `typedef uint64_t *ptbl_t;`，删除全部 11 处绕行块——`vfs.h`/`devfs.h` 的 `#ifdef mmap`+save/restore 对、`vma.c`/`fb.c` 的就地 `#undef`/`#define` 对、`devfs.c` 的 `#undef mmap`，以及 6 处 hosttests `#undef mmap`（含一处 undef 了从未定义过的 `munmap`）。这些块存在的唯一原因是该宏占用了 `mmap` 这个标识符。类型用途实为 8 处：`kernel_map`（x86 与 aarch64 各一份定义）、`vmm_alloc_map`、`vmm_free_user_map`、`get_next_level`，以及 aarch64 `vmm_backend.c` 的定义与赋值。选 `ptbl_t` 而非 roadmap 原建议的 `pgd_t`，因为该宏同时覆盖页表根与中间级表指针，`pgd_t` 会把 `get_next_level` 的返回值标错。`int (*mmap)(struct vfs_node *, struct vma *)` 字段名保持不变：结构体成员名与普通标识符分属不同命名空间，不与 libc `mmap` 冲突。纯编译期重构，无 codegen 差异（8 处类型用法全是单声明符/返回类型/单参数/转型位置，不存在 `mmap a, b;` 这种多声明符下 `b` 会静默退化为标量的隐患）。验证：`make test-host` 全绿、全量 `make` exit 0、`test-qemu SUITE=phase-0` 启动到 BusyBox shell、`test-qemu SUITE=systest` 98/98 syscall 测试通过（覆盖 mmap/mprotect/munmap、fork-mmap、COW、prot_ranges，即本次触及的 `do_mmap` 设备分支）。过程中编译器抓出一处 grep（`| head -30` 截断）漏掉的 `kernel/arch/aarch64/memory/vmm_backend.c`——删类型宏应以编译器而非 grep 收尾。

- fix(sched): **parked monitor #1：缺少 idle 身份时跳过记账**：正常 BSP/AP 启动均先设置 idle 再发布 scheduler_ok，尚无正常启动路径复现；注入 idle=NULL 后，原生产函数会将全部样本计 busy，甚至给真实 idle 任务增加 runtime。现在在任何记账前丢弃无法分类的样本，保留原计数、epoch 和 tick rate；idle 恢复后继续原差分基线。新增真实 accounting.c 回归覆盖 busy/idle/NULL current、既有非零计数、恢复后计数与任务 runtime、无锁和无分配快路径；旧实现 7 条断言失败，修复后 84 条全部通过，完整 monitor/host 和独立 ASan/UBSan 验证通过。QEMU 1/8 核通过，2 核因独立串口帧交错超时，另记 parked #14，未将整组标为通过。详见 [调查与验证](debug/2026-10-23-accounting-missing-idle.md)。

- fix(build): **parked monitor #13：宿主测试分发门禁闭环**：`test-monitor-host` 原先在依赖运行后才检查 rootfs 能力，未知 CASE 又先触发缺失依赖，既有报错分支不可达。现在仅为支持 rootfs 且 CASE 合法的调用生成前置任务；无能力 profile 或非法 CASE 在任何递归 make 前失败，并列明合法值。白名单精确匹配单个 CASE，空值/纯空白仍执行完整七组。新增真实根 Make + 记录型 submake 回归，覆盖 `-n`/`-j4`、未知/通配符/多 CASE、七个合法分发及无关目标，接入 `test-harness` CI 路径。旧实现 27 个组合失败，修复后新增回归和真实七组 monitor 测试通过；完整 harness 607 项、0 失败、1 跳过，独立评审通过。详见 [调查与验证](debug/2026-10-23-monitor-host-dispatch.md)。

- fix(resolution): **parked `_client_estale_case` 确定性失败闭环**：后台 `test_lvgl wait` 的 stdin 被无作业控制的 ash 接到 `/dev/null`，客户端在切换分辨率前即 EOF 正常退出；即使保持存活，静态展示页也不会周期性 PRESENT。新增专用 `test_lvgl --watch-display`，不消费 stdin，以 250ms 轮询间隔重绘并走真实 PRESENT/ESTALE 清理路径；交互 wait、smoke 和默认模式保留。runner 改用观察模式，启动窗口过滤旧 ready/exit，并拒绝已观察到的提前退出。34 项 runner 单测、真实 QEMU ESTALE 回归和完整 production/fault resolution 套件及完整 host 回归通过；真实 guest 的原有三种模式均 exit 0。详见 [调查与验证](debug/2026-10-23-resolution-client-estale.md)。

- fix(monitor): **parked monitor #10：短读覆盖与零频率校验闭环**：真实 collector 的分块累积逻辑正确，但 `/proc/stat` 解析器会接受 `TickHz: 0`，导致非法 CPU 样本进入模型。补充正值校验，其他正频率继续接受；新增 1/2/7/31 字节分块读取、跨 read 的 Name 转义、复用 sample、四类文件 cap−1/cap/cap+1 边界与指标隔离覆盖。超长 fixture 使用可解析文本加空行，确保静默截断无法蒙混通过。旧实现零频率用例两条断言失败，修复后 collect 12 项/1278 断言、完整 monitor/host、用户镜像构建及独立 ASan/UBSan 检查通过。详见 [调查与验证](debug/2026-10-23-monitor-collect-short-reads.md)。

- fix(monitor): **parked monitor #9：OOM 历史时间轴闭环**：采样循环在模型分配失败后仍计入该轮并渲染，但 model 原先直接返回而不追加历史点，压缩 60 槽时间轴。两处分配失败统一追加 CPU/内存无效空点；上一轮已提交的显示快照和差分基线保持原样，第二次失败仍释放首次临时分配。明确更新 model 接口及 spec §7 的 OOM 约定，区分数据回滚与历史记账。新增两个确定性故障注入回归，覆盖 62 次连续失败、环回绕、旧快照/基线字节不变、恢复后按 63 秒差分和分配余额归零。旧实现两项回归失败；最终 model 14 项/1164 断言、完整 monitor/host、用户镜像构建通过，独立 ASan/UBSan 检查通过。详见 [调查与验证](debug/2026-10-23-monitor-oom-history.md)。

- fix(x86): **`54c_user_fault_write_ro` 偶发 status=0 闭环**：异常 helper 在 `do_exit()` 之前提前写 `TASK_ZOMBIE`；#PF trap gate 保留 IF，定时器可将尚未完成清理的任务切出，父进程在 `on_cpu=0` 后提前回收其活动内核栈、读到默认退出码 0。删除 helper 的提前状态发布，由 `do_exit()` 在清理及 exit_code 写入后统一发布。临时主动调度探针红绿验证后移除；持久回归使用大惰性只读 VMA 延长 teardown 并重复 8 次，旧内核复现 6 PASS / 2 FAIL（waited 为目标 PID、errno=0、status=0），修复后通过。官方单 case 在 1/2/4/8 CPU 全通过（32/32 次），真实 terminal 五轮每轮 349 passed / 0 failed；完整 host 与静态检查通过。详见 [退出发布竞态调查](debug/2026-10-23-user-fault-zombie-publication.md)。

- fix(qemutests): **x86 `test-syscall-repeat` stage 0 停滞闭环**：systest 的 COW TTY case 等 `[COW TTY READY 0..3]` 后四字节输入，repeat runner 只等完成标记，从未回复。新增按事件回复精确 `COW!` 的 stage 驱动；累积每次 ProcessSession 消费的输出片段，保留循环内前几轮 RESULT。正常 terminal/ash、1/1/3 轮命令、严格结果门禁、私有镜像、总截止均保留，无内核/TTY/user 改动。旧 runner SMP=2 在 READY 0 后 180 秒超时；新 runner 三次完整五轮通过（含根 make 目标和 SMP=4），另一次正确拦下独立 `54c_user_fault_write_ro` 失败。27 项 repeat runner 单测通过，含逐字节 READY/RESULT、20 次无换行握手、陈旧事件、失败结果与超时覆盖。原 M3 豁免中的 stage0 阻塞解除；54c 偶发另列 parked。详见 [调查与验证](debug/2026-10-23-systest-repeat-handshake.md)。

- fix(sync): **调查并处理 parked `kernel_mutex` 负载敏感问题**：确定性 host 时序复现生产 mutex 在 CAS 失败后、入队前 unlock 时丢失唤醒（owner=0，等待者仍睡眠）。统一普通/interruptible 获取为关 IRQ 的 arm→CAS 重查→schedule 协议，退出时持 waitqueue 锁 disarm，信号取消转交可能消费的 wake-one。真实 `mutex.c` + `wait.c` host 回归覆盖入队前/后 unlock、信号、取消后的下一位等待者与 IRQ 恢复。内核自测改为 pid 返回值创建、等待两个 worker 在最终 unlock 后发布完成，再读受锁保护计数；使用 500 jiffies 截止替代固定次数循环，并在临界区主动让出 CPU 以覆盖竞争。历史 parked 超时缺少状态快照，不能排他归因于此次发现的丢失唤醒。验证与限制见 [调查记录](debug/2026-10-23-kernel-mutex.md)。

## 2026-10-22

- fix(selftest): **`test_tty_vintr` parked 超时根因闭环**：late test 的观察方是 BSP idle；收到信号的 RUNNING 工作线程在等待 release 时反复 `schedule()`，同 CPU 上会饿死 idle。改为受 `task_list_lock` 保护的不可中断睡眠，cleanup 显式唤醒并等待恢复 `PF_KTHREAD`；新增固定同 CPU、跨 CPU、首次睡眠前提前清理覆盖，注入后主动切换稳定检验原死锁时序。修复前固定同 CPU 在 SMP=4 复现 `missing END line`，GDB 确认 worker `vintr_seen=1/release=0` 仍循环调度。修复后完整自测 SMP=4/8 各一次通过；2/4/8 CPU 各 3 次重复的 `tty_vintr` 9/9 PASS（完整套件其中 7 次仍因既有 `fd_refcount rc_-1` 失败）。43 项自测 runner 单测通过。10 月 19 日直接检查 signal/state 的改动仅缩短竞态窗口，旧归因由本次证据修正。roadmap 移除 VINTR 挂起项，保留独立 `kernel_mutex` 负载问题。详见 [根因与回归证据](debug/2026-10-22-tty-vintr-timeout.md)。

- feat(gui): **GUI-2 输入与焦点闭环并合入 `master`**（分支 `feat/gui-2-input-focus`，基线 `6126504a` 之上 41 个 commit，2026-10-23 00:01 以 merge commit `90d3caa3` 合入 `master`——`master` 同期另有独立提交 `13000cbe`，故为**合并**而非 fast-forward，唯一的冲突是 `docs/changelog.md`，两侧条目均保留）：把 `/dev/keyboard` 扫描码与 `/dev/mouse` 事件接入 LVGL indev，实现光标、修饰键、按下/释放、重复、Tab 导航与控件焦点，并用**独占物理输入租约**保证同一时刻只有一个读取者。设计 [spec](superpowers/specs/2026-10-20-gui-2-input-focus-design.md)，实施 [plan](superpowers/plans/2026-10-20-gui-2-input-focus.md)，验证 [验证报告](superpowers/reports/2026-10-22-gui-2-validation.md)。
  - **内核侧输入路由**（`kernel/driver/input_route.c`）：PS/2 驱动与所有输入字节读者之间的唯一通道。持有**物理键状态**（Caps Lock 锁存只在这里翻转一次，NORMAL 路径的翻译器只读快照）、**租约**（至多一个活跃租约，绑定一个 display view，随显示所有权获得与释放）与**路由决定**（一个完整扫描序列发布到 NORMAL 或租约的 ACTIVE 路径之一）。锁序 route → NORMAL queue/poll；`input_route_lease_live()` 只取 route 锁，可在显示锁下调用。
  - **file-aware devfs 钩子**：`struct devfs_ops` 追加 `read_file` / `poll_file` 两个可选钩子（追加在最后，不改动既有设备 ABI）。`read_file` 收到具体的 `file_t`，设备因此能分辨**哪一个**读者拥有物理键盘；`poll_file` 返回 0 是合法的「未就绪」，不会被升级成「永远就绪」；`read_file` 的错误是终态。设备的最终关闭（final close）在内核内部撤销租约，进程被 SIGKILL 时租约一定回收。
  - **UAPI**：`GFX_BIND_INPUT` (0x470A) / `GFX_UNBIND_INPUT` (0x470B) 与 32 字节的 `gfx_bind_input_t`（字段偏移由 `_Static_assert` 固定）。errno 表面：`EFAULT`/`EBADF`/`EINVAL`/`EACCES`/`ESTALE`/`EBUSY`/`EAGAIN`/`EIO`/`ENODEV`。ACTIVE 路径的带外错误只有 `EOVERFLOW`（**数据流有丢失**，或**键状态可能已分叉**，即跨 epoch 的序列被吞掉），在整个租约生命周期内粘住——应用必须交还租约并重新绑定才能恢复；字节流本身保持原始 Set 1 负载。
  - **用户态**（`libgui/include/gui/gui_input.h`）：`gui_input_create/destroy/get_pollfds/has_mouse/dispatch/set_key_handler/set_pointer_handler`。真实设备模式占用独占物理输入租约，合成输入模式只用于离屏平台。回调**先于**控件路径：回调返回 false 的事件才继续进 LVGL keypad。
  - **保留的 ABI 不动**：`mouse_event_t` 仍是 8 字节（`_Static_assert` 固定），键盘仍是 Set 1 原始扫描码负载，TTY `ISIG` 保持开启，隔离由内核路由层实现（不用 termios）。
  - **固定容量与限制**：FIFO 512 项；每轮 dispatch 最多 256 个键盘字节 / 64 个鼠标事件；每步最多投递 256 项。软件重复首次延迟 400 ms、之后 40 ms。Ctrl/Alt 从无到有取消旧活动键，释放后不恢复。指针 1 计数 = 1 像素；光标是程序绘制的 12×18 箭头、热点 (0,0)，不引入图片/字体依赖；垂直滚轮每刻度 32 px。**单活动键**：LVGL 控件路径一次只跟踪一个活动键，新键的 DOWN 先取消旧活动键，迟到的不匹配释放被拒绝。
  - **发布面不变**：已发布 sysroot 的 GUI 树仍**恰好**是 `{gui.h, gui_input.h, gui_platform.h}`，无 `lv_*` 类型、无私有桥接头、无测试注入接口。`libgui/Makefile::install` 只写暂存树，`mk/components/sysroot.mk` 是唯一写入者。
  - **构建/验证入口**：新增聚焦的宿主 CASE（`make PROFILE=x86_64-clang test-gui-host CASE=<name>`，`GUI_HOST_REGISTERED_CASES` 是唯一列表：platform / lvgl / devfs-input / input-frame / input-route / input-decode / input-nonce / input-platform / input-lvgl）、`make PROFILE=x86_64-clang test-gui-incremental`（Task 9 的增量/sysroot 契约）与 GUI QEMU suite 的 5 个新场景（`input_interaction` / `input_isolation` / `input_crash` / `input_owner` / `input_overflow`）。所有测试仍只从仓库根 make 入口运行。
  - **验证结果**（干净回归，`make clean` 先行）：**最终树**（`4d3a2019` + `77cbe65` 之后）的合并前门禁七条命令**退出码全 0**——`test-gui-host CASE=all`（10 个 GUI 宿主二进制全 PASS）/ `test-host`（128 binary）/ `test-static`（13 项审计）/ `test-harness`（**Ran 594 tests**，OK，skipped=1）/ `test-qemu SUITE=gui`（**SMP 1 与 SMP 2 各 12/12，共 24/24**，归档 `20261022T155513Z-79812a4e4c8545d9990c4b3bbea4c229`，`git_dirty=false`）/ `test-gui-incremental`（四阶段全 PASS）。Task 13 那一轮另跑过 `test-qemu SUITE=gfx` 与 `OS01_SYSTEST=1 test-syscall`（342 passed, 0 failed），同为零退出码。`test-harness` 的用例数由 Task 13 的 582 增至 594，来自评审修复波新增的回归（Codex 轮的 4 项 shell marker 匹配测试等）。
  - **已知限制（如实记录）**：① `make PROFILE=x86_64-clang test-qemu SUITE=resolution` 的 `_client_estale_case` 在本环境**确定性失败**（`client_estale_no_diagnostic` + `client_estale_no_exit_status`）——同环境基线对照证明基线提交 `6126504a` 报出完全相同的两条断言，故**不是**本分支的回归；未修，也未代门禁宣布豁免。② GUI QEMU suite 的终端身份检查存在一处**已观察到但未复现**的 flake（guest 控制台把 shell 自己的 `fork:` 诊断插进回显命令中间，导致 `/proc/<pid>/status` 抓取在 `Pid:\t` 后被截断）；本次干净回归 24/24 通过，失败路径自 Task 11 起逐字节未变，故**未**在本任务修改，**未**声称已修。同一轮修复把该现象的**根因类别**定位为控制台写入逐字节释放锁、导致不同写者交错（`kernel/fs/devfs.c::serial_write()`），并已在 `3d6ab8b9` 从内核侧消除（整条缓冲在一次 `spin_lock_irqsave` 内发出，终端打印内容不变）；此后所有运行均未再复现，但该现象从未被复现出来钉死，仍按"未闭环"记录、**不声称已修**。③ guest 侧 `test_lvgl` 的**用户态 SIGSEGV**（第二轮修复期间第 3 次 `SUITE=gui` 运行中观察 1 次）：SMP 1 `smoke` 报 `rc=139`，guest 打印 `Killing task 5 (user fault at RIP=000000000049884D, signal=11)`（`0x49884D` = `test_lvgl.elf` 的 `memset`，发生在 `[TEST PASS]` 之后）；**不是**这两轮修复引入（`libgui` 侧仅一次比较，`fb_state.c`/`gfx.h` 为纯注释），此后未复现。**未修**、**未**声称已修，已登记到 `docs/roadmap.md` Parked 段与验证报告 §7.5。细节与证据见 [验证报告](superpowers/reports/2026-10-22-gui-2-validation.md)。

## 2026-10-20

- feat(gui): **GUI-1 LVGL 平台后端（libgui）闭环**（2026-10-19/20，7 个 commit `2fb58e71` → `d829b36f`，已合入 `master`）：从 `test_lvgl` 抽出可复用的 `libgui` 平台层，roadmap 的 GUI-1 行此前**漏勾**，本次一并更正。① `build(lvgl)`（`2fb58e71`）：fatal 断言改为 `LV_ASSERT_HANDLER` 立即 `_exit(1)`，并新增 `qemutests/gui_build_audit.py` 审计**真实**宏展开（不靠改配置伪造红灯）；② `feat(gui)`（`622d6734`）：公共 API `gui_platform.h`（`gui_platform_create/destroy/get_width/get_height/get_mode/step/get_pixel`）与私有 tick 契约 `libgui/internal/gui_tick.h`、`gui_lvgl_private.h`，公共 sysroot 只发布 `gui.h`/`gui_input.h`/`gui_platform.h`，不暴露 LVGL 类型；③ `feat(gui)`（`648a23f7`）：`libgui/src/platform.c` + `libgui/src/tick.c` 实现显示/离屏后端、tick、flush、生命周期与失败回退，`gui_platform_step()` 输出下一次 poll 等待时间供驱动方结合 `poll` 推进，格式/stride/容量/错误传播由宿主确定性契约测试钉死；④ `test(gui)`（`c27b900a`）：真实 LVGL 渲染（跨 partial 行边界取样）、50 轮 create→destroy 生命周期、4 MiB TLSF 池耗尽后触发 fatal 断言的子进程；⑤ `build(gui)`（`f5a9f7bf`）：目标归档迁移与 sysroot 原子发布；⑥ `refactor(test_lvgl)`（`acebb9df`）：应用改用平台生命周期与 poll/step 循环；⑦ `test(gui)`（`d829b36f`）：QEMU 门禁覆盖 SMP 冒烟、显示所有权与 fatal 恢复。设计 [spec](superpowers/specs/2026-10-19-gui-1-lvgl-platform-backend-design.md)、实施 [plan](superpowers/plans/2026-10-19-gui-1-lvgl-platform-backend.md)。

- test(resolution): **roadmap Parked #5 #1–#7 闭环**（分支 `fix/resolution-switcher-cosmetic`，commit [`cd65b868`](../../logs/git/refs/master)）：`qemutests/test_resolution_switcher.py` 7 项 cosmetic/鲁棒性收尾一次性合并（+231/−24，1 文件）。① `expect()` 增 `sentinels=` 参数，boot/restore/m{} 三类 surface 强制 `(0,0)=黑` sentinel（来自 `terminal_render.c:356` `gfx_fill_rect(... bg)` 后置约束）由 `_mk_surface_with_sentinel(w,h,fill=(2,2,2),sentinel=(0,0,(0,0,0)))` host 复现非纯黑 surface + 已知像素双源 ground truth；② `_drain_case` 用 hold-then-setres-assert-EBUSY 串起三步且 release 匹配 `start=` 过滤陈旧 serial；③ `wait_for(r"HOLD ok", ..., start=mark)` 收紧所有 callsite 防陈旧行；④ `_client_eagain_case` 同 start 化 + hold 8s 余量；⑤ `_mmap_case` 改 `parse_resjson` 过滤 `op=="mmap" and rc==0`，不再误命中 rc:-1 失败 JSON；⑥ `run_test_suite` 现跑三启动 `_isolation_case`（spec §8.2 item-1 隔离契约）；⑦ `PROMPT_RE = re.compile(rf"^{re.escape(PROMPT)}", re.MULTILINE)` 行首锚定 3 callsite 替换；⑧ `main()` `parser.error()` 校验 `case` 置于 firmware gate 之前，未知 case 返回 SystemExit(2)。补充 5 host unit test（`test_round_trip_sentinel_violation`、`test_mmap_helper_filters_partial_success`、`test_prompt_re_matches_at_line_start`/`_rejects_mid_line`/`_matches_after_first_command`、`test_main_unknown_case_exits_2`、`test_main_round_trip_standalone_rejected`）。roadmap Parked #5 #8–#21（客户端策略/`terminal.c:301-302` present 门限/`fb_test` 残留/T6 pty/`fb_write_row` 死码等）仍未闭环。

- fix(aarch64): **phantom `-ENOENT` 根因钉死并修复**（分支 `fix/aarch64-phantom-enoent`，commit [`80eed2e9`](../../logs/git/refs/master)）：`kernel/include/arch/atomic.h` 的 AArch64 `arch_atomic_cas()` 内联汇编执行 `cmp` 但只声明 `"memory"` clobber，未声明 `"cc"`，导致编译器保留 PROTNONE 检测的 `tst` NZCV 跨 CAS 跨过去再被 `csel` 消费。首次 CAS 失败（lock busy）时 `cmp 0,1` 把 Z=0，csel 依此清掉描述符 V 位，retry 路径不改 desc 直接 `stlr` 出 V=0/TABLE=1/PA=correct 的畸形 desc，下一次读 V=0 即 phantom `-ENOENT`。修复 +1 行（加 `"cc"`）。回归钉死测试：新增 `kernel/selftest/test_aarch64_page_table_multicore.c` §5b（BSP+AP 紧邻 PTE 索引上 500 次 `replace_4k` RW→RO 原子循环，`ap_work_cmd_ext` 增 `WORK_PT_REPLACE_LOOP=5` 枚举值驱动 AP 侧）—— cpus=4 无修复 100% 复现，有修复 100% PASS；cpus=2/4 实端到端各 5/5 PASS。roadmap Parked #2 + M4「不得引入并发 `arch_vmm_*` updater」硬约束同步撤销。根因证据：`docs/aarch64-phantom-enoent-root-cause-2026-10-19.md`（前后 disasm）；pause-session 期间的历史档：`docs/aarch64-phantom-enoent-handoff-2026-10-19.md`。

## 2026-10-19

- fix(selftest): **test_tty_vintr 间歇性挂死**（分支 `fix/tty-vintr-timeout`）：VINTR → signal_pgrp 链路本身工作正常（vintr 子线程确实唤起并 `vintr_seen=1`），但测试线程用 `arch_local_irq_disable` 的紧忙等 spin2 在 SMP=4 负载均衡把 vintr 子线程迁到与测试线程同 CPU 时永远等不到该值（runner 75s 超时）。改为直接检查 `t->signal & SIGINT && t->state == RUNNING`，与 `test_pgrp_signal` 同模式。SMP=4 与 SMP=8 各连测多次 tty_vintr 0 失败；fd_refcount 在 SMP=4/8 偶发独立 `rc_-1` 失败与本 fix 无关（roadmap parked item #1）

## 2026-10-14

- fix(kernel): **用户态异常终止编码为信号（`3d56e1d0`）**：`kernel/arch/x86_64/intr/trap.c` 的 `kill_current_user_task()` 原先硬编码 `do_exit(0)`，`current->exit_code` 存 0，父进程 `waitpid()` 看到的是 `WIFEXITED && WEXITSTATUS == 0`——与干净的 `exit(0)` 无法区分。于是任何 POSIX 消费者（shell / init / busybox `wait` / bash trap）都把用户态异常（segfault、FPE、非法指令、对齐故障……）当成正常自愿退出，信号语义被静默破坏。信号被一路打通：`kill_current_user_task(pt_regs_t *regs, int sig)` 与 `handle_user_fault(pt_regs_t *regs, const char *name, int sig)` 各增一个 `sig` 参数（前者内联汇编把 `xorl %edi, %edi` 换成 `movl sig, %edi`），`do_exit()` 原样存入 `current->exit_code`（低字节放信号，与 `kernel/syscall/sys_proc.c` 里正常退出码放高字节的编码不冲突）。14 处 `handle_user_fault` 调用点按 trap 映射信号：DE/OF/BR/NM/CA/MF/XM → SIGFPE、UD → SIGILL、GP/PF/TS → SIGSEGV、NP/SS/AC/MC/VE → SIGBUS；#PF 处理器的 16 处直接 `kill_current_user_task` 调用统一用 SIGSEGV。回归：`user/systest.c` 新增 54b/54c/54d 三例，fork 后分别触发 #DE（volatile 除零）、#PF（写 mmap 的 PROT_READ 页）、#UD（`asm ud2`），父进程 `waitpid()` 校验 `WIFSIGNALED` 与对应 `WTERMSIG`；全套 98 项 syscall 测试通过。

- fix(monitor): **`monitor_term_enter` 后立即 flush ANSI 转义（`9042bd20`）**：`monitor_term_enter()` 用 `fprintf` 排了两条转义序列（清屏、隐藏光标）却从不 flush；两者都不含换行，于是它们滞留在 libc 缓冲里，直到下一次带换行的 `fprintf` 才落地——交互模式下第一帧约 1 秒后才到，用户这段时间看到的是脏屏 + 闪烁光标。在 `user/monitor/render.c::monitor_term_enter` 末尾加 `fflush(out)`，让「终端已进入清屏模式」这个契约同步生效。`monitor_term_leave()` 不动：它执行时 monitor 已打印过大量带换行的帧，缓冲自然排空，无用户可见延迟。roadmap monitor parked ⑤。二进制级验证：`monitor_term_enter` 的反汇编以尾调用 `fflush` 结束（clang 把尾部的 `fflush(out)` 优化成 `jmp fflush`，因 fflush 是 nothrow + 返回 void）。

- test(hosttests): **framework 负契约 binary 接入 CI（`339ffe26`）**：`hosttests/cases/test_framework_contract.c` 自 `2d594ce0` 起就存在，但既没有 Make 目标也没有运行入口，于是 `TEST_RESULTS()` 复位 stats、断言宏双次求值这些契约的回归可以带着全绿的 CI 溜过去。`hosttests/Makefile` 增加 `test-framework-contract` 目标：构建 `.elf` 并断言其退出码**恰好为 1**（契约要求框架「正确地坏掉」），失败时打印 binary 输出与按退出码给的提示——0=断言不再传播、2=宏双次求值、3=`TEST_RESULTS` 复位 stats（即 `2d594ce0` 之前的 bug）。`mk/components/run.mk` 增加包装目标并让 `make test-harness` 依赖它；`.github/workflows/ci.yml` 的 harness job 本就跑 `make test-harness`，回归从此被 CI 拦住。验证：`make test-framework-contract` 通过（binary 退 1、make 退 0）；把 `2d594ce0` 之前的复位行为注入回 `TEST_RESULTS()` 后 binary 退 3，Make 目标退 2 并打出对应提示。

## 2026-10-11

- feat(monitor): **终端系统监控器（Terminal Monitor）**（分支 `worktree-monitor-impl`，15 commits，Tasks 1–9 全部闭环，spec `docs/superpowers/specs/2026-10-09-monitor-design.md`）：
  - **CPU 记账**：`kernel/sched/accounting.c` 每核 100Hz busy/idle tick 记账（BSP/AP 各记一次、epoch/valid 语义、LAPIC 失败核 unavailable）+ `task_t.runtime_ticks` 原子累计
  - **PMM/thermal**：`pmm_get_stats()` 锁内 T/U/F 字节快照；`arch/thermal.c` 弱 unsupported facade（真实 DTS 后端属独立 thermal 计划）
  - **task 身份与 procfs**：`birth_id`/`start_ns`/`comm` 生命周期；`/proc/stat`、`/proc/thermal`、升级版 meminfo/status（Name 逐字节编码）；VFS 新增 per-file open/read/release 快照钩子（open 时快照、分段读稳定、末次释放）；legacy `vfs_read` 路径保留
  - **user runtime**：consumer=user builtins 归档 + receipt（`__udivti3`），`test-monitor-runtime` 门禁
  - **monitor 应用**：`/bin/monitor`（collect 严格解析、128 位差分模型 + 60 槽历史、VT100 渲染、termios/PTY 生命周期、`--samples` 纯文本模式）
  - **测试**：`test-monitor-host`（file/accounting/pmm/procfs/collect/model/render 七 CASE，约 1600 断言）、`test-qemu SUITE=monitor`（1/2/8 核矩阵 + PTY 端到端）、`/proc/meminfo` 移除 legacy `Zones:` 行（无消费者，格式变更备案见上）
- feat(gfx): **显示所有权协调器与异常恢复（Display Ownership Coordinator）**（分支 `feat/display-ownership`，Tasks 1–6 全部闭环）：
  - **所有权协调状态机**：`kernel/driver/fb_state.c`（头文件 `kernel/include/driver/fb_state.h`）维护 `g_owner` / `g_fallback` 单例（均为非所有权视图 cookie `const void *cookie`，非 `file_t *`）；ioctl UAPI 定义在 `kernel/include/uapi/gfx.h` 并由 `kernel/driver/gfx.c` 分发（新增 `GFX_ACQUIRE`、`GFX_RELEASE`、`GFX_REGISTER_FALLBACK`、`GFX_REPLACE_FALLBACK`、`GFX_GET_DISPLAY_STATE`）；`g_owner` 进程显式释放或最终关闭（`gfx_release_file` 调用 `fb_owner_detach`）时自动降级切换至 `g_fallback`
  - **写入路径仲裁**：`gfx0` 的 present 提交走 `fb_gfx_writer_begin`（逐行写入使用 `fb_write_row_leased`），非当前 owner 严格拒绝（`-EACCES`）；传统 `fb_writer_begin` 与 GUI owner 互斥
  - **原始 mmap 边界防护**：存在 GUI owner 时禁止直接可写 `mmap("/dev/fb")`（返回 `-EBUSY`）；只读 mmap 映射为只读 PTE；`do_mprotect` 对 `VMA_IO`（包含显存）禁止提升为可写（返回 `-EPERM`）
  - **用户态客户端与 libgfx**：`libgfx` 新增所有权管理包装 API（`gfx_acquire` / `gfx_release` / `gfx_register_fallback` 等）；terminal 注册为 fallback owner，在 GUI 进程退出或崩溃后无缝接管并恢复终端画面
  - **全量测试与回归**：
    - `qemutests/test_display_owner.py`：覆盖正常接管、释放恢复、owner 崩溃恢复（SIGABRT/fault）、非 owner 被拒、fallback 替换等完整状态机场景，SMP 1 和 SMP 2 均全绿通过（9/9 steps）
    - 分辨率切换器套件兼容：`_drain_case` 与 `_mmap_case` 接入独立 raw inittab 启动，全套 resolution suite 与 `FB_RESOLUTION_TEST=1` 验证通过
    - 全量回归：`test-host`（108 套件）、`test-static`、`test-resolution-host`、`test-qemu SUITE=gfx`、`test_lvgl_runner.py` 全绿通过
  - **设计与实施文档**：`docs/gui/gui.md` 第 4 节、`docs/driver/driver.md`、`docs/superpowers/specs/2026-10-07-display-ownership-design.md`、`docs/superpowers/plans/2026-10-07-display-ownership.md`
- docs(test): **`test_display_owner` 的 libgfx 句柄 fd 读取保持现状（2026-10-11，显示独占实施）**——该测试通过 `*(const int *)gh` 直接读 `libgfx` handle 首字段的 fd（布局耦合；设计规范禁止公开 `gfx_get_fd`）。候选方案是引入非 sysroot 的内部测试头 `libgfx/gfx_test.h` 配 `offsetof` static assert 校验，或在测试代码里保留当前读法并补上显式注释。**评审决定保持现状、不改测试代码**，记录在案。

- chore(monitor): **/proc/meminfo 移除 legacy `Zones:` 行**（monitor 分支 Task 4）——可观察的格式变更备案：该行无任何消费者（内核内或用户态），monitor spec 亦未约束它，故随 procfs 统一格式化（`kernel/fs/procfs_fmt.h`）一并删除；其余 meminfo 字段与 kB 单位保持不变

## 2026-10-07

- fix(hosttests): **断言退出码完整性与宏参数单次求值（`2d594ce0`，test-framework plan Task 1）**：host test 的结果此前不可信——`TEST_RESULTS()` 会把 `__test_stats` 清零，`RUN_ALL_TESTS()` 之后失败计数归 0，失败断言到不了进程退出码（Makefile 的 run recipe 只看非零退出），于是「断言失败但 suite 绿」是可能的。`TEST_RESULTS()` 改为只打印快照、不再 reset，另加独立的 `TEST_RESET()` 宏；`assert_eq`/`assert_str_eq`/`assert_mem_eq` 改为宏参数只求值一次（后两者原先在失败打印路径上会二次求值）；新增 `hosttests/cases/test_framework_contract.c` 负契约 binary（**不在 `TEST_BINS`**，故意失败），用三个不同退出码钉死三条契约——1=期望的失败被传播到退出码、2=宏双次求值、3=`TEST_RESULTS` 复位 stats。顺带删除 `test_aarch64_arch_vmm_init.c` 里只因 `TEST_RESULTS` 清零 stats 才存在的影子失败计数机制（`g_test_failed_count` + `ASSERT_*_TRACKED`），`page_table_test_runner.h` 改为在 summary 之后读 `__test_stats.failed`；`TEST_BINS` 去掉重复的 `test_libc_stdio_registry.elf` 条目并加 `check-test-bins-dup` Make 门禁在 run 前拒绝重复。验证：`make test-host` 107/107 suite exit 0；contract binary 退出码 1 且两次 summary 都显示 `Failed: 3`（双次求值那条先以 rc=2 确认过 RED）。

- cleanup(aarch64): **M2/M3 杂项 cosmetic 遗留 7 项全部闭环**（分支 `worktree-m23-cosmetic-cleanup`，7 commits）：
  - **死枚举**：删 `ap_work.h` 的 `WORK_PT_STRESS`/`WORK_PT_ALLOC`（无任何用户；`WORK_PT_MAP` 由 kernel selftest 的 `ap_work_ext_run` override 消费，保留），同步修正 `ap_work.c` 与 selftest 头注释里过时的 WORK_PT_STRESS 叙述
  - **head.S 过时注释**：`head.S` TPIDR_EL1 注释里 "152 (0x98)" 改为陈述真实约束（16-bit mov immediate），不再硬编码可漂移的尺寸数字（实际 `PERCPU_DATA_SIZE` 自 M3 Task 12 起为 144）
  - **mock percpu 陷阱**：`hosttests/mock/test_platform.h` 删 legacy `tlb_wanted/tlb_ack`（生产端 M3 Task 12 已移除）、`apic_id` 改名 `arch_processor_id` 对齐生产命名，注释明确「compile-only stub，布局契约以 `kernel/include/percpu/percpu.h` 为准（`test_percpu_layout.c` 运行时钉）」
  - **serial_printk %d 字面输出**：确认根因（aarch64 `printk_stub` 用 `kputs(fmt)` 打字面格式串）已在 `7ebc9d0a`（share color_printk + serial_printk via kernel-core）移除；本次以 `test-aarch64 MODE=smp` 9/9 boot 日志 0 处字面格式串 + `[selftest] 6 total: 6 passed` 闭环，无代码改动
  - **errno 归一化（TDD）**：`vmm_backend.c` 新增 `pt_err_to_linux()`，替换原 Task 19 两点式 EEXIST/ENOENT 归一化，覆盖全部 `arch_vmm_*` 出口——此前 `AARCH64_PT_EINVAL=-1`（撞 Linux `-EPERM`）、`ENOMEM=-4`（撞 `-EINTR`）、`EPERM=-6`、`EAGAIN=-7`、`ECONFLICT=-5` 裸穿透（可达路径：split 已发布 root 的 `-EPERM` 与 alloc-fail 的 `-ENOMEM`）。`AARCH64_PT_EPROT_NONE` 三态 sentinel 保留；未知码原样穿透 fail-loud。RED→GREEN：`test_aarch64_backend_4k.c` 新增 errno-contract 4 断言（先 3 失败后全绿），`shootdown_probe.c`/`vmm_backend.h` 注释同步
  - **read_l2_desc test-only 守卫**：声明与定义均加 `#ifdef OS01_SELFTEST`（`kernel/random/random.c` test-hook 同款模式）——生产误用直接编译错；hosttests 的 `AARCH64_PT_SW_HOST_CFLAGS` 加 `-DOS01_SELFTEST=1` 保持 host harness 可用；生产 aarch64 内核 `nm` 0 引用验证
  - **§5.2b TLBI TODO**：`walk_to_l2` 的 `TODO(Task 18)` 过时任务编号改为条件式描述（首个「已发布 root + create」caller 出现时落地 per-level TLBI 分支；M4 phantom-ENOENT 钉死前不实现）
  - **验证**：`make clean` 后 `test-host` 108 套件 0 失败（避开增量假绿）；生产 aarch64 构建 rc=0；`make PROFILE=aarch64-clang test-aarch64 MODE=smp` 9/9 PASS（cpus 1/2/4 × 3）且 selftest `6 total: 6 passed`

## 2026-10-06

- refactor(driver-model): **ARCH-9 驱动模型 / 总线抽象** —— commits `14119cf7..8b29b25`（分支 `arch9-driver-model` 已合并并删除，21 commit + 1 followup fix；84 文件，+16,695/-1,875）：
  - **A 阶段（块设备 ops + GPT 包装）**：`struct block_device_ops {read, write, flush}` + `struct block_device_desc {name, sector_count, sector_size, ops, private_data, parent, kind}`；移除 `port_num` 直访问，驱动注册自己的 ops；GPT 经父入口 dispatch；fs/boot 只从 BLOCK_DISK 中选第一盘
  - **B 阶段（device core + PCI core + AHCI + boot coordinator）**：typed `pci_driver`（id_table 匹配 + probe/remove） + `struct pci_device`（dev, domain/bdf, vendor/device/class, bars[6] {kind, address, valid, index}, driver, driver_data） + `struct pci_backend {roots, root_count, read32/write32/route_gsi}` + x86 backend 隔离在 `kernel/arch/x86_64/bus/pci.c`；AHCI 私有 `ahci_instance` + 安全撤销/DMA 隔离 + boot coordinator（`device_core_init` → `net_device_init` → `pci_enumerate` → `pci_bind_all` → `net_lwip_start`）
  - **C 阶段（net_device 抽象 + lwIP + e1000 + virtio-net + ENETDOWN 守卫）**：`struct net_device {name, mac, mtu, link_up, parent, ops, priv, adapter}` + ops `{xmit, poll_rx, get_link, stop}`；`net_service_ready()` / `net_default_ipv4()` 公共 readiness 查询；e1000 与 virtio-net 每实例驱动，3 种 IRQ 模式（INTX/POLL/MSIX）；socket `do_socket` 在 NIC 未 ONLINE 时返回 `-ENETDOWN`（spec §Review Focus #4）；过渡期 `os01_netif` 在 Task 9/10 一起移除，`kernel/net/net.c` 收敛为 39 行两向薄 shim
  - **D 阶段（QEMU 多卡矩阵 + 故障 fixture + per-BDF 观察）**：`make test-qemu SUITE=driver-model` 跑 16 case × 2 SMP；`ARCH9_FAULT=none|observe|bad-nic-bar|adapter-fail|ahci-empty|irq-conflict` 通过 root 传 kernel flag；build contract 路径固定 `build/x86_64-clang/{kernel,artifacts,image}/driver-model-<fault>/`；每个 BDF 注册观察计数器（probe/bar/adapter）让 unsupported/modern-only 能精确断言未匹配设备 zero probe；boundary audit + test-syscall alias 恢复（用户 AGENTS 唯一受允许例外）；`kernel/include/ipc/mbox.h` 从 `kernel/net/sys_arch.c` 抽出满足 source/header symmetry
  - **最终 wave fix**：AHCI id-table 严式 `01/06/01` 单一匹配（移除 `0xFFFF00` prog_if 掩码，避免非 AHCI SATA 控制器被程序化为 AHCI）；新增 `arch9-irq-mode: ... nic=ethN mode=POLL|INTX` log，使 irq-conflict 矩阵 case 真正验证第二卡 POLL 回落而非仅凭 `iface=` 标记放行
  - **全量测试验证**：`test-arch9-host` 10 套件绿、`test_driver_model_matrix.py` 21/21（10 RED 注入 + 11 含 IRQ mode/per-card evidence/SMP 过滤/边界审计/IRQ mode 解析）、`test_arch9_build_contract.py` 12/12、`make test-qemu SUITE=driver-model` 31/31（16 case × 2 SMP，含 net-block-smp@SMP=2 only、`irq-conflict` 第二卡 POLL marker）、e1000 + virtio 单卡网络回归、`make PROFILE=x86_64-clang image` 全链路；`test-driver_model_boundary_audit.py` 7 套件验证 `test-syscall` alias gate 与 5 条边界规则
  - **已知遗留**：`make OS01_SYSTEST=1 test-systest` 暴露 pre-existing 回归（`config/inittab.systest` 不带起网卡，Task 9/10 的 `net_service_ready()` 正确返回 -ENETDOWN；不在本分支范围，单独 follow-up）；多项 minor cleanup（test-only externs、`copy_to_user_ft_res` utility split、文档准确性）通过 task 评审 parked 进 ledger
  - **测试质量复盘**：完成 12 task × 1 task-reviewer + 4 fix rounds（Task 11 的 silent-pass 隐藏 bug 已修复：`nic1=None` 与 `ok1 >= 3` 互斥 + SMP=1 误跑 + `unsupported` 观察断言反向）+ 最终 wave（AHCI prog_if mask + irq-conflict POLL marker）；交付 12 commits + 7 fix commits
  - **设计/计划文档**：`docs/superpowers/specs/2026-10-05-arch9-driver-model-design.md`（已确认设计）+ `docs/superpowers/plans/2026-10-05-arch9-driver-model.md`（实现计划，12 任务 4 阶段）；执行评审记录见 `.superpowers/sdd/2026-10-05-arch9-driver-model/progress.md`（gitignored）

## 2026-10-05

- refactor(syscall): **ARCH-2 Linux ABI 兼容层独立**：
  - **保持 OS01 原生 ABI 为唯一标准**：Linux x86_64 系统调用仅作为兼容层映射转调原生分发器，杜绝兼容逻辑反向主导系统调用号与语义
  - **兼容模块子目录化与防溢出保护**：拆分至独立子目录 `kernel/syscall/compat/{dispatch.c, table_x86_64.c, proc.c, fs.c}`，表项类型升级为 `int16_t`（`compat_syscall_nr_t`），附带编译期 `_Static_assert` 杜绝溢出隐患
  - **未映射/不支持严格哨兵拦截**：未映射、越界或显式不支持（`COMPAT_UNSUPPORTED`）编号严格返回 `-ENOSYS` (-38)，不再透传至原生分发器
  - **领域语义适配**：在 `compat/proc.c` 中承接 `rt_sigaction`（Linux 13，4 参数 `sigsetsize == 8` 校验）与 `wait4`（Linux 61，参数转发至 `SYS_waitpid`）
  - **ELF 探测与 Linux ABI 激活解耦**：`elf_detect_abi()` 通过 `PT_NOTE` 段（`NT_GNU_ABI_TAG`，OS=Linux）检测外部静态 Linux 二进制并赋予 `PF_LINUX_ABI`；**严格不碰 `PT_INTERP`**，彻底解耦 Linux ABI 激活与未来动态链接机制
  - **全量测试验证**：`test-static` 静态审计（含递归兼容目录扫描）、`test-host` 57/57 单元测试套件、`systest` 341/341 项端到端测试（新增 `54_linux_abi_compat`）、QEMU `phase-0`、`inittab-phase` 与 `network` 套件全量绿灯通过
- feat(mm): **aarch64 M2 Slab 接入 + M3 内核 VMM 合入** —— merge `2034e5bc`（commits `dabc9f0..2034e5bc`，86 commits）：
  - **M2 Slab 接入**：删除占位实现 `kernel/arch/aarch64/runtime/slab_stub.c`（`slab_init()` 无操作、`kmalloc()` 返回 `NULL`），aarch64 改编译生产 `memory/slab.c`；`slab.c` 的 x86 专属 `pushfq`/`cli`/`sti` 锁路径改为 `arch_local_irq_save/restore`；`slab_init()` 的 j-loop 与 8 页记账幂等化，与 boot reservation 不重复计数；新增 `slab_layout_compute()` 纯函数真值源，供 `early_arena.c` preflight 与启动后断言共用，arena 候选扫描计入 slab 段字节
  - **M3 语义层拆分**：公共 `kernel/include/memory/vmm.h` 收敛为语义层（仅 `VM_*` + `arch_vmm_*` 语义 API），x86 裸 PTE 位与 `PAGE_*` 迁入私有头 `kernel/include/arch/x86_64/pte.h`，`kernel/memory/vmm.c` 退化为 backend wrapper 并原样保留 free/COW 所有权逻辑
  - **M3 aarch64 backend**：新增 `kernel/include/arch/aarch64/vmm_backend.h` + `kernel/arch/aarch64/memory/vmm_backend.c`；`page_table.c` 补齐 walk_to_l2/l3、4 KiB map/unmap/replace、2 MiB block 与 split（已发布 root 拆分返回 `-EPERM`）、root 发布登记制与软件位编码
  - **M3 SMP shootdown**：`kernel/memory/tlb.c` 串行化协议 + 每核 `tlb_ack_gen` 代数 ack + 超时 FATAL，经 `kernel/arch/aarch64/smp/ap_work.c` 工作槽与 SGI 3 广播（逻辑向量 `IPI_VECTOR_TLB` → GIC SGI 3 + 显式目标列表）
  - **M3.1 audit gate**：`docs/memory/memory.md` 新增「vmm 变更调用链审计」清单（I1/I2/I3 不变量 + 排序规则 R1），配 `hosttests/cases/test_vmm_caller_audit.c` 源码扫描与 `make PROFILE=aarch64-clang test-aarch64-audit` nm 扫描双重把关
  - **验收**：host 用例 `test_slab_basic` / `test_slab_layout_compute` / `test_slab_lock_path` / `test_slab_idempotent_reservation` / `test_slab_basic_x86_count` / `test_arena_candidate_slab_bytes` / `test_aarch64_backend_4k` / `test_aarch64_pt_2m_block` / `test_aarch64_pt_split` / `test_aarch64_pt_software_bits` / `test_aarch64_pt_root_publish` / `test_aarch64_pt_locks` / `test_vmm_replace_protocol` / `test_vmm_prot_none` / `test_vmm_ack_wraparound` / `test_vmm_semantic_header` / `test_x86_vmm_backend` / `test_aarch64_arch_vmm_init` 全 PASS；QEMU `qemutests/aarch64_m3_probe.py` 产出 `M3-SHOOTDOWN-PROBE: OK`；多核 selftest `kernel/selftest/test_aarch64_page_table_multicore.c` 覆盖 target-set / partial-mask / 锁交错 / 共享 L1；x86_64 行为不变
  - **后续**：`kernel/include/arch/atomic.h` 的 AArch64 `arch_atomic_cas()` 缺 `"cc"` clobber 导致的并发 replace phantom `-ENOENT`，于 2026-10-19 根因钉死并修复（`80eed2e9`，详见 2026-10-20 条目与 `docs/aarch64-phantom-enoent-root-cause-2026-10-19.md`）
  - 设计 [spec](superpowers/specs/2026-10-03-aarch64-m2-m3-slab-vmm-design.md)（v9）、实施 [plan](superpowers/plans/2026-10-03-aarch64-m2-m3-slab-vmm.md)（v4）

## 2026-10-04

- refactor(syscall): **ARCH-1 syscall 层脱离 arch** —— commits `2765103..74e3cfeb`：74 个已实现处理器迁至 `kernel/syscall/`，x86_64 入口保留寄存器解码、Linux ABI 预翻译和用户态信号返回；`SYS_getpeername`（62）保留 trace 名称但无 handler、返回 `-EINVAL`。新增分发表 hosttest 与静态边界审计；host 56/56、syscall systest 340/340、网络 QEMU、普通启动通过。独立内核自测在 `test_tty_vintr` 有通过和超时两种结果，本地 master 合并验收亦复现超时；根因未确认并列入 roadmap Parked。
- refactor(arch): **ARCH-3 头文件定义全局 + 彻底移除 `-z muldefs`**：
  - 彻底移除 `kernel/arch/x86_64/make.config` 与 `kernel/arch/aarch64/make.config` 中的 `-z muldefs` / `-Wl,-z,muldefs` 链接参数，消除链接期对符号冲突的静默吞噬
  - 全局对象定义迁入 `.c`：`kernel/include/sched/task.h` 中的 `init_task_union`、`init_task[]`、`init_mm`、`init_thread` 迁入 `kernel/sched/task.c`；`kernel/include/intr/interrupt.h` 中的 `irq_table` 迁入 `kernel/intr/irq.c`
  - x86_64 TSS 硬件结构解耦：新建 `kernel/include/arch/x86_64/tss.h`，`init_tss[]` 实体定义迁入 `kernel/arch/x86_64/cpu/task_arch.c`，通用调度器头文件 `task.h` 不再包含体系结构特定的 TSS 结构体与硬编码 IST 字段
  - 清理暴露的重复符号：lwIP `sys_msleep` 宏覆盖与 `arch/x86_64/string.h` 中的 `memset` `static inline` 修饰
  - 静态测试门禁闭环：`qemutests/runtime_audit.py` 增加禁止 `-z muldefs` 参数校验；新增 `qemutests/header_object_audit.py` 静态审计 149 个公开头文件，保障零强/弱对象定义
  - 完善后台自测执行：`test-kernel-selftest` QEMU 增加 `< /dev/null` 重定向避免非交互终端挂起；放宽 `test_mutex.c` SMP 自测自旋上限
- feat(terminal): **terminal 迁移至 libgfx + 刷新与 I/O 优化** —— commits `52a77005` / `920b48a6` / `c9105090`：
  - terminal 渲染路径切至 `libgfx.a` 静态库，支持离屏双缓冲与字形绘制加速（`gfx_draw_glyph`）
  - 优化刷新算法：利用像素级区域滚动替代整屏重绘，批量合并串口 I/O 读写
  - 增加 eager PTY 排空与 8 像素宽字体快速展开路径，显著降低高负载输出卡顿
- feat(mm): **用户空间信封扩展至 512 MiB + 匿名 mmap 缓冲** —— commits `c53d7cde` / `8ad4d1a8`：
  - `USER_PAGE_SIZE` 重命名为 `USER_ENVELOPE_SIZE`，上限由 16 MiB 提升至 512 MiB（`0x400000` 到 `0x20400000`），为大分辨率像素缓冲与后续应用预留充足空间
  - `libgfx` 像素缓冲分配由堆（`malloc`/`brk`）迁移为匿名 `mmap`（`MAP_ANON`），彻底解耦图形缓冲与 brk 用户堆
- docs(roadmap): **规划 P2 架构治理任务 ARCH-1..10 并清理过时项** —— commit `dd05877b` 及后续收尾：
  - 确认 OS01 自有 syscall ABI 为唯一标准，Linux ABI 仅作兼容层
  - 规划 ARCH-1（syscall 脱离 arch）、ARCH-2（Linux ABI 兼容层独立）、ARCH-3（消除 muldefs 与头定义全局变量）、ARCH-4（拆分 sched/task.c）等 10 项治理任务
  - 清理 roadmap 中已在 master 闭环的规划项（用户堆与 ELF 映射隔离、2D 图形 API、PS/2 鼠标驱动、aarch64 M0/M1、增量重编等）

## 2026-10-03

- feat(mm): **aarch64 M1 运行期直映严式页表树合入** —— merge `e531167b`（commits `10826fef..f1df9444`）：
  - 共同 `arch/boot_memory.h` 启动直映契约：PMM 之后、AP 启动前单点调用 `arch_boot_direct_map_init()`
  - x86_64 适配器保留现有 2 MiB 启动直映，传播中间页表分配失败；`ZONE_UNMAPPED_INDEX` 覆盖截止语义保持
  - aarch64 从低窗口 arena 建立独立 TTBR1，覆盖高 RAM 与 holes，收紧设备与保留 block；严格限制非 RAM 映射
  - BSP/AP root 与 probe 全面验证，覆盖 16 组 RAM/CPU/镜像矩阵（256 MiB..4 GiB，1..4 核），通过稀疏、容量耗尽、AP 坏 root 注入测试
- docs(spec,plan): **aarch64 M2 Slab + M3 内核 VMM 详细设计闭环** —— commits `93f8ae7f..9edc1cab`：
  - spec v8 + plan v4：明确 frame 级记账、锁契约（BBM、inactive root 拆分）、SGI/SMP shootdown 协议与初始化顺序契约，就绪待实装（**已由下方 2026-10-05 条目闭环**）

## 2026-10-02

- feat(mm,elf): **用户堆与 ELF 映射隔离全面闭环** —— merge `277b6315`（8-task plan，16 commits `fb6635a3..085f50e1`）：
  - **Task 1**（`5cc047e6`）：纯 ELF64 加载布局校验器（`elf_layout_validate`）
  - **Task 2**（`01d95d6c`）：4 KiB 分页 ELF 加载器与单回滚所有者（`elf.c` 重写，杜绝大页溢出）
  - **Task 3**（`fa3fa45d`）：暂存 spawn/exec 镜像生命周期 + 零长度初始 `VM_HEAP` VMA + `mm_init_user_heap`
  - **Task 4**（`76076b9d`）：`mm_set_brk` 拥有已提交 4 KiB 堆页，`SYS_brk` 委托处理；扩展按页分配，收缩时释放物理页并解除映射；缺页保护防止越界预写
  - **Task 5**（`a6b9507d`）：`mm_user_range_protected` 保护 ELF envelope、堆保留区、guard 保护页与用户栈免受 `mmap`/`munmap`/`mprotect` 侵入
  - **Task 6**（`95129e93`）：暂存两阶段 `fork_mm_copy` + 回滚修复（VMIO huge、fork-of-fork COW）
  - **Task 7**（`81dc54a0`）：`prepare_user_write_range(_locked)` 用户写前 COW 私有化准备与 11+ 审计调用点接入
  - **Task 8**（`284d846a`）：全屏 1440×900 + `brk(0)` headroom + Tetris smoke + E2E 验证；host 43/43 suites，systest 334/334 全绿
- feat(aarch64): **aarch64 M0 启动直映契约合入** —— merge `2523a311`（commits `e829c377..8a9fa7e6`）：
  - `head.S` 在 MMU 打开前建立 0..2 GiB boot map，内核 block 保持 EL1 可执行，其余 RAM PXN/UXN；普通镜像与自测镜像契约完全对齐，消除 C 代码补图差异

## 2026-10-01

- feat(aarch64): **aarch64 EL1 sync 致命异常诊断** —— merge `d6fbb22d`（commits `8734f1e5..cba5f578`）：
  - 实现 ESR_EL1 / FAR_EL1 / ELR_EL1 致命诊断与寄存器解析
  - 建立 sync-fault 独立探针镜像构建机制，配套 QEMU 自动化测试 harness
- docs: **文档目录树状结构重构** —— commit `931393b9`：
  - 将 `docs/` 下 50+ 扁平文件重组为与 `kernel/<subsys>/` 对称的树状子目录
  - 历史 closure 报告与调试日志归档至 `docs/archive/`；合并构建相关手册为 `docs/build/build.md`

## 2026-09-30

- feat(gfx): **P3 2D 图形 API 闭环（libgfx + /dev/gfx0）** —— merge `8a0f4e7b`（commits `f81f092f..614e181c`，6-task plan）：
  - `libgfx.a` 用户态静态库：点/线/矩形/填充矩形/位图 blit 与局部视口裁剪
  - `/dev/gfx0` 受限 present 设备：per-file ioctl 视图生命周期、受控矩形呈现、坐标校验
  - Tetris 游戏迁移：将直接 `/dev/fb` mmap 替换为受限视图呈現，游戏逻辑零改动平滑迁移
  - QEMU ring-3 E2E 测试套件（`test-qemu SUITE=gfx`）与 host 单元测试全覆盖

## 2026-09-28

- refactor(kernel_main): **x86 引导阶段分步拆分** —— merge `9f911824`（commits `f5004588..8a43cea3`）：
  - 拆分 BSP per-cpu 初始化、启动文件系统挂载、x86 设备节点注册、控制台 TTY 引导连接与 AP 引导辅助
  - 提升引导模块内聚性，为 x86/aarch64 统一单 `kernel_main` 入口铺平路径

## 2026-09-27

- feat(driver): **PS/2 鼠标驱动落地（P3 首项闭环）** —— commits `116e57ec..0a68ae97`（10-task plan）：
  - `i8042` 控制器共用层：互斥锁、键盘/鼠标来源分流、command byte 事务机制与 pump drain 上限保护
  - `mouse_event_t` 8 字节事件 ABI（`kernel/include/uapi/mouse.h`）
  - `/dev/mouse` 设备节点：event ring 环形缓冲、非阻塞/阻塞读取、`poll` 就绪通知
  - 500 ms 有界硬件探测与 F5 safe cleanup，QEMU 与 host 单元测试全通过

## 2026-09-26

- refactor(aarch64,build): **最后两个 Parked 项闭环（idle 正名 + sysroot 头文件级增量重编）** —— worktree `parked-two-closures`：
  - **idle 正名**：`kernel/arch/aarch64/cpu/idle_resume_stub.c` → `cpu/idle.c`。`subsys_stub.c` 此前已由 AAGU-3 闭环删除（`serial_printk`/`strcmp`/`num_cpus` 均有真实实现）；wfi 永循环对无调度器的 aarch64 是正确的 idle 行为而非占位符，per-CPU need_resched 再入归 §P2 上下文切换统一
  - **增量重编**：genid→`-B` 全量重编移除。kernel 编译/链接（`-isystem`、`-include stdint.h`、`ALL_LDFLAGS -L`、`KERNEL_RAW_LIBDIR`）全部切稳定 `$(SYSROOT)` symlink；publisher `cp -p` 保 mtime + publish 等 lease（防中途换 header）；`mk/components/kernel.mk` 删 `.sysroot-generation` stamp
  - **顺带修复三个预先存在的缺陷**：① `-MMD` 跳过 system header → kernel `.d` 从未记录 `-isystem` 引入的 libc 头，改 `-MD`（`.cflags` fingerprint 加 `deps=system-headers` 前缀使现存 build 目录一次性自动重建）；② runtime grouped rule receipt 无条件重写 + archive 永不更新 → 每次 kernel 构建都 relink（stage1+kallsyms 两遍），receipt 写入改 cmp 内容门控；③ aarch64 下 `KERNEL_RAW_LIBDIR` 空 `-L/usr/lib`（宿主路径）风险随 symlink 切换消除
  - 回归：`test-contract` 新 mode `sysroot-headers`（单头编辑只重编依赖者 / `.cflags` stamp 不动 / 还原一致 / 零污染）。验证：x86 contract 9 modes `>>> ALL TESTS PASSED <<<`；aarch64 构建 + SMP 9/9 PASS

- fix(build): **CFLAGS-only build cache 失效闭环（Parked 项）** —— `kernel/Makefile` 新增 `$(BUILD_DIR)/.cflags` fingerprint stamp（编译 pattern rule 前置依赖；`ALL_CFLAGS`+`ALL_C_ONLY_FLAGS` 内容变化才改写 → 全量重编；recipe-time 展开规避 `make -n` 破坏性，本地 `FLAGS_STAMP_FORCE` phony 替代 root-only 的 `FORCE`）。新增 `KERNEL_EXTRA_CFLAGS` 旋钮并入 `OS01_SUBMAKE_ALLOWED`。`test-contract` 新增 `flags-cache` mode（TDD：先 RED「stamp missing」后 GREEN，4 门禁：stamp 存在 / identical 重建零重编 / flag 变化 stamp+重编 / flag 还原重编；排除 `kernel/runtime/`——runtime builtins 用独立 `RUNTIME_CFLAGS_kernel`，不参与 kernel CFLAGS）。根因核查：原始 `KERNEL_SELFTEST=1` 症状已被 AAGU-5.6 variant 目录顺带修复（复现实测 169 个 .o 全落 `kernel/selftest/`，normal 命名空间仅共享 runtime builtins 被动重建）；本次闭合通用面——实测注入 `-D` 探针后 0 个 `.o` 重编。`make test-contract`（x86 8 modes + aarch64）PASS；`aarch64-uefi-kernel` 构建正常。**预先存在的独立问题**（未修）：aarch64 contract `targets` mode 依赖 x86 build 目录存在——`make -n PROFILE=x86_64-clang kernel.bin` 的 `+` 前缀 artifact recipe 在 `-n` 下也执行，host-test mode 末尾的 `make clean` 删掉 x86 目录后 aarch64 contract 即失败（clean workspace 上跑 aarch64 contract 同样触发）

- refactor: **AAGU-4 残留清理 — close §3.3 ❌ + 🟡** —— branch `docs/roadmap-slim-v30`（commit 链路 `099b060`（spec）→ `4e1d33b`（plan）→ 4 task commits）：
  - **Task 1**（`b994d8f`）：softirq 原子操作实现从 `kernel/arch/<arch>/cpu/atomic.c` 外部函数迁移到 `kernel/include/arch/<arch>/atomic_bitops.h` static inline + `__attribute__((always_inline))`；`kernel/intr/softirq.c` 删除 2 个 `#if defined(__x86_64__)/#elif defined(__aarch64__)/#else #error` 块
  - **Task 2**（`3ababa8`）：`kernel/time/tick.c` poll-timeout scan + PIT/LAPIC handoff 拆分：weak 默认 `poll_timeout_tick()` 在 `kernel/time/tick.c`（aarch64 phase 1 路径），strong 实现在 `kernel/fs/poll.c`（x86_64）；PIT/LAPIC handoff 合并到 `kernel/arch/x86_64/platform/time.c::arch_tick_start()`；`tick.c` 删除 2 个 `#if defined(__x86_64__)` 块
  - **Task 3**（`29996e9`）：`kernel/include/time/clocksource.h` 2 个 `#if defined(__x86_64__)` 块（percpu include + `clocksource_read_ns()` inline）迁至新建 `kernel/include/arch/x86_64/clocksource.h`；`kernel/time/clocksource.c` + `kernel/time/timer.c` SUBSYS_INITCALL ifdef 删除；`timer.c` spin hint 接 `arch_cpu_pause()`
  - **Task 4**（`e91d0b9`）：`kernel/driver/{ahci,keyboard,pit,serial}.c` + `kernel/net/net.c` 共 5 个 TU 删除冗余 `#ifdef __x86_64__` SUBSYS_INITCALL 守护（TU 已在 x86-only 路径，Makefile 已 gate）
  - 文档同步：`docs/arch/cross-boundary-symbols.md` §3.3 ❌/🟡 状态全改 ✅，§6 验收清单增条目
- fix(aarch64): **IPI cpus≥2 FAIL 根因修复（TPIDR_EL1 误读）** —— commit `d695020`（worktree `fix/aarch64-ipi-fail`，当前 HEAD）：`kernel/arch/aarch64/intr/ipi_test.c::ipi_cpu_id()` 从 `TPIDR_EL1` 取指针后误解释为 `aarch64_boot_percpu_t *`（实际 `percpu_t`），跨字段偏移导致 cpu≠self 时 `id` 计算错位（`cpu=1 received=3`、`cpu=2/3 received=0`）。两个「暖机」SGI 掩盖真问题：`cpu=0` 暖机时 IPI handler 调 `gic_send_sgi(self,2,0,FILTER_SELF)` 写 `0x02000002`，触发 SGI 2 self-trigger，handler 重入将 `received[cpu]` 加 1 —— 看似通过；删暖机后 `cpu=1..N-1 received == 0`。修：去掉类型转换，改用运行期 `cpu_id()`（`mrs x0, TPIDR_EL1` → `percpu_current()`），删除两个暖机 SGI；`SGI_TEST_ACK_COUNT == N-1`（`cpu=0` 不应收到自给 SGI）+ `GICC_IAR == 0x401`（sgi_int_id=2，CPU targets=1）严格断言。QEMU `make PROFILE=aarch64-clang test-aarch64-uefi-smp` 1/2/4 ×3 共 9/9 PASS（0 TIMEOUT / 0 FAIL）。完整根因 + 修复记录见 `docs/archive/aarch64/aarch64-ipi-fail-handoff-2026-09-26.md`
- ci: **bucket test targets 迁移** —— commit `36e6230`：CI workflow 从旧的 forwarding `test-*` aliases 切到 6 个 bucket 目标（`test-qemu` / `test-host` / `test-static` / `test-aarch64` / `test-contract` / `test-kernel-selftest`）
- refactor(harness): **删 forwarding `test-*` aliases** —— commit `ce258a1`（worktree `feat/build-system-harness-consolidation` 收尾）：删 09-25 引入的临时 forwarding `test-*` aliases，仅保留 6 个 bucket 目标作为权威入口。follow-up 见 `9f13465`（track followup）

## 2026-09-25

- refactor: **build system harness consolidation**（worktree `feat/build-system-harness-consolidation`，12 commits `70daaea..ce258a1`）：统一 6 个 bucket test 目标（`test-qemu` / `test-host` / `test-static` / `test-aarch64` / `test-contract` / `test-kernel-selftest`），target taxonomy / capability gate / alias policy 收敛到 `docs/build/build.md` 作为权威 reference：
  - `70daaea` plan (v3, 12 tasks) → `64dd4ac` gate x86 validation + 公开隐藏 test target → `5c43897` drop unused `all` alias → `452bbf1` DRY QEMU command lines（`RUN_QEMU_BASE` / `_FLAGS_<target>`）
  - `f2652a7` consolidate 4 QEMU E2E 目标 → `test-qemu SUITE=<name>`（保留旧名为 aliases）→ `58a6ef9` canonical `test-host` + 拆分 pmm helper → `64e3207` `test-static` umbrella + 4 subset aliases
  - `c28e67a` consolidate 3 aarch64 tests → `test-aarch64 MODE=<smp|no-ack|gic-spi>` → `7350c61` consolidate 2 contract tests → `test-contract PROFILE=<name>`
  - `782e964` `docs/build/build.md` 新建（target taxonomy 权威 reference）→ `30f3508` update Quick start / 用户入口 / test recipes
- refactor: **arch source groups**（commit `c3412da` + `18a4cff`，merge `79ccffb`，spec `2026-09-25-arch-source-groups-design.md`）：kernel 源按 `kernel/arch/<arch>/<topic>/` 职责分组（如 `kernel/arch/x86_64/intr/{8259A,lapic,lapic_timer}.c`、`kernel/arch/x86_64/sched/{task,switch}.c`、`kernel/arch/aarch64/intr/{gic_driver,gic,irq_probe,ipi_test,entry.S}` 等），更新所有架构引用路径 + 文档架构图
- refactor: **compiler_rt 目录裁撤**（commit `1c5816b`，merge `ca72145`）：`kernel/compiler_rt/` 整个目录裁撤，符号落点各归其位——`__stack_chk_*` 迁至 `kernel/core/stack_chk.c`；`__udivti3` 走 `runtime/builtins/`；elf-loader 保留 `runtime/builtins/`。消除「目录名与实际职责不符」的违例
- fix(run): commit `1fbca71` aarch64 QEMU harness 改用 selftest 变体镜像
- feat: **AAGU-5.8 三档环境测试 + 验收清单** —— commit `a05d975`（PR #28，worktree `feat/aagu-5-entropy-facade`）：QEMU 默认 / `+rdrand,+rdseed` / 真硬件 三档测试 + 验收清单。spec `docs/arch/entropy-source-facade.md`
- feat: **AAGU-5.7 STRONG-only pool + selftest NONE-mode + spawn/exec 策略** —— commit `587a894`（PR #27，6 commits `c97a8f5` / `3721e93` / `001528e` / `e9ae356` / `21f0bc7` / `587a894`）：STRONG-only pool + selftest NONE-mode 修正 + AT_RANDOM STRONG-only 控制流 + B3 WEAK-pool e2e 测试 + spawn/exec 决策

## 2026-09-24

- feat(aarch64): **AAGU-5.6 arch entropy facade + strong overrides + kernel/random refactor** —— merge #25（commits `4ab3370` / `f41e4e6` / `1c5bf17`）：spec `docs/arch/entropy-source-facade.md` + `arch_random_get_entropy()` + `arch_random_get_strong()` 双接口 + `kernel/random` refactor。x86_64 strong override：`RDSEED` = STRONG / `RDRAND` = WEAK；aarch64 strong override：`RNDRRS` = STRONG / `RNDR` = WEAK
- feat(aarch64): **AAGU-29 aarch64 libk.a link**（commit `bee8da5`，merge #26 `bec4a4c`，worktree `fix/aagu-29-libk-aarch64`）：aarch64 kernel 现在 link `libk.a`（`kernel/arch/aarch64/make.config`: `ARCH_LIBS = -nostdlib -lk`）→ `memcpy/memset/memmove/calloc/free/malloc/strlen/strcmp` 走 libc 单一来源。删 `kernel/compiler_rt/memset.c` weak fallback + `kernel/arch/aarch64/libc_stub.c` calloc/free shim。完整闭项见 `docs/archive/aarch64/aarch64-libk-aarch64-closure-2026-09-24.md`
- feat: **AAGU-6 P2 风格/头/测试 cleanup batch** —— commit `b580432`（PR #24，worktree `feat/aagu-6-p2-cleanup`）：统一 kernel `__stack_chk_guard` + 移除 `kernel/arch/aarch64/memset.c` weak stub + `idle_resume`/calloc shim 等清理

## 2026-09-23

- feat(libc): **AAGU-4.6 libc atexit 串入 exit 路径** —— commit `c4a93fe`（PR #23）：libc atexit 串入 exit 路径，关闭 AAGU-4.6
- refactor(atomic): **AAGU-4.5 arch_atomic_or/and_u64 facade + softirq.c drop ifdef** —— commit `7828d7d`（PR #22）：新增 `arch_atomic_or/and_u64` arch-neutral facade + `softirq.c` 内 `#ifdef __x86_64__` 移除，关闭 AAGU-4.5
- feat(kernel): **AAGU-4.3.6 kernel/include/compat/stdlib.h → libc 单一来源** —— commit `bfb314d`（PR #20）：删除 `kernel/include/compat/stdlib.h` 文件，改走 libc 单一来源（`git rm compat/`，目录裁撤的早期收口）。至此 AAGU-4.3.1~6 全套（`list/rbtree/string/stdlib/sys-cdefs/sys-types` 6 mirror 头）落地
- feat(kernel): **AAGU-4.3.5 kernel/include/compat/string.h → libc 单一来源** —— commit `93c5403`（PR #19）
- feat(kernel): **AAGU-4.3.2 kernel/include/compat/rbtree.h → libc 单一来源** —— commit `0ddbfa7`（PR #18）
- feat(kernel): **AAGU-4.3.1 kernel/include/compat/list.h → libc 单一来源** —— commit `f976a1b`（PR #17）
- feat(kernel): **AAGU-4.3.4 kernel/include/compat/sys/types.h → libc 单一来源** —— commit `bdb1630`（PR #16）
- feat(kernel): **AAGU-4.3.3 kernel/include/compat/sys/cdefs.h → libc 单一来源** —— commit `06777a2`（PR #15）

## 2026-09-22

- feat: **AAGU-4.2 UAPI auxv + stat.h 单一源收口** —— commit `8937932`（PR #14）：kernel UAPI 为唯一源，libc 端 sysroot 安装
- refactor(compiler_rt): **AAGU-4.4 kernel `__stack_chk_guard` 单一来源** —— commit `5b4f736`（PR #13）：`kernel/compiler_rt/` 内 `__stack_chk_guard` 单一来源（2026-09-25 目录裁撤后迁至 `kernel/core/stack_chk.c`）
- fix(libc/stdio): **AAGU-4.1 libc stdio `fread/fwrite` stream 验证** —— commit `baaa3e9`（PR #12）：`fread/fwrite` 入口加 `is_open_file` stream 验证，关闭 AAGU-4.1
- spec: **AAGU-4 跨边界符号/ABI 边界规范** —— commit `35ed5e3`（PR #11）+ `180b14e`（Explore agent 二次复核）：spec `docs/arch/cross-boundary-symbols.md`（R1 完成）。规则分 4 类：
  - **builtin 类**（kernel 唯一来源，禁止 libc 镜像）—— 落地于 AAGU-4.3 / 4.4
  - **UAPI 类**（kernel UAPI 为唯一源，libc 端 sysroot 安装）—— 落地于 AAGU-4.2
  - **arch-value 类**（arch-neutral facade + strong override）—— 落地于 AAGU-4.5
  - **libc 镜像类**（libc 单一来源，kernel include 仅引用声明）—— 落地于 AAGU-4.6
- feat(aarch64): **AAGU-3 subsys_stub convergence + BSP-exclusive CNTP** —— commit `0345ca8`（PR #10，worktree `feat/aagu-3-subsys-stub`）：`kernel/arch/aarch64/subsys_stub.c` 收敛为 API-parity 占位 + BSP 独占 CNTP 控制（APs 跳过 timer init）。替换条件已记录：AAGU-29 落地后只剩 ~10 行 drop-in
- fix(ci): **AAGU-8 CI 跑在 `ghcr.io/aagu/os01-ci` 镜像内** —— commit `a062a67`（PR #9）：CI 改跑在 published `ghcr.io/aagu/os01-ci` 镜像内

## 2026-09-20

- ci: **AAGU-7 GitHub Actions CI 启用** —— commit `c5e730b`（PR #6）：GitHub Actions CI 启用，初始 workflow

## 2026-09-19

- fix(ci): commit `424e25f`（PR #8）retain LLVM 和 QEMU runtime libraries —— 修复 CI 镜像 missing `.so` 问题
- ci: **AAGU-7.1 ship clang-22 + qemu-11.1.1 CI image** —— commit `2396622`（PR #7）：CI 镜像发布 clang-22 + qemu-11.1.1
- feat: **AAGU-1 / AAGU-2 [P0] CSPRNG entropy fail-closed + UEFI GetRNG + aarch64 log variadic + AT_PLATFORM facade** —— commit `8931cab`（PR #5）：CSPRNG entropy fail-closed + UEFI `EFI_RNG_PROTOCOL` GetRNG + aarch64 log variadic 适配 + `AT_PLATFORM` facade

## 2026-09-18
- feat(aarch64): **Generic Timer Phase 1 + Phase 2 全套闭环（5/5 follow-ups）** —— 6 merges `b2b81fc` / `e115d79` / `10fd3d2` / `3037df3` / `5edf79a` / `a110ab9`（worktrees `feat/aarch64-timer-phase1` 等）：
  - **Phase 1** CNTP + CNTVCT + clocksource 框架（merge `b2b81fc`，`aarch64_timer.c` 133 行 + `clocksource_register`）
  - **Phase 2 #1** SUBSYS_INITCALL plumbing（merge `e115d79`）
  - **Phase 2 #2** `cntp_tick_handler` → `tick_handler` 集成（merge `10fd3d2`）
  - **Phase 2 #3** per-CPU timer / SMP timer（merge `3037df3`）
  - **Phase 2 #4** `__udivti3` hoist 至 `runtime/builtins/`（merge `5edf79a`，commit 详见 `docs/archive/aarch64/aarch64-udivti3-hoist-closure-2026-09-18.md`）
  - **Phase 2 #5** `-I libc/include` 清理：6 mirror 头（`list/rbtree/string/stdlib/sys-cdefs/sys-types`）改走 `kernel/include/compat/`，由 libc 单一来源提供（merge `a110ab9`，详见 `docs/archive/aarch64/aarch64-libc-include-policy-closure-2026-09-18.md`）
  - **Phase 2 P2 follow-ups 5/5 全闭环**

- fix(aarch64): **GIC clobber-probe TIMEOUT 修复** —— commit `2481e1f`（worktree `fix/gic-probe-timeout`）：`kernel/arch/aarch64/irq_probe.c:103-105` asm `mov x10,#0x200` + `movk x10,#0x0002,lsl #16` 两个 immediate 错位，实际算出 `0x0002_0200`（SGI 512, filter=LIST）而非 `0x0200_0002`（SGI 2 + filter SELF）；GICv2 静默丢弃 out-of-range SGI → 2 秒 ldar 轮询命中 deadline → `[gic-probe] save-restore TIMEOUT`。修正为 `mov x10,#2` + `movk x10,#0x200,lsl #16`，`x10 == 0x02000002 == gic_send_sgi(dev,2,0,FILTER_SELF)`（C wrapper 编码 `filter<<24 | targets<<16 | sgi&0xf`）。配套 RED→GREEN hosttest `hosttests/cases/test_gic_probe.c`（180 行 + Makefile wiring）：suite 1 用 production gic_driver.c + mock MMIO 断言 C wrapper 写 `0x02000002`；suite 2 静态扫描 irq_probe.c 源码禁止已知 buggy literal pair。QEMU E2E 矩阵 `make test-aarch64-uefi-smp --cpus 1 2 4 --repeat 3` = 9/9 PASS（0 TIMEOUT / 0 FAIL）；hosttests 23/23（x86_64-clang + aarch64-clang）；aarch64 uefi KERNEL_SELFTEST=1 build PASS。`docs/superpowers/specs/.../phase1` §7.3 clobber-probe 设计 + plan §2.2 评审均提及 SGI 2 self-trigger，但 plan:1078 原写法 `movk x10,#2,lsl #24` 本身是 assembler error（lsl #24 非法），实现层的 bug 制造了 *silent* TIMEOUT，plan 的 bug 只会产生 *obvious* 编译失败——一并记入 follow-up

## 2026-09-17
- feat(aarch64): **GICv2 通用中断框架 Phase 1** —— merge `545a935`（worktree `feat/aarch64-gic`，14 commits = 6 spec/plan docs + 8 实现）：
  - spec/plan docs：`1c555f5` GICv2 Phase 1 spec + plan → `2a0f30a` R1 (9) → `42ece05` R2 (7) → `7d59ac4` R3 (1) → `ccf5b47` R4 (4) → `08c6302` plan NIT-1 hosttest 编译路径（合计 22 条修订全落地）
  - Task 1.1/1.2 GICv2 driver 泛化：`c885d94` gic.h + gic_driver.c 198 行 + gic.c wrapper + `a99ed71` mock-MMIO hosttest RED→GREEN（8 suites 69/69）
  - Task 2.1/2.2 QEMU marker harness + entry.S 全量 save/restore + 通用 dispatch：`ec167ae` RED + `d601570` GREEN（4 marker + 22/22 hosttest grep）
  - Task 2.3a/2.3b SPI harness + PL011 RX handler + DTB interrupts 解析 + test-aarch64-gic-spi target：`5180681` RED + `ef19203` GREEN（8 fixture self-test）
  - Task 3.1/3.2 SMP IPI harness + gic_pub.h dsb ishst + ipi_test.c SGI send + handler + per-CPU trace：`a50b8aa` RED + `93bafb3` GREEN（14 fixture）
  - 边界全守：x86_64 0 改动 / AGENTS.md 0 改动 / `kernel/include/arch/barrier.h` 0 改动（R4-2 决策）/ 不编 kernel core / 不进调度器；Spec G1-G6 + R1-R11 全落地。QEMU E2E `test-aarch64-uefi-smp` 全程 **6/6 触发 `[gic-probe] save-restore TIMEOUT`**——根因 asm immediate 错位，已在 09-18 commit `2481e1f` 修复
- docs: **`user-stack-canary` 闭环** —— master `0819e20`（merge），R1-R14 codex review
  + opus v2 重写 + 9 commits subagent-driven。commit 清单：
  - `00a98e6` feat(user): RED canary_smash probe + systest 45/46/47
  - `3d3fed8` feat(libc): GREEN user-space stack canary (SSP) + 3 build flags
  - `414065c` feat(user): canary_dump probe + systest 43/44 guard entropy
  - `2aa346a` feat(build): test-user-canary 7-step Layer 1 audit target
  - `84b05fe` chore(build): preserve busybox symbols via SKIP_STRIP=y
  - `9174fe0` feat(selftest): RED kernel selftest at_random (Task 2.1)
  - `e35c763` feat(kernel+libc): AT_RANDOM auxv + selftest wrapper + getauxval (Task 2 GREEN)
  - `ae472a6` feat(net): LWIP_RAND → kernel ChaCha20 CSPRNG + hosttest path test
  - `815ba57` docs: roadmap + changelog 同步 (Task 4 — user-stack-canary 闭环)
  - `0819e20` Merge feat/user-stack-canary: user-space stack canary + kernel CSPRNG + LWIP_RAND
- feat(libc): 用户态栈 canary —— libc/user/busybox 全面
  -fstack-protector-strong；guard 每次 exec 经 SYS_getrandom 播种；
  systest 43-47；make test-user-canary 构建期审计
- feat(kernel): auxv AT_RANDOM(16B CSPRNG)+AT_PLATFORM("x86_64")；
  getauxval()；kernel selftest at_random_layout/entropy；systest 48-53
- feat(net): LWIP_RAND 接内核 ChaCha20（替换 jiffies LCG）；
  hosttest --wrap 白盒 + test-network 6/6

## 2026-09-16

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **统一用户态启动方式**（worktree `startup-unification`，merge `01c96a8`，9 commits `d7822cc..09264e4`）：`task.c` 两处用户栈构造（spawn argv-only / exec argc+envc）提取共享 `setup_user_stack()`（一处 auxv 逻辑，消除双站点漂移，`56867f1`）；`__libc_start_main` 真实现（`environ` 初始化 + auxv walk，`2a198ab`）；crt0 从寄存器传参（rdi/rsi/rdx）改为标准 SysV `_start`——从 `[rsp]` 解析 argc/argv/envp/auxv 传 `__libc_start_main`，并正确设置 `environ`；busybox overlay crt0 同步（`09264e4`）。执行中发现并顺手修复两个 latent kernel 缺陷作前置——ext2 稀疏洞读零填（Task 0，`d7822cc`/`2773763`）+ `deep_copy_argv` 接受显式空 `{NULL}` argv/envp 数组（Task 4.5，`e4e3a85`/`26be52e`/`9100ea4`）。验证：systest 基线 268 + 11 新启动探针全 GREEN、test-network 6/6、test-inittab PASS、52-applet 回归 clean | 5 天 | 09-12~09-16 |
| **deep_copy_argv over-cap 回归修复**（`8fa1663`）：`26be52e` 删 `if (count==0) return -E2BIG` 同时干掉了合法空数组 + over-cap 两个判断，回归仅 over-cap 检查。补回 `count > MAX_ARGV` 显式拒绝；RED test `tests/runtime/user/test_argv_overcap.c` 验证。merge `b39d571` | 半天 | 09-16 |

## 2026-09-13

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **文档学习层**：`docs/README.md` 索引（按 0/1/2/3/4 层组织，全局认知→架构骨架→核心机制→I/O 子系统→构建调试）+ 4 份源码导读（调度器 / trap.c / vfs+memory / tty+intr），让新人按指定顺序读源码即可走通关键路径。`docs/README.md` + `docs/sched/scheduler-reading-guide.md` + `docs/syscall/trap-reading-guide.md` + `docs/memory/vfs-memory-reading-guide.md` + `docs/interrupt/tty-intr-reading-guide.md` | 1 天 | 09-13 |

## 2026-09-12

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **目录重构 P1-P6**（merge `31bb128`，commits `eb70174` + `0a392c8` + `e45c650` + `f8fbf62` + `99f5f45` + `7551910` + `6730681` + `6e6d180`）：test 目录 → `hosttests/qemutests/kernel-selftest/runtime-selftest`；`kernel/kernel/ → kernel/core/`（字节相同）；headers 按 subsystem 拆分 + `arch/` lift 出 `include/kernel/`；random/log/font/logo 拆出 core/；pic/timer 归属 owning subsystem + pty 迁移；P6 清理 + 全量文档同步 + PMM 测试移植。spec `2026-09-12-directory-restructure-design.md` | 1 周 | 09-04~09-12 |
| **PMM/sched 稳定性系列**（merge `be7db4f`，commits `1506d6e` + `36eb6a3` + `ea89136` + `beb351c` + `0ba888a` + `0809100` + `4468e75` + `514e062` + `b68e1b1`）：MEMORY_RANGE_GRANULE 64-bit + E820 MEMORY_TYPE 保留 + alloc/free/预留 RAM-relative 索引（3 个独立 PMM 隐患，每个先 RED 后 GREEN）+ 调度 lost-wakeup 窗口修复 + 6 例 PMM host 测试 + `find_mount` 防御（user-pointer mount entry → ENOENT）+ SMP=4 boot 双 bug 修复（linker script orphan sections + active PGD lifetime）。SMP=1/2/4 + systest-repeat 7 连 268/268 | 1 周 | 09-02~09-12 |
| **aarch64 页表原语**（spec `2026-09-11-aarch64-page-table-primitives-design.md`，commits `169e0d5` + `c9c0246` + `cdb6625` + `466401d` + `28ac0ea`）：`kernel/arch/aarch64/page_table.c` arch-local walk/map 原语，为 head.S + MMU 铺路。附带 aarch64 PMM 修复：`1c51dfa` 多 zone `alloc_pages` 索引 + `053a226` direct map 覆盖 PMM 分配范围。QEMU smoke 断言（descriptor bit + minimal intermediate descriptor） | 3 天 | 09-11~09-12 |
| **x86_64 内核栈保护**（commits `ea72359` + `4d884d1` + `b918b08` + `1b79ace` + `51f9617` + `d7209c7` + `77c60dd`）：x86_64 内核使用 `-fno-pic -mcmodel=large -fstack-protector-strong`，全局 per-boot guard；审计直接 `R_X86_64_PC32` 或 `R_X86_64_64` guard 引用并拒绝 GOT；编译门控的 QEMU 破坏性 canary trip test 已打印预期诊断 | 半天 | 09-12 |

## 2026-09-09 ~ 09-10

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **v25 arch-cleanup-gh 系列**（merge `3ab4ef1`，10 commits `67132e2..5dbc63d`）：weak-default + strong-override 模式覆盖 7 个子系统。① `67132e2` roadmap v24 doc；② `af9a6ce` bootinfo(arch) E820 → `bootinfo_x86.h`；③ `19b84a8` arch(neutral) `arch/regs.h` pt_regs_t facade + `rwlock_relax()` 走 `arch_cpu_pause()`；④ `dfead87` intr(arch) `arch_irq` hooks 拆分 controller selection + gsi↔vector + dispatch；⑤ `c37522a` rtc(arch) core + per-arch impl 拆分；⑥ `52f99a1` mm(arch) PGD/PUD/PMD/PTE 层级统一 + bit-constant rename；⑦ `0ecee53` arch(subsys) `SUBSYS_INITCALL()` + `.subsys_init` section 替代硬编码 driver 列表；⑧ `58db2a7` arch(sched) `arch_kernel_thread_entry` 弱默认 + x86_64 强覆盖；⑨ `5dbc63d` build(uefi) digest + staged copy 排除 `*.o/*.a/*.lib` 残留 | 3 天 | 09-09~09-10 |
| **PMM arch-neutral**（spec 13 轮 review 通过 + plan 16 task + final fix，17 commits `2c08e78..a2e7389`）：`pmm_init(const struct boot_context *ctx)` 单一入口 + 弱默认 `pmm_arch_normalize`/`pmm_arch_zone_split` 在 `kernel/memory/pmm_arch.c` + x86_64 强覆盖（E820 + kernel-LMA/handoff/trampoline excludes + 2 MiB granule + sort/merge）+ aarch64 强覆盖（读 `aarch64_ram_map_get()`）。`pmm.c` body 用 RAM-relative indexing（`pages_struct + ((start - lowest_ram) >> 21)`），Step 7 clamp 防 aarch64 unsigned-underflow。x86_64 systest 268/268 + nettest 6/6 零退化，aarch64 uefi-smp 9/9 PASS | 3 天 | 09-09 |
| **log API 统一**：`kernel/log.h` 提供 gate-wrapped `log_err/warn/info` 宏（`do { if (LEVEL <= g_log_level) _log_*_impl(__VA_ARGS__); } while(0)`），保留 `log()` core macro + `g_log_level` 调度；`_log_write` 拆为 variadic forwarder + `_log_writev` va_list core；x86_64 走 `_log_writev`/vsnprintf 串口，aarch64 走 `kputs(fmt)` 忽略 variadic（-nostdlib） | 半天 | 09-09 |

## 2026-09-06

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **回退 busybox 副本变通，切回符号链接 rootfs**（回退 `b32e1e0`；开启 busybox `CONFIG_LN/CONFIG_FIND/CONFIG_FEATURE_FIND_TYPE`；rootfs manifest 重新以 debugfs symlink 写入 29 条 applet 链（含 `/bin/ln`）；`mkdisk` 注释同步；`user/systest.c` case 41 改用 busybox `ln` 创链 + lstat 验证 + exec 跟随；case 42 改用 busybox `find -type l` 验证 `VFS_SYMLINK→DT_LNK` 映射。QEMU：systest 268/268（含 41/42 新断言全部 PASS）、nettest 6/6。spec §6 全部落地） | 半天 | 09-06 |
| **symlink/readlink + 4 新 syscall**（commits `43588c8`..`c64c854`）：VFS 软链接 + ext2 symlink（fast ≤60B inline `i_block[0..59]` / long data block via `i_block[0]`）；4 新 syscall `SYS_symlink/readlink/lstat/fstatat`=71..74；libc `syscall3`/`syscall4`（r10 ABI for fstatat）；`vfs_getdents` `VFS_SYMLINK→DT_LNK`；67 systest + 6 kernel selftest。spec `2026-09-05-symlink-support-design.md` v5 | 1 天 | 09-06 |

## 2026-09-02 ~ 09-05

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **自托管 compiler runtime**（多个 commits）：udivti3 实现 + provider-keyed selfhosted archive + provider 构建不变量硬化 + 内核链接 compiler runtime + kernel link publication 加固 + compiler-rt eligibility 验证 + kernel runtime validation targets + syscall/selftest suite 隔离 + variant link paths + root `make sysroot` 入口。详见 `runtime/` + `docs/build/build.md` | 3 天 | 09-04~09-05 |
| **aarch64 UEFI bootloader 统一**（commits `af166bc`..`06e6127`，merge `06e6127`）：x86_64 + aarch64 共享 `boot/uefi/main.c` + arch 分发 + `boot_context` handoff ABI + boot_context 头部偏移断言 | 1 天 | 09-03 |
| **aarch64 UEFI 固件修复**：firmware 截断 64MiB 适配 QEMU pflash（`11aa6ed`）+ aarch64 UEFI 默认 URL 下载（`bad8825`）+ aarch64 也显式传 clang+lld 到 posix-uefi（`25872d1`） | 半天 | 09-03 |
| **profile-only UEFI overlay 简化**：x86 UEFI 固件 per-profile（不再用运行时 overlay patch）+ host test 按 profile 隔离 + 所有组件强制声明 profile + profile-only UEFI cleanup contract | 半天 | 09-03 |
| **GNU Make 构建系统重构**（profiles + 受控递归 Make + 单 writer sysroot 原子 generation 发布 + 锁/租约协议 + kernel/libc/user/busybox/mbedtls/posix-uefi 组件适配器 + rootfs manifest + mkdisk 重写 + aarch64 bring-up 图分离 + 变体隔离镜像 + 全部公开 target 恢复。QEMU 验证：x86 disk.img 启动、aarch64 handoff/phase1 双签名、systest 228/228、nettest 6/6。契约测试 5 模式全绿）。**Plan deviation**：rootfs applet 项以 busybox 副本替代 symlink（内核 vfs 无软链接跟随 → exec symlink ENOEXEC，历史构建即副本规避）；kernel exec-symlink gap 列入 roadmap（P1/P5） | 1 天 | 09-02 |

## 2026-08-24 ~ 08-26

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **syscall 边界审计**（commits `a1ad1b9`..`80eab1a`，11 commits）：逐 syscall 检查 user-pointer 边界 + 可睡眠路径 + copy_{to,from}_user 失败处理 + ASLR/canary 未来兼容；详见 `docs/syscall/syscall.md` 末尾审计触达清单 | 半天 | 08-24~08-26 |

## 2026-08-23

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **roadmap 瘦身（v1）**：已完成内容迁出到 `docs/` 专题文档，roadmap.md 只剩 phase 表 + 待办 + 主题 docs 索引 | 半天 | 08-23 |

## 2026-08-17 ~ 08-18

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **timer 重构** (clocksource+clockevent 双层抽象 + TSC/RTC-PIE 联合校准 + LAPIC tick 接管掩 PIT + CLOCK_MONOTONIC/REALTIME/nanosleep/poll 迁纳秒 + jiffies self-test，systest 150/150，根治 PIT 200Hz) | 2 天 | 08-17~18 |

## 2026-08-16

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **网络回归 harness** (make test-network + OS01_TCP_ECHO_DELAY_MS delayed-reply + 20/20 no-delay + 10/10 delay250 cohort) | 1 天 | 08-16 |
| **DHCP ACD 关闭** (LWIP_DHCP_DOES_ACD_CHECK=0，消除 ~10.6s ACD 竞态导致的偶发不绑定) | 1 小时 | 08-16 |
| **requested-event-aware poll/select** (按请求方向注册/唤醒，修复复合 flags + PTY 双注册容量 + 时序敏感 select 断言) | 1 天 | 08-16 |
| **E1000 RX ring 所有权串行化** (tcpip 线程独占硬件 ring 消费，IRQ 仅 ack + wake) | 半天 | 08-16 |
| **俄罗斯方块游戏** (framebuffer 像素渲染 + 扫描码输入 + alt-screen 恢复 + UX) | 1 天 | 08-16 |

## 2026-08-15

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **per-poll timeout registry** (poll_timeout_head 链表 + PIT 扫描，修复 lost-wakeup + 并发 clobber) | 半天 | 08-15 |
| **keyboard poll 支持** (/dev/keyboard 扫描码 wait queue + keyboard_poll_dev) | 半天 | 08-15 |
| **tty termios 诚实化** (TCSETS 真存储 + raw 默认 + ICANON/ECHO) | 半天 | 08-15 |
| **terminal alt-screen 双缓冲** (?1049h/l) | 半天 | 08-15 |
| **/proc/<pid>/fd/ 观测性** (files_t 引用协议 pin/unpin + dup/dup2/fcntl 路由重构 + exit 路径 pin) | 1 天 | 08-15 |
| **arch 边界收紧** (x86 平台源选择、early task-state hook、公共 gate ABI、arch signal API、端口 I/O wrapper) | 1 天 | 08-15 |
| **网络正确性加固** (DNS 超时、端口字节序、部分读缓存、shutdown、UDP readiness、响应 hang) | 3 天 | 08-12~08-15 |
| **lwIP 网络栈合并** (E1000/virtio-net + PCI/MSI-X + DHCP/DNS + TCP/UDP socket + poll/select + HTTP wget) | 多迭代 | 08-15 |

## 2026-08-01

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **inittab 配置支持** (ACT_* 位掩码修复 + parse_inittab() open/read 解析器 + 3 套模板 + Makefile/build 集成 + test-inittab 相位派发验证) | 1 天 | 08-01 |

## 2026-07-25 ~ 07-29

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **SMP 负载均衡** (idle-steal + per-schedule pull + sched_pick_cpu + nr_running 指标 + 振荡防护 + 6 项 SMP 前置条件加固 + init 迁移保护 + idle→idle #PF 修复 + smp_stress 验证) | 2 天 | 07-29 |
| **EEVDF 调度器** (rbtree 可运行队列 + vruntime/deadline + pick_eevdf O(log n) + per-CPU TSS SMP 修复) | 2 天 | 07-25 |

## 2026-07-24

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **select/pselect syscall** (poll_table 动态化 + do_poll_core 提取 + 适配层 + pselect6 sigmask 原子性；当时 systest 118/118，当前 126/126) | 2 天 | 07-24 |

## 2026-07-19

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **ext2 读写** (alloc_block/inode、create/mkdir/rmdir/unlink/rename/truncate、selftest) | 2 天 | 07-19 |
| refactor: 固定数组→堆分配 (VFS name/cwd + mount_table + pipe buf + ext2 buf) | 1 天 | 07-19 |

## 2026-07-18

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **poll syscall** (poll_table + 双队列 wake + pipe/tty/devfs + PIT timeout) | 2 天 | 07-18 |
| Makefile QEMU targets 统一 AHCI (run/run-kvm/debug) | 10 分钟 | 07-18 |

## 2026-07-15 ~ 07-17

| 项目 | 工作量 | 日期 |
|------|--------|------|
| **busybox ash 方向键+行编辑** (VT100 CSI + terminal + FIONREAD) | 2 天 | 07-17 |
| arch/x86_64 头文件引用清理 | 1 小时 | 07-17 |
| **多架构清理收尾** (8 dispatch + 7 aarch64 桩) | 1 天 | 07-15 |

## 2026-07-12

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 日志级别系统 | 5 天 | 07-12 |
| 子系统注册框架 | 半天 | 07-12 |
| arch 通用头文件迁移 | 2 天 | 07-12 |

## 2026-07-11

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 内核栈 canary (SSP) | 30 分钟 | 07-11 |
| SMP 栈溢出修复 (ext2 buf[256]→buf[4096]) | 30 分钟 | 07-11 |
| selftest 10/10 + systest 70/70 修复 | 2 小时 | 07-11 |
| disk.img GPT 双分区 + tools/mkdisk | 1 天 | 07-11 |
| ext2 只读 + GPT + tmpfs + /dev 块设备 | 1 天 | 07-11 |
| VFS mount point getdents 修复 | 半天 | 07-11 |
| **COW fork (4KB-only)** | 2 天 | 07-11 |
| 4KB 页面 + VMA + mmap/mprotect | 3 天 | 07-08 |

## 2026-07-02 ~ 07-05

| 项目 | 工作量 | 日期 |
|------|--------|------|
| systest 70/70 | 1 天 | 07-05 |
| 信号 handler 用户态投递 | 2 天 | 07-05 |
| busybox ash shell + 9 applet | 1 天 | 07-04 |
| FPU/SSE 状态保存 (fxrstor/fsave) | 半天 | 07-03 |
| do_signal_delivery (Ctrl-C→SIGINT) | 1 天 | 07-03 |
| syscall + signal 框架 (systest 43 syscall) | 2 天 | 07-02 |
