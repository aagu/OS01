# SMP 启动统一设计规格（Unified SMP Boot）

- **日期**：2026-11-19
- **状态**：待审阅
- **对应路线图**：`docs/roadmap.md` P2 §架构统一推进链「SMP 启动统一」
- **适用架构**：`x86_64`、`aarch64`

---

## 1. 背景与目标

### 1.1 现状问题
在统一 `kernel_main()` 入口（Spec C）与 BOOT-1~5 落地后，双架构的 SMP 启动仍存在三处结构性割裂：

1. **符号与签名冲突**：
   - `kernel/include/core/smp.h`（x86_64 侧）声明 `void smp_boot_aps(void);`
   - `kernel/include/arch/aarch64/smp.h`（aarch64 侧）声明 `uint32_t smp_boot_aps(void);`
   - `kernel/core/main.c` 阶段 8 仅调用粗粒度钩子 `arch_boot_aps()`，两架构在各自 `arch_boot_aps()` 中重复编排 `smp_boot_aps()` 与 `subsys_prepare_cpus()`。
2. **AP 入口未统一**：
   - x86_64 的 `trampoline.S` 调用无参 `ap_entry(void)`（从 GS 读取 `this_cpu()`）。
   - aarch64 的 `secondary_entry.S` 硬编码跳转 `=secondary_idle`（传入 `x0 = cpu_id`），未消费由 BSP 传入的入口函数指针。
   - 两架构各自维护 `online` → 中断开启 → `ipi_ready` 的发布顺序，缺乏架构中立的公共 AP 生命周期状态机。
3. **失败策略分裂**：
   - x86_64 采用 **Fail-Closed**：任一 AP 激活/握手超时或违背不变量，直接 `kpanic` 停机。
   - aarch64 采用 **Degraded**：AP 失败或超时后冻结 `online_mask`、下发 `AARCH64_BOOT_GO_IDLE`，继续以剩余核启动。

### 1.2 设计目标
1. 引入架构中立门面 `kernel/include/arch/smp.h`，提供路线图要求的单入口 `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)`，消除同名 `smp_boot_aps()` 签名冲突。
2. 建立公共编排与 AP 状态机 `kernel/core/smp.c`：
   - 收敛 `arch_boot_aps()` 为单一公共实现；
   - 提供统一 AP C 入口 `smp_ap_common_entry(uint32_t cpu_id, percpu_t *cpu)`，在公共层强制执行 `cpu_init → online=1 → post_online → irq_enable → ipi_ready=1 → enter_idle` 状态机顺序。
3. **双架构统一 Fail-Closed**：任一 AP 在前置校验、准备、激活、上线握手或调度就绪等待失败时，由公共层统一输出诊断并经 `boot_failure_stop("smp", ...)` 永久停机；废弃 aarch64 的 degraded 启动路径。
4. 保持既有阶段 9 调度交接边界不变（方案 A）：不重排 aarch64 的 `arch_boot_runtime()`、`aarch64_shootdown_probe`、`ap_work` 与 `ap_sched` 阶段 9 交接，使范围聚焦于「SMP 启动统一」。

### 1.3 非目标（Out of Scope）
- 不将 aarch64 的 AP idle task 创建与 `scheduler_ok` 发布前移至阶段 8（留待「SMP 用户调度与抢占」演进）。
- 不新增 aarch64 `spin-table` 激活方式（仍走 PSCI `CPU_ON64`）。
- 不修改 `percpu_t` 结构体布局（保持 `PERCPU_DATA_SIZE == 144`）。
- 不修改 x86_64 `trampoline_data_t` 布局（保持 40 字节）与 aarch64 `aarch64_boot_percpu_t` 大小（保持 48 字节）。

---

## 2. 架构分层与统一接口

### 2.1 架构中立门面：`kernel/include/arch/smp.h`

