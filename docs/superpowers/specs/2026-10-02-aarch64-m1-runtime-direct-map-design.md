---
title: OS01 aarch64 M1 运行期直映设计
created: 2026-10-02
type: spec
status: draft-for-review
tags: [osdev, aarch64, memory, boot]
---

# aarch64 M1 运行期直映

## 1. 意图、基线与完成标准

依据 roadmap P2 M1，在 BSP 上、首次 PSCI CPU_ON 前，使全部 PMM 可分配 RAM 经 `ARCH_PAGE_OFFSET + PA` 可读写。支持超过 M0 的 `0x80000000` 上界以及不连续 RAM；RAM 与设备使用不同内存属性。用户明确选择：**M1 同时收紧非 RAM 映射**。

基线为 `2523a31`，M0 已合入。roadmap 中关于 C 补图及“M0 是下一项”的描述已过时；本设计以当前 head.S 的固定启动映射为准。M0 不修改，M1 建立独立 TTBR1 根，去除高半区不必要的 Normal 映射。TTBR0 的启动 identity 映射暂时保留，供低地址 AP trampoline、栈和 boot metadata 使用。

收紧粒度为 **2 MiB block**：高半区 Normal 映射恰为发布的 RAM map 与启动保留 block 的并集。内核、boot 页表、boot stacks、handoff 所在 `[0x40000000,0x40200000)` 是唯一固定 Normal 保留 block；其中未使用字节仍可访问。Device 窗口另列，不称为可分配 RAM。本项不提供字节级或 4 KiB 级启动保留区隔离，也不宣称已移除所有低地址别名。

M1 不接真实 Slab、不实现通用 VMM、block 拆分、用户空间、COW、ASID 或 AP 启动后的页表修改。运行期直映是启动时一次建立、之后长期使用的内核映射。

成功标准：普通和 selftest 镜像均完成建表和切换；各 PMM zone 的每个 frame 都有精确属性的直映；高半区其他 Normal block 缺失；BSP/AP 均使用同一 TTBR1；实际读写、SMP、GIC、Timer 和异常诊断验收通过。

**双架构目标（用户补充）**：最终同时支持 x86_64 与 aarch64，尽量让通用调用方不感知差异。M1 的 arch 后端必须落到共同的启动直映契约；不能只新增 aarch64 专用入口，再要求未来 `kernel_main` 用 ifdef 选择。具体接入和现状差距见 §10。

## 2. 已核实的约束

- `head.S` 建立六页启动表，TTBR0/1 共用根。固定 Normal 区含 `0..1 GiB` 及 `1..2 GiB`，其中 `[0x08000000,0x0a000000)` 为 Device。首个 DRAM block 保持 EL1 可执行，其余 RAM PXN/UXN。
- `aarch64_ram_map_get()` 提供最多 16 个、2 MiB 对齐、已排序并排除内核/handoff 的真实可用 RAM 区间；`pmm_arch_normalize()` 直接转为通用范围。
- PMM `pages_struct` 按最低至最高 RAM 的跨度计费，空洞也占描述符。元数据大小取决于跨度，不能只按 RAM 总量估计。
- 当前 `PMMngr.start_brk = _kernel_end`；linker 只保证内核结束不越过 `0x401e0000`，没有保证随后元数据写入不会覆盖 handoff。因此扩大内存前必须先规划和放置元数据。
- `pmm_init()` 末尾调用 Slab stub 并初始化 4 KiB 子页池。首次 `alloc_4k_page()` 会直接写取得的物理 frame；建直映前不能使用它。
- RAM map 容量为 16，但 PMM `MAX_NR_ZONES` 为 10，超出的 zone 当前会被跳过。本项对超容量输入明确失败，禁止悄悄丢失 RAM。
- 4 KiB 原语只支持 L3 leaf，block 返回 ECONFLICT；active-root 判定和现有 smoke 只读取 TTBR0。分离两根后必须修正这个假设。
- AP 调用 `write_tcr_mair_ttbr()`，当前会再次把两根装成 boot 根。SMP 发布只清理六页 boot tables 到 PoC，不包含新表。
- 当前 TCR 采用 48-bit VA、4 KiB granule、40-bit PA；本项 PA 上限为 `1 << 40`，不扩大 IPS。

