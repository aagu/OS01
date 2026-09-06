# AArch64 UEFI PSCI SMP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在现有 UEFI 链上实现有真实 AP 确认的 PSCI SMP，通过 QEMU 1/2/4 核回归。

**Architecture:** 有边界的 DTB 解析提供拓扑和 conduit；PSCI transport 负责固件调用；AP 使用独立栈完成初始化后确认上线。启动、测试和 idle 使用分离的命令/确认协议，BSP 汇总结果。

**Tech Stack:** freestanding C、AArch64 assembly、clang/lld、GNU Make profiles、Python unittest、QEMU/AAVMF。

**Spec:** [设计方案](../specs/2026-09-05-aarch64-uefi-psci-smp-design.md)。状态：v2 评审修订稿，待实施；文档中的新目标/测试尚不存在。

## Global Constraints

- 新增头文件统一位于 `kernel/include/kernel/arch/aarch64/`，使用 `<kernel/arch/aarch64/文件名.h>` 包含；host runner 与生产源码同样使用 `-Ikernel/include`。现有头文件不在本次目录调整范围内。

- QEMU virt / Cortex-A53 / GICv2；PSCI >=0.2；验收 1/2/4 CPU。
- boot_context v2 ABI 不变；DTB 只使用 UEFI handoff 副本；禁止伪造拓扑或自动回退 spin-table。
- 仅 BSP 开启 CNTP；AP IRQ 保持屏蔽；不引入通用调度器、用户态和 IPI。
- 不修改 x86 行为，不引入 aarch64 的 libc/BusyBox/sysroot 依赖。
- 结构变化必须 `make clean`；执行前保留用户已有未提交文件，不能覆盖 roadmap/symlink 文档改动。
- 新诊断使用 `log_err/log_warn/log_info`；只为 bring-up 提供私有适配层。
- 不在写方案阶段修改内核、运行破坏性 clean 或提交用户文件。执行阶段每任务通过后仅提交本任务路径。

## 文件职责

| 文件 | 动作与职责 |
|---|---|
| `kernel/arch/aarch64/dtb.c`, 新 `kernel/include/kernel/arch/aarch64/dtb.h`, `dtb_parse.c` | wrapper/纯解析分离、拓扑验证、现有设备 accessor |
| `kernel/arch/aarch64/psci.c`, 新 `kernel/include/kernel/arch/aarch64/psci.h`, `psci_call.S` | 协议策略、可测试返回值解析、真实 SMC/HVC transport |
| `kernel/arch/aarch64/smp.c`, 新 `kernel/include/kernel/arch/aarch64/smp.h`, `smp_boot_core.c`, `kernel/include/kernel/arch/aarch64/smp_boot_core.h` | 硬件 wrapper/纯状态机分离，逐核启动、ACK、失败降级 |
| `kernel/arch/aarch64/head.S`, `linker.ld` | BSP/AP context/栈/EL/MMU 路径与布局断言 |
| `kernel/arch/aarch64/aarch64_percpu.h`, `boot_percpu.c`, 新 `kernel/include/kernel/arch/aarch64/boot_offsets.h` | 命令/ACK 契约、布局断言 |
| `kernel/arch/aarch64/gic.c`, `main.c`, `test_spinlock.c` | 接入顺序、每核 GIC、测试/idle 分离 |
| 新 `kernel/include/kernel/arch/aarch64/boot_log.h` | ARM 骨架日志适配 |
| `mk/components/run.mk`, `mk/project.mk`, `mk/components/image.mk`, `kernel/arch/aarch64/make.config` | SMP 目标、注入参数从 root 到编译器的传递 |
| 新 `tests/aarch64_dtb_test.py`, `aarch64_psci_test.py`, `aarch64_smp_test.py` | host 编译驱动、纯逻辑/状态机回归 |
| 新 `tests/aarch64_uefi_smp.py` | QEMU 矩阵与串口结果解析 |
| `tests/build_contract.sh`, `docs/boot.md`, `docs/roadmap.md` | 构建契约与用户文档 |

