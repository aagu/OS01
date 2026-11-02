# OS01 优化路线图

> **基准**: `e8412d1e`（2026-10-31）；aarch64 EL1 调度器、boot logo、ARCH-5 VFS、GUI-2b 原生控件与统一焦点、PID 1 延迟启动 lwIP 闭环
> **日期**: 2026-10-31

roadmap 只列**未完成 / 进行中**的规划项；所有已完成工作见 `docs/changelog.md`。

## 当前状态（一句话）

- **Phase 1-9**：全部就绪（COW/mmap、调度/信号/SMP、文件系统、设备驱动、用户态、poll/select、网络、时间系统，PID 1 init 优雅关机闭环）
- **工程与架构治理**：AAGU 全套就绪；ARCH-1（syscall 脱离 arch）、ARCH-2（Linux ABI 兼容层独立）、ARCH-3（消除 muldefs 全局符号）、ARCH-4（拆分 sched/task.c）、**ARCH-5（VFS 抽象补强：fs 注册表/ops 分离/元数据/活跃节点回收/挂载树/dcache，2026-10-24 闭环）**、ARCH-7（移除 `#define mmap`）、ARCH-9（驱动模型/PCI/总线解耦）、**ARCH-10（静态容量与工程卫生，2026-11-02 闭环）** 均已闭环合入 master；剩余 ARCH-6、ARCH-8、ARCH-11、ARCH-12 推进中
- **aarch64 适配**：Generic Timer 全栈 + GICv2 Phase 1 + IPI fix + EL1 sync 致命诊断 + M0 启动直映 + M1 运行期直映 + M2 Slab 接入 + M3 内核 VMM + **EL1 BSP 协作式内核线程调度（P1–P5 全通过，2026-10-31 闭环合入 master）** + **AP 协作式调度 P1（轮询协作式：accepted AP 经单向 task-stack handoff 进入公共 EEVDF 调度器，显式 pinned kthread 全生命周期跨核覆盖，无 SGI/无抢占，2026-11-01 完成验收）** + **AP 调度 P2（SGI4 显式远端唤醒 + WFI idle，2026-11-01 验收）** + **引导字标（boot logo 经 `fb_write` facade 绘制与端到端验证）** 已闭环；中断/异常分派统一（arch-neutral dispatch + 6 条入口 + x86 20 异常 28 IRQ 第二期）已闭环；M4 用户态 VMA 与统一 kernel_main 尚未启动
- **GUI**：PS/2 鼠标、`libgfx.a` + `/dev/gfx0`、Tetris 与 terminal 迁移、QEMU 分辨率切换与显示所有权协调器、LVGL v9.5.0 初步移植与图形 showcase、**GUI-1 LVGL 平台后端（libgui）**、**GUI-2 输入与焦点**、**GUI-3 Material 风格桌面**、**GUI-2b 原生控件体系与统一焦点引擎（彻底废除桌面临时自建焦点，`gui_widget.h` / `gui_layout.h` / `gui_event.h` / `gui_focus.h`，2026-10-26 闭环）** 均已合入 master；后续推进 GUI-4 字体资源与 GUI-5/6 窗口服务器
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

