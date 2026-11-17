# AArch64 内核 LMA 平台配置 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 AArch64 在构建时选择物理启动块，并证明默认和改址镜像都能完成 UEFI 装载、M0/M1 换表与 SMP 启动。

**Architecture:** 链接器、EFI loader、M0 和 M1 消费同一份布局。镜像继续用固定地址 ELF `ET_EXEC`；UEFI 按 `p_paddr` 申请页，内核保留整个启动块并用现有高半区直映关系执行。

**Tech Stack:** GNU Make、Clang/LLD、AArch64 C/汇编、posix-uefi、QEMU virt、现有 Python harness 和 C hosttests。

**Spec:** [2026-11-17-aarch64-kernel-lma-design.md](../specs/2026-11-17-aarch64-kernel-lma-design.md)

## Global Constraints

- AArch64 默认 `B=0x40000000`，验证用 `B=0x42000000`，仅静态验证用 `B=0x0`。B 大小及对齐均为 `0x200000`；入口 `B+0x80000`，handoff `[B+0x1e0000,B+0x200000)`，stub `B+0x1ff000`。
- 保留 `ARCH_PAGE_OFFSET=0xffff000000000000`、`boot_context` v3、M0 的 `[0, 0x80000000)`、D 的 `[0x08000000,0x0a000000)`、6 页静态表空间及 AP/PSCI 协议。候选 arena 在 `[B.end,0x80000000)`，构建期保证窗口至少 20 MiB，启动时仍按实际 R 计算。
- 保留 x86_64 构建行为。动态物理重定位、动态 handoff、缩减 M0 映射、设备发现和真机启动验证不在范围内。
- 在独立实现分支完成 Task 1–5 后才视为可交付改址镜像；中间提交只要求其验证项和默认路径成立。执行前保留 spec 和本 plan；每任务收尾检查 diff 并单独提交。各验证层只覆盖其独有风险；默认、改址、混搭三个 guest 足以验收。新增测试登记与同任务的测试一起提交，不能让目录漂移留到下一个任务。

## Review Focus

1. **切换配置再切回导致 EFI 或磁盘残留旧值**：Task 5 从 FAT 抽取两件产物，再用任务 2 按构建布局校验 loader/ELF 的配对。
2. **向低于 LMA 的页或 handoff 写入**：Task 2 用实际验证器在分配前拒绝，保留合法共享页及贴边结束。
3. **B=0 时选择了错误的 L2 表**：Task 3 检查实际汇编写槽路径，改址 QEMU 再检查实际安装的两张表。
4. **R 在 B 前方被删掉，或旧 B 仍可执行**：Task 4 在低地址 fixture 和生产 M1 验证器中覆盖。
5. **错误产物引发静默超时而被当作正确拒绝**：Task 5 要求专用诊断、通用加载失败和无内核接管证据。

## 文件职责

| 路径 | 职责 |
|---|---|
| `mk/profiles/aarch64-clang.mk`、`mk/project.mk`、`kernel/Makefile`、`kernel/arch/aarch64/{make.config,linker.ld}` | 配置校验与传播，stage1 和最终链接共用实际生成的链接脚本 |
| `kernel/include/arch/aarch64/{boot_layout,handoff_layout}.h`、`kernel/include/arch/mmu.h` | 纯常量布局与 C 接口；AArch64 页偏移只定义一次 |
| `mk/components/{uefi,image}.mk`、`boot/uefi/Makefile`、`boot/uefi/arch/aarch64/{loader.h,elf.c,boot.c,handoff.S}` | EFI 构建和装载事务、handoff 路径 |
| `kernel/arch/aarch64/head.S`、`kernel/arch/aarch64/boot/main.c`、`kernel/arch/aarch64/platform/dtb.c` | M0 写槽与走表证据，实际入口记录和 DTB 验证 |
| `kernel/arch/aarch64/memory/{ram,ram_core,early_arena,runtime_tree,boot_direct_map,page_table_selftest}.c` 及对应公开头 | R 保留、arena、M1 映射与 prune 探针 |
| `qemutests/aarch64_smp_test.py`、`hosttests/cases/test_aarch64_boot_elf.c`、`hosttests/cases/test_aarch64_boot_layout.c`、`hosttests/mk/memory.mk` | ELF/链接/反汇编检查，生产验证器及 RAM/arena/M1 宿主用例 |
| `qemutests/{aarch64_uefi_smp,aarch64_m1_evidence,aarch64_m1_matrix,test_aarch64_harness}.py`、`mk/components/run.mk`、`config/test-scenarios.json` | 正负 QEMU 验收、标准入口和目录登记 |

