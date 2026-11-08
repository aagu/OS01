# BOOT-4 Stage Contracts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 双架构公共启动边界可核验，注册阶段和同阶段依赖明确，CPU准备API准确表达执行位置。

**Architecture:** 拆开controller/context/device/SMP/runtime边界，保留架构内部IRQ与admission协议；固定表通过有界preflight和稳定扫描满足依赖。checker读真实对象，不另建ready位；删除LAPIC空callback，旧percpu API兼容映射为BSP CPU准备。

**Tech Stack:** freestanding C、Clang/LLVM、x86_64/AArch64、固定注册表、C hosttests、Python unittest、QEMU/UEFI及现有make profile/variant harness。

**Spec:** [BOOT-4设计 R2](../specs/2026-11-08-boot-4-stage-contracts-design.md)，`boot4_spec_review` 已 APPROVE。基线 `eebecfbf`，实施前重核对HEAD。本文R2，独立 reviewer `boot4_plan_review` 已 APPROVE；见[评审记录](../reports/2026-11-08-boot-4-doc-review.md)。本轮仅文档，checkbox均为未来工作。

## Global Constraints

- BSP表64、CPU prepare表16、每项依赖最多8；零依赖API兼容，元数据错误即使OPTIONAL也fatal。
- 命名phase值CONTROLLERS=3/TIME=4/DEVICE_IRQ=5/STORAGE=6；数字别名保留，整体boot步骤不复用这些编号。
- 不改boot_context/UAPI/percpu/task/pt_regs布局、CPU admission/资格发布、BOOT-2 RTC/PIT/LAPIC硬件偏序。
- x861/1与ARM0/0为本项产品配置；保留USERSPACE⇒VFS编译约束，不增加ARM用户态支持。
- 不在checker修复状态/放IRQ/启动timer；ARM软件phase提前但CNTP/IRQ/IPI/shootdown仍在AP收束后；softirq_init先于timer_init且各一次。
- root/TTY/PTY/stdio/PID1必需失败仍按BOOT-3停止，task_init/服务拆分与VFS bootstrap退出留BOOT-5。
- error沿用BOOT-3五参数stop/warn，状态0/1/-1/未知-999，环码-ELOOP=40，不扩张errno镜像。
- operational日志用log_*；ARM不使用不支持的signed formatter，fatal交BOOT-3处理。
- 新增源/头位置对称；新增架构source纳入各profile；删除文件检查显式与自动发现两条构建路径。
- KERNEL_SELFTEST关闭传空；systest与kernel-selftest分开；所有测试构建从根make入口，产物隔离。

## Review Focus

1. PIC读取尚未初始化的IOAPIC、fault dump先于设备probe（Task1逆序生产wrapper测试）。
2. OPTIONAL生产者失败使required消费者误跑；元数据环上游被误报环成员（Task1失败与实际环成员测试）。
3. BSP GS/TPIDR无效时checker先解引用this_cpu，或把inactive clocksource当失败（Task3真实checker负例）。
4. ARM softirq重置抹掉timer action；提早BSP IRQ或修改smp_starting/admission（Task3实际timer对象与双架构轨迹）。
5. normal混入fixture、fatal靠超时通过、ARMrunner错用x86镜像（Task4 artifact审计与parser负例）。

## 文件及接口归属