已完成前置项（详见 `docs/changelog.md` + 各专题报告）：
- **基础与外设**：v25 arch-cleanup / PMM arch-neutral / 页表原语 / Generic Timer Phase 1 + Phase 2 #1~#5 / GICv2 Phase 1 + GIC probe fix / AAGU-3 subsys_stub convergence / AAGU-29 libk.a link / IPI TPIDR_EL1 fix / EL1 sync 致命诊断 / **引导字标（boot logo 经 `fb_write` facade 绘制）**。
- **内存管理**：**M0 启动映射契约**（`head.S` 0..2 GiB boot map）/ **M1 运行期直映**（`arch_boot_direct_map_*` 接口 + 独立 TTBR1）/ **M2 Slab 接入**（编译生产 `memory/slab.c`，2026-10-05 闭环）/ **M3 内核 VMM**（语义层拆分 + aarch64 backend + SMP shootdown + M3.1 audit gate，2026-10-05 闭环）。
- **执行体与调度**：**EL1 BSP 协作式内核线程调度（P1–P5 全通过）**（2026-10-31 闭环，merge `fa7d51ac`）：架构上下文门面（`arch/context.h` + per-arch `thread_t` 布局与偏移断言）、公共调度器拆分（`sched/boot.c`/`sched/kthread.c`/`sched/internal.h`）、aarch64 EL1 切换机器码（`switch.S`/`thread_entry.S`/`task_arch.c` 预切栈校验）与 BSP 栈交接、协作 `kthread_yield()`/blocker/`kthread_exit()` 自回收、`MODE=sched` 严格证据 runner 与 4 个编译期故障注入变体。
- **aarch64 AP 协作式调度 P1 — 轮询协作式（2026-11-01 验收）**：`<arch/sched.h>` 门面 + per-arch `cpu/sched_notify.c` 后端（默认 mask/通知/scan 策略）、并发安全资源 observer（token/批次/失败旗标）、`arch/aarch64/cpu/ap_sched.c` 单向 task-stack handoff（prepare/release/released/enter/idle_handoff/stack_enter + 冻结 `smp_accepted_mask` admission）、fatal 无栈单 owner 仲裁（`AARCH64_AP_SCHED_TEST=fatal-owner` 独立 2 核故障 boot）、生产路径 7 case 验收（`MODE=sched-ap` 1/2/4/8 核矩阵 + `aarch64_ap_scheduler.py` 严格 runner）。P1 验收时 idle 全程忙等轮询，后续 P2 已替换此 idle 路径。
- **aarch64 AP 协作式调度 P2 — SGI + WFI（2026-11-01 验收）**：SGI4 远端通知、显式 blocker wake 与生命周期保护、IRQ 屏蔽的最终工作检查和 `dsb sy; wfi; ret` 等待窗口；`MODE=sched-wake` 继承 P1 七个 case 并新增六个 wake case，1/2/4/8 核各两次运行。两轮独立评审修复后明确 APPROVE；[验证报告](superpowers/reports/2026-10-31-aarch64-ap-sched-p2-validation.md) / [评审记录](superpowers/reports/2026-11-01-aarch64-ap-sched-p2-final-review.md)。**仍为 EL1 协作式：AP timer、IRQ 返回抢占、迁移与 M4 用户态不在本期范围。**
- **中断/异常分派统一**：**第一期（2026-10-24）** 落地架构中立无状态分派器 `arch_intr_dispatch()` 与 6 条统一入口（aarch64 EL1h sync/IRQ 与 x86 #DE/#UD/#GP/#PF）；**第二期（2026-10-25）** 接入 x86 当前安装的 20 条异常与 28 条 IRQ。

#### 内存管理与执行体后续任务

| 阶段 | 任务与完成条件 | 前置 |
|------|----------------|------|
| M4 用户地址空间与 VMA | 建立 aarch64 用户页表根、EL0 权限和地址空间切换/回收，再使 VMA/mmap/ELF、缺页分配、COW 与 `munmap` 使用 M3 接口；把 `arch_user_range_accessible()` 的 aarch64 fail-closed 实现替换为真实跨页权限检查，并接通 uaccess 故障恢复。现有 `memory/vma.c` 和 x86_64 `do_page_fault()` 直接使用 x86 PTE flag，需先剥离架构语义；EL1 sync 目前只有致命诊断，EL0 sync 入口仍未接入。以隔离、权限、COW、回收和用户态 QEMU 用例验收。**当前状态：尚未开始（仓库内无对应实现代码）** | M3 ✅；Slab ✅、EL1 调度/上下文门面 ✅、EL0 异常路径 |

