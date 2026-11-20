# OS01 优化路线图

> **基准**: `c4a44cb8`（2026-11-20）；BOOT-1~5 启动治理、libc 补全 T1~T17、Doom 移植 D01~D06、
> systest 模块化、aarch64 每核 tick 记账、framebuffer write-back 修复、
> **aarch64 M4 用户态八块 + 产品用户态接入（`OS01_HAS_USERSPACE=1`）+ 正式 EL0 IRQ 抢占 / 8-bit ASID / SMP 用户调度均已闭环**、
> **统一 SMP 启动**、**USB 大容量存储 (MSC/BOT) 与 USB HID 引导输入闭环**、
> **x86 NX 按 VMA 策略强制执行**、**libc malloc 堆加固**、**损伤区域与批量 present（`GFX_PRESENT_RECTS`）**、
> **内存子系统架构边界清理**、**测试框架生成式目录重构与目录漂移门禁接入 CI**、
> **aarch64 VFS 门打开**、**aarch64 内核启动块平台配置化（`AARCH64_BOOT_BLOCK_BASE`）**、
> **aarch64 平台值解绑与早期 MMIO Device 窗口 / 控制台 handoff 运行时化**
> **日期**: 2026-11-20

roadmap 只列**未完成 / 进行中**的规划项；所有已完成工作见 `docs/changelog.md`，
近期闭环项的快速索引见文末「近期闭环索引」。

## 当前状态（一句话）

- **Phase 1-9**：全部就绪（COW/mmap、调度/信号/SMP、文件系统、设备驱动、用户态、poll/select、网络、时间系统，PID 1 init 优雅关机闭环）
- **工程与架构治理**：AAGU 全套就绪；ARCH-1（syscall 脱离 arch）、ARCH-2（Linux ABI 兼容层独立）、ARCH-3（消除 muldefs 全局符号）、ARCH-4（拆分 sched/task.c）、ARCH-5（VFS 抽象补强）、ARCH-6（FS 并发与块缓存）、ARCH-7（移除 `#define mmap`）、ARCH-8（子系统框架补完，2026-11-08 随 BOOT-3/BOOT-4 闭环）、ARCH-9（驱动模型/PCI/总线解耦）、ARCH-10（静态容量与工程卫生）、ARCH-11（devfs 子目录化）、ARCH-12（`/bin/lspci`）、ARCH-14（USB xHCI + `/bin/lsusb`）**均已闭环合入 master**；**MEM-1（内存子系统架构边界清理，2026-11-14）** 亦已闭环——用户 VM / uaccess / VMM 所有权下沉到各自架构，公共面只剩 TLB 广播协议与逻辑 VMA 位
- **测试与工程基础设施**：测试框架生成式目录重构闭环（2026-11-14）——`config/test-catalog.json` 由 `tools/tests/scan_tests.py` 生成并入库，六处平行的 SUITE 白名单收敛为单一源，宿主测试注册表改为消费生成清单，场景目录 `config/test-scenarios.json` 落地（340 场景），断言分层政策 + 豁免登记表 + 静态审计处置台账到位，flake 账本报告器就位；**目录漂移门禁接入 CI**（2026-11-17，`make check-test-catalog` 进入 CI `harness` job，字节级 drift 由 `check_ci_schedule.py` 规则 2b 反向守护）
- **启动编排治理**：BOOT-1~5 五项 spec 全部实施并验收（2026-11-08）—— CPU 生命周期与 SMP 握手、早期中断与时间设施顺序、启动失败传播、阶段契约与跨架构编排、调度启动与服务解耦；双架构统一 `kernel_main` 入口已于 2026-11-07 闭环
- **aarch64 适配**：EL1 内核、统一 SMP 启动（2026-11-19）、每核 tick 记账已就绪；**VFS 门（2026-11-18）、产品用户态接入（2026-11-19，`OS01_HAS_USERSPACE=1`，`/bin/init` → `/bin/sh`）与正式 EL0 IRQ 返回抢占 / 8-bit ASID / 固定单核亲和 SMP 用户调度（2026-11-20）均已闭环**；共享 syscall 表在 aarch64 侧服务 47+ 项（含 misc 组）；**剩余**：运行期跨核任务迁移、EL0 FP/SIMD、`arch/context.h` 用户态进入/返回门面、跨进程 inode 级 `MAP_SHARED`、userland libc 完整度（floatscan/strtod/setjmp 等）；**真机适配**：加载地址层（`AARCH64_BOOT_BLOCK_BASE`）、DTB 地址层（PL011/GIC）与早期 MMIO Device 窗口 / 控制台 handoff 运行时化均已落地（2026-11-18 ~ 11-19），剩余项（GIC Group-1、spin-table、PL011/timer 形状校验、GICv3、handoff 容量、早期映射 2 GiB 上限、真机启动验证）见 P2 节专项表
- **用户态 libc**：补全 T1~T17 闭环（2026-11-09），15 个接口组全部交付且有 host + guest 证据；**POSIX 正则引擎（T16）与 glob 匹配本期不实现**（用户决定，保留桩）
- **GUI**：PS/2 鼠标、`libgfx.a` + `/dev/gfx0`、Tetris 与 terminal 迁移、QEMU 分辨率切换与显示所有权协调器、LVGL v9.5.0 初步移植与图形 showcase、GUI-1 LVGL 平台后端、GUI-2 输入与焦点、GUI-3 Material 风格桌面、GUI-2b 原生控件体系与统一焦点引擎均已合入 master；**帧缓冲映射已从 UC 改为 write-back 并统一所有别名 cache policy（2026-11-11）**；**x86 帧缓冲宽写优化 + RAM shadow 滚动缓冲（`FB_SHADOW=1`，默认关）+ present A/B 诊断（2026-11-15/16）**；**损伤区域与批量 present（`GFX_PRESENT_RECTS`，2026-11-20）**；后续推进 GUI-4 字体资源与 GUI-5/6 窗口服务器
- **安全与内存**：用户堆与 ELF 映射隔离（4 KiB ELF 分段加载、512 MiB 用户信封、brk 动态收缩与页解绑、保护范围审计）已闭环；**x86 NX 已按 VMA 策略强制执行**（2026-11-13）并细化到 **ELF 分段叶子**（2026-11-20，`.data`/`.bss` RW+NX、`.rodata` RO+NX、仅 `.text` 可执行，见 `docs/memory/cow-mmap.md`）；**libc malloc 堆加固（double-free/溢出检测，2026-11-18）**已闭环；用户态 ASLR 与 UBSan/KASan 规划中
- **硬件适配**：USB xHCI 主机控制器与 USB 核心栈 Phase 1–4 就绪（PCI 探测、寄存器解析、命令/事件环、中断器 0、Root Hub 端口复位、Slot 分配、地址配置、EP0 控制传输、配置描述符层次树解析、动态端点环、多端点配置、`/dev/usb` 属性切片节点与用户态 `/bin/lsusb`）；**USB 大容量存储 (MSC/BOT) 闭环**（2026-11-14/15：CBW/CSW 分帧、SCSI transparent 命令集、注册为 `BLOCK_DISK` 并进 devfs/引导挂载路径、写穿透 + SYNCHRONIZE CACHE 持久化、固件 ESP GUID 选盘）；**USB HID 引导输入闭环**（2026-11-15：Boot Protocol 键盘/鼠标解码并接入 `input_route` 独占租约，与 PS/2 并存）；**xHCI Hub 分层枚举、Scratchpad Buffers、PSIC 速度类解析**均已落地；ACPI **S5 关机已实现**（FADT PM1a + SLP_EN）；**剩余**：物理实机 USB 启动验收、USB 大容量存储 UAS/4Kn/热插拔、ACPI 休眠
- **应用层**：Doom 移植闭环（2026-11-11）—— `doomgeneric` 引擎经独占 `libgfx` surface + 租约化 PS/2 输入发布为 `/bin/doom`，含 IWAD 本地打包、许可材料、存档与配置持久化、跨 SMP boot 玩法验证；**音频输出为禁用静音桩**（尚无声卡驱动）
- **项目与许可**：项目更名为 **Craton**（原 OS01；仅 README 标题与启动字标更名，代码内 `OS01_*` 标识符按"其它标识符暂不动"的策略保留，2026-11-17）；仓库补 **MIT 许可**与第三方组件逐项归属表（busybox/doomgeneric GPL、lwIP BSD-3、musl 算法 MIT 等，均按各自许可随附，2026-11-16）

详细背景、commit 记录、经验教训见 `docs/changelog.md` + 各专题 closure 文档（`docs/archive/aarch64/`）+ 主题 docs。

---

## 待实施路线图（按 5 优先级）

> **P0 工程基础** ✅（仅剩一项尾巴，见下） → **P1 安全加固** → **P2 aarch64 适配 + 架构治理/扩展性** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### 🧰 P0 工程与测试基础设施（仅剩一项尾巴）

