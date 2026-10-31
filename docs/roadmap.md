# OS01 优化路线图

> **基准**: `4ac18f54`（2026-10-23）；GUI-1/GUI-2 闭环后重整于 2026-10-22，aarch64 M2/M3 状态对齐于 2026-10-23
> **日期**: 2026-10-23

roadmap 只列**未完成 / 进行中**的规划项；所有已完成工作见 `docs/changelog.md`。

## 当前状态（一句话）

- **Phase 1-9**：全部就绪（COW/mmap、调度/信号/SMP、文件系统、设备驱动、用户态、poll/select、网络、时间系统）
- **工程与架构治理**：AAGU 全套就绪；2026-10-04 规划 ARCH-1..10 架构治理任务（syscall 解耦、Linux ABI 兼容层独立、Muldefs 消除、调度器/VFS/驱动抽象治理等）
- **aarch64 适配**：Generic Timer 全栈 + GICv2 Phase 1 + IPI fix + EL1 sync 致命诊断 + M0 启动直映 + M1 运行期直映 + **M2 Slab 接入 + M3 内核 VMM 已闭环**（merge `2034e5bc`，2026-10-05，spec v9 / plan v4）；M4 用户态 VMA 与统一 kernel_main 尚未启动
- **GUI**：PS/2 鼠标、`libgfx.a` + `/dev/gfx0`、Tetris 与 terminal 迁移已闭环；QEMU 分辨率切换与显示所有权协调器（独占接管/fallback 自动恢复/只读 raw mmap 保护）已闭环；LVGL v9.5.0 初步移植与图形 showcase 已合并；**GUI-1 LVGL 平台后端已闭环**（`libgui` 公共平台层：显示/tick/flush/生命周期、离屏与屏幕后端分离）；**GUI-2 输入与焦点已闭环并合入 `master`**（独占物理输入租约、LVGL indev、光标、修饰键、重复、Tab 焦点，QEMU SMP 1/2 共 24/24 通过）；**GUI-3 Material 风格桌面已闭环**（令牌表 + 主题落地 + 状态栏/启动器/抽屉/设置 + 四个壳内应用，`SUITE=gui-desktop` 端到端门禁）；后续推进窗口系统与 GUI-4 字体资源
- **安全与内存**：用户堆与 ELF 映射隔离（4 KiB ELF 分段加载、512 MiB 用户信封、brk 动态收缩与页解绑、保护范围审计）已闭环；用户态 ASLR 规划中

详细背景、commit 记录、经验教训见 `docs/changelog.md` + 各专题 closure 文档（`docs/archive/aarch64/`）+ 主题 docs。

---

## 待实施路线图（按 5 优先级）

> **P0 工程基础** ✅ → **P1 安全加固** → **P2 aarch64 适配 + 架构治理/扩展性** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### 🔒 P1 安全加固

前置链（已完成，详见 `docs/changelog.md`）：`getrandom` → 统一用户态启动方式 → 用户栈 canary + `AT_RANDOM` → AAGU-5 arch entropy facade（STRONG-only 控制流）→ 用户堆与 ELF 映射隔离（4 KiB ELF 分段加载、512 MiB 用户信封、brk 动态收缩与页解绑、保护范围审计，2026-10-02 闭环，commit `277b6315`）。ASLR 的随机种子须由内核 STRONG-only 接口提供；`AT_RANDOM` 已就绪，但不是 mmap 选址函数的直接输入。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| ASLR-A：mmap 基址 | 先限 x86_64 用户态：每次新地址空间建立时随机化 mmap 搜索基址；fork 继承现有布局；保留 MAP_FIXED 语义，并验证地址窗口、冲突与溢出 | STRONG-only `kernel_random_get_strong()` ✅；需先定熵不可用时的失败语义与用户 VA 窗口 | |
| ASLR-B：ET_DYN/PIE | 单独实施可重定位 ELF 的加载偏移、静态 PIE 重定位及用户程序构建迁移（含 BusyBox） | ASLR-A 验收；当前 ELF loader 仅支持 ET_EXEC、用户构建使用 `-fno-pie -no-pie`；需设计重定位、linker script、启动 ABI/auxv 与回退测试 | |
| UBSan + KASan | 内核编译期 instrument | 独立 | ArvernOS |
| 堆加固 | malloc double-free/溢出检测 | 独立 | |
| NX 页 | 栈/堆不可执行 + mmap `PROT_EXEC` 审计 | 独立 | |

ASLR 分期实施，不把 A/B 合成一个小任务。当前用户栈固定在 `USER_STACK_BASE=0x1400000`；栈随机化另列后续范围，完成 A/B 后也不能称为完整用户态 ASLR。aarch64 phase 1 尚无用户态，本项先不扩大到 aarch64。

### 🏗 P2 aarch64 适配

已完成：v25 arch-cleanup / PMM arch-neutral / 页表原语 / Generic Timer Phase 1 + Phase 2 #1~#5 / GICv2 Phase 1 + GIC probe fix / AAGU-3 subsys_stub convergence / AAGU-29 libk.a link / IPI TPIDR_EL1 fix / EL1 sync 致命诊断 / M0 启动直映契约 / M1 运行期直映严式页表树 / **M2 Slab 接入 / M3 内核 VMM（语义层拆分 + aarch64 backend + SMP shootdown + M3.1 audit gate）**（详见 `docs/changelog.md` + `docs/archive/aarch64/` 各专题报告）。

#### 内存管理：共同启动直映接口（2026-10-02）

两种架构现在都在 PMM 初始化后调用 `arch_boot_direct_map_init()`，通过 `arch/boot_memory.h` 查询 readiness 与不可变、合并的 RAM coverage。x86_64 沿用 2 MiB `vmm_init()`，保留 `ZONE_UNMAPPED_INDEX` 非零时的覆盖截止语义，并传播中间页表分配失败。aarch64 的 M0 在 `head.S` 中提供普通/自测一致的 0..2 GiB 启动映射；M1 从低窗口 arena 建立独立 TTBR1，只映射真实 RAM、启动保留 block 和设备窗口，再启动 AP。两者的页表编码、偏移与 TLBI 留在架构实现内。

以下是**实施顺序**，每项单独设计与验收。启动直映（M0/M1）与运行期 VMM（M3）已分别闭环并合入 `master`：M3 提供 `arch_vmm_*` 架构中立语义接口、4 KiB/2 MiB 原语与跨核 TLB shootdown，不再依赖 x86 页表 flag。RAM 范围以 UEFI 归一化结果为准，固定映射到 `0x80000000` 不能代替任意内存容量及稀疏范围的处理。

