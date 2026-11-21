# GICv2 Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标**　交付 AArch64 SError 诊断与停驻、GICv2 Non-secure 固件交接及未用 INTID 覆盖。

**架构**　正式 EL1 向量保存 `pt_regs` 后进入无锁致命诊断。GICv2 core 保持 mock MMIO 可测，BSP/AP 发布探测到的物理 target。软件和真机按各自证据验收。

**技术栈**　C、AArch64 汇编、Clang/LLVM、QEMU virt GICv2、UEFI、Python unittest。

**Spec**　[2026-11-21-aarch64-gic-phase2-design.md](../specs/2026-11-21-aarch64-gic-phase2-design.md)

## 全局约束

- 入口限于正式 EL1 VBAR `+0x180`、`+0x380`、`+0x580`，保持 272 字节 `pt_regs` ABI，正式 EL0 PSTATE 为 `0x240`。
- 致命诊断保留原始 ESR/ELR/SPSR，固定 `far=n/a`，共用 EL1h sync 首错 claim 和早期 UART。匹配 ELF 离线符号化，保留 `kernel/core/trace.c` 现有 x86 用法。
- GICv2 最大 1020 个可注册 INTID，按 TYPER 截断。1020–1023 跳过 handler lookup、unexpected 和 EOIR。有效 ack 原样写回。
- Non-secure GICv2 由固件提供 Group 1，EL1 跳过 IGROUPR。控制寄存器 bit 0，EOImodeNS=0，SGIR NSATT=0。
- BSP target 在 SPI 和 AP 启动前已发布。AP GIC 验证在 runtime online、ACK 与 IPI-ready 前完成。
- 新宿主或 harness 测试按仓库元数据方式登记。所有命令从仓库根执行，除非步骤指定另一个工作目录。

## 评审重点

- 固件保留安全优先级 PMR，AP 停于 ACK 前且不会被纳入可用核集合（任务 1–2）。
- DT 只启一个 CPU、GIC 有多个 interface，仍须读实际 target 并拒绝全零（任务 2）。
- Non-secure 读到部分 banked 字段 RAZ，合法 target 仍可恢复，冲突与多位值拒绝（任务 2）。
- 错栈、未实际开启 A、错误 ERET 后又异常停住，各自必须能让验收失败（任务 3–4）。
- 实机拿不到 Group 1 递送或真正 SError 时，档案可区分未执行、运行失败与已验证（任务 5）。

## 任务 1　GICv2 访问视图和 handler 边界

**文件**

- 修改 `kernel/include/arch/aarch64/gic.h`、`kernel/arch/aarch64/intr/gic_driver.c`、`kernel/arch/aarch64/intr/gic.c`。
- 测试 `hosttests/cases/test_gic_driver.c`，在 `hosttests/mk/intr.mk` 为该测试单独编译既有 `OS01_HOST_GIC_MMIO_TRACE` 版本。

**接口**

- `struct gic_dev` 缓存 `bool security_extn`、`uint8_t nr_cpu_interfaces`，来源分别为 TYPER bit 10、TYPER[7:5]+1。
- `int gic_dev_cpu_enable(struct gic_dev *dev)` 成功返回 0，安全扩展视图 PMR 写入受阻返回 -1。`void gic_cpu_init(void)` 保持调用接口，失败路径停驻。

