# 架构抽象层（multi-arch）

OS01 同时支持 **x86_64** 和 **aarch64**（后者仅 UEFI 启动路径）。本系统最重要的统一模式是：

> **weak-default + strong-override**：每个 arch-neutral 头文件位于 `kernel/include/arch/<topic>.h`，作为派发 facade。弱默认实现在 `kernel/<subsys>/arch_*.c`（FATAL-halt / panic-on-call / silent no-op / identity），强覆盖实现在 `kernel/arch/<arch>/<topic>.c`（per-arch 真实现）。链接器按强覆盖优先解析。`SUBSYS_INITCALL()` + `.subsys_init` section 用于 driver 自注册。

下面列出本模式涉及的所有 facade-override 对（截至 v25 arch-cleanup 系列完成）。

---

## Facade / override 矩阵

| Facade 头（arch-neutral） | 弱默认实现 | x86_64 强覆盖 | aarch64 强覆盖 | 用途 |
|---|---|---|---|---|
| `arch/include/regs.h` | — | `arch/x86_64/regs.h` | `arch/aarch64/regs.h` | `pt_regs_t` 跨 arch 定义；`arch_cpu_pause()` 共享 helper（替换 rwlock 本地 `#if` switch） |
| `arch/include/irq.h` | `intr/arch_irq_hooks.c`（FATAL/identity/no-op） | `arch/x86_64/intr/irq_hooks.c`（APIC→PIC ladder + 0x20+gsi 翻译 + do_IRQ） | — | 中断 controller selection / gsi↔vector / dispatch 三段 hook；do_IRQ 从 `intr/pic/8259A.c` 移出 |
| `arch/include/rtc.h` | `driver/rtc.c` core（走 `arch_rtc_read/write` hook） | `arch/x86_64/platform/rtc_cmos.c` + `rtc_pie.c`（CMOS port I/O + BCD + UIP regA bit7 + BIN regB bit2；PIE/LAPIC/TSC 校准） | — | 实时时钟 + PIE |
| `arch/include/subsys.h` | — | `arch/x86_64/linker.ld` 收集 `.subsys_init` | — | 10 driver 自注册（apic/pic/pit/lapic-timer/timer/serial/keyboard/ahci/pci/net）；`arch_register_subsys()` 缩到 7 行 loop |
| `arch/sched/...` `kernel_thread_entry` | `sched/arch_kernel_thread_entry.c`（panic-on-call） | `arch/x86_64/cpu/thread_entry.S` | — | arch-neutral 调度器线程入口；`task.c` 不再 include per-arch 头 |
| `memory/pmm.h` `pmm_init(boot_context*)` | `memory/pmm_arch.c`（弱默认 `pmm_arch_normalize`/`pmm_arch_zone_split`） | `arch/x86_64/memory/pmm_arch.c`（E820 + kernel-LMA/handoff/trampoline excludes + 2 MiB granule + sort/merge） | `arch/aarch64/memory/pmm_arch.c`（读 `aarch64_ram_map_get()`） | 物理内存 arch-neutral 入口；`MEMORY_RANGE[]` 中介；RAM-relative indexing（`pages_struct + ((start - lowest_ram) >> 21)`） |
| `log/log.h` `_log_*_impl` 宏 | `log/log.c` core（gate-wrapped） | `core/log.c` 走 `_log_writev` + vsnprintf + serial | `arch/aarch64/runtime/log_impl.c` 走 `kputs(fmt)` 忽略 variadic | 跨 arch 日志；aarch64 `-nostdlib` 无 vsnprintf |

**共同模式**：所有 facade 都满足「weak default 提供无操作/panic/FATAL/identity 默认行为，strong override 提供真实现，per-arch 通过头文件位于 `kernel/include/arch/<arch>/` 镜像」。链接器强覆盖优先解析同名符号。**没有运行时分派**：编译期决定。

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

详见 `docs/subsys.md`。

---

## 中断 hook 三段式

```c
// kernel/include/arch/irq.h
void arch_irq_select_controller(uint32_t gsi);          // 选 controller
uint8_t arch_irq_gsi_to_vector(uint32_t gsi);            // gsi → vector
uint32_t arch_irq_vector_to_gsi(uint8_t vector);         // vector → gsi
void arch_irq_dispatch(pt_regs_t *regs, uint32_t hwirq); // 实际 dispatch
```

