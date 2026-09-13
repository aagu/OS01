# OS01 优化路线图 v26

> **基准**: `f2085c3` (master, 2026-09-13)
> **日期**: 2026-09-13
> **变更**: 同步 v25 之后的实际进展（`a274f6f..f2085c3`，两大系列 + 若干加固）：① **aarch64 页表原语**（spec `2026-09-11-aarch64-page-table-primitives-design.md`，`169e0d5` + 修复 `c9c0246`/`cdb6625` + host smoke `466401d`/`28ac0ea`）——`kernel/arch/aarch64/page_table.c` 提供 arch-local 页表 walk/map 原语（descriptor bit 修正 + intermediate entry 用 minimal descriptor），配套 QEMU 级 smoke 断言，为 P2 `head.S + MMU` 铺路；附带两个 aarch64 PMM 修复（`1c51dfa` 多 zone `alloc_pages` 索引、`053a226` direct map 覆盖 PMM 分配范围）。② **目录重构 P1–P6**（merge `31bb128`，spec `2026-09-12-directory-restructure-design.md`）——test 目录改 `hosttests/qemutests/kernel-selftest/runtime-selftest`（P1）、`kernel/kernel/ → kernel/core/`（P2，字节相同）、random/log/font/logo 拆出 core/（P3）、headers 按 subsystem 拆分 + `arch/` lift 出 `include/kernel/`（P4）、pic/timer 归属 owning subsystem + pty 迁移（P5）、P6 清理 + 全量文档同步（`6730681`）+ master 新 PMM 测试移植（`11695b6`）。③ **PMM/sched 稳定性系列**（worktree `pmm-three-fixes`，merge `be7db4f`）——`ea89136` MEMORY_RANGE_GRANULE 32→64-bit、`beb351c` 保留 E820 MEMORY_TYPE、`0ba888a` alloc/free RAM-relative 索引、`0809100` boot/slab 帧预留 RAM-relative、`514e062` 调度 lost-wakeup 窗口修复、`b68e1b1` 6 例 PMM host 测试；另有 `4468e75` `find_mount` 防御（user-pointer mount entry → dump + ENOENT；**底层根因 devfs mount entry 0x600000 被覆写仍未定位**，疑似 boot slab 帧被 e1000 TX ring 复用，硬件 watchpoint 已取证，parked）。④ **SMP=4 boot crash 双 bug 修复**（`1506d6e` large-model orphan sections 进 `_end` 前、`36eb6a3` active PGD lifetime 与 CR3 同步）+ `tests/x86_64_systest_repeat.py` 回归。⑤ **文档学习层**（`2d289d3`/`414d022` docs/README 索引 + 调度器/trap.c/vfs+memory/tty+intr 4 份源码阅读指南）。v25 的 arch-cleanup 明细（PGD/PUD/PMD/PTE 统一等 10 commits）见 `git log 67132e2..3ab4ef1`。

标记: ✅ 已完成 | 🔒 P1 安全加固 | 🏗 P2 aarch64 适配 | 🖥 P3 GUI | 🔧 P4 硬件适配 | 📐 P5 ABI 扩展/兼容性

---

## 当前状态总览（9 个 Phase 全部就绪 ✅）

| Phase | 说明 | 状态 |
|-------|------|------|
| **Phase 1: COW + 内存** | Copy-On-Write Fork, mmap/mprotect/munmap, demand paging | ✅ |
| **Phase 2: 内核基础设施** | arch 抽象层、子系统注册框架、x86 平台源隔离、aarch64 dispatch 桩、SMP（percpu+GS-base+AP boot+负载均衡）、canary、hang detector、debug channels、kallsyms、FPU 保存、slab/PMM/softirq/timer SMP 加固、**profile 化 GNU Make 构建体系**（`make PROFILE=<name>` + 自管 sysroot generation + host-tool mkdisk + make help capability 标签）、**profile-only UEFI overlay 简化**（x86 固件 per-profile + UEFI runtime 补丁删除）、**自托管 compiler runtime**（udivti3 + provider-keyed archive + kernel link publication + kernel runtime validation targets）、**aarch64 UEFI bootloader 统一**（x86_64+aarch64 共享 `boot/uefi/main.c` + `boot_context` handoff ABI）、**PMM arch-neutral**（`pmm_init(const struct boot_context*)` 单一入口 + `MEMORY_RANGE[]` 中介 + 弱默认/强覆盖 dispatch + RAM-relative indexing，aarch64/x86_64 复用同一 body，2 MiB granule 统一）、**log API arch-neutral**（gate-wrapped `_log_*_impl` 宏 + `_log_writev` va_list core + per-arch impl：x86_64 vsnprintf/串口，aarch64 `kputs(fmt)` 忽略 variadic）、**mm(arch) PGD/PUD/PMD/PTE 页表层级统一**（Linux/ARM 命名替换 x86_64 PML4/PDPT/PDE，`mm->pml4 → mm->pgdir`，bit-constant 同步 `PAGE_VALID/USER/WRITE/HUGE/NO_EXEC/GLOBAL/CACHE_DISABLE/WRITE_THROUGH`，~150 站点 rename 字节相同 1,739,024 B）、**bootinfo(arch) E820 拆出 `bootinfo_x86.h`**（arch-neutral `bootinfo.h` 不再泄漏 `struct E820_ENTRY`/`BOOT_MEMORY_FORMAT_E820`，aarch64 编译视图纯净）、**arch(neutral) pt_regs_t facade + rwlock 用 `arch_cpu_pause`**（新增 `arch/regs.h` 派发 facade + 替换 rwlock 本地 `#if __x86_64__/__aarch64__` switch 为共享 helper）、**intr(arch) `arch_irq` hooks 拆分 controller selection + dispatch**（`arch_irq_select_controller`/`arch_irq_gsi_to_vector`/`arch_irq_vector_to_gsi`/`arch_irq_dispatch` 四 hook + 弱默认 FATAL/identity/no-op + x86_64 强覆盖 APIC→PIC ladder + 0x20+gsi 翻译；do_IRQ 移出 `intr/pic/8259A.c`；`unregister_irq` 从 vector 改 gsi 跟 `register_irq` 对齐）、**rtc(arch) core + per-arch impl 拆分**（`driver/rtc.c` 只走 `arch_rtc_read/write` hook，x86_64 强覆盖 `rtc_cmos.c` + `rtc_pie.c`，新 `arch/x86_64/rtc.h`）、**arch(subsys) 平台自决 driver 注册**（`SUBSYS_INITCALL()` 宏 + `.subsys_init` linker section 替代 `arch_register_subsys()` 硬编码列表，10 driver 自注册，平台 glue 缩到 7 行 loop）、**arch(sched) arch-neutral `kernel_thread_entry`**（弱默认 panic-on-call + x86_64 强覆盖，`task.c` 不再 include per-arch 头）、**build(uefi) 排除 `*.o/*.a/*.lib`**（digest `! -name` + staged `cp -a` 后 `find -delete`，根治 worktree posix-uefi 残留污染 aarch64 BOOTAA64.EFI 链接） | ✅ |
| **Phase 3: 信号 + 调度** | arch 信号帧投递、进程组/会话（setpgid/setsid/getpgid/getsid 67-70）、tty 行规程（VINTR/VQUIT→SIGINT/SIGQUIT）、SYS_kill 支持 pid=0/-pid/-1、per-CPU EEVDF rbtree 可运行队列、SMP 负载均衡 | ✅ |
| **Phase 4: 文件系统** | ext2 R/W、FAT32 R/W、tmpfs、devfs、procfs、GPT 双分区 | ✅ |
| **Phase 5: 设备驱动** | 8259A PIC、APIC/IOAPIC/LAPIC、PIT/LAPIC timer、PS/2 键盘、16550 串口、AHCI SATA | ✅ |
| **Phase 6: 用户态** | busybox ash shell（方向键行编辑+光标闪烁+行规程 TTY）、52 applet（见 `docs/applet-verification.md`）、init（/etc/inittab 配置解析、4 阶段引导）、libc、VT100 CSI 终端模拟器 | ✅ |
| **Phase 7: poll/select** | poll_table + 双队列级联唤醒、select/pselect、do_poll_core 共享、pselect6 sigmask 原子 swap、requested-event-aware 注册、per-poll timeout registry | ✅ |
| **Phase 8: 网络** | lwIP 2.2.1、E1000 + virtio-net、PCI/MSI-X、DHCP/DNS、TCP/UDP socket、poll/select 集成、BusyBox HTTP wget、自动化网络回归 harness | ✅ |
| **Phase 9: 时间系统** | clocksource + clockevent 双层抽象、TSC 频率校准、LAPIC 周期 tick 接管、CLOCK_MONOTONIC/REALTIME + nanosleep + poll/select 迁纳秒 | ✅ |

