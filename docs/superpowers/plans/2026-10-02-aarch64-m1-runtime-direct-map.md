# AArch64 M1 Runtime Direct Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在首次 CPU_ON 前完成 aarch64 严格运行期直映，并让两种架构通过共同启动直映接口报告真实完成状态和 RAM coverage。

**Architecture:** PMM 共用 checked layout、预留策略与记账；aarch64 先规划低窗口 arena，再初始化 PMM，独立构建和安装 TTBR1。安装、内部 smoke 清理、最终验证完成后才发布 ready/root；AP 在打开 MMU 前加载新根。x86_64 保持既有资源顺序，通过 checked boot mapper 传播错误。

**Tech Stack:** freestanding C、AArch64 assembly、Clang/LLD、native hosttests、Python QEMU harness、UEFI。

**Spec:** [2026-10-02-aarch64-m1-runtime-direct-map-design.md](../specs/2026-10-02-aarch64-m1-runtime-direct-map-design.md)；设计基线 `2523a31`，已评审 spec 提交 `1bda72b`。执行前读 spec 与 AGENTS.md。

## Global Constraints

- `ARCH_PAGE_OFFSET=0xffff000000000000`，48-bit VA、4 KiB granule、PA < `1 << 40`；不改 TCR/MAIR。
- 新 TTBR1 只映射 `OFFSET+(R∪B∪D)`：R 为全部 PMM RAM，B=`[0x40000000,0x40200000)`，D=`[0x08000000,0x0a000000)`。2 MiB L2 block；RAM/Device 冲突直接失败。
- R：AttrIndx 1、WBWA、Inner Shareable、AF、EL1 RW、EL0 禁止、PXN/UXN。B 同 M0，PXN=0、UXN=1。D：AttrIndx 0、Device-nGnRnE、Non-shareable、AF、PXN/UXN。Contiguous=0。
- TTBR0 和六页 boot tree 保留。新树无共享中间表；runtime root 和所有中间表长期归 arena 所有。
- R 最多 16 项且非空 zone 不超过 `MAX_NR_ZONES=10`。共同 coverage 最多 `MEMORY_RANGE_MAX=64` 项，排序、合并相邻、只含 RAM。
- arena 在 R∩`[0x40200000,0x80000000)`；大小为 `round2M(round4K(metadata_bytes)+table_pages*4096)`。表页数为 1+不同 512 GiB 桶数+不同 1 GiB 桶数，最大 1027。
- preflight 不使用 allocator；aarch64 普通 4 KiB 分配只能在 TTBR1 安装后开始。保留 Slab stub 和 pmm_init 内既有调用。
- 共用 init 每 boot 一次，包括失败；重复 `-EALREADY`。最终验证之前 public ready=false。BSP 失败记录原因后循环 halt，不能只执行一次 WFI。
- 公共头全部在 `kernel/include/`，arch 专用头在 `kernel/include/arch/aarch64/`；不在源目录旁新增头。
- M2 Slab、M3 通用动态 VMM/回收、M4 用户空间及 x86 非 RAM 收紧不在本计划。结构/头改变后执行 `make clean`，不能依赖增量构建发现 ABI 变化。

## Review Focus

1. 稀疏跨度导致 metadata 溢出或低窗口 arena 不足：Task 1/3 在任何内存写入前失败。
2. arena 位于后续 RAM 区间：Task 2 只预留 arena，之前的 RAM 仍可分配；重复预留不重复计数。
3. 跨 1 GiB/512 GiB 与接近 1 TiB 的输入：Task 4 精确计表并检查全树、canary、冲突及池耗尽。
4. 切换成功但 smoke 部分建表失败：Task 6 先脱链/TLBI 再释放，不能发布 partial ready/root。
5. AP 迟到、root 无效或 probe 失败：Task 7 在 ACK 前校验；失败保留 probe，Task 8 parser 拒绝假成功。

## 文件与任务依赖

Task 1 → Task 2 → Task 3 → Task 4 → Task 6 → Task 7 → Task 9；Task 5 依赖 Task 1/2，Task 8 在 Task 6/7 marker 契约确定后接入。每个任务交付独立 host 测试或 harness parser 测试；Task 9 完成双架构集成验收。下述 Create 文件是计划新增，执行时才创建。