## 3. 方案比较与选择

| 方案 | 成本与限制 | 决定 |
|---|---|---|
| 新建独立 TTBR1，TTBR0 保留启动根 | 需要早期 arena、TTBR1 发布及现有原语适配；新树可在切换前完整检查，低地址 AP 路径稳定 | 推荐 |
| 原地修改共享 boot 根 | 会同时收紧 identity 映射，必须维护 AP 使用的全部低地址路径；清除/替换活动项要求更复杂的失效维护 | 不选 |
| 只扩展共享 boot 根 | 改动较少，但留下固定窗口里的非 RAM 映射 | 不满足用户选择 |

不克隆整个 boot 树再删洞；从空树按明确的允许集合建立，避免继承遗漏。新树不与 TTBR0 共享任何可写中间表。原 boot 六页一直保留。

## 4. 早期资源与 PMM 接入

### 4.1 两遍规划，先验证后写内存

在 `aarch64_ram_init()` 成功后、`pmm_init()` 前做纯计算：

1. 验证区间数、排序、不重叠、2 MiB 对齐、端点及 PA 上限；非空 RAM 区间数不得超过 `MAX_NR_ZONES`。对 Device 窗口的冲突直接失败。
2. 提取共用 PMM 元数据 layout calculator：按现有 bitmap、Page、Zone 大小与对齐公式计算各 offset 和总长度，所有乘加、向上对齐检查溢出。生产 PMM 初始化与 aarch64 preflight 必须调用同一函数，不能维护两份 sizing math。
3. 根据目标映射集合统计所需表页：一页 L0、每个被覆盖的 512 GiB 桶一页 L1、每个被覆盖的 1 GiB 桶一页 L2。仅使用 L2 block，不分配 L3。包含 Device 与保留 block；每个桶只计一次。40-bit PA 下最多 `1 + 2 + 1024 = 1027` 页。
4. arena 大小为 `align_up(metadata_bytes, 4 KiB) + table_pages * 4 KiB`，整体向上对齐到 2 MiB。从 RAM map 与 M0 DRAM 窗口 `[0x40200000,0x80000000)` 的交集中，选择能完整容纳 arena 的第一个连续区间。arena 不跨 RAM 空洞、handoff 或 kernel exclusion。

使用现有 M0 映射访问 arena；这里不调用任何分配器。找不到连续空间时打印带需求/可用空间信息的 FATAL，在首次写入前停机。稀疏跨度即使 PA 可表示，也可能因元数据要求而不受支持；明确失败优于 data abort 或部分启用。

### 4.2 所有权与预留

将 `PMMngr.start_brk` 设置到 arena 的高半区起点。PMM 用 layout calculator 设置各指针，元数据段之后是只增不减的 4 KiB 页表池。初始化前核对 layout 终点在元数据段内。

共用 PMM 增加启动预留策略 hook：默认保持 x86_64 既有的 kernel/metadata 前缀预留；aarch64 覆盖为**只预留 arena 对应的 RAM frames**，内核/handoff 已由 normalizer 排除。在清空 RAM bitmap 并建立 zones 之后、Slab/子页池初始化之前完成预留。bitmap 用 RAM-relative 索引，计数只在 free→reserved 时更新；设置 Kernel/Init/PTable_Mapped 属性并保持 refcount 约定。不扩大共享结构以存储 arch 特有状态。

不得因 arena 位于某个高一点的 RAM 区间，就把它之前所有可用 RAM 永久预留。arena 尾部对齐余量也归启动资源所有，不交给普通分配器。生产过程中持有 arena 的不可变 base/end/table-used 信息；后续 M3 可研究回收策略，M1 不释放这些页。