## Task 1：固定基线与验收 harness

**Files:** `tests/aarch64_uefi_smp.py`（新），`mk/components/run.mk`。
**Interfaces:** harness CLI：`--cpus 1 2 4 --repeat 3 --timeout 90 --firmware PATH --image PATH --qemu PATH --log-dir PATH`；默认正常模式。新 `--expect-no-ack CPU_ID` 仅接受 `--cpus 2 --repeat 1` 与 CPU_ID=1，专测预期降级；退出 0 表示当前模式的全部断言通过。`--self-test` 不要求固件或镜像参数。

- [ ] 执行 `git status --short`、`make PROFILE=aarch64-clang aarch64-uefi`、`make PROFILE=aarch64-clang test-build-contract-aarch64`，保存现有基线日志与版本，不把单核 smoke 当作 SMP 成功。
- [ ] 写串口 parser 单元测试（置于同文件并支持 `--self-test`）；完整正常 fixture 使用设计中的原样日志并加 `[smp-test] no_ack_cpu=0`，每核 done 包括 BSP，变体用删行/改计数产生。`passed(text, cpus)` 是待实现纯函数，以下 fixture 变量必须由这个完整字符串生成：

```python
assert not passed(log_with_only_uefi_banner, cpus=4)
assert not passed(log_with_online4_but_no_done, cpus=4)
assert not passed(log_with_degraded_and_ticks, cpus=4)
assert not passed(log_with_duplicate_cpu_ack, cpus=4)
assert not passed(log_with_wrong_total, cpus=4)
assert passed(complete_log_for_4_cpus, cpus=4)
```

- [ ] 实现 subprocess.Popen + monotonic deadline，stdout/stderr 保存文件；finally 中 terminate/wait，超时则 kill/wait。测试以成功标记、每核 done、精确总和及 3 条 tick 为联合判据；只识别内核结构化 FATAL/PANIC/FAIL/DEGRADED 标记，不对固件任意字符串做 FAIL 子串匹配。正常模式要求 `[smp-test] no_ack_cpu=0`，这些标记优先判失败；负例模式要求 `requested=2 online=1 status=DEGRADED`、`cpu=1 reason=online-timeout`、`[spinlock] status=SKIP`、`[smp-test] no_ack_cpu=1` 及后续 3 条 tick，禁止出现任何 benchmark PASS。使用 selectors/非阻塞读取，不能让无输出的 readline 绕过 90 秒期限。
- [ ] 运行 `python3 tests/aarch64_uefi_smp.py --self-test`；执行旧内核四核 case 应 FAIL（缺少 AP ACK），这是功能 RED 基线。
- [ ] 提交：`test(aarch64): define UEFI SMP acceptance harness`。

## Task 2：真实 DTB 拓扑和边界校验

**Files:** `kernel/arch/aarch64/dtb.c`、新 `kernel/include/kernel/arch/aarch64/dtb.h`、`kernel/arch/aarch64/dtb_parse.c`、`tests/aarch64_dtb_test.py`。
**Interfaces:** 实现设计中的 `aarch64_topology`、`aarch64_platform_info`、`aarch64_dtb_parse`；运行 wrapper 维持现有设备 accessor。

- [ ] Python 用 struct.pack 构造真实 FDT header/structure/strings，所有成功 fixture 都包含固定平台设备节点；host C runner 在临时目录生成，用 `cc -std=c11 -Wall -Wextra -Werror -Ikernel/include kernel/arch/aarch64/dtb_parse.c <runner.c> -o <runner>` 编译同一生产纯解析源码。加入以下用例：

```python
cases = [
    ('one_cpu_no_psci', 0), ('four_cpu_hvc', 0),
    ('two_cells_affinity_0x100', 0), ('aff3_0x100000000', 0),
    ('bsp_not_first', 0), ('missing_device', -4), ('wrong_gicc', -4),
    ('disabled_cpu_ignored', 0), ('duplicate_mpidr', -2),
    ('capacity_plus_one', -2), ('missing_bsp', -2),
    ('truncated_property', -1), ('nameoff_outside_strings', -1),
    ('unterminated_string', -1), ('invalid_method', -3),
    ('multi_cpu_without_psci', -3), ('wrong_enable_method', -3),
]
```