```c
#ifndef ARCH_SMP_H
#define ARCH_SMP_H

#include <stdint.h>
#include <percpu/percpu.h>

/* 统一 AP C 入口签名（双架构汇编入口均按各自 ABI 传入 cpu_id 与 percpu_t*） */
typedef void (*arch_smp_ap_entry_t)(uint32_t cpu_id, percpu_t *cpu)
    __attribute__((noreturn));

/* 统一 SMP 启动失败阶段分类 */
enum smp_boot_stage {
    SMP_STAGE_INVARIANT = 0, /* 前置或后置状态不变量违背 */
    SMP_STAGE_PREPARE,       /* AP 资源准备失败（如 idle 栈/metadata/发布失败） */
    SMP_STAGE_ACTIVATE,      /* 底层唤醒失败（如 PSCI 不可用或 CPU_ON 拒绝） */
    SMP_STAGE_ONLINE,        /* AP 未在有界预算内发布 online / boot ACK */
    SMP_STAGE_SCHEDULER,     /* AP 未在有界预算内发布 scheduler_ok（x86_64） */
};

/* 最近一次 arch_smp_boot_aps() 的只读诊断快照 */
struct arch_smp_boot_status {
    uint32_t requested;              /* 请求的总 CPU 数（含 BSP） */
    uint32_t online;                 /* 确认上线的 CPU 数（含 BSP） */
    uint64_t online_mask;            /* bit i = 1 表示逻辑 CPU i 已确认上线 */
    uint32_t failed_cpu;             /* 失败的逻辑 CPU ID（成功时为 0） */
    enum smp_boot_stage failed_stage;/* 失败阶段 */
    int32_t  arch_rc;                /* 架构底层错误码（如 PSCI rc 或 -ETIMEDOUT） */
};

/*
 * 单入口 SMP AP 启动（强覆盖，双架构各自实现）：
 * - cpu_count:    逻辑 CPU 总数（含 BSP，必须满足 1 <= cpu_count <= NR_CPUS）
 * - entry:        非空统一 AP C 入口（由公共层传入 smp_ap_common_entry）
 * - per_cpu_data: 指向 percpu_data[0..cpu_count-1]
 *
 * 返回值：
 * - 0：所有 AP（1..cpu_count-1）均成功完成本架构阶段 8 握手；
 * - 负 errno（-EINVAL / -ENOMEM / -ENODEV / -EIO / -ETIMEDOUT）：
 *   任一 AP 失败，且 arch_smp_last_status() 已记录失败核、阶段与底层错误码。
 */
int arch_smp_boot_aps(uint32_t cpu_count,
                      arch_smp_ap_entry_t entry,
                      percpu_t *per_cpu_data);

/* 只读查询最近一次 arch_smp_boot_aps() 的状态快照（强覆盖） */
const struct arch_smp_boot_status *arch_smp_last_status(void);

/*
 * AP 生命周期底层钩子（强覆盖，仅由 kernel/core/smp.c::smp_ap_common_entry 调用）：
 * 1. arch_smp_ap_cpu_init:          本核硬件初始化，必须在 online=1 发布前完成
 * 2. arch_smp_ap_post_online:       online=1 发布后、开中断前的架构握手
 * 3. arch_smp_ap_publish_ipi_ready: 开中断并执行屏障后发布 ipi_ready=1
 * 4. arch_smp_ap_enter_idle:        进入 idle / 调度交接循环（永不返回）
 */
void arch_smp_ap_cpu_init(uint32_t cpu_id, percpu_t *cpu);
void arch_smp_ap_post_online(uint32_t cpu_id, percpu_t *cpu);
void arch_smp_ap_publish_ipi_ready(uint32_t cpu_id, percpu_t *cpu);
void arch_smp_ap_enter_idle(uint32_t cpu_id, percpu_t *cpu)
    __attribute__((noreturn));

/*
 * 可选的 BSP 启动后钩子（kernel/core/smp.c 提供弱默认 no-op）：
 * 仅在所有 AP 启动成功后、subsys_prepare_cpus() 之前执行。
 * aarch64 强覆盖用于执行 test_spinlock_smp(online_cpus)。
 */
void arch_smp_post_boot_hook(uint32_t online_cpus);

#endif /* ARCH_SMP_H */
```

> **头文件清理**：
> - 删除 `kernel/include/core/smp.h`（原仅包含 x86 专用 `void smp_boot_aps(void)`）。
> - `kernel/include/arch/aarch64/smp.h` 移除 `uint32_t smp_boot_aps(void)` 与 `secondary_idle()` 声明，仅保留 aarch64 内部辅助接口（`test_spinlock_smp`、`smp_bench_iter`、`aarch64_smp_accepted_mask`）。

