# OS01 优化路线图

> **基准**: `dd05877b`（2026-10-04）
> **日期**: 2026-10-04

roadmap 只列**未完成 / 进行中**的规划项；所有已完成工作见 `docs/changelog.md`。

## 当前状态（一句话）

- **Phase 1-9**：全部就绪（COW/mmap、调度/信号/SMP、文件系统、设备驱动、用户态、poll/select、网络、时间系统）
- **工程与架构治理**：AAGU 全套就绪；2026-10-04 规划 ARCH-1..10 架构治理任务（syscall 解耦、Linux ABI 兼容层独立、Muldefs 消除、调度器/VFS/驱动抽象治理等）
- **aarch64 适配**：Generic Timer 全栈 + GICv2 Phase 1 + IPI fix + EL1 sync 致命诊断 + M0 启动直映 + M1 运行期直映已闭环；M2 Slab + M3 内核 VMM 设计就绪（spec v8 / plan v4），M4 用户态 VMA 与统一 kernel_main 待接入
- **GUI**：PS/2 鼠标驱动 + 2D 图形 API（`libgfx.a` + `/dev/gfx0`）+ Tetris 移植 + terminal 双缓冲与字形加速迁移已闭环
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

已完成：v25 arch-cleanup / PMM arch-neutral / 页表原语 / Generic Timer Phase 1 + Phase 2 #1~#5 / GICv2 Phase 1 + GIC probe fix / AAGU-3 subsys_stub convergence / AAGU-29 libk.a link / IPI TPIDR_EL1 fix / EL1 sync 致命诊断 / M0 启动直映契约 / M1 运行期直映严式页表树（详见 `docs/changelog.md` + `docs/archive/aarch64/` 各专题报告）。

#### 内存管理：共同启动直映接口（2026-10-02）

两种架构现在都在 PMM 初始化后调用 `arch_boot_direct_map_init()`，通过 `arch/boot_memory.h` 查询 readiness 与不可变、合并的 RAM coverage。x86_64 沿用 2 MiB `vmm_init()`，保留 `ZONE_UNMAPPED_INDEX` 非零时的覆盖截止语义，并传播中间页表分配失败。aarch64 的 M0 在 `head.S` 中提供普通/自测一致的 0..2 GiB 启动映射；M1 从低窗口 arena 建立独立 TTBR1，只映射真实 RAM、启动保留 block 和设备窗口，再启动 AP。两者的页表编码、偏移与 TLBI 留在架构实现内。

以下是**实施顺序**，每项单独设计与验收；启动页表修复只解决当前直映缺口，不等于完成运行期 VMM。RAM 范围以 UEFI 归一化结果为准，固定映射到 `0x80000000` 不能代替任意内存容量及稀疏范围的处理。

