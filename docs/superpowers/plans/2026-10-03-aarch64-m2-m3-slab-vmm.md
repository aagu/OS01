# aarch64 M2 Slab 接入 + M3 运行期 VMM 实施计划 (v2)

> **For agentic workers:** REQUIRED SUB-KILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **v4 rewrite.** v3 had 15 verified issues (smp_starting order, AP IRQ-before-ipi_ready, percpu field dependency, IPI migration atomicity, gic_target_bit init for testing, M1 root registration timing, compute_arena_end compile errors, x86 slab upper bound overflow, aarch64 selftest entry not invoked, probe false-positive OK, x86 ipi_ready call sites, 12-combo test split, arena test inputs, file paths). v4 corrects the most impactful items; see "v4 fix for v3 review item N" markers inline.
>
> **Static review still in progress.** Even after v4 fixes, implementation will surface issues — proceed iteratively: build, find issue, fix, re-build. Do not block on plan review.
>
> **v3 fixes retained.** v3 had fixed the v1 review's 16 items; those markers are still inline.

**Goal:** 在 aarch64 上接上生产 Slab（任意缓存大小可分配/释放），并把运行期 VMM（4 KiB 映射/解除、2 MiB block 映射/解除/split、共享映射跨核 shootdown）落到能跑双核 `-smp 2` 正式镜像探针并产出 `M3-SHOOTDOWN-PROBE: OK`；x86_64 全程行为不变。

**Architecture:** 把公共 VMM 拆成 `kernel/include/memory/vmm.h` 语义层（仅 `VM_*` 位 + `arch_vmm_*` 语义 API）+ per-arch backend；x86 把 `PAGE_*` 与裸 PTE 访问迁入 `kernel/include/arch/x86_64/pte.h` 私有头，`vmm.c` 退化为 backend wrapper，**保留** `kernel/memory/vmm.c:317-339` 的 free/COW 所有权逻辑原样（仅一条 free 调用：COW 路径走 `page_cow_put` 返回 true 时才 free，普通路径直接 free）。aarch64 在 `kernel/arch/aarch64/memory/page_table.c` 增加持锁公开原语 + 软件位编码 + block 编解码 + split 协议（发布登记表 + 未发布 root 原子 store；已发布 root → `-EPERM`）。IPI 路径沿用 `kernel/include/intr/ipi.h` 已定义的逻辑向量值（`IPI_VECTOR_TLB = 0x40`），在 `kernel/arch/aarch64/intr/ipi.c` 内做"逻辑向量 → SGI 编号（=3）→ GIC 目标字节"的两层映射；SGI 用显式目标列表（`GICD_SGIR_FILTER_LIST`），不用 all-but-self。Shootdown 走 `tlb_sd_lock` 串行化 + 每核 `tlb_ack_gen` 代数 + 超时 FATAL。Slab 在 aarch64 上用 `arch_local_irq_save/restore` 替换内联 `pushfq; cli`，并把 slab_init 的 j-loop/8 页记账幂等化以避免与 boot reservation 重复计数。`slab_layout_compute()` 给 preflight 与启动后断言一个共同的真值源；`kernel/arch/aarch64/memory/early_arena.c` 的候选选择阶段**先**把 slab 段字节计入 arena 估算再扫描 R 区间。

**Tech Stack:** freestanding C11、Clang/LLD、AArch64 EL1 stage-1 translation tables、GICv2（`virt,gic-version=2`）、现有 PMM、QEMU aarch64、native hosttests、Python QEMU harness。

**Spec:** [2026-10-03-aarch64-m2-m3-slab-vmm-design.md](../specs/2026-10-03-aarch64-m2-m3-slab-vmm-design.md)（v9）。基线 `dabc9f0`。前置：M0 ✅ / M1 ✅ / Generic Timer ✅ / GICv2 Phase 1 ✅ / IPI fix ✅ / PMM arch-neutral ✅。

## Global Constraints

- `ARCH_PAGE_OFFSET=0xffff000000000000`、48-bit VA、4 KiB granule、PA < `1 << 40`；不改 TCR/MAIR；新映射只在 TTBR1 root（`aarch64_runtime_root_address`）内活动。
- Slab 在 M0 直映（TTBR0，0..2 GiB）下真实运行：slab 元数据 + 8 个预分配 2 MiB 页的物理地址必须 < 2 GiB；`arena_end ≤ 2 GiB` 是硬约束，越界 FATAL。`PMMngr.end_of_struct` 启动后断言 `≤ slab_meta_end`（直接等于，不是 `slab_meta_end - 8 * 2 MiB`；v1 review item 2）。
- aarch64 slab `slab_init` 的 j-loop 与 8 页循环**幂等化**（`bits_map` 位已置 → 跳过、不 ++/--），与 `pmm_reserve_boot_ranges` 的覆盖记账共同满足"每 2 MiB frame 恰好记一次"。x86 无预预留 → 位全 0 → 行为不变（用 hosttest 验证 using/free 计数回归）。
- `slab_layout_compute()` 是 meta 字节估算的单一事实来源，preflight 与启动后断言均消费同一函数。实现只读 `extern struct Slab_Cache kmalloc_cache_size[16]`（`kernel/include/memory/slab.h:34`）的 `.size`，不引入 `kmalloc_cache_size_array_size()` 这类新抽象（v1 review item 1）。
- `slab.c` 锁路径必须用 `arch_local_irq_save/restore`（`kernel/include/arch/irq.h` 已存在）；文件内禁止再出现 `pushfq/cli/sti/popfq` 字面。
- 公共 `kernel/include/memory/vmm.h` 只暴露 `VM_*` + 语义 API（`arch_vmm_*`）；x86 硬件位（PTE bit、PAGE_HUGE、用户/特权、PS 等）迁入 `kernel/include/arch/x86_64/pte.h` 私有头。`PAGE_*` 公共宏全部删除。
- `arch_vmm_unmap_4k` / `arch_vmm_unmap_2m` / `arch_vmm_update_4k` **永不 free 数据页**——释放/COW 引用计数在 x86 层保留 `kernel/memory/vmm.c:317-339` 原逻辑（v1 review item 9：COW 路径 `if (page_cow_put(phys)) free_4k_page(phys);` 只一条 free；普通路径 `else free_4k_page(phys);`）。
- 4 KiB 替换协议按 spec §4.4.3 四类分：仅权限位 → 原子 store + dsb ishst + local TLBI + shootdown；内存类型变 / PA 变 / 有效性翻转 → PTE 级 BBM（持 `pt_lock_for(root, l2)`）。block↔table 替换走 §5.3 split 协议（未发布 root 原子 store；已发布 root → `-EPERM`）。
- aarch64 已发布 root 判定走 **`published_roots[]` 登记表**（静态 0 初始、`spin_init` 双保险）。登记点 = 安装点之前：M1 安装 TTBR1 之前 `aarch64_pt_root_publish(tree.root_pa)`，让登记失败可保留旧 root 不变（v1 review item 12）；`arch_vmm_init` 引用同一 root 幂等登记；M4 的 `arch_switch_mm` 后续成为用户 root 登记点。
- aarch64 后端锁：3 锁（`pt_locks[64]`/`pt_upper_lock`/`tlb_sd_lock`）全部**普通 `spin_lock`**（不用 irqsave——等待者必须能响应 SGI）。锁序 `pt_lock → pt_upper_lock → tlb_sd_lock`，无反序；TLB IPI handler 无锁。锁定义落在 §3.1 阶段（Task 11），早于 §3.2 阶段使用它们的 task。
- aarch64 shootdown 协议：`tlb_shootdown()` **保持无参**（现有 7 个调用者：`vmm.c:89,180; uaccess.c:226; vma.c:398,457; task.c:2194`；v1 review item 8），内部取目标快照 = `load_acquire(ip_i_ready) ∩ ~self`；等待条件 `!=`（不是 `<`）——32 位代数回绕单测已写在 §8.2；超时 FATAL `panic` 不静默继续；首发使用 LIST filter、不用 all-but-self。
- aarch64 双状态发布：`percpu_data[i].online`（锁生效，BSP 在 `smp_boot_aps` **之前**完成 `percpu_install_gs+percpu_init+store_release(online=1)`）+ `percpu_data[i].ipi_ready`（SGI 可响应）。BSP `ipi_ready=1` **必须**在 `arch_local_irq_enable()`（`kernel/arch/aarch64/boot/main.c:591`）之后、handler/IDT 可用、GIC target 表项就绪后置位（v1 review item 7）。`num_cpus` 在 `smp_boot_aps` 前 `store_release`；BSP/AP 均无回退。
- aarch64 SMP 启动**开始后**所有 aarch64 已发布 root 的映射变更必须等待全部目标 CPU `ipi_ready`：用一次性 `smp_starting` 相位位，**通过显式函数** `smp_starting_enter()` 发布（不让 caller 直接写 static 变量；v1 review item 13）；BSP 在首次可能发出 PSCI `CPU_ON` 之前调该函数。M1 pre-SMP 例外（M1 selftest 用低级 `aarch64_pt_*` 直接修改 TTBR1 root，允许本地 TLBI 后返回）。
- `ap_work[]` 工作槽：BSP 写 IDLE→READY→等 DONE→IDLE；AP 见 READY 且 seq 未消费过则执行 → store_release(DONE)。seq 首项 = 1，AP "最后已消费 seq" 初值 = 0。**超时 FATAL**：打印 `WORK-TIMEOUT cpu=%u seq=%u` 与 `M3-SHOOTDOWN-PROBE: FAIL work-timeout`，`for (;;) arch_cpu_halt()`。无取消/迟到完成握手。
- aarch64 公共头 `vmm_backend.h` 提供位位置 + `AARCH64_PT_SOFTWARE_PROTNONE/COW` + `AARCH64_PT_EPROT_NONE`。
- IPI 逻辑向量：复用 `kernel/include/intr/ipi.h` 已定义 `IPI_VECTOR_TLB = 0x40`；aarch64 `ipi.c` 在内部把 0x40 映射到 SGI 编号 3 + `gic_dev_current()` + `uint8_t targets`（逻辑→GIC target byte）。
- GIC handler 注册使用现成 3 参签名 `(uint32_t intid, uint64_t param, struct pt_regs *regs)`（`kernel/include/arch/aarch64/gic.h:15`）；handler **不**调 EOI——`gic_dev_dispatch`（`kernel/arch/aarch64/intr/gic_driver.c:188`）在 handler 返回后用完整 IAR 调 `gic_eoi`。
- `tlb.c` 同时编入 x86 与 aarch64；x86 `ipi_tlb_handler` 重写为"无条件 `flush_tlb()` → `atomic_fetch_add_release(&cpu->tlb_ack_gen, 1)` → 由 x86 dispatch EOI"，删旧 `tlb_wanted`/`tlb_ack`；x86 BSP `ipi_ready` 在 `smp_boot_aps` 内 `ipi_init()` 之后、AP 启动之前置位；AP 在 handler/IDT 可用且 `arch_local_irq_enable()` 之后置位。
- x86 不套用 `smp_starting` 门禁（x86 `num_cpus` 是注册数且 AP 启动失败可继续）。
- `vma.c`/`uaccess.c`/`task.c`（fork）M3 不编入 aarch64（gate 白名单已排除，零改动写死契约）。
- 链接后 `nm` 检查 aarch64 kernel **无** `vma_*`/`uaccess_*`/fork 符号（除 `vmm_*` 与 `arch_vmm_*`）。
- 任何结构/头改变后先 `make clean`，不依赖增量构建发现 ABI 变化（`AGENTS.md`）。
- 测试落 `hosttests/cases/`（hosttest）+ QEMU `make PROFILE=aarch64-clang test-aarch64 MODE=smp`（正式 KERNEL_SELFTEST 入口）+ QEMU 正式镜像（`make PROFILE=aarch64-clang aarch64-uefi-kernel` 后手启 QEMU `-smp 2`）。x86 回归：`qemutests/x86_64_systest_repeat.py` 5 连。
- IPI/GIC 实测基址来自 `dtb_gicd_base()`，非固定 `GIC_DIST_BASE`（v1 review item 11）。

## Review Focus

1. **`VM_NOCACHE` 别名违规**（同一 PA 同时有 Normal direct map 与 Device 别名）：spec §4.4.3 文档化为 caller 契约、DEBUG 构建加 PMM zone 交叉检查（F11 follow-up）；Task 18 测试只用 Device 窗口 PA；正常构建无强校验（静默风险）。
2. **BBM 持锁等 ack 的安全性**是通用规则（§5.4），覆盖 BBM 类 update、已发布 root 的 map/unmap 全部路径；正式构建无 I2 断言（IRQ 开断言），**M3.1 audit gate 是唯一防线**（Task 13 写 `docs/memory.md` 审计清单）——漏掉新增 caller 会无声破坏。
3. **`smp_starting` 阶段门禁的 M1 pre-SMP 例外**：`aarch64_m1_selftest` 在 M1 安装 TTBR1 后运行，期间允许本核失效；SMP 启动后变更路径必须等全部 `ipi_ready`。Task 23 gate 验证包含"pre-SMP 例外通过"与"SMP 后未就绪 fail"两端。
4. **`ap_work` 工作槽超时**：BSP 超时打印 `WORK-TIMEOUT ... M3-SHOOTDOWN-PROBE: FAIL work-timeout` 后**停机**，不取消、不复用。AP 迟到的 DONE 写入无人读。Task 9 工作项测试覆盖 timeout FATAL 路径。
5. **slab_init 幂等化回归**：aarch64 已有 boot reservation 标记 slab 帧 → slab_init 跳过 → 不 ++/--；x86 无预预留 → 全标 → using/free 计数与改动前一致。Task 2 hosttest x86 计数回归断言失败即发现行为偏移。

## 范围与拆分说明

Spec 同时覆盖 M2（Slab 接入）和 M3（运行期 VMM），但二者并非独立子系统：M3 的 `arch_vmm_*` 通过 slab 分配 4 KiB 表页；M3.1 的 audit gate 直接审查 M2 路径（`slab_lock` 持有 → vmm 调用）；`percpu_t` 一次性加 `ipi_ready + tlb_ack_gen` 同步影响两侧。本计划是单文件、分两阶段（M2 → M3），每阶段独立可交付（合并 M2 单独可用、合并 M3 在 M2 之上扩展）。如果拆分更顺手，可以拆成 `2026-10-03-aarch64-m2-slab.md`（Task 1-6）+ `2026-10-03-aarch64-m3-vmm.md`（Task 7-26），二者用 M2 完成的 commit 作为 M3 的新基线即可。

## 文件与任务依赖

**M2 阶段**：Task 1 → Task 2 → Task 3 → Task 4 → Task 5 → Task 6。
**M3.1 audit gate 必须通过才能进 M3.2**（Task 12）。

**M3 阶段**（v3 fix for item 18：统一 audit gate 在 Task 13；Task 14 的硬前置是 Task 13 验收通过）：Task 7 → Task 8 → **Task 9（percpu 字段早于 handler）** → Task 10 → Task 11 → **Task 12（tlb.c 协议 + x86 迁移）** → **Task 13（M3.1 audit gate）** → Task 14 → Task 15 → Task 16 → Task 17 → Task 18 → Task 19 → Task 20 → Task 21 → Task 22 → Task 23 → Task 24 → Task 25 → Task 26。

| 文件 | 职责 |
|---|---|
| `kernel/include/memory/slab.h` | **已存在**：`extern struct Slab_Cache kmalloc_cache_size[16]` 暴露 |
| `kernel/memory/slab.c` | aarch64 锁路径替换、slab_init 幂等化、`slab_layout_compute()` 实现 |
| `kernel/arch/aarch64/runtime/printk_stub.c` | `color_printk` ABI 改为 `(unsigned int FRcolor, unsigned int BKcolor, const char *fmt, ...)` 返回 `int`，与 `kernel/include/core/printk.h:53` 一致 |
| `kernel/arch/aarch64/runtime/slab_stub.c` | 删除 |
| `kernel/arch/aarch64/memory/early_arena.c` | arena 公式链扩展（§3.2）、preflight、候选扫描阶段**含** slab 段字节 |
| `kernel/arch/aarch64/memory/runtime_tree.c` | 从新 `table_base_pa` 取页（M1 验证期望零改动） |
| `kernel/include/memory/vmm.h` | 公共语义层：仅 `VM_*` + `arch_vmm_*` 语义 API |
| `kernel/include/arch/x86_64/pte.h` | 新建：x86 硬件 PTE 位 + `vmm_pt_walk`（裸 PTE） + `vmm_free_user_map` |
| `kernel/include/arch/<arch>/vmm_backend.h` | 新建：arch 私有位位置映射 + 软件位常量 |
| `kernel/memory/vmm.c` | x86 backend wrapper；保留 `:317-339` free/COW 逻辑 |
| `kernel/memory/tlb.c` | `tlb_shootdown()` 保持**无参**，串行化协议 + 目标快照 + 代数 ack（`!=`）+ 超时 FATAL；公共头同步 |
| `kernel/include/intr/ipi.h` | **已存在**：`IPI_VECTOR_TLB = 0x40`（不重定义） |
| `kernel/selftest/selftest.c` | **v4 fix for v3 review item 9**：白名单加入 + aarch64 main 调 `selftest_run_all()` |
| `kernel/arch/aarch64/intr/ipi.c` | `ipi_broadcast(vector, target_mask)`：逻辑向量 0x40 → SGI 3 + `gic_dev_current()` + `uint8_t targets`（逻辑→GIC target byte） |
| `kernel/arch/aarch64/intr/gic.c` | SGI 3 白名单加入；`gic_target_bit_init()` 读 `GICD_ITARGETSR0`（基址来自 `dtb_gicd_base()`） |
| `kernel/arch/aarch64/intr/trap.c` | TLB handler 注册 SGI 3，3 参签名；handler 不调 EOI（dispatch 负责） |
| `kernel/arch/aarch64/smp/smp.c`、`secondary_idle` | 移除 `#if OS01_SELFTEST` 门、生产开 IRQ + 工作项循环（保留 `secondary_idle(uint32_t cpu_id)` 签名） |
| `kernel/arch/aarch64/smp/ap_work.c` | `ap_work[]` 工作槽协议（state/seq） |
| `kernel/include/arch/aarch64/ap_work.h` | 对应公共头 |
| `kernel/arch/aarch64/smp/percpu.c` | memset 清零、`online` 发布；`ipi_ready` 在 §6.2b 时序点显式置位 |
| `kernel/arch/aarch64/memory/boot_direct_map.c` | **安装 TTBR1 之前** `aarch64_pt_root_publish(tree.root_pa)` |
| `kernel/arch/aarch64/boot/main.c` | `arch_vmm_init` 生产调用 + `M3-SHOOTDOWN-PROBE` 探针 |
| `kernel/include/percpu/percpu.h` | **共享**：`ipi_ready` + `tlb_ack_gen` + `PERCPU_DATA_SIZE` bump + `_Static_assert` |
| `kernel/arch/aarch64/head.S` | percpu stride 站点同步（**v4 fix for v3 review item 15**：不是 `boot/head.S`） |
| `kernel/arch/x86_64/intr/apic/ipi.c` | `ipi_tlb_handler` 重写（删旧 `tlb_wanted`/`tlb_ack`）+ ack gen 路径（**v4 fix for v3 review item 15**：不是 `x86_64/apic/ipi.c`） |
| `kernel/arch/x86_64/smp/boot.c` | x86 BSP/AP `ipi_ready` 发布点（DEBUG 断言 IF/handler/percpu） |
| `kernel/Makefile` | aarch64 白名单加 `memory/slab.c`、`memory/tlb.c` |
| `hosttests/cases/test_slab_*.c`、`hosttests/Makefile` | slab 幂等化、color_printk ABI、arena 公式、2 GiB 边界注入 |
| `hosttests/cases/test_vmm_*.c` | 语义层组合 + PROT_NONE + 软件位 round-trip + split + ack 回绕（**链接生产实现**而非仅扫描文本） |
| `qemutests/aarch64_uefi_smp.py` | 单核/多核 selftest 解析（已存在，含 `--expect-selftest`） |
| `qemutests/x86_64_systest_repeat.py` | x86 5 连回归（已存在） |

