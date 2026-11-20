# 架构抽象层（multi-arch）

OS01 同时支持 **x86_64** 和 **aarch64**（后者仅 UEFI 启动路径）。本系统最重要的统一模式是：

> **weak-default + strong-override**：每个 arch-neutral 头文件位于 `kernel/include/arch/<topic>.h`，作为派发 facade。弱默认实现在 `kernel/<subsys>/arch_*.c`（FATAL-halt / panic-on-call / silent no-op / identity），强覆盖实现在 `kernel/arch/<arch>/<topic>.c`（per-arch 真实现）。链接器按强覆盖优先解析。`SUBSYS_INITCALL()` + `.subsys_init` section 用于 driver 自注册。

下面列出本模式涉及的所有 facade-override 对（截至 v25 arch-cleanup 系列完成，2026-11-07 新增「启动编排 facade」一行）。

---

## Facade / override 矩阵

| Facade 头（arch-neutral） | 弱默认实现 | x86_64 强覆盖 | aarch64 强覆盖 | 用途 |
|---|---|---|---|---|
| `arch/include/regs.h` | — | `arch/x86_64/regs.h` | `arch/aarch64/regs.h` | `pt_regs_t` 跨 arch 定义；`arch_cpu_pause()` 共享 helper（替换 rwlock 本地 `#if` switch） |
| `arch/include/irq.h` | `intr/arch_irq_hooks.c`（FATAL/identity/no-op） | `arch/x86_64/intr/irq_entry.c`（原生入口适配）+ `irq_hooks.c`（controller / 业务） | — | 中断 controller selection / gsi↔vector / dispatch 三段 hook；do_IRQ 从 `intr/pic/8259A.c` 移出 |
| `arch/include/rtc.h` | `driver/rtc.c` core（走 `arch_rtc_read/write` hook） | `arch/x86_64/platform/rtc_cmos.c` + `rtc_pie.c`（CMOS port I/O + BCD + UIP regA bit7 + BIN regB bit2；PIE/LAPIC/TSC 校准） | — | 实时时钟 + PIE |
| `arch/include/fb_write.h` `arch_fb_write_begin/end` | `driver/arch_fb_write.c`（弱默认：直绘 `Pos.FB_addr`，无 coordinator） | `arch/x86_64/platform/fb_write.c`（强覆盖：display coordinator writer lease，lease 内嵌于调用方持有的 `fb_write_view`） | —（用弱默认） | 独占 framebuffer 写入视图；`fb_write_view` 自持 arch 私有存储，`snap` 指针在 begin→end 之间有效 |
| `arch/include/subsys.h` | — | `arch/x86_64/linker.ld` 收集 `.subsys_init` | — | 10 driver 自注册（apic/pic/pit/lapic-timer/timer/serial/keyboard/ahci/pci/net）；`arch_register_subsys()` 缩到 7 行 loop |
| `arch/sched/...` `kernel_thread_entry` | `sched/arch_kernel_thread_entry.c`（panic-on-call） | `arch/x86_64/cpu/thread_entry.S` | — | arch-neutral 调度器线程入口；`task.c` 不再 include per-arch 头 |
| `memory/pmm.h` `pmm_init(boot_context*)` | —（无弱默认：三项 `pmm_arch_*` 策略 hook 每架构强定义，缺失即链接失败） | `arch/x86_64/memory/{pmm_arch.c,pmm_reserve.c}`（E820 normalize/4 GiB zone split + 遗留前缀 reservation `[0, ceil2M(metadata_end_pa))`） | `arch/aarch64/memory/{pmm_arch.c,early_arena.c}`（读 `aarch64_ram_map_get()`；zone split 显式 `UINT64_MAX`；reservation 返回已发布 arena，未准备报 `-EINVAL`） | 物理内存 arch-neutral 入口；`MEMORY_RANGE[]` 中介；RAM-relative indexing（`pages_struct + ((start - lowest_ram) >> 21)`） |
| `arch/boot_memory.h` `arch_boot_direct_map_init/ready/ranges` | 无默认实现；每架构提供同名后端 | `arch/x86_64/memory/boot_direct_map.c`，checked `vmm_init()` + zone cutoff | `arch/aarch64/memory/boot_direct_map.c`，arena + strict TTBR1 + AP 验证 | 启动期共同契约；coverage 是映射 RAM，包含已占用页；动态 VMM 不在 M1 范围内 |
| `log/log.h` `_log_*_impl` 宏 | `log/log.c` core（gate-wrapped） | `core/log.c` 走 `_log_writev` + vsnprintf + serial | `arch/aarch64/runtime/log_impl.c` 走 `kputs(fmt)` 忽略 variadic | 跨 arch 日志；aarch64 `-nostdlib` 无 vsnprintf |
| `arch/smp.h` `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` + `arch_smp_last_status()` + AP 钩子（`arch_smp_ap_cpu_init/post_online/publish_ipi_ready/enter_idle`）+ `arch_smp_post_boot_hook` | `kernel/core/smp.c`（公共编排 `arch_boot_aps()`：前置不变量 + 结果核验 + Fail-Closed `boot_failure_stop`；AP 状态机 `smp_ap_common_entry()`：cpu_init→online=1(RELEASE)→post_online→irq_enable→mb→ipi_ready→idle；`arch_smp_post_boot_hook` weak no-op） | `arch/x86_64/smp/smp.c`（trampoline 传 `(cpu_id, gs_base)`；INIT-SIPI-SIPI + online/scheduler_ok 有界等待 + TSC 握手） | `arch/aarch64/smp/smp.c`（PSCI CPU_ON + 启动槽 `entry`（offset 40 复用）+ `secondary_entry.S` 经 callee-saved `x20` 调用共享入口；Fail-Closed，DEGRADED 路径已删） | 统一 SMP AP 启动语义与失败策略（2026-11-19 闭环，spec/plan 见 `docs/superpowers/`） |
| `arch/boot.h` 启动编排与阶段校验钩子（`arch_early_init` / `arch_boot_memory` / `arch_pre_subsys_selftest` / `arch_boot_controllers` / `arch_boot_percpu` / `arch_boot_devices` / `arch_boot_device_nodes` / `arch_boot_smp_prepare` / `arch_boot_aps` / `arch_boot_runtime` / `arch_boot_validate` / `arch_post_smp_selftest` / `arch_early_panic` + 无用户态专用 `arch_start_kernel_idle`） | 无弱默认 —— 缺实现即链接失败 | `arch/x86_64/{platform/boot.c,memory/boot.c,smp/boot.c,platform/boot_contract.c}`（转调 `x86_64_boot_early/memory/controllers/devices/device_nodes/percpu/aps` 及能力核验） | `arch/aarch64/{boot/main.c,platform/boot_contract.c}`（原 `aarch64_main()` 改写为同名 facade 实现及能力核验） | 双架构共用单一 `kernel_main` 入口（2026-11-07 闭环，BOOT-4 统一阶段契约与三检查点能力核验）；配合 `arch/features.h` 的 `OS01_HAS_VFS` / `OS01_HAS_USERSPACE` 编译期特性门裁剪缺省阶段 |

