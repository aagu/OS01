# 统一双架构 kernel_main 启动入口设计

日期：2026-11-07。基线：`492f8ff0`。状态：Draft (Rev 5)。  
修订记录：
- **Rev 2**：区分 Pre-SMP 与 Post-SMP 自测/探针时序、规范 `expected_late` 变体语义、锁定契约测试迁移、恢复设备节点错误停机、下沉 Canary 汇编、纠正 aarch64 验证命令、在 aarch64 审计中显式增加 `kernel_main` 门禁。
- **Rev 3**：修正入口审计正则与 `readelf`、隔离 1536 字节栈帧至 `aarch64_early_arena_preflight()`、清理 `head.S` 旧声明、严格保持 aarch64 时钟源后置时序、补齐 Feature Gates 契约、收敛 Canary 至 x86。
- **Rev 4**：配置 `AARCH64_READELF`、恢复 `kernel/Makefile` 源码接入、明确 `arch_early_panic` 内部串口初始化与停机保证、解耦时序契约与函数物理排列、接入 `TEST_HARNESS_MODULES`。
- **Rev 5**：
  1. **[P2] 恢复 `CAPACITY_TEST` 局部前向声明**：在 `kernel_main` 中完整保留 `void arch10_capacity_fixture_init(void);` 前向声明，消除 `-Werror=implicit-function-declaration` 风险，并在验收清单中补充该变体验证。
  2. **[P2] 修复审计管道屏蔽 `readelf` 退出码**：先捕获 `readelf` 完整输出并校验其自身退出状态码（`|| exit 1`），杜绝由于管道后半段 `grep` 导致的工具静默失败；补充“产生合法输出但工具非零退出”负向用例。
  3. **[P2] 严格符号末尾匹配**：将正则限定为 `[[:space:]]+kernel_main[[:space:]]*$`，防止 `kernel_main.extra` 等后缀符号产生假阳性匹配；补充后缀符号拒绝用例。

---

## 1. 目标与边界

### 1.1 核心目标
消除 x86_64 与 aarch64 当前存在的双 `main` 入口分化，将两个架构的启动编排逻辑彻底统一到单一的 `kernel/core/main.c::kernel_main()`，彻底消除 `aarch64_main()`。

1. **统一启动主干**：建立跨架构的有序启动阶段契约（Canary 栈保护 → Handoff 数据校验 → 早期硬件初始化 → 内存初始化 → 早期自测/探针 → 中断控制器 → BSP per-CPU → 随机数 → VFS/TTY（条件开启）→ SMP 启动 → 后 SMP 自测/探针 → 调度器与用户态）。
2. **能力条件包裹（Feature Gates）**：对于 aarch64 暂不支持的能力（VFS 文件系统、块设备/分区挂载、DevFS 节点、TTY/虚拟控制台行规程、Futex、EL0 用户态 init 进程），在 `kernel_main()` 中采用编译期特征宏（`OS01_HAS_VFS`、`OS01_HAS_USERSPACE`）进行整块隔离，保证 x86_64 现有生产能力与行为 100% 保持不变，同时为 aarch64 后续功能接入预留即插即用切入口。
3. **时序与探针绝对保持**：严格保持 aarch64 早期自测（Pre-DTB/GIC/SMP）、同步故障探针（`AARCH64_SYNC_FAULT_TEST`）、多核击落探针（`aarch64_shootdown_probe`）、多核页表测试、时钟源自注册/分派标记以及 GIC 调度探针的原有执行时机与前置条件。

### 1.2 非目标（Non-Goals）
- **不在本期为 aarch64 实现 VFS / 块设备 / Virtio**：aarch64 当前保持在内核态阶段，不提前引入虚拟文件系统。
- **不在本期为 aarch64 引入 EL0 用户态进程**：aarch64 继续沿用当前的内核态测试线程与换栈 idle 模型。
- **不修改底层汇编初始化**：x86_64 的 `head.S` 与 aarch64 的 `head.S` 分别完成各自的 CPU 模式切换与早期恒等映射，仅将跳转目标统一对准 `kernel_main`。
- **不修改既有串口日志协议与测试契约**：所有已有自动化测试所断言的串口记录（如 aarch64 的 `[clocksource]`, `[cntp]`, `[fb]`, `SHOOTDOWN-PROBE: OK`，以及 x86_64 的 `[selftest]` 等）输出时序与文本内容必须 100% 保持一致。

---