---

## M2 阶段：aarch64 Slab 接入

### Task 1: slab 锁路径替换 + slab_layout_compute()

**v1 review items fixed:** item 1（slab.h 已存在，Modify；移除 `kmalloc_cache_size_array_size` 抽象；用 `kmalloc_cache_size[i].size`）

**Files:**
- Modify: `kernel/memory/slab.c:33-55`、`kernel/memory/slab.c:336-415`
- Modify: `kernel/include/memory/slab.h`（已存在；新增 `slab_layout_compute()` 声明）
- Test: `hosttests/cases/test_slab_layout_compute.c`（新建）、`hosttests/cases/test_slab_lock_path.c`（新建）

**Interfaces:**
- `struct slab_layout { uint64_t meta_bytes; uint64_t reserved_2m_pages; };`
- `struct slab_layout slab_layout_compute(void);` —— 只读 `kmalloc_cache_size[i].size` 与类型布局（`sizeof(struct Slab)`、`sizeof(long)`、color 公式 `align8((PAGE_2M / size) / 8)`），不触碰内存、不分配、不持锁。
- `slab_lock_acquire()` 返回 `arch_irq_state_t`，首句 `arch_irq_state_t flags = arch_local_irq_save();`；`slab_lock_release(flags)` 末尾 `arch_local_irq_restore(flags);`。`slab.c` 内不再有 `pushfq/cli/sti/popfq` 字面。

- [ ] **Step 1: 写 RED 测试 `test_slab_layout_compute.c`**

```c
#include "test_framework.h"
#include <memory/slab.h>
#include <stdint.h>
#include <kernel/pmm.h>  /* PAGE_2M_SIZE */

TEST_FUNC(test_meta_bytes_matches_spec_formula) {
    /* spec §3.2：meta = Σ_{i=0..15} [ sizeof(struct Slab) + 10*sizeof(long)
     *                          + align8(PAGE_2M / size_i / 8) + 10*sizeof(long) ] */
    struct slab_layout l = slab_layout_compute();
    uint64_t expected = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t entries = (uint64_t)PAGE_2M_SIZE / kmalloc_cache_size[i].size;
        uint64_t bm = ((entries + 7) / 8 + 7) & ~7ULL;  /* align8 */
        expected += sizeof(struct Slab) + 10 * sizeof(long)
                  + bm + 10 * sizeof(long);
    }
    assert_eq(l.meta_bytes, expected);
    assert_eq(l.reserved_2m_pages, 8);
}

TEST_FUNC(test_layout_is_pure) {
    /* 不分配、不触碰全局 PMM 状态；两次调用结果一致 */
    struct slab_layout a = slab_layout_compute();
    struct slab_layout b = slab_layout_compute();
    assert_eq(a.meta_bytes, b.meta_bytes);
}
TEST_LIST_BEGIN
    TEST_ENTRY(test_meta_bytes_matches_spec_formula),
    TEST_ENTRY(test_layout_is_pure),
TEST_LIST_END
int main(void) { RUN_ALL_TESTS(); return __test_stats.failed > 0 ? 1 : 0; }
```

注册 `test_slab_layout_compute.elf` 到 `hosttests/Makefile` 的 `TEST_BINS`，链接真实 `slab.c`（`SLAB_HOST_CFLAGS` 模式：shadow `<arch/spinlock.h>`/`<arch/cpu.h>`/`<arch/irq.h>` 走现有 `mock_kernel.o`，但必须保留 `arch_local_irq_save/restore` 的真实符号解析——按需新增 `SLAB_HOST_CFLAGS` 包含 `<arch/irq.h>` 的 host 桩头）。`make PROFILE=x86_64-clang test-host` 期望失败：`slab_layout_compute()` 未声明。

- [ ] **Step 2: 写 RED 测试 `test_slab_lock_path.c`**

源级扫描（`test_slab_lock_path.elf`，v3 fix for item 12）：避免裸子串 `cli`/`sti` 误报普通标识符或注释；只匹配**内联汇编字符串中的独立指令 token**（`__asm__ __volatile__("...cli...")` / `"...sti..."` / `"...pushfq..."` / `"...popfq..."`），正则如 `"[^"]*\b(pushfq|cli|sti|popfq)\b[^"]*"`；同时断言 `"arch_local_irq_save"` 与 `"arch_local_irq_restore"` 出现在源中。`make PROFILE=x86_64-clang test-host` 当前应 RED（`slab.c:39`、`slab.c:49` 内联汇编仍在）。

- [ ] **Step 3: `slab.h` 声明 + `slab.c` 实现 `slab_layout_compute()`**

`kernel/include/memory/slab.h` 末尾追加：

```c
struct slab_layout {
    uint64_t meta_bytes;
    uint64_t reserved_2m_pages;
};
struct slab_layout slab_layout_compute(void);
```

`kernel/memory/slab.c` 末尾追加：

```c
#include <arch/aarch64/mmu.h>  /* PAGE_2M_SIZE 的统一来源；x86 也提供同名宏 */

struct slab_layout slab_layout_compute(void) {
    /* spec §3.2：纯公式，不分配、不持锁、不写 PMM 状态。
     * 只读 kmalloc_cache_size[].size（kernel/include/memory/slab.h:34）。 */
    uint64_t meta = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t entries = (uint64_t)PAGE_2M_SIZE / kmalloc_cache_size[i].size;
        uint64_t bm = ((entries + 7ULL) / 8 + 7ULL) & ~7ULL;
        meta += sizeof(struct Slab) + 10 * sizeof(long)
              + bm + 10 * sizeof(long);
    }
    return (struct slab_layout){ .meta_bytes = meta, .reserved_2m_pages = 8 };
}
```

`make clean && make PROFILE=x86_64-clang test-host`：测试 1 GREEN（值等于 spec 公式精确值）、测试 2 GREEN（幂等）。如 `PAGE_2M_SIZE` 在 `<kernel/pmm.h>` 与 `<arch/aarch64/mmu.h>` 命名不同，按实际可用宏调整（host test 已链接真实 `slab.c`，同一宏来源）。

- [ ] **Step 4: 锁路径替换**

`slab.c:33-55` 替换：

```c
#include <arch/irq.h>   /* arch_local_irq_save/restore 与 arch_irq_state_t */
/* 删除原内联汇编 pushfq; popq; cli / sti */

static inline arch_irq_state_t slab_lock_acquire(void) {
    arch_irq_state_t flags = arch_local_irq_save();
    if (percpu_data[0].online) {
        uint32_t cpu = cpu_id();
        if (slab_lock_depth[cpu]++ == 0)
            spin_lock(&slab_lock);
    }
    return flags;
}

static inline void slab_lock_release(arch_irq_state_t flags) {
    if (percpu_data[0].online) {
        uint32_t cpu = cpu_id();
        if (--slab_lock_depth[cpu] == 0)
            spin_unlock(&slab_lock);
    }
    arch_local_irq_restore(flags);
}
```

`grep -nE "pushfq|cli|sti|popfq" kernel/memory/slab.c` 应**仅**命中注释或别处（`test_slab_lock_path.elf` 源级断言覆盖）。`make PROFILE=x86_64-clang test-host` 全 PASS（含原 `test_slab_basic`）。

- [ ] **Step 5: 提交 Task 1**

```bash
git add kernel/memory/slab.c kernel/include/memory/slab.h \
        hosttests/Makefile hosttests/cases/test_slab_layout_compute.c \
        hosttests/cases/test_slab_lock_path.c
git commit -m "refactor(slab): arch_irq_state_t and slab_layout_compute"
```

---

### Task 2: slab_init j-loop 与 8 页循环幂等化

**Files:**
- Modify: `kernel/memory/slab.c`（`slab_init` 的 j-loop、8 页循环）
- Test: `hosttests/cases/test_slab_idempotent_reservation.c`（新建）、x86 计数回归断言

**Interfaces:**
- `slab_init` 内部对每个 2 MiB frame 调用前先查 `bits_map`（PA-relative index）；位已置 → 跳过、不 ++/--；未置 → 标记 + 记账（行为与当前一致）。

- [ ] **Step 1: 写 RED 测试 `test_slab_idempotent_reservation.c`**

链接真实 `pmm.c` + `slab.c`（`PMM_SHADOW_INC` 屏蔽 cli/sti，照搬 `test_pmm_ram_rel_index.c` 的 shadow 模式）。测试：
1. 准备 PMM：覆盖 `bits_map` 中**预定 slab 8 页**的对应 bit（模拟 boot reservation 已置）；
2. 调用 `slab_init()`，断言 8 页循环对每个 frame 只看到"已置位"、`zone_struct->page_using_count` 不递增；
3. 关闭预定覆盖，再调用一次 `slab_init()`（不同起点），断言每 frame 仍只记账一次（8 页循环 `using_count` 增量 = 8）。

RED：当前 slab_init 无条件 OR + ++/-- → using_count 增量 = 16。

- [ ] **Step 2: 幂等化 8 页循环**

`slab.c` 内 8 页循环：

```c
for (i = 0; i < 8; i++) {
    uint64_t page_addr = slab_page_start + page_offset;
    page_offset += PAGE_2M_SIZE;
    page = Virt_To_2M_Page((uint64_t *)page_addr);
    uint64_t page_index = (uint64_t)(page - PMMngr.pages_struct);
    uint64_t bm_word = page_index >> 6;
    uint64_t bm_bit  = 1UL << (page_index & 63);
    if (PMMngr.bits_map[bm_word] & bm_bit) {
        /* 已被 boot reservation 标记：跳过记账与 page_init */
        kmalloc_cache_size[i].cache_pool->page = page;
        kmalloc_cache_size[i].cache_pool->address = (uint64_t *)page_addr;
        continue;
    }
    PMMngr.bits_map[bm_word] |= bm_bit;
    page->zone_struct->page_using_count++;
    page->zone_struct->page_free_count--;
    page_init(page, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
    kmalloc_cache_size[i].cache_pool->page = page;
    kmalloc_cache_size[i].cache_pool->address = (uint64_t *)page_addr;
}
```

j-loop 同样包一层 `if (!(bits_map[idx>>6] & (1UL << (idx&63))))`。`make PROFILE=x86_64-clang test-host` 应 GREEN。

- [ ] **Step 3: x86 计数回归断言**

扩展 `test_slab_basic.elf` 或新增 `test_slab_basic_x86_count.c`：模拟 x86 路径（`bits_map` 全 0 → 无 reservation），调用 `slab_init()`，断言 `using_count` 增量 = 8 + j-loop 命中数；与改动前的数值对比（spec §3.2："x86 无预预留 → 位全 0 → 全标记，行为不变"）。如果记录不到改动前数值，加一个 `_before` 分支 cherry-pick 改动前的 `slab_init` 进同一 TU 内 static 函数，先用 `_before` 跑一次记录基线、再用生产 `slab_init` 跑一次断言一致。

- [ ] **Step 4: 提交 Task 2**

```bash
git add kernel/memory/slab.c hosttests/Makefile \
        hosttests/cases/test_slab_idempotent_reservation.c
git commit -m "refactor(slab): make slab_init idempotent vs boot reservations"
```

---

### Task 3: arena 公式链扩展 + preflight + 候选扫描含 slab 段

**v1 review items fixed:** item 3（候选扫描先估算再选，含 slab 段字节）、item 1（路径 `kernel/arch/aarch64/memory/early_arena.c`）

**Files:**
- Modify: `kernel/arch/aarch64/memory/early_arena.c`
- Modify: `kernel/include/arch/aarch64/early_arena.h`（`struct aarch64_m1_arena` 加字段）
- Test: `hosttests/cases/test_arena_layout_chain.c`（新建）、`hosttests/cases/test_arena_2gib_boundary.c`（新建）、`hosttests/cases/test_arena_candidate_slab_bytes.c`（新建）

**Interfaces:**
- `struct aarch64_m1_arena` 新增 `slab_meta_bytes`、`slab_page_start_pa`、`slab_page_end_pa` 字段。
- 候选扫描阶段（`early_arena.c:265-290` 一带）`arena_bytes` 计算公式改为：
  ```
  arena_bytes = round_up_2M(round_up_4K(metadata_bytes)
                          + slab_layout_compute().meta_bytes
                          + 8 * 2 MiB
                          + table_pages * 4 KiB)
  ```
- preflight（在 arena 写入前）检查：`arena_end ≤ 2 GiB`（M0 覆盖）；`slab_page_end ≤ 2 GiB`；公式链各步无回退/溢出。

- [ ] **Step 1: 写 RED 测试**

`test_arena_layout_chain.c`：输入 R 含低窗口一段 64 MiB（足够装 arena + slab 段）、B、D；调 `aarch64_m1_plan`；断言：
- `arena.slab_meta_bytes == slab_layout_compute().meta_bytes`；
- `arena.slab_page_start_pa == align_up_2M(arena.base_pa + arena.layout.end_of_struct_off + slab_meta_bytes)`；
- `arena.slab_page_end_pa == slab_page_start_pa + 8 * 2 MiB`；
- `arena.table_base_pa == slab_page_end_pa`（已 2M 对齐）；
- `arena.end_pa == align_up_2M(table_base_pa + table_pages * 4 KiB)`。

`test_arena_2gib_boundary.c`：注入 R 段使 `slab_page_end_pa > 2 GiB` → 期望 preflight FATAL（kputs 含 "FATAL: arena exceeds 2 GiB"）。

`test_arena_candidate_slab_bytes.c`（v4 fix for v3 review item 14：直接用合法对齐范围）：
- 第一区间 `[0x40200000, 0x40400000)` (2 MiB, 装不下完整 arena——含 slab 段字节 + 对齐空隙)
- 第二区间 `[0x50000000, 0x60000000)` (256 MiB, 足够)

调 `aarch64_m1_plan`；断言选第二个区间。当前实现会把 2 MiB 当作"够装 metadata"而误选 → RED。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: `early_arena.c` 公式链扩展 + 候选扫描含 slab 段（v3 fix for item 1）**

**核心修复（item 1）**：候选扫描与最终落位**必须共用同一套带溢出检查的公式**，逐步计算 `slab_meta_end → align_up_2M → slab_page_end → table_end → arena_end`，包含元数据末尾到下一个 2 MiB 边界的对齐空隙。

`kernel/arch/aarch64/memory/early_arena.c`：

```c
#include <memory/slab.h>  /* slab_layout_compute */

/* 唯一事实来源（v3 fix for item 1）：从 base_pa 起逐步推导 arena_end，
 * 每步 checked overflow；候选扫描用同一函数计算下限 */
static int compute_arena_end(uint64_t base_pa,
                             const struct pmm_layout *layout,
                             size_t table_pages,
                             struct slab_layout sl,
                             uint64_t *end_pa_out,
                             uint64_t *slab_meta_end_out,
                             uint64_t *slab_page_start_out,
                             uint64_t *slab_page_end_out,
                             uint64_t *table_base_out) {
    uint64_t meta_end, slab_meta_end, slab_start, slab_end;
    uint64_t table_bytes, table_end, arena_end;

    /* meta_end = base_pa + layout->end_of_struct_off */
    if (!checked_add(base_pa, layout->end_of_struct_off, &meta_end))
        return -EOVERFLOW;

    /* slab_meta_end = meta_end + sl.meta_bytes */
    if (!checked_add(meta_end, sl.meta_bytes, &slab_meta_end))
        return -EOVERFLOW;
    if (slab_meta_end_out) *slab_meta_end_out = slab_meta_end;

    /* slab_page_start = align_up_2M(slab_meta_end) —— 必须计入对齐空隙
     * （v3 fix for item 1 + v4 fix for v3 review item 7：checked add 之上
     * 额外加 checked align） */
    if (!checked_align_up(slab_meta_end, PAGE_2M, &slab_start))
        return -EOVERFLOW;
    if (slab_page_start_out) *slab_page_start_out = slab_start;

    /* slab_page_end = slab_page_start + 8 * 2 MiB（checked mul + add） */
    if (!checked_mul(8, (uint64_t)PAGE_2M_SIZE, &slab_end) ||
        !checked_add(slab_start, slab_end, &slab_end))
        return -EOVERFLOW;
    if (slab_page_end_out) *slab_page_end_out = slab_end;

    /* table_base = slab_page_end */
    if (table_base_out) *table_base_out = slab_end;

    /* table_end = slab_page_end + table_pages * 4 KiB（checked mul + add） */
    if (!checked_mul((uint64_t)table_pages, PAGE_4K, &table_bytes) ||
        !checked_add(slab_end, table_bytes, &table_end))
        return -EOVERFLOW;

    /* arena_end = align_up_2M(table_end) */
    if (!checked_align_up(table_end, PAGE_2M, &arena_end))
        return -EOVERFLOW;
    if (end_pa_out) *end_pa_out = arena_end;
    return 0;
}

/* 候选扫描阶段：调用 compute_arena_end(0, ...) 仅取 arena_bytes 估值 */
size_t table_pages_estimate = ...;  /* 同 M1 plan 的桶计数 */
struct slab_layout sl = slab_layout_compute();
uint64_t arena_end_est = 0;
if (compute_arena_end(0, &layout, table_pages_estimate, sl,
                      &arena_end_est, NULL, NULL, NULL, NULL) < 0)
    return -EOVERFLOW;
uint64_t arena_bytes_est = arena_end_est;  /* base=0 时等于 end */
```

