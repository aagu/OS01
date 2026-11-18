# aarch64 早期 MMIO Device 窗口运行时化（boot_context early-mmio）

日期：2026-11-18　分支：`feature/aarch64-early-mmio-window`（基于 `a0482380`）
Roadmap：P2 真机适配 hardcode 清单「早期 MMIO Device 块固化槽 0x40..0x4F」行（❌ 仍开放项）。
联动闭环：同表「early_arena D 侧固化」行的 D 侧剩余 + 「PL011 head.S:144」行的早期打印基址。

## 1. 问题

早期 MMIO 窗口 `[0x08000000, 0x0A000000)`（16 个 2 MiB Device 块，PMD_low0 槽
0x40..0x4F）在编译期固化于 `boot_layout.h`，被四层消费：

| 层 | 位置 | 现状 |
|----|------|------|
| M0 引导页表 | `head.S` `.Lmmio_blk`（504-534） | 汇编写死 16 槽 |
| M0 早期打印 | `head.S` `.Lprint_c`（144） | `mov x2,#0x09000000` |
| M1 arena preflight | `early_arena.c:76-77,147,186` | D 窗口 RAM 重叠即 FATAL |
| M1 runtime tree | `runtime_tree.c:95-96` | D 条目 = 固化窗口 |
| PT selftest prune | `page_table_selftest.c:45-46` | 固化窗口豁免 |

真机上 PL011/GIC 不在该窗口（BCM2711：PL011@0xFE201000、GIC-400@0x4C8xxxxx）→
窗口外 MMIO 一律缺页。

## 2. 设计

单一事实源 = `boot_context` 新字段；loader 从固件 FDT 推导；head.S 与内核 C
侧各自消费同一份值。

### 2.1 boot_context ABI（追加，不 bump version）

```c
struct BOOT_EARLY_MMIO {
    uint64_t console_base;    /* 早期 UART PA；0 = 用架构回退值 */
    uint64_t device_base;     /* 2 MiB 对齐的设备窗口基址 */
    uint32_t device_blocks;   /* 2 MiB 块数，>=1 */
    uint32_t reserved;
};
```

- 追加在 `boot_device` 之后（offset 176），`sizeof` 176 → 200；version 保持 3。
  旧 loader（零填充）+ 新内核 → 字段全 0 + flag 未置 → 走回退值，行为不变。
- 新 flag：`BOOT_CONTEXT_HAS_EARLY_MMIO = 1u << 7`。
- `boot_offsets.h` 新增汇编用偏移常量（BOOT_CTX_*_OFFSET），C 侧
  `_Static_assert`（test_bootinfo_abi）双重 pin。

### 2.2 loader 推导（boot/uefi/arch/aarch64/fdt_early.c，新 TU）

`arch_fill_firmware` 拷贝 FDT 后调用最小 FDT walker：

1. `#address-cells`/`#size-cells` 沿树继承（reg 用父节点的 cells；缺省 2/2）。
2. `compatible` 精确匹配（节点属性按深度分槽保存，子节点不覆盖父节点——
   GIC 的 v2m 子节点曾暴露此坑）：
   - `arm,pl011` → console_base = reg[0].base，且其 reg 一并入窗口并集；
   - `arm,cortex-a15-gic` / `arm,gic-400`（GICv2）→ 全部 reg 对入并集
     （覆盖 GICD/GICC/v2m）。
3. 窗口 = 并集的 2 MiB 块覆盖 `[block(min), block_up(max_end))`。
4. 有效性：块数 ∈ [1,1024] → 置 flag + 填字段；否则不动（kernel 侧回退）。
   结构块必须以 `FDT_END` 收尾，未知 token / 越界 / 版本 < 3 一律拒绝。

找不到任何设备（无 DTB / 无匹配）→ 不置 flag → QEMU 回退行为不变。
注意：外层 UEFI 构建清单在 `mk/components/uefi.mk` 的 `UEFI_BOOT_SRCS`
（boot/uefi/Makefile 的同名清单只是内层回退）。

### 2.3 引导路径（2026-11-19 rebase 到 `5784d5d8` head.S 重构后的形态）