本项保留 aarch64 Slab stub 和 `pmm_init()` 内的既有调用。真实 Slab 接入及初始化拆分仍属 M2。正常路径只有 M1 完成后才允许调用普通 `alloc_4k_page()`。

## 5. 新 TTBR1 的精确映射契约

令 `R` 为完整归一化 RAM map，`B=[0x40000000,0x40200000)`，`D=[0x08000000,0x0a000000)`。新树只映射 `ARCH_PAGE_OFFSET + (R ∪ B ∪ D)`；arena 已是 R 的子集。B 以外不存在额外 Normal leaf。R 的稀疏空洞不能通过连续填充获得映射。

| 集合 | 属性 | 权限 |
|---|---|---|
| R | MAIR AttrIndx 1：Normal WBWA；Inner Shareable；AF | EL1 RW、EL0 禁止、PXN=1、UXN=1 |
| B | 同 M0 Normal block | EL1 RW、EL0 禁止、PXN=0、UXN=1 |
| D | MAIR AttrIndx 0：Device-nGnRnE；Non-shareable；AF | EL1 RW、EL0 禁止、PXN=1、UXN=1 |

B 保持原有可执行权限，避免把 M1 变成 kernel W^X 改造。D 是当前 QEMU virt 设备窗口的保留映射，不通过 RAM normalizer推导；不扩到整个 `0..1 GiB`，不增加新 MMIO 区域。TTBR0 里的既有 Device 别名保持相同属性。TCR、MAIR 与 block 大小不变。

根与中间项为最小 table descriptor，L2 leaf 为 block descriptor，Contiguous bit 清零。不得复制 x86 PAGE_* flags。新 tree builder 放在 arch 层，纯建表逻辑通过页表池 callback 取得已映射页，和寄存器操作分离，便于 host 测试；已有 4 KiB 原语继续不拆 block。

在启用前全树验证：目标 block 数量及每个 PA/属性精确匹配；树中所有有效项属于允许集合；所有中间 PA 都在已预留 table pool，且无环、无别名中间表。发现非预期项或 table pool 耗尽则停止，不能留下部分直映继续启动。selftest smoke 期间允许 §7 描述的临时独占子树；拆除之后再次全树验证。首次 CPU_ON 前最终树必须重新满足此精确集合与 arena 所有权不变量。

## 6. BSP 切换与 AP 发布

### 6.1 BSP

首次切换时 AP 尚未启动，DAIF 保持屏蔽。新树在旧直映下构造。所有保留的 VA→PA 与属性和旧树一致，只增加 RAM 映射并删除多余映射，不把已有同 VA 改为不同 PA 或内存类型。

切换顺序：完成所有表写入 → `dsb ishst` → `msr ttbr1_el1, new_root_pa` → `isb` → `tlbi vmalle1` → `dsb ish` → `isb`。本核全失效同时消除旧 high-half leaf 和 walk-cache 结果。TTBR0 不修改，失效后可重新填充。

这里未对活动树执行 valid→different-valid 描述符替换；不把此顺序推广为 M3 原地修改页表的 break-before-make 实现。屏障/TLBI 的原则参照 [Arm 内存管理指南](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf?revision=01a01804-ca64-4e19-a55e-2af56afea5a5)；寄存器切换序列是本设计对启动前单核场景的具体选择。

切换后只执行可由 B/R/D 访问的代码和数据；boot root 仍在 B，保持可读。建表失败可报告并停机；切换完成后验证失败也停机，不回退到宽松启动表继续运行。

### 6.2 AP

增加独立、8-byte 对齐的 `.boot.bss` 标量 `runtime_ttbr1_pa`，以汇编 literal helper 访问其低物理地址，避免高半区 C 到低符号的 ADRP 距离问题。BSP bootstrap 时仍使用 boot 根；AP 路径在打开 MMU 前要求标量非零、4 KiB 对齐且符合 40-bit PA 范围，TTBR0 装 boot 根、TTBR1 装 runtime 根。禁止零值时静默回退。不改变已有 per-CPU boot struct/offset ABI。