候选扫描循环（`early_arena.c:285` 一带）：`e_hi - s_lo < arena_bytes_est` 过滤区间——含 slab 段 + 对齐空隙（item 1）。

中选后**真实落位**：用 `compute_arena_end(cand_base, &layout, table_pages_real, sl, ...)` 写 `arena->end_pa` 等字段；失败 → FATAL。

`prepare` 内 preflight（写入前）：`arena->end_pa > (1UL << 31)` → 打印需求/可用，`for (;;) arch_cpu_halt()`；同上 `slab_page_end_pa > 2 GiB`。

- [ ] **Step 3: preflight 验证检查表 + hosttest GREEN（v3 fix for item 13：测试输入对齐）**

`prepare` 失败路径前断言：区间排序、合并相邻、未对齐/重叠 → FATAL；R∩`[0x40200000,0x80000000)` 候选 canary 写入 `PMMngr.start_brk` 仅在所有 check 通过后；`arena->end_pa` 与任何 R 区间上界比较溢出用 checked helper（参见 M1 plan Task 1 `pmm_layout_calculate`）。

**`test_arena_candidate_slab_bytes.c` 输入修正（v3 fix for item 13）**：原 `[0x40200000, 0x40500000)` 终点非 2 MiB 对齐，会被 early_arena 输入校验先拒。改用两端均对齐、第一段仍不足容纳完整 arena 的区间：`[0x40200000, 0x40400000)` (2 MiB, 装不下完整 arena) + `[0x50000000, 0x60000000)` (256 MiB, 足够)。断言失败原因**不**是输入校验（mock 输入校验通过），而是候选筛选。

`make PROFILE=x86_64-clang test-host CASE=arena`：RED → GREEN。

- [ ] **Step 4: 提交 Task 3**

```bash
git add kernel/arch/aarch64/memory/early_arena.c \
        kernel/include/arch/aarch64/early_arena.h \
        hosttests/Makefile hosttests/cases/test_arena_layout_chain.c \
        hosttests/cases/test_arena_2gib_boundary.c \
        hosttests/cases/test_arena_candidate_slab_bytes.c
git commit -m "feat(aarch64): extend arena layout for slab pages with 2GiB guard (compute_arena_end shared by candidate and final)"
```

---

### Task 4: table_base_pa 移位 + M1 期望更新

**Files:**
- Modify: `kernel/arch/aarch64/memory/runtime_tree.c`（验证零改动；如必要更新从新 base 取页）
- Modify: `hosttests/cases/test_m1_*.c`（期望值更新：arena 总长变大）
- Test: `hosttests/cases/test_arena_table_base_shift.c`（新建）

**Interfaces:** 不变（`runtime_tree.c` 消费 `table_base_pa`/`table_end_pa`，构造时传入）。

- [ ] **Step 1: 写 RED 测试 `test_arena_table_base_shift.c`**

给定 R 32 MiB（低窗口）；调 `aarch64_m1_plan`；断言 `table_base_pa == slab_page_end_pa`；给 `runtime_tree_build` 注入 fake pool ops，断言 builder 取页第一项 PA ∈ `[table_base_pa, table_end_pa)`。`make PROFILE=x86_64-clang test-host CASE=arena-table-base` RED（当前 `table_base_pa` = PMM 元数据末尾）。

- [ ] **Step 2: 验证 `runtime_tree.c` 零改动**

读 `runtime_tree.c` builder 入口：若 builder 仅消费 base/end 而非依赖 base 的具体来源（PMM 元数据末尾 vs slab 页之后），零改动即 GREEN；否则更新 base 来源。hosttest 必须 GREEN。

- [ ] **Step 3: 更新 M1 测试期望**

`test_m1_arena.c`、`test_m1_arena_exhaust.c`、`test_m1_reservation.c`、`test_m1_layout.c`：arena 尺寸变大后，原期望值（`end_of_struct_off`、`arena_end`、canary、layout 总长）需更新。逐文件跑 hosttest 直到 GREEN。

- [ ] **Step 4: 提交 Task 4**

```bash
git add kernel/arch/aarch64/memory/runtime_tree.c hosttests/Makefile \
        hosttests/cases/test_arena_table_base_shift.c \
        hosttests/cases/test_m1_*.c
git commit -m "refactor(mm): shift table base past slab pages and refresh M1 expectations"
```

---

### Task 5: aarch64 build 接入 + color_printk ABI 修正

**v1 review items fixed:** item 4（`kputs` 返回 void，不能 `return kputs(fmt)`；无 `runtime/kputs.h`；hosttest 链接真实 aarch64 stub + mock `kputs`）

**Files:**
- Modify: `kernel/Makefile`（aarch64 白名单加 `memory/slab.c`）
- Delete: `kernel/arch/aarch64/runtime/slab_stub.c`
- Modify: `kernel/arch/aarch64/runtime/printk_stub.c`
- Test: `hosttests/cases/test_aarch64_color_printk_abi.c`（新建）

**Interfaces:**
- `int color_printk(unsigned int FRcolor, unsigned int BKcolor, const char *fmt, ...)`（与 `kernel/include/core/printk.h:53` 一致，**修正**当前 aarch64 stub 的 `void color_printk(const char *fmt, ...)` 单参签名）。
- aarch64 stub 忽略 fg/bg/变参、用 `kputs(fmt)` 输出字面、返回字节数（`kputs` 返回 void，需用 `strlen(fmt)` 单独算字节数）。

- [ ] **Step 1: 写 RED 测试 `test_aarch64_color_printk_abi.c`**

```c
#include "test_framework.h"
#include <core/printk.h>  /* 期望签名 */
#include <arch/aarch64/boot_log.h>  /* kputs mock 桩 */
#include <string.h>

extern void mock_kputs_clear(void);
extern const char *mock_kputs_last(void);

TEST_FUNC(test_color_printk_signature_matches_printk_h) {
    /* 编译期已通过 #include <core/printk.h> 校验；
     * 运行时取函数指针确认 ABI 一致 */
    int (*fp)(unsigned int, unsigned int, const char *, ...) = color_printk;
    assert_true(fp != NULL);
}

TEST_FUNC(test_color_printk_does_not_deref_colors) {
    /* 注入：fg/bg 设为不合法值（如 0xdeadbeef），不应解引用为指针 */
    mock_kputs_clear();
    int rc = color_printk(0xdeadbeef, 0xcafebabe, "M2-SLAB-ERR: ok\n");
    /* kputs 返回 void，color_printk 应返回字节数 */
    assert_eq(rc, (int)strlen("M2-SLAB-ERR: ok\n"));
    /* mock 收到的字符串是字面 fmt */
    assert_true(strcmp(mock_kputs_last(), "M2-SLAB-ERR: ok\n") == 0);
}
TEST_LIST_BEGIN
    TEST_ENTRY(test_color_printk_signature_matches_printk_h),
    TEST_ENTRY(test_color_printk_does_not_deref_colors),
TEST_LIST_END
int main(void) { RUN_ALL_TESTS(); return __test_stats.failed > 0 ? 1 : 0; }
```

新增 hosttest mock：`hosttests/mock/aarch64_color_printk/kputs_mock.c`，提供 `mock_kputs_clear/last` 与一个 `void kputs(const char *s)`（捕获最后调用）；新 shadow 头 `<arch/aarch64/boot_log.h>` 放在 `hosttests/mock/aarch64_color_printk/`。

`make PROFILE=x86_64-clang test-host` RED（当前 aarch64 `color_printk(const char *fmt, ...)` 单参与公共头不匹配）。

- [ ] **Step 2: 修 `printk_stub.c` ABI**

```c
/* kernel/arch/aarch64/runtime/printk_stub.c */
#include <core/printk.h>           /* 公共签名 */
#include <arch/aarch64/boot_log.h> /* kputs, void return */
#include <string.h>

int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...) {
    (void)FRcolor; (void)BKcolor;
    /* 不引入完整格式化器；spec §3.4 不要求完整 format。
     * 用 kputs 输出字面格式串并返回字节数。
     * 注意 kputs 返回 void（kernel/include/arch/aarch64/boot_log.h:9），
     * 必须用 strlen(fmt) 单独算字节数。 */
    kputs(fmt);
    return (int)strlen(fmt);
}
```

`make PROFILE=x86_64-clang test-host` GREEN。

- [ ] **Step 3: 白名单加 `slab.c`、删 stub**

`kernel/Makefile:39-47` aarch64 `KERNEL_C_SOURCES` 加 `memory/slab.c`。`git rm kernel/arch/aarch64/runtime/slab_stub.c`。`make PROFILE=aarch64-clang kernel` 期望编入（slab.c 真实编译），处理新增符号依赖（如 `arch_local_irq_save/restore` 在 aarch64 `arch/irq.h` 存在；如缺，添加）。

- [ ] **Step 4: 提交 Task 5**

```bash
git rm kernel/arch/aarch64/runtime/slab_stub.c
git add kernel/Makefile kernel/arch/aarch64/runtime/printk_stub.c \
        hosttests/Makefile \
        hosttests/mock/aarch64_color_printk/ \
        hosttests/cases/test_aarch64_color_printk_abi.c
git commit -m "feat(aarch64): wire real slab into build and fix color_printk ABI"
```

---

### Task 6: M2 QEMU selftest + x86 回归

**v1 review items fixed:** item 15（`make aarch64-uefi-kernel-selftest` 不存在；用 `test-aarch64 MODE=smp` + 已有 `aarch64_uefi_smp.py`；x86 用 `x86_64_systest_repeat.py`）

**Files:**
- Modify: `qemutests/aarch64_uefi_smp.py`（已含 `--expect-selftest`；新增 slab 16 缓存 PASS 行解析）
- Create: `kernel/selftest/test_slab_selftest.c`（按现有 `kernel/selftest/selftest.c` 模式注册入口）
- Test: x86 `qemutests/x86_64_systest_repeat.py` 5 连

- [ ] **Step 1: 启动断言 `end_of_struct ≤ slab_meta_end`（v3 fix for item 2：返回绝对地址 + x86 上界来源核对）**

`kernel/memory/slab.c` `slab_init` 末尾加：

```c
extern uint64_t PMMngr_end_of_struct_upper_bound(void);
assert(PMMngr.end_of_struct <= PMMngr_end_of_struct_upper_bound());
```

`PMMngr_end_of_struct_upper_bound()` 在 `kernel/memory/slab.c` 内实现（v3 fix for item 2）：

```c
/* v4 fix for v3 review item 8：x86 weak fallback 之前用 UINT64_MAX + meta_bytes
 * 会回绕成小地址、断言失败。改为：
 *   - aarch64：返回绝对 VA = slab_meta_start_va + sl.meta_bytes
 *   - x86：返回 (uint64_t)-1 让断言自动通过，且不参与加法
 * 两者都不再做可能溢出的加法。 */
extern uint64_t aarch64_m1_slab_meta_start_va(void);  /* 来自 early_arena.c 或 vmm_backend */

/* aarch64 强实现（在 kernel/arch/aarch64/memory/early_arena.c） */
uint64_t PMMngr_end_of_struct_upper_bound(void) {
    struct slab_layout sl = slab_layout_compute();
    uint64_t base = aarch64_m1_slab_meta_start_va();
    /* checked add 避免回绕 */
    uint64_t upper;
    if (!checked_add(base, sl.meta_bytes, &upper))
        return (uint64_t)-1;
    return upper;
}

/* x86 weak stub（在 kernel/memory/slab.c 同文件，用 __weak__ 标注） */
__attribute__((weak)) uint64_t PMMngr_end_of_struct_upper_bound(void) {
    return (uint64_t)-1;  /* x86 跳过此 aarch64 专用断言 */
}
```

x86 路径不参与 aarch64 slab 断言；链接器优先选 strong 实现，weak 仅作为兜底。

- [ ] **Step 2: aarch64 selftest `test-aarch64 MODE=smp` 启动到 M1 后无 panic**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

`-smp 2` 启动：M1 BSP PASS + slab_init ok + 启动断言通过；串口不出现 panic、`#PF`、`#UD`。`make clean` 后跑一次。`aarch64_uefi_smp.py` 解析器 `--expect-selftest=True` 已支持。

- [ ] **Step 3: 16 缓存大小 kmalloc/kfree selftest（v3 fix for item 10：同步更新白名单）**

`kernel/selftest/test_slab_selftest.c`（按现有 `kernel/selftest/` 模式注册入口 `test_slab_selftest`）：依次对 `kmalloc_cache_size[0..15]` 做 `kmalloc(size) → memset pattern → kfree`；记录每缓存 using/free 增量；最后断言 16 缓存全部 PASS。`KERNEL_SELFTEST=1` 启动包含此 selftest；解析器断言 `[selftest] slab: 16/16 PASS`。

**白名单同步 + selftest runner 集成（v3 fix for item 10 + v4 fix for v3 review item 9）**：
- `kernel/Makefile:39` aarch64 `KERNEL_C_SOURCES` 列表追加 `selftest/selftest.c` + `selftest/test_slab_selftest.c`（同时检查既有 `selftest/test_arch_atomic_u64.c`、`test_aarch64_rndr_encoding.c` 仍在）。
- `kernel/arch/aarch64/boot/main.c` 在 M1 selftest 之后调 `selftest_run_all()`（x86 已在 `kernel/core/main.c:145` 调；aarch64 缺这步——仅编入白名单而不调，则符号进镜像但永远不跑）。
- selftest 入口需 `#ifdef KERNEL_SELFTEST`（参考 x86 模式；`selftest_run_all` 本身在非 selftest 构建为空函数）。
- 提交前先 `make clean`，符号验证：`nm kernel.elf | grep -E "test_slab_selftest|selftest_run_all"` 应非空。
- 串口解析器断言每项运行标记（不仅是符号存在，还要 `[selftest] slab: 16/16 PASS` 行）。

- [ ] **Step 4: x86 5 连回归（v3 fix for item 14：用现有 `test-syscall-repeat`）**

```bash
make PROFILE=x86_64-clang test-syscall-repeat
```

该 target 在 root `run.mk` 已存在，自动传递 `--disk <artifacts/disk.img>` 与 `--firmware <OVMF.fd>`。期望 5/5 PASS。

如需手跑（验证 CLI 参数）：

```bash
python3 qemutests/x86_64_systest_repeat.py \
  --disk build/x86_64-clang/artifacts/disk.img \
  --firmware /usr/share/edk2/x64/OVMF_CODE.fd
```

- [ ] **Step 5: 提交 Task 6**

```bash
git add kernel/memory/slab.c kernel/selftest/test_slab_selftest.c \
        kernel/Makefile \
        qemutests/aarch64_uefi_smp.py
git commit -m "test(slab): cover all 16 cache sizes on aarch64 and x86 regression"
```

---

## M3.1 阶段：shootdown 协议 + 双状态发布

### Task 7: SGI 3 + ipi_broadcast（逻辑向量 → SGI 映射）

**v1 review items fixed:** item 5（`IPI_VECTOR_TLB = 0x40` 已定义于 `kernel/include/intr/ipi.h:8`；不重定义；添加逻辑向量 → SGI 编号映射）

**Files:**
- Create: `kernel/arch/aarch64/intr/ipi.c`（如不存在则新建；`ipi_test.c` 旁路保留）
- Modify: `kernel/arch/aarch64/intr/gic.c`（SGI 3 加入白名单注释）
- Modify: `kernel/include/arch/aarch64/ipi.h`（新增公共头，导出映射函数）
- Test: `hosttests/cases/test_aarch64_ipi_broadcast.c`（新建）

**Interfaces:**
- 复用 `kernel/include/intr/ipi.h` 的 `IPI_VECTOR_TLB = 0x40`（不重定义）。
- **修改公共头签名（v3 fix for item 3）**：`void ipi_broadcast(uint8_t vector, int exclude_self)` → `void ipi_broadcast(uint32_t vector, uint64_t target_mask)`。x86 `apic/ipi.c:70` 同步改为按掩码逐 CPU 发送（target_mask bit i → `ipi_send(percpu_data[i].arch_processor_id, vector)`，跳过 `online==0`）；`tlb.c:39` 调用点从 `ipi_broadcast(IPI_VECTOR_TLB, 1)` 改为 `ipi_broadcast(IPI_VECTOR_TLB, build_target_mask_excl_self())`（`build_target_mask_excl_self` 见 Task 12 引入）。
- aarch64 私有映射 `IPI_VECTOR_TLB (0x40) → SGI 编号 3` 在 `kernel/arch/aarch64/intr/ipi.c` 内 `static const` 查表。
- aarch64 `ipi_broadcast(vector, target_mask)` 内部：vector == IPI_VECTOR_TLB → `gic_send_sgi(gic_dev_current(), 3, logical_to_gic_targets(mask), GICD_SGIR_FILTER_LIST)`。`gic_send_sgi` 签名为 `(struct gic_dev *, uint32_t sgi, uint8_t targets, uint8_t filter)`（`kernel/arch/aarch64/intr/gic_driver.c:136`），`targets` 是 `uint8_t` GIC target byte，不是 64-bit 掩码。
- `static uint8_t gic_target_bit[NR_CPUS]` —— 每 CPU GIC target byte 缓存（0 静态初始）。

**Foundational primitives（v3 fix for items 7, 8, 9：必须先于 Task 16 的首次使用）**：
- `aarch64_pt_root_publish(uint64_t root_pa)` / `aarch64_pt_root_is_published(const uint64_t *root)` —— 在 `kernel/arch/aarch64/memory/page_table.c` 实现（提前自 Task 20）。
- `void smp_starting_enter(void)` —— `kernel/arch/aarch64/memory/page_table.c` 暴露；单次发布、不回退。
- `void ipi_ready_publish_and_count(uint32_t cpu)` —— BSP 和 AP **均**使用同一函数（v3 fix for item 9：BSP 不再单独置 `ipi_ready=1`）。实现：`DEBUG_ASSERT(percpu_data[cpu].ipi_ready == 0); atomic_store_release(&percpu_data[cpu].ipi_ready, 1); atomic_fetch_add_release(&ipi_ready_count, 1);`。
- vmm 变更入口门禁 `vmm_gate_check()` —— `smp_starting == 0` 时允许 pre-SMP 例外；`smp_starting == 1` 时要求 `ipi_ready_count >= dtb_cpu_count()`（含 BSP）。**所有 `arch_vmm_*` 与 `aarch64_pt_*` 公开原语入口均调用** `vmm_gate_check()`（v3 fix for item 8：必须在 API 开放时同步就位）。