| 文件 | 职责 |
|---|---|
| `kernel/memory/pmm_boot.c`、`kernel/include/memory/pmm_boot.h` | checked metadata layout 与启动预留范围类型 |
| `kernel/memory/pmm.c`、`pmm_arch.c`、`kernel/include/memory/pmm_arch.h` | 共用预留/精确 claim 记账与架构范围策略 |
| `kernel/include/arch/boot_memory.h` | 双架构 init/ready/ranges 契约 |
| `kernel/arch/aarch64/memory/{early_arena,runtime_tree,boot_direct_map,m1_selftest}.c` | preflight、纯建表、启动编排与 smoke 所有权 |
| `kernel/include/arch/aarch64/{early_arena,runtime_tree,boot_direct_map,m1_selftest}.h` | 对应 arch 内部接口 |
| `kernel/arch/x86_64/memory/boot_direct_map.c` | x86 adapter；真实 checked VMM 完成状态 |
| `kernel/arch/aarch64/{head.S,boot/boot_percpu.c,boot/main.c,smp/smp.c,memory/page_table.c}` | 根发布、启动顺序、AP、活动根适配 |
| `hosttests/test_m1_*.c`、`hosttests/mock/m1_include/` | 生产纯逻辑/PMM/编排的 native 测试与硬件链接替身 |
| `qemutests/aarch64_m1_{evidence,matrix}.py` | 严格 marker parser 与实际镜像矩阵 |
| `kernel/Makefile`、`hosttests/Makefile`、`mk/{project.mk,profiles/aarch64-clang.mk,targets/aarch64.mk,components/run.mk}` | 源清单、native 测试入口、隔离变体与运行目标 |

### Task 1: checked PMM layout 与可运行 host 入口

**Files:** Create `kernel/memory/pmm_boot.c`, `kernel/include/memory/pmm_boot.h`, `hosttests/test_m1_layout.c`; Modify `kernel/memory/pmm.c`, `kernel/Makefile`, `hosttests/Makefile`, `mk/components/run.mk`。

**Interfaces:** `int pmm_layout_calculate(uint64_t base_va, uint64_t span_pages, struct pmm_layout *out)`；结构字段为 `bits_map_off,bits_length,pages_struct_off,pages_length,zones_struct_off,zones_length,end_of_struct_off,total_bytes`，均 `uint64_t`，offset 相对 base。`total_bytes` 包含终点向 4 KiB 对齐的余量。失败不产生可使用 layout；NULL 返回 `-EINVAL`，其余失败清零 out。

- [ ] 写 `test_m1_layout`：512 MiB/4 GiB、最小一 frame、稀疏 min/max span、未对齐 base、near-UINT64_MAX base、乘法/加法/align overflow。断言每段地址遵循现有 pmm.c 公式，metadata 尾保留 `32*sizeof(unsigned long)`，Page 数按跨度而非 RAM 总和。
- [ ] 注册 native 二进制到 `TEST_BINS`，添加 focused `test-m1-host CASE=layout`；根目标在 x86 profile 转发到 hosttests，显式转发 CASE。随后各任务添加同一组 CASE，省略 CASE 跑完整 M1 组，未知非空 CASE 非零退出。Run `make PROFILE=x86_64-clang test-m1-host CASE=layout`，先确认测试因缺少生产 calculator 编译/链接失败。
- [ ] 实现所有乘加/align 的 checked 运算，保持 LP64 生产布局公式；pmm_init 使用此函数设置指针，检查错误后 fatal。增加 `memory/pmm_boot.c` 到 aarch64 neutral source whitelist。
- [ ] `make clean` 后重复 focused 测试，再运行 `make PROFILE=x86_64-clang test-host`，期望全部 PASS，包括原 PMM reservation 回归。
- [ ] 提交 Task 1 文件，信息 `refactor(mm): share checked PMM boot layout`。

### Task 2: 共用预留策略和单 frame 精确 claim