新 root 仅在 BSP 完成切换、smoke 子树清理及最终验证后发布。`aarch64_smp_publish_boot()` 在首次 CPU_ON 前，按实际 cache line 清理 arena 内全部已用 runtime table pages、root 标量及现有 boot metadata 到 PoC，最后 `dsb sy`。selftest 的 arena 外临时表必须已脱链/回收，不留在发布树中。当前六页表的清理保留，不能仅扩大一个固定长度常数。release/acquire 不能替代向 MMU/cache-off AP 的 PoC 发布。

AP 在其既有 pre-MMU 本地失效及 barrier 路径下启用两根；在 C 入口核对实际 TTBR1 的物理根等于发布值，失败不发送 online ACK，进入既有 secondary fatal 路径。selftest 另在 ACK 前执行下面的探针，成功发出带 CPU id/root/probe PA 的 M1 AP marker。AP 启动后 runtime direct-map tree 不再修改，不引入 SMP shootdown。

selftest 的 BSP 在 M1 成功后、PoC 发布前，从空闲 PMM frame 中精确取得一个 2 MiB frame 的独占所有权，写入其首个 4 KiB 的 sentinel。若 R 含 PA≥`0x80000000` 的空闲 frame，必须从此范围选择；若配置无窗口外可用 RAM（256/512 MiB），从 R 中选择一个窗口内空闲 frame。存在窗口外 RAM 却无法取得窗口外空闲 frame 时，扩展覆盖测试明确失败，不以窗口内探针替代。生产 PMM helper 在锁内只 claim 一个已确认空闲的 frame，并更新正常分配账本；不能对未拥有的边界地址直接写入。

probe PA/expected sentinel 使用独立的 `.boot.bss` selftest 标量，PoC 发布同时清理这些标量和实际写入的数据 cache lines。所有 AP 通过高半区别名读取 sentinel，在验证成功后才 ACK。probe 的所有权至少保持到全部被请求 AP 确认；若全部 ACK，则 BSP 完成探针判定后 free frame；若有 timeout/拒绝 ACK，则该 frame 保留至本次 boot 结束，防止迟到 AP 读取已复用内存。probe 不修改 runtime tree，普通镜像不分配该 frame。

## 7. 原语与启动顺序适配

共同的启动直映入口放在 `kernel/include/arch/` facade，两种架构提供同名实现，具体契约见 §10。aarch64 内部接口包括早期规划/预留准备、运行期直映安装、取得 runtime root、发布 table ranges；其头/源成对放置于 `kernel/include/arch/aarch64/` 和 `kernel/arch/aarch64/memory/`，通用调用方不包含 per-arch 头，也不接触原始 descriptor bits。

`arch_get_page_table()` 的既有 TTBR0 语义不变。aarch64 增加明确读取 TTBR1 的 kernel-root helper。4 KiB 原语的 active-root 判定认可安装在 TTBR0 或 TTBR1 的根；调用者必须选择与 VA regime 匹配的 root，不能拿 TTBR0 验证高半区映射。现有高半区 map smoke 和 sync-fault precondition 改读 TTBR1，其 local TLBI 继续服务 BSP/pre-SMP 用途。

4 KiB smoke 的 `AARCH64_PT_SELFTEST_VA` 使用 L0 槽 256；本稿直映 PA<1 TiB，只使用槽 0/1，因此槽 256 可供 smoke 独占。开始前要求该整个槽 invalid，不仅要求某个 leaf ENOENT。测试仍 against 活动 TTBR1，以验证真实映射；缺失中间表可通过 `alloc_4k_page()` 产生最多三页 arena 外临时表。

smoke 成功或任何失败分支都必须清理其独占子树：先捕获并验证已建立的中间页物理地址（部分建表也需覆盖），清除 root[256] → `dsb ishst` → `tlbi vmalle1` → `dsb ish` → `isb`，确保任何临时 leaf/walk-cache 都失效，再释放测试数据页及实际建立的中间页，各释放一次。若某个地址/所有权无法证明，停止并保留相关页，不能猜测地址释放后继续启动。generic 4 KiB unmap 仍不承担中间表回收；本次清理由 smoke 对独占槽的明确所有权负责。