- [ ] **Step 1: 写 RED 测试 `test_aarch64_ipi_broadcast.c`**

链接真实 `gic_driver.c`（已有 mock MMIO）+ 新 `ipi.c`；测试：
1. **v4 fix for v3 review item 5**：测试用 `gic_target_bit_inject(cpu, byte)` 接口设置 `gic_target_bit[cpu_id]`（Task 7 提供 test-only 接口；不依赖 Task 8 的真实 GIC target 读取）；
2. `target_mask=0b10`（仅 CPU1）+ `vector=IPI_VECTOR_TLB` + `gic_target_bit_inject(1, 0x02)` → 调 `ipi_broadcast`，断言 mock GICD 中 `GICD_SGIR` 寄存器值 = `(3 << 0) | (0x02 << 16) | (filter=LIST=0)`；
3. `target_mask=0` → 断言不写 SGIR；
4. `vector == 0x41`（IPI_VECTOR_RESCHED，M3 暂不支持）→ 断言 panic。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 ipi_broadcast**

```c
/* kernel/arch/aarch64/intr/ipi.c */
#include <intr/ipi.h>            /* IPI_VECTOR_TLB = 0x40 等已定义 */
#include <arch/aarch64/ipi.h>     /* 本 arch 私有头 */
#include <arch/aarch64/gic.h>     /* gic_send_sgi, gic_dev_current, GICD_SGIR_FILTER_LIST */
#include <arch/aarch64/percpu.h>  /* gic_target_bit[] */
#include <stdint.h>

/* 逻辑向量 → SGI 编号映射（M3 仅 TLB；其余 panic） */
static uint32_t ipi_vector_to_sgi(uint32_t vector) {
    switch (vector) {
    case IPI_VECTOR_TLB: return 3;
    default: panic("ipi_vector_to_sgi: unsupported vector %u\n", vector);
    }
}

/* 逻辑 CPU 位图 → GIC target byte：按 gic_target_bit[] 查表求或 */
static uint8_t logical_to_gic_targets(uint64_t mask) {
    uint8_t out = 0;
    for (int cpu = 0; cpu < NR_CPUS; cpu++) {
        if (mask & (1UL << cpu))
            out |= gic_target_bit[cpu];
    }
    return out;
}

void ipi_broadcast(uint32_t vector, uint64_t target_mask) {
    uint32_t sgi = ipi_vector_to_sgi(vector);
    uint8_t targets = logical_to_gic_targets(target_mask);
    if (targets == 0) return;
    gic_send_sgi(gic_dev_current(), sgi, targets, GICD_SGIR_FILTER_LIST);
}
```

`gic.c:25-30` 注释加："SGI 3 reserved for IPI_VECTOR_TLB（M3 shootdown）"。

`make PROFILE=x86_64-clang test-host` GREEN。

- [ ] **Step 3: 提交 Task 7**

```bash
git add kernel/arch/aarch64/intr/gic.c kernel/arch/aarch64/intr/ipi.c \
        kernel/include/arch/aarch64/ipi.h \
        hosttests/Makefile hosttests/cases/test_aarch64_ipi_broadcast.c
git commit -m "feat(aarch64): IPI vector mapping (IPI_VECTOR_TLB 0x40 -> SGI 3) and ipi_broadcast"
```

- [ ] **Step 4: 公共 IPI 头迁移 + foundational primitives（v3 fix for items 3, 7, 8, 9 + v4 fix for v3 review item 3）**

**v4 fix for v3 review item 6（M1 root 注册提前）**：M1 安装 TTBR1 之前调 `aarch64_pt_root_publish(tree.root_pa)`——必须在 Task 7（本步）就把 `boot_direct_map.c` 的注册调用同步迁过来，否则 Task 16-19 后端的 `aarch64_pt_root_is_published()` 会把"正在使用"的 M1 root 误判为未发布、跳过相应失效协议。Task 20 之后仅作测试与回归。

**v4 fix for v3 review item 3（percpu 字段依赖）**：本步骤先在 `kernel/include/percpu/percpu.h` 加 `ipi_ready`/`tlb_ack_gen` 字段并 bump `PERCPU_DATA_SIZE` 144→152，让后续 `ipi_ready_publish_and_count` 引用 `percpu_data[cpu].ipi_ready` 不再是前向引用。`kernel/arch/aarch64/head.S:691` 的 `mov x8, #PERCPU_DATA_SIZE` 同步改 `#152`；`kernel/percpu/percpu.c` 的 memset 不触碰新字段（caller 显式置位）。Task 9 之后**仅**作 layout 校验 + hosttest 验收，不再放字段新增。

**公共头迁移（item 3）+ IPI 原子化（v4 fix for v3 review item 4）**：本步骤一次性改完 header + x86 impl + tlb.c 调用方 + aarch64 impl，**一个 commit 一个可构建快照**：

`kernel/include/intr/ipi.h:18`：

```c
/* v3: vector + logical-CPU mask; x86 与 aarch64 共用 */
void ipi_broadcast(uint32_t vector, uint64_t target_mask);
```

`kernel/arch/x86_64/intr/apic/ipi.c:70` 同步重写：

```c
void ipi_broadcast(uint32_t vector, uint64_t target_mask) {
    for (uint32_t i = 0; i < num_cpus; i++) {
        if (!(target_mask & (1UL << i))) continue;
        if (!percpu_data[i].online) continue;
        ipi_send(percpu_data[i].arch_processor_id, (uint8_t)vector);
    }
}
```

`tlb.c:39` 调用点：

```c
/* 旧：ipi_broadcast(IPI_VECTOR_TLB, 1); */
/* 新：mask 在 tlb_shootdown() 内 build_target_mask_excl_self() 后传入 */
```

**aarch64 foundational primitives（items 7, 8, 9）**：

`kernel/arch/aarch64/memory/page_table.c` 顶部新增：

```c
#include <arch/aarch64/dtb.h>  /* dtb_cpu_count */

#define AARCH64_PT_MAX_PUBLISHED_ROOTS 8
static uint64_t published_roots[AARCH64_PT_MAX_PUBLISHED_ROOTS];
static spinlock_T published_roots_lock = { .lock = 1UL };
static _Atomic uint32_t smp_starting = 0;
static _Atomic uint32_t ipi_ready_count = 0;

bool aarch64_pt_root_publish(uint64_t root_pa) { /* 同 v2 Task 20 实现 */ }
bool aarch64_pt_root_is_published(const uint64_t *root) { /* 同 v2 Task 20 实现 */ }

void smp_starting_enter(void) {
    uint32_t prev = atomic_exchange(&smp_starting, 1);
    DEBUG_ASSERT(prev == 0);
}

void ipi_ready_publish_and_count(uint32_t cpu) {
    DEBUG_ASSERT(percpu_data[cpu].ipi_ready == 0);
    atomic_store_explicit(&percpu_data[cpu].ipi_ready, 1,
                          memory_order_release);
    atomic_fetch_add_explicit(&ipi_ready_count, 1, memory_order_release);
}

void vmm_gate_check(void) {
    if (atomic_load_acquire(&smp_starting) == 0) return;  /* pre-SMP 例外 */
    uint32_t expected = dtb_cpu_count();
    uint32_t ready = atomic_load_acquire(&ipi_ready_count);
    if (ready < expected)
        panic("vmm: smp_starting=1 but ipi_ready_count=%u < cpu_count=%u\n",
              ready, expected);
}
```

所有 `arch_vmm_*` 与 `aarch64_pt_*` 公开原语入口首句加 `vmm_gate_check();`（v3 fix for item 8：与 API 开放同步就位）。

**单测覆盖**：

`hosttests/cases/test_foundational_primitives.c`（新建）：
1. `aarch64_pt_root_publish` 重复幂等、超限 panic、空 root 返回 false；
2. `smp_starting_enter` 单调不回退（第二次触发 BUG_ON）；
3. `ipi_ready_publish_and_count` 单次发布（重复触发 BUG_ON），BSP/AP 都用同一函数；
4. `vmm_gate_check`：mock `smp_starting=0` 通过；`smp_starting=1, ipi_ready_count<dtb_cpu_count` → panic；
5. `ipi_broadcast` 新签名：mock `target_mask=0b10` → x86 `apic/ipi.c` 调 `ipi_send(percpu_data[1].arch_processor_id, IPI_VECTOR_TLB)`；aarch64 `ipi.c` 调 `gic_send_sgi` 的 SGIR 寄存器值正确。

`make PROFILE=x86_64-clang test-host` 全 PASS（**编译** `kernel/arch/x86_64/intr/apic/ipi.c` 应通过；**编译** `kernel/arch/aarch64/memory/page_table.c` 应通过 —— 因为 Task 9 之前 `page_table.c` 已有 stub，foundation 层可叠加）。

- [ ] **Step 5: 提交 Task 7 完整**

```bash
git add kernel/include/intr/ipi.h kernel/arch/x86_64/intr/apic/ipi.c \
        kernel/memory/tlb.c \
        kernel/arch/aarch64/memory/page_table.c \
        hosttests/Makefile hosttests/cases/test_foundational_primitives.c
git commit -m "feat: migrate ipi_broadcast signature + foundational primitives (root registry, smp_starting, ipi_ready_count)"
```

注：后续 Task 20/23 的实现步骤**仅添加测试 + 集成调用点**，不再放实现（已在 Task 7 Step 4 实现）。

---

### Task 8: GIC target bit 实测构建（多核 + 单核例外）

**v1 review items fixed:** item 11（基址来自 `dtb_gicd_base()`，非固定 `GIC_DIST_BASE`；非恒等直接 FATAL，非 WARN）

**Files:**
- Modify: `kernel/arch/aarch64/intr/ipi.c`（`gic_target_bit_init()` 函数）
- Modify: `kernel/arch/aarch64/intr/gic.c`（暴露 `dtb_gicd_base()` 路径）
- Test: `hosttests/cases/test_aarch64_gic_target_probe.c`（新建）

**Interfaces:**
- `void gic_target_bit_init(uint32_t cpu_id);` —— 每核在 `ipi_ready` 之前调用（Task 11 时序）。

- [ ] **Step 1: 写 RED 测试**

`test_aarch64_gic_target_probe.c`：
1. mock `dtb_cpu_count()=1` → `gic_target_bit_init(0)` → 断言 `gic_target_bit[0] == 1u`（单核 RAZ/WI 例外）；
2. mock `dtb_cpu_count()=4` + mock GICD `ITARGETSR0`（基于 `dtb_gicd_base()`）byte 0 = `0x02`（CPU1 target bit）→ `gic_target_bit_init(1)` → 断言 `gic_target_bit[1] == 0x02`；
3. mock `dtb_cpu_count()=4` + byte 0 = `0x05`（多 bit，违反 QEMU virt 恒等拓扑）→ 期望 FATAL（非 WARN；v1 review item 11）；
4. 源码扫描断言：禁用固定 `GIC_DIST_BASE` 字面（`grep -nE "GIC_DIST_BASE\s*=" kernel/arch/aarch64/intr/ipi.c` 应空）；使用 `dtb_gicd_base()`。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 `gic_target_bit_init`**

```c
/* kernel/arch/aarch64/intr/ipi.c */
#include <arch/aarch64/dtb.h>  /* dtb_cpu_count, dtb_gicd_base */

void gic_target_bit_init(uint32_t cpu_id) {
    if (dtb_cpu_count() == 1) {
        /* spec §6.3：GICv2 单核 GICD_ITARGETSR0 可 RAZ/WI，不读校验 */
        gic_target_bit[cpu_id] = 1u;
        return;
    }
    /* 多核：读本核 banked GICD_ITARGETSR0（offset 0x800），
     * 取 SGI 对应 byte（SGIs are in ITARGETSR0..7 byte 0..3），
     * 要求恰好一个 bit 置位，否则 FATAL（QEMU virt 必须等于 1u << cpu）。
     * 基址来自 dtb_gicd_base()，非固定常量。 */
    volatile uint8_t *itargets =
        (volatile uint8_t *)((uintptr_t)dtb_gicd_base() + 0x800);
    uint8_t sgi_byte = itargets[0];  /* SGI 0..3 share byte 0 */
    if (sgi_byte == 0 || (sgi_byte & (sgi_byte - 1)) != 0) {
        panic("gic_target_bit_init cpu=%u unexpected ITARGETSR0=%x\n",
              cpu_id, sgi_byte);
    }
    gic_target_bit[cpu_id] = sgi_byte;
    if (sgi_byte != (1u << cpu_id)) {
        /* 非恒等拓扑留待 P4；当前 QEMU virt 必须相等 */
        panic("gic_target_bit_init cpu=%u non-identity topology bit=%x (expected %x)\n",
              cpu_id, sgi_byte, 1u << cpu_id);
    }
}
```

`make PROFILE=x86_64-clang test-host` GREEN。

- [ ] **Step 3: 提交 Task 8**

```bash
git add kernel/arch/aarch64/intr/ipi.c kernel/arch/aarch64/intr/gic.c \
        hosttests/Makefile hosttests/cases/test_aarch64_gic_target_probe.c
git commit -m "feat(aarch64): probe GIC target bits via dtb_gicd_base, FATAL on non-identity"
```

---

### Task 9: percpu_t 加 ipi_ready + tlb_ack_gen + PERCPU_DATA_SIZE bump（**早于 handler**）

**v1 review items fixed:** item 7（`kernel/include/percpu/percpu.h` 是**共享**头，**不**是 arch 私有；字段加到这里；spec §R11 `_Static_assert` 流程）

**v3 fix for item 4：**
- 类型名是 **`percpu_t`**（typedef，标签 `struct percpu`），**不**是 `struct percpu_t`。
- 真实文件路径：
  - `kernel/include/percpu/percpu.h`（头，已存在）
  - `kernel/arch/aarch64/head.S`（**不是** `kernel/arch/aarch64/boot/head.S`；后者不存在）
  - `kernel/percpu/percpu.c`（**不是** `kernel/arch/aarch64/smp/percpu.c`；后者不存在）

**Files:**
- Modify: `kernel/include/percpu/percpu.h`（共享头，加 `ipi_ready` + `tlb_ack_gen` + bump `PERCPU_DATA_SIZE`）
- Modify: `kernel/arch/aarch64/head.S`（stride 站点同步 —— 见 head.S:691 `mov x8, #PERCPU_DATA_SIZE`）
- Modify: `kernel/percpu/percpu.c`（memset 不触碰新字段；新字段由 caller 显式置位）
- Test: `hosttests/cases/test_percpu_layout.c`（新建）

**Interfaces:**
- `typedef struct percpu { ...; _Atomic uint32_t online; _Atomic uint32_t ipi_ready; _Atomic uint32_t tlb_ack_gen; } percpu_t;`（**v3 fix for item 4**：类型名是 `percpu_t`，**不**是 `struct percpu_t`；追加在末尾，**不**重排既有字段，避免偏移漂移）。
- `PERCPU_DATA_SIZE` 从 144 bump 到 152（或新值）；`_Static_assert(sizeof(percpu_t) == PERCPU_DATA_SIZE, ...)` 钉死。
- head.S stride 站点 `mov x8, #PERCPU_DATA_SIZE`（`kernel/arch/aarch64/head.S:691`）必须更新为 `#152`；保留 `mrs TPIDR_EL1, ...` 序列与 `ldr x7, =percpu_data`。

- [ ] **Step 1: 写 RED 测试 `test_percpu_layout.c`**

```c
#include <percpu/percpu.h>
TEST_FUNC(test_percpu_t_size_bumped) {
    /* spec §R11：碰 144 B 钉死布局必须 bump。断言 sizeof(percpu_t) > 144。 */
    assert_true(sizeof(percpu_t) >= 152);
}
TEST_FUNC(test_percpu_t_field_offsets_aligned) {
    assert_true(__builtin_offsetof(percpu_t, ipi_ready)    % 4 == 0);
    assert_true(__builtin_offsetof(percpu_t, tlb_ack_gen) % 4 == 0);
}
TEST_FUNC(test_static_assert_holds) {
    /* 编译期已生效；运行时仅取 sizeof 验证 */
    assert_true(sizeof(percpu_t) == PERCPU_DATA_SIZE);
}
```

`make PROFILE=x86_64-clang test-host` RED（当前 `ipi_ready`/`tlb_ack_gen` 字段不存在）。

- [ ] **Step 2: 改 percpu.h**

`kernel/include/percpu/percpu.h`：

```c
#include <stdatomic.h>

typedef struct percpu {
    /* 既有字段保持原顺序（percpu.h:47-...） */
    uint64_t self;              /* 0, 既有 */
    uint64_t need_resched;      /* 8, 既有 */
    uint32_t cpu_id;            /* 既有 */
    uint32_t arch_processor_id; /* 既有 */
    _Atomic uint32_t online;          /* 既有；改 _Atomic */
    uint32_t scheduler_ok;      /* 既有 */
    /* M3 新增： */
    _Atomic uint32_t ipi_ready;
    _Atomic uint32_t tlb_ack_gen;
} percpu_t;
#define PERCPU_DATA_SIZE 152          /* 从 144 bump */
_Static_assert(sizeof(percpu_t) == PERCPU_DATA_SIZE,
               "percpu_t size drift; update PERCPU_DATA_SIZE and asm stride sites");
```

- [ ] **Step 3: head.S stride 站点**

`kernel/arch/aarch64/head.S:691`：

```asm
- mov     x8, #PERCPU_DATA_SIZE  /* 原值 144 */
+ mov     x8, #152               /* 同步 PERCPU_DATA_SIZE */
```

保留 `mrs TPIDR_EL1, ...` 序列与 `ldr x7, =percpu_data`（head.S:690）。`make PROFILE=aarch64-clang kernel` 应无 link error / 无 layout 警告。

- [ ] **Step 4: percpu.c memset 行为**

`kernel/percpu/percpu.c` 内 `percpu_init(cpu)` 仅 memset，**不**写 `ipi_ready`/`tlb_ack_gen`（这两个字段由 caller 在 §6.2b 时序点显式置位——见 Task 11 调用 `ipi_ready_publish_and_count(cpu)`，由 Task 7 Step 4 实现）。

- [ ] **Step 5: 提交 Task 9**

```bash
git add kernel/include/percpu/percpu.h kernel/arch/aarch64/head.S \
        kernel/percpu/percpu.c \
        hosttests/Makefile hosttests/cases/test_percpu_layout.c
git commit -m "refactor(percpu): bump PERCPU_DATA_SIZE for ipi_ready and tlb_ack_gen"
```