- [ ] 运行 `python3 tests/aarch64_dtb_test.py`，确认实际行为错误/缺接口导致 RED。
- [ ] 将硬件读取和纯解析拆开；先以 `off <= size && len <= size-off` 验证所有区间，token 读取要求剩余 >=4，property header >=8；字符串必须在所属区间找到 NUL；depth 最大 32，拒绝不完整结构。仅验证成功后提交输出。
- [ ] 成功 fixture `aff3_0x100000000` 同时含 MPIDR 0 和 0x0000000100000000，断言输出保留两个独立 CPU，不能只断言 rc。实现 `(uint64_t)be32(hi)<<32 | be32(lo)`，按 affinity mask 归一化并把 BSP 排第一；有容量溢出立即报错。compatible 处理 NUL 分隔字符串列表；method 必须精确匹配。
- [ ] wrapper 在解引用 header 前检查 handoff 地址，再检查 totalsize；删除 UEFI 路径候选地址扫描/合成四核。同步修复同一 walker 的 GIC/PL011 reg 解码和 unit-address 节点识别，设备地址/长度按设计限定验证；不改变当前 reg.h 的 IRQ accessor 基址契约。DTB HAS_DTB flag 缺失或 blob 错误时明确 FATAL，在 GIC/PSCI 调用之前结束。
- [ ] 重跑 host 用例及单核 UEFI smoke；提交：`fix(aarch64): validate UEFI CPU topology and PSCI binding`。

## Task 3：PSCI transport 和版本契约

**Files:** `psci.c`、新 `kernel/include/kernel/arch/aarch64/psci.h`、`psci_call.S`、`tests/aarch64_psci_test.py`。
**Interfaces:** `psci_init(conduit)` 返回 0 成功/-1 不可用；`psci_cpu_on` 返回 int32_t PSCI 状态。内部 transport：`uint64_t psci_call_smc(uint64_t fid,uint64_t a1,uint64_t a2,uint64_t a3)`，HVC 同签名。

- [ ] 用 host stub 捕获 fid/a1/a2/a3，验证 CPU_ON64、不截断高位地址、NONE 不执行调用、0.1 拒绝、0.2/1.0 接受、负返回值不被当作版本。运行 `python3 tests/aarch64_psci_test.py` 确认 RED。
- [ ] 新建叶函数：

```asm
.text
.global psci_call_smc
psci_call_smc:
    smc #0
    ret
.global psci_call_hvc
psci_call_hvc:
    hvc #0
    ret
```

- [ ] C 选择已验证 conduit；VERSION 原始低 32 位先检查错误，再解析 major/minor。CPU_ON 使用 `UINT64_C(0xc4000003)`；移除旧错误注释和内联 asm。
- [ ] host 测试通过；交叉编译并用 llvm-objdump 核实 smc/hvc #0 与 ret，确认外部调用保留 ABI。此测试证明编码，实际固件调用由 Task 5/7 验证。
- [ ] 提交：`fix(aarch64): implement PSCI SMC and HVC call ABI`。

## Task 4：AP 入口和共享数据发布

**Files:** `kernel/arch/aarch64/head.S`、`linker.ld`、`aarch64_percpu.h`、新 `kernel/include/kernel/arch/aarch64/boot_offsets.h`、`boot_percpu.c`、`smp.c`、`tests/aarch64_smp_test.py`（源码简称相对 kernel/arch/aarch64；新增头文件使用列出的完整路径）。
**Interfaces:** 48 字节槽位保持；online=AP ACK，go=0等待/1测试/2 idle；context_id=逻辑槽索引。