**Files:** Modify `kernel/memory/{pmm.c,pmm_arch.c}`, `kernel/include/memory/pmm_boot.h`, `hosttests/Makefile`; Create `kernel/include/memory/pmm_arch.h`, `hosttests/test_m1_reservation.c`。

**Interfaces:** `struct pmm_phys_range { uint64_t start, end; };`；`int pmm_arch_boot_reservations(const struct pmm_layout *layout, struct pmm_phys_range *out, size_t capacity, size_t *count)`（声明不带 weak，默认实现 weak）；`int pmm_reserve_boot_ranges(struct Physical_Memory_Manager *pm, const struct pmm_phys_range *ranges, size_t count)`；`struct Page *pmm_claim_free_frame(uint64_t start_pa, uint64_t end_pa, bool from_end)`。

- [ ] 测试 sparse zones、非零最低 PA、arena 在第二 zone、相邻 frame、重复/重叠预留和 invalid ranges。断言 RAM-relative bitmap、free/using/link/refcount 与属性；预留两次结果不变。测试精确 claim 的最低/最高 free frame、空范围、已占用范围、锁内 bitmap 改变，不允许 claim hole。
- [ ] Run `make PROFILE=x86_64-clang test-m1-host CASE=reservation`，确认 RED；测试链接真实 pmm.c 和现有 slab/log mocks，更新 PMM shadow headers，不能用复制的 allocator 代替生产路径。
- [ ] 用策略返回的范围**替换**原 kernel/metadata prefix reservation loop。默认返回 legacy prefix `[0,ceil2M(metadata_end_pa))`，共用 helper 只遍历 represented RAM、跳过 holes；aarch64 后续覆盖只提交 arena。先验证全部范围，再记账；只在 free→reserved 时调用 page_init 和更新 bitmap/zone counters，设置 Kernel/Init/PTable_Mapped。
- [ ] claim 在 pmm_lock 内选择范围内确切空闲 frame，采用 alloc_pages(ZONE_NORMAL,1) 的 bitmap/属性/计数/refcount 约定；不改变通用结构。调用方需要 page_init 时明确配对 page_clean→free_pages；不同时重复维护总 link 数。
- [ ] Run focused 测试与 `make PROFILE=x86_64-clang test-host`，PASS 后提交 `fix(mm): make boot reservations range based`。

### Task 3: aarch64 preflight，先放置 metadata 再初始化 PMM

**Files:** Create `kernel/arch/aarch64/memory/early_arena.c`, `kernel/include/arch/aarch64/early_arena.h`, `hosttests/test_m1_arena.c`; Modify `kernel/arch/aarch64/boot/main.c`, `hosttests/Makefile`。

**Interfaces:** `struct aarch64_m1_arena { uint64_t base_pa,end_pa,table_base_pa,table_end_pa; size_t table_pages; struct pmm_layout layout; };`；纯函数 `int aarch64_m1_plan(const struct MEMORY_RANGE *ram,size_t count,struct aarch64_m1_arena *out)`；`int aarch64_m1_prepare(const struct MEMORY_RANGE *ram,size_t count)`；`const struct aarch64_m1_arena *aarch64_m1_arena_get(void)`。prepare 成功后 state immutable；getter 未准备返回 NULL。强 `pmm_arch_boot_reservations` 返回单个 arena 范围，未 prepare 返回错误。

- [ ] 测试首区间不足而后区间可用、holes、kernel/handoff exclusion、只在高窗口有 RAM、zone>10、未对齐/未排序/重叠、PA≥1TiB、R/D conflict 和 overflow。断言失败前 candidate canary/PMM.start_brk 未变；layout(base=arena high VA) 与纯 sizing 一致。
- [ ] Run `make PROFILE=x86_64-clang test-m1-host CASE=arena`，确认 RED。
- [ ] plan 验证输入，统计 B/D/R 的 unique 512 GiB/1 GiB 桶，用 Task 1 calculator 得到 span metadata 大小，checked 合并并 round2M；选择第一个足够大的连续交集。table_base 为 metadata 之后 round4K，table_end 不超过 arena_end。prepare 所有检查通过后才设置 `PMMngr.start_brk=OFFSET+base_pa`。
- [ ] main 在 RAM map 成功后、pmm_init 前调用 prepare；保留 pmm_init 内 Slab stub。失败打印需求/可用空间并循环 halt。纯 PMM alloc/free smoke 不写数据；M0 descriptor test 继续读旧 TTBR0。data/subpage smoke 延后 Task 6。
- [ ] Run focused 测试；`make PROFILE=aarch64-clang kernel` 构建成功；提交 `feat(aarch64): plan the M1 early arena before PMM`。