| 阶段 | 任务与完成条件 | 前置 |
|------|----------------|------|
| M0 启动映射契约 ✅ | `head.S` 在 MMU 打开前建立 0..2 GiB boot map，内核 block 保持 EL1 可执行，其余 RAM PXN/UXN；普通/自测一致，已移除 C 补图差异。 | 现有 boot 页表、PMM ✅ |
| M1 运行期直映 ✅ | 共同 `arch_boot_direct_map_*` 接口；aarch64 arena 建立独立 TTBR1，收紧非 RAM 映射并覆盖高 RAM/holes；BSP/AP root 与 probe 验证，16 组矩阵和稀疏/耗尽/坏 root 注入通过。 | M0；RAM 归一化、PMM ✅ |
| M2 Slab 实装与初始化顺序 | aarch64 目前编译 `kernel/arch/aarch64/runtime/slab_stub.c`：`slab_init()` 无操作，`kmalloc()` 返回 `NULL`。移除占位实现并移植/共用真实 Slab；处理 `slab.c` 中 x86 专属 `pushfq`/`cli`/`sti` 锁路径。`pmm_init()` 当前在末尾调用 `slab_init()`，因此先明确早期映射是否足以覆盖 Slab 元数据与预留页；若需等 M1，则拆分初始化顺序为 PMM 元数据 → 运行期直映 → Slab，同时保持 x86_64 的预留语义。验证跨缓存大小的分配/释放和 QEMU 启动。M2+M3 详细设计 spec (v8) 与 plan (v4) 已完成。 | M1；PMM ✅ |
| M3 内核 VMM 接口 | 以 `arch/aarch64/memory/page_table.c` 的 4 KiB 原语为基础，补运行期内核映射/解除映射、2 MiB block 与 4 KiB table 共存及必要的拆分、权限/属性、页表页生命周期和 SMP TLB 失效；给通用调用方提供架构中立接口。现有 `memory/vmm.c` 使用 x86 页表 flag 和 `kernel_map=Phy_To_Virt(0x101000)`，不能直接列入 aarch64 源清单。为页表原语补 host 边界测试，并用 QEMU 验证真实映射。M2+M3 详细设计 spec (v8) 与 plan (v4) 已完成。 | M1、M2；4 KiB 页表原语 ✅ |
| M4 用户地址空间与 VMA | 建立 aarch64 用户页表根、EL0 权限和地址空间切换/回收，再使 VMA/mmap/ELF、缺页分配、COW 与 `munmap` 使用 M3 接口；把 `arch_user_range_accessible()` 的 aarch64 fail-closed 实现替换为真实跨页权限检查，并接通 uaccess 故障恢复。现有 `memory/vma.c` 和 x86_64 `do_page_fault()` 直接使用 x86 PTE flag，需先剥离架构语义；EL1 sync 目前只有致命诊断，EL0 sync 入口仍未接入。以隔离、权限、COW、回收和用户态 QEMU 用例验收。 | M3；Slab、调度/上下文切换、EL0 异常路径 |