| 任务 | 创建/修改/删除 |
|---|---|
| 1 | 修改 `kernel/subsys/subsys.c`、`kernel/include/subsys/subsys.h`；依赖wrapper：`kernel/arch/x86_64/intr/pic_8259a.c`、`kernel/driver/pit.c`、`kernel/arch/x86_64/intr/apic/lapic_timer.c`、`kernel/arch/thermal.c`、`kernel/device/test_fault.c`；新增 `hosttests/cases/test_boot_contract_subsys.c`；hosttests/Makefile、mk/components/run.mk |
| 2 | 修改subsys源/头、`kernel/arch/x86_64/smp/boot.c`、`kernel/include/arch/subsys.h`；删除 `kernel/arch/x86_64/smp/subsys_percpu.c`；新增 `hosttests/cases/test_boot_contract_cpu_prepare.c`；清理kernel/Makefile（若显式列出）及测试mock |
| 3 | 修改 `kernel/core/main.c`、`kernel/include/arch/boot.h`、`kernel/arch/x86_64/platform/boot.c`、x86 smp/boot.c及两个boot头、`kernel/arch/aarch64/boot/main.c`；新增 `kernel/arch/{x86_64,aarch64}/platform/boot_contract.c`（接口统一声明于已有 `kernel/include/arch/boot.h`）；getter修改原模块及对称公开头；新增 `hosttests/cases/test_boot_contract_{sequence,checks,softirq}.c`、硬件边界mock；kernel/Makefile、hosttests/Makefile、qemutests/test_boot_facade_headers.py |
| 4 | 新增 `kernel/selftest/boot_contract_fixture.c` ↔ `kernel/include/selftest/boot_contract_fixture.h`、`qemutests/boot_contract.py`、`qemutests/test_boot_contract.py`；修改双架构controller注册接线、kernel/Makefile、mk/profiles/{x86_64-clang,aarch64-clang}.mk、mk/project.mk、mk/components/{kernel,image,run}.mk、mk/tests/catalog.mk、qemutests/scenario_catalog.py及对应catalog/dryrun/隔离测试 |
| 5 | 修改 `docs/architecture.md`、`docs/arch.md`、`docs/subsys/subsys.md`、`docs/smp/smp.md`、`docs/build/build.md`、`docs/roadmap.md`、`AGENTS.md`；新增未来验收报告 `docs/superpowers/reports/2026-11-08-boot-4-validation.md` |

新增focused入口 `make test-boot-contract-host CASE=subsys|cpu-prepare|sequence|checks|softirq|all`（默认all）。五个binary独立依赖，不因后续尚不存在的文件而阻断前项RED。统一加入test-host；Python负向测试加入test-harness。错误CASE解析失败。

## Task 1: 命名阶段与固定表依赖执行

**Interfaces:** 新增 `int register_subsys_deps(const char *name, int (*init)(void), int phase, uint32_t flags, const char *const *requires, unsigned require_count)`；旧register_subsys→零依赖wrapper；subsys_entry内部追加静态生命周期requires/count。旧dispatch/query签名不变；新增phase命名及旧值别名。

- [x] **Step 1 — 行为RED。**
- [x] **Step 2 — preflight RED。**
- [x] **Step 3 — 实现。**
- [x] **Step 4 — GREEN。**
- [x] **Step 5 — Commit。** `feat(subsys): name boot phases and enforce bounded dependencies`。

## Task 2: BSP CPU数据准备语义与空callback删除

**Interfaces:** 新 `int register_subsys_cpu_prepare(const char *name, int (*prepare_for_cpu)(int target_cpu), uint32_t flags)`、`void subsys_prepare_cpus(void)`、`int subsys_cpu_prepare_result(const char *name, uint64_t *failed_mask)`；旧三API为同表wrapper。内部entry/type改cpu_prepare；本项无on_cpu硬件执行API。

- [x] **Step 1 — RED。**
- [x] **Step 2 — 验证RED。**
- [x] **Step 3 — 实现。**
- [x] **Step 4 — GREEN。**
- [x] **Step 5 — Commit。** `refactor(subsys): distinguish BSP CPU preparation from local init`。

## Task 3: 公共钩子重排与真实能力核验

**Interfaces:** 在arch/boot.h定义 `enum arch_boot_checkpoint` 三值DEVICES/PRE_SMP/RUNTIME（前缀ARCH_BOOT_CHECK_）；新 `void arch_boot_controllers(const struct boot_context *)`、`void arch_boot_devices(const struct boot_context *)`、`void arch_boot_smp_prepare(void)`、`void arch_boot_runtime(void)`、`int arch_boot_validate(enum arch_boot_checkpoint, const char **component)`。删除旧public/private subsystems入口，拆分x86 helper命名为 `x86_64_boot_controllers/devices`，保留percpu与aps public。

