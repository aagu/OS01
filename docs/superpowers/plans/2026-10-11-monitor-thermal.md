# OS01 Monitor Thermal Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 monitor 增加安全的 Intel BSP core DTS 采集；不能验证的硬件保持 unsupported。

**Architecture:** 精确匹配已标记 rdmsr 指令的 #GP recovery，不改变其他异常语义。新增创建前CPU限制协议，BSP内核任务每秒读取传感器并锁内发布缓存，procfs消费已完成 monitor 主计划的thermal facade。

**Tech Stack:** x86_64 assembly/C、MSR/CPUID、OS01 scheduler/subsys、hosttests、kernel selftests、QEMU。

**Spec:** `docs/superpowers/specs/2026-10-09-monitor-design.md` §3；与 `docs/superpowers/plans/2026-10-11-monitor.md` Task4 的thermal类型/读取接口一致。

## Global Constraints

- 第一版显示 `CPU0 Core Temp`，不冒充整包温度或全核最大值；package DTS 留作后续。
- `temp_mc` 为有符号毫摄氏度，只有 `state=OK` 时有效；`0` 不表示不存在。
- `IA32_TEMPERATURE_TARGET=0x1A2` bits `23:16`；`IA32_THERM_STATUS=0x19C` bit `31` 有效、bits `22:16` 距离。
- 温度公式 `(TjMax_C - digital_readout_C) × 1000`；不硬编码 `100°C`。
- AMD/aarch64/hypervisor 返回 unsupported。真实Intel只允许核实型号文档后的family/model白名单。
- 固定 BSP 的内核任务每秒采集；IRQ不读温度、不格式化、不分配。
- 缓存状态/值/采样时间成组发布，超过 `3秒` 未更新为 stale。
- task结构改动必须 `make clean`；用户态不可执行rdmsr，不增加临时syscall。

## Review Focus

- 未标记指令的#GP：继续正常fatal路径，禁止被安全探测吞掉（Task1）。
- hypervisor部分宣告DTS：不执行任何温度MSR读取（Task3）。
- CPU限制创建时机：新线程第一次运行及唤醒迁移均不能离开BSP（Task2）。
- 上次有效、下一次invalid/read_error：不得用旧值伪装本次OK（Task3）。
- BSP worker创建失败/采集停止：返回错误状态或stale，不发布假温度（Task3）。

## File Structure and Dependencies

| Boundary | Files |
|---|---|
| safe MSR | 新 `kernel/arch/x86_64/cpu/msr_safe.S`, `kernel/include/arch/x86_64/msr_safe.h`；修改 `kernel/arch/x86_64/intr/trap.c`, `kernel/arch/x86_64/intr/entry.S` |
| CPU限制 | `kernel/include/sched/task.h`, `kernel/sched/{fork,core,balance}.c` |
| thermal | 新 `kernel/arch/x86_64/cpu/thermal.c`；主计划的 `kernel/include/arch/thermal.h` 和弱 `kernel/arch/thermal.c` |
| 验证 | 新 `hosttests/cases/test_thermal_{fixup,affinity,decode}.c`, `kernel/selftest/test_msr_safe.c`, `kernel/selftest/test_thermal_affinity.c`；修改测试登记 |

Task1和Task2相互独立，Task3依赖两者及主计划Task4。不得在主计划Task4之前创建另一套thermal类型。可先完成温度后端再交付主计划，但没有真实硬件证据仍只能声称unsupported路径和算法经过验证。

### Task 1: Instruction-scoped safe RDMSR

**Files:** 新 `kernel/arch/x86_64/cpu/msr_safe.S`, `kernel/include/arch/x86_64/msr_safe.h`, `hosttests/cases/test_thermal_fixup.c`, `kernel/selftest/test_msr_safe.c`；修改 `kernel/arch/x86_64/intr/trap.c`, `kernel/arch/x86_64/intr/entry.S`, `kernel/Makefile`, `hosttests/Makefile`, `mk/components/run.mk` 及现有selftest登记文件。

**Interfaces:** `int arch_rdmsr_safe(uint32_t msr, uint64_t *value)`，成功0、#GP返回-EIO且不修改value；调用者提供有效kernel指针。`bool arch_msr_gp_fixup(pt_regs_t *regs, uint64_t error_code)` 只接受CPL0、error_code=0、RIP等于safe rdmsr汇编标签；命中只把RIP移到error返回标签，其余返回false。

- [ ] **Step 1:** production匹配fixture断言标记RIP/kernel/err0命中；其他RIP、用户CS、非零error均false且regs不变。kernel selftest读取已存在且可读MSR，再访问QEMU拒绝的保留索引0xffffffff，断言-EIO和值不变；若QEMU意外实现该索引本项不得通过（记录未验证并让验证门失败），不能把成功当成恢复已验证。自测在IF=0下置本核need_resched及无害待处理softirq后执行safe probe，验证仍恢复、pending未被IST出口消费；再恢复IF并等普通timer IRQ确认pending在正常task栈消费，断言当前task与栈哨兵未损坏。注册根 `test-thermal-host CASE=fixup`，接入TEST_BINS与selftest列表。