随后验证槽 256 invalid，并重跑 §5 的精确全树验证；其余 tree 完全未改且都属于 arena。M1 success/coverage-ready 只能在最终验证后发布；smoke 中间的安装态不暴露为 ready。普通镜像没有临时子树，安装后直接执行最终验证。sync-fault 独立变体在清理之后执行，所以其缺失地址预期不变。

启动顺序：

```text
M0 / VBAR
  → aarch64_ram_init
  → early layout + arena selection
  → pmm_init（精确预留 arena；Slab stub；subpage list）
  → M0 descriptor selftest（仅 selftest，读 TTBR0）
  → arch_boot_direct_map_init
      → runtime TTBR1 build / validate / install（所有镜像）
      → M1 selftest / 4 KiB smoke / 子树清理（仅 selftest，读 TTBR1）
      → 最终全树验证 / coverage-ready / BSP PASS（所有镜像）
  → AP probe claim / sentinel 初始化（仅 selftest）
  → sync-fault 独立探针（专用变体）
  → DTB / GIC / runtime tables PoC publication
  → CPU_ON / AP root validation
  → Timer / IRQ
```

PMM 的纯 alloc/free smoke 可保留在 M1 前，但不能写未建立直映的 frame；data-page/subpage smoke 只能在 TTBR1 安装后的 init 内部验证阶段运行，或在共同 init 成功之后运行。删除 4 KiB smoke 对 `data_pa < 0x80000000` 的旧 M0 验收限制，改为检查属于 PMM RAM 且 runtime direct map 有正确 block。M0 全表断言仍测试旧 boot 根，因此仍能独立证明启动契约。

## 8. 错误路径与验收

### 8.1 明确失败

布局溢出、超过 IPS、zone 容量不足、无低窗口 arena、RAM/Device 冲突、表池不足、非法 descriptor、重复 install/publish 均输出 M1 FATAL 原因并停机。不得仅靠 assert，NDEBUG 下仍执行。**BSP 的规划/建表/清理/最终验证失败**发生在 CPU_ON 前，harness 断言没有 AP online/Timer marker。BSP success marker 只能在实际 TTBR1、TLB completion 和最终树验证完成之后发出，不表示 AP 已通过。

**AP 的 root/probe 验证失败**发生在 CPU_ON 后：可用日志路径下报告带 cpu id 的 M1 AP FAIL；pre-MMU malformed-root 走既有 secondary panic，不能要求必然有 C 日志。失败 AP 不发送 online ACK。BSP 走现有 timeout/DEGRADED 处理并保留 probe 所有权；M1 正常验收拒绝 DEGRADED、缺失 AP PASS、root 不同或出现 AP FAIL。既有专用 no-ACK/降级回归继续按其独立预期判定，不能把它当作完整 M1 SMP 成功；BSP 的 Timer 在降级模式可继续，故此类用例不要求没有 Timer marker。

### 8.2 Host

- 共用 layout：普通、小 RAM、最大 PA 跨度、溢出与对齐；生产计算与 preflight 一致。
- arena：首选区间、首区间太小但后续可用、空洞、无连续空间、handoff/kernel 排除；不写候选区域外。
- reservation：bitmap RAM-relative、重复预留计数稳定、arena 前后 free frames 不受影响，x86 原有预留回归。
- tree：跨 1 GiB/512 GiB 边界、PA 接近 1 TiB、稀疏 ranges、范围数量超限、Device 冲突、精确表数、容量不足；用独立 walker 检查所有叶子和空洞，检查保护 canary。
- harness：缺/重复/乱序 marker、BSP/AP root 不同、只见 UEFI banner 或超时都不能通过。

### 8.3 QEMU