> 各 Phase 的实施细节、commit 记录、决策与经验见下方「相关文档」。

---

## 待实施路线图（v22 按 5 优先级）

> 优先级框架（用户确认，2026-08-18）：**P0 工程基础** → **P1 安全加固** → **P2 aarch64 适配** → **P3 GUI** → **P4 硬件适配** → **P5 ABI 扩展/兼容性**

### P0 工程基础（✅ 已完成）

| 项 | 内容 | 状态 |
|----|------|------|
| 文档同步 | `docs/syscall.md` 71 syscall（0..70）、`docs/timer.md` 新架构、`pit-200hz-handoff.md` 状态 | ✅（ba56d34） |
| applet 验证清单 | 52/52 编译进 busybox；详见 `docs/applet-verification.md` | ✅（2026-08-18） |
| roadmap 瘦身 | 已完成内容迁出到 `docs/` 专题文档（见下） | ✅（2026-08-23） |
| GNU Make profile 重构 | profile 化构建入口（`make PROFILE=<name>`）+ profile-aware Make 接口 + 自管 sysroot generation（原子发布）+ UEFI/image 产物按 profile 隔离 + host-tool mkdisk + capability-aware 入口校验（缺少能力的 profile 在解析期 fail-fast）+ profile-only cleanup contract + make help capability 标签 | ✅（e567394，15 commits `26920ad..e567394`，2026-09-02） |
| profile-only UEFI overlay 简化 | x86 UEFI 固件 per-profile（不再用运行时 overlay patch）+ host test 按 profile 隔离 + 所有组件强制声明 profile + profile-only UEFI cleanup contract | ✅（T1–T5 landed，2026-09-03，T6 docs/wget flag 待办） |
| 自托管 compiler runtime | udivti3 实现 + provider-keyed selfhosted archive + provider 构建不变量硬化 + 内核链接 compiler runtime + kernel link publication 加固 + compiler-rt eligibility 验证 + kernel runtime validation targets + syscall/selftest suite 隔离 + variant link paths + root `make sysroot` 入口 | ✅（多 commits 2026-09-04/05，详见 `runtime/` + `docs/build.md`） |
| aarch64 UEFI bootloader 统一 | x86_64 + aarch64 共享 `boot/uefi/main.c` + arch 分发 + `boot_context` handoff ABI + boot_context 头部偏移断言 | ✅（commits `af166bc`..`06e6127`，merge `06e6127`） |
| aarch64 UEFI 固件修复 | firmware 截断 64MiB 适配 QEMU pflash（`11aa6ed`）+ aarch64 UEFI 默认 URL 下载（`bad8825`）+ aarch64 也显式传 clang+lld 到 posix-uefi（`25872d1`） | ✅（2026-09-03） |
| 目录重构 P1–P6 | test 目录 → `hosttests/qemutests/kernel-selftest/runtime-selftest`；`kernel/kernel/ → kernel/core/`（字节相同）；headers 按 subsystem 拆分 + `arch/` lift 出 `include/kernel/`；random/log/font/logo 拆出 core/；pic/timer 归属 owning subsystem + pty 迁移；P6 清理 + 全量文档同步 + PMM 测试移植。目录约定见 spec `2026-09-12-directory-restructure-design.md` 与 `docs/structure.md` | ✅（merge `31bb128`，2026-09-12） |
| PMM/sched 稳定性系列 | MEMORY_RANGE_GRANULE 64-bit + E820 MEMORY_TYPE 保留 + alloc/free/预留 RAM-relative 索引（3 个独立 PMM 隐患，每个先 RED 后 GREEN）+ 调度 lost-wakeup 窗口修复 + 6 例 PMM host 测试 + `find_mount` 防御（user-pointer mount entry → ENOENT）。SMP=1/2/4 均通过，systest-repeat 7 连 268/268 | ✅（merge `be7db4f`，2026-09-12；`1506d6e`/`36eb6a3` SMP=4 boot 双 bug 修复同批落地） |
| 文档学习层 | `docs/README.md` 索引 + 调度器/trap.c/vfs+memory/tty+intr 源码阅读指南 | ✅（2026-09-13） |

### 🔒 P1 安全加固