2026-11-14 的测试框架生成式目录重构已闭环（spec [2026-11-14-test-framework-design.md](superpowers/specs/2026-11-14-test-framework-design.md) / plan [2026-11-14-test-framework-redesign.md](superpowers/plans/2026-11-14-test-framework-redesign.md)）。当前形态：`config/test-catalog.json` 由 `tools/tests/scan_tests.py` 扫描 `@os01-test` 元数据生成并入库，六处平行的 SUITE 白名单收敛到 `mk/generated/test-dispatch.mk` 单一源，宿主测试注册表消费生成清单（手写 `register_host_test` / `TEST_BINS` 已退役），场景目录 `config/test-scenarios.json`（340 场景）按层归属校验，断言分层政策 + `config/assertion-exemptions.json` 豁免登记 + `docs/testing/audit-dispositions.md` 处置台账到位。

| 项 | 内容 / 完成条件 | 依赖 | 备注 |
|----|-----------------|------|------|
| flake 账本入库 | `make test-flake-report` 报告器已就位（连续失败 ≥3 轮标记 quarantine 候选，`QUARANTINE_STREAK_THRESHOLD=3`），但**账本本身没有入库文件**：报告只能现算——本地走 `build/*/logs/tests/**/result.json`，CI 走 artifacts（`flake_report.py` 只读、不写回），本地无历史账本可审计 | 报告器 ✅ | `qemutests/flake_report.py`、`mk/components/run.mk:724` |

### 🔒 P1 安全加固

前置链（已完成，详见 `docs/changelog.md`）：`getrandom` → 统一用户态启动方式 → 用户栈 canary + `AT_RANDOM` → AAGU-5 arch entropy facade（STRONG-only 控制流）→ 用户堆与 ELF 映射隔离（4 KiB ELF 分段加载、512 MiB 用户信封、brk 动态收缩与页解绑、保护范围审计，2026-10-02 闭环，commit `277b6315`）。ASLR 的随机种子须由内核 STRONG-only 接口提供；`AT_RANDOM` 已就绪，但不是 mmap 选址函数的直接输入。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| ASLR-A：mmap 基址 | 先限 x86_64 用户态：每次新地址空间建立时随机化 mmap 搜索基址；fork 继承现有布局；保留 MAP_FIXED 语义，并验证地址窗口、冲突与溢出 | STRONG-only `kernel_random_get_strong()` ✅（`kernel/random/random.c`）；需先定熵不可用时的失败语义与用户 VA 窗口。当前 `mm->mmap_base` 是常量 `0x40000000`（`kernel/memory/mm.c:17`），搜索从该常量起 first-fit（`kernel/arch/x86_64/memory/mmap.c:452`） | |
| ASLR-B：ET_DYN/PIE | 单独实施可重定位 ELF 的加载偏移、静态 PIE 重定位及用户程序构建迁移（含 BusyBox） | ASLR-A 验收；当前 ELF loader 仅支持 ET_EXEC（非 `ET_EXEC` 直接 `-ENOEXEC`，`kernel/fs/elf_layout.c:93`——该文件已架构中立化，**旧路径 `kernel/arch/x86_64/fs/elf_layout.c` 已不存在**；`kernel/include/fs/elf.h:74` 只定义 `ET_EXEC`）、用户构建使用 `-fno-pie -no-pie`（`user/Makefile:29,79` + `mk/profiles/x86_64-clang.mk:177`）；需设计重定位、linker script、启动 ABI/auxv 与回退测试 | |
| UBSan + KASan | 内核编译期 instrument | 独立（构建体系当前无 `-fsanitize` 接入；`mk/`、`kernel/Makefile`、`libc/Makefile`、`user/Makefile` 全树零命中，第三方 lwIP 上游文件与工具链自举脚本除外） | ArvernOS |
| ELF 分段 NX 细化 | x86 栈/堆/`PROT_EXEC` 的 NX 已在 2026-11-13 闭环（见 `docs/memory/cow-mmap.md`），**剩余**：ELF 装载器不细化分段 NX，`.data`/`.bss` 叶子为 RWX、`.rodata` 叶子为 ROX，均无 NX | `kernel/arch/x86_64/fs/elf.c:67-70`（`elf_pte_flags()` 只按 `PF_W` 分派 `PAGE_USER_PTE` / `PAGE_USER_PTE_RO`；两个宏都不含 `PAGE_NO_EXEC`，`kernel/include/arch/x86_64/pte.h:64-65`）。**根因**：ELF 加载器**完全不建 VMA**（直接写 PTE），故 2026-11-13 的 VMA 级 NX 策略（`kernel/include/arch/x86_64/vma.h:32-40`）**覆盖不到 ELF 段叶子**——对比栈 `exec.c:526,716` 带 `PAGE_NO_EXEC`、堆 `brk.c:61` RW/NX | |

ASLR 分期实施，不把 A/B 合成一个小任务。当前用户栈固定在 `X86_USER_STACK_BASE = 0x20400000`（`kernel/include/arch/x86_64/user_layout.h:20`）；栈随机化另列后续范围，完成 A/B 后也不能称为完整用户态 ASLR。aarch64 已有 EL0 用户态（M4 八块，见 P2），但 ASLR 本项先不扩大到 aarch64。

### 🏗 P2 aarch64 适配

**已完成前置项**（详见 `docs/changelog.md` 2026-10-05 ~ 11-14 各条目 + `docs/arch.md`）：
v25 arch-cleanup / PMM arch-neutral / 页表原语 / Generic Timer Phase 1 + Phase 2 #1~#5 / GICv2 Phase 1 / AAGU-3 subsys_stub convergence / AAGU-29 libk.a link / IPI TPIDR_EL1 fix / 引导字标 / M0 启动映射契约 / M1 运行期直映 / M2 Slab 接入 / M3 内核 VMM / EL1 BSP 协作式内核线程调度 P1–P5 / AP 协作式调度 P1（轮询）与 P2（SGI + WFI）/ 每核调度 tick 记账 / 中断-异常分派统一第一期与第二期 / 双架构统一 `kernel_main`。
**M4 用户态八块（2026-11-11 ~ 2026-11-14 闭环，编号口径同 `docs/build/build.md`）**：M4.1~M4.7 隔离 EL0 探针（私有 TTBR0、SVC 跨页复制与 `-EFAULT`、静态 ELF 装载、真实 CNTP IRQ 返回 EL0、双上下文协作/中断切换、内核线程代跑）；M4 用户页表验收（`aarch64_user_mm`，mm 私有根与销毁回收）；M4.8 正式 EL0 用户任务（`aarch64_user_task_lifecycle`）；M4.9 正式 SVC 与 uaccess 故障恢复（`aarch64_user_svc_uaccess`）；M4.10 匿名 VMA 与按需缺页（`aarch64_user_vm`）；M4.11 原生 `SYS_fork` 与写时复制（`aarch64_user_fork_cow`）；**M4.12 原生 `SYS_waitpid` 与父子退出/ZOMBIE 回收**（`aarch64_user_waitpid`）；**M4.13 原生 `SYS_exec` 与 SysV 用户栈参数传递**（`aarch64_user_exec`）；**M4.14 原生信号投递与 `sigreturn`**（`SYS_kill`/`SYS_signal`/`SYS_sigprocmask`/`SYS_sigreturn`，`aarch64_user_signal`）。**现状叙述与边界见 `docs/arch.md` §aarch64 用户态现状（含 2026-11-18 VFS 门）与 §aarch64 平台值解绑；闭环记录见 `docs/changelog.md` 2026-11-11 ~ 2026-11-18。**

#### 内存管理与执行体后续任务

> M4 用户态八块（2026-11-11 ~ 11-14）、VFS 门（2026-11-18）、产品用户态接入（`OS01_HAS_USERSPACE=1`，2026-11-19）及正式 EL0 IRQ 返回抢占 / 8-bit ASID / 固定单核亲和 SMP 用户调度（2026-11-20）均已闭环，详见 `docs/changelog.md` 与 `docs/arch.md`。

| 任务 | 内容与完成条件 | 前置 |
|------|----------------|------|
| 运行期跨核任务迁移 | 解除当前任务终身固定单核亲和限制，补全跨核负载均衡与 `TTBR0`/ASID/per-task VM 状态同步 | 固定单核亲和 SMP 用户调度 ✅（2026-11-20） |
| EL0 FP/SIMD 上下文 | 开放 EL0 FPEN 与 `switch_to()` / signal 帧保存恢复（目前 userland 走 `-mgeneral-regs-only` soft-float） | SMP 用户调度 ✅；libc floatscan/strtod |
| 跨进程 inode 级 `MAP_SHARED` | 引入 shmem/页缓存注册表，使非 fork 后代的独立进程间共享文件/匿名映射可见一致（目前仅限 fork 后代） | 文件映射与 `MAP_SHARED` v1 ✅（2026-11-19） |

#### 架构统一推进链（迈向单一 kernel_main）