关键断言（变量由本步骤的 fixture 提供）：

```c
assert(!arch_msr_gp_fixup(&unmarked_regs, 0));
assert(value_after_failed_probe == value_before_probe);
```

- [ ] **Step 2:** `make test-thermal-host CASE=fixup`，预期接口缺失；新的kernel负例在实现前不能直接运行导致主测试机挂死，先只运行host red。
- [ ] **Step 3:** 汇编按SysV ABI保存out指针，成功才写EDX:EAX，标记指令标签仅覆盖rdmsr本身。do_general_protection最前面尝试fixup，失败继续原异常处理；同时修改entry.S：仅general_protection handler返回时走 `ret_from_exception -> RESTORE_ALL`，safe修复及现有用户GP的kill重定向均不经过ret_from_intr。可在共用error_code的call后通过保存的FUNC识别GP，其他异常/IRQ出口保持原样；不在IST上执行softirq/schedule/基于RSP的current，pending留给下一次正常IRQ返回。保证helper符号/汇编加入x86源列表，aarch64不引用。
- [ ] **Step 4:** `make test-thermal-host CASE=fixup`；`make KERNEL_SELFTEST=1 test-kernel-selftest`，预期标记#GP恢复后自测可继续；保留一般#GP的host负例证据。
- [ ] **Step 5:** 提交 `feat(x86): recover only marked safe MSR probes`。

### Task 2: BSP-restricted kernel task creation

**Files:** 修改 `kernel/include/sched/task.h`, `kernel/sched/{fork,core,balance,fair,exec}.c`, `kernel/include/sched/internal.h`, `kernel/arch/x86_64/smp/smp.c`, `hosttests/Makefile`, `mk/components/run.mk`；新 `hosttests/cases/test_thermal_affinity.c`, `kernel/selftest/test_thermal_affinity.c` 及selftest登记。

**Interfaces:** `task_t.allowed_cpus` 为64位CPU mask（NR_CPUS≤8）；普通任务默认为 `(1ULL << NR_CPUS) - 1`，选择目标时与当前online且scheduler_ok的CPU集合相交，避免早期创建时固化尚未上线AP集合。INIT_TASK在静态初始化宏置非零默认mask，普通spawn_user_task在exec.c发布前初始化，普通fork在fork.c明确设默认mask而非继承服务线程限制；AP idle在smp.c置单核bit，BSP idle的mask置bit0。`int64_t kernel_thread_on_cpu(uint64_t (*fn)(uint64_t), uint64_t arg, unsigned cpu)`，返回pid或-EINVAL/-ENOMEM。指定cpu必须online、调度就绪；在fork共同创建路径发布到任务表/队列之前设置mask与初始cpu。既有kernel_thread默认允许全部可调度核；若普通任务创建时没有就绪CPU，保持现有早期BSP创建路径，指定受限cpu不就绪则-EINVAL。`unsigned sched_pick_cpu_mask(uint64_t allowed)` 返回合法cpu或NR_CPUS，声明在internal.h；旧sched_pick_cpu包装默认mask。生产enqueue_task只接受其rq对应cpu处于mask内；创建/唤醒在入队前选择目标，mask为空不能继续入队，保持blocked并报告内部错误，禁止静默换到mask外CPU。

- [ ] **Step 1:** fixtures验证cpu=NR_CPUS/离线=>-EINVAL；cpu0 worker在首次enqueue前mask=1/cpu=0；wake、选择入队cpu及双rq迁移拒绝mask外cpu；fork普通任务默认不错误继承仅服务线程限制；真实spawn/INIT_TASK/AP idle生产构造均mask非零且idle所属cpu可用。受限内核spin在双核真实QEMU中cpu=0、进程runtime连续、整体利用率约50%，补齐主计划仅观测稳定归属窗口留下的绑核验证缺口。kernel自测多轮yield/sleep/wake与balance后worker采样cpu_id始终0。注册CASE=affinity。

关键断言（变量由本步骤的 fixture 提供）：

```c
assert(worker_allowed_cpus == 1);
assert(observed_cpu_id == 0);
```

- [ ] **Step 2:** `make test-thermal-host CASE=affinity`，预期接口/字段缺失。
- [ ] **Step 3:** 提取do_fork内部创建参数而非创建后修改task指针；所有enqueue/wake/负载均衡目标选择遵循mask；不修改GS或用户ABI。BSP worker依PID监督而不长期保存未经pin的task裸指针。审计同CPU队列pick不导致受限任务在错误CPU执行，不能仅在balance过滤。
- [ ] **Step 4:** `make clean`；`make test-thermal-host CASE=affinity`；显式 `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` 验证受限单核spin整体约50%（容差±15个百分点，至少5个有效窗口）；再运行 `make KERNEL_SELFTEST=1 test-kernel-selftest` 标准4核回归，此时单核spin期望约25%（容差±15个百分点），不能沿用双核50%断言。测试按实际online CPU数推导整体目标 `100 / online_cpus`，同时断言worker始终cpu0。两轮预期退出0并记录实际SMP数及限制断言。
- [ ] **Step 5:** 提交 `feat(sched): create CPU-restricted kernel service tasks`。