---

### Task 10: TLB handler + secondary_idle 工作项循环

**v1 review items fixed:** item 6（GIC handler 用 3 参签名；handler 不调 EOI——dispatch 负责）；item 7（保留 `secondary_idle(uint32_t cpu_id)` 签名）

**Files:**
- Modify: `kernel/arch/aarch64/intr/trap.c`（TLB handler 注册 SGI 3）
- Modify: `kernel/arch/aarch64/smp/smp.c`（`secondary_idle` 重写）
- Create: `kernel/arch/aarch64/smp/ap_work.c`（ap_work 协议实现）
- Create: `kernel/include/arch/aarch64/ap_work.h`（arch 私有头；公共 IPI 头在 `kernel/include/intr/ipi.h`）
- Test: `hosttests/cases/test_ap_work_protocol.c`（新建）

**Interfaces:**
- `enum ap_work_state { AP_WORK_IDLE = 0, AP_WORK_READY, AP_WORK_DONE };`
- `enum ap_work_cmd { WORK_READ64 = 1, WORK_BARRIER = 2 };`
- `struct ap_work { _Atomic uint32_t state; uint32_t seq; uint32_t cmd; uint64_t arg0, arg1, out; };`
- `struct ap_work ap_work[NR_CPUS];`（BSS 静态，不放 percpu_t）
- `void ap_work_submit(uint32_t cpu, uint32_t seq, uint32_t cmd, uint64_t arg0, uint64_t arg1);`
- `bool ap_work_wait(uint32_t cpu, uint32_t seq, uint64_t *out, uint64_t deadline_cycles);`

- [ ] **Step 1: 写 RED 测试 `test_ap_work_protocol.c`**

mock 两侧（BSP mock + AP mock 用同一 `ap_work[]`）：
1. BSP submit（seq=1, cmd=READ64, arg=VA）→ AP 收到 seq=1 → AP 模拟读 → store_release(DONE) → BSP 等到 DONE、读 out → BSP 置 IDLE；
2. 第二次 submit seq=2 → AP 不会把旧的 DONE 当新请求（验证 seq 匹配）；
3. 超时：AP 不写 DONE → BSP `ap_work_wait` 触发 FATAL（验证打印 `WORK-TIMEOUT cpu=%u seq=%u` + `M3-SHOOTDOWN-PROBE: FAIL work-timeout` + `for (;;) arch_cpu_halt()`）；
4. seq 首项 = 1，AP "最后已消费 seq" 初值 = 0。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 `ap_work.c`**

```c
/* kernel/arch/aarch64/smp/ap_work.c */
#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/boot_log.h>  /* kputs/kputu */
#include <arch/cpu.h>               /* arch_cpu_halt, arch_cycle_counter, arch_cpu_pause */
#include <stdatomic.h>

struct ap_work ap_work[NR_CPUS];

void ap_work_submit(uint32_t cpu, uint32_t seq, uint32_t cmd,
                    uint64_t arg0, uint64_t arg1) {
    struct ap_work *slot = &ap_work[cpu];
    /* 普通 store 写 cmd/arg0/arg1/seq，最后 store_release(READY) */
    slot->cmd  = cmd;
    slot->arg0 = arg0;
    slot->arg1 = arg1;
    slot->seq  = seq;
    atomic_store_explicit(&slot->state, AP_WORK_READY,
                          memory_order_release);
}

bool ap_work_wait(uint32_t cpu, uint32_t seq, uint64_t *out,
                  uint64_t deadline_cycles) {
    struct ap_work *slot = &ap_work[cpu];
    uint64_t start = arch_cycle_counter();
    while (atomic_load_explicit(&slot->state,
                                memory_order_acquire) != AP_WORK_DONE) {
        if (arch_cycle_counter() - start > deadline_cycles) {
            /* spec §6.2 超时终态：打印并停机，不复用槽 */
            kputs("WORK-TIMEOUT cpu="); kputu(cpu);
            kputs(" seq=");              kputu(seq);
            kputs("\nM3-SHOOTDOWN-PROBE: FAIL work-timeout\n");
            for (;;) arch_cpu_halt();
        }
        arch_cpu_pause();
    }
    /* 验证 seq 匹配（防止读旧 DONE） */
    if (slot->seq != seq) return false;
    if (out) *out = slot->out;
    atomic_store_explicit(&slot->state, AP_WORK_IDLE,
                          memory_order_release);
    return true;
}

/* AP 端：在 secondary_idle 工作循环内调用 */
void ap_work_run_one(uint32_t cpu) {
    struct ap_work *slot = &ap_work[cpu];
    if (atomic_load_explicit(&slot->state, memory_order_acquire) != AP_WORK_READY)
        return;
    static uint32_t last_consumed_seq[NR_CPUS] = {0};
    if (slot->seq == last_consumed_seq[cpu]) return;  /* 旧请求，忽略 */
    switch (slot->cmd) {
    case WORK_READ64:
        slot->out = *(volatile uint64_t *)(uintptr_t)slot->arg0;
        break;
    case WORK_BARRIER:
        break;  /* 同步栅栏 */
    }
    last_consumed_seq[cpu] = slot->seq;
    atomic_store_explicit(&slot->state, AP_WORK_DONE,
                          memory_order_release);
}
```

- [ ] **Step 3: TLB handler 注册**

`kernel/arch/aarch64/intr/trap.c`：

```c
static void ipi_tlb_handler(uint32_t intid, uint64_t param, struct pt_regs *regs) {
    (void)intid; (void)param; (void)regs;
    /* 3 参签名（kernel/include/arch/aarch64/gic.h:15）；
     * handler 不调 EOI——gic_dev_dispatch（kernel/arch/aarch64/intr/gic_driver.c:188）
     * 在 handler 返回后用完整 IAR 调 gic_eoi。 */
    arch_flush_tlb_all();  /* vmalle1 + dsb sy + isb */
    atomic_fetch_add_explicit(&percpu_data[current_cpu_id()].tlb_ack_gen, 1,
                              memory_order_release);
}
/* gic_register_handler(3, ipi_tlb_handler) 在 gic_init 后调 */
```

- [ ] **Step 4: secondary_idle 尾部增加工作循环（v3 fix for item 5）**

**关键**：**不替换** `kernel/arch/aarch64/smp/smp.c:208-264` 既有 `secondary_idle` 主体——`cntp_ctl_el0_write(0)` / `gic_cpu_init` / `percpu_install_gs` / `percpu_init` / `aarch64_m1_ap_verify` / `boot_online_set` / `boot_go_get` / `smp_bench_iter` / selftest-only `arch_local_irq_enable` / `arch_cpu_halt` 全部保留。仅在 `for (;;) arch_cpu_halt();` 之前插入 M3 工作循环：

```c
void secondary_idle(uint32_t cpu_id)
{
    /* ... 既有 smp.c:215-250 全部保留 ... */
    if (cpu_id != AARCH64_SMP_TEST_NO_ACK_CPU)
        boot_online_set(cpu_id);
    uint32_t command;
    do {
        command = boot_go_get(cpu_id);
        if (command == AARCH64_BOOT_GO_WAIT) arch_cpu_pause();
    } while (command == AARCH64_BOOT_GO_WAIT);
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);

#if OS01_SELFTEST
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");
#endif

    /* v3 fix for item 5 + v4 fix for v3 review item 2：AP 必须先开 IRQ、
     * ISB、再发布 ipi_ready。否则发起方可能在开 IRQ 前的窗口发 SGI，AP
     * 不应答导致 timeout FATAL。顺序：handler 已注册 + GIC target 已初始化
     * + 开 IRQ + ISB → 然后 ipi_ready_publish_and_count。 */
    gic_target_bit_init(cpu_id);  /* Task 8 函数；单核也安全 */
    arch_local_irq_enable();  /* 生产开 IRQ（v1 review item 7：去掉 #if OS01_SELFTEST 门） */
    __asm__ __volatile__("isb" ::: "memory");
    ipi_ready_publish_and_count(cpu_id);  /* BSP/AP 同一函数；now-after-IRQ-enable */
    for (;;) {
        ap_work_run_one(cpu_id);  /* Task 10 Step 2 函数 */
        arch_cpu_pause();
    }
}
```

注：M3 生产构建**统一**在 `secondary_idle` 末尾开 IRQ，不再 `#if OS01_SELFTEST` 门控。M3 工作循环仅在 `boot_go_get` 之后进入，确保 AP 已对 BSP 完成 boot 在线信号。

- [ ] **Step 5: 提交 Task 10**

```bash
git add kernel/arch/aarch64/smp/ap_work.c kernel/include/arch/aarch64/ap_work.h \
        kernel/arch/aarch64/smp/smp.c kernel/arch/aarch64/intr/trap.c \
        hosttests/Makefile hosttests/cases/test_ap_work_protocol.c
git commit -m "feat(aarch64): TLB handler (3-arg sig) and ap_work protocol; append M3 tail to secondary_idle (preserve boot handshake)"
```

---

### Task 11: BSP percpu init 提前 + 双状态发布时序

**v1 review items fixed:** item 7（BSP `ipi_ready=1` **必须** 在 `arch_local_irq_enable()` 之后——`kernel/arch/aarch64/boot/main.c:591`）

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c`（`percpu_install_gs(0)+percpu_init(0)` 提前到 `smp_boot_aps()` 之前；`ipi_ready=1` 移到 `arch_local_irq_enable()` 之后、handler/IDT/GIC target 全就绪后）
- Modify: `kernel/arch/aarch64/smp/boot_percpu.c`（AP `secondary_idle` 内 `percpu_init(cpu)` → `store_release(online=1)` → 开 IRQ + target 表项就绪 → `store_release(ipi_ready=1)`）
- Test: `hosttests/cases/test_double_state_publish.c`（新建）

**Interfaces:** 不变（已有 percpu 字段，调用次序由代码控制）。

- [ ] **Step 1: 写 RED 测试 `test_double_state_publish.c`**

源码扫描 `main.c`：
- 断言 `percpu_install_gs(0)` 与 `percpu_init(0)` 在 `smp_boot_aps()` 调用之前；
- 断言 `arch_local_irq_enable()` 在 `store_release(percpu_data[0].ipi_ready = 1)` 之前（v1 review item 7）；
- DEBUG 断言：发布 `ipi_ready=1` 时 IF 已开、TLB handler 已注册、GIC target 表项已初始化；
- AP `secondary_idle`：断言 `arch_local_irq_enable()` 在 `store_release(percpu_data[cpu].ipi_ready = 1)` 之前。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 改 main.c 顺序**

```c
/* kernel/arch/aarch64/boot/main.c（伪代码，标识插入位置） */
/* ... preflight, pmm_init, arch_boot_direct_map_init, arch_vmm_init ... */

/* 提前 percpu init（纯内存初始化，无依赖；消灭"AP 已运行而 BSP 未 online"跳锁窗口）。
 * 当前 main.c:578-580 已有 percpu_install_gs(0)+percpu_init(0)，确认它在 smp_boot_aps() 之前。
 * 如不在，挪到这里。 */
percpu_install_gs(0);
percpu_init(0, mpidr_bsp);  /* 既有 main.c:578 附近 */
atomic_store_explicit(&percpu_data[0].online, 1, memory_order_release);

/* num_cpus 在 smp_boot_aps 前 store_release */
atomic_store_explicit(&num_cpus, dtb_cpu_count(), memory_order_release);

/* ... gic_init + handler 注册 + smp_boot_aps ... */

/* v4 fix for v3 review item 1：smp_starting_enter() 必须在 smp_boot_aps() 之前。
 * 约束："BSP 在首次可能发出 PSCI CPU_ON 之前置位"——意味着 smp_boot_aps()
 * 之前就是置位时机。Task 7 Step 4 已暴露 smp_starting_enter() 单次发布函数。 */
smp_starting_enter();  /* 一次性置位；门禁自此生效 */

/* 既有 BSP 启动顺序：preflight, pmm_init, arch_boot_direct_map_init,
 * arch_vmm_init, percpu_install_gs/init, gic_init, handler 注册 */
smp_boot_aps();   /* 既有 main.c:501 附近 */
arch_register_subsys();  /* 既有 main.c:548 附近 */
subsys_init_phase(SUBSYS_PHASE_4);
/* ... timer init ... */

if (!arch_tick_start()) {
    log_err("[smp] FATAL: BSP timer initialization failed\n");
    for (;;) arch_cpu_halt();
}
/* ... GIC target 表项 init、TLB handler 注册 ... */
arch_local_irq_enable();   /* 既有 main.c:591 */
__asm__ __volatile__("isb" ::: "memory");
/* 此刻可以发布 BSP ipi_ready=1（v3 fix for item 9：用同一函数，含计数 +1） */
DEBUG_ASSERT(irqs_enabled());
DEBUG_ASSERT(tlb_handler_registered());  /* 内部 bool */
ipi_ready_publish_and_count(0);  /* BSP 也走 Task 7 Step 4 的同一函数 */

- [ ] **Step 3: AP 端（已与 Task 10 Step 4 合并）**

`secondary_idle` 尾部（Task 10 Step 4）：`boot_go_get` → 既有 `arch_cpu_halt` 替换为 `ipi_ready_publish_and_count(cpu_id)` + `arch_local_irq_enable()` + 工作循环（v3 fix for item 5：保留所有既有 handshake）。

- [ ] **Step 4: 提交 Task 11**

```bash
git add kernel/arch/aarch64/boot/main.c kernel/arch/aarch64/boot/boot_percpu.c \
        hosttests/Makefile hosttests/cases/test_double_state_publish.c
git commit -m "feat(aarch64): BSP uses ipi_ready_publish_and_count + smp_starting_enter"
```

---

### Task 12: tlb.c 串行化 + 代数 ack + 超时 FATAL + x86 迁移（**锁在此定义**）

**v1 review items fixed:** item 8（`tlb_shootdown()` **保持无参**——7 个调用者不迁移；`tlb_sd_lock` 在本任务定义，不推迟到 Task 17）

**Files:**
- Modify: `kernel/memory/tlb.c`（aarch64 + x86 共用；目标快照内部化；锁定义在此）
- Modify: `kernel/include/memory/tlb.h`（公共头不变——保持 `tlb_shootdown(void)` 无参）
- Modify: `kernel/arch/x86_64/apic/ipi.c`（`ipi_tlb_handler` 重写）
- Modify: `kernel/arch/x86_64/smp/boot.c`（BSP/AP `ipi_ready` 发布点）
- Test: `hosttests/cases/test_tlb_serial_protocol.c`（新建）、`hosttests/cases/test_x86_ipi_ready_publish.c`（新建）

**Interfaces:**
- `void tlb_shootdown(void);` —— **保持无参**（v1 review item 8）；内部取目标快照 = `{ cpu : cpu != self && load_acquire(percpu_data[cpu].ipi_ready) }`。
- 全程持 `tlb_sd_lock`（**普通 spin_lock**，等待者 IRQ 保持开）。
- 等待条件 `!=`（不是 `<`）；超时 FATAL panic。

- [ ] **Step 1: 写 RED 测试 `test_tlb_serial_protocol.c`**

mock per-CPU `tlb_ack_gen`、`ipi_ready`、mock `ipi_broadcast`：
1. 两个目标 CPU 在 `ipi_ready` 集；BSP 发 `tlb_shootdown()` → 调 mock handler → gen+1；BSP 等到 gen 追上 target → 返回 OK；
2. 32 位回绕：`tlb_ack_gen = UINT32_MAX`、target=0 → mock handler `gen = 0`；BSP 等待 `!=` → 等到 0 → 返回 OK；
3. 超时：mock handler 不调 → BSP 超时 FATAL（用 `setjmp`/`longjmp` 拦截 panic 或独立 `tlb_shootdown_panic` 路径）。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 `tlb_shootdown()`**

```c
/* kernel/memory/tlb.c */
#include <memory/vmm.h>
#include <intr/ipi.h>
#include <percpu/percpu.h>
#include <arch/cpu.h>
#include <arch/mmu.h>
#include <arch/spinlock.h>

/* spec §5.4：锁定义在本任务（v1 review item 8），后续所有 vmm 变更入口使用。 */
spinlock_T tlb_sd_lock = { .lock = 1UL };

/* 构造 mask ∩ ipi_ready \ {self} */
static uint64_t build_target_mask_excl_self(void) {
    uint32_t self = cpu_id();
    uint64_t m = ~(1UL << self);
    uint64_t ready = 0;
    for (uint32_t cpu = 0; cpu < NR_CPUS; cpu++) {
        if (atomic_load_acquire(&percpu_data[cpu].ipi_ready))
            ready |= (1UL << cpu);
    }
    return m & ready;
}

void tlb_shootdown(void) {
    /* spec §5.4 通用规则：持 tlb_sd_lock 等 ack 安全（普通 spin_lock +
     * I2 不变式，等待者 IRQ 保持开；TLB handler 无锁）。 */
    spin_lock(&tlb_sd_lock);
    uint64_t mask = build_target_mask_excl_self();
    uint32_t target_gen[NR_CPUS] = {0};
    for (uint32_t cpu = 0; cpu < NR_CPUS; cpu++) {
        if (mask & (1UL << cpu))
            target_gen[cpu] =
                atomic_load_explicit(&percpu_data[cpu].tlb_ack_gen,
                                     memory_order_relaxed) + 1;
    }
    /* 本地失效 + 自增 gen */
    arch_flush_tlb_all();
    atomic_fetch_add_explicit(&this_cpu()->tlb_ack_gen, 1,
                              memory_order_release);
    /* SGI 发送 */
    if (mask)
        ipi_broadcast(IPI_VECTOR_TLB, mask);
    /* 等所有目标 gen 追上（!= 而非 <，处理 32 位回绕） */
    uint64_t start = arch_cycle_counter();
    while (mask) {
        uint64_t done = 0;
        for (uint32_t cpu = 0; cpu < NR_CPUS; cpu++) {
            if (!(mask & (1UL << cpu))) continue;
            uint32_t g = atomic_load_acquire(&percpu_data[cpu].tlb_ack_gen);
            if (g == target_gen[cpu]) done |= (1UL << cpu);
        }
        mask &= ~done;
        if (!mask) break;
        if (arch_cycle_counter() - start > TLB_SD_TIMEOUT_CYCLES)
            panic("tlb_shootdown: TIMEOUT mask=%lx\n", mask);
        arch_cpu_pause();
    }
    spin_unlock(&tlb_sd_lock);
}
```

`make PROFILE=x86_64-clang test-host` GREEN（mock 测试通过）。

- [ ] **Step 3: x86 `ipi_tlb_handler` 迁移（spec §6.4.6）**