- [ ] 先写带地址和写值的 MMIO 测试。保留现有模拟写语义，支持丢弃 PMR 写，证明失败时 GICC_CTLR 未启用。用该测试专用的读钩子与既有写钩子分别统计。分别跑 TYPER `0x001f` 和 `0x041f`，向 IGROUPR 预置全 1，记录 Group 0 的读改写，断言 Group 1 的 IGROUPR 读写计数都为 0。核对 PMR=0xff 在 CPU CTLR=1 前，Distributor CTLR=1，SGIR 的 NSATT 位始终为零。模拟 PMR 对 NS 可见的部分位读回（如 0xf8）与全零读回。
- [ ] 增补 dispatch 用例。对 `{5,15,16,29,31,1019}` 逐个注册、模拟 IAR、校验收到相同 INTID/param，注销后复用同槽；重复注册返回 -2，未注册注销返回 -3。TYPER=0 时 31 合法、32 拒绝。对 1020–1023 逐个断言回调和 EOIR 写计数均为零。1019 未注册应回调并 EOI；原始 SGI IAR `0xc05` 应原值写回。
- [ ] 运行 `make test-host`，记录新断言在旧实现上的失败。接口变更所需的 PMR 结果断言在最小签名接线后跑红，确保返回码自身被验证。
- [ ] 在 core 实现 TYPER 缓存和配置分支。仅 Group 0 路径做组寄存器清位。PMR 写后读回检查低 8 位，失败时保持 CPU interface 未由 OS 启用。若由固件提前启用，失败路径也不得让本核开 IRQ 或继续启动。保留真实 ack 到 EOI 的顺序和原值，特殊 INTID 在 dispatch 最前面返回。
- [ ] 在 wrapper 用 `log_err` 记录 PMR 交接失败及硬件 MPIDR，全屏蔽停驻。现有 AP 调用已在 online 前。两种视图都保证配置写在本地 IRQ 开启前完成。
- [ ] 运行 `make test-host`。运行 `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi`，要求真实 PL011 RX 递送通过。提交本任务的驱动与测试改动。

## 任务 2　BSP/AP target 探测、发布与实际消费者

**文件**

- 修改 `kernel/arch/aarch64/intr/{gic.c,gic_driver.c,ipi.c,ipi_test.c,irq_probe.c,spi_test.c}`、`kernel/include/arch/aarch64/{gic.h,ipi.h}`、`kernel/arch/aarch64/platform/console.c`、`kernel/arch/aarch64/smp/smp.c`。
- 测试 `hosttests/cases/test_aarch64_gic_target_probe.c`、`hosttests/cases/test_double_state_publish.c`、`hosttests/cases/test_smp_starting_gate.c`。

**接口**

- 新增 `uint8_t gic_cpu_target_mask(const struct gic_dev *dev, bool single_cpu_topology)`。返回已验证的单 bit，0 表示非法。既有 `gic_target_bit_init(uint32_t cpu_id)` 和 `gic_target_bit_get(uint32_t cpu)` 继续负责发布与读取。
- 消费任务 1 的 `dev->gicd` 与 `dev->nr_cpu_interfaces`。外部调用显式提供 BSP route，低层 `gic_irq_enable()` 保留已有调用语义。