## 2. 现状分析与阶段映射

### 2.1 双架构现状对比

| 阶段 | x86_64 当前实现 (`kernel/core/main.c`) | aarch64 当前实现 (`kernel/arch/aarch64/boot/main.c`) | 统一样式与处理策略 |
|---|---|---|---|
| **0. 栈保护 Canary** | `__stack_chk_guard = arch_cycle_counter() ^ 0xDEADBEEFCAFEBABE` | 静态初值（未在 main 显式重置） | **完全统一**：`arch_cycle_counter()` 跨架构通用，统一作为 `kernel_main` 第一行语句 |
| **0. Canary 测试** | `kernel_canary_selftest_trip()`（含 `movq` 汇编） | 不支持（无编译器 SSP 参数） | **范围收敛**：限定为 x86 专有特性，在 `mk/project.mk` 拒绝非 x86 构建，代码加 `#if defined(__x86_64__)` |
| **0.5 Handoff 校验** | `boot_context_valid(bootctx)`，失败直写串口 halt | `boot_context_valid(bootctx)`，失败写 log_err halt | **完全统一**：调用 `arch_early_panic()`，实现自备 UART 前提并保证停机 |
| **1. 架构早期初始化** | `x86_64_boot_early()` (IDT/GDT/TSS/Serial) | `pl011_init()`, `vbar_el1`, `aarch64_ram_init()` | **Facade 抽象**：`arch_early_init(bootctx)` |
| **2. 内存子系统** | `x86_64_boot_memory()` (PMM/DirectMap/Slab/FB) | `pmm_init()`, `arch_boot_direct_map_init()`, `arch_vmm_init()`, `aarch64_boot_fb_init()` | **Facade 抽象**：`arch_boot_memory(bootctx)`；1536B scratch 隔离至独立辅助函数 |
| **2.5 Pre-SMP 早期自测/探针** | 无（x86 自测依赖 VFS，移至 SMP 后） | `selftest_begin_run()` + `selftest_run_all()` + `AARCH64_SYNC_FAULT_TEST` 探针 | **Facade 抽象**：`arch_pre_subsys_selftest(bootctx)`，保持 aarch64 依赖的单核纯净环境 |
| **3. 设备/中断控制器** | `x86_64_boot_subsystems()` (ACPI/APIC/IOAPIC/Subsys) | `dtb_init()`, `gic_init()` | **Facade 抽象**：`arch_boot_subsystems(bootctx)`（注意：aarch64 阶段 4 时钟源保留在 SMP 后） |
| **4. BSP per-CPU** | `x86_64_boot_percpu()` (MADT/GS/online) | `percpu_install_gs()`, `percpu_init()`, `num_cpus` | **Facade 抽象**：`arch_boot_percpu()` |
| **5. 随机数 CSPRNG** | `random_init(bootctx)` (BSP 单次) | 现未在 main 中调用（实现已存在） | **完全统一**：跨架构统一调用 `random_init(bootctx)` |
| **5.5 x86 破坏性中断探针** | `x86_intr_fault_probe_run()` (BP/NMI) | 无 | **条件包裹**：`#if defined(__x86_64__) && (defined(X86_INTR_FAULT_TEST_BP) || ...)` |
| **6. VFS / DevFS / TTY** | `fs_boot_prepare()`, `pty_init()`, `device_nodes`, `fs_boot_mounts()`, `tty_boot_init()` | **无**（暂无文件系统与 TTY 驱动） | **条件宏隔离**：`#if OS01_HAS_VFS`，严格恢复错误返回值检查与 `kpanic()` |
| **7. SMP / AP 启动** | `x86_64_boot_aps()` (`smp_boot_aps`) | `smp_boot_aps()` → 时钟源自注册/阶段 4 → `arch_tick_start()` → shootdown 探针 | **Facade 抽象**：`arch_boot_aps()`，完整保持 aarch64 原有时序 |
| **8. Post-SMP 自测/探针** | `selftest_begin_run(8)` + `selftest_run_all()` | `test_aarch64_page_table_multicore_run()` + GIC 调度探针 | **Facade 抽象**：`arch_post_smp_selftest()` |
| **9. Futex / Console** | `futex_init()`, `console_init()` | **无** | **条件宏隔离**：`#if OS01_HAS_USERSPACE` |
| **10. 调度与用户态** | `sched_boot_creation_begin()`, `task_init()`（生成 `/bin/init`） | 换栈至 `init_task_union`，进入 `arch_aarch64_bsp_handoff()` 执行内核测试线程并 idle | **条件分支隔离**：`#if OS01_HAS_USERSPACE` 走用户态 init，`#else` 走 `arch_start_kernel_idle()` |