### Task 1：共享布局和内核构建

**接口：** 默认 `AARCH64_BOOT_BLOCK_BASE=0x40000000`，覆盖值接受单个十进制或 `0x` 十六进制整数，显式空值报错。新 `AARCH64_BUILD_ROOT` 只用于 AArch64 的独立输出根；缺省等于原 profile 的 `BUILD_DIR`，显式值须为绝对路径。在 profile 内派生所有现有输出目录；透传这两个变量，避免直接覆写 kernel 子 Make 的 `BUILD_DIR`。

- [ ] **写红测。** 给 `qemutests/aarch64_smp_test.py` 加 `--layout-only --build-root DIR`，在旧 `check_build_controls()` 之前分流；不要执行其已退役的 no-ack Make 目标，也不要改动旧 `--host-only` 路径：调用顶层 Make，在私有根构建默认和 `0x42000000` 的 `aarch64-uefi-kernel`。读取实际产物的 ELF header、PT_LOAD、`_start` 和 `_kernel_lma_end`；入口分别为 `0x40080000` 和 `0x42080000`。保留既有 AP/栈/boot 表检查。用实际生成的脚本重跑越界 fixture，断言失败原因为 `kernel overlaps UEFI handoff`。fixture 提供满足其它断言的 32 KiB 对齐 `.data.init_task` 和正确的 `_start`，只让 BSS 越界。负配置每类只取一个代表：`0x40001000` 未对齐、`0x08000000` 落在 D、`0x7ec00000` 放不下最小 arena、`0x80000000` 越出 M0；须因配置本身失败。负向测试用 argv，不经 shell 拼接。
- [ ] **确认红。** `python3 qemutests/aarch64_smp_test.py --layout-only --build-root "$(mktemp -d)/build"`；旧路径没有生成正确的改址 ELF 或未拒绝坏配置，不能把工具链缺失当红。
- [ ] **实现。** 布局头导出 `AARCH64_BOOT_BLOCK_{BASE,END}`、`AARCH64_KERNEL_ENTRY`、`AARCH64_HANDOFF_{BASE,END}`、`AARCH64_TRAMPOLINE_BASE`、`AARCH64_BOOT_DEVICE_{BASE,END}`、`AARCH64_BOOT_MAP_END`、`AARCH64_PAGE_OFFSET`；用不带 C 类型/后缀的常量，C facade 的 host override 保持有效。profile 校验数值后将同一 `-D` 加入 AArch64 C、汇编、链接脚本预处理；经 `OS01_SUBMAKE_ALLOWED` 传递配置和构建根。AArch64 生成 `$(KERNEL_BUILD_DIR)/linker.generated.ld`，stage1/final 都依赖它和实际配置变更；x86 用原脚本。链接断言入口、LMA/镜像上界和既有对齐。避免文件名按值复用导致 A→B→A 漏重编，用当前有效值的内容保护戳或现有编译键。
- [ ] **确认绿，提交。** 重跑红测命令；全部布局及边界断言通过，默认构建根未被写入。给该脚本加源内 `@os01-test`（`layer: static`、场景 `boot.lma.configure`），在 `config/test-scenarios.json` 登记该场景的静态 owner 和 selector（本任务的 `--layout-only --build-root` 命令）；运行 `make regen-test-catalog`、`make check-test-catalog` 和 `python3 qemutests/scenario_catalog.py --check`。提交 `build(aarch64): define the boot block layout`。

### Task 2：UEFI 装载、交接和构建失效