弱默认：`kernel/intr/arch_irq_hooks.c`（select FATAL-halt；vector↔gsi identity；dispatch silent no-op）
x86_64 强覆盖：`kernel/arch/x86_64/intr/irq_hooks.c`（APIC→PIC ladder + 0x20+gsi 翻译 + 移动过来的 do_IRQ 体 + `nr & 0x80` spurious 检查）

`unregister_irq(uint64_t nr)` → `unregister_irq(uint32_t gsi)`，跟 `register_irq` 对齐（消除 `2026-08-17-timer-clocksource-clockevent.md:20` 文档的 off-by-vector footgun）。

---

## RTC 三层拆分

- `kernel/include/driver/rtc.h` — 只剩 `datetime_t` + `rtc_read/write_datetime`
- `kernel/driver/rtc.c` — 走 `arch_rtc_read/write` hook 的 core（KERNEL_C_SOURCES 跨 arch 编译）
- x86_64 强覆盖 `kernel/arch/x86_64/platform/rtc_cmos.c`（CMOS port I/O + BCD + UIP regA bit7 + BIN regB bit2 全本地化）+ `kernel/arch/x86_64/platform/rtc_pie.c`（PIE/LAPIC/TSC 校准 verbatim move）

---

## 距离单一 kernel_main 还差多远（v25 后）

~4–8 周（一个人全职），3 个独立 spec/plan 增量推进：

1. **Spec A — 中断/异常 dispatch 收尾 + 上下文切换 arch 抽象**：`arch_irq_dispatch` 已落地，剩余 `x86_64/trap.c` 3065 行的 x86 register decode 抽到 arch 层；aarch64 `smp.c` 拆分 context switch。预计 2–4 周。
2. **Spec B — 统一 SMP 启动 + 定时器 + CPU 特性**：`arch_smp_boot_aps` + `clockevent` 双 arch 注册 + `arch_cpu_features()`。预计 2–3 周。
3. **Spec C — 统一 kernel_main**：在 A、B 之上定义 `arch_early_init`/`arch_late_init`，单 `kernel_main` 按固定 init 顺序调（pmm_init → arch_early_init → scheduler → arch_late_init → ...）。设计 init 顺序契约。预计 2–4 周。

**永远无法统一的（ISA/HW 差异）**：`head.S`/`entry.S` 指令集差异；MMU 页表格式（PTE bit-position）；中断控制器驱动；SoC 外设（UART/timer/GPIO 等）。这些靠 arch 抽象层封装，统一接口、不统一实现。

**v25 已统一**：页表层级名（PGD/PUD/PMD/PTE，bit-position 仍 per-arch）、bootinfo ABI（解析层仍 per-arch，但输出 v2 已 arch-neutral）、intr dispatch hook 化、driver 自注册 initcall、kernel_thread_entry arch-neutral、RTC 拆分（核心 arch-neutral + per-arch CMOS/PIE）。

---

## 设计依据

- 详细 spec：`docs/superpowers/specs/2026-09-09-pmm-arch-neutral-design.md`（13 轮 subagent review 通过）
- 实施 plan：`docs/superpowers/plans/2026-09-09-pmm-arch-neutral.md`（3 轮 subagent review + 16 task + final fix）
- v25 arch-cleanup 系列 10 commits `67132e2..3ab4ef1`：roadmap v24 doc；bootinfo(arch) E820 → `bootinfo_x86.h`；arch(neutral) `arch/regs.h` pt_regs_t facade + `rwlock_relax()` 走 `arch_cpu_pause()`；intr(arch) `arch_irq` hooks 拆分；rtc(arch) core + per-arch impl 拆分；mm(arch) PGD/PUD/PMD/PTE 层级统一 + bit-constant rename；arch(subsys) `SUBSYS_INITCALL()` + `.subsys_init` section；arch(sched) `arch_kernel_thread_entry`；build(uefi) digest + staged copy 排除 `*.o/*.a/*.lib`；merge
- x86_64 启动细节：见 `docs/architecture.md` + `docs/boot.md`