#### 架构统一推进链（迈向单一 kernel_main）

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 统一 kernel_main | 单 `kernel_main` 按固定顺序调 `arch_early_init()` / `arch_late_init()` / `scheduler_init()`；aarch64 必须先 dtb 才能用 DTB 信息；内存阶段须满足 M0→PMM→M1→M2/M3 的资源可用顺序。**Spec A 中断 dispatch / Spec B SMP+timer / Spec C kernel_main 单一入口** | M1–M3 的顺序契约 ✅；SMP ✅, arch_irq ✅, GIC ✅, Timer ✅, UEFI 链 ✅ | 长项 spec |
| 中断/异常 dispatch 统一（剩余项） | 第一期（6 条入口）与第二期（x86 48 条入口）已完成。**剩余待统一**：EL0/SVC 异常、aarch64 可恢复 abort 与 uaccess 恢复路径、用户页表、ASID/COW、统一 SMP、统一 `kernel_main` | arch_irq ✅, head.S ✅, GIC ✅；M4 用户态路径仍未开始 | Linux do_IRQ |
| SMP 启动统一 | `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` 单入口；内部 aarch64 PSCI CPU_ON / x86_64 INIT-SIPI + trampoline 各自实现 | GIC ✅, 启动链 ✅ | opuntiaOS |
| 上下文切换统一（剩余项） | EL1 架构上下文门面（`arch/context.h`）与 aarch64 BSP 切换已闭环。**剩余待统一**：多核 SMP 调度切换、IRQ 返回抢占与用户态 EL0/sysret 切换 | EL1 调度 ✅；SMP 启动统一；M4 用户态 | Tilck |
| CPU 特性探测 | `arch_cpu_features()` 返回统一位图（has_fpu / has_virt / has_cache_coherency）；x86 CPUID vs aarch64 ID_AA64* 各实现一份 | 独立 | Linux cpufeature |
| GICv2 Phase 2 | SError/真机覆盖率；`aarch64_main` 接 GIC init；handler 表扩容（SGIs 0-15 + PPI 16-31 全注册）；dtb_gicd_base 解耦 | GICv2 Phase 1 ✅ | |

**距离单一 kernel_main 还差多远（粗估，一个人全职）**：~4–8 周。详见 `docs/arch.md` 末段 3 个 spec/plan 增量推进。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。靠 arch 抽象层封装，统一接口、不统一实现。

### 🧱 P2 架构治理 / 扩展性（2026-10-04 仓库扫描）

已完成治理任务（已合入 master，交付详情见 `docs/changelog.md`）：
- **ARCH-1 syscall 层脱离 arch**（2026-10-04 完成，74 个 syscall 业务 switch 抽离出 `trap.c`）
- **ARCH-2 Linux ABI 兼容层独立**（2026-10-05 完成，`kernel/syscall/compat/` 独立子目录，`int16_t` 翻译表防溢出）
- **ARCH-3 头文件全局符号消除 + `-z muldefs` 裁撤**（2026-10-04 完成，消除 37 TU 重复符号风险）
- **ARCH-4 拆分 `sched/task.c`**（2026-10-04 完成，2764 行巨无霸拆分为调度、进程生命周期、用户栈/ELF 等子模块）
- **ARCH-5 VFS 抽象补强（Phase 1–4 全阶段）**（2026-10-24 完成，merge `c01d7969`：文件系统类型注册表 `vfs_fs_type_t`、`inode_operations`/`file_operations` 拆分并实现 `lookup`/`getattr`、`vfs_node` 元数据补全、活跃节点 `active_nodes` 与延迟孤儿回收、层级节点挂载树、256 项 LRU dcache 引擎）
- **ARCH-7 移除 `#define mmap uint64_t*` 类型宏**（2026-10-23 完成，替换为 `typedef uint64_t *ptbl_t;` 并清理 11 处绕行块）
- **ARCH-9 驱动模型 / 总线抽象**（2026-10-06 完成，块设备 ops、PCI core + typed `pci_driver`、`net_device` 抽象、多网卡与 BDF 观测）
- **ARCH-10 静态容量与工程卫生**（2026-11-02 闭环，单点 NR_CPUS、范围/动态 IOAPIC GSI 上限、x86 扩展中断向量表与固定 MSI-X 解耦、动态 poll 与 RLIMIT_NOFILE 软硬限制、动态 blockdev/devfs、工程卫生与 .gitignore/.S 清理、隔离变体 arch10-capacity 验收）