**接口：** 保持 `int aarch64_elf_validate(const void *image, uint64_t image_size, uint64_t *entry_out)` 和 `aarch64_page_interval`。内部改为 `noreturn void aarch64_enter_kernel(uint64_t entry, uint64_t context_phys, uint64_t trampoline_phys)`；公共 `arch_enter_kernel` 签名不变。

- [ ] **写红测。** `test_aarch64_boot_elf.c` 用同一 fixture 分别链接默认和替代布局编译的生产 `elf.c`。用一份合法 ELF 同时覆盖入口、最后一页贴住 handoff 和共享分配页；默认只跑合法 ELF，替代布局再用代表反例覆盖入口不匹配、3 字节文件入口、无 PF_X、低于 LMA、高半区 VMA/PA 不符、`p_memsz` 越界。保留生产溢出检查。只对实际入口不匹配要求专门错误类别。用两个不同路径的生产对象，替代对象用编译期重命名导出的 `aarch64_elf_validate` 和 `aarch64_page_interval`，一并链接进一个目录登记的 `test_aarch64_boot_elf.elf`。宿主 fixture 使用两个调用入口和独立的预期地址；`test-aarch64-boot-elf` 运行这一个 ELF。
- [ ] **确认红。** `make -C hosttests OS01_PROFILE_FILE="$PWD/mk/profiles/aarch64-clang.mk" PROFILE=aarch64-clang DRIVER_MODEL_FAULT=none test-aarch64-boot-elf`；旧校验接受某项非法输入或无法接受有效改址 ELF。
- [ ] **实现。** `elf.c` 在分配前验证唯一 identity 启动段、完整 4 字节入口和每个高段的直映关系，校验算术、页界和段重叠；`boot.c` 保留申请、复制、补零、清理、缓存顺序，专门打印 `UEFI-A64: kernel entry mismatch`，只给入口地址不等的错误。handoff 初始化、清理、DTB 和 memmap 缓冲区用派生界限，stub 用 `br x2` 进入，保持内核接收 `x0=context_phys`。
- [ ] **接通 EFI 构建。** 根 `uefi.mk` 和 wrapper 的 EFI 依赖包括共享头、`loader.h` 和当前布局键；把 `-D` 随环境 CFLAGS 送入 posix-uefi 应用编译，保留 COFF flags。布局键或这些头变化时至少强制重建受影响的应用对象及 EFI，不能只更新外层 stamp 后复制旧 EFI。使用现有内容保护发布；不要改第三方源码。确认 FAT 目标依赖当前两件产物。
- [ ] **确认绿，提交。** 登记源内 `@os01-test` 和宿主场景 `boot.lma.validate_elf`，依次运行 `make regen-test-catalog`、`make check-test-catalog` 及 `python3 qemutests/scenario_catalog.py --check`；重跑宿主目标，随后在临时输出根构建默认 EFI（`make PROFILE=aarch64-clang AARCH64_BUILD_ROOT="$root" "$root/artifacts/uefi/BOOTAA64.EFI"`，`root` 取绝对临时路径），确认产物为 AArch64 PE/COFF；FAT 检查留给 Task 5。提交 `feat(aarch64): validate and load the configured UEFI image`。

### Task 3：M0 选槽和内核入口证据

**接口：** `head.S` 在唯一 B 写槽指令序列前定义链接可见的本地符号 `boot_write_exec_block`；提供 `uint64_t aarch64_boot_observed_entry(void)`（仅自测消费），返回 _start 低地址代码实际用 PC 相对指令取得并保存的地址。