扩展现有 UEFI harness 的 RAM 参数，默认继续 512 MiB。普通/selftest 分别覆盖 256 MiB、512 MiB、2 GiB、4 GiB；其中 2/4 GiB RAM 必须实际访问 PA≥`0x80000000`。512 MiB、2 GiB 各跑 1/2/4 CPU；复用 GIC SPI、Timer/IPI 和 sync-fault 回归。不能用仅支持固定 512 MiB 的成功条件判定更大 RAM。

对每个 PMM zone，从空闲状态精确取得首/尾可用 frame 的临时所有权，读写其中首/尾 4 KiB 的两个 sentinel 并释放；仅对已确认不含元数据/页表/固件保留的 frame 写入。检查每个 represented frame 的 runtime block PA/属性，同时比较 PMM zones 与 RAM map；不能只测一次普通分配。

稀疏输入由 host synthetic ranges 覆盖。另用专用 selftest 变体在归一化 RAM 发布前注入一个 2 MiB 保留空洞（仅移除原本真实 RAM，不伪造物理内存），让 PMM 和 M1 接受同一份改变后的 map；访问洞两侧页、walker 确认洞 absent。该变体独立记录输入，正常镜像不改变固件 map。无需把 QEMU virt 的 DTB memory 节点当成 UEFI RAM 来源。

证明收紧不仅改变表内存：切换前以 `AT S1E1R` 预热一个 M0-only Normal 地址，切换后 `AT S1E1R` + `isb` 读取 PAR_EL1 要求 fault；同时确认允许 RAM 和 Device 翻译仍正常。不实际解引用缺失地址，不引入可恢复缺页。专用 sync-fault 变体仍验证预期 fatal data abort。

本设计阶段未运行这些构建/测试；上述为实施验收要求，不是已通过的证据。

## 9. 文件职责与后续交付

| 文件/目录 | 变更职责 |
|---|---|
| `kernel/memory/pmm.c`、`pmm_arch.c` 与对称公开头 | 共用 checked layout calculator；arch reservation hook；保持 x86 默认行为 |
| `kernel/arch/aarch64/memory/` 与 `kernel/include/arch/aarch64/` | early arena、纯 tree builder、TTBR1 installer/query/publication 契约及精确预留 override |
| `kernel/arch/aarch64/boot/main.c` | 接新启动顺序、验证与 root 选择；不扩展硬件 init 职责 |
| `kernel/arch/aarch64/head.S`、`boot/boot_percpu.c` | 独立 runtime root 标量及 AP 两根装载；保留 M0 builder |
| `kernel/arch/aarch64/memory/page_table.c`、对应头 | 移除 active root 等于 TTBR0 的假设；不扩为通用 VMM |
| `kernel/arch/aarch64/smp/smp.c` | 清理 runtime table pages/root/probe 到 PoC，AP 验收和超时所有权 |
| `kernel/include/arch/boot_memory.h`、两个 arch memory 后端 | 共同初始化、ready/coverage 查询与失败语义；x86 适配及 OOM 状态传播 |
| `hosttests/`、`qemutests/`、build harness | layout/tree/reservation 边界测试；RAM 参数与独立 sparse/failure 变体 |
| `docs/roadmap.md`、内存/arch 文档 | 实施闭环后更新状态与实际限制 |

本稿供用户审阅；尚未修改产品代码或标记 M1 完成。书面设计获认可后另写实施计划。M2 以“PMM 元数据及 arena 已安全预留、M1 完成后全部可分配 RAM 已直映”为入口，M3 再处理动态映射与页表回收。

## 10. 与 x86_64 的对齐及共同接口

### 10.1 匹配程度

两种架构都采用“汇编提供最小启动映射 → 共用 PMM 建立物理页账本 → C 扩展运行期内核直映 → AP 启动”的阶段模型。当前 x86_64 的 `x86_64_boot_memory()` 调用 `pmm_init()` → `vmm_init()`；Slab 初始化实际位于 `pmm_init()` 内。x86_64 的 `vmm_init()` 在旧根上补 2 MiB leaf，通过 Slab/calloc 分配中间表，并在存在 `ZONE_UNMAPPED_INDEX` 时停止扩展。它没有重建精确映射集合或移除启动映射，也没有实现本稿的早期 arena。