`kernel/arch/x86_64/intr/apic/ipi.c` 重写 `ipi_tlb_handler`（**v3 fix for item 6**：保留 3 参签名 `(uint64_t nr, uint64_t param, pt_regs_t *regs)`，handler **保留** `lapic_eoi()`——`kernel/intr/dispatch.c:20` 的 `generic_intr_dispatch` 只调 handler 不调 EOI；EOI 由 handler 自己负责）：

```c
static void ipi_tlb_handler(uint64_t nr __attribute__((unused)),
                            uint64_t param __attribute__((unused)),
                            pt_regs_t *regs __attribute__((unused)))
{
    percpu_t *cpu = this_cpu();
    /* v3 fix for item 6：删旧 tlb_wanted/tlb_ack 协议（条件检查 + 标志置位），
     * 替换为无条件 flush_tlb + gen 递增；保留 lapic_eoi()。 */
    flush_tlb();  /* 现有 CR3 reload */
    __sync_synchronize();
    atomic_fetch_add_explicit(&cpu->tlb_ack_gen, 1, memory_order_release);
    lapic_eoi();
}
```

源码扫描 `kernel/arch/x86_64/intr/apic/ipi.c` 断言不再出现 `tlb_wanted` / `tlb_ack` 符号；`lapic_eoi()` 仍在 handler 末尾。`nm kernel.elf | grep tlb_wanted` 应空。

`make PROFILE=x86_64-clang test-host` + `qemutests/x86_64_systest_repeat.py` 全 PASS。

- [ ] **Step 4: x86 `ipi_ready` 发布点**

`kernel/arch/x86_64/smp/boot.c`：

```c
/* BSP：smp_boot_aps() 内 ipi_init() 之后、启动 AP 之前 */
void ipi_ready_publish_bsp(void) {
    DEBUG_ASSERT(irqs_enabled());
    DEBUG_ASSERT(tlb_handler_registered());
    DEBUG_ASSERT(percpu_data[0].online == 1);
    atomic_store_explicit(&percpu_data[0].ipi_ready, 1,
                          memory_order_release);
}

/* AP：secondary_start（smp.c:137 一带）handler/IDT 可用 + 开 IRQ 之后 */
void ipi_ready_publish_ap(uint32_t cpu) {
    DEBUG_ASSERT(irqs_enabled());
    atomic_store_explicit(&percpu_data[cpu].ipi_ready, 1,
                          memory_order_release);
}
```

`ipi_broadcast(vector, target_mask)` 签名同步迁移：`tlb.c:38` 调用点改 `ipi_broadcast(IPI_VECTOR_TLB, mask)`；APIC 实现按掩码逐核发送。

新增 hosttest `test_x86_ipi_ready_publish.c`：源码扫描 `smp/boot.c` 与 `apic/ipi.c`，断言 BSP/AP 两处 `store_release(ipi_ready=1)` 出现且带 DEBUG 断言。

`make PROFILE=x86_64-clang test-host` + `qemutests/x86_64_systest_repeat.py` 5/5。

- [ ] **Step 5: 提交 Task 12**

```bash
git add kernel/memory/tlb.c kernel/include/memory/tlb.h \
        kernel/arch/x86_64/apic/ipi.c kernel/arch/x86_64/smp/boot.c \
        hosttests/Makefile hosttests/cases/test_tlb_serial_protocol.c \
        hosttests/cases/test_x86_ipi_ready_publish.c
git commit -m "feat(mm): tlb_shootdown serial protocol + x86 handler/gen ack migration"
```

---

### Task 13: M3.1 audit gate — irqsave→vmm caller 链审查

**Files:**
- Modify: `kernel/Makefile`（白名单加 `memory/tlb.c` 编入 aarch64）
- Create: `docs/memory.md` 新增 "vmm 变更调用链审计（M3.1 验收）" 小节
- Test: `hosttests/cases/test_vmm_caller_audit.c`（新建，nm + 源码扫描）

**Interfaces:** 不变；本任务为契约性验收。

- [ ] **Step 1: 编写 `test_vmm_caller_audit.c`**

静态扫描 + `nm` 检查：
1. `nm kernel.elf | grep -E " (T|t) (vma_|uaccess_|fork_)"` —— x86 编译应保留这些符号（vmm.c 仍在 x86 用），aarch64 应**无**这些符号；
2. 源码扫描 `kernel/memory/slab.c`、`kernel/memory/vmm.c`（x86 路径）、`kernel/arch/aarch64/memory/early_arena.c`：在 `slab_lock`/`pmm_lock`/`spin_lock_irqsave` 持锁段内，是否调用 `tlb_shootdown`/`arch_vmm_*`/`vmm_*` —— 必须为 0；
3. `kernel/arch/aarch64/memory/page_table.c`（Task 16 之前是当前 stub）：持锁段不调 vmm；
4. `kernel/arch/aarch64/intr/ipi.c`：`ipi_broadcast` 内不持任何 vmm 锁；
5. `kernel/memory/tlb.c`：`tlb_shootdown` 内 `tlb_sd_lock` 不嵌套其他锁。

`make PROFILE=x86_64-clang test-host` RED（当前 tlb.c 还未编入 aarch64）。

- [ ] **Step 2: 完整列出审计清单**

将上述扫描结论写成 `docs/memory.md` 新增小节：

```markdown
## vmm 变更调用链审计（M3.1 验收）

| 调用点 | 持锁 | 调 vmm? | 结论 |
|---|---|---|---|
| `slab.c:kmalloc_create` | `slab_lock` (irqsave) | 否 | OK |
| `vmm.c:vmm_unmap_4k_page` (x86) | 无 | 是 (free) | OK（x86 现状不变）|
| ... | ... | ... | ... |
```

逐条记录：现存 irqsave 锁内调用 vmm 变更 API 的链（如有）→ 消除（挪出临界区 / 锁拆分），否则标"无"。

- [ ] **Step 3: M3.1 QEMU selftest**

`make PROFILE=aarch64-clang test-aarch64 MODE=smp` 跑通：IPI 自测 + TLB IPI 并存 PASS；AP 启动后 `cpu_id=1` 工作循环运行；BSP 发 shootdown → gen+1 → ACK；无死锁、无超时。

新增 `MODE=ipc-noready`（Task 8 标志位控制；如需新增 mode，按 `mk/components/run.mk:159` 同款模式）：shootdown 只等就绪者、未就绪 AP 的 gen 不变；就绪后下一次 shootdown 才递增。

- [ ] **Step 4: audit gate 通过性断言**

`docs/memory.md` 审计清单全部 OK；audit test 全部 PASS。**audit gate 必须通过才能进 Task 14**。

- [ ] **Step 5: 提交 Task 13（audit gate）**

```bash
git add kernel/Makefile docs/memory.md \
        hosttests/Makefile hosttests/cases/test_vmm_caller_audit.c \
        mk/components/run.mk  # MODE=ipc-noready 新增
git commit -m "feat(aarch64): M3.1 audit gate — list vmm caller chains in docs"
```

---

## M3.2 阶段：语义层拆分 + x86 backend wrapper

### Task 14: 公共 vmm.h 语义层 + x86 pte.h 私有层拆分

**v1 review items fixed:** item 10（语义 API 必须有完整 backend 实现任务——本任务只拆头，aarch64 backend 实现在 Task 15 之后新增的 Task 14b 风格子任务或合并进 Task 16/18）

**Files:**
- Create: `kernel/include/memory/vmm.h`（重写为语义层；移除 `PAGE_*`）
- Create: `kernel/include/arch/x86_64/pte.h`（x86 硬件 PTE 位 + `vmm_pt_walk` + `vmm_free_user_map`）
- Create: `kernel/include/arch/<arch>/vmm_backend.h`（arch 私有位位置 + 软件位常量）
- Modify: `kernel/memory/vma.c`、`kernel/memory/uaccess.c`、`kernel/sched/task.c`、x86 `intr/trap.c`（`do_page_fault`）、`kernel/arch/x86_64/memory/boot_direct_map.c`、`kernel/memory/vmm.c`（include 私有头）

**Interfaces:**
- `kernel/include/memory/vmm.h`（语义层）：
  - `VM_PRESENT/VM_WRITE/VM_USER/VM_NO_EXEC/VM_HUGE/VM_NOCACHE/VM_PROTNONE/VM_COW`；
  - `VM_KERNEL_RW/VM_KERNEL_RO/VM_USER_RW/VM_USER_RO/VM_DEVICE`；
  - 语义 API：`int arch_vmm_init(void); int arch_vmm_map_4k_new(...); int arch_vmm_update_4k(...); int arch_vmm_unmap_4k(...); int arch_vmm_query_4k(...); int arch_vmm_map_2m(...); int arch_vmm_unmap_2m(...); int arch_vmm_split_2m_to_4k(...);`
  - **不**包含 `PAGE_*` 硬件位。
- `kernel/include/arch/x86_64/pte.h`：原 `PAGE_*` 宏、`vmm_pt_walk`（裸 `uint64_t *`）、`vmm_free_user_map`。
- `kernel/include/arch/<arch>/vmm_backend.h`：`arch_vmm_*` 的位映射 + `AARCH64_PT_SOFTWARE_PROTNONE/COW` + `AARCH64_PT_EPROT_NONE`。

- [ ] **Step 1: 写 RED 测试 `test_vmm_semantic_header.c`**

1. `#include <memory/vmm.h>` 后断言所有 `VM_*` 宏已定义；
2. 断言 `#include <memory/vmm.h>` 后**未**定义 `PAGE_PRESENT`/`PAGE_RW`/`PAGE_USER`/`PAGE_HUGE` 等 x86 硬件位；
3. 断言 `arch_vmm_*` 函数原型均在公共头声明；
4. 断言 `#include <arch/x86_64/pte.h>` 提供 `PAGE_PRESENT` 等 x86 硬件位与 `vmm_pt_walk` 原型。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 公共 vmm.h 重写**

按 spec §4.2-4.3 定义 `VM_*` 与 `arch_vmm_*` 原型；移除 `PAGE_*` 宏；移除 x86 裸 PTE 类型。

- [ ] **Step 3: x86 pte.h 私有头**

将现有 `kernel/memory/vmm.h` 的 `PAGE_*` 宏、`vmm_pt_walk`（裸 `uint64_t *`）、`vmm_free_user_map` 迁入 `kernel/include/arch/x86_64/pte.h`。

- [ ] **Step 4: 修改 include 列表（v3 fix for item 11：含 selftest）**

`vma.c`、`uaccess.c`、`task.c`（fork）、x86 `intr/trap.c`（`do_page_fault`）、`arch/x86_64/memory/boot_direct_map.c`、`kernel/memory/vmm.c`：include 私有头 `arch/x86_64/pte.h` 替代原 `memory/vmm.h` 中 `PAGE_*` 引用。

**v3 fix for item 11**：以下 selftest 文件直接使用 `PAGE_*` 宏，必须同步迁移：

```bash
grep -rln "PAGE_PRESENT\|PAGE_RW\|PAGE_USER\|PAGE_HUGE\|PAGE_PUD\|PAGE_PMD\|PAGE_PTE\|PAGE_PGD\|PAGE_4K_SIZE\|PAGE_4K_MASK" kernel/selftest/ kernel/arch/x86_64/
```

已知用户（依 review）：
- `kernel/selftest/test_deep_copy_argv.c:132`（`PAGE_*` 使用）
- `kernel/selftest/test_uaccess.c:32-162`（`PAGE_USER`/`PAGE_USER_PGD`/`PAGE_USER_PUD`/`PAGE_USER_PTE`/`PAGE_USER_PMD`/`PAGE_4K_SIZE`）

每个文件按使用宏的语义决定：
- 直接硬件位（`PAGE_PRESENT`/`PAGE_RW` 等）→ include `arch/x86_64/pte.h`
- 语义位（`VM_*`）→ include `memory/vmm.h`，改写为 `VM_PRESENT`/`VM_WRITE`/...

**grep 验证完成后**做 `make clean` 再构建（per `AGENTS.md`）。

- [ ] **Step 5: 提交 Task 14**

```bash
git add kernel/include/memory/vmm.h kernel/include/arch/x86_64/pte.h \
        kernel/include/arch/x86_64/vmm_backend.h \
        kernel/memory/vmm.c kernel/memory/vma.c kernel/memory/uaccess.c \
        kernel/sched/task.c kernel/arch/x86_64/intr/trap.c \
        kernel/arch/x86_64/memory/boot_direct_map.c \
        kernel/selftest/test_deep_copy_argv.c \
        kernel/selftest/test_uaccess.c \
        hosttests/Makefile hosttests/cases/test_vmm_semantic_header.c
git commit -m "refactor(vmm): split semantic header from x86 PTE internals + migrate selftest PAGE_* usage"
```

---

### Task 15: x86 vmm.c backend wrapper + 释放/COW 逻辑保留

**v1 review items fixed:** item 9（COW 路径 `if (page_cow_put(phys)) free_4k_page(phys);` ——只一条 free；普通路径 `else free_4k_page(phys);`，与 `kernel/memory/vmm.c:330-339` 现状一致）

**Files:**
- Modify: `kernel/memory/vmm.c`
- Test: `hosttests/cases/test_x86_vmm_backend.c`（新建）、`test_m1_*` 全跑

**Interfaces:** 不变（外部 API 保持兼容），内部拆为：
- `x86_vmm_map_4k_page` / `x86_vmm_unmap_4k_page_with_free` / `x86_vmm_query_4k_page`（保留 free/COW 所有权语义）。

- [ ] **Step 1: 写 RED 测试 `test_x86_vmm_backend.c`**

链接真实 `vmm.c`，断言：
1. `x86_vmm_map_4k_page` / `x86_vmm_unmap_4k_page_with_free` / `x86_vmm_query_4k_page` API 存在；
2. **COW 共享页 + 非最后引用**（mock `page_cow_put` 返回 false）：仅 `page_cow_put` 一次，**不** `free_4k_page`；调用计数断言；
3. **COW 共享页 + 最后引用**（mock `page_cow_put` 返回 true）：`page_cow_put` 一次 + `free_4k_page` 一次；
4. **普通页**：`free_4k_page` 一次，**不** `page_cow_put`；
5. fork/COW/munmap 回归：`test_fork_user_map.elf`、`test_user_write_cow.elf` 行为不变。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 拆分 vmm.c**

```c
/* backend 摘除/查询 */
int x86_vmm_map_4k_page(...);
int x86_vmm_unmap_4k_page(...);
int x86_vmm_query_4k_page(...);

/* wrapper with free/COW —— v1 review item 9：保留 vmm.c:330-339 现状 */
int x86_vmm_unmap_4k_page_with_free(uint64_t *pgdir, uint64_t virt) {
    uint64_t *pte = vmm_pt_walk(pgdir, virt, 0, 0);
    if (!pte) return -ENOENT;
    if (!(*pte & (PAGE_VALID | PAGE_PROTNONE))) return -ENOENT;
    uint64_t phys = *pte & PAGE_4K_MASK;
    int rc;
    if (*pte & PAGE_COW) {
        /* COW 共享页：page_cow_put 返回 true 时（最后引用）才 free */
        rc = page_cow_put(phys) ? free_4k_page(phys), 0 : 0;
    } else {
        /* 普通页：直接 free */
        free_4k_page(phys);
        rc = 0;
    }
    *pte = 0;
    return rc;
}
```

- [ ] **Step 3: 跑所有 x86 回归**

`make PROFILE=x86_64-clang test-host` 全 PASS；`qemutests/x86_64_systest_repeat.py` 5/5。

- [ ] **Step 4: 提交 Task 15**

```bash
git add kernel/memory/vmm.c hosttests/Makefile \
        hosttests/cases/test_x86_vmm_backend.c
git commit -m "refactor(vmm): keep x86 free/COW logic in backend wrapper (single-free per branch)"
```

---

## M3.3 阶段：aarch64 page_table.c 公开原语 + 软件位 + block

### Task 16: aarch64 page_table.c 公开原语 + 软件位 + block 编解码

**v1 review items fixed:** item 10（Task 14 拆头后，本任务给 `arch_vmm_*` 的 aarch64 后端**完整**实现步骤）

**Files:**
- Modify: `kernel/arch/aarch64/memory/page_table.c`
- Modify: `kernel/include/arch/aarch64/page_table.h`
- Modify: `kernel/include/arch/aarch64/vmm_backend.h`（arch 私有位位置）
- Create: `kernel/include/memory/vmm.h` 的 `arch_vmm_*` aarch64 后端实现（**新建** `kernel/arch/aarch64/memory/vmm_backend.c`）
- Test: `hosttests/cases/test_aarch64_pt_software_bits.c`（新建）

**Interfaces:**
- 公共 `arch_vmm_*` aarch64 后端（`kernel/arch/aarch64/memory/vmm_backend.c`）：
  - `int arch_vmm_init(void);`
  - `int arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);`
  - `int arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags, uint64_t *old_phys_out, uint32_t *old_vm_out);`
  - `int arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out, uint32_t *old_vm_out);`
  - `int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out, uint32_t *vm_out);` —— 三态：0 / -EPROT_NONE / -ENOENT
  - `int arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);`
  - `int arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);`
  - `int arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt);`
- 这些全部 thin wrapper，调用 `aarch64_pt_*` 原语 + 公共语义位校验。
- 低级 `aarch64_pt_*` 原语（在 `page_table.c`）：
  - `int aarch64_pt_map_4k_ext(root, va, pa, perm, software_bits);`
  - `int aarch64_pt_replace_4k(root, va, pa, perm, software_bits, *old_pa, *old_sw);`
  - `int aarch64_pt_query_4k(root, va, *pa, *perm, *sw);`
  - `int aarch64_pt_unmap_4k(root, va, *pa, *perm, *sw);`
  - `uint64_t aarch64_pt_encode_block_desc(pa, perm, sw);`

- [ ] **Step 1: 写 RED 测试 `test_aarch64_pt_software_bits.c`**

`page_table.c` 公开原语层面：
1. map + sw=PROTNONE → query 返回 -EPROT_NONE 且 phys_out 有效；
2. unmap 带 PROTNONE → 返回 phys 且不释放；
3. update 切到 VALID → query 正常；
4. update 切回 PROTNONE（保留 PA） → query -EPROT_NONE；
5. 拒绝组合 sw=PROTNONE|COW → -EINVAL；
6. encode_block_desc 输出 bit1=0、bit55/56 正确。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 `aarch64_pt_*` 原语 + 软件位**

`kernel/arch/aarch64/memory/page_table.c`：