- [ ] 添加 sizeof/offsetof 编译断言，布局检查断言 online 与 go 的 offset 分别为 32/36；本任务的 `aarch64_smp_test.py` 先提供交叉构建/ELF 检查，生产状态机测试于 Task 5 加入。构建检查 AP 地址属于可执行低物理 PT_LOAD，所有栈不重叠且 16 字节对齐，所有 LOAD p_paddr+p_memsz <=0x401e0000。linker.ld 添加 `_kernel_lma_end <= 0x401e0000` 断言；vector 地址 0x800 对齐。
- [ ] 移除 UEFI `_start` 的 Aff0→spin-table 分流。AP 入口保存 x0，EL1/EL2 规范化，EL1 显式 `msr spsel,#1`，EL2 的 HCR/CNTHCTL/CNTVOFF 配置与 BSP 一致，全程 DAIF 屏蔽；在任何压栈/函数调用前无栈验证索引和 MPIDR，再设置独立 SP。删除共享 scratch stack 和 online 等待环；32 位 count 用 `ldr wN`。
- [ ] 用已发布 BSP 页表启 MMU，不清 BSS；TPIDR 槽位和高半区 C 使用一致索引。无效 context 走无栈错误停驻，BSP 超时报告目标 MPIDR。
- [ ] BSP 初始化全部槽位后，按 CTR_EL0 导出的行长一次性 clean 页表与元数据到 PoC，`dsb sy` 后才允许 Task 5 调用 CPU_ON；此后禁止整区 clean/清零，所有槽位使用 identity VA，不提前写 AP online。Task 4 只提供发布 helper，不在此任务提前接入启动调用。
- [ ] 运行 `python3 tests/aarch64_smp_test.py` 和 ARM kernel 构建/ELF 检查；汇编路径完整行为留至下一任务的真实两核启动。提交：`fix(aarch64): establish independent PSCI secondary entry`。

## Task 5：启动状态机、GIC 和主入口集成

**Files:** `kernel/arch/aarch64/smp.c`、新 `kernel/include/kernel/arch/aarch64/smp.h`、`smp_boot_core.c`、`kernel/include/kernel/arch/aarch64/smp_boot_core.h`、`gic.c`、`main.c`、新 `kernel/include/kernel/arch/aarch64/boot_log.h`、`test_spinlock.c`、`tests/aarch64_smp_test.py`。
**Interfaces:** `smp_boot_aps()` 返回期限内 ACK 的冻结在线数量；`gic_cpu_init()` 仅本核配置。此任务一次性把 `secondary_idle(id)` 移入 smp.c，导出 `smp_bench_iter(uint32_t,uint32_t)`，统一 `bool test_spinlock_smp(uint32_t)` 声明和定义，供 Task 6 加强测试。不得在相邻任务保留签名不一致或重复符号。

- [ ] 创建设计中 `smp_boot_ops`、`smp_boot_result`、`smp_boot_run()` 的完整接口；生产 `smp_boot_core.c` 不包含 `cpu.h`/`aarch64_percpu.h`，硬件指令 adapter 留在 `smp.c`。Python 在临时目录生成 C runner，用 `cc -std=c11 -Wall -Wextra -Werror -Ikernel/include kernel/arch/aarch64/smp_boot_core.c <runner.c> -o <runner>` 调用实际生产函数。
- [ ] runner 用事件队列和假的 counter/CPU_ON/ACK 测试成功、错误码、ALREADY_ON 无 ACK、ON_PENDING 延迟 ACK、超时、迟到 ACK、频率 0、counter 回绕；验证 `online_mask` 精确、BSP 未被 CPU_ON、每 AP 最多调用一次、失败向全部 AP 发 go=2、成功没有提前发 go=1。回绕 case 使用起始 `UINT64_MAX-100` 和小频率，不依赖 host 墙钟。Aff3 测试必须断言 CPU_ON 捕获完整 MPIDR。
- [ ] 按设计的 `smp_boot_run` 返回值与冻结 mask 契约实现，先运行上述 runner 确认 RED，再实现生产函数。关键分支如下（计时起点在 CPU_ON 前，测试期限在接受 ACK 前检查）：