依赖链：`getrandom → AT_RANDOM → 用户栈 canary / ASLR`；UBSan/KASan 编译期独立。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| getrandom syscall ✅ | **已完成**（SYS_getrandom=66，ChaCha20 池 + RDRAND/RDSEED 熵源 + 周期 reseed，`/dev/urandom` 同源）。详见 `docs/syscall.md`。`LWIP_RAND`/AT_RANDOM 种子改用仍待做 | 独立 | Linux getrandom(2) |
| x86_64 内核栈保护 ✅ | **已完成**：x86_64 内核使用 `-fno-pic -mcmodel=large -fstack-protector-strong`，全局 per-boot guard；审计直接 `R_X86_64_PC32` 或 `R_X86_64_64` guard 引用并拒绝 GOT；编译门控的 QEMU 破坏性 canary trip test 已打印预期诊断。用户态 canary/`AT_RANDOM` 仍待做 | 独立 | Linux SSP |
| **统一用户态启动方式** | `task.c` 两处用户栈构造（spawn argv-only / exec argc+envc）提取共享 `setup_user_stack()`（一处 auxv 逻辑，消除双站点漂移）；`crt0.S` 从寄存器传参（rdi/rsi/rdx）改为标准 SysV `_start`——从 `[rsp]` 解析 argc/argv/envp/auxv 传 `__libc_start_main`（libc/csu 已有但从未被调用），并正确设置 `environ`；busybox overlay crt0 同步。**为 P1 后续（AT_RANDOM/用户栈 canary/ASLR）与 P5 动态链接铺路**：musl crt1 只认栈布局不认寄存器，每个新 auxv 条目在双站点下成本 ×2。验收：systest 268/268 + busybox applet 无回归 | 独立 | musl/glibc crt1 |
| 用户栈 canary | libc `-fstack-protector-strong` + ELF 加载器 AT_RANDOM auxv 传种子（原 P1#5）。**前置：统一用户态启动方式 ✅ 后在单站点压 AT_RANDOM**；同批把 `LWIP_RAND` 换内核熵池 | 统一启动方式, getrandom | |
| ASLR | mmap 基址随机化 + ET_DYN/PIE 加载随机化（原 P3#12） | getrandom | |
| UBSan + KASan | 内核编译期 instrument（原 P3#13） | 独立 | ArvernOS |
| syscall 边界审计 ✅ | **已完成**（2026-08-24，commits `a1ad1b9`..`80eab1a`，11 commits）。详见下文「Syscall 边界审计实施总结」 | 独立 | |
| 堆加固 | malloc double-free/溢出检测 | 独立 | |
| NX 页 | 栈/堆不可执行 + mmap PROT_EXEC 审计 | 独立 | |
| **exec 软链接跟随** ✅ | **已完成**（2026-09-06，commits `43588c8`..`c64c854`）。`__vfs_lookup_raw` 早返中段/末段 symlink；`vfs_lookup_at` fold walk + splice + restart（≤ MAXSYMLINKS=8）；NOFOLLOW 仅禁止末段（POSIX）；sys_stat/open/chdir/exec 均已改走 `vfs_lookup_at`。spec §5.3 v3/v4/v5 修复全部应用。~~Parked follow-up~~：`__vfs_lookup_raw` 的 `consumed` 路径 mount prefix 播种**已修**（`b0c95e3`，2026-09-13 核实）。 | 独立 | Linux do_filp_open |

### 🏗 P2 aarch64 适配

前置：**rwlock/seqlock**（多核并发正确性，VFS/`/proc` 多核缩放，SMP 基础）。