- 新增 `walk_to_l2(root, va, create, ...)`（Task 17 详细）；
- 新增 `aarch64_pt_map_4k_ext` / `replace_4k` / `query_4k` / `unmap_4k`，内部持 `pt_lock_for(root, l2)`、调 `walk_to_l3`、per-VA local TLBI（及时性）；
- `replace_4k` 按 spec §4.4.3 分类：仅权限变 → 原子 store；内存类型变/PA 变/有效性翻转 → PTE BBM；
- `encode_block_desc` 与 leaf 共享 vm→AP/SH/Attr/XN 翻译，但 OA 字段 [39:21]（IPS=40）、bit1=0；
- 软件位 bit55/56 进出：x86 = bit9/10，aarch64 = bit55/56；常量 `AARCH64_PT_SOFTWARE_PROTNONE = (1UL << 55)`、`AARCH64_PT_SOFTWARE_COW = (1UL << 56)`、`AARCH64_PT_EPROT_NONE = -1111`（自定义负码）。

- [ ] **Step 3: 实现 `arch_vmm_*` aarch64 后端（v1 review item 10）**

`kernel/arch/aarch64/memory/vmm_backend.c`（新建）：

```c
#include <memory/vmm.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/mmu.h>      /* aarch64_read_ttbr1, ARCH_PAGE_OFFSET */
#include <memory/pmm.h>
#include <memory/tlb.h>

mmap kernel_map = NULL;  /* 见 v1 review item 16：复用现有 mmap 类型 */

int arch_vmm_init(void) {
    uint64_t raw = aarch64_read_ttbr1();
    uint64_t pa  = raw & AARCH64_TTBR_BASE_MASK;
    /* v1 review item 16：用现有掩码提取 PA 后再校验 < 1 TiB */
    if (pa == 0 || (pa & 0xFFF) != 0) return -EINVAL;
    if (pa >= (1UL << 40)) return -EINVAL;
    kernel_map = (mmap)(uintptr_t)(pa + ARCH_PAGE_OFFSET);
    aarch64_pt_root_publish(pa);  /* idempotent */
    return 0;
}

/* 其余 arch_vmm_*：vm_flags → perm + software_bits 翻译 + 调 aarch64_pt_* +
 * 错误码转换（-EEXIST, -EPROT_NONE, -EINVAL, -ENOENT） */
```

迁移 `aarch64_m1_selftest.c`（Task 14 之前的旧 `aarch64_pt_map_4k/query_4k/unmap_4k` 调用点）：把所有签名更新到新原语（`aarch64_pt_map_4k_ext` 等）；新原语保留 4 参路径（`map/query/unmap`）作为旧签名的薄包装（如 spec §1.4 "M3 不迁移上述文件内部实现"——M1 selftest 属 M1 范围，用 4 参包装即可）。

`make PROFILE=x86_64-clang test-host` + `make PROFILE=aarch64-clang test-aarch64 MODE=smp` 全 PASS（v1 review item 10 解决：`arch_vmm_*` 有明确后端实现 + 旧原语调用点已迁移）。

- [ ] **Step 4: 提交 Task 16**

```bash
git add kernel/arch/aarch64/memory/page_table.c \
        kernel/include/arch/aarch64/page_table.h \
        kernel/include/arch/aarch64/vmm_backend.h \
        kernel/arch/aarch64/memory/vmm_backend.c \
        kernel/arch/aarch64/memory/m1_selftest.c \
        hosttests/Makefile hosttests/cases/test_aarch64_pt_software_bits.c
git commit -m "feat(aarch64): page_table primitives + arch_vmm_* backend + selftest migration"
```

---

### Task 17: pt_locks/pt_upper_lock 静态初始化 + walk_to_l2(create)

**v1 review items fixed:** item 8 配套（`tlb_sd_lock` 已在前置 Task 12 定义；这里只剩 `pt_locks` + `pt_upper_lock`）

**Files:**
- Modify: `kernel/arch/aarch64/memory/page_table.c`
- Test: `hosttests/cases/test_aarch64_pt_locks.c`（新建）

**Interfaces:**
- `static spinlock_T pt_locks[64] = { [0...63] = { .lock = 1UL } };`
- `static spinlock_T pt_upper_lock = { .lock = 1UL };`
- `spinlock_T *pt_lock_for(uint64_t root_pa, uint32_t l2_idx);`
- `int walk_to_l2(uint64_t *root, uint64_t va, bool create);` —— 锁序 `pt_lock_for(root, l2)` → `pt_upper_lock`；ENOMEM 时回滚本次未发布页、保留已发布的空中间表。

- [ ] **Step 1: 写 RED 测试 `test_aarch64_pt_locks.c`**

1. 两锁静态初始化不为 0（不会自旋：`spin_init` 双保险）；
2. `pt_lock_for(root_pa, l2_idx)` 不同 (root, l2) 哈希冲突仅损性能；相同输入得相同锁；
3. `walk_to_l2(create=true)` 在全新 L1 范围建 block：先 alloc + zero + publish 两级中间表；
4. `alloc_4k_page` mock 返回 0 → walk_to_l2 释放本次未发布页、返回 -ENOMEM、**保留**已发布的空中间表；
5. 锁序：pt_lock + pt_upper_lock 同时持有时不允许持 tlb_sd_lock（源码扫描）。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现**

```c
static spinlock_T pt_locks[64] = { [0 ... 63] = { .lock = 1UL } };
static spinlock_T pt_upper_lock = { .lock = 1UL };

int aarch64_pt_init_locks(void) {
    for (int i = 0; i < 64; i++) spin_init(&pt_locks[i]);
    spin_init(&pt_upper_lock);
    return 0;
}

spinlock_T *pt_lock_for(uint64_t root_pa, uint32_t l2_idx) {
    return &pt_locks[((root_pa >> 12) ^ l2_idx) & 63];
}

int walk_to_l2(uint64_t *root, uint64_t va, bool create) {
    /* spec §5.2b 锁序 + 回滚 */
}
```

`arch_vmm_init` 内调 `aarch64_pt_init_locks()`（双保险）。

- [ ] **Step 3: I1/I2 DEBUG 断言**

`page_table.c` 的 `arch_vmm_*` 入口加 `BUG_ON(irqs_disabled());`（DEBUG 构建）—— **仅限** vmm 变更 API 入口（map/unmap/update/split），不放在 M1 selftest smoke 路径；`spin_lock(&pt_lock_*)` 不带 irqsave（理由见 Task 12 Global Constraints）。

- [ ] **Step 4: 提交 Task 17**

```bash
git add kernel/arch/aarch64/memory/page_table.c \
        hosttests/Makefile hosttests/cases/test_aarch64_pt_locks.c
git commit -m "feat(aarch64): pt_locks init and walk_to_l2(create) with rollback"
```

---

### Task 18: aarch64_pt_* 2M block + backend 全部 4K/2M 原语

**Files:**
- Modify: `kernel/arch/aarch64/memory/page_table.c`
- Modify: `kernel/include/arch/aarch64/page_table.h`
- Test: `hosttests/cases/test_aarch64_pt_2m_block.c`（新建）、`hosttests/cases/test_aarch64_backend_4k.c`（新建）

**Interfaces:**
- `int aarch64_pt_map_2m_block(root, va, pa, vm_flags);`
- `int aarch64_pt_unmap_2m_block(root, va, *pa_out);`
- `int aarch64_pt_split_block_2m(root, va);` —— 见 Task 20。
- `int aarch64_pt_map_4k_new(root, va, pa, vm_flags);` —— 遇任何已占用（含 PROT_NONE）返回 -EEXIST。
- `int aarch64_pt_unmap_4k(root, va, *pa_out, *old_vm_out);`

- [ ] **Step 1: 写 RED 测试（v3 fix for item 16：明确 12 合法 + 4 拒绝矩阵）**

`test_aarch64_backend_4k.c`（**v3 fix for item 16**：明确枚举 12 合法 + 4 拒绝组合）：

**12 合法组合**（按 spec §4.2 `VM_*` 复合位；v4 fix for v3 review item 13：leaf/block 分到不同 API）：

| # | 复合名 | bits | 描述 | API |
|---|---|---|---|---|
| 1 | `VM_KERNEL_RW` | `PRESENT\|WRITE` | leaf Normal, K RW | `map_4k_new` |
| 2 | `VM_KERNEL_RO` | `PRESENT` | leaf Normal, K RO | `map_4k_new` |
| 3 | `VM_USER_RW` | `PRESENT\|WRITE\|USER` | leaf Normal, U RW | `map_4k_new` |
| 4 | `VM_USER_RO` | `PRESENT\|USER` | leaf Normal, U RO | `map_4k_new` |
| 5 | `VM_KERNEL_RW\|VM_HUGE` | `PRESENT\|WRITE\|HUGE` | block Normal, K RW | `map_2m_block` |
| 6 | `VM_KERNEL_RO\|VM_HUGE` | `PRESENT\|HUGE` | block Normal, K RO | `map_2m_block` |
| 7 | `VM_USER_RW\|VM_HUGE` | `PRESENT\|WRITE\|USER\|HUGE` | block Normal, U RW | `map_2m_block` |
| 8 | `VM_USER_RO\|VM_HUGE` | `PRESENT\|USER\|HUGE` | block Normal, U RO | `map_2m_block` |
| 9 | `VM_DEVICE` | `PRESENT\|WRITE\|NOCACHE\|NO_EXEC` | leaf Device, K | `map_4k_new` |
| 10 | `VM_DEVICE\|VM_USER` | `PRESENT\|WRITE\|USER\|NOCACHE\|NO_EXEC` | leaf Device, U | `map_4k_new` |
| 11 | `VM_DEVICE\|VM_HUGE` | `PRESENT\|WRITE\|NOCACHE\|NO_EXEC\|HUGE` | block Device, K | `map_2m_block` |
| 12 | `VM_DEVICE\|VM_USER\|VM_HUGE` | `PRESENT\|WRITE\|USER\|NOCACHE\|NO_EXEC\|HUGE` | block Device, U | `map_2m_block` |

**4 拒绝组合**（VM_NOCACHE & ~VM_NO_EXEC → -EINVAL；仅在 `map_4k_new` / `map_2m_block` 入口校验）：

| # | bits | 描述 |
|---|---|---|
| R1 | `PRESENT\|WRITE\|NOCACHE` | K RW 缺 NO_EXEC |
| R2 | `PRESENT\|NOCACHE` | K RO 缺 NO_EXEC |
| R3 | `PRESENT\|WRITE\|USER\|NOCACHE` | U RW 缺 NO_EXEC |
| R4 | `PRESENT\|USER\|NOCACHE` | U RO 缺 NO_EXEC |

测试断言（链接真实 `vmm_backend.c` + `page_table.c`，**非**仅扫描文本——v1 review item 15）：
1. 1-4 + 9-10：用 `map_4k_new`，断言 leaf descriptor；
2. 5-8 + 11-12：用 `map_2m_block`，断言 block descriptor（`HUGE` 位、有效位、AttrIndx 等）；
3. 4 拒绝：`map_4k_new` 与 `map_2m_block` 任一用 R1-R4 → -EINVAL；
4. 软件位 round-trip：map+sw=PROTNONE → query -EPROT_NONE + phys_out 有效；update 切回 VALID → 正常。

`test_aarch64_pt_2m_block.c`：
   1. map + query + unmap 2 MiB block（组合 5-8, 11-12）；alignment check；
   2. L3 已建 → map_2m_block 返回 -EEXIST；
   3. unmap_2m_block on L3 table desc → -EINVAL；
   4. unmap on invalid → -ENOENT。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 map/unmap_4k_new/unmap_4k**

```c
int aarch64_pt_map_4k_new(uint64_t *root, uint64_t va, uint64_t pa, uint32_t vm) {
    /* 拒绝组合检查 (spec §4.2) */
    if ((vm & VM_NOCACHE) && !(vm & VM_NO_EXEC)) return -EINVAL;
    /* ... 其余位校验 ... */
    spinlock_T *lk = pt_lock_for(va_to_root_pa(root), l2_idx(va));
    spin_lock(lk);
    /* walk_to_l3(create=true) */
    /* 遇任何已占用（含 PROTNONE）→ spin_unlock; return -EEXIST */
    /* 写 leaf descriptor */
    spin_unlock(lk);
    /* 已发布 root → block local TLBI */
    if (aarch64_pt_root_is_published(root)) {
        tlb_invalidate_local(va);  /* per-VA TLBI（及时性） */
        /* shootdown 留给 caller（§4.4.3 分类） */
    }
    return 0;
}
```

- [ ] **Step 3: 实现 map/unmap_2m_block**

```c
int aarch64_pt_map_2m_block(...) {
    /* root/va/pa 对齐 + < 1 TiB */
    /* 持 pt_lock_for(root, l2) */
    /* walk_to_l2(create=true) */
    /* pmd[l2] 已占用 → -EEXIST */
    /* 写 block desc + dsb ishst */
    /* 已发布 root → block local TLBI + shootdown */
}
```

`make PROFILE=x86_64-clang test-host` GREEN。

- [ ] **Step 4: 提交 Task 18**

```bash
git add kernel/arch/aarch64/memory/page_table.c \
        kernel/include/arch/aarch64/page_table.h \
        hosttests/Makefile hosttests/cases/test_aarch64_backend_4k.c \
        hosttests/cases/test_aarch64_pt_2m_block.c
git commit -m "feat(aarch64): backend 4K and 2M block primitives"
```

---

### Task 19: hosttest 矩阵 — 4 类替换协议 + PROT_NONE + ack 回绕（**链接生产**）

**v1 review items fixed:** item 15（必须链接真实 `page_table.c` + `vmm_backend.c`，非仅文本扫描；4 类替换协议各列中间态断言）

**Files:**
- Create: `hosttests/cases/test_vmm_replace_protocol.c`（4 类协议各一组）、`test_vmm_prot_none.c`、`test_vmm_ack_wraparound.c`

**Interfaces:** 不变（消费前序任务实现）。

- [ ] **Step 1: 4 类替换协议中间态断言**

`test_vmm_replace_protocol.c`（**链接生产** `page_table.c` + `vmm_backend.c`，mock 锁与 TLBI）：
1. **仅权限变**：map(KERNEL_RW) → update(KERNEL_RO) → 中间态断言 `*pte & PRESENT && !(*pte & WRITE)` → ack 完成 → query 正常；
2. **内存类型变（描述符级，无 live 访问——v1 review item 11/F11）**：mock 构造 PA 在 Device 窗口（vmm.h grep 验证测试只用 Device 窗口 PA）；map(Normal) → update(Device) → 中间态断言 PTE 中 AttrIndx 翻转 → ack 完成 → query 正常；
3. **PA 变（重映射）**：map(P1) → update(P2) → BBM 序列中间态断言 `*pte == 0` → dsb → tlbi → ack → `*pte = P2|NEW_PERM` → query 正常；
4. **有效性翻转（↔PROTNONE）**：map(P, VALID) → update(P, PROTNONE) → 中间态 `*pte & !VALID && (PROTNONE_BIT)` → ack → query -EPROT_NONE + phys 有效。

- [ ] **Step 2: PROT_NONE 三态**

`test_vmm_prot_none.c`：见 spec §4.3：
1. query PROT_NONE → -EPROT_NONE 且 phys_out 有效；
2. unmap PROTNONE → 返回 phys（不 free）；
3. update 暂存 PROTNONE → 再 update 恢复 VMA 原权限 → query 正常。

- [ ] **Step 3: ack 代数回绕**

`test_vmm_ack_wraparound.c`：mock `tlb_ack_gen` 与 handler；构造 `gen=UINT32_MAX` → 发起 shootdown → target=0 → mock handler `gen = 0`（回绕）；BSP 等待 `!=` → 等到 0 → 返回 OK；mock 立即 `gen = UINT32_MAX`（不变） → BSP 等待超时 FATAL。

`make PROFILE=x86_64-clang test-host` 全 PASS。

- [ ] **Step 4: 提交 Task 19**

```bash
git add hosttests/Makefile hosttests/cases/test_vmm_replace_protocol.c \
        hosttests/cases/test_vmm_prot_none.c hosttests/cases/test_vmm_ack_wraparound.c
git commit -m "test(vmm): 4-class replace protocol, PROT_NONE states, and ack wraparound"
```

---

## M3.4 阶段：split 协议 + 发布登记表

### Task 20: split 协议 — 发布登记表 + **安装前**登记 M1 TTBR1

**v1 review items fixed:** item 12（登记点 = **安装点之前**——保留旧 root 不变能力）

**Files:**
- Modify: `kernel/arch/aarch64/memory/boot_direct_map.c`（**安装 TTBR1 之前** 调 `aarch64_pt_root_publish`，确保登记失败保留旧 root）
- Test: `hosttests/cases/test_aarch64_pt_root_publish.c`（新建；测试已在 Task 7 Step 4 实现的函数）

**Interfaces:**
- `bool aarch64_pt_root_publish(uint64_t root_pa);` —— **已在 Task 7 Step 4 实现**；登记；重复幂等；超限 panic。
- `bool aarch64_pt_root_is_published(const uint64_t *root);` —— **已在 Task 7 Step 4 实现**。

- [ ] **Step 1: 写 RED 测试 `test_aarch64_pt_root_publish.c`**

1. 新登记 root → `is_published` true；
2. 重复登记 → 幂等；
3. 登记 9 个不同 root → panic（第 9 次）；
4. M1 安装的 kernel_map → is_published true；
5. scratch root 未登记 → is_published false → split 允许；
6. **登记失败注入**：`aarch64_pt_root_publish` 内部 mock 返回 false 时，boot_direct_map 不应继续安装 TTBR1（保留旧 root）。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 登记点（实现已在 Task 7 Step 4；本任务只接 boot_direct_map）**

`boot_direct_map.c`（**关键顺序修正**——v1 review item 12 + v3 fix for item 7）：

```c
/* 在 aarch64_m1_install_ttbr1 之前登记；登记失败则保留旧 root 不变 */
if (!aarch64_pt_root_publish(tree.root_pa)) {
    rc = -ENOSPC;  /* 登记表已满 */
    goto fail;
}
aarch64_m1_install_ttbr1(tree.root_pa);
installed_root = tree.root_pa;
/* ... 既有 main.c:85-89 一带的 TTBR1 校验 + selftest ... */
```

- [ ] **Step 3: 提交 Task 20**

```bash
git add kernel/arch/aarch64/memory/page_table.c \
        kernel/arch/aarch64/memory/boot_direct_map.c \
        hosttests/Makefile hosttests/cases/test_aarch64_pt_root_publish.c
git commit -m "feat(aarch64): root publish registry, register before install_ttbr1"
```