**ABI 决策（2026-10-04）**：OS01 自有 syscall ABI（`kernel/include/uapi/syscall.h`）为**唯一标准**；Linux x86_64 ABI 仅作为兼容层（服务 BusyBox 等 `PF_LINUX_ABI` 二进制），不反向主导编号或语义。

#### 剩余架构治理任务

| 项 | 问题 | 目标 / 完成条件 | 依赖 |
|----|------|----------------|------|
| ARCH-6 FS 并发与块缓存 | `fat.c`、`tmpfs.c` 0 处加锁（`ext2.c` 19 处），默认 `-smp 2` 下 FAT 表/簇分配/tmpfs 目录存在数据竞争；全内核无 buffer/page cache，每次读直达 AHCI | 短期：每挂载点锁兜底 FAT/tmpfs；中期：blockdev 层按 (dev, blkno) 哈希的块缓存，ext2/FAT 共用，含写回与一致性；SMP 并发读写压力用例 | 独立（正确性优先）；块缓存与 ARCH-9 块层解耦协同 |
| ARCH-8 子系统框架补完 | phase 用魔数 3–6，Phase 1–2 / 7–9 仍硬编码于 `kernel_main`；同 phase 内顺序依赖链接顺序（需 link-order 审计兜底）；必需子系统失败只打印 `FAIL` 继续启动；固定表 `MAX_SUBSYS 64`/`MAX_SUBSYS_PERCPU 16`；框架自身用 `serial_printk` 违背日志规范 | 命名 phase 枚举并覆盖全部启动阶段；支持显式依赖（或 phase 内 order 字段）取代链接顺序；非 OPTIONAL 失败 `panic`；表改链接段驱动（容量随注册数）；日志改 `log_*` | 与“统一 kernel_main”协同 |
| ARCH-11 devfs 子目录化 | `devfs_register_chrdev(name, …)` 只接受 ≤32 字节单段名字，devfs 表 `devices[DEVFS_MAX_DEVICES]`（32 项）拍平；任何 `/dev/<a>/<b>` 形态的层级接口只能退化为 `/dev/<a>-<b>` 或要求重构 VFS lookup 路径。当前 `/dev/null` `/dev/zero` `/dev/random` `/dev/serial` 等是拍平命名；想要 Linux sysfs 风格的 `/dev/pci/<BDF>/<attr>` 必须先解此约束 | devfs 注册接受 `/` 分隔的 path 段；`devices[]` 改为按组件的内部 trie/子表或简单 component table（容量按需，废除 `DEVFS_MAX_DEVICES` 固定表）；VFS `find_mount`/`__vfs_lookup_raw` 支持多段解析（现有单段逻辑需扩展路径分量迭代）；现存拍平命名（`null`/`zero`/`serial` 等）保持兼容，旧 `devfs_register_chrdev("foo", …)` 自动当单段注册；同时引入 `devfs_register_path(const char *path, …)` 多段 API。先单独文档章节并逐步以 hosttest 验证注册/读取/卸载全链 | 独立；P4 NVMe/USB 多级 device 表达、lspci（ARCH-12）等路径化用户态工具的前置；ARCH-5 挂载树与 dcache 已就绪 ✅ |
| ARCH-12 lspci PCI 设备列举命令 | 用户态无任何 PCI 设备列举入口；QEMU 调试、驱动绑定验证、故障注入证据收集都只能看 kernel `log_info`；与 Linux `lspci`（pciutils，读 `/sys/bus/pci/devices/<BDF>/{vendor,device,class,config,resource,irq}`）的能力对齐 | 新 `user/lspci.c`：`syscall(SYS_pci_count)` 取 N → 循环 `pci_device_get(i)` 拿 BDF → 逐项 `open`+`read` `/dev/pci/<BDF>/<attr>`（依赖 ARCH-11）；属性文件：`vendor`/`device`/`class` 返回十六进制文本，`config` 返回原始 256 字节 config space（供 `-x` hex dump），`resource` 返回 BAR 列表文本，`irq` 返回 pin 编号；嵌入式 `pci.ids` 子集（e1000、virtio-net、AHCI 类名）供名称解析，无数据库时回退 `8086:100e` 形式。`mk/components/user.mk` + `config/rootfs.mk` 加 `lspci` 条目；`hosttests/cases/test_pci_devfs.c` 用 `OS01_HOST_TEST` fixtures 验证每个属性文件的字符串/字节内容；QEMU `phase-0` shell 中 `/bin/lspci` 实跑 | ARCH-9 ✅；ARCH-11；pciutils lspci（实现借鉴） |