- [ ] **写红测。** 扩展 Task 1 的 `--layout-only`，加入 B=0 构建，断言 ELF 入口 `0x80000`。只反汇编这一份实际完成链接的 `head.S` 路径：确认 L1[0] 挂到 `boot_page_tables+0x2000`，`boot_write_exec_block` 调用 `write_slot` 时选择该表和槽 0。默认和替代槽位由 Task 5 的 live 自测验证。失败须指出哪一条写槽路径缺失或错误。
- [ ] **确认红。** 重跑 Task 1 命令；成功链接也必须因旧写槽路径失败。
- [ ] **实现。** 先填普通 Normal NX 与 D Device NX，最后令 `boot_write_exec_block` 用基址编译时选择 L2 表及槽，写唯一 Normal UXN、非 PXN 的 B；不动 D、TTBR、MAIR 或 PSCI 协议。自测在原地遍历实际两张 L2 表，检查 1024 个完整描述符以及现有 bit-40 污染负例。BSS 清零后、开 MMU 前从低入口路径记录 `adr ..., _start`；`arch_early_init` 自测用保存值和传入的 `bootctx` 指针检查期望值，再 `log_info` 输出 `UEFI-A64: layout entry=<hex> context=<hex>`。不要把配置常量打印为实测值。
- [ ] **确认绿，提交。** 重跑静态审计；默认和改址的实际页表及 AP 验证合并到 Task 5。提交 `feat(aarch64): select the executable M0 block from layout`。

### Task 4：M1、RAM/arena 所有权和 prune

**接口：** `aarch64_early_arena_plan`、`aarch64_runtime_tree_build/validate` 现有签名保持；在 `ram_core.h/.c` 新增 `int aarch64_ram_normalize_boot(const uint8_t *bytes, uint32_t entry_count, uint32_t entry_size, uint32_t format, uint32_t descriptor_version, struct aarch64_ram_map *out)`，由 `ram.c` 和宿主测试共同调用。它用共享布局的整个 B 调用现有纯 normalizer；通用带排除列表的接口保持。`aarch64_page_table_prune_warm(const struct MEMORY_RANGE *ram, size_t count)` 消费将被安装的同一组 R，调用点在 `boot_direct_map.c` 换 TTBR1 前。

- [ ] **写红测。** `test_aarch64_boot_layout.c` 用 B=`0x02000000` 的有区别对象编译生产 `early_arena.c`、`runtime_tree.c`、`ram_core.c`（复用 `test_page_table_tree.c` 的 host 后端写法）。R 有 `[0x01000000,0x02000000)`、`[0x02200000,0x06200000)` 和含旧 B 的 `[0x40000000,0x44000000)`；断言 arena 从 `0x02200000` 开始，M1 建表/验证成功，新 B 是 Normal 可执行、低 R 和旧 B 为普通 NX；传入覆盖 B 的 R 必须失败。对跨越 B 的单段 EFI RAM 描述符调用 `aarch64_ram_normalize_boot`，断言整个 B 被排除且两侧整块仍保留；测试不自行传入排除区。测试案以 `test-aarch64-boot-layout` 在 hosttests 运行。
- [ ] **确认红。** 用 Task 2 同样的 hosttests 命令改目标为 `test-aarch64-boot-layout`；旧 arena/树会误用默认 B 或返回失败。
- [ ] **实现。** `ram.c` 改用 `aarch64_ram_normalize_boot`，并确认仍以同一产出向 PMM 发布 R；`dtb.c` 与 `ram.c` 用共享 handoff 界限校验 DTB 与描述符；`early_arena.h/.c` 用 B.end 和 M0 末端做窗口并更新诊断；`runtime_tree.c` 在输入验证、建表、期望迭代器均用共享 B/D，不假定 PA 顺序。TTBR1 安装/发布和保留 TTBR0 不改。
- [ ] **实现 prune 与证据。** `page_table_selftest.c` 优先选 `0x00200000`，否则从 M0 按 PA 找到首个不属于 B/D/R 的块；用独立有效标志记录候选（PA 0 合法）。切换前验证有效、切换后验证同址 fault；无候选仅记录 `M1 PRUNE SKIP reason=no-candidate` 并继续其它自测。harness 从日志解析 PA，校验对齐、范围及与 B/D/R 不相交；默认和替代 QEMU 要求 PASS。把 arena 故障白名单中的下界取自测试实际布局并保留精确匹配，不能放宽到忽略任意 FATAL。
- [ ] **确认绿，提交。** 登记源内 `@os01-test` 和宿主场景 `boot.lma.reserve_ram`，并重复 Task 2 的生成与校验命令。跑宿主新目标，并使用与红测相同的 hosttests 调用前缀（含 `DRIVER_MODEL_FAULT=none`）运行现有组件目标 `test-early-arena test-page-table-tree`。已有宿主 arena 用例覆盖空间不足，无需另起 guest 验证同类失败。提交 `feat(aarch64): carry the selected boot block through M1`。