因此本稿匹配阶段目标和 PMM 数据模型，但不是 x86_64 现有实现的逐行移植。更严格的 sparse/hole、权限及资源失败契约是增量改进；不能因为加入同名入口，就声称两种架构已获得全部相同行为。

| 层次 | 应共同拥有 | 后端保留的差异 |
|---|---|---|
| 物理资源 | `MEMORY_RANGE[]`、PMM layout、bitmap/Page/Zone、预留范围记账 | UEFI/E820 解析，bootstrap 可访问范围、arena 选址约束 |
| 直映阶段 | 输入有效性、成功/失败语义、可直映 RAM 范围与启动前约束 | x86 在旧根扩展；aarch64 新建/安装 TTBR1；设备映射策略 |
| 页表机制 | 后续 M3 的映射权限/内存类型语义、页表所有权 | descriptor 编码、CR3/TTBR、TLBI/屏障、AP cache 发布 |
| 启动调用方 | 完成内存阶段后才启动依赖资源的子系统及 AP | linker 符号、early framebuffer、DTB、trampoline 等准备 |

TTBR0/TTBR1 分开是 aarch64 的正确后端选择；x86 用单 CR3 根并共享内核半区，同样可以满足“用户地址空间切换时内核映射持续可用”的共同语义。不能为了相同数据结构强迫 x86 使用两个根，或强迫 aarch64 复制 PGD[256..511]。

### 10.2 M1 内必须完成的最小共同边界

增加 `kernel/include/arch/boot_memory.h` facade，声明 `int arch_boot_direct_map_init(void)`。两个强实现均位于 `kernel/arch/<arch>/memory/`，编译期选取；必需实现缺失时链接失败，不提供成功的 no-op 弱默认。

契约：只能在共用 PMM 初始化后、AP 启动前调用；成功返回 0，表示后端确定的可直映 RAM 范围已建立内核直映，页表写入、本核失效维护及最终验证完成。失败返回负错误，启动调用方必须记录并停机。入口之前及执行期间允许读写已证明由 bootstrap 覆盖的内存，以及只使用该覆盖范围的后端启动分配（包括既有 x86 Slab）；禁止将尚未完成的扩展范围当作已可用直映。aarch64 本阶段只使用其 early arena，普通 4 KiB allocator 直到 TTBR1 安装后才可使用。该入口不启动 AP、不接设备驱动、不提供任意 VA map/unmap。缺少 early resources 的错误可在更早的 preflight 阶段停机。

x86 后端在 M1 中适配现有 `vmm_init()`，替换 `x86_64_boot_memory()` 的直接调用；保留现有 framebuffer、Slab 和预留顺序。其返回值必须依据真实完成状态，不能只把无返回值的 legacy 函数包成无条件成功：补足中间页表分配失败检查和状态传播，避免 NULL/corrupt descriptor 被当作成功。aarch64 后端包装本稿的建表/切换/验证阶段，替换直接调用专用 installer 的启动站点。

共同 facade 还声明以下确定接口（含 `<stdbool.h>`、`<stddef.h>` 和 `<memory/memory_map.h>`）：

```c
bool arch_boot_direct_map_ready(void);
int arch_boot_direct_map_ranges(const struct MEMORY_RANGE **out, size_t *count);
```

`ready()` 初始为 false，只有共同 init 的最终验证成功才置 true。aarch64 的 smoke 及其清理确定编排于 init 内部，安装后的 internal-ready 与此公开 ready 分开；内部 smoke 允许在公开 ready=false 时检查活动根。公共 init 每 boot 只允许调用一次，重复返回 `-EALREADY`，不改现有树及此前 ready 状态；第一次 init 失败始终保持 false，调用方必须停机，不支持重试。