---

## 3. Feature Gates 规范与定义契约

新增 `kernel/include/arch/features.h`，提供编译期静态特性派发，并强制进行类型约束与依赖校验：

```c
#ifndef ARCH_FEATURES_H
#define ARCH_FEATURES_H

#ifndef OS01_HAS_VFS
#if defined(__x86_64__)
#define OS01_HAS_VFS        1
#elif defined(__aarch64__)
#define OS01_HAS_VFS        0
#else
#error "Unsupported architecture for OS01_HAS_VFS default"
#endif
#endif

#ifndef OS01_HAS_USERSPACE
#if defined(__x86_64__)
#define OS01_HAS_USERSPACE  1
#elif defined(__aarch64__)
#define OS01_HAS_USERSPACE  0
#else
#error "Unsupported architecture for OS01_HAS_USERSPACE default"
#endif
#endif

/* 编译期契约：特征宏必须严格定义为 0 或 1，不得留空或未定义 */
#if (OS01_HAS_VFS != 0 && OS01_HAS_VFS != 1)
#error "OS01_HAS_VFS must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_USERSPACE != 0 && OS01_HAS_USERSPACE != 1)
#error "OS01_HAS_USERSPACE must be explicitly defined as 0 or 1"
#endif

/* 依赖契约：用户态基础设施必须依赖 VFS 文件系统 */
#if (OS01_HAS_USERSPACE && !OS01_HAS_VFS)
#error "OS01_HAS_USERSPACE requires OS01_HAS_VFS"
#endif

#endif /* ARCH_FEATURES_H */
```

---

## 4. 栈帧门禁保障与内存初始化抽取

### 4.1 栈帧溢出机理
`kernel/arch/aarch64/boot/main.c:460` 包含：
```c
struct MEMORY_RANGE scratch[MEMORY_RANGE_MAX]; // 64 * 24 = 1536 字节
```
若将该代码块直接内联在 `arch_boot_memory()` 中，在 `-O2` 优化下其栈帧将达 **1584 字节**。现有 `qemutests/stack_frame_audit.py` 规定默认函数栈帧上限为 **512 字节**，且仅向旧入口 `aarch64_main` 授予了 1600 字节的基线例外。若直接放入 `arch_boot_memory()`，既会打破 512 字节通用编排函数约束，又会导致静态审计直接红牌失败。

### 4.2 解决方案：抽离独立早期预检函数
将 1536 字节的局部数组严格隔离在独立的 `noinline` 辅助函数中：

```c
/* kernel/arch/aarch64/boot/main.c */
__attribute__((noinline))
static void aarch64_early_arena_preflight(const struct boot_context *handoff)
{
    struct MEMORY_RANGE scratch[MEMORY_RANGE_MAX];
    size_t n = pmm_arch_normalize(handoff, scratch);
    if (n == 0) {
        log_err("[smp] FATAL: pmm_arch_normalize returned no ranges\n");
        for (;;) arch_cpu_halt();
    }
    if (aarch64_early_arena_prepare(scratch, n) != 0) {
        log_err("[smp] FATAL: aarch64 M1 arena preflight failed\n");
        for (;;) arch_cpu_halt();
    }
}
```

### 4.3 静态审计例外精准迁移
在 `qemutests/stack_frame_audit.py` 及 `qemutests/test_stack_frame_audit.py` 中：
- 删除旧入口例外：`"aarch64_main": 1600`
- 新增专用辅助函数例外：`"aarch64_early_arena_preflight": 1600`
- `kernel_main()` 与 `arch_boot_memory()` 的栈帧均严格受控在 512 字节以内（实测 < 64 字节），不放宽任何顶层通用函数的阈值。

---

## 5. 自测（Selftest）协议与时序对齐

### 5.1 `selftest_begin_run(unsigned int expected_late)` 参数语义
根据 `kernel/selftest/result.c:95-112`：
- `selftest_begin_run()` 的参数 **必须是 `expected_late`**（即预期在后续阶段通过 `selftest_record_late()` 登记的用例数）。
- 若传入值与实际注册的 `late_count` 不匹配，协调器将输出错误并置位 `run_error`，导致协议失败。