剩余待统一项：

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 上下文切换统一（剩余项） | EL1 架构上下文门面（`arch/context.h`）、aarch64 EL0 切换、IRQ 返回抢占与多核 SMP 用户调度已闭环。**剩余待统一**：**用户态进入/返回纳入 `arch/context.h` 门面**（x86 `iretq` 与 aarch64 `eret` 仍各走各的实现） | EL1 调度 ✅；M4 ✅；SMP 用户调度 ✅ | Tilck |
| CPU 特性探测 | `arch_cpu_features()` 返回统一位图（has_fpu / has_virt / has_cache_coherency）；x86 CPUID vs aarch64 ID_AA64* 各实现一份（仓库内目前无此接口） | 独立 | Linux cpufeature |
| GICv2 Phase 2 | SError/真机覆盖率（**SError 三个向量槽目前是自跳，无保存/诊断/恢复路径**——`kernel/arch/aarch64/intr/entry.S:63,91,107` 均为 `b .`）；handler 表扩容（当前生产路径只注册 SGI 3/4 与 CNTP PPI，SGI 0~2 仅自测用，SGI 5-15 与 PPI 16-31 未注册） | GICv2 Phase 1 ✅；`dtb_gicd_base` 解耦 ✅（2026-11-18） | |

**距离「用户态就绪的统一内核」还差多远（粗估，一个人全职）**：单一入口、`OS01_HAS_USERSPACE=1` 产品启动、统一 SMP 启动（2026-11-19）、正式 EL0 IRQ 返回抢占、8-bit ASID 与固定单核亲和 SMP 用户调度（2026-11-20）均已达成；剩余差距集中在 `arch/context.h` 用户态进入/返回门面、运行期跨核任务迁移、EL0 FP/SIMD、userland libc 完整度（floatscan/strtod/setjmp 等）与跨进程 inode 级 `MAP_SHARED`。详见 `docs/arch.md` §aarch64 用户态现状与末段、统一 kernel_main spec/plan。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。靠 arch 抽象层封装，统一接口、不统一实现。

#### 真机适配审计：QEMU virt hardcode 清单（2026-11-17 扫描）

范围：`kernel/arch/aarch64/` + `kernel/include/arch/aarch64/` + `boot/uefi/arch/aarch64/` 全量通读（head.S 页表构建、DTB 解析、GIC/PSCI/时钟、UEFI loader、内存 preflight）。**结论**：hardcode 不是零星魔数，而是三层固化——① DRAM 基址 / 内核加载地址（0x40000000 / 0x40080000）贯穿 6 个文件互为同值耦合；② 早期 MMIO 窗口 [0x08000000, 0x0A000000) 在 head.S 固化；③ DTB 解析器（`dtb_parse.c`，FDT v17 结构校验本身很扎实）实为 **"QEMU virt 形状校验器"**——解析出的设备基址先和 QEMU 常量对账、相等才通过，再写死同值，而不是用起来。

> **2026-11-19 更新**：加载地址层（`AARCH64_BOOT_BLOCK_BASE`）、DTB 地址层（PL011/GIC 基址与波特率）及早期 MMIO Device 窗口 / 控制台 handoff 运行时化均已闭环（见 `docs/changelog.md` 与 `docs/arch.md` §aarch64 平台值解绑）；下表只保留**仍开放的真机阻断项**。

**硬阻断（任何非 QEMU virt 平台直接无法启动）**：

| 问题 | 位置 | 真机影响 | 状态 / 剩余解法 |
|------|------|----------|----------|
| DTB 形状校验（PL011 interrupts）与 GICv3 缺失 | `kernel/arch/aarch64/platform/dtb_parse.c:152-155`（PL011 interrupts 仍要求恰 3 cell + LEVEL_HIGH）；全树无 `arm,gic-v3` 支持 | DTB 形状不同的板被拒；GICv3 平台无法启动 | ❌ **仍开放**：放宽 PL011 interrupts 形状校验 + GICv3 另列驱动任务 |
| handoff 窗口容量固化（128 KiB） | `boot/uefi/arch/aarch64/boot.c:246-248` 固定址申请、`:460-461` `EFI_BUFFER_TOO_SMALL`（窗口位置已随 B 派生） | 128 KiB 总窗口对真机 DTB（数百 KiB 常见）偏小 → `EFI_BUFFER_TOO_SMALL` | ❌ **仍开放**：改为 LMA_end 后动态分配 + `boot_context` 传指针 |

**条件阻断（能到早期启动，特定环节卡死/静默失效）**：

| 问题 | 位置 | 真机影响 | 状态 / 剩余解法 |
|------|------|----------|----------|
| GICv2 按 Group 0 编程（IGROUPR 清位，CTLR 只写 bit0） | `kernel/arch/aarch64/intr/gic_driver.c:100-102`（注释自认"QEMU 无 EL3"）、`:73`（`GICD_CTLR=1u`）、`:131-132`（`GICC_CTLR=1u`） | 经 TF-A 启动的真机上 IRQ 被路由为 Group 1，Group 0 中断以 FIQ 信号出现 → 内核在 EL1 的 IRQ 入口永远收不到，**所有 IRQ 静默丢失** | ❌ **仍开放**：真机写 IGROUPR=1 + GICD_CTLR/GICC_CTLR 的 EnableGrp1（bit1），或按 GICD_IIDR/板型检测分组模式 |
| 非 BSP CPU 强制 enable-method=psci | `kernel/arch/aarch64/platform/dtb_parse.c:108`（`exact(n->enable,"psci")`）、`:361-362`（非 BSP 无 psci → -3 FATAL） | spin-table 平台（老 RPi 固件、`cpu-release-addr`）全部 AP 拒启（-3 FATAL） | ❌ **仍开放**：若目标板需要，加 spin-table 等待路径 |
| EL3 分支盲写 SCR_EL3/SPSR_EL3 | `kernel/arch/aarch64/head.S:191-215`（`CurrentEL==3` → `msr scr_el3` `:202` / `msr spsr_el3` `:209`） | 仅 QEMU `-M virt,secure=on` 场景成立；真机 TF-A 不从 EL3 起跑，且盲写会踩掉 secure world 依赖位 | ❌ **仍开放**：标注仅 QEMU 支持（真机主路径是 EL2/EL1 分支，不受影响） |
| timer 中断形状校验偏脆（要求 interrupts 第二项 = PPI 14） | `kernel/arch/aarch64/platform/dtb_parse.c:200-207`（`len < 24 \|\| len % 12 \|\| data+12 != 1 \|\| data+16 != 14`；`cntp_ppi` 写死 30 = 16+14，GICv2 架构定值本身正确） | 真机 DT 省略 secure timer 项时条目位移被 -4 拒 | ❌ **仍开放**：改为按"non-secure physical timer 的 PPI 30"匹配而非固定槽位 |

**平台能力上限（非阻断，选型需知）**：PA 上限 40-bit/1 TiB（`head.S:630` TCR IPS=0b010、`page_table.c:51`、`vmm_backend.c` 多处 `>= (1<<40)`；正解读 `ID_AA64MMFR0_EL1.PARange` 动态设 IPS）；早期 identity map 现为 **[0x00000000, 0x80000000) 减去 Device 窗口 [0x08000000,0x0a000000)**（`head.S:485-501` 填 PMD_low0 0..1 GiB + `:549-566` 填 PMD_low1 全部 512 槽；>2 GiB 依赖 M1 runtime tree，该层通用）；**aarch64 已接 RNDR/RNDRRS**（`kernel/arch/aarch64/cpu/random.c:30-60,110-122`，经 `ID_AA64ISAR0_EL1.RNDR` 探测，输出 STRONG/WEAK/NONE），STRONG 池可用；无 RNDR 的板仍只剩 EFI_RNG_PROTOCOL（RPi UEFI 有 RngDxe；无 RngDxe 的板 random_init fail-closed）—— *注：2026-11-17 扫描稿写的"无 RNDR 接线"是错的，RNDR 早在 2026-09-25 已接线*；`AARCH64_M1_SPARSE` 假设 0x4fe00000..0x50400000 是 RAM（`ram.c:140-157`，仅自测宏，需 `AARCH64_M1_TEST=sparse`）。

**已可复用的真机地基**（无需返工）：PSCI conduit（DTB 取 smc/hvc）+ 版本 ≥0.2 校验 + CPU_ON64，发布协议带 clean-to-PoC + DSB（`smp.c`）；时钟频率动态读 `CNTFRQ_EL0`；framebuffer PA 来自 GOP、运行期经 vmm 映到固定 VA；RAM 来自 UEFI memory map（非 DT `/memory`）；缓存行大小经 `CTR_EL0` 动态获取（含 loader `dc cvau`/`ic ivau`）；MIDR 只查 architecture=0xF（`04caf253` 已去 Cortex-A53 假设）；FDT v17 解析骨架（cells/字符串/结构校验完备）。