### Task 3: Intel thermal backend and cache

**Files:** 新 `kernel/arch/x86_64/cpu/thermal.c`, `hosttests/cases/test_thermal_decode.c`；修改 `kernel/include/arch/thermal.h`, `kernel/arch/thermal.c`（主计划Task4引入），`kernel/Makefile`, `hosttests/Makefile`, `docs/architecture.md`；需要worker启动时修改 `kernel/sched/core.c` 的task_init中scheduler_ok发布后、无限idle loop之前，调用架构中性服务启动hook；同时修改 `mk/components/run.mk` 注册CASE=decode。

**Interfaces:** 实现强 `int arch_thermal_init(void)`、`void arch_thermal_read(struct thermal_sample *out)`；新增中性 `int arch_thermal_start_worker(void)`（弱默认返回0），准确在 `core.c:task_init` 的 `this_cpu()->scheduler_ok = 1` 后且 `while (1)` idle loop前调用，不能放在函数返回后；该hook仅启动已由register_subsys初始化的后端，不把硬件初始化写进kernel_main。新增 `int thermal_decode_core(uint64_t target, uint64_t status, int32_t *temp_mc)`，成功0、target无效/读数invalid返回-EINVAL；缓存READ_ERROR由safe读取错误产生。cache锁与sampled_ns使用同一x86 clocksource时基；使用主计划Task4新增thermal_reason，不另造Reason查询：hypervisor→HYPERVISOR、非Intel→VENDOR、缺DTS→NO_DTS、白名单外→MODEL、worker未启→NOT_READY、创建失败→WORKER_START、safe读取失败→MSR、目标无效→TARGET、有效位清零→READING、正常→NONE。

- [ ] **Step 1:** fake CPUID/MSR/clock fixture：非Intel、leaf不足、DTS位缺失、hypervisor、白名单外型号均unsupported且MSR访问次数0；白名单fixture target=100、distance=25、valid=1=>75000mC，distance=110=>-10000mC；target0/valid0=>INVALID；读MSR失败=>READ_ERROR；last_ok后失败不得state=OK；elapsed=3000000000仍非stale，3000000001=>STALE；初次worker创建失败=>READ_ERROR且无有效数值。注册CASE=decode。

关键断言（变量由本步骤的 fixture 提供）：

```c
assert(decoded_temp_mc == 75000);
assert(hypervisor_msr_read_count == 0);
```

- [ ] **Step 2:** `make test-thermal-host CASE=decode`，预期后端缺失。
- [ ] **Step 3:** 创建register_subsys init wrapper探测CPU与初始化锁/unsupported缓存；在上述可达启动hook中用Task2 cpu0线程采集；返回启动错误由调用点log_warn记录、缓存发布READ_ERROR/WORKER_START，不让optional温度服务中止正常boot。每秒相对单调deadline更新，无catch-up忙循环；使用Task1两次safe读取，锁外采集，锁内发布。unsupported无需worker；arch_thermal_read复制后锁外计算stale，仅曾OK的过期样本可保留旧值但state=STALE。缓存未有有效值时失败不产生stale数值。
- [ ] **Step 4:** Intel生产白名单每个family/model条目附Intel官方型号文档位置和已核实MSR字段/target范围；在未获取该证据前不添加条目，可交付空白名单及确定性unsupported。host测试注入白名单fixture不等于宣称该真实型号支持。拿到真实支持型号后追加条目和寄存器回放fixture；不能凭CPUID DTS位就启用。Package MSR不纳入本任务。
- [ ] **Step 5:** `make test-thermal-host`（全部CASE）、`make test-static`、`make test-qemu SUITE=monitor`、`make KERNEL_SELFTEST=1 test-kernel-selftest`，预期QEMU State=unsupported/Reason=hypervisor且无温度值；这个字段必须从缓存reason经主计划procfs映射输出，不靠测试harness猜测。真实Intel机器记录vendor/family/model、TjMax、读数有效位、sample时刻，和可信平台读数在相邻时刻比较；没有裸机证据则明确记录未验证，不以QEMU代替。
- [ ] **Step 6:** 提交 `feat(thermal): cache safe BSP core DTS readings`；文档区分软件路径完成、已入白名单型号和裸机验证结论。

## Self-Review and Handoff

Spec §3.1→Task3 facade/标签；§3.2→Task3有型号证据的解析；§3.3→Task1异常恢复、Task2创建前限制、Task3worker/cache/stale。五项Review Focus均有归属断言。计划覆盖安全软件路径；生产传感器支持的交付门槛是型号文档和裸机证据。独立计划可在主计划基础指标交付后实施；执行方式由用户评审计划后选择。
