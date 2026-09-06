# AArch64 UEFI PSCI SMP 设计

日期：2026-09-05。状态：v2 评审修订稿，尚未实现或进行 QEMU 验收。

## 目标与边界

在现有 `aarch64-clang` profile 的 UEFI 启动链上，通过真实 DTB + PSCI 启动 AP，完成 QEMU virt / Cortex-A53 / GICv2 的 1、2、4 核验收。每个在线 CPU 有独立栈、TPIDR_EL1、异常向量和 GIC CPU interface；共享计数测试证明 AP 实际执行，BSP 定时器继续运行。

本阶段仅 BSP 开启 CNTP tick；AP 初始化 CPU interface 后保持 IRQ 屏蔽，执行测试后进入 idle。每核 tick、IPI、调度器、用户态、热插拔、GICv3、RPi 真机和 ACPI CPU 枚举另立任务。不能把本任务完成描述为完整 ARM OS 移植。

## 现状与必须修正的问题

- `main.c` 的 UEFI 路径没有调用 `smp_boot_aps()`；`mk/components/run.mk` 强制 `-smp 1`，profile 已有 `AARCH64_SMP ?= 4`。
- `smp.c` 使用旧 spin-table release，并由 BSP 写 AP `online=1`，随后轮询同一字段，无法证明 AP 上线。
- `psci.c` 固定 SMC 指令，没有使用 DTB method；普通 C 变量未约束到 x0；版本 major/minor 解析颠倒；旧注释关于 SMC immediate 和 context 参数的结论不能继续作为依据。
- `dtb.c` 缺失 DTB 时扫描内存并伪造四核；双 cell MPIDR 拼接高低位颠倒，超容量 CPU 被静默丢弃，节点和 property 边界检查不足。
- `head.S` 的 AP 入口使用共享 scratch stack，32 位 CPU 数量却用 64 位 load；等待 BSP 写 online 的协议需要删除；GICC 地址硬编码。
- `boot.c` 已把 DTB 复制到 handoff 区域；继续使用这个副本，不扩展 boot_context ABI。DTB 指针和 totalsize 必须验证落在 `[0x401e0000, 0x401ff000)` 内，最后一页属于 handoff trampoline。
- 通用日志组件尚未链接进 ARM 骨架。新增诊断通过一个小型 bring-up 日志适配层提供 `log_err/log_warn/log_info`，底层复用现有 PL011 输出，避免引入 x86 printk 依赖。中断只保留已有 BSP tick 输出；SMP 成功摘要由 BSP 串行打印。

## 方案选择

| 方案 | 取舍 |
|---|---|
| **UEFI 使用 PSCI，保留旧入口代码边界（采用）** | 接上已完成的 UEFI 链；共享 AP 初始化，但不把不可信拓扑作为启动依据 |
| 继续扩展 spin-table | 无法直接解决当前 UEFI 固件管理 AP 的启动方式 |
| 同时接入通用 scheduler/percpu | 跨内存、任务和中断接口，验收范围过大 |

当前受支持入口是 UEFI。保留旧 direct/spin-table 源码不等于承诺旧文档中的裸 ELF 测试仍可运行；不得为本任务重新引入 direct-boot 构建目标。UEFI 路径禁止自动 fallback 到 spin-table。

## 数据与接口

所有新增头文件统一放在 `kernel/include/kernel/arch/aarch64/`，C、汇编及 host runner 使用 `#include <kernel/arch/aarch64/文件名.h>`；编译 include 根为 `kernel/include`。现有 `aarch64_percpu.h` 和 `reg.h` 保持原路径，本任务修改其内容时不额外迁移。`boot_offsets.h` 只含汇编可消费的整数宏，C 的类型/offsetof 断言留在 C 头或 translation unit 中。

新增架构私有 `kernel/include/kernel/arch/aarch64/dtb.h`、`kernel/include/kernel/arch/aarch64/psci.h`、`kernel/include/kernel/arch/aarch64/smp.h`，统一跨文件声明。`dtb_parse.c` 独立承载无硬件指令的解析，`dtb.c` 只做启动 wrapper、状态提交和设备 accessor。`kernel/include/kernel/arch/aarch64/dtb.h` 仅包含标准整数/布尔类型，定义 `AARCH64_BOOT_MAX_CPUS=8`，ARM wrapper 用静态断言确认等于 NR_CPUS；host 测试不得通过伪造 `__aarch64__` 宏编译 ARM 指令。解析输入 blob、可访问长度和 BSP affinity，输出拓扑及设备信息。