**共同模式**：多数 facade 满足「weak default 提供无操作/panic/FATAL/identity 默认行为，strong override 提供真实现，per-arch 通过头文件位于 `kernel/include/arch/<arch>/` 镜像」。链接器强覆盖优先解析同名符号。**没有运行时分派**：编译期决定。

---

## 页表层级（arch-neutral 命名）

**v25 统一**：x86_64 PML4/PDPT/PDE 三层命名 → Linux/ARM 标准 **PGD/PUD/PMD/PTE** 四层。Bit-constant 同步 rename：

| 旧名（x86_64） | 新名（arch-neutral） |
|---|---|
| `mm->pml4` | `mm->pgdir` |
| `vmm_walk_pml4` 等 | `vmm_pt_walk` |
| `PAGE_GDT_SHIFT` | `PAGE_PGD_SHIFT` |
| `PAGE_USER_GDT/Dir/Page` | `PAGE_USER_PGD/PUD/PMD` |
| `PAGE_KERNEL_GDT/...` | `PAGE_KERNEL_PGD/PUD/PMD` |
| `PAGE_USER_4K/4K_RO` | `PAGE_USER_PTE/PTE_RO` |
| `PAGE_KERNEL_4K` | `PAGE_KERNEL_PTE` |
| `PAGE_KERNEL_MMIO` | `PAGE_KERNEL_PMD_NOCACHE` |
| `PAGE_Present` | `PAGE_VALID` |
| `PAGE_U_S` | `PAGE_USER` |
| `PAGE_R_W` | `PAGE_WRITE` |
| `PAGE_PS` | `PAGE_HUGE` |
| `PAGE_XD` | `PAGE_NO_EXEC` |
| `PAGE_Global` | `PAGE_GLOBAL` |
| `PAGE_PCD` | `PAGE_CACHE_DISABLE` |
| `PAGE_PWT` | `PAGE_WRITE_THROUGH` |

~150 站点 rename（vmm.c / sched COW fork / elf loader / vma/uaccess/fb/futex / `test_uaccess.c`）。保留 x86_64 `head.S` 的 `__PML4E`/`__PDPTE` 硬件 label（asm 段不可改）+ `kernel/include/memory/vmm.h` 的 PTE bit-position 常量（标为 x86_64 PTE 格式专属）。

**Bit-position 仍 per-arch**（PGD/PUD/PMD 是层级名，PTE bit 字段 ISA-specific）。aarch64 PTE bit 与 x86_64 不同，由 `kernel/arch/aarch64/memory/page_table.c` 自管。

---

## bootinfo ABI

- `kernel/include/core/bootinfo.h` — arch-neutral 部分：`struct boot_context` v2 ABI（magic/version/size/flags + 内存图 + 帧缓冲 + RSDP/FDT 指针）
- `kernel/arch/x86_64/bootinfo_x86.h` — **v25 拆出**：`struct E820_ENTRY` + `BOOT_MEMORY_FORMAT_E820=1u`。`pmm.c` 强覆盖已校验 `n==0`，不再有 arch-neutral `entry_size` 分支。
- aarch64 编译视图纯净（看不到 E820 符号）

---

## 调度器 arch-neutral 入口