### 🖥 P3 GUI

已完成基座与功能（详见 `docs/changelog.md`）：
- **底层与驱动**：fb、fb mmap、terminal 双缓冲 + alt-screen、键盘扫描码、PS/2 鼠标驱动（`/dev/mouse` 8 字节事件 ABI）、`libgfx.a` + `/dev/gfx0`、Tetris 与 terminal 迁移至 libgfx、匿名 mmap 像素缓冲。
- **显示所有权与恢复**：帧缓冲显示所有权协调器（`g_owner`/`g_fallback` 状态机、自动 fallback 回退、libgfx API、原始 mmap 边界与只读 PTE 保护、terminal 接管与故障恢复，2026-10-11 闭环）。
- **LVGL 基础与平台后端**：LVGL v9.5.0 sysroot 接入；**GUI-1 LVGL 平台后端（libgui）**（显示/离屏后端、tick、flush、生命周期，2026-10-19/20 闭环）。
- **输入交互与桌面外壳**：**GUI-2 输入与焦点**（独占物理输入租约、LVGL indev、光标、修饰键、Tab 焦点，2026-10-22 闭环）；**GUI-3 Material 风格桌面**（令牌表驱动、状态栏/启动器/抽屉/设置、四个壳内应用、16×24 图标光标、`surface_dim` 壁纸角色，2026-10-24 闭环）。
- **原生控件体系**：**GUI-2b 原生控件与统一焦点引擎**（`gui_widget.h`、`gui_layout.h`、`gui_event.h` 与 `gui_focus.h`，彻底废除桌面临时自建焦点，2026-10-26 闭环）。

#### 后续功能任务（按依赖推进）

| 项 | 内容 / 完成条件 | 依赖 |
|----|-----------------|------|
| GUI-4 字体与资源 | 先使用裁剪的抗锯齿位图字体与编译内置图标，覆盖桌面所需字号；单列中文字符集、字库容量与输入方案。按需再引入动态字体或文件资源加载，并审计依赖、许可证及缓存开销 | GUI-1 ✅、GUI-2 ✅、GUI-3 ✅、GUI-2b ✅；GUI-3 界面文案现暂用 ASCII，汉字字形归本项 |
| GUI-5 Window Server + compositor | 单独设计 surface、IPC、缓冲共享、损伤区域、Z-order、移动/缩放、遮挡、焦点/输入抓取与进程退出回收；可信窗口服务器统一呈现，收紧 `/dev/fb` 与重叠 gfx view 的旁路访问。IPC/共享内存能力先评估，缺项独立补齐；客户端允许使用 LVGL 或其他绘制库 | GUI-1 ✅、GUI-2 ✅、GUI-2b ✅；IPC/共享缓冲与图形权限设计；借鉴 opuntiaOS/HackOS |
| GUI-6 terminal 窗口化 | 保留 PTY/VT100 核心，将渲染输出接入窗口 surface；键盘只投递到焦点终端，验证 Ctrl-C、alt-screen、窗口关闭和 shell 存活；缩放时更新 PTY winsize 并接通 SIGWINCH | GUI-5；terminal/PTY ✅；SIGWINCH（P5 剩余项） |