| 阶段 | 任务与完成条件 | 前置 |
|------|----------------|------|
| M0 启动映射契约 ✅ | `head.S` 在 MMU 打开前建立 0..2 GiB boot map，内核 block 保持 EL1 可执行，其余 RAM PXN/UXN；普通/自测一致，已移除 C 补图差异。 | 现有 boot 页表、PMM ✅ |
| M1 运行期直映 ✅ | 共同 `arch_boot_direct_map_*` 接口；aarch64 arena 建立独立 TTBR1，收紧非 RAM 映射并覆盖高 RAM/holes；BSP/AP root 与 probe 验证，16 组矩阵和稀疏/耗尽/坏 root 注入通过。 | M0；RAM 归一化、PMM ✅ |
| M2 Slab 实装与初始化顺序 ✅ | **已完成（2026-10-05，merge `2034e5bc`）**：删除占位实现 `kernel/arch/aarch64/runtime/slab_stub.c`（`slab_init()` 无操作、`kmalloc()` 返回 `NULL`），aarch64 改编译生产 `memory/slab.c`（`kernel/Makefile` aarch64 白名单）；`slab.c` 中 x86 专属 `pushfq`/`cli`/`sti` 锁路径改为 `arch_local_irq_save/restore`；`slab_init()` 的 j-loop 与 8 页记账**幂等化**，与 boot reservation 不重复计数；新增 `slab_layout_compute()` 纯函数真值源，供 `early_arena.c` preflight 与启动后断言共用，arena 候选扫描计入 slab 段字节（`pmm_init()` 顺序保持 PMM 元数据 → 运行期直映 → Slab，x86_64 预留语义不变）。验收：`test_slab_basic` / `test_slab_layout_compute` / `test_slab_lock_path` / `test_slab_idempotent_reservation` / `test_slab_basic_x86_count` / `test_arena_candidate_slab_bytes` / `test_aarch64_color_printk_abi` 全 PASS；PMM boot/slab reservation 6 组通过。 | M1 ✅；PMM ✅ |
| M3 内核 VMM 接口 ✅ | **已完成（2026-10-05，merge `2034e5bc`）**：公共 `kernel/include/memory/vmm.h` 拆为**语义层**（仅 `VM_*` + `arch_vmm_*` 语义 API），x86 裸 PTE 位与 `PAGE_*` 迁入私有头 `kernel/include/arch/x86_64/pte.h`，`kernel/memory/vmm.c` 退化为 backend wrapper 并原样保留 free/COW 所有权逻辑（`kernel_map=Phy_To_Virt(0x101000)` 不再泄漏进 aarch64 源清单）；aarch64 侧新增 `kernel/include/arch/aarch64/vmm_backend.h` + `kernel/arch/aarch64/memory/vmm_backend.c`（`arch_vmm_*` 实现，errno 经 `pt_err_to_linux()` 归一化为 Linux errno）；`page_table.c` 补齐 walk_to_l2/l3、4 KiB map/unmap/replace、2 MiB block 与 split（已发布 root 拆分返回 `-EPERM`）、root 发布登记制与软件位编码；SMP TLB 失效走 `kernel/memory/tlb.c` 串行化协议 + 每核 `tlb_ack_gen` 代数 ack + 超时 FATAL，经 `kernel/arch/aarch64/smp/ap_work.c` 工作槽与 SGI 3 广播。验收：**M3.1 audit gate 通过**（`docs/memory/memory.md` §vmm 变更调用链审计清单 + `test_vmm_caller_audit.c` 源码扫描 + `make PROFILE=aarch64-clang test-aarch64-audit` nm 扫描，2026-10-23 复跑 `[audit] OK`）；host 用例 `test_aarch64_backend_4k` / `test_aarch64_pt_2m_block` / `test_aarch64_pt_split` / `test_aarch64_pt_software_bits` / `test_aarch64_pt_root_publish` / `test_aarch64_pt_locks` / `test_vmm_replace_protocol` / `test_vmm_prot_none` / `test_vmm_ack_wraparound` / `test_vmm_semantic_header` / `test_x86_vmm_backend` / `test_aarch64_arch_vmm_init` 全 PASS；QEMU `qemutests/aarch64_m3_probe.py` 当时产出 `M3-SHOOTDOWN-PROBE: OK`（wire 前缀曾在 `045cba3f` 改名后与 runner 漂移；2026-10-24 已对齐为 `SHOOTDOWN-PROBE:` 并经正式双核 QEMU 验收通过，见 [修复记录](debug/2026-10-24-shootdown-probe-investigation.md#修复与正式验收)）；多核 selftest `kernel/selftest/test_aarch64_page_table_multicore.c` 覆盖 target-set / partial-mask / 锁交错 / 共享 L1。并发 replace phantom `-ENOENT` 已于 2026-10-19 根因钉死并修复（见 Parked 段）。 | M1 ✅、M2 ✅；4 KiB 页表原语 ✅ |
| EL1 BSP 协作式线程调度 ✅ | **已完成（2026-10-31，`feature/aarch64-el1-sched`，P1–P5 五阶段全过）**：BSP-only 的 aarch64 EL1 内核线程调度落地——架构上下文门面（`kernel/include/arch/context.h` + per-arch `thread_t` 布局与偏移断言，x86 偏移零漂移）、公共调度器拆分（`sched/boot.c`/`sched/kthread.c`/`sched/internal.h`，x86 行为以 phase-0/systest/kernel-selftest 回归锁死）、aarch64 EL1 切换机器码（`switch.S`/`thread_entry.S`/`task_arch.c` 预切栈上下文校验）与 BSP 栈交接（仅 CPU0 发布 `scheduler_ok=1`，AP 保持隔离与工作槽）、协作 `kthread_yield()`/blocker 四时序/`kthread_exit()` 自回收 + 九用例 protocol-v1 late selftest（单 END，total=passed=14），以及 P5 验收面（`MODE=sched` 严格证据 runner（冻结 `[sched-evidence]` 字段/顺序/谓词 + 120 s 绝对期限）、`sched-fault-*` 四个编译期故障注入变体证明坏上下文在**切栈前** fatal、宿主分配/放置/布局审计与 x86 UP boot 窗口真机探针、双架构回归矩阵）。证据与逐命令退出码见 [验证报告](superpowers/reports/2026-10-30-aarch64-el1-sched-validation.md)。**仍开放**：AP 调度/SMP 调度、IRQ 返回抢占、FPU/SIMD 上下文、EL0/用户地址空间（M4）、统一 `kernel_main`。 | M1–M3 ✅ | Linux sched |
| M4 用户地址空间与 VMA | 建立 aarch64 用户页表根、EL0 权限和地址空间切换/回收，再使 VMA/mmap/ELF、缺页分配、COW 与 `munmap` 使用 M3 接口；把 `arch_user_range_accessible()` 的 aarch64 fail-closed 实现替换为真实跨页权限检查，并接通 uaccess 故障恢复。现有 `memory/vma.c` 和 x86_64 `do_page_fault()` 直接使用 x86 PTE flag，需先剥离架构语义；EL1 sync 目前只有致命诊断，EL0 sync 入口仍未接入。以隔离、权限、COW、回收和用户态 QEMU 用例验收。**当前状态：尚未开始（仓库内无对应实现代码）** | M3 ✅；Slab ✅、调度/上下文切换、EL0 异常路径 |

**当前已具备的部分**：aarch64 的 RAM 归一化及共用 `PMMngr`/`alloc_pages()`/`alloc_4k_page()`；BSP 启动期 4 KiB map/query/unmap smoke；**生产 Slab**（跨缓存大小分配/释放，`kmalloc()` 不再返回 `NULL`）与 **`arch_vmm_*` 语义接口**（4 KiB/2 MiB 映射、解除、替换、拆分 + 跨核 shootdown）。`aarch64_pt_range_accessible()` 已存在于页表原语，但 `arch/mmu.h::arch_user_range_accessible()` 仍返回 `false`，不能视为 uaccess 已接通。M0、M1、M2、M3 已独立验收并合入 `master`；**M4 尚未开始**。M1 的 16 组 RAM/CPU/镜像矩阵与稀疏、容量耗尽、AP 无效 root 注入记录、以及 M3.1 的 vmm 调用链审计清单见 `docs/memory/memory.md`；M2/M3 的实施交付记录见 [`docs/changelog.md` 2026-10-05 条目](changelog.md#2026-10-05)。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 统一 kernel_main | 单 `kernel_main` 按固定顺序调 `arch_early_init()` / `arch_late_init()` / `scheduler_init()`；aarch64 必须先 dtb 才能用 DTB 信息；内存阶段须满足 M0→PMM→M1→M2/M3 的资源可用顺序（该顺序已随 M2/M3 落地验证）。**Spec A 中断 dispatch / Spec B SMP+timer / Spec C kernel_main 单一入口** | M1–M3 的顺序契约 ✅（M2/M3 已闭环）；SMP ✅, arch_irq ✅, GIC ✅, Timer ✅, UEFI 链 ✅ | 长项 spec |
| 中断/异常 dispatch 统一 | **第一期主体已完成（2026-10-24）**：新增 arch-neutral 无状态事件分派 `kernel/intr/dispatch.c::arch_intr_dispatch()` + 事件契约 `kernel/include/intr/dispatch.h`（kind/origin/reason/access + `fault_addr_valid`/`fault_addr` + `native_vector`/`native_status`/`native_address` 原始快照），每架构提供强定义后端 `arch_intr_handle_sync` / `arch_intr_handle_irq` / `arch_intr_bad_event`（无 weak no-op）。**已统一的六条入口**：aarch64 EL1h sync、aarch64 EL1h IRQ、x86 #DE/#UD/#GP/#PF。四种控制流（正常返回、故障恢复、用户任务终止、内核致命停机）与原诊断/ABI 保持不变；第一期的 x86 IRQ 后端原为 fail-closed；第二期已接入当前安装的生产入口。证据见 [验证报告](superpowers/reports/2026-10-24-intr-dispatch-phase1-validation.md)。**遗留验收已闭环（2026-10-24）**：runner 的 wire 标记与内核对齐为 `SHOOTDOWN-PROBE:`，新增真实生产者输出回归；正式 `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` PASS，START/OK 各一次、无 probe SKIP/FAIL。原先由 `045cba3f` 引入的前缀漂移及历史失败保留在阶段一报告，后续修复证据见 [修复记录](debug/2026-10-24-shootdown-probe-investigation.md#修复与正式验收)。**x86 第二期已完成（2026-10-25）**：当前安装的 20 条异常及 28 条 IRQ（含 LAPIC timer、TLB/resched IPI、`0xff`）均经共同事件分派；中立注册表与 x86 入口分离，MADT NMI 路由及 AP IST 就绪顺序修复。证据见 [第二期验证报告](superpowers/reports/2026-10-25-intr-dispatch-phase2-x86-validation.md)；内核 selftest 的既有 `fd_refcount` 失败仍单列，不称整套通过。**仍未做**：EL0/SVC 异常、aarch64 可恢复 abort 与 uaccess 恢复路径、用户页表、ASID/COW、上下文切换、统一 SMP、统一 `kernel_main`。原 roadmap 简写的 `arch_intr_dispatch(vector, pt_regs*)` 已细化为带类型的事件结构（IDT vector / VBAR 槽 / syndrome / INTID 不混用）。 | arch_irq ✅, head.S ✅, GIC ✅；M4 用户态路径仍未开始 | Linux do_IRQ |
| SMP 启动统一 | `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` 单入口；内部 aarch64 PSCI CPU_ON / x86_64 INIT-SIPI + trampoline 各自实现 | GIC ✅, 启动链 ✅ | opuntiaOS |
| 上下文切换统一 | `arch_task_switch(prev, next)` + `arch_thread_entry()`；aarch64 ret 到 user vs x86_64 sysret/iret | SMP 启动统一 | Tilck |
| CPU 特性探测 | `arch_cpu_features()` 返回统一位图（has_fpu / has_virt / has_cache_coherency）；x86 CPUID vs aarch64 ID_AA64* 各实现一份 | 独立 | Linux cpufeature |
| GICv2 Phase 2 | SError/真机覆盖率；`aarch64_main` 接 GIC init；handler 表扩容（SGIs 0-15 + PPI 16-31 全注册）；dtb_gicd_base 解耦 | GICv2 Phase 1 ✅ | |

**距离单一 kernel_main 还差多远（粗估，一个人全职）**：~4–8 周。详见 `docs/arch.md` 末段 3 个 spec/plan 增量推进。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。靠 arch 抽象层封装，统一接口、不统一实现。

### 🧱 P2 架构治理 / 扩展性（2026-10-04 仓库扫描）

来源：2026-10-04 全仓扫描（架构合理性 / 扩展性）确认的 10 项问题，与 aarch64 适配同属 P2。ARCH-1/2 是 aarch64 用户态（M4、中断/异常 dispatch 统一）的前置；ARCH-3、ARCH-6 属正确性风险，P2 内优先处理。

**ABI 决策（2026-10-04）**：OS01 自有 syscall ABI（`kernel/include/uapi/syscall.h`）为**唯一标准**；Linux x86_64 ABI 仅作为兼容层（服务 BusyBox 等 `PF_LINUX_ABI` 二进制），不反向主导编号或语义。

| 项 | 问题 | 目标 / 完成条件 | 依赖 |
|----|------|----------------|------|
| ARCH-1 syscall 层脱离 arch（已完成） | 原 74 个 syscall 的业务 `switch` 位于 `kernel/arch/x86_64/intr/trap.c::do_system_call`；aarch64 用户态接入前需抽离 | 2026-10-04 完成；交付记录详见 [`docs/changelog.md` 2026-10-04 条目](changelog.md#2026-10-04) | aarch64 M4 / 中断 dispatch 统一的前置 ✅ |
| ARCH-2 Linux ABI 兼容层独立（已完成） | Linux 翻译表仍在 `do_system_call` 热路径；`static const int8_t linux_to_os01[320]` 只能表示 ≤127 的 OS01 号，超出后**静默溢出**；新增兼容映射仍需维护这张表 | 2026-10-05 完成；交付记录详见 [`docs/changelog.md` 2026-10-05 条目](changelog.md#2026-10-05) | ARCH-1 ✅ |
| ARCH-3 头文件定义全局 + `-z muldefs`（已完成） | `sched/task.h` 直接定义 `init_task_union`/`init_task[]`/`init_mm`/`init_thread`/`init_tss[]`，被 37 个 TU 包含，每个 `.o` 都有强符号；靠 `kernel/arch/x86_64/make.config` 的 `-z muldefs` 链接通过，会吞掉所有真实重复定义；x86 `struct tss_struct` 与硬编码 IST 地址位于通用调度器头 | 2026-10-04 完成；交付记录详见 [`docs/changelog.md` 2026-10-04 条目](changelog.md#2026-10-04) | 无（正确性优先） |
| ARCH-4 拆分 `sched/task.c`（已完成） | 原 2764 行混合 EEVDF 调度/负载均衡、fork(COW)、exec + 用户栈/auxv、信号/进程组、kthread；原 FS syscall 已随 ARCH-1 迁至 `kernel/syscall/sys_fs.c` | 2026-10-04 完成；交付记录详见 [`docs/changelog.md` 2026-10-04 条目](changelog.md#2026-10-04) | ARCH-1 ✅ |
| ARCH-5 VFS 抽象补强 | `vfs_ops` 无 `lookup`/`getattr`/`permission`，路径解析靠 `readdir` 线性扫描（O(n)、无 dentry cache）；`vfs_node` 缺 mode/uid/gid/时间戳/nlink；挂载按路径字符串前缀匹配；无 FS 类型注册表（`fat_vfs_ops`/`ext2_vfs_ops` 全局 extern） | 引入 `file_system_type` 注册 + mount by type；`inode_operations`/`file_operations` 分离并新增 `lookup`/`getattr`；`vfs_node` 补元数据；挂载点挂在 node 上；简单 dentry cache。分阶段 spec，每阶段 FS 回归 | 独立；完成后利好 P5 权限/FIFO/openat |
| ARCH-6 FS 并发与块缓存 | `fat.c`、`tmpfs.c` 0 处加锁（`ext2.c` 19 处），默认 `-smp 2` 下 FAT 表/簇分配/tmpfs 目录存在数据竞争；全内核无 buffer/page cache，每次读直达 AHCI | 短期：每挂载点锁兜底 FAT/tmpfs；中期：blockdev 层按 (dev, blkno) 哈希的块缓存，ext2/FAT 共用，含写回与一致性；SMP 并发读写压力用例 | 独立（正确性优先）；块缓存与 ARCH-9 块层解耦协同 |
| ARCH-7 移除 `#define mmap uint64_t*`（已完成） | 共 11 处绕行块，含 `#define mmap` 的 5 处（`kernel/include/memory/vmm.h` 唯一定义处；`kernel/include/fs/vfs.h`、`kernel/include/fs/devfs.h` 的 `#undef`+恢复；`kernel/memory/vma.c`、`kernel/driver/fb.c` 的就地恢复）、仅 `#undef mmap` 的 `kernel/fs/devfs.c` 1 处、以及 6 处 hosttests `#undef mmap`（存在理由随宏消失）。原记「任何名为 `mmap` 的标识符都会被替换」表述正确但未说明后果：**本仓库实际出现过的冲突形态**（结构体字段声明、`.mmap =` 指定初始化式、`->mmap` 访问、libc `mmap()` 调用）展开后都是非法语法，属编译期硬报错，11 处绕行块正是为此存在；**唯一真会静默出错**的是多声明符写法 `mmap a, b;` → `uint64_t* a, b;`（`b` 退化为标量），本仓库无此写法 | 已完成：`kernel/include/memory/vmm.h` 改为 `typedef uint64_t *ptbl_t;`，11 处绕行块全部删除，`int (*mmap)(...)` 字段名保持不变（成员名与普通标识符分属不同命名空间，与 libc `mmap` 不冲突）。类型用途实为 8 处（`kernel_map` / `vmm_alloc_map` / `vmm_free_user_map` / `get_next_level` + aarch64 `vmm_backend.c` 的定义与赋值）；选 `ptbl_t` 而非 `pgd_t`，因该宏同时覆盖页表根与中间级表指针 | 无；原记「宜在 aarch64 M3 VMM 接口前完成」——M3 已于 2026-10-05 闭环 |
| ARCH-8 子系统框架补完 | phase 用魔数 3–6，Phase 1–2 / 7–9 仍硬编码于 `kernel_main`；同 phase 内顺序依赖链接顺序（需 link-order 审计兜底）；必需子系统失败只打印 `FAIL` 继续启动；固定表 `MAX_SUBSYS 64`/`MAX_SUBSYS_PERCPU 16`；框架自身用 `serial_printk` 违背日志规范 | 命名 phase 枚举并覆盖全部启动阶段；支持显式依赖（或 phase 内 order 字段）取代链接顺序；非 OPTIONAL 失败 `panic`；表改链接段驱动（容量随注册数）；日志改 `log_*` | 与“统一 kernel_main”协同 |
| ARCH-9 驱动模型 / 总线抽象（已完成） | 驱动自行 `pci_find_device(class, subclass…)`，无 `pci_driver` + id_table / probe-remove；`net/net.c` 用全局 `is_virtio` 在 e1000/virtio-net 间 `if` 分支，无 `net_device` 抽象；通用 `block/blockdev.c` 内含 `default_ahci_read/write` | 2026-10-05/06 完成（commit `14119cf7..8b29b25`；分支 `arch9-driver-model` 已合并并删除，21 commit + 1 followup fix；84 文件，+16,695/-1,875）。4 阶段 12 任务全部落地；交付记录、测试矩阵与已知遗留详见 [`docs/changelog.md` 2026-10-06 条目](changelog.md#2026-10-06) | ARCH-8 ✅（依赖 phase 1 子系统框架的链接段注册，本计划借 driver 注册段先行）；P4 NVMe/USB 多级 device 表达、ARCH-11/12 的前置 ✅ |
| ARCH-10 静态容量与工程卫生 | `NR_CPUS 8` 三处重复定义；`MAX_GSI 24` 仅够单 IOAPIC，与 `MAX_IOAPICS 4` 矛盾；`POLL_MAX_FDS 16` 栈数组硬拒；`BLOCKDEV_MAX 8`、`DEVFS_MAX_DEVICES 32` 固定表。`.gitignore` 忽略全部 `*.S`，新增汇编默认不入库，例外路径 `kernel/arch/x86_64/thread_entry.S` 已过时（实为 `cpu/thread_entry.S`）；AGENTS.md 仍称“Makefile 无头文件依赖”但 `kernel/Makefile` 已 `-MD -MP`；源码树残留 `kernel/fs/select.o`、`kernel/kernel.bin` | `NR_CPUS` 单点定义；GSI 上限按 IOAPIC 枚举动态计算；poll 改堆分配 + `RLIMIT_NOFILE` 上限；设备表改动态/链表；`.gitignore` 收窄 `*.S` 规则（只忽略生成物如 `kallsyms.S`）；同步 AGENTS.md 构建依赖描述；清理源码树构建产物 | 独立 |
| ARCH-11 devfs 子目录化 | `devfs_register_chrdev(name, …)` 只接受 ≤32 字节单段名字，devfs 表 `devices[DEVFS_MAX_DEVICES]`（32 项）拍平；任何 `/dev/<a>/<b>` 形态的层级接口只能退化为 `/dev/<a>-<b>` 或要求重构 VFS lookup 路径。当前 `/dev/null` `/dev/zero` `/dev/random` `/dev/serial` 等是拍平命名；想要 Linux sysfs 风格的 `/dev/pci/<BDF>/<attr>` 必须先解此约束 | devfs 注册接受 `/` 分隔的 path 段；`devices[]` 改为按组件的内部 trie/子表或简单 component table（容量按需，废除 `DEVFS_MAX_DEVICES` 固定表）；VFS `find_mount`/`__vfs_lookup_raw` 支持多段解析（现有单段逻辑需扩展路径分量迭代）；现存拍平命名（`null`/`zero`/`serial` 等）保持兼容，旧 `devfs_register_chrdev("foo", …)` 自动当单段注册；同时引入 `devfs_register_path(const char *path, …)` 多段 API。先单独文档章节并逐步以 hosttest 验证注册/读取/卸载全链 | 独立；P4 NVMe/USB 多级 device 表达、lspci（ARCH-12）等路径化用户态工具的前置 |
| ARCH-12 lspci PCI 设备列举命令 | 用户态无任何 PCI 设备列举入口；QEMU 调试、驱动绑定验证、故障注入证据收集都只能看 kernel `log_info`；与 Linux `lspci`（pciutils，读 `/sys/bus/pci/devices/<BDF>/{vendor,device,class,config,resource,irq}`）的能力对齐 | 新 `user/lspci.c`：`syscall(SYS_pci_count)` 取 N → 循环 `pci_device_get(i)` 拿 BDF → 逐项 `open`+`read` `/dev/pci/<BDF>/<attr>`（依赖 ARCH-11）；属性文件：`vendor`/`device`/`class` 返回十六进制文本，`config` 返回原始 256 字节 config space（供 `-x` hex dump），`resource` 返回 BAR 列表文本，`irq` 返回 pin 编号；嵌入式 `pci.ids` 子集（e1000、virtio-net、AHCI 类名）供名称解析，无数据库时回退 `8086:100e` 形式。`mk/components/user.mk` + `config/rootfs.mk` 加 `lspci` 条目；`hosttests/cases/test_pci_devfs.c` 用 `OS01_HOST_TEST` fixtures 验证每个属性文件的字符串/字节内容；QEMU `phase-0` shell 中 `/bin/lspci` 实跑 | ARCH-9 ✅；ARCH-11；pciutils lspci（实现借鉴） |

### 🖥 P3 GUI

基座（已完成）：fb、fb mmap、terminal 双缓冲 + alt-screen、键盘扫描码、PS/2 鼠标驱动（i8042 共享控制器层 + `/dev/mouse` 8 字节事件 ABI + 500 ms 有界探测，2026-09-27 落地，见 `docs/driver/driver.md`）。

**2026-09-30 闭环**：`libgfx.a` 静态库 + `/dev/gfx0` 受限 present 设备 + Tetris 迁移；terminal 亦已迁移至 libgfx（双缓冲、像素滚动优化与字形绘制加速）；像素缓冲分配已切至匿名 mmap（commit `8ad4d1a8`）。细节见 `docs/gui/gui.md`、`docs/driver/driver.md` gfx0 章节。

**LVGL 当前基线（2026-10-06）**：v9.5.0 已接入 submodule/manifest、profile staging 与 single-writer sysroot，提供 `liblvgl.a`；采用单 UI 线程、4 MiB TLSF 池、软件渲染、XRGB8888。`test_lvgl` 已包含内存像素断言、控件 showcase 与经 libgfx 提交到屏幕的路径（移植合并 `918b6a2e`，showcase `32208a7e`）。**鼠标/键盘控件交互已由 GUI-2 闭环**（独占物理输入租约 + LVGL indev + 光标 + 修饰键 + 重复 + Tab 焦点，2026-10-22）；桌面迁移与多进程窗口系统仍未完成；不把软件像素提交称为 GPU 加速。范围与依据见 [移植 spec](superpowers/specs/2026-10-06-lvgl-port-design.md)、[实施 plan](superpowers/plans/2026-10-06-lvgl-port.md) 和 [libc 评估](assessments/2026-10-06-lvgl-libc.md)。

#### 后续功能任务（按依赖推进）

| 项 | 内容 / 完成条件 | 依赖 |
|----|-----------------|------|
| GUI-1 LVGL 平台后端 ✅ | **已完成（2026-10-19/20）**：从 `test_lvgl` 抽出可复用的 `libgui` 平台层——公共 API `gui_platform.h`（`gui_platform_create/destroy/step/get_pixel`；`gui_platform_step` 输出下一次 poll 等待时间，驱动方据此结合 `poll` 推进）、`libgui/src/platform.c`（显示/离屏后端、tick、flush、生命周期）、`libgui/src/tick.c`；格式、stride、缓冲容量与错误传播已核对并由宿主契约测试钉死。计划 7 个任务的 commit `2fb58e71`（fatal 断言审计）→ `622d6734`（公共/私有头）→ `648a23f7`（平台实现）→ `c27b900a`（真实渲染与生命周期）→ `f5a9f7bf`（目标归档与 sysroot）→ `acebb9df`（`test_lvgl` 迁移）→ `d829b36f`（QEMU 门禁）已全部合入 `master`。设计 [spec](superpowers/specs/2026-10-19-gui-1-lvgl-platform-backend-design.md)、实施 [plan](superpowers/plans/2026-10-19-gui-1-lvgl-platform-backend.md) | LVGL 初步移植 ✅；libgfx ✅ |
| GUI-3 Material 风格桌面 ✅ | **已完成（2026-10-24）**：用 LVGL 重写 `user/desktop`，颜色角色 / 字体层级 / 间距 / 圆角 / elevation 与 hover/pressed/focus/disabled 六类状态全部由**单一令牌表**驱动（令牌表 `libgui/include/gui/gui_theme.h` 公共且 LVGL 无关，主题落地在 libgui 内部）；实现状态栏、启动器、应用抽屉、基本设置与四个**壳内应用**（文件 / 终端 / 系统 / 设置）。应用语义显式：**启动 = 整屏替换 `HOME→APP(id)`、退出 = 销毁应用表面回 `HOME`、全屏切换 = 整屏替换（无窗口化中间态）**；**不绘制任何窗口装饰**（无标题栏边框 / 最小化 / 最大化 / 关闭 / Z-order）。键鼠一律走 GUI-2 输入通道，桌面不再直开设备文件。指针光标已换成 16×24 图像资源（深主体 + 浅描边，任意底色可见）；壁纸底色用新增的 `surface_dim` 角色。验证：宿主契约测试（令牌 / 主题 / 状态机 / 像素）+ 静态审计（禁颜色字面量、禁裸字号、禁手绘 primitive、禁私有头、禁窗口装饰标识）+ QEMU 端到端门禁 `SUITE=gui-desktop`（SMP 1/2）。门禁收尾另有**防孤儿契约**（真实杀 QEMU + 信号路径无锁硬杀 + `PR_SET_PDEATHSIG` 内核兜底）：QEMU 持有镜像写锁，孤儿进程会让后续 `make run` 报「disk.img 被占用」。设计 [spec](superpowers/specs/2026-10-24-gui-3-material-desktop-design.md)，实施 [plan](superpowers/plans/2026-10-24-gui-3-material-desktop.md) | GUI-1 ✅、GUI-2 ✅；现有 desktop 演示 |
| GUI-2b 原生控件与统一焦点引擎 ✅ | **已完成（2026-10-26）**：提供无 LVGL 符号暴露的原生控件体系（`gui_widget.h`、`gui_layout.h`、`gui_event.h`）与统一焦点引擎（`gui_focus.h`），彻底废除桌面临时自建的第二套焦点模型（`desktop_shell_focus_*`）。已发布头包含纯 C 接口，通过 `qemutests/gui_build_audit.py` 严格审计（禁 LVGL 符号泄漏、头依赖图完备）；单平台容量上限 1024 控件与 128 焦点项；控件树销毁自动摘除焦点项（防野指针）；桌面状态栏、抽屉、启动器、设置及壳内应用（Files/Terminal/Sysinfo/Settings）全面重构为原生控件与统一焦点驱动；通过增量构建审计（`test-gui-incremental` 4 阶段全 PASS）、宿主契约测试（`test-gui-host` 全 PASS）、13 项静态审计及 QEMU GUI/Desktop 端到端回归（`SUITE=gui` 24/24 PASS，`SUITE=gui-desktop` SMP=1/2 9 场景全 PASS）。设计 [spec](superpowers/specs/2026-10-25-gui-2b-public-widget-focus-design.md)，实施 [plan](superpowers/plans/2026-10-25-gui-2b-public-widget-focus.md) | GUI-2 ✅、GUI-3 ✅ |
| GUI-4 字体与资源 | 先使用裁剪的抗锯齿位图字体与编译内置图标，覆盖桌面所需字号；单列中文字符集、字库容量与输入方案。按需再引入动态字体或文件资源加载，并审计依赖、许可证及缓存开销 | GUI-1 ✅；GUI-3 的文字/资源需求；文件后端见优化项 |
| GUI-5 Window Server + compositor | 单独设计 surface、IPC、缓冲共享、损伤区域、Z-order、移动/缩放、遮挡、焦点/输入抓取与进程退出回收；可信窗口服务器统一呈现，收紧 `/dev/fb` 与重叠 gfx view 的旁路访问。IPC/共享内存能力先评估，缺项独立补齐；客户端允许使用 LVGL 或其他绘制库 | GUI-1 ✅、GUI-2 ✅；IPC/共享缓冲与图形权限设计；借鉴 opuntiaOS/HackOS |
| GUI-6 terminal 窗口化 | 保留 PTY/VT100 核心，将渲染输出接入窗口 surface；键盘只投递到焦点终端，验证 Ctrl-C、alt-screen、窗口关闭和 shell 存活；缩放时更新 PTY winsize 并接通 SIGWINCH | GUI-5；terminal/PTY ✅；SIGWINCH（P5 剩余项） |

顺序：GUI-1 ✅ → **GUI-2 ✅（2026-10-22 闭环，2026-10-23 合入 `master`）** → **GUI-3 ✅（2026-10-24 闭环）** → **GUI-2b ✅（原生控件与统一焦点引擎，2026-10-26 闭环）** → GUI-4；GUI-2b 已消除桌面自建的第二套焦点模型，GUI-4 可随桌面推进（GUI-3 的界面文案暂用 ASCII，汉字字形归 GUI-4）。GUI-5/6 是独立系统工程，不与首版单进程桌面捆绑；aarch64 GUI 等待用户态与显示设备基础就绪。

#### 优化与验证项

| 项 | 当前边界 / 后续工作 | 验收依据 |
|----|---------------------|----------|
| 显示接管与恢复 ✅ | **已完成（2026-10-11）**：帧缓冲显示所有权协调器（`g_owner`/`g_fallback` 状态机、自动 fallback 回退、libgfx API、原始 mmap 边界与只读 PTE 保护、terminal 接管与故障恢复全部落地闭环，详见 `docs/changelog.md` 2026-10-11 与 `docs/gui/gui.md` 第 4 节） | 不互相覆盖；崩溃/异常退出自动恢复终端；通过 SMP 1/2 E2E 验证 |
| 测试与 showcase 分离（优先） | 固定尺寸离屏 smoke 不依赖真实屏幕；显示 showcase 不以静默离屏回退替代屏幕验收。覆盖分辨率/最大宽度边界、分配与 present 失败；runner 验证无换行 prompt、静默超时、仅输入回显和跨块标记 | 离屏、屏幕、输入三类结果可区分；失败非零退出并保留日志 |
| 脏矩形与批量提交 | 当前 LVGL 局部绘制后，libgfx 的 present 仍提交整个视图。先确保一次逻辑刷新最多一次提交；再设计受边界校验的损伤区域/批量 present ABI，并评估减少额外缓冲复制 | 静态界面无无效提交；小区域变化减少上传字节；保留越界/用户指针防护 |
| 帧耗时与呈现能力 | 分别测量绘制、复制、内核提交和等待开销，覆盖多分辨率及实际阴影/滚动/动画场景；按瓶颈优化。vsync、page flip、write-combining 或 GPU 后端另行设计；当前私有缓冲不保证无撕裂 | 记录帧耗时分布、CPU 占用与上传量；优化前后使用同一场景比较 |
| 内存池与资源生命周期 | 评估 4 MiB TLSF 池是否适合桌面；大型绘制缓冲使用 mmap，预算包含字体、图片、图层与多窗口 surface。测试反复创建/销毁、低内存和池耗尽，明确失败处理 | 内存占用有上限；长期运行无持续增长；耗尽不崩溃或破坏状态 |
| libc / 可选 LVGL 能力 | 按实际需求整理 `lseek/SEEK_*` 的标准头声明并补 `fseek/ftell`；浮点矩阵/矢量功能启用前补所需 libm。pthread 后端等待线程、条件变量、信号量与递归 mutex 完整实现；FreeType/ThorVG/图片解码库逐项审计 | 每次配置扩展重新验证构建、符号和运行时；不让可选依赖阻塞基础桌面 |
| 构建与版本维护 | 持续验证无修改构建、配置/头文件变更重编、staging 孤立文件清除及已发布 sysroot 编译；审计实际 archive 与当前对象清单一致，删除源码时重建归档。保留固定版本，升级单独回归 | 增量结果与干净构建一致；无陈旧归档成员；符号审计与 GUI 回归通过 |

### 🔧 P4 硬件适配

无基座，全部 pending。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| USB 驱动栈 | HID/存储/网络 | 独立 | |
| 真机启动 (USB) | 建立硬件验证路径 | USB 存储 | Tilck |
| NVMe 驱动 | 替代 AHCI | 独立 | |
| HPET clocksource | 真实硬件跨平台时间源 | 独立 | |
| ACPI | 电源管理/关机 | 独立 | |

### 📐 P5 ABI 扩展/兼容性

已完成（详见 `docs/changelog.md`）：symlink/readlink、exec symlink ABI、syscall 边界审计、AAGU-4 跨边界符号全套、AAGU-5 entropy facade 全套、arch source groups、AAGU-29 libk.a link、arch entropy strong overrides（RDSEED/RNDRRS = STRONG）。

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；`socket ✅ → AF_UNIX`；`mbedTLS ✅ → HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc | ELF ✅ | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构 | |
| clone/pthread | 线程模型 + pthread_create | futex ✅ | |
| readv/writev | scatter-gather I/O | 独立 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立 | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立 | |
| 作业控制（部分完成）| setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅；**剩余**：SIGWINCH 派发、SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs） | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch | libc | |
| libc 完整性 | printf `%f/%F/%e/%E/%g/%G` + `%ld/%lu` + `%x/%o`、strtod、getopt、getcwd ✅；**仍缺**：stdio 高级流接口（getline/fseeko）、cut/paste 的 getopt 解析 | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

### Parked（未闭环 follow-on，随时可拾起）

此前挂起的 follow-ons（sysroot 头文件级增量重编、idle 正名、CFLAGS 编译参数缓存失效、compat/ 目录裁撤、x86 驱动目录重定位、softirq ifdef 清除、LWIP_RAND 种子等）已全部闭环并归档至 `docs/changelog.md`。`fd_refcount` 跨核 fixture 的迁移/压力循环时序问题于 2026-10-28 修复并归档，根因及正反例证据见 [修复记录](debug/2026-10-28-fd-refcount.md)。

**2026-10-24 清理**：本段原先列着若干已闭环项，现已移出并归档至 `docs/changelog.md`——aarch64 并发 replace phantom `-ENOENT`（`80eed2e9`）、hosttests `__test_stats` 复位 bug（`2d594ce0` + CI 接线 `339ffe26`）、内核用户态异常退出状态未编码为信号终止（`3d56e1d0`）、`test_display_owner` 的 libgfx 句柄 fd 耦合（评审决定保持现状，备案归档）、monitor 计划遗留 ⑤ `monitor_term_enter` 的 `fflush`（`9042bd20`）与 ⑭ monitor 帧被诊断输出撕裂（`c6e1c340`）。原编号保留在归档条目与下方 monitor 子项里，便于交叉引用。

**2026-10-31 局部闭环**：dispatch 第一期 host 补强中的 `test_brk_pages.c` 源码扫描缺锚点保护已完成；缺文件/缺锚点现在明确 FAIL 并完成协议，回归覆盖与验证记录见 [`docs/changelog.md` 2026-10-31 条目](changelog.md#2026-10-31)。其余子项继续挂起。

当前挂起：

- **中断/异常 dispatch 第一期遗留（2026-10-24；主体与最终修复已评审，原 m3-probe 验收失败已闭环）**：
  - **host 测试补强（非阻断）**：decoder 表行名用于失败诊断；增加 x86 CPL0 + U/S=1 仍判 KERNEL、ARM OTHER EC 的无效 FAR 断言；ARM backend 有效 FAR 用例补齐地址/有效位；dispatcher 的 x86 host 现场假设与 `memcmp(pt_regs_t)` 布局假设显式化，注明 longjmp 用例无法观察跳转后的清理；x86 backend 删除 reset 后才断言零计数的空检查；ARM backend RED 证据可补有意破坏断言的 mutation。
  - **调用链审计 fixture 补强（非阻断）**：`qemutests/test_intr_dispatch_audit.py` 的 movabs/leaq 无法解析目标用例应分别钉住对应诊断，避免只匹配错误目标也会出现的函数名；梳理 leaq 专属错误分支不可达、重复边诊断，修正 movabs docstring 中 decoder call 与 dispatcher call 地址的混淆，并整理方法间空行。
  - **维护性收尾（非阻断）**：x86 四个向量常量在 decoder/adapter/backend 三处重复；sync/IRQ backend 的 canary 属性不对称可说明；ARM 私有头统一 `pt_regs_t *` 写法；`test_uaccess.c` 分配注释应与实际 order-1（8 KiB）一致。`hosttests/Makefile` 的 “EVERY production header” 注释应限定为内核头与 ARM shim：libc/framework 的既有手写依赖缺口尚未覆盖，后续单独治理。
  - **已裁定保留，非待修缺陷**：ARM 非 EL1h frame 仍保留真实入口 slot `0x200`、由 origin 表达不匹配；decoder 先编入镜像再接线是已结束的中间阶段。bad-event 声明属性、intr 内核头依赖、GIC 旧注释和验证报告归档表述均已在 `6f479bd6` 修复，不再列为待办。其余异常/IRQ/EL0 等后续迁移范围见上方「中断/异常 dispatch 统一」行。

- **QEMU 分辨率切换器收尾 follow-up（2026-10-06，`866fd801` FF 合并入 master；2026-10-20 parked #5 #1–#7 闭环于 `cd65b86`）**：主线交付后未闭环的 cosmetic/健壮性项，均不阻塞。**仍未闭环（parked #5 #8–#21）**：`HOLD ok`/`RELEASE ok` 匹配无 `start=` 会命中陈旧行（已用 start= 收紧但底层 r 仍可能误命中『nledo panic』或类似串，待审 SPEC §8.2 的副词表）；`_client_eagain_case` hold 8s 余量，慢机仍可能 flake（建议 12s）；客户端策略：`user/desktop.c`/`tetris.c`/`test_lvgl.c` 的重试/退出块三处近似重复（仅 classifier 抽到 `gfx_client_policy`）。`user/terminal.c:301-302` present 门限混淆「限频」与「view 已失效」（瞬时 EAGAIN/get_state 失败会压制 250ms 有效 present），fatal 计数排除 `EBUSY` 而 present 只为 EAGAIN/ESTALE 起 retry（errno 集合不一致）。`fb_test`（仅测试构建）：token 存而不用作判别子。其他：`kernel/driver/fb.c` 的 `fb_write_row` 生产已死（仅 host mock 用）；`test_fb_uapi.c` 未用 `CHECK_TRUE`；T6 pty 遗留（`pty.h` 匿名 pgrp 联合体、`TIOCSWINSZ` 用 `pgrp>0` vs spec `pgrp!=0`）。**环境阻塞（非代码 bug，供复现参考）**：`make OS01_SYSTEST=1 test-syscall` 恒 339/340，唯一失败 `sendto user boundaries (UDP socket setup)`——systest 的 QEMU 未注册 NIC，加 NIC 后 341/341；本机 OVMF 无 1280×960 GOP 模式，off-table 初态保证改在 1600×1200 验证（配置字节仍按 `resolution 1280x960\n` 写入并断言）。

- **GUI QEMU suite 中 guest 侧 `test_lvgl` 的用户态 SIGSEGV（2026-10-22，GUI-2 第二轮修复期间观察到一次）**：第二轮修复期间在最终树上连续跑了 4 次 `SUITE=gui`，其中第 3 次的 SMP 1 `smoke` 用例报 `/bin/test_lvgl smoke alias failed (ok=True, rc=139)`，guest 侧打印 `Killing task 5 (user fault at RIP=000000000049884D, signal=11)`；`0x49884D` 解析到 `build/x86_64-clang/user/test_lvgl.elf` 的 `memset`，且崩溃发生在 `[test_lvgl] Rendering and pixel assertions passed.` **之后**。该现象**不是**这两轮修复引入的：第二轮在 `libgui` 内的唯一改动是 `gui_input_lvgl_repeat_after_down` 里的一次比较，不可能损坏 `memset`；对 `kernel/driver/fb_state.c` 与 `libgfx/gfx.h` 的改动是**纯注释**。它在第 4、5 次运行与最终干净门禁中**均未复现**。因此**未修**、**未**打包进任何修复，也**未**声称已修。后续需在能复现时抓 guest 侧寄存器/栈再定位。证据见 [GUI-2 验证报告](superpowers/reports/2026-10-22-gui-2-validation.md) §7.5。

- **测试配置 QEMU 中 ash 用户态 SIGSEGV（2026-10-24）**：验证 terminal ARM 时，SMP 1 的重复 ARM 已打印 EBUSY，随后 ash（task 3）在 `RIP=0x40DE24`、signal 11 终止，terminal 被 init respawn。该轮失败；后续放慢命令发送，SMP 1/2 各 13 项协议验证通过。尚未定位或修复，不能确认与本次变更或 GUI `test_lvgl` SIGSEGV 是否有关。失败日志与验证边界见 [terminal ARM 调查](debug/2026-10-24-fbtest-terminal-arm-validation.md)。

- **monitor 计划遗留 parked 项（2026-10-11，monitor plan 9 任务合并后 triage）**：全程评审约 50 条缓办 Minor 中筛出以下值得跟踪的；其余为注释措辞/不可达边界/host fixture 风格问题（最终评审逐条判定 ship-ok，含一条前提纠正：`PROCFS_STAT_CAP=1024` 的 8 核 worst case 实为 ~869B、余量 ~155B，非 ~2B）。本批全部不阻塞，随相关计划拾起（编号沿用评审时的原始编号，已闭环项移出本段、归档至 `docs/changelog.md`）：
  - **正确性防护（当前不可达，但失败方向错误）**：② `file_free` 仅在 `f->node` 非空时调 `vfs_file_release`，`fs_private` 有值而无 node 的理论状态会泄漏快照（open 契约保证不可达）；③ `monitor_model_init`/`monitor_sample_destroy` 先 free 后清零，调用栈垃圾结构体 = 任意指针 free（头文件已文档化"须先清零或复用已初始化结构"，现有调用方均 `memset` 前置；建议改 zero-first 或改名标注前置条件）。
  - **验证强度缺口**：⑥ monitor QEMU 双核绑核验证弱：spin 归属采样粒度实为 ~0.2s（`test_monitor.c` 注释称 ~50ms），亚 0.2s 迁移漏检可被 epoch 探测错过；双核重试最坏 ~53s/70s 预算余量薄；严格绑核覆盖仍缺（内核无 sched_setaffinity，需按 monitor plan §Self-Review 在 thermal Task2 后补受限内核 spin 自测，不能以观察到的稳定归属替代）。
  - **语义/契约选择备案（有意为之，记录在案）**：⑪ `monitor_collect` 在 `opendir("/proc")` 失败时返回 0 + 空进程列表，与"无进程"不可区分（无 list-valid 位）；⑫ thermal `stale`（>3s 未更新）标记与进程 `stale` 字段 v1 恒不触发——渲染端已就绪，等独立 thermal 计划的 BSP 周期采集 worker 激活；"QEMU 温度不得报数值"的专项泄漏断言同样推迟（当前管线 State=unsupported 时无任何数值路径，collect 只在 State=ok 填 temp）。

---

## 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档索引（按 0-4 层组织，按阅读顺序排列） |
| `docs/architecture.md` | 启动链、内存布局、初始化序列（全局骨架图） |
| `docs/boot/boot.md` | UEFI 引导 + `boot_context` v2 ABI + v25 E820 拆分 |
| `docs/arch.md` | arch-neutral facade + per-arch 强覆盖模式（weak default / strong override）+ v25 arch-cleanup 系列 7 子系统矩阵 |
| `docs/arch/cross-boundary-symbols.md` | **跨边界符号/ABI 边界规范（AAGU-4）** —— builtin、UAPI、arch-value、libc 镜像 4 类规则 + 现状对照表 + 后续 issue 切分 |
| `docs/arch/entropy-source-facade.md` | **arch-neutral entropy facade spec（AAGU-5.3 / .5）** —— STRONG/WEAK/NONE 三档质量标签 + 控制流决策 + 禁止条款 |
| `docs/memory/memory.md` + `docs/memory/cow-mmap.md` | 物理/虚拟内存管理 + COW fork/mmap + v25 PMM arch-neutral + 页表层级统一 |
| `docs/interrupt/interrupt.md` | 中断处理（do_IRQ、register_irq、IDT） |
| `docs/smp/smp.md` | SMP 架构 + 负载均衡实施总结 |
| `docs/sched/scheduler.md` + `docs/sched/scheduler-complexity.md` | EEVDF 调度器设计与复杂度评估 |
| `docs/syscall/syscall.md` | 71+ syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 |
| `docs/signal/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/time/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结 |
| `docs/gui/gui.md` | Tetris 游戏实施总结 + P3 GUI 路线图 |
| `docs/fs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/net/network.md` + `docs/archive/lwip-debugging-experience.md` | lwIP 网络栈 + 正确性加固 |
| `docs/fs/filesystem.md` | VFS, FAT32, ext2, devfs, procfs, tmpfs, GPT, block device |
| `docs/driver/driver.md` | 驱动子系统（keyboard, serial, ahci, pci, e1000, virtio-net, fb, mouse, gfx0） |
| `docs/subsys/subsys.md` | 子系统注册框架（`SUBSYS_INITCALL()` + phase 顺序） |
| `docs/log/log.md` | 日志级别、DEBUG_CHANNELS、LOG_TARGET、NDEBUG |
| `docs/build/build.md` + `docs/build/toolchain.md` | 构建系统手册（三篇合一：profile 构建体系、端到端运行调试、构建/测试 harness 权威 taxonomy 与 6 bucket 目标）+ 工具链指南 |
| `docs/debug/debug.md` | 调试通道 |
| `docs/archive/applet-verification.md` | busybox applet 验证清单 |
| `docs/decisions.md` | 关键设计决策总账（含开源 OS 借鉴对照表） |
| `docs/structure.md` | 目录结构逐目录说明 |
| `docs/changelog.md` | **历史完成记录**（按时间倒序） |
| `docs/superpowers/specs/` + `docs/superpowers/plans/` | 历史设计 spec + 实施 plan（过程档案） |
| `docs/archive/aarch64/` | aarch64 专题 closure 与 handoff 报告（Generic Timer Phase 1/2、libk.a、IPI fix 等归档） |