---

## 3. 公共编排与 AP 状态机：`kernel/core/smp.c`

`kernel/core/smp.c` 编译进双架构内核（`KERNEL_C_SOURCES`），并可独立编译进宿主单元测试。

### 3.1 BSP 公共编排：`arch_boot_aps(void)`
`kernel/core/main.c` 阶段 8 继续调用 `arch_boot_aps()`（保持 `arch/boot.h` 与 `test_boot_contract_sequence.c` 契约不变），但其实现收敛为 `kernel/core/smp.c` 中的唯一公共定义：

1. **前置不变量校验（Fail-Closed）**：
   - `num_cpus < 1 || num_cpus > NR_CPUS` → `smp_boot_fail(0, SMP_STAGE_INVARIANT, -EINVAL)`
   - `__atomic_load_n(&percpu_data[0].online, __ATOMIC_ACQUIRE) != 1` → `smp_boot_fail(0, SMP_STAGE_INVARIANT, -EINVAL)`
   - 对所有 `i = 1 .. num_cpus - 1`：若 `percpu_data[i].online != 0 || percpu_data[i].scheduler_ok != 0` → `smp_boot_fail(i, SMP_STAGE_INVARIANT, -EINVAL)`
2. **调用架构单入口**：
   - `int rc = arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data);`
3. **统一结果核验与 Fail-Closed**：
   - 若 `rc != 0`：读取 `const struct arch_smp_boot_status *st = arch_smp_last_status()`，取 `failed_cpu` 与 `failed_stage`（若 `st == NULL` 则回退 `cpu=0, stage=SMP_STAGE_INVARIANT`），调用 `smp_boot_fail(failed_cpu, failed_stage, rc)`。
   - 若 `rc == 0`：核验 `st != NULL && st->requested == num_cpus && st->online == num_cpus`，否则调用 `smp_boot_fail(0, SMP_STAGE_INVARIANT, -EIO)`。
4. **成功标记与后置阶段**：
   - 输出统一完成标记：`[boot-1] complete discovered=<N> ready=<N>`（使用栈上十进制格式化 + `%s`，兼容 AArch64 mini-formatter）。
   - 调用 `arch_smp_post_boot_hook(num_cpus)`。
   - 调用 `subsys_prepare_cpus()`。

### 3.2 统一 Fail-Closed 停机路径：`smp_boot_fail()`
```c
static const char *smp_stage_name(enum smp_boot_stage stage)
{
    switch (stage) {
    case SMP_STAGE_INVARIANT: return "invariant";
    case SMP_STAGE_PREPARE:   return "prepare";
    case SMP_STAGE_ACTIVATE:  return "activate";
    case SMP_STAGE_ONLINE:    return "online";
    case SMP_STAGE_SCHEDULER: return "scheduler";
    default:                  return "unknown";
    }
}
```
`smp_boot_fail(uint32_t cpu, enum smp_boot_stage stage, int rc)`（`noreturn`）：
1. 在栈上格式化 `cpu` 为十进制字符串（无堆分配、不依赖 `%u`，与 `boot_failure.c` 保持同等 AArch64 mini-formatter 安全性）。
2. 输出 `[boot-1] FATAL cpu=%s stage=%s\n`。
3. 调用 `boot_failure_stop("smp", smp_stage_name(stage), rc, -1, (int)cpu)`，后者输出 `[boot-3] FATAL stage=smp component=<stage> rc=<rc> phase=-1 cpu=<cpu>\n`，关中断并永久 halt。

### 3.3 公共 AP C 入口状态机：`smp_ap_common_entry(uint32_t cpu_id, percpu_t *cpu)`
```c
void smp_ap_common_entry(uint32_t cpu_id, percpu_t *cpu)
{
    /* 1. 本核硬件初始化（在 online=1 对 BSP 可见之前完成） */
    arch_smp_ap_cpu_init(cpu_id, cpu);

    /* 2. 公共唯一发布点：release-store online=1 */
    __atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);

    /* 3. 上线后、开中断前的架构握手（x86 TSC 同步 / aarch64 boot ACK & go） */
    arch_smp_ap_post_online(cpu_id, cpu);

    /* 4. 统一开本地中断 + 屏障，再发布 ipi_ready=1（强制 online=1 ∧ irq_enabled ⇒ ipi_ready=1） */
    arch_local_irq_enable();
    arch_mb();
    arch_smp_ap_publish_ipi_ready(cpu_id, cpu);

    /* 5. 进入架构 idle / 调度交接循环（noreturn） */
    arch_smp_ap_enter_idle(cpu_id, cpu);
}
```