**剩余解绑顺序**：①~④（DTB 基址白名单、运行期串口/GIC、早期 MMIO/console handoff、加载地址/DRAM 基址 `AARCH64_BOOT_BLOCK_BASE`）均已闭环；剩余按目标板补 Group-1 / spin-table、放宽 PL011/timer 形状校验、GICv3 与 handoff 动态容量，以及实机启动验证。

### 🧱 P2 架构治理 / 扩展性

**ABI 决策（2026-10-04）**：OS01 自有 syscall ABI（`kernel/include/uapi/syscall.h`）为**唯一标准**；Linux x86_64 ABI 仅作为兼容层（服务 BusyBox 等 `PF_LINUX_ABI` 二进制），不反向主导编号或语义。

#### 剩余架构治理任务

**当前无剩余项**：ARCH-1~ARCH-12、ARCH-14 已全部闭环合入 master（清单见本文「当前状态」与 `docs/changelog.md`）。ARCH-8（子系统框架补完）随 BOOT-3 / BOOT-4 于 2026-11-08 闭环，契约细节见 [`docs/subsys/subsys.md`](subsys/subsys.md) 与 [`docs/boot/boot.md`](boot/boot.md)。**MEM-1（内存子系统架构边界清理，2026-11-14）**亦已闭环：用户 VM / uaccess / VMM 所有权下沉到各自架构，公共 `kernel/include/memory/vmm.h` 只保留 TLB 广播协议、`vma_t.vm_flags` 只保留逻辑权限位，硬件 PTE 位由架构按需换算；边界由 `qemutests/mm_header_boundary.py` 自动门禁。详见 `docs/memory/memory.md` §内存子系统架构边界。新增治理项沿 AAGU 编号追加，先记入 `docs/decisions.md` 与 `docs/changelog.md`，再回填本节。

### 🖥 P3 GUI

**已完成基座与功能**（详见 `docs/changelog.md` + `docs/gui/gui.md`）：
- **底层与驱动**：fb、fb mmap、terminal 双缓冲 + alt-screen、键盘扫描码、PS/2 鼠标驱动（`/dev/mouse` 8 字节事件 ABI）、`libgfx.a` + `/dev/gfx0`、Tetris 与 terminal 迁移至 libgfx、匿名 mmap 像素缓冲
- **显示所有权与恢复**：帧缓冲显示所有权协调器（`g_owner`/`g_fallback` 状态机、自动 fallback 回退、libgfx API、原始 mmap 边界与只读 PTE 保护、terminal 接管与故障恢复，2026-10-11 闭环）
- **LVGL 基础与平台后端**：LVGL v9.5.0 sysroot 接入；GUI-1 LVGL 平台后端（libgui）（显示/离屏后端、tick、flush、生命周期，2026-10-19/20 闭环）
- **输入交互与桌面外壳**：GUI-2 输入与焦点（独占物理输入租约、LVGL indev、光标、修饰键、Tab 焦点，2026-10-22 闭环）；GUI-3 Material 风格桌面（令牌表驱动、状态栏/启动器/抽屉/设置、四个壳内应用、16×24 图标光标、`surface_dim` 壁纸角色，2026-10-24 闭环）
- **原生控件体系**：GUI-2b 原生控件与统一焦点引擎（`gui_widget.h`、`gui_layout.h`、`gui_event.h` 与 `gui_focus.h`，彻底废除桌面临时自建焦点，2026-10-26 闭环）
- **帧缓冲 cache policy**（2026-11-11 闭环）：framebuffer 映射由 UC 改为 write-back，policy 由 backend 自证发布并与页表映射构成同一事务，所有别名（kernel 永久映射 / `/dev/fb` 用户别名 / early boot 别名）统一同一内存类型
- **纯 PS/2 decoder API**（2026-11-10）：扫描码解码从 `libgui` 内部拆为纯函数公开头，供 Doom 的租约输入适配复用
- **呈现路径优化与诊断**（2026-11-15/16）：x86 帧缓冲宽写（架构中立 facade 弱默认 + x86_64 强覆盖，32 位顺序写原语）、RAM shadow 滚动缓冲（`FB_SHADOW=1`，**默认关**）、present A/B 诊断 ioctl（`GFX_TEST_PRESENT_AB`，含分段耗时）
- **损伤区域与批量 present**（2026-11-20 闭环）：`GFX_PRESENT_RECTS=0x470D`（≤64 矩形、整批校验、单租约单 setjmp 共用 bounce）、terminal 损伤列表与 LVGL `flush_cb` 双消费端迁移，详见 `docs/gui/gui.md` §损伤区域与批量 present 与 `docs/changelog.md`

#### 后续功能任务（按依赖推进）

| 项 | 内容 / 完成条件 | 依赖 |
|----|-----------------|------|
| GUI-4 字体与资源 | 先使用裁剪的抗锯齿位图字体与编译内置图标，覆盖桌面所需字号；单列中文字符集、字库容量与输入方案。按需再引入动态字体或文件资源加载，并审计依赖、许可证及缓存开销 | GUI-1 ✅、GUI-2 ✅、GUI-3 ✅、GUI-2b ✅；**GUI/LVGL 栈当前字体仍为内置 `lv_font_montserrat_{12,14,16,20,24}`**（`config/lv_conf.h:659-665`，CJK 源全关 `:682-683`，默认 `lv_font_montserrat_14` `:701`；字形集为 ASCII + U+00B0/U+2022 + FontAwesome 图标），无中文字形、无自建裁剪位图字体；**terminal 侧另有一份内置 PSF2 位图字体 `user/terminal/terminal_font.psf`（objcopy 内嵌），与桌面字库无关**，不可据此认为 GUI-4 已部分落地 |
| GUI-5 Window Server + compositor | 单独设计 surface、IPC、缓冲共享、损伤区域、Z-order、移动/缩放、遮挡、焦点/输入抓取与进程退出回收；可信窗口服务器统一呈现，收紧 `/dev/fb` 与重叠 gfx view 的旁路访问。IPC/共享内存能力先评估，缺项独立补齐；客户端允许使用 LVGL 或其他绘制库 | GUI-1 ✅、GUI-2 ✅、GUI-2b ✅；IPC/共享缓冲与图形权限设计；借鉴 opuntiaOS/HackOS |
| GUI-6 terminal 窗口化 | 保留 PTY/VT100 核心，将渲染输出接入窗口 surface；键盘只投递到焦点终端，验证 Ctrl-C、alt-screen、窗口关闭和 shell 存活；缩放时更新 PTY winsize 并接通 SIGWINCH | GUI-5；terminal/PTY ✅；SIGWINCH ✅（PTY master `TIOCSWINSZ` 尺寸变化时派发到前台进程组，`kernel/tty/pty.c`；`qemutests/test_resolution_switcher.py` 的 `pty-watch` 场景已验证前台进程收到信号并读到四个 winsize 字段） |

顺序：GUI-1 ✅ → GUI-2 ✅ → GUI-3 ✅ → GUI-2b ✅ → **GUI-4**；GUI-5/6 是独立系统工程，不与首版单进程桌面捆绑；aarch64 GUI 等待显示设备基础就绪（用户态已就绪，见 P2）。

#### 优化与验证项

| 项 | 当前边界 / 后续工作 | 验收依据 |
|----|---------------------|----------|
| 测试与 showcase 分离（优先） | 固定尺寸离屏 smoke 不依赖真实屏幕；显示 showcase 不以静默离屏回退替代屏幕验收。覆盖分辨率/最大宽度边界、分配与 present 失败；runner 验证无换行 prompt、静默超时、仅输入回显和跨块标记 | 离屏、屏幕、输入三类结果可区分；失败非零退出并保留日志。**2026-11-11 已补齐 host 可见的帧缓冲像素验收探针（`user/fb_pixel_test/` + `qemutests/fb_pixel_verify.py`），并强制几何门槛（裁小图直接 FAIL 且非零退出）** |
| 帧耗时与呈现能力 | 分别测量绘制、复制、内核提交和等待开销，覆盖多分辨率及实际阴影/滚动/动画场景；按瓶颈优化。vsync、page flip、write-combining 或 GPU 后端另行设计；当前私有缓冲不保证无撕裂 | 记录帧耗时分布、CPU 占用与上传量；优化前后使用同一场景比较。**2026-11-11 已取得首组基线：WB KVM 9.65 / 9.59 ms/帧 vs UC KVM 510.3 / 512.8 ms/帧（同一份源码、逐像素比对 1,296,000 像素 0 失配）—— 可见 UC→WB 的量级收益；但这只降低拷贝时间，不提供无撕裂保证**。**2026-11-15/16 又补三项计量与优化**：① present A/B 诊断 ioctl `GFX_TEST_PRESENT_AB`（`kernel/include/uapi/gfx.h`，含 `copy_ns`/`fb_ns` 分段耗时）；② x86 帧缓冲宽写（32 位顺序写原语，`kernel/include/arch/x86_64/fb_copy.h`，x86 强覆盖 + arch-neutral 弱默认 `memcpy`）；③ RAM shadow 滚动缓冲（`FB_SHADOW=1`，**默认关**，`kernel/core/printk.c`）。**仍缺**：跨分辨率的帧耗时分布统计与统一帧时序子系统——现有计时是诊断级（内核 present 统计 + Tetris 的 `elapsed_ns` + terminal `[term-present]` 的 `begin_ns/end_ns/success_ns` 上报），不是通用采集（全树无 frame-timing 符号） |
| 内存池与资源生命周期 | 评估 4 MiB TLSF 池是否适合桌面；大型绘制缓冲使用 mmap，预算包含字体、图片、图层与多窗口 surface。测试反复创建/销毁、低内存和池耗尽，明确失败处理 | 内存占用有上限；长期运行无持续增长；耗尽不崩溃或破坏状态 |
| libc / 可选 LVGL 能力 | 按实际需求整理 `lseek/SEEK_*` 的标准头声明并补 `fseek/ftell`；浮点矩阵/矢量功能启用前补所需 libm。pthread 后端等待线程、条件变量、信号量与递归 mutex 完整实现；FreeType/ThorVG/图片解码库逐项审计 | 每次配置扩展重新验证构建、符号和运行时；不让可选依赖阻塞基础桌面。**注：stdio 高级流接口（`getline`/`fseeko`/`fgets`/`getdelim`）已随 libc 补全 T8/T9 交付** |
| 构建与版本维护 | 持续验证无修改构建、配置/头文件变更重编、staging 孤立文件清除及已发布 sysroot 编译；审计实际 archive 与当前对象清单一致，删除源码时重建归档。保留固定版本，升级单独回归 | 增量结果与干净构建一致；无陈旧归档成员；符号审计与 GUI 回归通过 |