```c
#define AARCH64_MPIDR_AFFINITY_MASK UINT64_C(0x000000ff00ffffff)
enum psci_conduit { PSCI_CONDUIT_NONE, PSCI_CONDUIT_SMC, PSCI_CONDUIT_HVC };
struct aarch64_topology {
    uint64_t mpidr[AARCH64_BOOT_MAX_CPUS];
    uint32_t cpu_count;
    enum psci_conduit conduit;
    bool psci_compatible;
};
struct aarch64_platform_info {
    struct aarch64_topology topology;
    uint64_t gicd_base, gicc_base, pl011_base;
    uint32_t cntp_ppi;
};
int aarch64_dtb_parse(const void *blob, uint32_t size, uint64_t bsp,
                      struct aarch64_platform_info *out);
int psci_init(enum psci_conduit conduit);
int32_t psci_cpu_on(uint64_t mpidr, uint64_t entry_pa, uint64_t context_id);
uint32_t smp_boot_aps(void); /* 返回实际已确认在线数量，含 BSP */
void secondary_idle(uint32_t cpu_id);
void gic_cpu_init(void);    /* 只配置本核接口，不重置 distributor */
bool test_spinlock_smp(uint32_t active);
void smp_bench_iter(uint32_t cpu_id, uint32_t iterations);
```

`aarch64_dtb_parse`：0 成功；-1 blob/边界错误；-2 CPU topology 错误；-3 PSCI binding 错误。失败时输出清零，禁止消费半解析结果。空指针返回 -1，out 非空才清零；结构深度上限 32，超出返回 -1。纯解析不能改全局设备信息，wrapper 仅在完整成功后提交所有结果。现有设备 DTB accessor 保留，但使用同一个有边界的 walker；GIC/PL011 地址按父节点 cell 数解码，不推导 GICC 固定步长。本阶段设备限定 GICD=0x08000000、GICC=0x08010000、PL011=0x09000000、CNTP PPI=30；从 DTB 验证这些值和各 reg 的最小访问长度，不接受仅仅“落在映射区”的其他地址，避免 `reg.h` IRQ accessor 和早期 PL011 与初始化使用不同基址。设备信息缺失/不匹配返回 -4；不提供默认值掩盖解析错误，不扩充 MMIO 映射。

CPU 仅接受 `device_type="cpu"` 且 status 缺省或 `okay`/`ok` 的节点；按照 `/cpus/#address-cells` 的 1/2 cell 解码，禁止根据长度猜测。过滤 disabled CPU；有效 CPU 超过 NR_CPUS、重复 affinity、没有 BSP 均报错。输出统一把 BSP 放索引 0，后续 C 和汇编均消费同一顺序。多核要求 AP enable-method 为 `psci`，PSCI compatible 包含 `arm,psci-0.2` 或 `arm,psci-1.0`，method 精确为 `smc`/`hvc`；单核可无 PSCI 节点，有节点但畸形仍拒绝。

## PSCI 调用约定

用两个极短的外部汇编叶函数实现 SMC/HVC transport，参数自然位于 x0–x3；执行 `smc #0` 或 `hvc #0` 后返回 x0，遵守 AAPCS64 的 callee-saved 约束。不保留旧 `.inst ... #1` workaround。CPU_ON 使用 64 位 function ID `0xc4000003`，不能截断 MPIDR 或入口；VERSION 用 `0x84000000`，major=bits[31:16]，minor=bits[15:0]，要求至少 0.2。

启动调用的 context_id 在调用方 x3，AP 初始入口从 x0 收到。AP 验证 context_id 在容量内，并核对槽位 MPIDR 与本核 affinity；不信任 Aff0 作为数组索引。不尝试另一种 conduit 来“探测”，避免异常调用。

## AP 启动及内存可见性

BSP 的 UEFI `_start` 不再用 Aff0 将入口 CPU 分流到旧 spin-table 等待环；由 UEFI 移交的唯一 CPU 作为 BSP，PSCI AP 只进入 `secondary_start`。旧等待代码可保留为不可达历史代码，不能阻塞非零 affinity BSP。

BSP 顺序：验证 handoff → 初始化 PL011 → 安装异常向量 → 解析 DTB → 初始化 GIC distributor 和 BSP interface → 初始化全部槽位和共享测试状态 → 发布 page tables/槽位 → PSCI init（多核时必须成功）→ 启动并逐核等待 ACK → 共享测试 → 开启 BSP CNTP/IRQ → idle。

AP 顺序：屏蔽 DAIF → 保存 context → 仅支持 EL1/EL2（EL2 规范化到 EL1h；EL1 显式 SPSel=1）→ 无栈检查 context 和 MPIDR → 设置独立 16 字节对齐栈 → 设置 TPIDR → 安装早期向量 → 加载 BSP 页表并开启 MMU → 安装高半区向量 → C 中初始化本核 GIC interface → release-store online → 等待测试命令 → 测试或 idle。AP 绝不能清 BSS、重建共享页表或重置 distributor。EL2 路径设置 HCR_EL2.RW、CNTHCTL_EL2 的 EL1 计数器/物理 timer 访问权限、CNTVOFF_EL2=0，并设置 SPSR/ELR 后 eret；BSP 的 EL2 路径使用同一计数器配置。PSCI 返回 EL1 时依赖固件满足 PSCI 的初始状态契约，不访问 EL2 寄存器。AP 不启用 CNTP，进入 idle 前关闭自身 CNTP_CTL_EL0.ENABLE。