- `arch_kernel_thread_entry` 在 `sched/arch_kernel_thread_entry.c`（弱默认 panic-on-call）
- x86_64 强覆盖在 `arch/x86_64/cpu/thread_entry.S`
- `task.c` 不再 include per-arch 头
- v25 同批把 `kernel/.stage1` + `kernel/.stage2` 加进 `.gitignore`（避免每 make 重生成的中间产物污染 git status）

---

## Driver 自注册

10 driver 自注册：ahci/keyboard/pci/pit/serial/lapic/lapic_timer/pic/net/clocksource/timer。每 `.c` 加 init wrapper + `SUBSYS_INITCALL()` 行。`arch_register_subsys()` 缩到 7 行 loop。

OS01 libc-free 无 `.init_array` runtime support，故采用 Linux initcall 同款 trick（不用 `__attribute__((constructor))`，否则指针落在没人迭代的 section）。

详见 `docs/subsys/subsys.md`。

---

## 中断 hook 三段式

```c
// kernel/include/arch/irq.h
void arch_irq_select_controller(uint32_t gsi);          // 选 controller
uint8_t arch_irq_gsi_to_vector(uint32_t gsi);            // gsi → vector
uint32_t arch_irq_vector_to_gsi(uint8_t vector);         // vector → gsi
void arch_irq_dispatch(pt_regs_t *regs, uint64_t vector); // x86 原生 vector 入口
```

弱默认：`kernel/intr/arch_irq_hooks.c`（select FATAL-halt；vector↔gsi identity；dispatch silent no-op）
x86_64 原生适配在 `kernel/arch/x86_64/intr/irq_entry.c`，先校验 frame/vector，再经 decoder → `arch_intr_dispatch()` → x86 后端；controller 选择、GSI 翻译与设备 IRQ 业务保留在 `irq_hooks.c`。共同 `kernel/intr/vector_dispatch.c::intr_dispatch_registered_vector()` 只负责中立的注册表查找，不含 x86 路由白名单、ACK 或返回工作。

`unregister_irq(uint64_t nr)` → `unregister_irq(uint32_t gsi)`，跟 `register_irq` 对齐（消除 `2026-08-17-timer-clocksource-clockevent.md:20` 文档的 off-by-vector footgun）。

---

## RTC 三层拆分

- `kernel/include/driver/rtc.h` — 只剩 `datetime_t` + `rtc_read/write_datetime`
- `kernel/driver/rtc.c` — 走 `arch_rtc_read/write` hook 的 core（KERNEL_C_SOURCES 跨 arch 编译）
- x86_64 强覆盖 `kernel/arch/x86_64/platform/rtc_cmos.c`（CMOS port I/O + BCD + UIP regA bit7 + BIN regB bit2 全本地化）+ `kernel/arch/x86_64/platform/rtc_pie.c`（PIE/LAPIC/TSC 校准 verbatim move）

---

## 距离单一 kernel_main 还差多远（v25 后）

> **2026-11-07 更新：Spec C（统一 kernel_main）已闭环**，双架构共用单一 `kernel/core/main.c::kernel_main()` 入口，`aarch64_main` 已删除。剩余差距集中在用户态与抢占语义（M4 用户地址空间、EL0/SVC 异常、IRQ 返回抢占、统一 SMP），粗估仍为 4–8 周（一个人全职）。设计与实施记录见 [spec](superpowers/specs/2026-11-07-unified-kernel-main-design.md) / [plan](superpowers/plans/2026-11-07-unified-kernel-main.md)，闭环摘要见 `docs/changelog.md` 2026-11-07 条目。
>
> **2026-11-13 更新：M4 用户态五块已闭环**（单核 BSP 上跑通正式 EL0 用户任务，见下文 §aarch64 用户态现状），
> 剩余差距收窄为**产品接入 + 抢占/多核语义**（产品 `/bin/init` 链路与 VFS 门、waitpid/信号/文件映射、
> IRQ 返回抢占、统一 SMP 启动与多核上下文切换、ASID），粗估 **3–5 周**（一个人全职）。
>
> **2026-11-20 更新：中断/异常 dispatch 剩余项、正式 EL0 IRQ 返回抢占、8-bit ASID 与固定单核亲和 SMP 用户调度已闭环**（`OS01_HAS_USERSPACE=1` 与统一 SMP 启动已于 2026-11-19 闭环；详见 `docs/changelog.md` 2026-11-19 / 2026-11-20 条目与下文 §aarch64 用户态现状）。

~4–8 周（一个人全职），3 个独立 spec/plan 增量推进：