### 🔧 P4 硬件适配

**已完成前置基座**（详见 `docs/changelog.md`）：
- **xHCI 主机控制器与 USB 核心栈（Phase 1–5 全闭环合入 master）**：
  - **Phase 1 控制器生命周期**：PCI 探测、MMIO 映射、能力/操作寄存器解析、命令环/事件环/中断器 0/DCBAA 分配与握手启动、Root Hub 端口连接检测（commit `c54a1583`）。
  - **Phase 2 端口复位、Slot 分配与 EP0 控制传输**：端口复位与速度协商、Enable Slot、Address Device、EP0 控制传输（Setup/Data/Status TRB 与事件环轮询）及 18 字节设备描述符获取（commit `dcd10284`）。
  - **Phase 3 配置解析、动态端点环与多端点配置**：配置描述符层次树解析、`struct xhci_ep_ring **ep_rings` 动态分配、Configure Endpoint、Set Configuration 及迁移至 Configured 状态（commit `78d6ff85`）。
  - **Phase 4 `/dev/usb` 属性切片节点与 `/bin/lsusb`**：控制器跟踪与原始描述符缓存、`usb_devfs` 10 类文本与二进制切片读取节点、用户态 `/bin/lsusb` 实用工具（`-s [[bus]:][devnum]` 过滤与 `-v` 完整树状解析器，commit `cf071a0b` ~ `ae7a5999`）。
  - **Phase 5 Hub / Scratchpad / PSIC（2026-11-15）**：USB 2.0 Hub 分层枚举与端口准备（`kernel/arch/x86_64/driver/xhci_hub.c`）、Scratchpad Buffers 分配与 DMA 生命周期（`xhci_alloc_scratchpads()`）、自定义 PSI 表（PSIC）解析与速度类映射（`xhci_speed_class()`）。