保留 boot_percpu 已有 48 字节布局，给每个汇编读取字段加 `_Static_assert(offsetof(...))` 和 sizeof 断言，集中汇编 offset 常量。`online` 只有 AP 在初始化完成后写 1（BSP 槽除外）；`go` 用作 BSP 发往 AP 的命令：0=等待，1=执行测试，2=进入 idle。命令与 ACK 都用 release/acquire。删除旧 `benchmark_go` 全局变量及其 accessor，唯一命令源是每槽 `go`；`benchmark_done[]` 仍是测试完成确认。`secondary_idle` 唯一定义在 `smp.c`，`test_spinlock.c` 只提供计数与结果核验。MMU 启用后的 per-CPU 槽位统一用低地址 identity VA（与 BSP TPIDR 保持一致），不再通过高半区别名写同一槽位。

CPU_ON 前按 CTR_EL0 cache line 大小 clean 实际页表范围、低地址拓扑和槽位到 PoC，再 `dsb sy`；对 metadata/page table 的 clean 只在发出第一个 CPU_ON 之前执行一次；任何 AP 启动后不得再整区 clean/清零槽位或栈，避免旧 cache line 覆盖 AP 的 ACK。随后只使用一致性映射上的 release/acquire。入口指令的可见性继续依赖已完成的 loader handoff cache 维护。不能仅 clean release 字段，也不能用 volatile 代替内存序。MMU 开启后的共享访问统一使用一致的 Normal、Inner Shareable 映射。

启动期限从 CNTFRQ_EL0 计算，每 AP 2 秒，使用计数差比较，不依赖 IRQ；频率 0 输出 FATAL 并停驻，不承诺 tick 恢复；PSCI init 不可用且平台 DTB 有效时，跳过全部 CPU_ON 并按单核降级继续 tick。SUCCESS、ALREADY_ON、ON_PENDING 都必须继续等待本内核 AP ACK，不能直接计入在线数。其他返回码立刻记录失败并继续尝试下一个 CPU。

超时后不回收槽位/栈，不重试 CPU_ON。本轮 online 集合冻结；迟到 AP 看到 idle 命令后驻留，不加入测试。出现任何启动失败时整轮 benchmark 跳过，向所有 AP 发布 idle；BSP 仍开启 tick，并输出 `[smp] requested=N online=M status=DEGRADED` 及 `[spinlock] status=SKIP`。正常模式测试程序必须把降级判为失败；专门的预期降级模式必须核对失败 CPU/原因、benchmark SKIP 和后续 tick，才能把“故障恢复测试”判成功。缺失/畸形 DTB 在调用 GIC/PSCI 前输出 FATAL 停驻，不承诺此类无有效平台信息的错误继续 tick。

## 可在 host 测试的启动状态机

`kernel/arch/aarch64/smp_boot_core.c` 是内核实际调用的生产状态机，不含系统寄存器或 ARM 内联汇编；`kernel/include/kernel/arch/aarch64/smp_boot_core.h` 只依赖 `kernel/include/kernel/arch/aarch64/dtb.h` 和标准类型。`smp.c` 完成 cache 发布、PSCI init、日志和硬件 adapter 后调用它。测试用 C stub 注入同一接口，不在 Python 复制状态机。

```c
enum smp_failure { SMP_FAILURE_NONE, SMP_FAILURE_CPU_ON, SMP_FAILURE_TIMEOUT };
struct smp_boot_ops {
    void *ctx;
    uint64_t (*counter)(void *ctx);
    int32_t (*cpu_on)(void *ctx, uint64_t mpidr, uint64_t entry, uint64_t id);
    uint32_t (*online_acquire)(void *ctx, uint32_t id);
    void (*command_release)(void *ctx, uint32_t id, uint32_t command);
    void (*relax)(void *ctx);
};
struct smp_boot_result {
    uint32_t requested, online, online_mask;
    int32_t psci_rc[AARCH64_BOOT_MAX_CPUS];
    enum smp_failure failure[AARCH64_BOOT_MAX_CPUS];
};
int smp_boot_run(const struct aarch64_topology *topology,
                 uint64_t entry_pa, uint64_t counter_hz,
                 const struct smp_boot_ops *ops,
                 struct smp_boot_result *result);
```