已有基座：arch 抽象层 ✅、dispatch 桩 ✅、平台源隔离 ✅、aarch64 spinlock（ldxr/stlxr）✅、clocksource/clockevent 接口 hook ✅、**PMM arch-neutral ✅**（`pmm_init(boot_context)` 单一入口 + `MEMORY_RANGE[]` 中介 + 弱默认/强覆盖 dispatch + RAM-relative indexing；x86_64 systest 268/268 + nettest 6/6 零退化，aarch64 uefi-smp 9/9 PASS）、**log API arch-neutral ✅**（gate-wrapped `_log_*_impl`：x86_64 走 `_log_writev`/vsnprintf，aarch64 走 `kputs(fmt)`）、**boot_context v2 handoff ABI ✅**（arch 中立）、**mm(arch) PGD/PUD/PMD/PTE 层级统一 ✅**（Linux/ARM 命名替换 PML4/PDPT/PDE，bit-constant 同步 arch-neutral，~150 站点 rename 字节相同 1,739,024 B）、**bootinfo(arch) E820 拆出 ✅**（`struct E820_ENTRY`/`BOOT_MEMORY_FORMAT_E820` 移到 `arch/x86_64/bootinfo_x86.h`，aarch64 编译视图纯净）、**arch(neutral) pt_regs_t facade + arch_cpu_pause ✅**（`arch/regs.h` facade 派发 + `rwlock_relax()` 走共享 helper）、**intr(arch) `arch_irq` hooks 拆分 ✅**（四 hook + 弱默认 FATAL/identity/no-op + x86_64 强覆盖；do_IRQ 移出 `intr/pic/8259A.c`；`unregister_irq` 改 gsi）、**rtc(arch) core + per-arch impl 拆分 ✅**（driver/rtc.c 走 `arch_rtc_read/write` hook，x86_64 强覆盖 `rtc_cmos.c` + `rtc_pie.c`）、**arch(subsys) 平台自决 driver 注册 ✅**（`SUBSYS_INITCALL()` + `.subsys_init` section，10 driver 自注册）、**arch(sched) arch-neutral `kernel_thread_entry` ✅**（弱默认 panic-on-call + x86_64 强覆盖）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| rwlock/seqlock | 基础原语 ✅；VFS mount/lookup ✅；`/proc` 读路径未纳入本次范围（原 P1#4 提为前置） | 独立 | |
| **aarch64 页表原语** ✅ | **已完成**（2026-09-11/12，spec `2026-09-11-aarch64-page-table-primitives-design.md`，`169e0d5` + `c9c0246` descriptor bit 修正 + `cdb6625` intermediate entry minimal descriptor + `466401d`/`28ac0ea` QEMU smoke 断言）。`kernel/arch/aarch64/page_table.c` arch-local walk/map 原语，为 head.S + MMU 铺路。附带 aarch64 PMM 修复：`1c51dfa` 多 zone `alloc_pages` 索引、`053a226` direct map 覆盖 PMM 分配范围 | 独立 | ArvernOS |
| head.S + MMU | 启动入口 + TTBR0_EL1/页表（**页表原语已就绪 ✅**，下一步是把原语接到 aarch64 `main.c` 建立内核恒等映射 + user 页表） | 页表原语 ✅ | ArvernOS |
| GICv2 驱动 | 中断控制器 | head.S | opuntiaOS |
| Generic Timer | cntvct_el0 读数 + CNTP 周期定时器（clockevent hook 已预留） | head.S | opuntiaOS |
| 交叉编译链 | aarch64-linux-gnu-gcc + QEMU virt 平台 | 独立 | |
| UEFI 启动链 | 共享 boot/uefi/main.c，aarch64 通过 PSCI 启动 AP；DTB handoff 副本固定 `[0x401e0000,0x401ff000)` | 独立 | |
| SMP 验证 (UEFI PSCI) | QEMU virt/Cortex-A53/GICv2 下 BSP+AP 独立栈+TPIDR+异常向量+每核 GIC interface；共享 spinlock 计数 1/2/4 核 ×3 验证；故障注入 `AARCH64_SMP_TEST_NO_ACK_CPU=1` 验证降级恢复 | GIC、UEFI 链 | |
| **PMM arch-neutral** ✅ | **已完成**（2026-09-09，17 commits `2c08e78..a2e7389`）。`pmm_init(const struct boot_context *ctx)` 单一入口 + 弱默认 `pmm_arch_normalize`/`pmm_arch_zone_split` 在 `kernel/memory/pmm_arch.c`，x86_64 强覆盖在 `kernel/arch/x86_64/pmm_arch.c`（E820 + kernel-LMA/handoff/trampoline excludes + 2 MiB granule + sort/merge），aarch64 强覆盖在 `kernel/arch/aarch64/pmm_arch.c`（读 `aarch64_ram_map_get()`）。`pmm.c` body 用 RAM-relative indexing（`pages_struct + ((start - lowest_ram) >> 21)`），Step 7 clamp 防 aarch64 unsigned-underflow。详见 spec `docs/superpowers/specs/2026-09-09-pmm-arch-neutral-design.md`（13 轮 review 通过）+ plan（3 轮 review + 16 task + final fix 通过）。**Follow-on 已修**：非-RAM 类型写入 `out[].type` 由 `beb351c`（2026-09-12）完成，E820-derived type 完整保留到 `MEMORY_RANGE[]`。 | 独立 | |
| **log API 统一** ✅ | **已完成**（2026-09-09）。`kernel/log.h` 提供 gate-wrapped `log_err/warn/info` 宏（`do { if (LEVEL <= g_log_level) _log_*_impl(__VA_ARGS__); } while(0)`），保留 `log()` core macro 和 `g_log_level`/`log_set_level`/`log_get_level` 调度；`_log_write` 拆为 variadic forwarder + `_log_writev` va_list core；x86_64 走 `_log_writev`/vsnprintf 串口，aarch64 走 `kputs(fmt)` 忽略 variadic（-nostdlib）；aarch64 `g_log_level = LOG_INFO` 在 `kernel/arch/aarch64/log_impl.c` 定义。 | `pmm_init` ✅ | Linux printk |
| **mm(arch) PGD/PUD/PMD/PTE 层级统一** ✅ | **已完成**（2026-09-10，`52f99a1`）。Linux/ARM 命名替换 x86_64 PML4/PDPT/PDE：`mm->pml4 → mm->pgdir`、`vmm_walk_pml4` 系列 → `vmm_pt_walk`（参数已 `pgdir`）、`PAGE_GDT_SHIFT → PAGE_PGD_SHIFT`、`PAGE_USER_GDT/Dir/Page → PAGE_USER_PGD/PUD/PMD`、`PAGE_KERNEL_GDT/... → PAGE_KERNEL_PGD/PUD/PMD`、`PAGE_USER_4K/4K_RO → PAGE_USER_PTE/PTE_RO`、`PAGE_KERNEL_4K → PAGE_KERNEL_PTE`、`PAGE_KERNEL_MMIO → PAGE_KERNEL_PMD_NOCACHE`；bit-constant rename：`PAGE_Present → PAGE_VALID`、`PAGE_U_S → PAGE_USER`、`PAGE_R_W → PAGE_WRITE`、`PAGE_PS → PAGE_HUGE`、`PAGE_XD → PAGE_NO_EXEC`、`PAGE_Global → PAGE_GLOBAL`、`PAGE_PCD → PAGE_CACHE_DISABLE`、`PAGE_PWT → PAGE_WRITE_THROUGH`；~150 站点 rename（vmm.c / sched COW fork / elf loader / vma/uaccess/fb/futex / `test_uaccess.c`）。保留 x86_64 `head.S` `__PML4E:`/`__PDPTE:` 硬件 label + `kernel/include/memory/vmm.h` bit-position 常量（标为 x86_64 PTE 格式专属）。x86_64 `kernel.bin` 字节相同（1,739,024 B）、aarch64 pmm.c 编译干净。 | PMM ✅ | Linux/ARM |
| **bootinfo(arch) E820 拆出 `bootinfo_x86.h`** ✅ | **已完成**（2026-09-10，`af9a6ce`）。`struct E820_ENTRY` + `BOOT_MEMORY_FORMAT_E820=1u` 从 `bootinfo.h` 移到 `kernel/arch/x86_64/bootinfo_x86.h`；`bootinfo.h` 保留 enum 值 1 + 注释指针；`pmm.c` 去掉 arch-neutral `entry_size` 分支（强覆盖已校验 `n==0`）；4 处 include 加 `bootinfo_x86.h`。字节相同，aarch64 编译视图无 E820 符号。 | PMM ✅ | |
| **arch(neutral) pt_regs_t facade + arch_cpu_pause** ✅ | **已完成**（2026-09-10，`19b84a8`）。新增 `kernel/include/arch/regs.h` 派发 facade（guard `_KERNEL_ARCH_REGS_FACADE_H`）→ `arch/x86_64/regs.h` / `arch/aarch64/regs.h`（aarch64 从原 `arch/thread.h` 抽出）；`arch/thread.h` 改为 include `arch/regs.h` 不再硬引 `arch/x86_64/regs.h`，aarch64 `pt_regs_t` typedef 移出到 `aarch64/regs.h`，函数声明 lift out of `#ifdef`；`arch/irq.h` include `arch/regs.h` 而非 `arch/thread.h`（irq.h 只需 pt_regs_t）。`rwlock_relax()` 本地 `#if defined(__x86_64__)/__aarch64__` switch 替换为共享 `arch_cpu_pause()`（与 `tlb.c`/`rtc.c`/`serial.c`/`keyboard.c`/`lapic_timer.c` 同）。字节相同，`pause` (f3 90) 仍按预期位点 emit。 | PMM ✅ | |
| **intr(arch) `arch_irq` hooks 拆分** ✅ | **已完成**（2026-09-10，`dfead87`）。新增 `arch_irq_select_controller(gsi)` / `arch_irq_gsi_to_vector(gsi)` / `arch_irq_vector_to_gsi(vector)` / `arch_irq_dispatch(regs, hwirq)` 四 hook + 弱默认 `kernel/intr/arch_irq_hooks.c`（select FATAL-halt / vector↔gsi identity / dispatch silent no-op）+ x86_64 强覆盖 `kernel/arch/x86_64/irq_hooks.c`（APIC→PIC ladder + 0x20+gsi 翻译 + 移动过来的 do_IRQ 体 + `nr & 0x80` spurious 检查）。do_IRQ 从 `intr/pic/8259A.c` 移出（该文件回到单纯 8259A controller 实现）。`unregister_irq(uint64_t nr)` → `unregister_irq(uint32_t gsi)`，跟 `register_irq` 对齐（消除 plan `2026-08-17-timer-clocksource-clockevent.md:20` 文档的 off-by-vector footgun）；`kernel/driver/rtc.c` 改为 `unregister_irq(8)`，删除 `RTC_PIE_IRQ_VEC` 宏。 | PMM ✅ | Linux do_IRQ |
| **rtc(arch) core + per-arch impl 拆分** ✅ | **已完成**（2026-09-10，`c37522a`）。三层拆分：① `kernel/include/driver/rtc.h` 只剩 `datetime_t` + `rtc_read/write_datetime`（去 CMOS_ADDR/DATA/BCD2BIN/is_updating/get/set_rtc_register/rtc_pie_calibrate）；② `kernel/driver/rtc.c` 改为走 `kernel/include/arch/rtc.h` 中 `arch_rtc_read/write` hook 的 core，KERNEL_C_SOURCES 跨 arch 编译；③ x86_64 强覆盖 `kernel/arch/x86_64/rtc_cmos.c`（CMOS port I/O + BCD + UIP regA bit7 + BIN regB bit2 全本地化）+ `kernel/arch/x86_64/rtc_pie.c`（PIE/LAPIC/TSC 校准 verbatim move），新 `kernel/include/arch/x86_64/rtc.h`。`kernel/Makefile` 把 `driver/rtc.c` 从 filter-out 移出，`kernel/arch/x86_64/make.config` 从 `ARCH_PLATFORM_C_SOURCES` 删除（wildcard 已收 `rtc_cmos.c`），`arch/x86_64/time.c` 改 include `<arch/x86_64/rtc.h>`。 | PMM ✅ | |
| **arch(subsys) 平台自决 driver 注册** ✅ | **已完成**（2026-09-10，`0ecee53`）。新增 `SUBSYS_INITCALL()` 宏（typedef + externs 在 `kernel/include/subsys/subsys.h`） + `kernel/arch/x86_64/linker.ld` 新 `.subsys_init` section + sentinels，10 driver 自注册（ahci/keyboard/pci/pit/serial/lapic/lapic_timer/pic/net/clocksource/timer，每 .c 加 init wrapper + `SUBSYS_INITCALL()` 行）；`arch_register_subsys()` 缩到 7 行 loop（迭代 `.subsys_init` 表调 `register_subsys()`）。OS01 libc-free 无 `.init_array` runtime support，故采用 Linux initcall 同款 trick（不用 `__attribute__((constructor))`，否则指针落在没人迭代的 section）。 | PMM ✅ | Linux initcalls |
| **arch(sched) arch-neutral `kernel_thread_entry`** ✅ | **已完成**（2026-09-10，`58db2a7`）。`kernel_thread_func` 重命名为 arch-neutral `arch_kernel_thread_entry`；弱默认 panic-on-call 在新 `kernel/sched/arch_kernel_thread_entry.c`；x86_64 强覆盖在 `arch/x86_64/thread_entry.S`；`task.c` 不再 include per-arch 头。同时把 `kernel/.stage1` + `kernel/.stage2` 加进 `.gitignore`（每 make 重生成的中间产物之前污染 git status）。 | PMM ✅ | |
| **build(uefi) 排除 `*.o/*.a/*.lib`** ✅ | **已完成**（2026-09-10，`5dbc63d`）。`mk/components/uefi.mk` 两处 patch：① `find thirdpart/posix-uefi -type f` digest 用 `! -name "*.o" ! -name "*.a" ! -name "*.lib"` 排除；② `cp -a "$(UEFI_RUNTIME_SOURCE)/." "$(UEFI_RUNTIME_DIR)/"` 后加 `find "$(UEFI_RUNTIME_DIR)" \( -name "*.o" -o -name "*.a" -o -name "*.lib" \) -delete`。根因：linked git worktree 的 `git -C posix-uefi status --porcelain` 用 submodule `config.worktree` 路径解析到 MAIN checkout（不在 worktree），与 find-based digest（跑在 OS01_ROOT 看 worktree 内容）不一致，导致前次 x86_64 build 残留 `*.o` 经 `uefi/*.o` glob 进 aarch64 `BOOTAA64.EFI` ld.lld 链接产生 duplicate-symbol + machine-type-mismatch。干净 checkout 无行为变化。验证 selftest 21/21 + syscall 268/268 + aarch64 UEFI BOOTAA64.EFI ARM64 PE32+。 | PMM ✅ | |
| **统一 kernel_main** | `pmm_init` 之后 aarch64_main (91 行) 与 x86_64 kernel_main 仍然分叉：aarch64 走 `dtb_init→gic_init→smp_boot_aps→arch_tick_start→halt`；x86_64 走 scheduler init/IDT/APIC/syscall table。目标：单 `kernel_main` 按固定顺序调 `arch_early_init()`/`arch_late_init()`/`scheduler_init()`/`arch_late_init()`。**设计决策**：aarch64 必须先 dtb 才能用 DTB 信息；init 顺序契约需明确。 | PMM ✅, SMP ✅, 调度器 core, arch_irq ✅, rtc ✅, pt_regs_t ✅ | 长项 spec（独立） |
| **中断/异常 dispatch 统一** | `arch_intr_dispatch(vector, pt_regs*)` 单入口抽象；x86_64 IDT vs aarch64 VBAR_EL1 vector tables 各自封装；x86_64 IST stack vs aarch64 SP_EL1 切换。**最大代码量减少**：`x86_64/trap.c` 3065 行（大部分是 x86 register decode）；aarch64 `trap.c` 20 行（占位）。`arch_irq_dispatch` 已落地（v25 `dfead87`），dispatch 路径 hook 化完成，剩余是 trap.c 内部 x86 register decode 的 arch 剥离。`intr_handler_table[256]` 已经是 arch-neutral。 | arch_irq ✅, head.S ✅ | Linux do_IRQ |
| **SMP 启动统一** | `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` 单入口；内部 aarch64 PSCI CPU_ON / x86_64 INIT-SIPI + trampoline 各自实现。`boot_percpu` / `subsys_percpu` layout 已经统一。 | GIC ✅, 启动链 ✅ | opuntiaOS |
| **上下文切换统一** | `arch_task_switch(prev, next)` + `arch_thread_entry()`；aarch64 ret 到 user vs x86_64 sysret/iret。寄存器保存集不同但语义同。x86_64 `task_arch.c` 34 行，aarch64 SMP 内含 context switch。 | SMP 启动统一 | Tilck |
| **定时器统一** | Phase 9 的 `clocksource`/`clockevent` 抽象已经存在；把两个 arch 的 timer driver 都注册到 `clockevent` 框架即可。aarch64 `time.c` 133 行 (Generic Timer)，x86_64 `time.c` 28 行 (LAPIC/HPET)。 | clocksource/clockevent hook ✅ | Linux tick |
| **CPU 特性探测** | `arch_cpu_features()` 返回统一位图 (has_fpu / has_virt / has_cache_coherency)；x86 CPUID vs aarch64 ID_AA64* 各实现一份。 | 独立 | Linux cpufeature |
| **DTB/BIOS 解析** | 解析保持 per-arch（DTB vs BIOS E820/UEFI），但输出已经是 `boot_context v2` arch-neutral。**此层已统一**，解析层保持 per-arch。 | boot_context v2 ✅ | |
| **用户态 syscall ABI** | `svc #0` 入口 + 参数传递 | 启动 | |
| **设备驱动 (UART/timer/...)** | 保持 per-arch（pl011 vs 16550、GIC vs APIC/IOAPIC）。抽象接口已部分存在 (`driver/serial.h`)，新驱动按抽象接入。 | Phase 2 基础设施 | |