顺序：GUI-1 ✅ → GUI-2 ✅ → GUI-3 ✅ → GUI-2b ✅ → **GUI-4**；GUI-5/6 是独立系统工程，不与首版单进程桌面捆绑；aarch64 GUI 等待用户态与显示设备基础就绪。

#### 优化与验证项

| 项 | 当前边界 / 后续工作 | 验收依据 |
|----|---------------------|----------|
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
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立；ARCH-5 挂载树与 dcache 已就绪 ✅ | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立；ARCH-5 已就绪 ✅ | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立 | |
| 作业控制（部分完成）| setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅；**剩余**：SIGWINCH 派发、SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs） | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch | libc | |
| libc 完整性 | printf `%f/%F/%e/%E/%g/%G` + `%ld/%lu` + `%x/%o`、strtod、getopt、getcwd ✅；**仍缺**：stdio 高级流接口（getline/fseeko）、cut/paste 的 getopt 解析 | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

### Parked（未闭环 follow-on，随时可拾起）

历史挂起项闭环记录：
- 此前挂起的 follow-ons（sysroot 头文件级增量重编、idle 正名、CFLAGS 编译参数缓存失效、compat/ 目录裁撤、x86 驱动目录重定位、softirq ifdef 清除、LWIP_RAND 种子等）已全部闭环并归档至 `docs/changelog.md`。
- **2026-10-24 闭环归档**：aarch64 并发 replace phantom `-ENOENT`（`80eed2e9`）、hosttests `__test_stats` 复位 bug（`2d594ce0` + CI 接线 `339ffe26`）、内核用户态异常退出状态未编码为信号终止（`3d56e1d0`）、`test_display_owner` 的 libgfx 句柄 fd 耦合（备案归档）、monitor 计划遗留 ⑤ `monitor_term_enter` 的 `fflush`（`9042bd20`）、⑭ monitor 帧被诊断输出撕裂（`c6e1c340`）、`test_lvgl` 早退跳过 alt-screen 恢复条目归档。
- **2026-10-28 闭环归档**：`fd_refcount` 跨核 fixture 的迁移/压力循环时序问题修复（详见 [修复记录](debug/2026-10-28-fd-refcount.md)）。
- **2026-10-31 闭环归档**：dispatch 第一期 host 补强中的 `test_brk_pages.c` 源码扫描缺锚点保护；monitor 计划遗留 ③ `monitor_model_init`/`monitor_sample_destroy` 防御性初始化与析构安全防护；分辨率切换器收尾 follow-up 中 `test_fb_uapi.c` 断言宏规范化与 T6 pty 遗留（`pty.h` 匿名 union 裁撤、`TIOCSWINSZ` 契约注释对齐）；调用链审计 movabs/leaq 无法解析目标的专用断言补强（详见 `docs/changelog.md` 2026-10-31 条目）。
- **2026-11-01 闭环归档**：**QEMU 分辨率切换器收尾 follow-up 全部子项闭环**（2026-10-06 建档的 5 条子项 + 原始清单遗留的 Spec §7 头统一）。① runner 契约：`_drain_case`/`_client_eagain_case` 的 `HOLD ok` 观察窗口由 8s 放宽到 12s（`HOLD_ACK_TIMEOUT = HOLD_SECS + 4s`，窗口必须大于 guest 持有时长，否则租约比 runner 的耐心活得久）；新增 SPEC §8.2.1「runner 串口匹配与超时契约」表（锚定 `start=`、提示符行首锚定、JSON 须同过滤操作与结果码、保持/释放窗口推导）。② 客户端策略：`user/common/gfx_client_policy` 升为**判定与文案的唯一来源**（新增 `gfx_client_verdict_t` + `gfx_client_classify()` + `gfx_client_verdict_reason()`），libgui 新增公共 `gui_platform_status_reason()`；`user/test_lvgl.c` 三份逐字重复的 status→文案块折叠为一个 helper，`user/tetris.c`/`libgui/src/platform.c`/`user/desktop/desktop.c` 改走同一判定（desktop/test_lvgl 早已迁到 libgui 平台层，原「三处重复」的实际形态是「一处 classifier + 三处文案/判定链」）。③ `user/terminal.c` present 门限：`terminal_display_t` 拆分 **present 侧限频**（`present_retry_deadline_ms`）/ **refresh 侧退避**（`retry_deadline_ms`）/ **view 失效标志**（`view_stale`）三个独立状态，消除「瞬时 get_state 失败压制 250ms 有效 present」与「present 失败延迟 mode rebind」两向混淆；errno 集合统一为单一分类器 `terminal_present_errno_classify()`（`RECOVERABLE = {EAGAIN, ESTALE}`），`EBUSY` 不再被 fatal 计数排除（原为「既不计失败、又不重试」的悬空区间，终端空转）。④ `fb_test` token 变为真判别子：消费时记录被消费 fault 的 token，经**内核私有**入口 `fb_test_last_consumed_token()` 供 host test 断言（`/dev/fbtest` UAPI 不变）。⑤ 删除 `kernel/driver/fb.c` 的 `fb_write_row` 生产死码（零调用者，host test 自带 mock）。⑥ Spec §7 头统一：`user/test_terminal_screen.c`/`user/tetris_dump.c` 改 `#include <uapi/fb.h>`，不再本地重复定义 `struct fb_info`。附带修订 `docs/superpowers/specs/2026-10-12-terminal-partial-present-design.md` §7.1（deadline 归属与 errno 集合统一，并如实记录本次对 terminal.c 失败计数的改动例外）。详见 `docs/changelog.md` 2026-11-01 条目。