- **USB 大容量存储 MSC/BOT（2026-11-14 ~ 11-15 闭环）**：CBW/CSW 分帧与超时恢复（`kernel/driver/usb_msc_bot.c`）、SCSI transparent 命令集（INQUIRY / READ CAPACITY / READ10 / WRITE10 / SYNCHRONIZE CACHE，`kernel/driver/usb_msc.c`）、注册为 `BLOCK_DISK` 并进 devfs 与引导挂载路径、写穿透 + cache 同步持久化、固件 ESP GUID 选盘（handoff v3）。详见 [`docs/driver/driver.md`](driver/driver.md#usb-大容量存储-mscbot)。
- **USB HID 引导输入（2026-11-15 闭环）**：Boot Protocol 键盘/鼠标报告解码（`kernel/driver/usb_hid_report.c`）+ xHCI 中断端点取报告（`kernel/arch/x86_64/driver/xhci_hid.c`），接入 `input_route` 独占租约，与 PS/2 **并存**而非替换。详见 [`docs/driver/driver.md`](driver/driver.md#usb-hid-引导输入) 与 [`docs/driver/usb-hid-qualification.md`](driver/usb-hid-qualification.md)。
- **ACPI S5 关机**：`kernel/arch/x86_64/intr/apic/acpi.c` 从 FADT 取 PM1a Control Block 端口（offset 64），`kernel/arch/x86_64/intr/syscall_frame.c` 在 `RB_POWER_OFF` 下写 `SLP_EN`（0x2000）后阻塞等待平台下电。

#### 待实施硬件适配任务

| 项 | 内容 / 完成条件 | 依赖 | 借鉴 |
|----|-----------------|------|------|
| USB boot M0 固件门禁转换 | **`USB_BOOT_CASE=firmware` 仍是过期的预期失败断言**：它断言的是 MSC 驱动落地**之前**的预期失败（runner 仍匹配 `[boot-3] FATAL stage=fs component=root rc=-19`，`qemutests/usb_boot.py:78,242-258`），而 USB-only 引导现在一路走通到 shell（`fs: boot disk selected by GUID (usba), root=usba2 boot=usba1` → PID 1 init → ash 提示符），故该用例超时失败。**它已不是默认路径**：`USB_BOOT_CASE` 默认值已是 `readonly`（`mk/components/run.mk:1175`），`firmware` 仅可显式 opt-in，且不在 `SUITE=usb-boot` 的六组循环内（`:1270-1273`）；`usb-boot` 套件本身已登记 nightly（`config/test-ci-schedule.json:199-201`）。**待决的只是 `firmware` 这一子用例**：① 改为成功门禁（断言 xHCI 枚举 → MSC 读盘 → GUID 选盘 → 根挂载 → shell 提示符的完整证据链）；② 退役该用例，由 `readonly` 用例覆盖成功路径 | MSC/BOT ✅；`readonly` 等六组已 PASS 并已入 nightly | Tilck |
| 真机启动 (USB) —— 物理实机验收 | **代码路径与 QEMU 验收已闭环**（`SUITE=usb-boot` 6/6 子用例：readonly / dual / ahci / persistence / superspeed / hub；另有 `SUITE=usb-hid` 覆盖 direct/hub × SMP=1/2）。**剩余**：制作实机可引导 USB 盘，验证 UEFI + bootloader + xHCI 实机硬件自举路径，并采集真实控制器 BDF、固件版本、外设 `VID:PID`、协商速度、物理拓扑与热拔插日志 | USB 存储 / FAT32 ✅；Hub ✅；HID ✅ | Tilck |
| USB 大容量存储的协议与介质覆盖 | 现仅支持 **BOT**（协议 0x50）、**512 字节逻辑块**、**32 位 LBA**（`kernel/driver/usb_msc.c:137-144`）；**无 UAS**、**无 4Kn**、**无 >2 TiB**、**无热插拔重探测**（`xhci_msc_probe_controller()` 仅在 init 调用一次），也**无用户态挂载入口**（无 `mount` syscall，挂载仅由引导期 ESP GUID 选择决定） | MSC/BOT ✅ | Linux usb-storage |
| NVMe 驱动 | PCIe NVMe 1.4+ 控制器驱动，Admin/IO 队列对与 Doorbell 机制，替代 AHCI 成为高性能块设备 | ARCH-9 PCI/块设备 ✅ | Linux nvme / HelenOS |
| HPET clocksource | 真实硬件跨平台高精度定时器源 | 独立 | |
| ACPI 电源管理（休眠部分） | **FADT/MADT 解析已完成**（仅取 FADT offset 64 的 PM1a 控制块，`kernel/arch/x86_64/intr/apic/acpi.c:352-358`；**DSDT/AML 全树未解析**）；**S5 关机已闭环**（`kernel/arch/x86_64/intr/syscall_frame.c:50-57` 在 `RB_POWER_OFF` 下写 `SLP_EN=0x2000`，其 SLP_TYP 位域为 0）。**剩余 S3 睡眠/唤醒**（SLP_TYP 选择、进入与唤醒路径、真机验证） | S5 ✅ | |

### 📐 P5 ABI 扩展/兼容性

**已完成**（详见 `docs/changelog.md`）：symlink/readlink、exec symlink ABI、syscall 边界审计、AAGU-4 跨边界符号全套、AAGU-5 entropy facade 全套、arch source groups、AAGU-29 libk.a link、arch entropy strong overrides（RDSEED/RNDRRS = STRONG）、**用户态 libc 补全 T1~T17（15 个接口组）**、**aarch64 侧共享 syscall 表扩到 47 项**（2026-11-18 VFS 门：新增 fs 组 32 项，随 `OS01_HAS_VFS` 编入 `kernel/syscall/dispatch.c:64-97`）。

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；`socket ✅ → AF_UNIX`；`mbedTLS ✅（仅入库为 `libmbedtls.a`，尚未接入 BusyBox；BusyBox 走自带 TLS）→ HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc | ELF ✅（loader **显式不检查** PT_INTERP，`kernel/arch/x86_64/fs/elf.c:357`；`kernel/include/fs/elf.h:80` 定义了 `PT_INTERP` 常量但两架构均不消费，aarch64 侧 `kernel/arch/aarch64/memory/user_elf.c:41` 亦显式跳过）；仓库内无 `ld.so` / 共享 libc / 任何 `.so` | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构；**现状**：Linux 兼容层的 `rt_sigaction` 只是转发到旧 `SYS_signal` 的 ABI shim（`kernel/syscall/compat/proc.c:11-19`），无 SA_RESTART / 实时信号 | |
| clone/pthread | 线程模型 + pthread_create | futex ✅；**现状**：`uapi/syscall.h` 无 `SYS_clone`（Linux nr 56 在兼容表里直接映射到 `SYS_fork`，`kernel/syscall/compat/table_x86_64.c:43`），`libc/include/pthread.h` 只声明 mutex 一族（`:10-14`），无 `pthread_create` | |
| readv/writev | scatter-gather I/O | 独立；**现状**：未进 syscall 表，也无 libc 包装 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立；ARCH-5 挂载树与 dcache 已就绪 ✅；**现状**：`SYS_fstatat` 已有（at 族部分落地，`kernel/include/uapi/syscall.h:101`），但 `openat`/`dup3`/`pipe2` 均未进 syscall 表 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立；ARCH-5 已就绪 ✅（当前仅有 pipe 的合成 stat `S_IFIFO`，`kernel/syscall/sys_fs.c:564`，**无 mkfifo**） | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立；**现状**：`libc/unistd/alarm.c` 是静默空桩（无 syscall），`setitimer`/`getitimer` 完全未实现 | |
| 作业控制（部分完成）| setpgid/setsid/getpgid/getsid ✅、tcgetpgrp/tcsetpgrp ✅、tty ISIG + VINTR/VQUIT ✅、kill pid=0/-pid/-1 ✅；**SIGWINCH 派发 ✅**（PTY master `TIOCSWINSZ` 尺寸变化时 `signal_pgrp(pgrp, SIGWINCH)` 派发到前台进程组，`kernel/tty/pty.c:109,137`；`user/test_resolution.c` 的 `pty-watch` 观察者与 `qemutests/test_resolution_switcher.py:1577-1606` 场景给出运行期证据）。注意**未收到 SIGWINCH 时默认动作仍是忽略**（`SIG_DFL` ignore，见 `docs/signal/signal.md:25,71`；内核侧证据 `kernel/arch/x86_64/intr/trap.c:1158-1163` 把 `SIGWINCH` 列入非致命 `SIG_DFL` 集，只清 pending 位不退出，`:1278-1283` 的致命判定同样跳过它）；**剩余**：SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs，当前仅作 ignore 处理，无 stop/continue 状态机）、控制终端与前台进程组的完整语义 | tty termios ✅；PTY ✅ | |
| /proc 完善 | **全局 `/proc/stat`** ✅ 已就绪（自有 key-value 格式，非 Linux 定长列 ABI，`kernel/fs/procfs_fmt.c:112-146`）；**`/proc/<pid>/stat` 不存在**（pid 目录 lookup 只解析 `status`/`maps`/`fd`，`kernel/fs/procfs.c:596-608`）；`status` 已有 Version/Name/Pid/PPid/State/CPU/Priority/BirthId/StartNs/SampleNs/RuntimeTicks（`kernel/fs/procfs_fmt.c:73-106`），**仍缺 signal mask 与 utime/stime**；**`cmdline` 完全缺失**（`kernel/fs/` 全目录零命中） | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget —— **HTTPS 能力本身已具备**：`CONFIG_TLS=y`（`config/busybox.config.in:975`）启用 BusyBox 1.36 **自带**的 TLS 客户端（`networking/tls.c` + `tls_{aes,rsa,fe,pstm}*.c`，非 OpenSSL/mbedTLS 后端），`CONFIG_FEATURE_WGET_HTTPS=y`（`:989`）时 `wget` 直接走 `tls_handshake()`；`libmbedtls.a` 已入库到 sysroot 但 **wget 并不链接它**（BusyBox 由原生 Makefile 构建、仅覆盖 `CC`/`LD`，全树无 `-lmbedtls`）。**剩余**：二选一 —— 把 mbedTLS 真正接进 BusyBox，或明确改用 BusyBox 内建 TLS 并放弃 mbedTLS 路线；并补 HTTPS 端到端验证（**当前无 guest 端 `wget https://` 实测证据**：guest 网络测试走的是 `http://`，`user/nettest.c:224`；`qemutests/build_contract.sh:126-157` 的 HTTPS 分支是宿主侧假 wget 夹具，只断言下载 URL 前缀，不构成 guest 端到端证据） | mbedTLS ✅ 已入库为 `libmbedtls.a` | |
| AF_UNIX/socketpair | 本地 socket IPC | socket ✅；**现状**：`socketpair()` 是 `ENOSYS` 桩（`libc/unistd/net_stubs.c:45-48`）；内核 `socket_alloc()` **不校验 `domain`**（`kernel/net/socket.c:64` 的 `socket_alloc(domain, type, protocol)`，`:67-71` 的 switch 只按 `SOCK_STREAM`/`SOCK_DGRAM` 映射 TCP/UDP，`domain` 仅在 `:81` 被原样存入 `s->domain`），请求 `AF_UNIX` 会拿到 TCP/UDP socket 而不是报错——这是正确性缺口，不只是缺功能 | |
| 更多 applet | ⚠️ **BusyBox 侧并非"grep/sed/find 已就绪"**：当前 `config/busybox.config.in` 只启用了 `CONFIG_FIND=y`（`:446`），而 **`CONFIG_GREP`（`:478`）/ `CONFIG_SED`（`:421`）/ `CONFIG_AWK`（`:412`）/ `CONFIG_XARGS`（`:482`）/ `CONFIG_TAR`（`:181`）/ `CONFIG_DMESG`（`:604`）均未启用**——生成的 `applet_tables.h` 里也没有这些 applet。**连带缺陷**：`config/rootfs.mk:9` 仍创建 `/bin/grep`、`/bin/sed`、`/bin/awk`、`/bin/xargs`、`/bin/tar`、`/bin/dmesg` 等软链，执行时得到 "applet not found"（悬空软链）→ **待决：启用这些 applet 还是撤掉对应软链**。libc 侧 POSIX 正则引擎（T16）未实现（`libc/include/regex.h` 为桩头，`regcomp`/`regexec` 全空转），`glob` 为返回 `GLOB_NOMATCH` 的桩（`libc/stdlib/glob.c:5-13`）；`fnmatch` 已实现（`*`/`?`/`[]`），**仅 `FNM_NOESCAPE` 被消费，其余 flags 全忽略**（`libc/string/fnmatch.c:12` `(void)flags;`、`:55`） | libc | |
| Alpine apk/musl | musl 二进制包兼容路线 | 动态链接器 | cavOS |

### Parked（未闭环 follow-on，随时可拾起）

> 历史挂起项闭环记录（2026-10-24 ~ 2026-11-01）已归档至 `docs/changelog.md`。

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

## 近期闭环索引（2026-11-06 ~ 2026-11-20）

只列条目名与文档指针；完整「做了什么 / 关键结论 / 验证」见 `docs/changelog.md` 对应日期条目。

| 闭环项 | 日期 | 详情 |
|--------|------|------|
| **损伤区域与批量 present**（`GFX_PRESENT_RECTS` + terminal / LVGL 双消费端） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [gui.md](gui/gui.md) · [spec](superpowers/specs/2026-11-19-gfx-damage-batch-present-design.md) |
| **aarch64 正式 EL0 IRQ 抢占、8-bit ASID 与 SMP 用户调度**（固定每任务单核亲和、`user-runtime` irq/asid/smp/product 1/2/4/8 核） | 2026-11-20 | [changelog](changelog.md#2026-11-20) · [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [plan](superpowers/plans/2026-11-20-intr-dispatch-remaining.md) |
| **aarch64 产品用户态接入**（`OS01_HAS_USERSPACE=1`、userland 构建链、`/bin/init` → `/bin/sh`、`MAP_FIXED`/文件映射/`MAP_SHARED`/`/dev/mem`） | 2026-11-19 | [changelog](changelog.md#2026-11-19) · [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) |
| **aarch64 早期 MMIO Device 窗口与控制台 handoff 运行时化**（`BOOT_EARLY_MMIO`、`fdt_early.c`、`early_mmu.c`、`device_window.c`、PL011 4/4 清零） | 2026-11-19 | [changelog](changelog.md#2026-11-19) · [arch.md](arch.md#aarch64-平台值解绑2026-11-18--2026-11-19) |
| **统一 SMP 启动**（`arch/smp.h` 单入口 `arch_smp_boot_aps` + `kernel/core/smp.c` 公共编排与 AP 状态机 + 双架构统一 Fail-Closed，废弃 aarch64 DEGRADED） | 2026-11-19 | [changelog](changelog.md#2026-11-19) · [spec](superpowers/specs/2026-11-19-unified-smp-boot-design.md) · [plan](superpowers/plans/2026-11-19-unified-smp-boot.md) |
| **堆加固：malloc double-free / 溢出检测**（32B header canary/magic、16B 尾 redzone、free 毒化、`SIGABRT`） | 2026-11-18 | [changelog](changelog.md#2026-11-18) · [libc.md](libc/libc.md) |
| **aarch64 VFS 门打开**（`OS01_HAS_VFS=1`：devfs/tmpfs/FAT32/ext2 下沉接入 + virtio-blk-mmio 块设备 + fs syscall 组 32 项，共享 syscall 表 aarch64 侧共 47 项） | 2026-11-18 | [changelog](changelog.md#2026-11-18) · [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) |
| **aarch64 内核启动块平台配置化**（`AARCH64_BOOT_BLOCK_BASE` profile 旋钮 + 五项校验 + `boot_layout.h` 单一常量源 + cpp 生成链接脚本 + loader/M0/M1/RAM 保留四处联动 + `MODE=lma-layout` 验收） | 2026-11-18 | [changelog](changelog.md#2026-11-18) · [spec](superpowers/specs/2026-11-17-aarch64-kernel-lma-design.md) |
| **aarch64 平台值解绑**（PL011 基址与波特率走 DTB、GIC 接受任意基址与 `arm,gic-400`、loader 改走 EFI Serial IO Protocol） | 2026-11-18 | [changelog](changelog.md#2026-11-18) · [arch.md](arch.md#aarch64-平台值解绑2026-11-18) |
| **项目更名 Craton**（README 标题 + 启动字标六字形，其余 `OS01_*` 标识符保留）+ **MIT 许可与第三方组件归属表** | 2026-11-16 ~ 17 | [changelog](changelog.md#2026-11-17) |
| 双架构统一 `kernel_main` 启动编排（单一入口 + 跨架构启动 facade + 特性门） | 2026-11-07 | [changelog](changelog.md#2026-11-07) · [spec](superpowers/specs/2026-11-07-unified-kernel-main-design.md) |
| ARCH-6 FS 并发与块缓存（per-mount mutex + 写回块缓存 + 四核压测） | 2026-11-07 | [changelog](changelog.md#2026-11-07) · [spec](superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md) |
| aarch64 每核调度 tick 记账 | 2026-11-06 | [changelog](changelog.md#2026-11-06) · [验证报告](superpowers/reports/2026-11-06-aarch64-percpu-tick-accounting-validation.md) |
| BOOT-1 CPU 生命周期与 SMP 握手 | 2026-11-08 | [boot.md](boot/boot.md#启动编排治理boot-1--boot-52026-11-08-全部验收) · [验证报告](superpowers/reports/2026-11-07-boot-1-validation.md) |
| BOOT-2 早期中断与时间设施启动顺序 | 2026-11-08 | [boot.md](boot/boot.md#启动编排治理boot-1--boot-52026-11-08-全部验收) · [验证报告](superpowers/reports/2026-11-08-boot-2-validation.md) |
| BOOT-3 启动失败传播与必需能力检查（**同时闭环 ARCH-8 失败处理部分**） | 2026-11-08 | [subsys.md](subsys/subsys.md#boot-3-startup-failure--capability-policies) · [验证报告](superpowers/reports/2026-11-08-boot-3-validation.md) |
| BOOT-4 阶段契约与跨架构编排（**同时闭环 ARCH-8 命名阶段 / 依赖检查 / per-CPU 语义部分**） | 2026-11-08 | [subsys.md](subsys/subsys.md#boot-4-named-stages-dependency-topology--per-cpu-semantics) · [arch.md](arch.md#启动阶段契约boot-42026-11-08-闭环) · [验证报告](superpowers/reports/2026-11-08-boot-4-validation.md) |
| BOOT-5 调度启动与系统服务解耦 | 2026-11-08 | [boot.md](boot/boot.md#启动编排治理boot-1--boot-52026-11-08-全部验收) · [验证报告](superpowers/reports/2026-11-08-boot-5-validation.md) |
| 用户态 libc 补全 T1~T17（15 个接口组 + 端到端验收） | 2026-11-09 | [libc.md](libc/libc.md) · [验收报告](superpowers/reports/2026-11-08-libc-completion-validation.md) |
| libc `remove()` 双语义 + 整数 abs 家族 | 2026-11-10 | [changelog](changelog.md#2026-11-10) |
| 纯 PS/2 decoder API（从 libgui 内部拆为公开纯函数头） | 2026-11-10 | [changelog](changelog.md#2026-11-10) |
| systest 模块化（6094 行单体 → 8 模块，119 条用例） | 2026-11-10 | [changelog](changelog.md#2026-11-10) |
| Doom 移植 D01~D06 全链路 | 2026-11-11 | [games/doom.md](games/doom.md) · [验收报告](superpowers/reports/2026-11-10-doom-validation.md) |
| framebuffer 映射 UC → write-back + 别名 cache policy 统一 + 像素验收 | 2026-11-11 | [driver.md](driver/driver.md#帧缓冲-cache-policy-与映射事务2026-11-11-闭环) · [changelog](changelog.md#2026-11-11) |
| aarch64 M4.1~M4.7 隔离 EL0 探针（私有 TTBR0 / SVC 跨页复制 / 静态 ELF / 真实 CNTP IRQ / 双上下文切换 / 内核线程代跑） | 2026-11-11 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [changelog](changelog.md#2026-11-11) |
| aarch64 M4 用户页表与 mm 生命周期验收（`aarch64_user_mm`） | 2026-11-11 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [changelog](changelog.md#2026-11-11) |
| aarch64 M4.8 正式 EL0 用户任务（`aarch64_user_task_lifecycle`） | 2026-11-12 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [changelog](changelog.md#2026-11-12) |
| aarch64 M4.9 正式 SVC 与 uaccess 故障恢复（`aarch64_user_svc_uaccess`）+ 低 EL 同步事件接入公共分派 | 2026-11-12 | [syscall.md](syscall/syscall.md#跨架构共享-syscall-表2026-11-12) · [changelog](changelog.md#2026-11-12) |
| aarch64 M4.10 匿名 VMA 与按需缺页（`aarch64_user_vm`） | 2026-11-12 | [cow-mmap.md](memory/cow-mmap.md#aarch64-cow--匿名-vma2026-11-11--2026-11-13-闭环) · [changelog](changelog.md#2026-11-12) |
| aarch64 M4.11 原生 `SYS_fork` 与写时复制（`aarch64_user_fork_cow`）+ 用户页所有权统一 | 2026-11-13 | [cow-mmap.md](memory/cow-mmap.md#aarch64-cow--匿名-vma2026-11-11--2026-11-13-闭环) · [changelog](changelog.md#2026-11-13) |
| aarch64 M4.12 原生 `SYS_waitpid` 与父子退出 / ZOMBIE 回收闭环（`aarch64_user_waitpid`） | 2026-11-14 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [changelog](changelog.md#2026-11-14) |
| aarch64 M4.13 原生 `SYS_exec` 与用户栈参数传递闭环（`aarch64_user_exec`） | 2026-11-14 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [changelog](changelog.md#2026-11-14) |
| aarch64 M4.14 原生信号与 `sigreturn` 闭环（`SYS_kill` / `SYS_signal` / `SYS_sigprocmask` / `SYS_sigreturn`、`aarch64_user_signal`） | 2026-11-14 | [arch.md](arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环) · [changelog](changelog.md#2026-11-14) |
| 跨架构共享 syscall 表（aarch64 由同一张表服务 exit/getpid/getppid/clock_gettime/brk/mmap/mprotect/munmap/fork/waitpid/exec/kill/signal/sigprocmask/sigreturn，共 15 项） | 2026-11-12 | [syscall.md](syscall/syscall.md#跨架构共享-syscall-表2026-11-12) · [changelog](changelog.md#2026-11-12) |
| **MEM-1 内存子系统架构边界清理**（用户 VM / uaccess / VMM 所有权下沉到各自架构；公共头只留 TLB 广播与逻辑 VMA 位；边界自动门禁） | 2026-11-14 | [memory.md](memory/memory.md#内存子系统架构边界mem-1-2026-11-14-闭环) · [changelog](changelog.md#2026-11-14) |
| **x86 NX 按 VMA 策略强制执行**（栈/堆不可执行、`PROT_EXEC` 按 VMA 生效、违规取指 SIGSEGV） | 2026-11-13 | [cow-mmap.md](memory/cow-mmap.md#x86-nx-按-vma-策略强制执行2026-11-13) · [changelog](changelog.md#2026-11-13) |
| **测试框架生成式目录重构**（`@os01-test` 元数据扫描 → 生成式 catalog + make 片段 + 漂移门禁；六处 SUITE 白名单收敛单一源；场景目录 340 场景；断言分层政策 + 豁免登记；flake 账本报告器） | 2026-11-14 | [testing-policy.md](testing/testing-policy.md) · [audit-dispositions.md](testing/audit-dispositions.md) · [spec](superpowers/specs/2026-11-14-test-framework-design.md) · [changelog](changelog.md#2026-11-14) |
| **ext2 双 / 三级间接块映射**（读、分配、释放、孤儿 inode 回收四路径齐全） | 2026-11-14 | [filesystem.md](fs/filesystem.md#ext2-间接块映射) · [changelog](changelog.md#2026-11-14) |
| **USB 大容量存储 MSC/BOT**（CBW/CSW 分帧 + SCSI transparent 命令集 + `BLOCK_DISK` 注册 + 引导期 ESP GUID 选盘 + 写穿透持久化） | 2026-11-14 ~ 15 | [driver.md](driver/driver.md#usb-大容量存储-mscbot) · [changelog](changelog.md#2026-11-14) |
| **xHCI Hub 分层枚举 / Scratchpad Buffers / PSIC 速度类解析** | 2026-11-15 | [driver.md](driver/driver.md#xhci-usb-主机控制器驱动arch-14) · [changelog](changelog.md#2026-11-15) |
| **USB HID 引导输入**（Boot Protocol 键盘/鼠标解码 + 接入 `input_route` 独占租约，与 PS/2 并存） | 2026-11-15 | [driver.md](driver/driver.md#usb-hid-引导输入) · [usb-hid-qualification.md](driver/usb-hid-qualification.md) · [changelog](changelog.md#2026-11-15) |
| **USB 引导身份交接 v3 + 固件 ESP GUID 选盘挂载**（`boot_context` v3 携带启动 ESP 身份；GPT 只读元数据与分区发布分离） | 2026-11-15 | [driver.md](driver/driver.md#usb-大容量存储-mscbot) · [changelog](changelog.md#2026-11-15) |
| **构建系统瘦身与表驱动**（RED/canary 管线脚本化、静态审计表驱动、`run.mk` 别名裁撤、聚焦用例与 boot 矩阵由宏生成） | 2026-11-15 | [build.md](build/build.md) · [changelog](changelog.md#2026-11-15) |
| **x86 帧缓冲宽写 + RAM shadow 滚动缓冲（`FB_SHADOW=1`，默认关）+ present A/B 诊断** | 2026-11-15 ~ 16 | [driver.md](driver/driver.md#帧缓冲驱动) · [changelog](changelog.md#2026-11-16) |
| **`timeit` 墙钟计时命令**（`CLOCK_MONOTONIC` + libc `execvp` PATH 搜索；无 user/sys 分项，因内核无 per-task CPU 时间累加器） | 2026-11-16 | [changelog](changelog.md#2026-11-16) |
| **目录漂移门禁接入 CI**（`make check-test-catalog` 进 CI `harness` job；字节级 drift gate + `check_ci_schedule.py` 规则 2b 反向守护） | 2026-11-17 | [changelog](changelog.md#2026-11-17) |

---

## 相关文档

| 文档 | 内容 |
|------|------|
| `docs/README.md` | 文档索引（按 0-4 层组织，按阅读顺序排列） |
| `docs/architecture.md` | 启动链、内存布局、初始化序列（全局骨架图） |
| `docs/boot/boot.md` | UEFI 引导 + `boot_context` v2 ABI + v25 E820 拆分 + **BOOT-1~5 启动编排治理** |
| `docs/arch.md` | Multi-arch weak-default + strong-override 总览（facade/override 矩阵、页表层级、bootinfo ABI、驱动 initcall、IRQ hook 3 段、RTC 拆分、BOOT-3/BOOT-4 启动契约、**aarch64 用户态现状（M4 八块）**、距离单一 `kernel_main`） |
| `docs/superpowers/specs/2026-11-07-unified-kernel-main-design.md` + `docs/superpowers/plans/2026-11-07-unified-kernel-main.md` | 双架构统一 kernel_main 的设计与实施计划（2026-11-07 闭环：启动 facade 钩子 + 特性门 + 单一入口符号审计） |
| `docs/arch/cross-boundary-symbols.md` | **跨边界符号/ABI 边界规范（AAGU-4）** —— builtin、UAPI、arch-value、libc 镜像 4 类规则 + 现状对照表 + 后续 issue 切分 |
| `docs/arch/entropy-source-facade.md` | **arch-neutral entropy facade spec（AAGU-5.3 / .5）** —— STRONG/WEAK/NONE 三档质量标签 + 控制流决策 + 禁止条款 |
| `docs/memory/memory.md` + `docs/memory/cow-mmap.md` | 物理/虚拟内存管理 + COW fork/mmap + v25 PMM arch-neutral + 页表层级统一 + **aarch64 COW 与匿名 VMA（驻留记录所有权模型）** + **MEM-1 内存子系统架构边界** + **x86 NX 按 VMA 策略强制执行** |
| `docs/interrupt/interrupt.md` | 中断处理（do_IRQ、register_irq、IDT、共同事件分派） |
| `docs/smp/smp.md` | SMP 架构 + 负载均衡实施总结 |
| `docs/sched/scheduler.md` + `docs/sched/scheduler-complexity.md` | EEVDF 调度器设计、复杂度评估与上下文门面抽象 |
| `docs/syscall/syscall.md` | 77 syscall 表（0..76）+ 用户指针边界语义 + syscall 边界审计触达清单 + **跨架构共享 syscall 表（aarch64 侧 15 项）** |
| `docs/signal/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/time/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结 |
| `docs/gui/gui.md` | Tetris、Material 桌面、libgui 平台后端与原生控件统一焦点体系 |
| `docs/fs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/net/network.md` + `docs/archive/lwip-debugging-experience.md` | lwIP 网络栈 + 正确性加固 + init PID 1 延迟启动 |
| `docs/fs/filesystem.md` | VFS 注册表、ops 分离、元数据、挂载树、dcache、FAT32、ext2（含**双/三级间接块映射**）、devfs、procfs、tmpfs、GPT、block device |
| `docs/driver/driver.md` | 驱动子系统（keyboard, serial, ahci, pci, e1000, virtio-net, fb, mouse, gfx0, logo, xhci, usb_devfs）+ **帧缓冲 cache policy 与映射事务** + **USB 大容量存储 MSC/BOT** + **USB HID 引导输入** + xHCI Hub/Scratchpad/PSIC |
| `docs/subsys/subsys.md` | 子系统注册框架（`SUBSYS_INITCALL()` + 阶段顺序 + **BOOT-3 失败策略 / BOOT-4 阶段契约与依赖拓扑**） |
| `docs/libc/libc.md` | 用户态 libc：接口覆盖 / 语义要点 / 已知限制（含 T1~T17 落地状态表） |
| `docs/games/doom.md` | Doom 移植：默认构建行为 / IWAD 打包 / 运行参数 / 按键 / 诊断构建 / 引擎许可与来源材料 |
| [启动顺序与阶段抽象分析](superpowers/reports/2026-11-07-boot-sequence-analysis.md) | 静态证据、CPU/IRQ/失败传播风险及 BOOT-1～5 spec 拆分依据（**五项均已实施并验收**） |
| `docs/log/log.md` | 日志级别、DEBUG_CHANNELS、LOG_TARGET、NDEBUG |
| `docs/build/build.md` + `docs/build/toolchain.md` | 构建系统手册（三篇合一：profile 构建体系、端到端运行调试、构建/测试 harness 权威 taxonomy 与 6 bucket 目标）+ 工具链指南 |
| `docs/testing/testing-policy.md` + `docs/testing/audit-dispositions.md` | 测试分层规范、场景归属与覆盖策略 + **生成式测试目录与派发表（§8）** + **静态审计处置台账** |
| `docs/debug/debug.md` | 调试通道与历史调查闭环索引 |
| `docs/archive/applet-verification.md` | busybox applet 验证清单 |
| `docs/decisions.md` | 关键设计决策总账（含开源 OS 借鉴对照表） |
| `docs/structure.md` | 目录结构逐目录说明 |
| `docs/changelog.md` | **历史完成记录**（按时间倒序） |
| `docs/superpowers/specs/` + `docs/superpowers/plans/` | 历史设计 spec + 实施 plan（过程档案） |
| `docs/superpowers/reports/` | 历史里程碑验收与闭环验证证据报告 |
| `docs/archive/aarch64/` | aarch64 专题 closure 与 handoff 报告（Generic Timer Phase 1/2、libk.a、IPI fix 等归档） |