`ranges()` 任一输出指针为 NULL 时返回 `-EINVAL`（不写输出）；参数有效时首先设置 `*out=NULL,*count=0`，未 ready 返回 `-EAGAIN`，ready 则返回 0 并给出数组。数组各项为 2 MiB 对齐的 `[phys_start,phys_end)` 非空物理区间、`type=MEMORY_TYPE_RAM`，升序、不重叠，相邻区间合并；最多 `MEMORY_RANGE_MAX` 项，超过容量使 init 返回 `-ENOSPC` 而非截断。输出由后端持有，不分配、不释放，ready 后只读并存活至 boot 结束。地址为 PA，不能让调用者解析 zone 下标或 root 寄存器。

coverage 包含已预留 arena 等 RAM frames，不是当前 free-list 快照；不包含固定内核保留 B、Device D 或 bootstrap 多余 Normal 映射。aarch64 M1 要求其集合等于完整 PMM RAM zones；x86 若沿用 unmapped-zone 限制，输出仅包含其实际已扩展的 PMM zone 子集，不能宣称所有 PMM frames 都可直接解引用。init 在发布前遍历覆盖范围验证映射成立。ready/coverage 为 BSP 启动阶段接口，不承诺 AP 在线后的动态映射并发查询；其不可变输出可以供后续读者使用。

共同 init 的参数/状态/容量错误采用上述 errno；后端 OOM 为 `-ENOMEM`，不合法范围/布局为 `-EINVAL`，最终硬件/页表核对失败为 `-EIO`。遇到无法继续报告的异常仍走 fatal 路径。失败不暴露部分 coverage，不要求回滚已建的表，因为启动调用方立即停机。共同契约测试覆盖未初始化、NULL 参数、成功输出、容量失败、OOM、重复 init 与公开 ready 的准确时机。

PMM layout 与预留记账应放在 `kernel/memory/`，架构只提交物理预留区间及 placement 约束。§4 的 reservation hook 用于选择范围，实际 bitmap/refcount/zone 计数更新由共用 helper 完成；不能在 aarch64 再复制一套 allocator 账本操作。RAM normalizer 的 per-arch 原始类型也不能向上渗透。

`runtime_ttbr1_pa`、table-pool callback 和 root literal helper 都是 aarch64 内部机制。取得“通用内核映射”的接口不能永久命名为 TTBR1；当前专用 smoke 可通过 per-arch helper 访问。在 M3 建立明确的内核映射对象/查询 facade 后，通用调用方使用该对象。必须区分 x86 的主内核表 `kernel_map` 与当前用户进程 CR3；不能把“读取当前寄存器”误当成始终取得主内核表。

### 10.3 后续收敛顺序及验收边界

M1：共同直映阶段入口、coverage 输出、PMM layout/预留 helper，完成 aarch64 的严格运行期直映；同时验证 x86 adapter 和原有启动回归。Host 测试共同契约，QEMU 仍分别使用两种架构的 harness。

M2：处理真实 Slab 的锁/IRQ 后端和资源顺序，评估两种架构共同采用“PMM 元数据/预留 → runtime direct map → Slab → 普通分配”的顺序。该目标是后续改变，M1 不能把目前 x86 的 Slab-before-direct-map 写成已经修正。

M3：把 `memory/vmm.c` / `memory/vmm.h` 中的 x86 descriptor 操作与 raw flags 收到 x86 arch 后端；通用层使用映射语义、所有权和错误契约。统一内核 map/query/unmap 及 local/SMP TLB 维护接口，保留 block/page 粒度差异的后端处理。M4 才统一用户空间、VMA、ELF、COW 和 fault/uaccess。

x86 的非 RAM 映射收紧另行设计和验收：它牵涉 early framebuffer、AP 启动和既有 MMIO 调用，不能借本次 facade 接入默默改变。共同接口允许后端资源与安装策略不同；长期用户可见语义收敛要靠双架构测试证明，不靠相同函数名或 PGD/PUD/PMD 命名证明。