当前仍挂起项（均不阻塞主线）：

- **中断/异常 dispatch 第一期遗留（2026-10-24；主体与第二期已闭环）**：
  - **host 测试补强（非阻断）**：decoder 表行名用于失败诊断；增加 x86 CPL0 + U/S=1 仍判 KERNEL、ARM OTHER EC 的无效 FAR 断言；ARM backend 有效 FAR 用例补齐地址/有效位；dispatcher 的 x86 host 现场假设与 `memcmp(pt_regs_t)` 布局假设显式化，注明 longjmp 用例无法观察跳转后的清理；x86 backend 删除 reset 后才断言零计数的空检查；ARM backend RED 证据可补有意破坏断言的 mutation。
  - **调用链审计 fixture 补强（非阻断）**：继续梳理重复边诊断。
  - **维护性收尾（非阻断）**：x86 四个向量常量在 decoder/adapter/backend 三处重复；sync/IRQ backend 的 canary 属性不对称可说明；ARM 私有头统一 `pt_regs_t *` 写法；`test_uaccess.c` 分配注释应与实际 order-1（8 KiB）一致。`hosttests/Makefile` 的 “EVERY production header” 注释应限定为内核头与 ARM shim：libc/framework 的既有手写依赖缺口尚未覆盖，后续单独治理。

- **GUI QEMU suite 中 guest 侧 `test_lvgl` 的用户态 SIGSEGV（2026-10-22 偶发）**：
  - 第二轮修复期间观察到一次（SMP 1 `smoke` 报 `rc=139`，`0x49884D` memset），后续运行与最终干净门禁均未复现。未修、未声称已修。后续需在能复现时抓寄存器/栈再定位。详见 [GUI-2 验证报告](superpowers/reports/2026-10-22-gui-2-validation.md) §7.5。

- **测试配置 QEMU 中 ash 用户态 SIGSEGV（2026-10-24 偶发）**：
  - 验证 terminal ARM 时观察到一次，后续放慢命令发送后通过。尚未定位，详见 [terminal ARM 调查](debug/2026-10-24-fbtest-terminal-arm-validation.md)。

- **monitor 计划遗留 parked 项（2026-10-11）**：
  - **正确性防护**：② `file_free` 仅在 `f->node` 非空时调 `vfs_file_release`，`fs_private` 有值而无 node 的理论状态会泄漏快照（open 契约保证不可达）。
  - **验证强度缺口**：⑥ monitor QEMU 双核绑核验证弱（亚 0.2s 迁移漏检可被 epoch 探测错过；需补受限内核 spin 自测）。
  - **语义/契约选择备案**：⑪ `monitor_collect` 在 `opendir("/proc")` 失败时返回 0 + 空进程列表，与"无进程"不可区分；⑫ thermal `stale` 标记与进程 `stale` 字段等待独立 thermal 计划激活。