- [ ] 先写 target 用例。对多接口 mock 测试 `[0,0x04,0x04,0]` 得 0x04、两核 target `{0x04,0x02}` 的逻辑 mask 3 编码 SGIR target 0x06、`[0x04,0x02]` 与 `[0x05]` 得 0。同一物理位分配给两个逻辑核时第二次 fatal，较早槽保持，后来槽为 0。DT 单核加多接口时可接受 0x04 且全零失败；DT 单核与 TYPER 单接口且全零才回退至 1。保留 cpu 越界/广播 mask 越界的断言。独立运行不同拓扑 fixture，隔离 claim 和槽缓存，避免借用清空单槽来重置真实 claim。将旧测试对 `ipi.c` 出现 `dtb_gicd_base()` 的字符串断言改为基址行为断言，mock 只通过 `gic_dev_current()->gicd` 提供有效 GICD，真实访问路径取 `gic_init()` 已建立的高半窗口。
- [ ] 改旧顺序测试。断言 `arch_smp_ap_cpu_init` 完成 GIC target 校验，公共 `smp_ap_common_entry` 随后发布 online，`arch_smp_ap_post_online` 才发 ACK。保留 ACK → go → 开 IRQ → IPI-ready 的断言，更新旧测试把 target 绑定在 ACK 后的假设。
- [ ] 运行 `make test-host`，保存非 identity 或启动顺序断言的 RED。
- [ ] 在 GIC core 从已映射的 `gicd + 0x800` 读 8 个 32 位字，逐字节折叠。零字段忽略，非零字段值不一致或非 one-hot 时拒绝。按 spec 判定全零回退。
- [ ] `ipi.c` 将 target 原子 claim 到全局位图再发布本核槽。启动期间每核只提交一次，失败不写槽。生产失败沿现有 weak hook 补可定位诊断（逻辑核、原始 target 或失败原因），停驻期间保持本地 IRQ 关闭。BSP 在 `gic_init()` 将 core 初始化后建立 CPU0 target。AP 将探测移到 `arch_smp_ap_cpu_init()` 的本地初始化内，删除 ACK 后调用。
- [ ] PL011 console、SPI test、unexpected probe 和 IPI reply 四处以 `gic_target_bit_get(0)` 取 BSP route；QEMU 对 AP1 raw IAR 的历史断言保留在 runner。真正的物理板取证走任务 5 的定向方法。
- [ ] 运行 `make test-host`、`make PROFILE=aarch64-clang test-aarch64 MODE=smp`、`make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` 以及 `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick`。另用干净的绝对构建目录依次运行 `make PROFILE=aarch64-clang AARCH64_BUILD_ROOT=<绝对隔离目录> AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi` 和 `make PROFILE=aarch64-clang AARCH64_BUILD_ROOT=<同一目录> AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64 MODE=no-ack`。核对 1/2/4 核 SGI 与 BSP tick、sched-tick 的 AP CNTP 和 no-ACK 隔离记录。提交本任务的路由与测试改动。

## 任务 3　SError 入口、首错诊断与启用

**文件**

- 修改 `kernel/arch/aarch64/intr/{entry.S,trap.c}`、`kernel/arch/aarch64/boot/main.c`、`kernel/arch/aarch64/smp/smp.c`、`kernel/include/arch/aarch64/user_image.h`、`kernel/arch/aarch64/cpu/user_task.c`。
- 测试 `qemutests/aarch64_smp_test.py` 与 `kernel/selftest/fixtures/aarch64_user_signal.S`。

**接口**

- `_Noreturn void aarch64_serror_fatal(struct pt_regs *regs, uint64_t esr)`，汇编 `el1t_serror_entry`、`el1_serror_entry` 与有 ELF 大小的 `aarch64_serror_park`。
- 现有 `AARCH64_USER_TASK_PSTATE` 设为 `UINT64_C(0x240)`，由首次用户映像、exec 和 sigreturn 共用。