**内存序与 BOOT-1 不变量**：
- `arch_smp_ap_cpu_init()` 中的所有本核初始化（含描述符表、中断控制器本地接口、页表校验、`percpu_init`）均发生在 `cpu->online = 1` 的 `__ATOMIC_RELEASE` 之前。
- `arch_local_irq_enable()` + 屏障发生在 `arch_smp_ap_publish_ipi_ready()` 之前，因此在任何核观测到 `ipi_ready == 1` 时，目标核必已满足 `online == 1` 且可接收 IPI/SGI。
- `boot1_fault_before_online`（x86_64 故障注入）保留在 `arch_smp_ap_cpu_init()` 末尾，以便 `delay-online` / `no-online` 精准阻断在 `online = 1` 发布之前；`boot1_fault_after_ipi_ready` 保留在 `arch_smp_ap_publish_ipi_ready()` 末尾，精准阻断在 `scheduler_ok = 1` 发布之前。

---

## 4. 双架构底层适配设计

### 4.1 x86_64 适配

#### 4.1.1 汇编入口：`kernel/arch/x86_64/trampoline.S`
在 `trampoline_long` 跳转到 `tdata->entry` 前装入 SysV AMD64 前两个参数寄存器：
```asm
    movl    trampoline_data + 36(%rip), %edi  // arg1: uint32_t cpu_id
    movq    trampoline_data + 8(%rip),  %rsi  // arg2: percpu_t *cpu (gs_base)
    movq    trampoline_data + 24(%rip), %rax  // tdata->entry
    call    *%rax
```
`trampoline_data_t`（`kernel/include/arch/x86_64/trampoline.h`）保持 40 字节布局不变。

#### 4.1.2 `kernel/arch/x86_64/smp/smp.c`
- 静态保存 `static struct arch_smp_boot_status x86_smp_status;` 并实现 `arch_smp_last_status()`。
- 实现 `int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry, percpu_t *per_cpu_data)`：
  - 校验 `!entry || !per_cpu_data || cpu_count == 0 || cpu_count > NR_CPUS` → 记录 `SMP_STAGE_INVARIANT` 并返回 `-EINVAL`。
  - 初始化 `x86_smp_status`（`requested = cpu_count, online = 1, online_mask = 1ULL`）。
  - 执行 `boot1_fault_variant_identity()`、`ipi_init()`、`ipi_ready_publish_bsp()`、拷贝 trampoline 二进制到 `TRAMPOLINE_BASE`。
  - 逐核（`i = 1 .. cpu_count - 1`）：
    1. 校验 `per_cpu_data[i].online == 0 && per_cpu_data[i].scheduler_ok == 0`（失败记 `SMP_STAGE_INVARIANT`，返回 `-EINVAL`）。
    2. `create_idle_task(i)`（失败记 `SMP_STAGE_PREPARE`，返回 `-ENOMEM`）、`task_list_add(idle)`、`set_tss_descriptor`、`per_cpu_data[i].tss_hw = &init_tss[i]`。
    3. 填充 `tdata`：`tdata->gs_base = (uint64_t)&per_cpu_data[i]`、`tdata->entry = (uint64_t)(uintptr_t)entry`、`tdata->cpu_id = i` 等。
    4. 发送 INIT-SIPI-SIPI。
    5. 有界等待 `per_cpu_data[i].online`（超时记 `SMP_STAGE_ONLINE`，返回 `-ETIMEDOUT`）。
    6. TSC 握手（超时仍仅记录 `tsc_offset = 0` 并告警，不作为启动失败）。
    7. 有界等待 `per_cpu_data[i].scheduler_ok`（超时记 `SMP_STAGE_SCHEDULER`，返回 `-ETIMEDOUT`）。
    8. 复检 `online == 1 && ipi_ready == 1`（失败记 `SMP_STAGE_INVARIANT`，返回 `-EIO`）。
    9. 更新 `x86_smp_status.online++`、`x86_smp_status.online_mask |= (1ULL << i)`，输出 `[boot-1] ready cpu=%u online=1 ipi_ready=1 scheduler_ok=1\n`。
  - 全部成功后返回 `0`（`[boot-1] complete discovered=N ready=N` 移至公共 `kernel/core/smp.c` 统一输出）。