1. **Spec A — 中断/异常 dispatch 收尾 + 上下文切换 arch 抽象**：`arch_irq_dispatch` 已落地；第一期已在 `kernel/intr/dispatch.c` 落地 arch-neutral 无状态事件分派，aarch64 EL1h sync/IRQ 与 x86 #DE/#UD/#GP/#PF 六条入口已接入（证据见 [验证报告](superpowers/reports/2026-10-24-intr-dispatch-phase1-validation.md)）。第二期已将当前安装的全部 48 条 x86 入口接入：异常 `0..14,16..20` 与 IRQ `0x20..0x37,0x38,0x40,0x41,0xff`；syscall `0x80` 及未安装 ISA 异常不在范围。gate 类型/DPL/IST 与返回语义保留，设备 ACK 和 timer/IPI EOI 仍归原业务，`0xff` 不 EOI 且不执行 `ret_from_intr` 工作。MADT type-4 按 processor UID/LINT/flags 配置 NMI，AP 在 GDT/IDT/TSS/TR 就绪后才启用路由。第二期证据见 [验证报告](superpowers/reports/2026-10-25-intr-dispatch-phase2-x86-validation.md)。**2026-11-20 闭环**：EL0/SVC、uaccess、用户页表/8-bit ASID/COW、正式 EL0 IRQ 返回抢占（`aarch64_user_irq_return()`）与固定单核亲和 SMP 用户调度均已落地；**剩余**：用户态进入/返回纳入 `arch/context.h` 门面、运行期跨核任务迁移与 EL0 FP/SIMD。
2. **Spec B — 统一 SMP 启动 + 定时器 + CPU 特性**：`arch_smp_boot_aps` **已于 2026-11-19 闭环**（见上文 Facade 矩阵与 `docs/changelog.md`）；**剩余**：`clockevent` 双 arch 注册与 `arch_cpu_features()`。
3. **Spec C — 统一 kernel_main（✅ 2026-11-07 闭环）**：在 A、B 之上定义 `arch_early_init`/`arch_late_init`，单 `kernel_main` 按固定 init 顺序调（pmm_init → arch_early_init → scheduler → arch_late_init → ...）。设计 init 顺序契约。**实际落地为 `kernel/include/arch/boot.h` 的 9 个强覆盖启动钩子 + `kernel/include/arch/features.h` 的 `OS01_HAS_VFS` / `OS01_HAS_USERSPACE` 编译期特性门**；x86_64 拆为 `x86_64_boot_early/memory/subsystems/device_nodes/percpu/aps`，aarch64 由原 `aarch64_main()` 改写为同名 facade 实现；入口唯一性由 `test-aarch64-audit` 的 readelf 符号审计门禁强制。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。这些靠 arch 抽象层封装，统一接口、不统一实现。

**v25 已统一**：页表层级名（PGD/PUD/PMD/PTE，bit-position 仍 per-arch）、bootinfo ABI（解析层仍 per-arch，但输出 v2 已 arch-neutral）、intr dispatch hook 化、driver 自注册 initcall、kernel_thread_entry arch-neutral、RTC 拆分（核心 arch-neutral + per-arch CMOS/PIE）。

**2026-11-07 已统一**：启动入口 —— 单一 `kernel/core/main.c::kernel_main()` + `arch/boot.h` 强覆盖启动钩子 + `arch/features.h` 编译期特性门；`aarch64_main` 符号删除，入口唯一性由 readelf 符号审计门禁强制。

---

## aarch64 用户态现状（M4，2026-11-11 ~ 2026-11-18 闭环）

M4 把 aarch64 从「只有 EL1 内核」推进到「单核 BSP 上跑得起来的正式 EL0 用户任务」。
八块按顺序交付，编号口径与 [build.md](build/build.md) 一致：

| 块 | 交付 | 验收用例 |
|----|------|---------|
| M4.1~M4.7 | 隔离 EL0 探针：私有 TTBR0、SVC 跨页复制与 `-EFAULT`、静态 ELF 装载、真实 CNTP IRQ 返回 EL0、双上下文协作/中断切换、内核线程代跑 | `aarch64_el0` / `aarch64_el0_irq` / `aarch64_el0_switch` / `aarch64_el0_preempt` / `aarch64_el0_task` |
| 第 1 块 | 用户页表与 `mm` 生命周期：运行期私有用户根、M3 4 KiB 入口、销毁回收 | `aarch64_user_mm` |
| 第 2 块 | 正式 EL0 用户任务：`PF_PROCESS`、私有 `mm`、专有内核栈、EL0 帧住在栈顶、调度器装 `TTBR0` | `aarch64_user_task_lifecycle` |
| 第 3 块 | 正式 SVC 与 uaccess：共享 syscall 表、低 EL 同步事件接入公共分派、用户复制 abort 恢复为 `-EFAULT` | `aarch64_user_svc_uaccess` |
| 第 4 块 | 匿名 VMA 与按需缺页：`mmap`/`brk`/`mprotect`/`munmap`、stash、逻辑权限与硬件权限分离 | `aarch64_user_vm` |
| 第 5 块 | 原生 `SYS_fork` 与写时复制：clone 事务、驻留记录所有权、COW 写故障、父子回收 | `aarch64_user_fork_cow` |
| 第 6 块 | 原生 `SYS_waitpid` 与父子退出 / ZOMBIE 回收：共享 `do_waitpid`、孤儿僵尸无锁回收 | `aarch64_user_waitpid` |
| 第 7 块 | 原生 `SYS_exec` 与 SysV 用户栈参数传递：argv/envp 拷贝、`-E2BIG` 合并计数上限、AAPCS 帧与寄存器初始化 | `aarch64_user_exec` |
| 第 8 块 | 原生信号与 `sigreturn`：`kill`/`signal`/`sigprocmask`/`sigreturn`，sigframe 压栈、伪造帧防护 | `aarch64_user_signal` |
| VFS 门（2026-11-18） | arch-neutral fs 栈接入 aarch64 产品内核：devfs / tmpfs / FAT32 / ext2 + virtio-blk-mmio 块设备 + fs syscall 组；`OS01_HAS_VFS` 翻 1 | `aarch64_vfs`（plain selftest 15/15、m4-1 28/28） |