### Task 5：配对启动、混搭拒绝和增量隔离

**接口：** `make PROFILE=aarch64-clang test-aarch64 MODE=lma-layout` 调度新增 `qemutests/aarch64_lma_layout.py`；复用 `aarch64_uefi_smp.py` 的 ProcessSession、QEMU 参数及归档。为直接 runner 新增互斥 `--expect-lma-base BASE`（正向，启用现有 M1/SMP 证据）和 `--expect-lma-mismatch`（负向，独立接受条件），旧模式语义不变。

- [ ] **写红测。** 在 `test_aarch64_harness.py` 加一份正向和一份负向日志 fixture；正向各做一次地址错误和 R 与 B 重叠的突变，负向在缺少专用诊断或出现内核接管时失败。AP/IPI、超时和归档复用已有测试。以独立日志目录保存正负 case，不改默认 FAT。
- [ ] **确认红。** `python3 -m unittest qemutests.test_aarch64_harness`，新证据断言因缺少模式或校验而失败。
- [ ] **实现正负验证。** 使用独立于常规默认构建树的临时根，固定 `KERNEL_SELFTEST=1`，按默认→改址→默认跑 `aarch64-uefi`，每轮消费 `image/selftest/aarch64-uefi.img` 和同轮的 `artifacts/kernel.elf`、`artifacts/uefi/BOOTAA64.EFI`，每轮用 `mcopy -i` 抽出 `::/kernel.elf`、`::/EFI/BOOT/BOOTAA64.EFI`，与该轮发布产物逐字节比较；ELF 逐轮断言真实入口，EFI 改址须与默认不同。保存改址轮和恢复默认轮的 FAT；各启动一次，以运行结果证明两份镜像的实际配对。首次默认不额外启动，也不比较两次默认 FAT 或 EFI 的重建哈希。比对常规默认构建树镜像的前后摘要；原来不存在的路径不得被创建。QEMU virt、2 CPU、512 MiB；正向以实际入口和 context（替代 `0x42080000/0x421e0000`）、boot-map OK、M1/PRUNE/AP/IPI 通过为条件，同时解析所有 M1 ZONE 验证不包含 B。以恢复后的默认 FAT 副本换入替代 ELF，只在观察到专用 mismatch 和 `UEFI: kernel load failed`、且无内核接管证据时通过，保留短观测窗口、进程回收与超时分类。
- [ ] **接入并验证。** 在 `mk/components/run.mk` 登记 `lma-layout` mode，构建及启动都由专用 runner 管理私有根；在 runner 源内加 `@os01-test`（`id: aarch64-lma-layout`、`layer: qemu_e2e`、场景 `boot.lma.uefi`）；在 `config/test-scenarios.json` 登记该 guest 场景和 selector `make PROFILE=aarch64-clang test-aarch64 MODE=lma-layout`。用已有 schema；不新增 legacy bridge 条目或引入新的 `test-qemu SUITE` 路由。运行：

  ```bash
  make regen-test-catalog
  make check-test-catalog check-ci-schedule
  python3 qemutests/scenario_catalog.py --check
  python3 -m unittest qemutests.test_aarch64_harness
  make PROFILE=aarch64-clang test-aarch64 MODE=lma-layout
  git diff --check
  ```

  预期目录检查和 parser tests 通过，归档各含一次默认配对成功、改址配对成功和混搭明确拒绝。出现回归迹象或 CI 要求时再运行完整的 `make PROFILE=aarch64-clang test-aarch64 MODE=smp` 和 `MODE=m1-ram` 矩阵。记录执行过的产物路径及日志；若因环境无法运行，不将 build/no-op 或 timeout 算作通过。
- [ ] **收尾。** 验收通过才更新路线图本条状态和 `docs/build/build.md` 的 AArch64 构建入口，注明只验证改址与 QEMU，不宣称真机已支持。提交 `test(aarch64): verify relocated UEFI boot layout`。