**当前已具备的部分**：aarch64 的 RAM 归一化及共用 `PMMngr`/`alloc_pages()`/`alloc_4k_page()`；BSP 启动期 4 KiB map/query/unmap smoke。`aarch64_pt_range_accessible()` 已存在于页表原语，但 `arch/mmu.h::arch_user_range_accessible()` 仍返回 `false`，不能视为 uaccess 已接通。M0、M1 已独立验收；M2–M4 尚未完成（M2+M3 设计 spec v8 / plan v4 已就绪）。M1 的 16 组 RAM/CPU/镜像矩阵与稀疏、容量耗尽、AP 无效 root 注入记录见 `docs/memory/memory.md`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 统一 kernel_main | 单 `kernel_main` 按固定顺序调 `arch_early_init()` / `arch_late_init()` / `scheduler_init()`；aarch64 必须先 dtb 才能用 DTB 信息；内存阶段须满足 M0→PMM→M1→M2/M3 的资源可用顺序。**Spec A 中断 dispatch / Spec B SMP+timer / Spec C kernel_main 单一入口** | M1–M3 的顺序契约；SMP ✅, arch_irq ✅, GIC ✅, Timer ✅, UEFI 链 ✅ | 长项 spec |
| 中断/异常 dispatch 统一 | `arch_intr_dispatch(vector, pt_regs*)` 单入口抽象；x86_64 IDT vs aarch64 VBAR_EL1 vector tables 各自封装；x86_64 IST stack vs aarch64 SP_EL1 切换。aarch64 EL1h IRQ 已接 GIC，EL1h sync 已有致命诊断；EL0 sync/IRQ、可恢复缺页和通用 dispatch 仍未接入。先明确 M4 所需的 fault/return 契约，再剥离 `x86_64/trap.c` 内的架构寄存器解码 | arch_irq ✅, head.S ✅, GIC ✅；M4 用户态路径 | Linux do_IRQ |
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
| ARCH-1 syscall 层脱离 arch | 74 个 syscall 以 ~2000 行 `switch` 写在 `kernel/arch/x86_64/intr/trap.c::do_system_call`，`SYS_open`/`SYS_chdir`/`SYS_stat` 等直接内联 VFS/路径逻辑；aarch64 `trap.c` 无 syscall 分发，接入用户态只能复制 | 新建 arch-neutral `kernel/syscall/`（+ `kernel/include/syscall/`）：`sys_call_table[]` 函数指针分发，按子系统拆 `sys_fs.c`/`sys_proc.c`/`sys_mm.c`…；arch 层只负责寄存器 ↔ 参数/返回值。x86_64 systest 全量回归 | 无；aarch64 M4 / 中断 dispatch 统一的前置 |
| ARCH-2 Linux ABI 兼容层独立 | 自有 ABI 与 Linux 翻译表混在 `do_system_call` 热路径；`static const int8_t linux_to_os01[320]` 只能表示 ≤127 的 OS01 号，超出后**静默溢出**；新增 syscall 需同改编号、翻译表、`switch` 三处 | **保持 OS01 ABI 为标准**；Linux 翻译抽到独立兼容模块（如 `kernel/syscall/compat_linux.c`），表项类型改 `int16_t`/显式 `SYS_xxx` 枚举，`_Static_assert(SYS_MAX < 表项上限)`；兼容层负责参数/结构体语义差异（stat、sigaction 等），核心 syscall 只见 OS01 语义；未映射号统一 `-ENOSYS` | ARCH-1 |
| ARCH-3 头文件定义全局 + `-z muldefs` | `sched/task.h` 直接定义 `init_task_union`/`init_task[]`/`init_mm`/`init_thread`/`init_tss[]`，被 37 个 TU 包含，每个 `.o` 都有强符号；靠 `kernel/arch/x86_64/make.config` 的 `-z muldefs` 链接通过，会吞掉所有真实重复定义；x86 `struct tss_struct` 与硬编码 IST 地址位于通用调度器头 | 头文件只留 `extern`；定义迁到 `sched/task.c` 与 `arch/x86_64/`；TSS 移到 `kernel/include/arch/x86_64/`；移除 `-z muldefs` 并清理由此暴露的重复符号；`test-static` 加“无 muldefs / 头文件无对象定义”审计 | 无（正确性优先） |
| ARCH-4 拆分 `sched/task.c` | 2764 行混合 EEVDF 调度/负载均衡、fork(COW)、exec + 用户栈/auxv、信号/进程组、kthread，以及 `sys_symlink`/`sys_readlink`/`sys_lstat`/`sys_fstatat` 等 FS syscall | 拆为 `sched/core.c`、`sched/fair.c`（EEVDF）、`sched/balance.c`、`kernel/fork.c`、`kernel/exec.c`、`kernel/signal.c`；FS syscall 迁 `fs/` 或 ARCH-1 的 `sys_fs.c`；纯搬迁不改语义，systest + selftest 回归 | ARCH-1（FS syscall 去向） |
| ARCH-5 VFS 抽象补强 | `vfs_ops` 无 `lookup`/`getattr`/`permission`，路径解析靠 `readdir` 线性扫描（O(n)、无 dentry cache）；`vfs_node` 缺 mode/uid/gid/时间戳/nlink；挂载按路径字符串前缀匹配；无 FS 类型注册表（`fat_vfs_ops`/`ext2_vfs_ops` 全局 extern） | 引入 `file_system_type` 注册 + mount by type；`inode_operations`/`file_operations` 分离并新增 `lookup`/`getattr`；`vfs_node` 补元数据；挂载点挂在 node 上；简单 dentry cache。分阶段 spec，每阶段 FS 回归 | 独立；完成后利好 P5 权限/FIFO/openat |
| ARCH-6 FS 并发与块缓存 | `fat.c`、`tmpfs.c` 0 处加锁（`ext2.c` 19 处），默认 `-smp 2` 下 FAT 表/簇分配/tmpfs 目录存在数据竞争；全内核无 buffer/page cache，每次读直达 AHCI | 短期：每挂载点锁兜底 FAT/tmpfs；中期：blockdev 层按 (dev, blkno) 哈希的块缓存，ext2/FAT 共用，含写回与一致性；SMP 并发读写压力用例 | 独立（正确性优先）；块缓存与 ARCH-9 块层解耦协同 |
| ARCH-7 移除 `#define mmap uint64_t*` | `memory/vmm.h:80`、`fs/vfs.h:77`、`fs/devfs.h:46` 三处定义类型宏，`vfs_ops` 用 `#undef`/恢复绕行；任何名为 `mmap` 的标识符都会被替换 | 改 `typedef uint64_t *pgd_t;`（或等价名）全仓替换，删除 save/restore 绕行 | 无；宜在 aarch64 M3 VMM 接口前完成 |
| ARCH-8 子系统框架补完 | phase 用魔数 3–6，Phase 1–2 / 7–9 仍硬编码于 `kernel_main`；同 phase 内顺序依赖链接顺序（需 link-order 审计兜底）；必需子系统失败只打印 `FAIL` 继续启动；固定表 `MAX_SUBSYS 64`/`MAX_SUBSYS_PERCPU 16`；框架自身用 `serial_printk` 违背日志规范 | 命名 phase 枚举并覆盖全部启动阶段；支持显式依赖（或 phase 内 order 字段）取代链接顺序；非 OPTIONAL 失败 `panic`；表改链接段驱动（容量随注册数）；日志改 `log_*` | 与“统一 kernel_main”协同 |
| ARCH-9 驱动模型 / 总线抽象 | 驱动自行 `pci_find_device(class, subclass…)`，无 `pci_driver` + id_table / probe-remove；`net/net.c` 用全局 `is_virtio` 在 e1000/virtio-net 间 `if` 分支，无 `net_device` 抽象；通用 `block/blockdev.c` 内含 `default_ahci_read/write` | 引入 `pci_driver`（id_table 匹配 + probe）；`net_device` ops 抽象，多网卡可共存；块层去 AHCI 耦合，驱动注册自己的 `block_device_ops` | 独立；P4 NVMe/USB 的前置 |
| ARCH-10 静态容量与工程卫生 | `NR_CPUS 8` 三处重复定义；`MAX_GSI 24` 仅够单 IOAPIC，与 `MAX_IOAPICS 4` 矛盾；`POLL_MAX_FDS 16` 栈数组硬拒；`BLOCKDEV_MAX 8`、`DEVFS_MAX_DEVICES 32` 固定表。`.gitignore` 忽略全部 `*.S`，新增汇编默认不入库，例外路径 `kernel/arch/x86_64/thread_entry.S` 已过时（实为 `cpu/thread_entry.S`）；AGENTS.md 仍称“Makefile 无头文件依赖”但 `kernel/Makefile` 已 `-MD -MP`；源码树残留 `kernel/fs/select.o`、`kernel/kernel.bin` | `NR_CPUS` 单点定义；GSI 上限按 IOAPIC 枚举动态计算；poll 改堆分配 + `RLIMIT_NOFILE` 上限；设备表改动态/链表；`.gitignore` 收窄 `*.S` 规则（只忽略生成物如 `kallsyms.S`）；同步 AGENTS.md 构建依赖描述；清理源码树构建产物 | 独立 |