- [ ] 扩展 ELF 布局测试的 A64 B 解码，断言三个偏移分别去 `el1t_serror_entry`、`el1_serror_entry`、`el1_serror_entry`。沿用现有保存宏校验。在反汇编中校验 EL1t 路径首个栈访问前 `msr spsel,#1`，且致命入口/处理器/停驻链无 `eret` 或跳到上下文恢复入口。保留 bootstrap vectors 的既有约束。在信号夹具的 handler 检查实际 sigframe 的 GPR 区偏移 264 处 PSTATE，屏蔽 NZCV 后须为 0x240。仿照既有 forged EL1h 场景，加 0x340 假帧用例并断言 SYS_sigreturn 返回 -EINVAL。
- [ ] 在隔离目录运行 `python3 qemutests/aarch64_smp_test.py --layout-only --build-root <临时目录>`，预期旧槽跳向自己导致 RED。
- [ ] 三个槽只写分支，新增保存链在向量表之外。EL1t 选择 SP_EL1 后进入共享入口。入口先全屏蔽，用 `save_pt_regs` 后读取 ESR，x0 传栈帧、x1 传 ESR。C 若意外返回直接进入 `aarch64_serror_park`。停驻符号内保持全屏蔽并循环 WFI。
- [ ] 在 `trap.c` 复用 `first_fault` CAS、原有十六进制输出 helper 与早期 UART。按 spec 固定一行，来源由 SPSR M[4:0] 解码，ESR/ELR/SPSR/MPIDR 固定位宽。先 claim 再打印，胜者和其他故障核都进入专用 park。保持 sync 行原样。无 GIC 访问、栈回溯或 logger。
- [ ] BSP 在 `arch_boot_controllers()` 第二次 `pl011_init()` 之后执行 `msr daifclr,#4` 和 ISB。AP 在 `arch_smp_ap_cpu_init()` 开始执行同样序列。正式 EL0 设 0x240，保留旧探针入出采用的临时屏蔽和调用方 DAIF 恢复。
- [ ] 重跑 ELF 审计，运行 `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` 保持旧行匹配，运行 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`，以串口 `[TEST] PASS aarch64_user_signal` 及 suite END 无失败为准，随后退出 QEMU。提交本任务的入口和状态改动。

## 任务 4　独立 QEMU 合成入口验收

**文件**

- 修改 `kernel/arch/aarch64/{intr/entry.S,boot/main.c,smp/smp.c}`、`kernel/Makefile`、`mk/profiles/{aarch64-clang,x86_64-clang}.mk`、`mk/project.mk`、`mk/components/run.mk`。
- 新建 `qemutests/aarch64_serror.py`、`qemutests/test_aarch64_serror.py`，后者登记 harness 元数据。生成物按 `make regen-test-catalog` 更新。

**接口**

- `make PROFILE=aarch64-clang test-aarch64 MODE=serror` 运行三个 2 核独立启动，编译选择器 `AARCH64_SERROR_TEST=el1t|el1h|el0t` 对应 `serror-<origin>` 的盘和 ELF。
- 仅测试构建导出 `_Noreturn void aarch64_serror_test_enter(uint64_t slot, uint64_t spsr, uint64_t test_sp_el0)`。实际输入槽 `{0x180,0x380,0x580}`，SPSR `{0x240+4,0x240+5,0x240}`，ESR `0x00000000be000055`，ELR 从当前变体的 `aarch64_serror_test_returned` 符号取得。
- Python 判据 `serror_evidence(log: str, origin: str, elr: int, park_pc: int, park_start: int, park_size: int) -> bool`。从 ELF 读 park 边界和返回哨兵地址，QMP 从暂停后的 CPU0 读取实际 PC。

- [ ] 写纯判据用例。用正确 DAIF、armed、精确 fatal 和 park PC 正例；去掉一核 DAIF、把 A 置位或 I 清零、替换 origin/MPIDR/ESR/ELR/SPSR/FAR、使 fatal 先于 armed、重复 fatal、添加续跑或 sync fatal、把 PC 改到 park 区外，逐一断言 False。模拟 QEMU 提前退出、PC 不可解析和超时，runner 非零退出。
- [ ] 接编译选择器和 Make 模式，使用现有 `_aarch64_prep`、变体路径 helpers 和运行参数约定。三个变体与普通镜像互斥，外层拒绝矛盾的选择器，分别构建并只消费对应盘和 ELF。将 AARCH64_SERROR_TEST 加入 `mk/project.mk` 的 OS01_SUBMAKE_ALLOWED，kernel Makefile 将三个枚举值映射为汇编和 C 都能读取的编译常量。x86_64 profile 拒绝非空值。测试使用 aarch64-lma-layout 同类的直接 MODE 入口。runner 元数据标记 `layer: qemu_e2e`、`timing: strict`、`tier: nightly`。
- [ ] 测试专用观察代码在 BSP/AP 的 `daifclr #4; isb` 后立即读 DAIF，各核将原始值发布到自己的槽。BSP 在 `arch_post_smp_selftest()` 开头核对 AP 已通过就绪握手，以 acquire 读取样本并统一打印 `[aarch64-serror-test] daif cpu=<0|1> a=0 i=1`，缺样本则报失败。随后打印 `[aarch64-serror-test] armed origin=<origin> mpidr=0x<16 hex>`，用独立 MRS 取 MPIDR 供 fatal 比对。注入汇编先执行 `msr daifset,#0xf`，再在 EL1 设置 ESR、SPSR 与 ELR，防止真实 IRQ 覆盖预置寄存器。EL1t 使用 SPSel=0 和故意不可写的 SP_EL0（`0`），EL1h/EL0 模拟入口以 SP_EL1 执行。陷入测试不声称生成物理 SError。返回哨兵能输出 `[aarch64-serror-test] returned` 的路径按异常返回模式处理，EL0 的错误 ERET 即使触发同步异常也会被 park PC 判据拒绝。
- [ ] runner 复用 `ProcessSession`、`QmpClient`、`RunArchive` 与诊断 DTB。启动前用 `mcopy -i <image> ::/kernel.elf <临时 ELF>` 提取 FAT 中的内核，与传入 ELF 比对 SHA-256。按一份单调期限等待完整 fatal 行，然后用 QMP `stop` 暂停并等待 STOP。通过 `human-monitor-command` 的 `{"command-line":"info registers","cpu-index":0}` 读取 CPU0 PC。若暂未到 park 且 QEMU 仍由 runner 控制，用 `cont` 短暂继续后重新采样，直至匹配或总期限到期。每轮都拒绝新出现的错误证据。要求 `park_start <= PC < park_start + park_size`，串口无拒绝记录，QEMU 没有自行退出。保存各来源串口、寄存器输出、QMP 记录和对应 ELF 标识。超时、歧义或工具错误均失败。
- [ ] 运行 `python3 -m unittest qemutests.test_aarch64_serror`、`make regen-test-catalog`、`make check-test-catalog`、`make PROFILE=aarch64-clang test-aarch64 MODE=serror`。要求三个不同来源均达到实际 park 范围。通过 `llvm-nm` 检查普通镜像无 `aarch64_serror_test_*`，负例交换两个变体的 ELF/镜像后拒绝。提交本任务的探针和 runner。