### 5.2 各架构与变体对应值矩阵

| 架构 / 变体 | `expected_late` 理论值 | 晚期测试注册位置 | 晚期测试执行与实现位置 | `selftest_end_run()` 调用归属 |
|---|---|---|---|---|
| **x86_64** (普通/Selftest) | **8** | `kernel/selftest/selftest.c` | `task_init()` 启动的内核调度线程 | `task_init()` 尾部 |
| **aarch64** (普通 `selftest`) | **9** | `kernel/selftest/selftest.c` | `arch_aarch64_bsp_handoff()` (`test_aarch64_scheduler.c`) | `arch_aarch64_bsp_handoff()` |
| **aarch64** (`sched-ap`) | **7** | `kernel/selftest/selftest.c` | `arch_aarch64_bsp_handoff()` (`test_aarch64_ap_scheduler.c`) | `arch_aarch64_bsp_handoff()` |
| **aarch64** (`sched-wake`) | **13** | `kernel/selftest/selftest.c` | `arch_aarch64_bsp_handoff()` (`test_aarch64_ap_wake.c`) | `arch_aarch64_bsp_handoff()` |
| **aarch64** (`sched-tick`) | **7** | `kernel/selftest/selftest.c` | `arch_aarch64_bsp_handoff()` (`test_aarch64_sched_tick.c`) | `arch_aarch64_bsp_handoff()` |

### 5.3 架构双层自测与时序对齐
1. **Pre-Subsystems 自测 (`arch_pre_subsys_selftest`)**：
   - aarch64：在 Phase 2（内存/Slab 就绪）之后、Phase 3（DTB/GIC 就绪）之前，执行 `selftest_begin_run(arch_selftest_expected_late())` 与 `selftest_run_all()`，并紧随触发 `AARCH64_SYNC_FAULT_TEST`。保证页表自测运行在干净单核环境。
   - x86_64：空操作（x86 自测包含 `vfs_lookup("/")`，必须在 VFS 挂载完成后执行）。
2. **Post-SMP 自测 (`arch_post_smp_selftest`)**：
   - aarch64：执行多核页表测试 `test_aarch64_page_table_multicore_run()` 与 GIC dispatch 探针。
   - x86_64：执行 `selftest_begin_run(8)` 与 `selftest_run_all()`。

---

## 6. 架构接口（Facade）设计

### 6.1 抽象接口声明 (`kernel/include/arch/boot.h`)

```c
#ifndef ARCH_BOOT_H
#define ARCH_BOOT_H

#include <core/bootinfo.h>
#include <arch/features.h>

/**
 * 阶段 1：架构早期硬件与异常基址配置
 * - x86_64: 载入 GDT/IDT/TSS，初始化 8250 串口，设置 syscall MSR
 * - aarch64: 初始化 PL011 串口，载入 VBAR_EL1 异常向量表，规范化 RAM 映射
 */
void arch_early_init(const struct boot_context *bootctx);

/**
 * 阶段 2：物理与虚拟内存子系统配置
 * - x86_64: pmm_init -> arch_boot_direct_map_init -> slab_init -> frame_buffer_init
 * - aarch64: aarch64_early_arena_preflight -> pmm_init -> direct_map -> arch_vmm_init -> aarch64_boot_fb_init
 */
void arch_boot_memory(const struct boot_context *bootctx);

/**
 * 阶段 2.5：Pre-Subsystems / 早期自测与故障探针
 * - x86_64: 空操作
 * - aarch64: 执行 selftest_begin_run() + selftest_run_all()，紧随其后执行 AARCH64_SYNC_FAULT_TEST
 */
void arch_pre_subsys_selftest(const struct boot_context *bootctx);

/**
 * 阶段 3：平台中断控制器
 * - x86_64: ACPI MADT 解析 -> APIC/IOAPIC -> arch_register_subsys -> subsys_init_all
 * - aarch64: DTB 解析 -> GICv2 初始化
 */
void arch_boot_subsystems(const struct boot_context *bootctx);

/**
 * 阶段 4：BSP 本地 per-CPU 拓扑与存储基址配置
 * - x86_64: 设置 GS base，percpu_init(0)，发布 num_cpus
 * - aarch64: 设置 TPIDR_EL1，percpu_init(0, mpidr)，发布 num_cpus 与 smp_starting_enter
 */
void arch_boot_percpu(void);

/**
 * 阶段 7：多核 SMP 唤醒、时钟源启动与同步
 * - x86_64: tick_start -> smp_boot_aps -> SUBSYS_PHASE_PERCPU
 * - aarch64: smp_boot_aps -> test_spinlock -> arch_register_subsys -> subsys_init_phase(4) ->
 *            clocksource 标记 -> softirq_init -> arch_tick_start -> shootdown_probe
 */
void arch_boot_aps(void);

/**
 * 阶段 8：Post-SMP 自测与探针
 * - x86_64: 执行 selftest_begin_run(8) + selftest_run_all()
 * - aarch64: 执行 test_aarch64_page_table_multicore_run() 及 GIC dispatch 探针
 */
void arch_post_smp_selftest(void);

/**
 * 设备节点注册（VFS 开启时使用）
 * - x86_64: 注册 keyboard/mouse/fb devfs 节点，失败必须返回非 0 并在调用处 kpanic
 * - aarch64: 弱默认返回 0
 */
int arch_boot_device_nodes(void);

/**
 * 早期致命错误停机输出（可在 arch_early_init 之前调用，不依赖有效 bootctx，负责停机）
 * - x86_64: 确保 COM1 串口可用，输出 "*** Corrupt UEFI handoff ***\n" 并 halt
 * - aarch64: 内部调用 pl011_init()，输出 "UEFI-A64: corrupt handoff\n[smp] FATAL: invalid UEFI handoff\n" 并 halt
 */
void arch_early_panic(const char *msg) __attribute__((noreturn));

#if !OS01_HAS_USERSPACE
/**
 * 纯内核态调度切换与 handoff（用于尚未开启用户态的架构，如 aarch64）
 */
void arch_start_kernel_idle(void) __attribute__((noreturn));
#endif

#endif /* ARCH_BOOT_H */
```