- 实现四个 AP 钩子：
  - `arch_smp_ap_cpu_init(cpu_id, cpu)`：启用 LAPIC、加载内核 GDT/IDT 与段选择子、初始化 `cpu->idle->thread`、填充 `cpu->tss` 与 `set_tss64`、`load_TR`、`lapic_configure_nmi()`、`lapic_timer_start(100)` + `sched_accounting_cpu_start(...)`、末尾调用 `boot1_fault_before_online(cpu_id)`。
  - `arch_smp_ap_post_online(cpu_id, cpu)`：等待 `tsc_sync_go` 并采样 `rdtsc()` 发布 `tsc_sampled = 1`。
  - `arch_smp_ap_publish_ipi_ready(cpu_id, cpu)`：`ipi_ready_publish_ap(cpu_id)` + `boot1_fault_after_ipi_ready(cpu_id)`。
  - `arch_smp_ap_enter_idle(cpu_id, cpu)`：发布 `cpu->scheduler_ok = 1`（release-store），进入 `hlt` / `need_resched` / `schedule()` 循环。

#### 4.1.3 `kernel/arch/x86_64/smp/boot.c` & `kernel/include/arch/x86_64/smp_boot.h`
- 移除 `x86_64_boot_aps()` 与 `arch_boot_aps()`（由 `kernel/core/smp.c` 接管）。

---

### 4.2 aarch64 适配

#### 4.2.1 启动槽 ABI 复用：`kernel/include/arch/aarch_percpu.h` & `boot_offsets.h`
`aarch64_boot_percpu_t` 末尾 offset 40 原本保留为 `uint64_t reserved`，直接复用为 `uint64_t entry`：
- `AARCH64_BOOT_ENTRY_OFFSET = 40`（替换 `AARCH64_BOOT_RESERVED_OFFSET`），结构体总长保持 48 字节不变，所有 `_Static_assert` 同步校验。
- `aarch64_smp_publish_boot(const struct aarch64_topology *topology, uint64_t entry_va)` 将 `entry_va` 写入每个槽的 `slots[i].entry`（拒绝 `entry_va == 0`），并在 `clean_to_poc()` 中一并清理到 PoC。

#### 4.2.2 汇编入口：`kernel/arch/aarch64/boot/secondary_entry.S`
在 MMU 启用与 `vbar_el1 = exception_vectors` 之后（原 lines 118–125）：
```asm
    /* 从已校验的 boot slot 加载统一 AP C 入口，传入 (x0=cpu_id, x1=&percpu_data[cpu_id]) */
    ldr     x30, [x6, #AARCH64_BOOT_ENTRY_OFFSET]
    cbz     x30, secondary_panic
    mov     x0, x19
    mrs     x1, tpidr_el1
    blr     x30
```
- `x6` 在前文已指向 `&aarch64_boot_percpu[cpu_id]` 且校验了 `self`、`cpu_id`、`mpidr`、`stack`。
- `tpidr_el1` 在前文已设为 `&percpu_data[cpu_id]`，作为第二参数 `percpu_t *cpu` 传入。
- 移除对 `secondary_idle` 的硬编码引用。