## 任务 5　真机取证与合并前回归

**文件**

- 修改 `docs/arch.md`、`docs/build/build.md`、`docs/roadmap.md`，记录配置、步骤与实际状态。

- [ ] 以 `make PROFILE=aarch64-clang aarch64-uefi` 构建生产镜像。真机步骤写明固件为 OS-owned SPI 和各核 banked SGI/PPI 配 Group 1、初始化 NS 可访问的 PMR、允许中断到 EL1。记录首次早期 PL011 C 初始化前提与其他仍存在的 DTB/PSCI 限制。
- [ ] 给出板级证据模板。记录板型、固件、DTB、镜像与 ELF 标识、CPU 数、每核 target、CNTP、定向 SGI、PL011 到 TTY。生产镜像用调试器观察各核 `cntp_tick_handler` 反复命中，利用现有 `ipi_broadcast(IPI_VECTOR_RESCHED, 1ULL << dst)` 在已接纳核之间双向定向发送，记录目的核 handler 与完整 IAR/EOIR。可以在具备受控执行机制的调试环境触发，或在独立板级诊断镜像安排每核发一次。输入两次独立串口数据，确认设备清源、EOI 及两次 TTY 消费，记录使用的方法。固定 SPI40 注入及 filter=OTHERS 的 QEMU 自测避开真机。
- [ ] 为有厂商文档的板补采真实 SError，记录异常路由、来源、fatal 和停驻证据。使用匹配 ELF 的 `llvm-addr2line -f -e <kernel.elf> <ELR>` 查内核接收点。对无法执行的板级项目逐项填写待验证与具体依赖。
- [ ] 跑 `make test-host`、`make test-static`、`make PROFILE=aarch64-clang test-aarch64-audit`、`make test-harness`、`make PROFILE=aarch64-clang test-aarch64 MODE=serror`，汇总任务 1–4 的 QEMU 证据。另跑 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`，核对 1/2/4/8 核运行与真实串口交互。文档按实际软件结果更新，并单列真机 IRQ 与物理 SError 状态。提交文档及可复查的证据索引。