---

## 7. 单一 `kernel_main()` 统一逻辑实现

在 `kernel/core/main.c` 中整合后的核心流程：

```c
__attribute__((no_stack_protector))
int kernel_main(const struct boot_context *bootctx)
{
    // ═══ 0. 栈保护 Canary（必须为第一条语句） ═════════════════════════
    __stack_chk_guard = arch_cycle_counter() ^ 0xDEADBEEFCAFEBABE;

#if defined(__x86_64__) && defined(OS01_CANARY_SELFTEST)
    kernel_canary_selftest_trip();
    __builtin_unreachable();
#endif

    // ═══ 0.5. 固件 Handoff 数据完整性校验（内部保证 UART 可用并停机） ═══
    if (!boot_context_valid(bootctx)) {
        arch_early_panic("corrupt UEFI handoff");
    }

    // ═══ 1. 架构早期硬件与异常基准 ══════════════════════════════════
    arch_early_init(bootctx);

    // ═══ 2. 内存子系统（PMM / Direct Map / Slab / Framebuffer）══════
    arch_boot_memory(bootctx);

    // ═══ 2.5. 早期 Pre-SMP 自测与探针（aarch64 纯净环境 / sync-fault）
    arch_pre_subsys_selftest(bootctx);

    // ═══ 3. 中断控制器与驱动子系统自注册分派 ════════════════════════
    arch_boot_subsystems(bootctx);

    // ═══ 4. BSP 本地 per-CPU 运行环境 ══════════════════════════════
    arch_boot_percpu();

    // ═══ 5. 跨架构统一 CSPRNG 熵池初始化 ═══════════════════════════
    random_init(bootctx);

#if defined(__x86_64__) && (defined(X86_INTR_FAULT_TEST_BP) || defined(X86_INTR_FAULT_TEST_NMI))
    x86_intr_fault_probe_run();
    for (;;) arch_cpu_halt();
#endif

#if OS01_HAS_VFS
    // ═══ 6. 存储、VFS 文件系统与设备树节点 ══════════════════════════
    if (fs_boot_prepare() != 0)
        kpanic("fs_boot_prepare failed\n");

    pci_devfs_register_all();
    usb_devfs_register_all();

#if defined(CAPACITY_TEST) && CAPACITY_TEST
    void arch10_capacity_fixture_init(void);
    arch10_capacity_fixture_init();
#endif

    if (pty_init() != 0)
        log_err("pty_init failed\n");

    // 恢复严格错误检查：设备节点注册失败必须 kpanic
    if (arch_boot_device_nodes() != 0)
        kpanic("arch_boot_device_nodes failed\n");

    fs_boot_mounts();

    // ═══ 7. 控制台 TTY 线路规程与 DevFS 冒烟 ════════════════════════
    if (tty_boot_init() != 0)
        log_err("tty_boot_init failed\n");
    fs_boot_probe_devfs();
#endif /* OS01_HAS_VFS */

    // ═══ 8. SMP / 多核 AP 启动与时钟源激活 ═════════════════════════
    arch_boot_aps();

    // ═══ 8.5. Post-SMP 自测与探针（x86_64 selftest / aarch64 多核探针）
    arch_post_smp_selftest();

#if OS01_HAS_USERSPACE
    // ═══ 9. 用户空间调度与 PID 1 (/bin/init) 启动 ═══════════════════
    sched_boot_creation_begin();
    futex_init();
    console_init();
    task_init(); // 派生 init.elf 并进入 EEVDF 调度
#else
    // ═══ 非用户态架构（aarch64 当前形态）：切至任务栈并进入内核 idle ═══
    arch_start_kernel_idle();
#endif

    while (1) arch_cpu_halt();
    return 0;
}
```