#### 4.2.3 `kernel/arch/aarch64/smp/smp.c`
- 静态保存 `static struct arch_smp_boot_status a64_smp_status;` 并实现 `arch_smp_last_status()`。
- 实现 `int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry, percpu_t *per_cpu_data)`：
  - 校验 `!entry || !per_cpu_data || cpu_count == 0 || cpu_count > NR_CPUS || boot_published` → 记录 `SMP_STAGE_INVARIANT` 返回 `-EINVAL`。
  - 核对 `topology = dtb_topology()` 且 `topology->cpu_count == cpu_count`；校验 `cntfrq_el0()` 合法。
  - 初始化 `bench_lock` / `benchmark_total` / `benchmark_done[]`，调用 `aarch64_smp_publish_boot(topology, (uint64_t)(uintptr_t)entry)`（失败记 `SMP_STAGE_PREPARE` 返回 `-EINVAL`）。
  - 打印既有 `[smp] boot el=... conduit=...` 与 `[smp-test] no_ack_cpu=...` 日志。
  - 若 `cpu_count > 1 && psci_init(topology->conduit) != 0`：
    - 记录 `a64_smp_status`（`failed_cpu = 1`, `failed_stage = SMP_STAGE_ACTIVATE`, `arch_rc = -ENODEV`），输出 `[smp] reason=psci-unavailable\n`，返回 `-ENODEV`（**不再降级返回 1**）。
  - 调用 `smp_boot_run(topology, secondary_start_addr(), hz, &ops, &result)`：
    - 若 `status < 0`：记录 `SMP_STAGE_INVARIANT`，返回 `-EINVAL`。
    - 遍历 `0 .. result.requested - 1` 输出每核 `[smp] cpu=...` 日志（保持正常路径与失败诊断日志清晰）。
    - 若 `status != 0`（即存在任意 AP `SMP_FAILURE_CPU_ON` 或 `SMP_FAILURE_TIMEOUT`）：
      - 定位首个失败核 `id`：
        - `SMP_FAILURE_CPU_ON` → `failed_stage = SMP_STAGE_ACTIVATE`, `arch_rc = result.psci_rc[id]`, 返回 `-EIO`；
        - `SMP_FAILURE_TIMEOUT` → `failed_stage = SMP_STAGE_ONLINE`, `arch_rc = -ETIMEDOUT`, 返回 `-ETIMEDOUT`；
      - 写入 `a64_smp_status` 并返回负 errno（**Fail-Closed，不再调用 `aarch64_page_table_probe_finish(false)` 或返回降级核数**）。
    - 全部成功时：冻结 `boot_result = result` 与 `a64_smp_status`（`online = result.online`, `online_mask = result.online_mask`），调用 `aarch64_page_table_probe_finish(true)`，输出 `[smp] requested=N online=N status=PASS\n`，返回 `0`。
- 实现四个 AP 钩子：
  - `arch_smp_ap_cpu_init(cpu_id, cpu)`：`cntp_ctl_el0_write(0)`、`gic_cpu_init()`、`percpu_install_gs(cpu_id)`、`percpu_init(cpu_id, mpidr_ap)`、`aarch64_page_table_ap_verify(cpu_id)`（校验失败则 halt）。
  - `arch_smp_ap_post_online(cpu_id, cpu)`：
    - 若 `cpu_id != AARCH64_SMP_TEST_NO_ACK_CPU` 则调用 `boot_online_set(cpu_id)`；
    - 等待 `boot_go_get(cpu_id) != AARCH64_BOOT_GO_WAIT`；若为 `AARCH64_BOOT_GO_TEST` 则运行 `smp_bench_iter(cpu_id, 1000000)`；
    - 调用 `gic_target_bit_init(cpu_id)`（在公共层开中断之前完成 GIC 目标位校验）。
  - `arch_smp_ap_publish_ipi_ready(cpu_id, cpu)`：执行 `isb`，调用 `ipi_ready_publish_and_count(cpu_id)`。
  - `arch_smp_ap_enter_idle(cpu_id, cpu)`：进入 `ap_work_run_one(cpu_id)` + `aarch64_ap_sched_released(cpu_id)` / `aarch64_ap_sched_enter(cpu_id)` + `arch_cpu_pause()` 循环。
- 实现 `arch_smp_post_boot_hook(uint32_t online_cpus)`：执行 `(void)test_spinlock_smp(online_cpus)`。

#### 4.2.4 `kernel/arch/aarch64/boot/main.c`
- 移除 `arch_boot_aps()`（由 `kernel/core/smp.c` 统一提供）。
- 移除 `boot1_no_ack_observation()` 及其在 `arch_boot_runtime()` 中的调用（`NO_ACK` 注入现在于阶段 8 Fail-Closed 停机，不会再抵达阶段 8.2）。