---

## 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档索引（按 0-4 层组织，按阅读顺序排列） |
| `docs/architecture.md` | 启动链、内存布局、初始化序列（全局骨架图） |
| `docs/boot/boot.md` | UEFI 引导 + `boot_context` v2 ABI + v25 E820 拆分 |
| `docs/arch.md` | arch-neutral facade + per-arch 强覆盖模式（含 `arch/fb_write.h` 门面）+ v25 arch-cleanup 系列 7 子系统矩阵 |
| `docs/arch/cross-boundary-symbols.md` | **跨边界符号/ABI 边界规范（AAGU-4）** —— builtin、UAPI、arch-value、libc 镜像 4 类规则 + 现状对照表 + 后续 issue 切分 |
| `docs/arch/entropy-source-facade.md` | **arch-neutral entropy facade spec（AAGU-5.3 / .5）** —— STRONG/WEAK/NONE 三档质量标签 + 控制流决策 + 禁止条款 |
| `docs/memory/memory.md` + `docs/memory/cow-mmap.md` | 物理/虚拟内存管理 + COW fork/mmap + v25 PMM arch-neutral + 页表层级统一 |
| `docs/interrupt/interrupt.md` | 中断处理（do_IRQ、register_irq、IDT、共同事件分派） |
| `docs/smp/smp.md` | SMP 架构 + 负载均衡实施总结 |
| `docs/sched/scheduler.md` + `docs/sched/scheduler-complexity.md` | EEVDF 调度器设计、复杂度评估与上下文门面抽象 |
| `docs/syscall/syscall.md` | 71+ syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 |
| `docs/signal/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/time/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结 |
| `docs/gui/gui.md` | Tetris、Material 桌面、libgui 平台后端与原生控件统一焦点体系 |
| `docs/fs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/net/network.md` + `docs/archive/lwip-debugging-experience.md` | lwIP 网络栈 + 正确性加固 + init PID 1 延迟启动 |
| `docs/fs/filesystem.md` | VFS 注册表、ops 分离、元数据、挂载树、dcache、FAT32、ext2、devfs、procfs、tmpfs、GPT、block device |
| `docs/driver/driver.md` | 驱动子系统（keyboard, serial, ahci, pci, e1000, virtio-net, fb, mouse, gfx0, logo） |
| `docs/subsys/subsys.md` | 子系统注册框架（`SUBSYS_INITCALL()` + phase 顺序） |
| `docs/log/log.md` | 日志级别、DEBUG_CHANNELS、LOG_TARGET、NDEBUG |
| `docs/build/build.md` + `docs/build/toolchain.md` | 构建系统手册（三篇合一：profile 构建体系、端到端运行调试、构建/测试 harness 权威 taxonomy 与 6 bucket 目标）+ 工具链指南 |
| `docs/testing/testing-policy.md` | 测试分层规范、场景归属与覆盖策略 |
| `docs/debug/debug.md` | 调试通道与历史调查闭环索引 |
| `docs/archive/applet-verification.md` | busybox applet 验证清单 |
| `docs/decisions.md` | 关键设计决策总账（含开源 OS 借鉴对照表） |
| `docs/structure.md` | 目录结构逐目录说明 |
| `docs/changelog.md` | **历史完成记录**（按时间倒序） |
| `docs/superpowers/specs/` + `docs/superpowers/plans/` | 历史设计 spec + 实施 plan（过程档案） |
| `docs/superpowers/reports/` | 历史里程碑验收与闭环验证证据报告 |
| `docs/archive/aarch64/` | aarch64 专题 closure 与 handoff 报告（Generic Timer Phase 1/2、libk.a、IPI fix 等归档） |