### Task 4: 独立 runtime tree 和严格 validator

**Files:** Create `kernel/arch/aarch64/memory/runtime_tree.c`, `kernel/include/arch/aarch64/runtime_tree.h`, `hosttests/test_m1_tree.c`; Modify `hosttests/Makefile`。

**Interfaces:** `struct aarch64_tree_ops { void *ctx; int (*alloc)(void *ctx,uint64_t *pa,uint64_t **va); uint64_t *(*resolve)(void *ctx,uint64_t pa); };`；`struct aarch64_runtime_tree { uint64_t root_pa,table_base_pa,table_used_end_pa; size_t page_count; };`；`int aarch64_runtime_tree_build(const struct MEMORY_RANGE *ram,size_t count,const struct aarch64_m1_arena *arena,const struct aarch64_tree_ops *ops,struct aarch64_runtime_tree *out)`；同参数的 `int aarch64_runtime_tree_validate(..., const struct aarch64_runtime_tree *tree)`（具体完整声明使用 build 前四参数加 tree）。allocator 返回单调连续 arena 表页，resolve 只接受已用池内 PA；host 以独立 fake-PA backing 实现 callbacks。

- [ ] 写独立 walker tests：512 MiB、4 GiB（包含 B/D 后准确计数，不能写死 5 页）、跨 1 GiB、512 GiB、near1TiB、sparse holes、输入超限/conflict、少一页池、非法 table/leaf、越界 PA、循环、重复 intermediate、unexpected leaf。canary 包围全部表页；最大表数用 host heap，不在 kernel 放 256/1027 页静态数组。
- [ ] Run `make PROFILE=x86_64-clang test-m1-host CASE=tree`，确认 RED。
- [ ] 从空页建 L0/L1/L2，只对允许集合写 block；B/D exact override 且 R/D 提前拒绝。最小 table descriptors，无 L3/Contiguous。生产 callbacks 从已映射 arena bump，耗尽 `-ENOMEM`。validator 枚举全部有效项及 expected blocks，证明集合、PA、全部属性、唯一中间页、pool 所有权与 page_count；validator 不能只是复用 builder 的写入遍历。
- [ ] Run focused 测试并 aarch64 kernel build，PASS 后提交 `feat(aarch64): build and validate the strict runtime tree`。

### Task 5: 共同 facade 与真实 x86 错误传播

**Files:** Create `kernel/include/arch/boot_memory.h`, `kernel/arch/x86_64/memory/boot_direct_map.c`, `hosttests/test_m1_contract.c`; Modify `kernel/memory/vmm.c`, `kernel/include/memory/memory.h`, `kernel/arch/x86_64/memory/boot.c`, `hosttests/Makefile`。

**Interfaces:** facade 含 `<stdbool.h>`, `<stddef.h>`, `<memory/memory_map.h>`，声明：

```c
int arch_boot_direct_map_init(void);
bool arch_boot_direct_map_ready(void);
int arch_boot_direct_map_ranges(const struct MEMORY_RANGE **out, size_t *count);
```

`vmm_init` 改为 `int vmm_init(void)`；增加仅供 boot initializer 的 checked intermediate-table allocation 路径，通用 runtime map API 不在本任务改造。