---

## 8. 构建适配、汇编调整与契约测试重构

### 8.1 aarch64 构建适配 (`kernel/Makefile`)
1. **源码白名单加入 `core/main.c`**：
   在 `kernel/Makefile` 的 aarch64 分支中显式追加通用入口：
   ```makefile
   ifeq ($(ARCH),aarch64)
   KERNEL_C_SOURCES := core/main.c \
                      memory/pmm.c memory/pmm_arch.c memory/pmm_boot.c \
                      ...
   ```
2. **共用头引用**：
   `kernel/core/main.c` 移除 `<arch/x86_64/boot.h>` 与 `<arch/x86_64/smp_boot.h>` 的直接硬编码，改为引用通用 Facade 头 `<arch/boot.h>`。
3. **基础构建门禁**：
   要求 `make PROFILE=aarch64-clang aarch64-uefi-kernel` 必须通过，产物符号表中包含定义明确的全局代码符号 `kernel_main`。

### 8.2 aarch64 `head.S` 完整适配
必须同时移除旧符号的全局导出声明与跳转目标：
```diff
--- a/kernel/arch/aarch64/head.S
+++ b/kernel/arch/aarch64/head.S
@@ -28,7 +28,6 @@
 .globl aarch64_dtb_slot
 .globl aarch64_dtb_mpidr_table
 .globl aarch64_dtb_cpu_count
-.globl aarch64_main
 .globl exception_vectors
 .globl secondary_start
 .globl secondary_panic
@@ -300,7 +299,7 @@ _start:
     /* 11. Absolute branch to kernel_main (high-half). */
     mov     x0, x19
-    ldr     x30, =aarch64_main
+    ldr     x30, =kernel_main
     blr     x30
```

### 8.3 契约测试语义化重构
解耦“代码文本物理排列”与“实际调用执行顺序”，避免整文件全局字符串索引扫描产生伪保证：
1. **顶层编排调用顺序断言**：
   更新 `hosttests/cases/test_double_state_publish.c` 与 `qemutests/test_kernel_selftest_result.py`，先扫描 `kernel/core/main.c::kernel_main()` 确认：
   `arch_boot_percpu()` 早于 `arch_boot_aps()` 调用。
2. **Helper 内部原子时序断言**：
   - 提取 `kernel/arch/aarch64/boot/main.c` 中 `arch_boot_percpu()` 的函数体，断言内部时序：
     `percpu_install_gs` → `percpu_init` → `online=1` → `num_cpus` → `smp_starting_enter()`。
   - 提取 `arch_boot_aps()` 的函数体，断言内部时序：
     `smp_boot_aps()` → ... → `arch_local_irq_enable()` → `ipi_ready_publish_and_count(0)` → `aarch64_shootdown_probe()`。
3. **负向测试**：
   测试套件包含用例证明：颠倒 helper 函数在 `main.c` 源码中的定义排列顺序不会影响测试通过；但若故意调换 `kernel_main()` 中的调用顺序，测试必须坚决报错失败。

### 8.4 工具配置与审计门禁升级 (`mk/components/run.mk`)
在 `mk/components/run.mk` 中显式定义 aarch64 审计所需的 `readelf` 工具，修复管道退出码屏蔽，并严格末尾匹配符号：