`0` 表示全核 ACK，`1` 表示已降级，`-1` 表示无效输入（包含零频率或 `counter_hz > UINT64_MAX/2`）；有效输入要求所有 AP 初始 online=0、go=0 且数据已发布。函数初始化 `online=1, online_mask=1`，顺序对每个 AP 调用一次 CPU_ON，记录原始返回码及超时原因；累计的 mask 只在该 AP 期限内读到 ACK 时更新。若出现失败，结束前向所有 AP release 发布 go=2；成功时保持 go=0，由 benchmark 函数唯一发布 go=1。BSP 槽不调用 CPU_ON。计时起点在每个 CPU_ON 前取，等待使用 `(uint64_t)(now-start) >= counter_hz*2`；测试假 counter 覆盖回绕及延迟确认。超时边界先检查期限再接受 ACK，不把边界后 ACK 计入 mask。

成功返回时尚未运行 benchmark。`smp_boot_aps()` wrapper 先检查 core 返回值，-1 必须 FATAL 停驻，不能消费未初始化 result；其余将 result 保存用于诊断，返回 `result.online`；main 仅当等于 `dtb_cpu_count()` 时调用 `test_spinlock_smp(active)`。PSCI init 不可用路径由 wrapper 发布 idle 命令并返回 1，输出对应降级原因。host 测试不宣称覆盖 cache 指令、MMU 或 PSCI transport，后者分别由汇编检查和 QEMU 验收覆盖。

## 可观测性与验收

正常串口标记（BSP 汇总，避免多核输出交错）：

```text
[smp] topology source=uefi-dtb cpus=4
[smp] cpu=1 online mpidr=0x1
[smp] cpu=2 online mpidr=0x2
[smp] cpu=3 online mpidr=0x3
[smp] requested=4 online=4 status=PASS
[spinlock] cpu=0 done=1000000
[spinlock] cpu=1 done=1000000
[spinlock] cpu=2 done=1000000
[spinlock] cpu=3 done=1000000
[spinlock] active=4 iterations=1000000 total=4000000 status=PASS
[tick] 1
[tick] 2
[tick] 3
```

1/2/4 核每个配置重复 3 次；每核 done ACK 齐全且总计数严格等于 N×1000000；至少连续 3 条递增 BSP tick。每次 QEMU 外部限时 90 秒，识别到完整成功证据后主动终止并回收进程。外部 timeout 本身不算 PASS。记录 QEMU 版本、固件 SHA256、EL、conduit、完整日志。

正例覆盖含 Aff1/Aff3 的 MPIDR、BSP 非首项、disabled CPU；负例覆盖缺失/畸形 DTB、错误 method、重复 MPIDR、超容量 topology、CPU_ON 错误、AP 不 ACK、迟到 ACK。host 测试验证高位 MPIDR 不截断，不宣称 QEMU 已实际启动这种拓扑。解析负例不调用 PSCI，降级例必须继续出现 BSP tick。锁死类故障由外部 watchdog 判失败，不能声称所有锁故障都能由内核 timeout 恢复。

## 构建与失败注入契约

`AARCH64_SMP_TEST_NO_ACK_CPU=0` 默认无注入；只允许 1..7 指定 AP，超出拓扑则拒绝故障测试。该值经 `mk/project.mk` 白名单、`mk/components/image.mk` 的显式 `OS01_SUBMAKE_ARGS` 和 `arch/aarch64/make.config` 的 `-D` 传到内核。切换该值必须 `make PROFILE=aarch64-clang clean` 后重建，本阶段不引入自定义 BUILD_DIR 路由。日志存仓库 `test-results/aarch64-uefi-smp/<run-id>/`，避免 profile clean 删除。运行日志必须输出 `[smp-test] no_ack_cpu=N` 的实际编入值，防止注入没有生效却被误判。无 ACK 注入的错误行固定为 `[smp] cpu=1 reason=online-timeout`；预期故障模式只接受这一原因，任何 FATAL/其他失败均不算故障恢复通过。正常模式要求注入值为 0。

ELF 检查要求所有 PT_LOAD 含 BSS 的物理末端不超过 `0x401e0000`，`.boot` 和每核栈位于已映射 2MiB RAM 窗口，异常向量 2KiB 对齐；链接新增断言防止代码增长覆盖 handoff。保留当前 boot_context ABI。

## 参考依据

- [Arm PSCI DEN0022E](https://documentation-service.arm.com/static/640f584656ea36189d4e94a4)：§5.3、§5.6、§6.4，调用、版本和 AP 初始状态。
- [PSCI DT binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/arm/psci.yaml)：compatible 与 method。
- [CPU DT binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/arm/cpus.yaml)：CPU reg 和 enable-method。
- 仓库前置设计：`2026-08-30-aarch64-uefi-bootloader-design.md`。本文覆盖其“仅单核、跳过 SMP”的阶段约束，保留 boot_context v2 和 profile 能力边界。