**跨架构 syscall 表**：AArch64 不维护第二张分派表，而是用 `OS01_SYSCALL_A64_SUBSET=1`
编译 `kernel/syscall/dispatch.c` 的**同一张** `syscall_table`，只隐藏未编入的表项。
当前 aarch64 服务 **47 项**：4 项 native（`exit` / `getpid` / `getppid` / `clock_gettime`）
+ 4 项 VM（`brk` / `mmap` / `mprotect` / `munmap`）+ 7 项进程/信号（`fork` / `waitpid` /
`exec` / `kill` / `signal` / `sigprocmask` / `sigreturn`）+ **32 项 fs 组**
（`write` / `read` / `open` / `close` / `dup` / `dup2` / `pipe` / `chdir` / `getcwd` /
`stat` / `fstat` / `lseek` / `fcntl` / `ioctl` / `getdents64` / `access` / `unlink` /
`mkdir` / `rmdir` / `rename` / `truncate` / `ftruncate` / `chmod` / `fchmod` / `poll` /
`ppoll` / `select` / `pselect6` / `symlink` / `readlink` / `lstat` / `fstatat`——
随 `OS01_HAS_VFS` 一起编入，`kernel/syscall/dispatch.c:64-97`）。
net / proc / resource / time / misc 组仍隐藏。详见
[syscall.md](syscall/syscall.md#跨架构共享-syscall-表2026-11-12)。

**当前边界（2026-11-20 更新，如实记录）**：

- **产品用户态已接入（2026-11-19）**：`OS01_HAS_VFS=1`、`OS01_HAS_USERSPACE=1`，aarch64 产品镜像经 boot-5 编排装载 `/bin/init` → `/bin/sh`，支持 `MAP_FIXED`、文件映射、`MAP_SHARED`（fork 后代一致性）与 `/dev/mem` 设备映射。
- **正式 EL0 IRQ 返回与抢占（2026-11-20）**：`aarch64_el0_irq()` 计入 per-CPU IRQ 深度，在设备分派与 EOI 后由正式分支调用 `aarch64_user_irq_return()`（检查拥有帧与 `need_resched` 后调 `schedule()`，并在返回 EL0 前投递挂起信号）。VM 授权/edit 状态按任务持有（`aarch64_vm_task_editing()`），copy window 按 CPU 持有，`switch_to()` 原子完成保存 prev TLS → 切换用户根 → 恢复 next TLS。旧 `aarch64_el0_preempt` 探针仍限单核（`kernel/selftest/test_aarch64_el0.c:414-416`）。
- **8-bit ASID 与失效边界（2026-11-20）**：`user_asid.c` 维护 1..255 非零标签池（0 为退化回退），`TCR_EL1.A1=0, AS=0`，ASID 编码于 `TTBR0[55:48]`（CnP=0），用户叶带 `nG` 位。BSP 与 AP 在接入调度前均安装共享空 TTBR0 根（内核与设备地址走 TTBR1）；活跃低 VA 使用 `tlbi vaae1is`，标签回收先完成 `tlbi aside1is` 再归还池位。
- **固定单核亲和 SMP 用户调度（2026-11-20）**：PID 1 固定 BSP（boot-5 特例），运行期 fork/spawn 通过 `aarch64_user_pick_cpu()` 在已就绪 CPU 上轮转放核（`sched_alloc_and_publish_task`），任务终身固定单核亲和（**不做运行期跨核迁移**）。配套完成两阶段孤儿退出交接（`task_finish_switch` / `task_publish_process_exit`）、COW 末引用释放顺序、原子信号 pending、共享 `FD_VFS` `file_t` pos 锁与 `ic ialluis` 跨核同步；产品镜像已通过 1/2/4/8 核启动与 2 核串口交互验收（`MODE=user-runtime USER_RUNTIME_CASE=irq|asid|smp|product`）。
- **仍保留的边界**：EL0 浮点指令仍会陷入（FPEN 已放行、per-task FP 保存恢复机制已就位，但 EL0 任务 PSTATE 的 F 位仍掩码 `0x340`，且无 EC 0x2C/0x2D 陷入处理路径；signal 帧也无 FP 区）；用户态进入/返回**尚未纳入 `arch/context.h` 门面**（x86 走 `iretq`、aarch64 走 `eret`）；`arch_cpu_features()` 仓库内仍无此接口；`arch_smp_boot_aps()` 已于 2026-11-19 随统一 SMP 启动闭环落地。

**剩余差距**：`arch/context.h` 用户态进入/返回门面、运行期跨核任务迁移、EL0 FP/SIMD 剩余项（PSTATE F 位与 signal 帧 FP 保存恢复）与跨进程 inode 级 `MAP_SHARED`。规划项见 [roadmap.md](roadmap.md) P2。
闭环记录见 `docs/changelog.md` 2026-11-11 ~ 2026-11-20 各条目；COW/VM 细节见
[cow-mmap.md](memory/cow-mmap.md#aarch64-cow--匿名-vma2026-11-11--2026-11-13-闭环)。

---

## aarch64 平台值解绑（2026-11-18 ~ 2026-11-19）

真机适配审计（路线图 P2 §真机适配审计）把 aarch64 的 QEMU virt 固化分三层。
**2026-11-18 落地了加载地址层与 DTB 地址层，2026-11-19 落地了早期 MMIO Device 窗口与控制台 handoff 运行时化**。

### 启动块平台配置化（加载地址层）

原先 `0x40000000` / `0x40080000` 固化在 6 个文件里互为同值耦合。现在收敛为
**一次构建、一处配置**：

| 项 | 位置 | 说明 |
|---|---|---|
| 唯一可配置输入 | `AARCH64_BOOT_BLOCK_BASE`（`mk/profiles/aarch64-clang.mk:22-28`） | 不设置即 QEMU virt 默认 `0x40000000`；显式置空是错误 |
| 构建期校验 | `mk/profiles/aarch64-clang.mk:39-42` | 十进制/`0x` 十六进制整数、2 MiB 对齐、M0 范围 `[0,0x80000000)`、与设备窗口 D 不相交、留得下 ≥20 MiB 早期 arena；非法即**构建期报错**，不掩码纠正、不回落默认 |
| 单一常量源 | `kernel/include/arch/aarch64/boot_layout.h:25-44` | 裸预处理常量（无 C 类型/无 `ULL`），内核 C、预处理汇编、预处理链接脚本三处展开一致；其余区间全部由 BASE 派生 |
| 链接脚本 | `kernel/arch/aarch64/linker.ld:27,45,51,157-158` → `linker.generated.ld`（`kernel/Makefile:670-689`） | 带 cfg stamp，避免「默认→改址→默认」增量构建漏重编 |
| 消费方（四处联动） | UEFI loader `boot/uefi/arch/aarch64/{elf.c,boot.c}`；M0 建表 `kernel/arch/aarch64/boot/early_mmu.c`；M1 建表 `kernel/arch/aarch64/memory/runtime_tree.c:93-96`；RAM 保留 `kernel/arch/aarch64/memory/ram_core.c:442-443` | 同一布局驱动四处 |

AArch64 `head.S` 负责 BSP 从固件交接到 `kernel_main`：建立 EL1、栈和早期向量，清零
`.boot.bss`，调用低地址、无外部调用的 M0 构造器，再安装并启用 MMU。PSCI 次核入口、
早期向量与诊断、高半区到低物理符号的地址桥分别在 `boot/secondary_entry.S`、
`boot/early_vectors.S` 和 `boot/boot_symbols.S`。

**fail-closed 契约**：loader 在分配任何内核段之前校验 ELF——`e_entry` 必须等于配置入口，
否则以可区分的诊断 `UEFI-A64: kernel entry mismatch` 拒绝（`elf.c:100-101`、`boot.c:305-306`）；
内核侧同样 fail-closed 取证 `UEFI-A64: layout entry=… context=…`
（`kernel/arch/aarch64/boot/main.c:394-415`）。trampoline 地址改由寄存器传入
（`handoff.S:4-9` 的 `br x2`），并用 `_Static_assert` 保证与布局常量一致（`loader.h:20-22`）。

**验收**：`make PROFILE=aarch64-clang test-aarch64 MODE=lma-layout`——默认布局 → 改址布局
→ 再回默认的配对启动，外加「默认 loader 搭替代 ELF」的混搭拒绝。

**已知边界**：**真机启动未验证**（只验证改址构建与 QEMU virt）；handoff 窗口仍是固定 128 KiB 固定址申请
（`boot.c:246-248,460-461`），真机大 DTB 仍有 `EFI_BUFFER_TOO_SMALL` 风险。

### 早期 MMIO Device 窗口与控制台 handoff（2026-11-19）

- **handoff ABI**：`boot_context` 追加 flag 门控 `struct BOOT_EARLY_MMIO`（`console_base`/`device_base`/`device_blocks`，v3 追加不 bump version）。UEFI loader `fdt_early.c` 从固件 FDT 推导 PL011 + GICv2 全部 `reg` 对并转为 2 MiB 块覆盖（上限 1024 块）。
- **内核侧消费**：`head.S` 停靠 handoff 并 fail-closed（窗口出 `[0, 2 GiB)` 或撞启动块 → `W!`）；`head.S`/`boot_vectors.S` 的早期打印改读停靠槽（`early_mmio.console_base`，flag 缺失回退 `0x09000000`）；`boot/early_mmu.c` 按窗口参数填 Device 块；`device_window.c` 提供运行时窗口供 `early_arena.c` preflight、`runtime_tree.c` 与 PT/M0 selftest 消费（`boot_layout.h` 常量仅作 flag 缺失时的回退）。
- **已知边界**：早期 identity map `[0, 2 GiB)` 上限不变，窗口/控制台超过 2 GiB 仍需早期映射扩展；单窗口表示不适配远距设备对（loader 不置 flag）。

### DTB 地址层解绑

DTB 解析器此前实为「QEMU virt 形状校验器」——解析出的基址先和 QEMU 常量对账、相等才通过。
现在改成解析出就用：

- **PL011 MMIO 基址（4/4 已清零）**：运行期消费 `dtb_pl011_base()`
  （`kernel/arch/aarch64/platform/pl011.c:30-36`，DTB 尚不可用时回退
  `PL011_EARLY_FALLBACK_PA`）；解析器接受任意基址（`dtb_parse.c:144-158`）；
  **UEFI loader 侧已完全不含 PL011 硬编码**，改优先走 EFI Serial IO Protocol
  （`boot/uefi/arch/aarch64/boot.c:52-69,93-118`）；`head.S`/`boot_vectors.S`
  的 MMU 前 fault 打印已于 2026-11-19 改读 `early_mmio.console_base` 停靠槽。
- **PL011 波特率**：改由 DTB `clock-frequency` / `clocks` 推导
  （`pl011_calc_divisors()` `pl011.c:63-77`，调用点 `pl011.c:93`，DTB 侧
  `dtb_parse.c:111-119,159-171`）。**DTB 无时钟时回退 16 MHz**；残留的 IBRD=8/FBRD=44
  仅作除数越界兜底。
- **GIC 基址**：不再要求 GICD/GICC 必须等于 0x08000000/0x08010000（只拒 `base==0`），
  compatible 白名单补 `arm,gic-400`（`dtb_parse.c:133-143`）；运行期 `gic_init` 消费
  `dtb_gicd_base()` / `dtb_gicc_base()`。

**已知边界**：PL011 interrupts 形状校验仍要求恰 3 cell + LEVEL_HIGH
（`dtb_parse.c:152-155`）；**GICv3（`arm,gic-v3`）仍完全不支持**。

### 顺带更正

2026-11-17 审计稿把「无 RNDR 接线」写进了路线图的平台能力上限段——**该说法是错的**：
RNDR/RNDRRS 早在 2026-09-25 已接线（`kernel/arch/aarch64/cpu/random.c:30-60,110-122`，
经 `ID_AA64ISAR0_EL1.RNDR` 探测，输出 STRONG/WEAK/NONE）。路线图已更正；
`boot/uefi/arch/aarch64/boot.c:467-471` 的同款陈旧注释仍留在代码里。

闭环记录见 `docs/changelog.md` 2026-11-18 各条目；设计依据见
[spec](superpowers/specs/2026-11-17-aarch64-kernel-lma-design.md) 与
[plan](superpowers/plans/2026-11-17-aarch64-kernel-lma.md)；剩余项见
[roadmap.md](roadmap.md) P2 §真机适配审计。

---

## 启动阶段契约（BOOT-4，2026-11-08 闭环）

BOOT-4 把「同一启动阶段内的子系统先后顺序」从链接顺序的副作用变成**可核验的显式契约**，
并在 x86_64 与 aarch64 之间共用同一套阶段定义：

- **命名阶段常量**：`SUBSYS_PHASE_CONTROLLERS=3` / `SUBSYS_PHASE_TIME=4` /
  `SUBSYS_PHASE_DEVICE_IRQ=5` / `SUBSYS_PHASE_STORAGE=6`（`kernel/include/subsys/subsys.h`）。
  裸数字 phase 字面量已弃用。
- **依赖声明与拓扑预检**：`register_subsys_deps()` 最多声明 8 项依赖；首次阶段调度前
  执行全局预检 —— 缺失依赖 `-ENOENT`、循环依赖 `-ELOOP`（上报首个成环组件）；同阶段内
  按拓扑序稳定调度，无依赖项保持注册序。
- **per-CPU 执行位置**：`subsys_prepare_cpus` **仅在 BSP 执行**（`executor=BSP`），只做
  通用软件数据结构分配，**不做本核硬件初始化**；原先注册但从未执行的
  `_lapic_timer_start_percpu` 与 `kernel/arch/x86_64/smp/subsys_percpu.c` 已删除。
- **对称强覆盖钩子**：粗粒度的 `arch_boot_subsystems` 被拆成
  `arch_boot_controllers` / `arch_boot_device_nodes` / `arch_boot_percpu` 三个对称钩子，
  配三个公共能力检查点，由编排层统一核验。

两架构各 14 组隔离编排矩阵全 PASS。详见
[验证报告](superpowers/reports/2026-11-08-boot-4-validation.md) 与
[subsys.md](subsys/subsys.md)。

---

## 设计依据

- 详细 spec：`docs/superpowers/specs/2026-09-09-pmm-arch-neutral-design.md`（13 轮 subagent review 通过）
- 实施 plan：`docs/superpowers/plans/2026-09-09-pmm-arch-neutral.md`（3 轮 subagent review + 16 task + final fix）
- v25 arch-cleanup 系列 10 commits `67132e2..3ab4ef1`：roadmap v24 doc；bootinfo(arch) E820 → `bootinfo_x86.h`；arch(neutral) `arch/regs.h` pt_regs_t facade + `rwlock_relax()` 走 `arch_cpu_pause()`；intr(arch) `arch_irq` hooks 拆分；rtc(arch) core + per-arch impl 拆分；mm(arch) PGD/PUD/PMD/PTE 层级统一 + bit-constant rename；arch(subsys) `SUBSYS_INITCALL()` + `.subsys_init` section；arch(sched) `arch_kernel_thread_entry`；build(uefi) digest + staged copy 排除 `*.o/*.a/*.lib`；merge
- x86_64 启动细节：见 `docs/architecture.md` + `docs/boot/boot.md`
## aarch64 AP 协作调度 P2

AP沿用已录用CPU的单向task-stack交接。`arch_sched_notify_remote()`在
need_resched发表后通过GICv2 SGI4通知远端；SGI3仍归TLB shootdown。
SGI handler只设置标志，公共GIC dispatch写回完整raw IAR作一次EOI。
`task_wake()`在rq锁内捕获目标，解锁后通知；blocker元数据由task-list锁
保护，state快照另外取得rq锁，callback不持rq锁，清理blocker后才发表唤醒。

idle在IRQ开启时消费ap_work，屏蔽IRQ并检查rq/READY；无工作时释放锁，
保持IRQ屏蔽执行`dsb sy; wfi`，返回后才开启IRQ并重检。ap_work的READY发表后
无条件kick，只依赖ipi_ready，避免scheduler_ok交接时省略通知。BSP仍独占
全局时间基准（jiffies/poll timeout/timer list/blocker scan）；AP不推进全局
时间基准，但已录用AP拥有私有的100 Hz CNTP周期中断（见下节）。默认线程留BSP，
显式pinned线程可在AP运行；尚无IRQ返回抢占、迁移、EL0或用户MM。

## aarch64 每核调度 tick 记账

每个已录用CPU（含BSP）拥有私有的CNTP周期中断，**名义频率100 Hz**。该中断只
做三件事：取本核记账样本、把本核运行任务的vruntime推进1格、在BSP上请求一次
协作式reschedule。它**不是**全局时间基准：jiffies、poll timeout、timer list
（TIMER_SIRQ）与blocker scan仍由BSP独占。

采样语义是**一计真实递送的CNTP中断 = 一个样本**，不做墙钟补算；丢掉的tick
不会被事后补上。因此100 Hz是名义值，实际样本数以真实递送的中断数为准。

记账入口是arch-neutral的`sched_account_tick()`：中断时若当前任务是该核的
idle任务则计idle样本，否则计busy样本并给该任务累加一个runtime tick。
`update_curr()`在aarch64上改由本核tick驱动（每tick +1 vruntime），公共
`schedule()`不再重复计费——否则同一tick会被计两次，且vruntime会依赖yield
频率而不是经过时间。

CNTP配置在启动期**冻结**并由BSP一次性发表（`aarch64_timer_publish_config()`
注册handler并发表配置，AP用acquire读并校验），AP随后只arm自己的计数器
（`aarch64_timer_start_local()`）。契约：频率必须≥100 Hz，period=freq/100
且落在`1..INT32_MAX`，PPI落在16..31，AP频率与BSP不一致必须失败；arm失败后
清CTL的ENABLE位。

中断路径保持EL1协作式：tick handler与IRQ返回路径都**不得**调用`schedule()`，
只发布记账并（仅BSP）置`need_resched`。BSP每个有效tick（含idle tick）
release-store `need_resched=1`，以便空runqueue也经由普通协作入口走到blocker
扫描；AP的idle tick不无条件请求调度，只有本核繁忙任务越过deadline时才置位。

锁序：usage lock（`sched_account_tick()`内部取用并释放）与rq lock**从不嵌套**，
tick不取`task_list_lock`。

验收面：`MODE=sched-tick`（`AARCH64_SCHED_TICK_TEST=1` → variant `sched-tick`）
在每个已录用CPU上发布七个冻结case（tick-ready/tick-idle/tick-busy/
tick-yield/tick-deadline/tick-global/tick-remote），runner
`qemutests/aarch64_sched_tick.py` 按冻结字段与跨字段关系严格解析，1/2/4/8核
各两次。证据在被描述的CPU上采集：idle窗口在目标自己的idle上下文测量，busy/
yield/deadline/global窗口在目标核的pinned worker上测量，remote窗口让每个CPU
都成为一次真实SGI4的目标。详见
[验证报告](superpowers/reports/2026-11-06-aarch64-percpu-tick-accounting-validation.md)。

仍为EL1协作式：**没有IRQ返回抢占**，没有迁移，没有EL0或用户MM。

## 启动失败与诊断日志契约 (BOOT-3)

通用启动错误与致命退出逻辑位于 `kernel/core/boot_failure.c`。
- **AArch64 mini-formatter 兼容约束**：AArch64 early console/logger mini-formatter 仅保证稳定支持 `%s`，不保证通用整数说明符。因此 `boot_failure_stop` 与 `boot_failure_warn` 在栈上使用无堆分配的专用十进制格式化（带负号与 `INT64_MIN` 正确处理），生成固定栈缓冲区后统一通过 `%s` 传入 `log_err` / `log_warn`。
- **HALT 边界**：致命退出直接调用 `arch_boot_fatal_halt()`（x86_64: cli + hlt 循环；aarch64: wfi 循环），不依赖已初始化的调度器，不在致命路径产生二次锁竞争或内存分配。