```makefile
# mk/components/run.mk
AARCH64_READELF ?= llvm-readelf

test-aarch64-audit: $(BUILD_DIR)/artifacts/kernel.elf
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@if ! command -v $(AARCH64_READELF) >/dev/null 2>&1; then \
	    echo "AUDIT GATE FAIL: $(AARCH64_READELF) not found in PATH" >&2; exit 1; \
	fi
	@echo "  [audit] aarch64 kernel.elf: verifying kernel_main entry symbol"
	@syms="$$($(AARCH64_READELF) -Ws $(BUILD_DIR)/artifacts/kernel.elf)" || { \
	    echo "AUDIT GATE FAIL: $(AARCH64_READELF) exited with failure on $(BUILD_DIR)/artifacts/kernel.elf" >&2; exit 1; \
	}; \
	if ! printf '%s\n' "$$syms" | grep -E 'FUNC[[:space:]]+GLOBAL[[:space:]]+DEFAULT[[:space:]]+[0-9]+[[:space:]]+kernel_main[[:space:]]*$$' >/dev/null; then \
	    echo "AUDIT GATE FAIL: valid GLOBAL kernel_main entry missing from aarch64 kernel.elf" >&2; exit 1; \
	fi; \
	if printf '%s\n' "$$syms" | grep -E '[[:space:]]+aarch64_main([[:space:]]|$$)' >/dev/null; then \
	    echo "AUDIT GATE FAIL: obsolete aarch64_main symbol (defined or undefined) still present in aarch64 kernel.elf" >&2; exit 1; \
	fi
```

### 8.5 接入 `TEST_HARNESS_MODULES`
在 `mk/components/run.mk:716` 的 `TEST_HARNESS_MODULES` 追加新审计契约测试模块：
```makefile
TEST_HARNESS_MODULES := ... qemutests.test_audit_symbol_contract
```
`test_audit_symbol_contract.py` 覆盖：
- 正常合法 ELF 解析：通过
- 缺少 `kernel_main`：断言失败
- `kernel_main` 仅为 `UND` 未定义符号：断言失败
- 后缀错误符号（如 `kernel_main.extra`、`kernel_main_alt`）：断言拒绝（严格行尾完整字段匹配）
- 残留 `aarch64_main`：断言失败
- `AARCH64_READELF` 工具缺失：显式非零报错
- `AARCH64_READELF` 在输出合法内容后以非零退出码退出：断言失败（捕获工具本身错误状态）

---

## 9. 完整验证与验收命令

实施此设计必须通过以下全部 canonical 门禁：

### 9.1 静态审计、宿主测试与 Harness
```bash
# 1. aarch64 专用内核编译检查
make PROFILE=aarch64-clang aarch64-uefi-kernel

# 2. x86_64 静态审计（layout, boundary, stack frame 严格受控）
make test-static

# 3. aarch64 静态审计（symbol leak, readelf kernel_main presence, no obsolete aarch64_main）
make PROFILE=aarch64-clang test-aarch64-audit

# 4. 宿主 C 单元测试
make test-host

# 5. Python 测试框架回归（包含 test_audit_symbol_contract 及其全部边界测试）
make test-harness
```

### 9.2 x86_64 端到端回归
```bash
# Phase-0 基础启动（验证 VFS, TTY, /init.elf 用户态拉起）
make test-qemu SUITE=phase-0

# 系统调用完整回归（228 个 syscall）
make OS01_SYSTEST=1 test-qemu SUITE=systest

# 内核内置 selftest
make KERNEL_SELFTEST=1 test-kernel-selftest

# 容量测试变体回归（验证 CAPACITY_TEST=1 编译与运行无隐式声明错误）
make CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity
```

### 9.3 aarch64 完整测试矩阵
```bash
# 1. 基础多核 SMP 与 Generic Timer 证据（1/2/4 核）
make PROFILE=aarch64-clang test-aarch64 MODE=smp

# 2. 启动字标与 Framebuffer 显示
make PROFILE=aarch64-clang test-aarch64-boot-logo

# 3. 调度器单核/多核回归
make PROFILE=aarch64-clang test-aarch64 MODE=sched

# 4. AP 协作调度生产路径（7 项 case）
make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap

# 5. AP 调度唤醒路径（13 项 case）
make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake

# 6. 每核调度 CNTP Tick 与记账（7 项 case）
make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick

# 7. 故障注入变体回归
make PROFILE=aarch64-clang test-aarch64 MODE=no-ack
make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault
make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe
```