- [ ] contract tests 对两后端同一断言：初始 false；NULL 参数 `-EINVAL` 且无输出写；有效输出先 NULL/0，未 ready `-EAGAIN`；success immutable merged RAM coverage；capacity `-ENOSPC`；OOM `-ENOMEM`；validation `-EIO`；首次失败后仍 false，重复包括失败后 `-EALREADY`，无副作用。本任务先跑 x86 后端，Task 6 加 aarch64 同组案例。
- [ ] 真实 x86 boot mapper 每级 allocation 注入 OOM：返回 `-ENOMEM`，不能构造 NULL descriptor；`kernel_map != NULL` 不作成功判据。测试 `ZONE_UNMAPPED_INDEX=0` 表示**无 cutoff**，非零只包含确实映射的 zone；相邻合并、holes、非法/超容量输入在 ready 前失败。Run `make PROFILE=x86_64-clang test-m1-host CASE=contract-x86` 确认 RED。
- [ ] 初始化 state 使用 UNSTARTED/INITIALIZING/READY/FAILED，首调用立即消费一次性资格。vmm_init 检查 boot mapper 分配和结果，完成既有 TLB maintenance；adapter 在 static coverage buffer 冻结并遍历映射确认后置 ready。保留 Slab、prefix reservation、framebuffer remap 和 AP 顺序；boot.c 替换直接 vmm_init 调用，非零 fatal。不提供 weak/no-op facade。
- [ ] Run focused contract、`make PROFILE=x86_64-clang test-host` 和 `make PROFILE=x86_64-clang test-static`；提交 `feat(mm): expose a checked boot direct-map contract`。

### Task 6: BSP 安装、4 KiB smoke 全路径清理和最终发布

**Files:** Create `kernel/arch/aarch64/memory/{boot_direct_map,m1_selftest}.c`, `kernel/include/arch/aarch64/{boot_direct_map,m1_selftest}.h`, `hosttests/test_m1_install.c`; Modify `kernel/arch/aarch64/{memory/page_table.c,boot/main.c,boot/boot_percpu.c,head.S}`, `kernel/include/arch/aarch64/page_table.h`, `hosttests/Makefile`。

**Interfaces:** `uint64_t aarch64_m1_installed_root(void)`（内部已安装根，非 public ready）；`const struct aarch64_runtime_tree *aarch64_m1_tree_get(void)`（最终成功后）；`uint64_t aarch64_read_ttbr1(void)`；`void aarch64_m1_install_ttbr1(uint64_t root_pa)`；`int aarch64_m1_selftest(uint64_t root_pa)`；`uint64_t aarch64_runtime_root_address(void)` 汇编 literal helper 返回 `.boot.bss` scalar 的低地址。root scalar `runtime_ttbr1_pa` 仅 8-byte aligned，不要求单独 4 KiB page。