---

## 5. 测试与验证计划

### 5.1 新增宿主单元测试：`hosttests/cases/test_smp_boot_unified.c`
附 `@os01-test` 元数据并执行 `make regen-test-catalog`，直接编译真实 `kernel/core/smp.c`：
1. **前置不变量拒绝**：`num_cpus == 0`、`num_cpus > NR_CPUS`、BSP `online == 0`、AP 残留 `online == 1` 或 `scheduler_ok == 1` → 触发 `[boot-1] FATAL ... stage=invariant` + `boot_failure_stop("smp", "invariant", ...)`，绝不调用 `arch_smp_boot_aps()`。
2. **正常多核编排**：验证 `arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data)` 参数传递正确，且严格按 `arch_smp_boot_aps → arch_smp_post_boot_hook → subsys_prepare_cpus` 顺序执行。
3. **公共 AP 状态机顺序**：驱动 `smp_ap_common_entry(1, &percpu_data[1])`，断言调用序严格为 `cpu_init`（此时 `online==0`, `irq==off`）→ `online=1` → `post_online`（此时 `online==1`, `irq==off`）→ `irq_enable` → `publish_ipi_ready`（此时 `online==1`, `irq==on`）→ `enter_idle`。
4. **Fail-Closed 阻断**：模拟 `arch_smp_boot_aps()` 在 `SMP_STAGE_PREPARE` / `SMP_STAGE_ACTIVATE` / `SMP_STAGE_ONLINE` / `SMP_STAGE_SCHEDULER` 返回错误，断言输出对应 `[boot-1] FATAL cpu=<id> stage=<stage>` 并调用 `boot_failure_stop("smp", <stage>, rc, -1, <id>)`，且绝不调用 `arch_smp_post_boot_hook` 或 `subsys_prepare_cpus`。

### 5.2 既有测试更新
1. **`qemutests/boot_lifecycle.py` & `qemutests/test_boot_lifecycle.py`**：
   - x86_64 的 `no-online` 与 `no-scheduler` 故障用例现在经 `boot_failure_stop("smp", ...)` 停机（不再触发 `kpanic`）；更新断言要求 `[boot-1] FATAL cpu=1 stage=online|scheduler` 与 `[boot-3] FATAL stage=smp component=online|scheduler ...` 且无任何 `kernel panic`，并保留 1 秒静默停机观察窗。
2. **`qemutests/aarch64_uefi_smp.py` & `qemutests/test_aarch64_ap_boot_contract.py`**：
   - 正常 1/2/4 核路径断言保持不变（`[smp] requested=N online=N status=PASS`、`[spinlock] ... PASS`、`[tick]`）。
   - `no_ack_cpu=1` 故障用例从验证 `DEGRADED + [boot-1] a64-no-ack` 更新为验证 **Fail-Closed**：必须出现 `[boot-1] FATAL cpu=1 stage=online` 与 `[boot-3] FATAL stage=smp component=online ...`，且绝不出现 `[spinlock]`、`SHOOTDOWN-PROBE` 或 `[tick]`，观察窗口内保持静默停机。
3. **头文件边界与静态审计**：
   - 更新 `qemutests/test_boot_facade_headers.py`（若引用了旧 `core/smp.h`）与 `make test-static` / `test-aarch64-audit`。

### 5.3 验收命令清单
- `make regen-test-catalog && make check-test-catalog`
- `make test-host`
- `make test-static`
- `make test-qemu SUITE=boot-lifecycle`（x86_64 正常 + 故障矩阵）
- `make PROFILE=aarch64-clang test-aarch64-audit`
- `make PROFILE=aarch64-clang test-aarch64 MODE=smp`（含正常多核 + `no_ack_cpu=1` Fail-Closed）
- `make test-qemu SUITE=phase-0`（x86_64 引导冒烟）

### 5.4 文档同步
实施并验收通过后，同步更新：
- `docs/roadmap.md`：将「SMP 启动统一」标记闭环并记入「近期闭环索引」。
- `docs/arch.md`：在 Facade 矩阵中补充 `arch/smp.h` 一行并更新 §aarch64 用户态现状中的 `arch_smp_boot_aps()` 描述。