```c
/* 每个 AP：启动之前 online 必须为 0。 */
rc = psci_cpu_on(mpidr, entry_pa, cpu_id);
if (rc == 0 || rc == -4 || rc == -5) {
    /* 若 (uint64_t)(now-start) >= freq*2 则 TIMEOUT；
     * 否则 acquire 读取 ACK，成功则计入冻结 mask。 */
} else {
    /* 保存 rc，记录该核失败，继续下一核。 */
}
/* 有任一失败：所有 go release-store 2，跳过 benchmark。 */
```

- [ ] 删除 `benchmark_go` 与 `bench_go_set/get`，把 BSP/AP 所有等待及发布切为设计中的每槽 go，done 保持单独确认；AP 在线后在 go=0 时 yield，go=1 调 `smp_bench_iter(id,1000000)`，go=2 直接 idle。调整全部声明/调用，避免旧全局门导致 AP 永久等待。
- [ ] 拆出 GIC CPU 初始化，AP 不写 distributor 控制；在 C 中使用 DTB 基址，删除 AP 汇编硬编码 GICC。AP 初始化完成才 release-store online。
- [ ] 接入设计规定的主入口顺序。添加只依赖 PL011 的日志适配，BSP 汇总 AP 日志；单核不要求 PSCI 调用；多核 PSCI init 失败但 DTB 有效时全部 go=2、输出 DEGRADED 后继续 BSP tick；counter frequency=0 为 FATAL。
- [ ] 实际 UEFI 两核验证两个独立 CPU 执行证据；降级后开启 BSP tick。提交：`feat(aarch64): bring up UEFI secondary CPUs with acknowledgements`。

## Task 6：共享计数验收与故障注入

**Files:** `kernel/arch/aarch64/test_spinlock.c`、`smp.c`、`make.config`；`mk/project.mk`、`mk/components/image.mk`、`mk/components/run.mk`；`tests/aarch64_smp_test.py`、`tests/aarch64_uefi_smp.py`。
**Interfaces:** 消费 Task 5 已统一的 `bool test_spinlock_smp(uint32_t active)` 和 `void smp_bench_iter(uint32_t cpu_id, uint32_t iterations)`；本任务不再迁移入口。

- [ ] 测试缺任何 done、总和少 1、重复 ACK、online 数不足均 FAIL；单核总和为 1000000，不宣称多核验证。
- [ ] 全部 AP ACK 后才发布 go=1；测试按一致拓扑索引运行，done 用 release/acquire，总和在锁内读取；BSP 输出每核 done 和设计中的结果标记。测试等待期限从真实 counter frequency 计算，不能保留固定 62MHz 常量。
- [ ] 核查 Task 5 的 AP 生命周期，保证 go=1 只跑一次，go=2 不碰共享计数；初始化所有 go/done 发生在 CPU_ON 前。benchmark 30 秒期限从发布 go=1 前计算；若仍有核未 done，不再阻塞获取 bench_lock 读取总和，输出 FAIL 并由 host 判失败。
- [ ] 增加仅测试构建启用的 `AARCH64_SMP_TEST_NO_ACK_CPU`（默认 0，无注入）用于指定非 BSP 核不发布 ACK。固定采用 profile clean 重建，禁止依赖当前未支持的 BUILD_DIR override。注入参数加入 `mk/project.mk` 白名单，`mk/components/image.mk` ARM kernel 的 os01_submake 显式追加 `$(OS01_SUBMAKE_ARGS)`，`arch/aarch64/make.config` 校验 0..7 并加入 `-DAARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU)`（默认 0）。注入核仍进 C 初始化，但跳过 online 写入，继续观察 go 并在 idle 命令后停驻。BSP 必须打印实际编入的注入值。
- [ ] 同时在 run.mk 增加能力隔离的 `test-aarch64-uefi-smp-no-ack` 目标，只消费事先以注入值 1 构建的镜像（目标校验顶层值必须为 1），传 harness `--cpus 2 --repeat 1 --expect-no-ack 1`。QEMU 两核注入 cpu1 不 ACK，要求 `DEGRADED`、没有 benchmark PASS、继续 3 条 tick，90 秒内结束。明确重建顺序：