- [ ] host 编排以链接替身替代寄存器/屏障、真实 tree/state 逻辑：验证 build→validate→install→selftest→final validate→publish 的事件顺序；每阶段失败均不 ready/root publish；重复失败不重试。把 aarch64 加入 Task 5 contract cases。Run `make PROFILE=x86_64-clang test-m1-host CASE=install` 和 `CASE=contract-aarch64` 确认 RED。
- [ ] 4 KiB primitive tests 两个根都 active，按 VA regime 选择 TTBR0/TTBR1；不得用 ternary 只选 TTBR0。smoke VA=`0xffff800000000000`、L0[256] 开始整个 invalid。对 data allocation 与三个 table allocation 的每个失败位置测试 partial ownership；成功/失败都捕获实际中间页→清 root[256]→完整 TLBI→逐页 free，顺序与恰好一次均断言。不能证明所有权时 fatal 且不释放猜测页。
- [ ] 生产 installer 严格执行 `dsb ishst; msr ttbr1_el1,root; isb; tlbi vmalle1; dsb ish; isb`，保留 TTBR0/DAIF。内部 installed 状态供 smoke，public ready 仍 false。generic unmap 只处理 leaf；独占 slot 清理由 selftest 负责。移除旧 data PA<2GiB 限制，改验证 RAM membership/active block；sync-fault precondition 改查 TTBR1，安排在 cleanup 后。
- [ ] 在现有 `.boot.bss` 放 scalar；C 只能用 literal helper 取得地址再转高 alias 访问，避免直接引用低符号 ADRP。使用现有 linker `.boot.bss`/`.boot.bss.*`，不新增 `>RAM` memory region。最终 tree 验证、coverage 冻结、实际 TTBR1 核对完成后写 root 和 ready；输出一次 `M1 BSP PASS root=<hex> ranges=<decimal>`，失败 `M1 FATAL reason=<token>`，启动调用方循环 halt。
- [ ] selftest 在 install 前用 `AT S1E1R` 预热 M0-only Normal PA=`0x00200000`；install 后 AT+ISB/PAR 要求 fault，并验证 R/D 翻译。每个 zone 精确 claim 首/尾 free frame（同一个只测一次），测试其首/尾 4 KiB sentinel 并配对释放；遍历每个 represented frame 检查映射和 R/zone 集合相等。输出证据格式：每 zone 一次 `M1 ZONE id=<dec> start=<hex> end=<hex> frames=<dec>`，共同 coverage 每项一次 `M1 COVERAGE id=<dec> start=<hex> end=<hex>`（所有镜像在 BSP PASS 前）；selftest 每 zone 一次 `M1 EDGE PASS zone=<dec> first=<hex> last=<hex>`，`M1 WALK PASS frames=<dec>`，`M1 PRUNE PASS pa=0x00200000 before=valid after=fault`，`M1 CLEANUP PASS slot=256`。zone/coverage 来自真实 PMM 和 frozen coverage；edge 地址是实际 claim 的 free frame。输出 `M1 SELFTEST PASS` 仅在全部检查和 slot cleanup 成功后，在 BSP PASS 前。为 1CPU 同样提供窗口外实际读写证据：probe_prepare 成功写入并本核读回 sentinel 后输出 `M1 PROBE PASS pa=<hex>`，位于 BSP PASS 后、CPU_ON 前。
- [ ] Run focused cases、`make clean`、`make PROFILE=aarch64-clang KERNEL_SELFTEST=1 kernel`；检查反汇编 installer/literal helper。提交 `feat(aarch64): install M1 before publishing boot readiness`。

### Task 7: AP pre-MMU 根装载、PoC 和 probe 生命周期

**Files:** Modify `kernel/arch/aarch64/{head.S,boot/boot_percpu.c,smp/smp.c}`, `kernel/include/arch/aarch64/boot_direct_map.h`; Create `hosttests/test_m1_publish.c`; Modify `hosttests/Makefile`。

**Interfaces:** `int aarch64_m1_probe_prepare(void)`、`int aarch64_m1_ap_verify(unsigned int cpu)`、`void aarch64_m1_probe_finish(bool all_requested_acked)`；放 Task 6 的 m1_selftest.c/头。probe PA/expected 使用独立 selftest `.boot.bss` scalars，普通镜像不分配 probe。保持既有 boot per-CPU ABI。

- [ ] host 测试 publication enumerate 所有已用 table pages、root scalar、原 boot metadata、probe scalars 和 sentinel lines；line size 模拟 32/64/128 bytes，全部清理后最后 dsb sy。probe tests：优先≥2GiB；无窗口外 RAM 可 fallback；存在外 RAM但无 free frame 不 fallback；全 ACK 释放一次、timeout/fail 不释放、迟到 AP 仍能读持有 sentinel。Run `make PROFILE=x86_64-clang test-m1-host CASE=publish` 确认 RED。
- [ ] BSP boot 的 `write_tcr_mair_ttbr` 保持 boot roots。`secondary_start` 在设置 SCTLR.M 前读 root scalar，检查非零、4KiB aligned、40-bit PA；失败 secondary_panic，不能 fallback；TTBR0=boot root，TTBR1=runtime root。沿用 pre-MMU invalidate/barriers；检查最终反汇编，不能只在 C 中改根。
- [ ] extend `aarch64_smp_publish_boot()`，通过 CTR_EL0 计算 line size，以**可访问 VA**对实际已用 table range 和全部 scalars/数据执行 dc cvac；保留六页 boot tables 和 slots/mpidrs/count 清理，最后 dsb sy。只在首次 CPU_ON 前一次发布；不能以 release/acquire 或固定 64 字节替代，也不使用不存在的 `__is_libk` 宏。
- [ ] probe 在 M1 ready 后、publication 前 claim/写 sentinel；AP C 入口在 online ACK 前比较实际 TTBR1 与发布值，再读 high alias sentinel。使用已建立 per-CPU/log 路径序列化输出 `M1 AP PASS cpu=<decimal> root=<hex> probe=<hex|none>`，避免 SMP 字符交错；失败无 ACK。root malformed 不要求 pre-MMU C 日志。
- [ ] BSP 所有请求 AP ACK 后释放 probe；DEGRADED/timeout 保留至 boot 结束，生产 probe=none。Run focused tests 和 aarch64 build，提交 `feat(aarch64): publish the runtime root to secondary CPUs`。