**距离单一 kernel_main 还差多远（粗估，一个人全职）**：~4–8 周（v25 后已落地：arch_irq hooks + pt_regs_t facade + rtc split + kernel_thread_entry + driver initcall + PGD/PUD/PMD/PTE）。建议 3 个独立 spec/plan 增量推进：

1. **Spec A — 中断/异常 dispatch 收尾 + 上下文切换 arch 抽象**：在 v25 `arch_irq_dispatch` 落地后，剩余 `x86_64/trap.c` 3065 行的 x86 register decode 抽到 arch 层，aarch64 `smp.c` 拆分 context switch。预计 2–4 周。
2. **Spec B — 统一 SMP 启动 + 定时器 + CPU 特性**：`arch_smp_boot_aps` + `clockevent` 双 arch 注册 + `arch_cpu_features()`。预计 2–3 周。
3. **Spec C — 统一 kernel_main**：在 A、B 之上定义 `arch_early_init`/`arch_late_init`，单 `kernel_main` 按固定 init 顺序调（pmm_init → arch_early_init → scheduler → arch_late_init → ...）。设计 init 顺序契约。预计 2–4 周。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。这些靠 arch 抽象层封装，统一接口、不统一实现。**v25 已统一**：页表**层级**名（PGD/PUD/PMD/PTE，bit-position 仍 per-arch），bootinfo ABI（解析层仍 per-arch，但输出 v2 已 arch-neutral），intr dispatch hook 化，driver 自注册 initcall，kernel_thread_entry arch-neutral，RTC 拆分（核心 arch-neutral + per-arch CMOS/PIE）。