### 🖥 P3 GUI

基座（已完成）：fb、fb mmap、terminal 双缓冲 + alt-screen、键盘扫描码、PS/2 鼠标驱动（i8042 共享控制器层 + `/dev/mouse` 8 字节事件 ABI + 500 ms 有界探测，2026-09-27 落地，见 `docs/driver/driver.md`）。

**2026-09-30 闭环**：`libgfx.a` 静态库 + `/dev/gfx0` 受限 present 设备 + Tetris 迁移；terminal 亦已迁移至 libgfx（双缓冲、像素滚动优化与字形绘制加速）；像素缓冲分配已切至匿名 mmap（commit `8ad4d1a8`）。细节见 `docs/gui/gui.md`、`docs/driver/driver.md` gfx0 章节。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API ✅ | HackOS |
| Window Server + compositor | 多窗口管理 + 合成 | 字体/2D/鼠标 | opuntiaOS + HackOS |

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

目前无挂起的 Parked 项。此前挂起的 follow-ons（sysroot 头文件级增量重编、idle 正名、CFLAGS 编译参数缓存失效、compat/ 目录裁撤、x86 驱动目录重定位、softirq ifdef 清除、LWIP_RAND 种子等）已全部闭环并归档至 `docs/changelog.md`。

> **独立缺陷备忘**：aarch64 contract `targets` mode 依赖 x86 build 目录存在（`make -n PROFILE=x86_64-clang kernel.bin` 的 `+` 前缀 artifact recipe 在 `-n` 下也会执行，clean workspace 下跑 aarch64 contract 会触发）。

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