### Task 8: RAM 参数、隔离变体和严格验收 parser

**Files:** Modify `qemutests/aarch64_uefi_smp.py`, `mk/{project.mk,profiles/aarch64-clang.mk,targets/aarch64.mk,components/run.mk}`, `kernel/Makefile`, `kernel/arch/aarch64/memory/ram.c`; Create `qemutests/aarch64_m1_{evidence,matrix}.py`。

**Interfaces:** existing harness 新增 `--ram-mib`（default512）与 `--expect-m1`；`qemu_command` 和 `generate_diagnostic_dtb` 都消费同一 RAM 参数。`aarch64_m1_evidence.py` 导出 `m1_evidence_ok(text: str, cpus: int, selftest: bool, ram_mib: int, variant: str = "normal") -> bool` 及 `--self-test`。matrix 接受 `--normal-image`, `--selftest-image`, `--firmware`, `--qemu`, `--log-dir`，调用现有函数/CLI，不能引用不存在的 harness class。

- [ ] parser 自测输入：正确 1/2/4CPU markers；1CPU高RAM却低probe、遗漏/重复/格式错误的每类zone/coverage/edge/walk/prune/cleanup/probe证据；缺/重复/错误顺序 BSP/AP；root 不同；selftest 缺 marker、probe=none、外 RAM probe<2GiB；AP FAIL/DEGRADED；仅 UEFI banner；timeout。要求 BSP PASS 在 AP之前、每个 requested AP 恰一次且 cpu集合正确；cpus=1 不要求 AP。Run `python3 qemutests/aarch64_m1_evidence.py --self-test` 确认 RED。
- [ ] existing `ram_summary_ok` 已检查 pages/bytes 一致且无固定 512MiB 总量，不改成按配置简单相等（固件 exclusions 存在）。新增 parser 验证上述 coverage/zone/edge/walk/prune/cleanup markers，zone集合等于coverage（允许合并），frames=(end-start)/2MiB、walk总数等于zone总数，edge均在对应zone且2MiB对齐，所有必需单例/编号无重复且顺序正确；4GiB 与2GiB selftest 要求 `M1 PROBE PASS` PA≥0x80000000，1CPU也必须有；所有AP probe与BSP probe PA相同。小配置允许窗口内probe；probe marker恰一次，失败/timeout不能成为成功。normal 模式验证 BSP/AP根和既有 SMP/Timer/GIC evidence，不能要求 selftest sentinel。
- [ ] 增加编译旗 `AARCH64_M1_TEST=sparse|arena-exhaust|table-exhaust|ap-bad-root`，只允许 KERNEL_SELFTEST=1，拒绝与 sync-fault/weak-selftest/canary 混用；加入 submake whitelist、kernel compile flags/fingerprint、profile `m1-<case>` 独立 KERNEL_BUILD_DIR 与 UEFI image path。shell 环境变量不传递测试 RAM map。
- [ ] sparse 变体在 RAM map 发布前仅从原本真实 RAM 删除 `[0x50000000,0x50200000)`；先证明洞和两侧 frame 存在，PMM 与 M1 使用同一修改 map，输出 `M1 SPARSE INPUT start=0x50000000 end=0x50200000`（RAM发布前）和 `M1 SPARSE PASS left=0x4fe00000 right=0x50200000 hole=absent`（实际claim邻居读写和walker检查后，在SELFTEST PASS前）；parser要求hole不在zone/coverage且邻居在coverage。arena-exhaust 缩小测试候选 arena capacity 以触发真实 preflight no-space；table-exhaust 降低 bump pool capacity一页以触发真实建表 OOM；不得伪造正常 success。ap-bad-root 在 BSP完成安装验证后、AP发布前将测试 root scalar 置零，预期 pre-MMU拒绝/noACK/DEGRADED，保留 probe。
- [ ] root `test-aarch64` MODE gate 增加 `m1-ram,m1-sparse,m1-arena-exhaust,m1-table-exhaust,m1-ap-bad-root`，沿用独立 prep/run recipe 和已有 firmware/qemu/logdir 参数。定义明确 NORMAL/SELFTEST/M1变体 artifact paths，由 Make variables 传给 parser/matrix。m1-ram prep 分别构建普通和 selftest；failure prep 构建对应隔离变体。不能复用一个被覆盖的 image，也不能把 SUITE 传 test-host 当 focused selector。
- [ ] Run parser selftests、`python3 qemutests/aarch64_uefi_smp.py --self-test`，对每个新 MODE `make -n PROFILE=aarch64-clang test-aarch64 MODE=<mode>` 检查独立prep/run与路径。提交 `test(aarch64): add strict M1 RAM and failure harnesses`。