- `_start` 在 `.boot.bss` 清零后、建表前，校验 x19 指向 `boot_context`
  （magic + flag），把 console/device_base/device_blocks 停靠到 `.boot.bss`
  槽（`aarch64_boot_early_mmio_*`）；无效 → 槽保持 0（=回退）。loader 加载
  时已把镜像 BSS 清零，停靠前的早期向量打印也能安全读到 0 并走回退。
- fail-closed 校验（停靠后立即做）：flag 存在但 base 未 2 MiB 对齐、块数为
  0 或 > 1024、窗口末端超过 `AARCH64_BOOT_MAP_END`（2 GiB）或与启动块 B 的
  2 MiB 区间重叠 → `aarch64_boot_print_c` 打 `W!` 后 spin。
- 页表构建已由 `boot/early_mmu.c` 的 C 函数承担：签名扩为
  `aarch64_build_boot_page_tables(tables, boot_base, device_base, device_end)`，
  head.S 解析回退常量或停靠窗口后传参。
- 早期打印 `boot_vectors.S` `.Lprint_c`：槽值非 0 用槽值，否则回退
  `0x09000000`。MMU 前/后均可用（MMU 后 identity 窗口覆盖 B 区）。
- flag 未置 → 使用 `boot_layout.h` 常量（旧 loader / 无 DTB 兼容）。

### 2.4 内核 C 侧（新 TU kernel/arch/aarch64/memory/device_window.c）

```c
void aarch64_device_window_init(const struct boot_context *ctx);
void aarch64_device_window_get(uint64_t *base, uint64_t *end); /* 2MiB 对齐 */
```

- 静态初值 = `AARCH64_BOOT_DEVICE_BASE/END`（host 测试零接线也正确）。
- `init`：flag 置位且 base 2 MiB 对齐、blocks ∈ [1,1024] → 采用；否则保持默认。
- 调用点：`arch_early_init`（layout 校验后、`aarch64_ram_init` 前）+ `log_info`。
- 消费方改造（getter 替换本地宏，签名不变）：
  - `early_arena.c`：`validate_ranges` / `count_buckets` 的 D 检查；
  - `runtime_tree.c`：`RT_D_BASE/END` → build/validate 入口读 getter；
  - `page_table_selftest.c`：`prune_pa_reserved`；
  - `boot/main.c` M0 selftest：`boot_map_walk_pmd_table` 的 device 槽区间按
    窗口与每张 PMD 表的覆盖范围求交集。

### 2.5 QEMU 行为变化（预期）

QEMU virt DTB 推导窗口 = GICD@0x08000000 + GICC@0x08010000 + v2m@0x0802xxxx +
PL011@0x09000000 → 9 块 `[0x08000000, 0x09200000)`（槽 0x40..0x48），不再是
16 块。M1 tree D 条目 16→9。`qemutests/aarch64_m1_evidence.py` 的
`_D_END` 常量同步改为 `0x09200000`。host 测试因回退值仍是旧 16 块窗口，
不受影响。

## 3. 明确不做

- 早期 identity map 超过 2 GiB 的映射（平台上限，另行任务）；窗口出界 →
  head.S fail-closed。
- 多设备相距过远（如 RPi4 GIC 与 UART 相距 2.8 GiB）→ 单窗口推导块数爆表，
  loader 不置 flag；本质仍卡在 2 GiB 上限，待上限任务一并解决。
- DTB 白名单其余项（PL011 interrupts 形状、GICv3）、GIC Group-1、spin-table。

## 4. 验收

1. `make test-host`（含 test_bootinfo_abi / 新 test_aarch64_fdt_early /
   test_early_arena / test_aarch64_boot_layout）。
2. `make KERNEL_SELFTEST=1 test-kernel-selftest`（含 M0 boot-map selftest）。
3. `make test-aarch64`（含 m1-evidence）+ `make test-qemu SUITE=phase-0`。
4. QEMU 启动日志出现 device window log，串口/GIC 正常（systest 间接覆盖）。
5. roadmap.md 三行状态回填。