```sh
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
```

日志放 `test-results/aarch64-uefi-smp/<run-id>/`，固件 hash/注入值记入元数据；clean 后使用重新构建的路径，并运行正常两核恢复测试。
- [ ] 提交：`test(aarch64): verify SMP execution and startup timeout recovery`。

## Task 7：profile 目标、完整矩阵与文档

**Files:** `mk/components/run.mk`、`tests/build_contract.sh`、`tests/aarch64_uefi_smp.py`、`docs/boot.md`、`docs/roadmap.md`。
**Interfaces:** 新目标 `test-aarch64-uefi-smp`，仅 uefi-bringup capability；使用当前 profile 的 firmware/image 路径，不硬编码 build 输出目录。

- [ ] 把 `run-aarch64-uefi` 的 `-smp 1` 改为 `-smp $(AARCH64_SMP)`，help 说明覆盖方式；harness 显式逐项传 1/2/4。QEMU 使用现有 pflash + virtio-blk 启动命令，加只读固件和镜像 snapshot，避免多次测试写共享镜像。
- [ ] 加能力边界回归：x86 profile 调新 ARM 测试目标明确报缺能力，不启动 ARM 构建；ARM 测试目标不能拉入 BusyBox/sysroot。
- [ ] 运行最终命令：

```sh
python3 tests/aarch64_dtb_test.py
python3 tests/aarch64_psci_test.py
python3 tests/aarch64_smp_test.py
python3 tests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang aarch64-uefi
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=aarch64-clang test-build-contract-aarch64
make PROFILE=x86_64-clang test
```

新 Make 目标执行 1/2/4 核 ×3 次完整验收，并传解析后的真实固件/镜像绝对路径。任何一次失败保留日志，不能仅重跑到绿后删除失败记录。汇编/结构发生变化时先执行仓库要求的 clean 再重建。

- [ ] 文档记录实际 QEMU/固件版本、conduit、日志路径、命令和已知范围。roadmap 仅调整 P2 对应行，保留当前用户修改；历史 spec 保留历史身份，由新设计说明覆盖关系。
- [ ] `git diff --check` 和精确文件 diff 审查后提交：`feat(aarch64): expose verified UEFI SMP regression target`。

## 自查与交付

### v2 自审修正记录

1. 固定 host 纯解析 translation unit 和包含设备信息的事务式输出，消除只返回拓扑却修改设备全局状态的歧义。
2. 将 AP 生命周期/benchmark API 的迁移集中到 Task 5，删除第二个 go 门，消除 Task 5/6 的依赖倒置。
3. 明确注入参数白名单、ARM 子 make 显式传参和 -D 链，采用 clean 重建，并添加负例 harness 模式。
4. 限定固定 MMIO 平台，补充非零 BSP affinity、EL1 SPSel、EL2 timer offset、LOAD 边界和一次性 cache 发布。
5. 区分无平台 DTB 的 FATAL、有效平台的 SMP DEGRADED，以及正常/预期故障两类测试成功条件。

子 agent 评审补充的两个 P2 已纳入：生产状态机的 host 注入接口→Task 5；Aff3 与 MPIDR=0 共存的解析断言→Task 2。Task 3 的调用参数保真不能替代 Task 2 的 DTB 解析验证。

设计覆盖映射：DTB→Task 2；PSCI→Task 3；栈/页表/可见性→Task 4；真实 ACK/超时/GIC→Task 5；执行证明/负例→Task 6；profile 和矩阵→Task 1、7。

独立子 agent 于 2026-09-05 复核 v2：未发现剩余 P1/P2 文档矛盾或实施阻塞。此结论仅覆盖设计/计划，不代表代码或 QEMU 验收通过。

实施交付需给出 9 次正常启动结果及注入失败恢复结果。若环境不提供所需固件或某 conduit，只报告已覆盖部分，不把 mock 调用包装成真实 SMC/HVC 双通道验收。