**详细盘点见 session 期间的对话**（v24 已落地 spec `2026-09-09-pmm-arch-neutral-design.md` + plan `2026-09-09-pmm-arch-neutral.md`，v25 arch-cleanup-gh 详见各 commit message：10 commits `67132e2..3ab4ef1` 按 weak-default + strong-override 模式分阶段完成 E820/pt_regs_t/intr hooks/rtc split/PGD-PUD-PMD-PTE/initcall/kernel_thread_entry/UEFI 残留排除）。

### 🖥 P3 GUI

已有基座：fb ✅、fb mmap ✅、terminal 双缓冲 + alt-screen ✅、键盘扫描码 ✅。Tetris 游戏已落地（见 `docs/gui.md`）。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| PS/2 鼠标驱动 | `/dev/mouse`，扩展 keyboard.c 的 PS/2 协议处理 | 独立 | |
| 2D 图形 API | fb 之上画线/矩形/位图 blit | 独立 | |
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API | HackOS |
| Window Server + compositor | 多窗口管理 + 合成（原 P3#15） | 字体/2D/鼠标 | opuntiaOS + HackOS |

### 🔧 P4 硬件适配

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| USB 驱动栈 | HID/存储/网络 | 独立 | |
| 真机启动 (USB) | 建立硬件验证路径（原 P3#14） | USB 存储 | Tilck |
| NVMe 驱动 | 替代 AHCI（原 P3#17） | 独立 | |
| HPET clocksource | 真实硬件跨平台时间源（timer spec 方案 B） | 独立 | |
| ACPI | 电源管理/关机 | 独立 | |

### 📐 P5 ABI 扩展/兼容性