### Task 9: 双架构集成矩阵与文档闭环

**Files:** Modify `docs/{roadmap.md,arch.md,build/build.md,memory/memory.md}`；必要修复限定 Task 1–8 的文件。

**Interfaces:** Task 8 matrix 默认 normal/selftest 各跑 `(RAM MiB,CPU)={(256,1),(512,1),(512,2),(512,4),(2048,1),(2048,2),(2048,4),(4096,1)}`；每次使用该镜像/内存匹配的诊断 DTB 和独立日志。selftest zone-edge sentinel、全 represented-frame walk、prune PAR 与 AP probe 全部必须 PASS。

- [ ] Run `make PROFILE=x86_64-clang test-m1-host`（不指定 CASE 跑完整组）和 `make PROFILE=x86_64-clang test-host`、`test-static`，全部 PASS。x86 `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` 及独立 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall`，不得夹带 KERNEL_SELFTEST=1。
- [ ] Run `make PROFILE=aarch64-clang test-aarch64 MODE=m1-ram`，要求完整8组×2镜像通过，没有 FATAL/DEGRADED/APFAIL。每个run都有实际root和coverage evidence；不能把编译成功当验收。
- [ ] Run MODE=m1-sparse，验证两侧owned frames实读写、hole tree absent。Run MODE=m1-arena-exhaust 与m1-table-exhaust，要求对应FATAL、没有 BSP PASS/AP online/Timer；MODE=m1-ap-bad-root 要求 BSP PASS后无AP ACK、DEGRADED，不要求 Timer缺失。
- [ ] Run 原 `MODE=smp`、`MODE=gic-spi`、`MODE=sync-fault`；另按现有 no-ACK 目标构建注入镜像并运行 `MODE=no-ack`，它的独立降级预期不能用于正常M1pass。保存命令、profile/flags、镜像路径、日志和结果；失败用 systematic-debugging定位后重跑受影响项。
- [ ] 更新 roadmap 到实际完成状态，文档写明共同接口、x86 coverage cutoff、TTBR0保留、2MiB收紧、低窗口arena容量、Slab仍M2、动态VMM仍M3，以及现有build MODE/flag。本计划编写/评审阶段没有跑上述实施验收，不得提前写PASS。
- [ ] 对实现分支进行独立 whole-branch review，处理发现并重新验证；提交文档闭环 `docs(mm): record verified M1 behavior and limits`。交付测试证据后再按用户选择的方式集成，不自动启动M2。

## 计划自身验收

- spec §4→Task1/2/3，§5→Task4，§6→Task6/7，§7→Task6，§8→Task8/9，§10→Task5/6；没有把未来M2–M4视为当前前置实现。
- 新文件/接口和测试入口必须在其拥有任务中定义；各 focused host 命令使用 native x86 profile，所有 QEMU variants由harness显式选择。
- 子agent评审整个计划与spec/现有代码，修正意见后再次评审，直至明确 APPROVE。批准只表示计划可执行，不表示实现或QEMU验收已完成。