---

### Task 21: split_block_2m — 未发布 root 原子 store + 已发布 -EPERM

**Files:**
- Modify: `kernel/arch/aarch64/memory/page_table.c`
- Modify: `kernel/arch/aarch64/boot/main.c`（boot 打印 `ID_AA64MMFR2_EL1.BBM`）
- Test: `hosttests/cases/test_aarch64_pt_split.c`（新建）

**Interfaces:** `int aarch64_pt_split_block_2m(uint64_t *root, uint64_t va);`

- [ ] **Step 1: 写 RED 测试 `test_aarch64_pt_split.c`**

1. 未发布 root：map_2m + split → query 512 PTE 全等 `inherit(block_desc)`（同 perm、同 sw、同 OA 偏移）；
2. 已发布 root（mock `aarch64_pt_root_is_published=true`）→ split 返回 -EPERM；
3. alloc 失败注入（mock `alloc_4k_page=0`）→ 原 block 完好、返回 -ENOMEM；
4. 并发 split 模拟（mock 两次调用交错）：第二次看到 L3 table desc → 返回 -EAGAIN。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: 实现 + BBM 调试输出**

按 spec §5.3 未发布 root 算法实现。

`main.c` 启动时：

```c
uint64_t mmfr2 = read_sysreg(ID_AA64MMFR2_EL1);
kputs("[aarch64] ID_AA64MMFR2_EL1.BBM = "); kputu((mmfr2 >> 20) & 0xF); kputs("\n");
```

注释化写入文档：cortex-a53 上 QEMU 通过不能替代架构前提；F10 立项时核对 Arm ARM。

`make PROFILE=x86_64-clang test-host` GREEN。

- [ ] **Step 3: 提交 Task 21**

```bash
git add kernel/arch/aarch64/memory/page_table.c kernel/arch/aarch64/boot/main.c \
        hosttests/Makefile hosttests/cases/test_aarch64_pt_split.c
git commit -m "feat(aarch64): split_block_2m with EPERM on published root"
```

---

### Task 22: 单核 QEMU selftest — GIC target RAZ/WI + map/update/unmap/split

**v1 review items fixed:** item 15（用 `make PROFILE=aarch64-clang test-aarch64 MODE=smp`，非 `aarch64-uefi-kernel-selftest`）

**Files:**
- Modify: `kernel/selftest/test_m3_selftest.c`（新建，单核 M3 selftest 入口）
- Modify: `qemutests/aarch64_uefi_smp.py`（已含 `--expect-selftest` 框架；新增 M3 selftest 行解析）

- [ ] **Step 1: 单核 + 多核 QEMU 启动 selftest（v3 fix for item 15：smp 模式已覆盖 1/2/4 核）**

**v3 fix for item 15**：`mk/components/run.mk:215` 的 `_test-aarch64-run-smp` 已支持 1/2/4 核（smp 模式默认行为）；**不复用** `MODE=no-ack`——那是两核故障注入模式。

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp  # 默认 -smp 2
# 单核变体：直接传 NR_CPUS=1（看 mk/components/run.mk 是否支持；如不支持，
# 临时修改 run.mk 的 -smp 参数；Task 26 总回归覆盖 1/2/4 核）
make PROFILE=aarch64-clang test-aarch64 MODE=smp  # -smp 2 跑默认
```

M3 selftest 内容（按 `-smp` 切换）：
1. GICD_ITARGETSR0 单核 RAZ/WI 例外路径（`-smp 1` 时 `gic_target_bit_init(0)=1u`）；
2. map_4k_new + update_4k + unmap_4k + query_4k 全套；
3. map_2m_block + unmap_2m_block；
4. split_block_2m 单元（未发布 scratch root）。

**白名单同步（v3 fix for item 10）**：`kernel/Makefile:39` aarch64 `KERNEL_C_SOURCES` 追加 `selftest/test_m3_selftest.c`。提交前 `nm kernel.elf | grep test_m3_selftest` 应非空。

`aarch64_uefi_smp.py` 解析器断言每行 PASS（`-smp 1` 时 `expected_selftest` 仍通过；`-smp 2` 走工作项测试）。

- [ ] **Step 2: M1 已发布 TTBR1 root 的 pre-SMP smoke**

`m1_smoke.c` 已通过；Task 13 audit gate 验证 `smp_starting=0` 时 M1 selftest 通过纯本核失效（`aarch64_pt_map_4k/unmap_4k` 直接修改 TTBR1 root，`vmm_gate_check()` 在 pre-SMP 阶段直接返回）。

- [ ] **Step 3: 提交 Task 22**

```bash
git add kernel/selftest/test_m3_selftest.c kernel/Makefile \
        qemutests/aarch64_uefi_smp.py
git commit -m "test(aarch64): M3 selftest covering map/update/unmap/split (single + multi-core)"
```

---

## M3.5 阶段：阶段门禁 + arch_vmm_init + 生产 shootdown 探针

### Task 23: smp_starting 阶段门禁 + DEBUG 断言（**仅限远端等待路径**）

**v3 fix for items 7, 8, 9**：实现已在 **Task 7 Step 4** 完成（`smp_starting_enter` / `ipi_ready_publish_and_count` / `vmm_gate_check` 与 `aarch64_pt_root_publish/is_published` 一起定义并接入所有公开原语入口）。本任务仅补充集成行为测试 + 主线调用确认。

**v1 review items fixed:** item 13（提供单次发布函数 `smp_starting_enter()`；IPI ready 计数发布顺序；IRQ 断言仅限远端等待路径——M1 smoke 不被拦）

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c`（Task 11 Step 2 已落 `smp_starting_enter` 调用；本任务确认）
- Test: `hosttests/cases/test_smp_starting_gate.c`（新建；测试已在 Task 7 Step 4 实现的函数）

**Interfaces:**
- `void smp_starting_enter(void);` —— **已在 Task 7 Step 4 实现**；单次发布（不回退）。
- `static _Atomic uint32_t smp_starting = 0;` —— **已在 Task 7 Step 4 定义**。
- `static _Atomic uint32_t ipi_ready_count = 0;` —— **已在 Task 7 Step 4 定义**；BSP/AP **均**通过 `ipi_ready_publish_and_count(cpu)` 增加（v3 fix for item 9：BSP 不再单独置 `ipi_ready=1`）。
- `void vmm_gate_check(void);` —— **已在 Task 7 Step 4 实现并接入所有 `arch_vmm_*` 与 `aarch64_pt_*` 公开原语入口**（v3 fix for item 8：与 API 开放同步就位）。

- [ ] **Step 1: 写 RED 测试 `test_smp_starting_gate.c`**

1. mock `smp_starting=0, ipi_ready_count=0` → 公开原语允许（pre-SMP 例外）；
2. mock `smp_starting=1, ipi_ready_count<dtb_cpu_count` → panic；
3. mock `smp_starting=1, ipi_ready_count==dtb_cpu_count` → 允许；
4. `smp_starting_enter()` 单次发布：再调用第二次触发 BUG_ON（不回退）；
5. **BSP + AP 都用 `ipi_ready_publish_and_count`**（v3 fix for item 9）：mock `dtb_cpu_count=2`，BSP 调一次 + AP 调一次 → `ipi_ready_count=2`；探测等 `dtb_cpu_count()` 满足。

`make PROFILE=x86_64-clang test-host` RED（首次实现已在 Task 7 Step 4，本测试覆盖集成行为）。

- [ ] **Step 2: 集成点确认（实现已在 Task 7 Step 4 + Task 11）**

`main.c` 在 `smp_boot_aps()` 调用前 `smp_starting_enter()`（Task 11 Step 2 已落）。

AP `secondary_idle`（Task 10 Step 4 改）：用 `ipi_ready_publish_and_count(cpu)` 替换原 `store_release(ipi_ready=1)`。

确认所有 `arch_vmm_*` 与 `aarch64_pt_*` 公开原语入口首句已加 `vmm_gate_check();`（v3 fix for item 8：与 API 开放同步）。

- [ ] **Step 3: 提交 Task 23**

```bash
git add kernel/arch/aarch64/boot/main.c \
        hosttests/Makefile hosttests/cases/test_smp_starting_gate.c
git commit -m "test(aarch64): smp_starting gate integration test (BSP+AP both count)"
```

---

### Task 24: arch_vmm_init 生产调用点 + kernel_map 校验

**v1 review items fixed:** item 16（`kernel_map` 是 `mmap` 指针，**不**是 `uint64_t`；用现有 `AARCH64_TTBR_BASE_MASK` 掩码先提取 PA 再校验；错误路径 `for (;;) arch_cpu_halt()`）

**Files:**
- Modify: `kernel/arch/aarch64/memory/vmm_backend.c`（Task 16 已实现 `arch_vmm_init`；本任务加 main.c 调用）
- Modify: `kernel/arch/aarch64/boot/main.c`
- Test: `hosttests/cases/test_aarch64_arch_vmm_init.c`（新建）

**Interfaces:** 不变。

- [ ] **Step 1: 写 RED 测试**

`test_aarch64_arch_vmm_init.c`：mock `aarch64_read_ttbr1()` 返回非零/4K 对齐/< 1 TiB；调 `arch_vmm_init`；断言 `kernel_map == (mmap)(pa + ARCH_PAGE_OFFSET)` 且返 rc = 0。mock 各种边界（pa=0, pa=未对齐, pa≥1 TiB）→ 各自返 -EINVAL。

`make PROFILE=x86_64-clang test-host` RED。

- [ ] **Step 2: `main.c` 生产调用 + 不可返回 halt**

```c
/* main.c：在 arch_boot_direct_map_init 成功后立即调 */
int rc = arch_vmm_init();
if (rc) {
    kputs("FATAL: arch_vmm_init rc=-");
    int64_t rcn = -(int64_t)rc;
    kputs(rcn < 0 ? "INT_MIN" : "");  /* 占位：实际用 kputu((uint64_t)rcn) */
    kputu((uint64_t)rcn);
    kputs("\n");
    for (;;) arch_cpu_halt();  /* v1 review item 16：不可返回 */
}
```

启动 selftest 断言 `kernel_map == (mmap)(uintptr_t)((aarch64_read_ttbr1() & AARCH64_TTBR_BASE_MASK) + ARCH_PAGE_OFFSET)`。

- [ ] **Step 3: 提交 Task 24**

```bash
git add kernel/arch/aarch64/boot/main.c \
        hosttests/Makefile hosttests/cases/test_aarch64_arch_vmm_init.c
git commit -m "feat(aarch64): arch_vmm_init production call with mmap kernel_map check"
```

---

### Task 25: 生产 shootdown 探针 + 多核 selftest 全套

**v1 review items fixed:** item 14（0 AP 就绪必须 FAIL；失败后 `for (;;) arch_cpu_halt()` 不可返回；打印缺席 CPU 编号）

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c`（§7.3 探针）
- Create: `qemutests/aarch64_m3_probe.py`（正式镜像探针解析）
- Test: `hosttests/cases/test_aarch64_scratch_probe_logic.c`（新建）

**Interfaces:**
- `#define SCRATCH_VA (ARCH_PAGE_OFFSET + 0x10000000UL)` —— PA 0x10000000 ∈ QEMU virt 设备间隙。

- [ ] **Step 1: 写 RED 测试**

`test_aarch64_scratch_probe_logic.c`：mock allocator、mock shootdown、mock work-item submit；模拟探针 7 步：BSP map → work submit (A 读到) → update → shootdown → work submit (B 读到) → unmap → query -ENOENT；断言所有 assertion 顺序触发。**0 AP 就绪**（mock `ipi_ready_count=0`）：断言探针 FAIL 并 `for (;;) arch_cpu_halt()`。

- [ ] **Step 2: 实现 §7.3 探针（**0-AP FAIL + 不可返回 halt**）**

```c
void aarch64_m3_shootdown_probe(void) {
    /* v1 review item 14: 正式双核探针至少需要一个 AP；单核必须 FAIL */
    uint32_t expected = dtb_cpu_count() - 1;  /* AP 数 */
    if (expected < 1) {
        kputs("M3-SHOOTDOWN-PROBE: FAIL requires-at-least-one-AP\n");
        for (;;) arch_cpu_halt();  /* 不可返回 */
    }
    /* 有界等待 AP 就绪 */
    uint64_t start = arch_cycle_counter();
    while (true) {
        uint32_t ready = atomic_load_acquire(&ipi_ready_count);
        if (ready >= dtb_cpu_count()) break;  /* 包含 BSP */
        if (arch_cycle_counter() - start > PROBE_DEADLINE_CYCLES) {
            kputs("M3-SHOOTDOWN-PROBE: FAIL ap-not-ready\n");
            for (;;) arch_cpu_halt();
        }
    }
    /* 同时检查 scratch VA 为空（v3 fix for item 17：完整 4 参签名） */
    uint64_t phys = 0;
    uint32_t vm = 0;
    if (arch_vmm_query_4k(kernel_map, SCRATCH_VA, &phys, &vm) != -ENOENT) {
        kputs("M3-SHOOTDOWN-PROBE: FAIL scratch-non-empty\n");
        for (;;) arch_cpu_halt();
    }
    kputs("M3-SHOOTDOWN-PROBE: START\n");
    /* step 4: BSP map_4k_new(kernel_map, P1, SCRATCH_VA, VM_KERNEL_RW)
     *         work submit WORK_READ64 → expect A */
    /* step 5: update_4k(SCRATCH_VA → P2) [BBM] → shootdown
     *         work submit → expect B */
    /* step 6: unmap → free → query -ENOENT */
    kputs("M3-SHOOTDOWN-PROBE: OK\n");
}
```

`main.c` 在 `arch_vmm_init` 成功 + AP `ipi_ready` 全到后调 `aarch64_m3_shootdown_probe()`（要求 `-smp 2`，否则 Step 1 立即 FAIL halt）。

- [ ] **Step 3: 多核 selftest**

`kernel/selftest/test_m3_multicore.c`：spec §8.2 6 项：
1. 工作项 seq 协议 + ack gen 递增断言；
2. 4 核部分掩码 {1,2}，CPU3 不 ack；
3. 一 AP 未 ipi_ready 时纯 shootdown 成功；
4. 全就绪后映射变更 + 全部 AP 读验证（§6.4.7 门禁正向）；
5. 双 CPU 交错竞争同一 pt_lock 且一方做 BBM 类 update；
6. 并发创建共享 L1 条目的两个 L2 slot 映射。

`-smp 2` 与 `-smp 4` 两种；②/④ 需 4 核。

- [ ] **Step 4: QEMU 正式镜像**

`qemutests/aarch64_m3_probe.py`：QEMU `-smp 2` 启动正式镜像（非 selftest 变体），grep `M3-SHOOTDOWN-PROBE: OK`。

- [ ] **Step 5: 提交 Task 25**

```bash
git add kernel/arch/aarch64/boot/main.c kernel/arch/aarch64/memory/page_table.c \
        kernel/Makefile \
        kernel/selftest/test_m3_multicore.c \
        qemutests/aarch64_m3_probe.py \
        hosttests/Makefile hosttests/cases/test_aarch64_scratch_probe_logic.c
git commit -m "feat(aarch64): M3 shootdown probe (0-AP FAIL) and multi-core selftest suite"
```

---

## M3.6 阶段：总回归

### Task 26: 总回归 — x86 5/5 + aarch64 单/多核 + M1 矩阵 + nm 符号检查

**v1 review items fixed:** item 15（用真实入口 `make PROFILE=aarch64-clang test-aarch64 MODE=smp` + `qemutests/x86_64_systest_repeat.py`）

**Files:** Modify: 无新代码（验收）。

- [ ] **Step 1: x86 hosttest + systest 5 连（v3 fix for item 14）**

```bash
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-syscall-repeat  # 用现有 target，自动传 --disk/--firmware
```

全 PASS。

- [ ] **Step 2: aarch64 单核 selftest 全过**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

M1+M2+M3 selftest 全 PASS（Task 22）。

- [ ] **Step 3: aarch64 多核 selftest 全过**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp  # -smp 2
make PROFILE=aarch64-clang test-aarch64 MODE=...  # 需新增 -smp 4 入口（如未支持）
```

- [ ] **Step 4: aarch64 正式镜像探针**

```bash
make PROFILE=aarch64-clang aarch64-uefi-kernel
# 手启 QEMU -smp 2，grep "M3-SHOOTDOWN-PROBE: OK"
```

- [ ] **Step 5: M1 16 组矩阵更新期望**

`test_m1_arena.c`、`test_m1_arena_exhaust.c`、`test_m1_tree.c`、`test_m1_reservation.c` 等期望值（arena 总长变大）已随 Task 4 更新；现跑全 PASS。

- [ ] **Step 6: ipi_test 不回归**

`make PROFILE=aarch64-clang test-aarch64 MODE=smp` → ipi_test 行为兼容（CPU1/2 启动仍按旧协议应答 SGI 0/1/2；`IPI_VECTOR_TLB` 走新映射路径，SGI 3 不与旧协议冲突）。

- [ ] **Step 7: nm 符号检查**

```bash
nm kernel.elf | grep -E " (T|t) (vma_|uaccess_|fork_)"
# x86_64：保留（vmm.c 仍调用）
# aarch64：无（gate 白名单排除）
```

aarch64 kernel 应**无** `vma_*`/`uaccess_*`/fork 符号。

- [ ] **Step 8: 提交 Task 26（验收 tag）**

```bash
git tag m2-m3-complete
git commit --allow-empty -m "test: M2+M3 full regression passes on aarch64 and x86_64"
```

---

## 执行提示

- 每次任务结束**先跑 hosttest 验证再 commit**；任何 RED 立即停。
- 结构/头改变后必须 `make clean` 再构建（per `AGENTS.md`）。
- Task 13 audit gate 通过是 M3.2+ 的硬前置；任何后续任务引用 vmm 变更 API 都需 review 是否落审计清单。
- v9 增量已分别落到：color_printk（Task 5）、GIC target 实测（Task 8）、smp_starting M1 例外（Task 23）、work-timeout FATAL（Task 10）、x86 handler/发布点迁移（Task 12）、tbl_sd_lock 提前定义（Task 12）。
- `IPI_VECTOR_TLB = 0x40` 已定义于 `kernel/include/intr/ipi.h`；aarch64 在 `ipi.c` 内部做映射，不重定义。
- `kernel_map` 复用现有 `mmap` 声明（`kernel/include/memory/vmm.h:82`），Task 24 不引入新类型。
- `percpu_t` 字段加在共享 `kernel/include/percpu/percpu.h`，Task 9 早于 Task 10 handler。