依赖链：`ELF loader ✅ → 动态链接器 → 共享 libc → Alpine apk/musl`；`futex ✅ → clone → pthread`；`socket ✅ → AF_UNIX`；`mbedTLS ✅ → HTTPS`。

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 动态链接器 | PT_INTERP + ld.so + 共享 libc（原 P2#9） | ELF ✅ | cavOS |
| rt_sigaction | 现代信号语义（SA_RESTART/si_value/实时信号），替换老 SYS_signal | 信号重构 | |
| clone/pthread | 线程模型 + pthread_create | futex ✅ | |
| readv/writev | scatter-gather I/O | 独立 | |
| openat/dup3/pipe2 | 现代 syscall 变体 | 独立 | |
| FIFO 命名管道 | S_IFIFO 语义 + mkfifo | 独立 | |
| alarm/setitimer | POSIX 定时器（busybox timeout 需要） | 独立 | |
| 作业控制（部分完成）| ✅ ~~setpgid/setsid/getpgid/getsid（67-70）~~、✅ ~~tcgetpgrp/tcsetpgrp 真实现~~、✅ ~~tty ISIG + VINTR/VQUIT~~、✅ ~~kill 支持 pid=0/-pid/-1~~；**剩余**：SIGWINCH 派发、SIGTSTP/SIGCONT 完整作业控制（bg/fg/jobs，需 busybox `CONFIG_ASH_JOB_CONTROL=y` 或自写 shell） | tty termios ✅ | |
| /proc 完善 | status（signal mask/ppid/utime/stime）+ cmdline + stat | 独立 | |
| symlink/readlink ✅ | **已完成**（2026-09-06，commits `43588c8`..`c64c854`）。VFS 软链接 + ext2 symlink（fast ≤60B inline `i_block[0..59]` / long data block via `i_block[0]`）；4 新 syscall `SYS_symlink/readlink/lstat/fstatat`=71..74；libc `syscall3`/`syscall4`（r10 ABI for fstatat）；`vfs_getdents` `VFS_SYMLINK→DT_LNK`；67 systest + 6 kernel selftest。详见 `docs/superpowers/specs/2026-09-05-symlink-support-design.md` v5 | 独立 | |
| **exec symlink ABI** ✅ | **已完成**（2026-09-06）。`b32e1e0` 的 busybox 副本变通已回退：rootfs manifest 以 debugfs symlink 写 29 条 applet 链（含 `/bin/ln`），busybox `CONFIG_LN/CONFIG_FIND/CONFIG_FEATURE_FIND_TYPE` 开启，`vfs_lookup_at(LOOKUP_FOLLOW)` 让 execve 跟随末段 symlink 到 busybox ELF、argv[0] 派发到对应 applet。`systest.c` case 41 fork+exec `/bin/ln -s` 创链 → lstat 验证 S_IFLNK → fork+exec 跟随 → busybox `true` applet exit 0；case 42 fork+pipe+exec `/bin/find <dir> -type l` 验证 `VFS_SYMLINK→DT_LNK` 映射。systest 268/268、nettest 6/6 全绿 | exec 软链接跟随（P1）✅ | Linux |
| **sysroot 增量重编译** | 2026-09-02 build refactor 取舍：不可变 generation 发布 → 任一 sysroot 内容变化使内核/用户/BusyBox 整体重编译（-B/digest），非只重编依赖者（spec 209 未满足）。GNU Make 重构 T1–T7 已落地 `c1a64c5`/`9efb970` 的 provider-keyed archive + 原子发布，但头文件级增量重编仍未达成。refinement：generation 内 .d 路径相对化/软链引用，使头文件变化只重编依赖者 | 独立 | |
| HTTPS/TLS | mbedTLS 集成 BusyBox wget（原 P2#10） | mbedTLS ✅ | |
| AF_UNIX/socketpair | 本地 socket IPC（原 P2#11） | socket ✅ | |
| 更多 applet | grep/sed/find，先补 libc regex/fnmatch（原 P1#7） | libc | |
| **libc 完整性** | ✅ printf `%f/%F/%e/%E/%g/%G` + `%ld/%lu` + `%x/%o`、strtod（小数/指数）、getopt 短选项（sleep/seq/du/cksum/sum 已恢复）；**仍缺**：stdio 行读取/seek（nl/tail/tac/expand 空 + nl user-fault 崩溃）、getcwd（pwd 空）、cut/paste 的 getopt 解析 | 独立 | |
| Alpine apk/musl | musl 二进制包兼容路线（原 P3#16） | 动态链接器 | cavOS |

### 依赖链总览

```
P1: getrandom ✅ → 统一用户态启动方式（进行中）→ AT_RANDOM → canary / ASLR（kernel SSP ✅，用户栈 canary 排在启动统一之后）
P2: PMM ✅ + log API ✅ + PGD/PUD/PMD/PTE ✅ + E820 拆分 ✅ + pt_regs_t facade ✅ + arch_irq hooks ✅ + rtc split ✅ + subsys initcall ✅ + kernel_thread_entry ✅ + UEFI 残留排除 ✅ + 页表原语 ✅
   → head.S + MMU（原语就绪）→ GICv2 → Generic Timer；
   统一 kernel_main（interrupt/SMP/context-switch 三独立 spec，arch_irq 已落地收尾）；
   rwlock → aarch64 SMP；timer hook ✅ → CNTP
P3: fb ✅ → 2D API → 字体 → Window Server；PS/2 鼠标并行
P4: USB 栈 → 真机启动；NVMe / HPET / ACPI 独立
P5: ELF ✅ → ld.so → 共享 libc → apk/musl；futex ✅ → clone → pthread
    socket ✅ → AF_UNIX；mbedTLS ✅ → HTTPS
```

### Parked（未闭环 follow-on，随时可拾起；2026-09-13 逐项对照代码核实）

| 项 | 状态 | 说明 |
|----|------|------|
| ~~devfs mount entry 0x600000 被覆写~~ | **已闭环** | 根因已定位并修复：硬件 watchpoint 证明 e1000 TX ring 落在了 boot slab 帧上——PMM 的 kernel/slab 帧预留用物理 PFN 而非 RAM-relative 索引（`0809100` 修复），e1000 DMA buffer 本身一直是 `alloc_4k_page()`（`60ce39a` 起）。SMP=1/2/4 + systest-repeat 7 连 268/268 验证。`4468e75` 的 `find_mount` 防御保留作纵深防御 |
| ~~PMM 非-RAM 类型丢弃~~ | **已闭环** | `beb351c` 已让 x86_64 `pmm_arch_normalize` 把 E820-derived type 写进 `out[].type`（代码注释明确 "do NOT force everything to MEMORY_TYPE_RAM"）；`pmm_init` Step 2 只 walk `MEMORY_TYPE_RAM` 是设计行为，非-RAM 条目留给未来 ACPI reclaim/NVS 消费者 |
| ~~`__vfs_lookup_raw` consumed 路径~~ | **已闭环** | `b0c95e3`（2026-09-06）已以 mount prefix 播种 `consumed`，含 `..` 的 consumed 回剪与非根 mount 相对 symlink 正确解析；下文 exec 软链接行的旧备注已过时 |
| sysroot 头文件级增量重编 | **仍开放** | `mk/components/kernel.mk` 仍是 genid 变化即 `-B` 全量重编；refinement：generation 内 .d 路径相对化/软链引用，使头文件变化只重编依赖者 |
| `LWIP_RAND`/AT_RANDOM 种子 | **仍开放** | `kernel/include/net/arch/cc.h:98` 仍是 `jiffies * 1103515245 + 12345` LCG，未接内核 ChaCha20 熵池；auxv 仍只压 `AT_NULL`（`task.c` exec 两处）。与 P1 用户栈 canary 同批做 |