新增只读getter精确冻结如下（在原模块实现，对称头声明，不写新ready状态）：

| 签名 | 源/头 | 真实状态与边界 |
|---|---|---|
| `bool sched_accounting_is_ready(void)` | sched/accounting.c ↔ include/sched/accounting.h | acquire既有accounting_ready，不以scheduler_ok推导 |
| `bool pit_started(void)` | driver/pit.c ↔ include/driver/pit.h | 读取既有started flag，只证明曾启动，单独不能证明route enabled |
| `bool ioapic_irq_unmasked(uint32_t gsi)` | arch/x86_64/intr/apic/ioapic.c ↔ include/intr/apic.h | published ready、已分配route、idx/pin合法后在既有controller锁下读RTE MASK=0；未知false，不写RTE |
| `bool pic_irq_unmasked(unsigned isa_irq)` | arch/x86_64/intr/pic_8259a.c ↔ include/intr/pic.h | <16、IMR对应位=0；slave还检查master IRQ2=0；只读IMR |
| `bool lapic_timer_local_running(void)` | arch/x86_64/intr/apic/lapic_timer.c ↔ include/intr/apic.h | apic_available先于MMIO；LVT未mask、periodic、正确vector、INIT>0；不读瞬时CURRENT来判停 |
| `bool aarch64_timer_config_ready(void)` | arch/aarch64/platform/time.c ↔ include/arch/aarch64/timer.h | acquire既有g_timer_config.ready，配合现有local_running使用 |

x86 checker组合PITstarted+实际selected controller的ISA0 route与LAPIC getter，PRE_SMP/RUNTIME要求两者恰一可递送，不将PIT曾启动flag当作仍unmasked。DEVICES只要求BOOT-2 runtime_opened，不要求尚未进行的LAPIC handoff。新getter及checker的硬件read/privileged primitive为host mock边界，算法和条件编译真实源码。

- [x] **Step 1 — RED轨迹/对象。**
- [x] **Step 2 — RED checker与getter。**
- [x] **Step 3 — 实现。**
- [x] **Step 4 — GREEN。**
- [x] **Step 5 — Commit。** `refactor(boot): enforce shared stage contracts across architectures`。

## Task 4: 双架构14组编排矩阵

**Interfaces:** `BOOT_CONTRACT_TEST=none|reverse|missing|cycle`；`void boot4_test_register(void)`仅variant编译，双架构controller内arch_register_subsys之后、首phase dispatch之前调用（BOOT-3 registration fixture仍保留原位置）。普通header inline空宏不发日志、不链接fixture。runner CLI `--arch x86_64|aarch64 --case normal|reverse|missing|cycle --cpus N --image PATH --firmware PATH --kernel-elf PATH --log-dir PATH`；纯函数 `parse_serial(text, arch, case, cpus)`返回结构化结果，QEMU启动/归档与解析分离。

- [x] **Step 1 — RED parser。**
- [x] **Step 2 — RED build契约。**
- [x] **Step 3 — 实现。**
- [x] **Step 4 — GREEN。**
- [x] **Step 5 — Commit。** `test(boot): validate cross-architecture stage and dependency contracts`。

## Task 5: 整体回归与公开契约同步

**Interfaces:** 不新增运行API；交付验证报告逐项区分新矩阵、旧回归、未运行项/阻塞，roadmap只有取得实际证据后才标已验收。

- [x] **Step 1 — 建立验收清单。**
- [x] **Step 2 — 执行通用/x86门禁。**
- [x] **Step 3 — 执行ARM门禁。**
- [x] **Step 4 — 更新文档并自审。**
- [x] **Step 5 — Commit。** `docs(boot): record BOOT-4 stage contracts and validation`。

## 计划自审

覆盖：spec§3公共边界与特性门→Task3；§4能力检查→Task3；§5依赖/阶段→Task1；§6执行位置→Task2；§7文件/文档→各任务及Task5；§8 host/矩阵/回归→Task1～5。5个任务、25个步骤；先行为RED再实现，首次无接口的编译失败独立记录。文档批准不表示以上代码、测试或验收已完成。