---

## 相关文档（已迁出内容）

| 文档 | 内容 |
|------|------|
| `docs/changelog.md` | 已完成工作按时间汇总（截至 2026-08-18） |
| `docs/decisions.md` | 46 条关键设计决策总账（按主题分区，供交叉引用） |
| `docs/references.md` | 开源 OS 项目借鉴表（已用 / 可拿） |
| `docs/syscall.md` | 71 syscall 表 + 用户指针边界语义 + syscall 边界审计触达清单 |
| `docs/signal.md` | 信号投递、handler、sigreturn、Ctrl-C→SIGINT |
| `docs/timer.md` | Timer 重构架构 + nanosleep 修复 + 重构实施总结（commit/验证） |
| `docs/smp.md` | SMP 架构 + 负载均衡实施总结（前置加固 / AP bug 修复 / 变更清单） |
| `docs/gui.md` | Tetris 游戏实施总结 + P3 GUI 路线图 |
| `docs/io-multiplexing.md` | select/pselect 实施总结 |
| `docs/network.md` | lwIP 网络栈 + 正确性加固实施总结 |
| `docs/scheduler.md` / `docs/scheduler-complexity.md` | EEVDF 调度器设计与复杂度评估 |
| `docs/applet-verification.md` | busybox applet 验证清单 |
| `docs/build.md` | GNU Make profile 化构建体系（2026-09-02 重构后）：profile 入口 / 能力契约 / 用户入口 / 工具链覆盖变量 / provider-keyed selfhosted archive / runtime validation targets / host-tool mkdisk / make help capability 标签。**v25 增量**：`mk/components/uefi.mk` digest + staged copy 排除 `*.o/*.a/*.lib`（worktree 内 posix-uefi 残留污染 aarch64 BOOTAA64.EFI 链接修复） |
| `docs/superpowers/specs/2026-09-09-pmm-arch-neutral-design.md` | v24 PMM arch-neutral spec（13 轮 subagent review 通过）：`pmm_init(boot_context)` 单一入口 + `MEMORY_RANGE[]` 中介 + 弱默认/强覆盖 dispatch + RAM-relative indexing + Step 7 unsigned-underflow clamp。**v25 已演化为 v24 之后的 arch-cleanup 基础层** |
| `docs/superpowers/plans/2026-09-09-pmm-arch-neutral.md` | v24 PMM arch-neutral implementation plan（3 轮 subagent review 通过 + 16 task + 1 final fix round 通过）：x86_64 E820 → MEMORY_RANGE[]，aarch64 读 `aarch64_ram_map_get()`，log API gate-wrapped `_log_*_impl` 统一，aarch64 stubs (printk/memset/slab/log_impl) |
| `git log a2e7389..3ab4ef1 --oneline` | **v25 arch-cleanup-gh 10 commits**（无独立 spec/plan，由 commit message 驱动实现）：① `67132e2` roadmap v24 doc；② `af9a6ce` bootinfo(arch) E820 → `bootinfo_x86.h`；③ `19b84a8` arch(neutral) `arch/regs.h` pt_regs_t facade + `rwlock_relax()` 走 `arch_cpu_pause()`；④ `dfead87` intr(arch) `arch_irq` hooks 拆分 controller selection + gsi↔vector + dispatch；⑤ `c37522a` rtc(arch) core + per-arch impl 拆分；⑥ `52f99a1` mm(arch) PGD/PUD/PMD/PTE 层级统一 + bit-constant rename；⑦ `0ecee53` arch(subsys) `SUBSYS_INITCALL()` + `.subsys_init` section 替代硬编码 driver 列表；⑧ `58db2a7` arch(sched) `arch_kernel_thread_entry` 弱默认 + x86_64 强覆盖；⑨ `5dbc63d` build(uefi) digest + staged copy 排除 `*.o/*.a/*.lib` 残留；⑩ `3ab4ef1` Merge branch 'arch-cleanup-gh'。共同模式：weak-default（panic/FATAL/identity/no-op 或友好默认）+ strong-override（per-arch 实现）+ `kernel/include/arch/{regs,irq,rtc,...}.h` facade 派发到 `kernel/arch/{x86_64,aarch64}/` |
| `docs/superpowers/specs/2026-09-11-aarch64-page-table-primitives-design.md` / `plans/2026-09-11-aarch64-page-table-primitives.md` | aarch64 页表 walk/map 原语 + smoke 断言（descriptor bit + minimal intermediate descriptor 两轮修复） |
| `docs/superpowers/specs/2026-09-12-directory-restructure-design.md` / `plans/2026-09-12-directory-restructure.md` | 目录重构 P1–P6：目录布局约定（core/hosttests/qemutests/include/<subsys>）+ 旧路径换算 |
| `docs/README.md` + 4 份 reading guide | 文档学习入口层（调度器 / trap.c / vfs+memory / tty+intr） |
| `docs/boot.md` | x86_64 + aarch64 UEFI bootloader 统一 + `boot_context` handoff ABI + 架构中立生命周期。**v25 增量**：`bootinfo.h` 剥离 E820 至 `arch/x86_64/bootinfo_x86.h`，aarch64 编译视图纯净；`pmm.c` 不再 arch-neutral `entry_size` 分支 |
| `docs/architecture.md` / `docs/structure.md` / `docs/driver.md` / `docs/debug.md` / `docs/build-run-debug.md` / `docs/lwip-debugging-experience.md` | 系统整体架构 + 源码目录约定 + 驱动子系统 + 调试通道 + 端到端构建运行调试 + lwIP 调试经验。**v25 增量**：driver 注册从硬编码 `arch_register_subsys()` 列表改为 `SUBSYS_INITCALL()` + `.subsys_init` section 自注册；`arch/regs.h` facade 派发 pt_regs_t；`arch_irq_*` hook 三段式；RTC core + per-arch 拆分；页表层级 PGD/PUD/PMD/PTE |
