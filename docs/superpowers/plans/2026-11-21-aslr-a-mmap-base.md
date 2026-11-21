# ASLR-A：x86_64 用户态 mmap 基址随机化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 x86_64 用户态每个新建地址空间（`spawn_user_task`、`sys_exec`）使用 STRONG-only 内核熵源独立随机化 `mm->mmap_base`，保持 `fork` 继承现有布局与 `MAP_FIXED` 语义，并加固 `mmap`/`munmap`/`mprotect` 的地址窗口、冲突规避与溢出处理。

**Architecture:** 在 x86 私有头 `user_layout.h` 中单源定义用户规范上界与 28-bit 随机窗口 `[0x0000400000000000, 0x0000410000000000)`；新增无 PMM/VMM 依赖的 `kernel/arch/x86_64/memory/aslr.c` 提供纯派生与 fail-closed 初始化接口；在 `exec.c` 的 staged 建像阶段调用并保持 `mm_alloc()` 双架构确定性与 `fork.c` 继承行为；在 `mmap.c` 统一有效上界与无回绕 first-fit 间隙搜索。

**Tech Stack:** freestanding C（x86_64/Clang）、GNU Make、hosttests（`@os01-test` + `scan_tests.py` + `scenario_catalog.py`）、`kernel/selftest`、`user/systest`、QEMU E2E harness。

**Spec:** [ASLR-A：x86_64 用户态 mmap 基址随机化设计](../specs/2026-11-21-aslr-a-mmap-base-design.md)

---

## 全局约束

- **仅限 x86_64 用户态**：不修改 `kernel/memory/mm.c`（`mm_alloc()` 维持双架构共享默认 `0x40000000`）、`kernel/init/userspace.c`（`init_mm.mmap_base = 0`）与任何 aarch64 源码；不改动 `mm_t` 结构体布局；不引入 `ET_DYN`/PIE、栈/堆随机化或运行时开关。
- **固定几何常量**（单源在 `kernel/include/arch/x86_64/user_layout.h`）：`X86_USER_ADDR_LIMIT = 0x00007FFFFFFFFFFFULL`、`X86_USER_MMAP_MAX_ADDR = 0x00007FFFFFFFF000ULL`、`X86_USER_MMAP_RND_BASE = 0x0000400000000000ULL`、`X86_USER_MMAP_RND_BITS = 28U`、`X86_USER_MMAP_RND_SPAN = (1ULL << (X86_USER_MMAP_RND_BITS + 12U))`、`X86_USER_MMAP_RND_END = (X86_USER_MMAP_RND_BASE + X86_USER_MMAP_RND_SPAN)`。
- **STRONG-only fail-closed**：`x86_aslr_init_mmap_base()` 必须独立调用 `kernel_random_get_strong(entropy)`（严禁复用 `AT_RANDOM`），在成功和失败路径均以 `volatile uint8_t *` 显式擦除 32 字节栈缓冲；熵不可用返回 `-EAGAIN` 且不修改 `mm->mmap_base`，禁止回退 `0x40000000` 或降级 WEAK/周期计数器。
- **既有源码审计锚点**：保留 `mmap.c` 中 `test_user_protected_ranges.c:869-896` 依赖的 `if (_dev_mmap) {` 与 `if (!(flags & MAP_FIXED)) {` 锚点及块内 `mm_user_range_protected(`，保留 `exec.c` 中 `test_process_image_lifecycle.c` 断言的 `elf_load < mm_init_user_heap < alloc_pages < setup_user_stack < task_list_lock / old_mm = current->mm` 偏序。
- **测试登记与分层隔离**：新增宿主测试必须带 `@os01-test` 元数据、同步更新 `config/test-scenarios.json` 并运行 `make regen-test-catalog`；`OS01_SYSTEST=1` 与 `KERNEL_SELFTEST=1` 必须分开构建运行；日志遵循 `log_*` 规范。

## 评审重点

1. **前方空隙不足的 VMA 推进**：`mmap_find_gap_locked()` 遇到 `v->vm_start > cand` 但 `v->vm_start - cand < length` 时必须无条件推进 `cand = v->vm_end`，绝不放入 `else` 导致与 `v` 重叠（任务 2：`test_x86_aslr_mmap`）。
2. **异常低值 `mm->mmap_base` 遇栈顶已有 VMA**：`mm->mmap_base == 0` 或落入保护区且 `X86_USER_STACK_END` 已有 VMA 时，必须钳制搜索起点到 `X86_USER_STACK_END` 后继续扫描跳过该 VMA，不得盲覆写导致重叠（任务 2：`test_x86_aslr_mmap`）。
3. **`MAP_FIXED(0)` 零页漏洞与顶保护页 / 非规范地址拒绝**：`MAP_FIXED` 传 `addr == 0` 返 `-EINVAL`；即使 `current->addr_limit` 为 `0xffff800000000000` 或 `UINT64_MAX`，`do_mmap` 对 `0x00007FFFFFFFF000`/`0x0000800000000000` 返 `-ENOMEM`，`do_munmap`/`do_mprotect` 返 `-EINVAL`，且零副作用（任务 2、4）。
4. **双阶段 STRONG 熵失败回滚**：`spawn_user_task` / `sys_exec` 在第 1 次取熵（`x86_aslr_init_mmap_base`）或第 2 次取熵（`setup_user_stack`）任一失败时均返 `-EAGAIN`，`spawn` 零发布且账本归零，`sys_exec` 不调 `arch_switch_mm` 且原 `current->mm`（含原 `mmap_base`）完整保留（任务 3：`test_x86_aslr_exec`）。
5. **非固定窗口耗尽不回绕与设备引用平衡**：`[mm->mmap_base, limit)` 空间不足时普通与设备非固定 `mmap` 均返 `-ENOMEM` 且永不回绕到 `mmap_base` 以下，设备路径平衡调用 `vfs_node_put`/`files_put_file` 且不触发 `_dev_mmap` 回调（任务 2、4）。

## 文件与职责

| 任务 | 新建 / 修改文件 | 职责 |
|---|---|---|
| 1 | 修改 `kernel/include/arch/x86_64/user_layout.h`、新建 `kernel/include/arch/x86_64/aslr.h` 与 `kernel/arch/x86_64/memory/aslr.c`、修改 `qemutests/mm_header_boundary.py`、`mk/components/run.mk`、`hosttests/cases/test_vmm_caller_audit.c`、新建 `hosttests/cases/test_x86_aslr_mmap.c`、修改 `hosttests/mk/memory.mk`、`config/test-scenarios.json`、生成目录产物 | 单源 VA 布局宏、纯派生与 STRONG fail-closed 初始化器、编译期几何断言、双架构头文件与 ELF 符号隔离审计、ASLR 核心宿主单测 |
| 2 | 修改 `kernel/arch/x86_64/memory/mmap.c`、`hosttests/cases/test_x86_aslr_mmap.c`、`hosttests/mk/memory.mk` | 统一 `mmap_effective_limit()`、重写 `mmap_find_gap_locked()`、封堵 `MAP_FIXED(0)`、清理旧重复上界检查、统一 `do_munmap`/`do_mprotect` 上界、稀疏 PTE 宿主全边界验证 |
| 3 | 修改 `kernel/arch/x86_64/sched/exec.c`、`kernel/arch/x86_64/sched/fork.c`、新建 `hosttests/mock/aslr_exec/arch/x86_64/mmu.h` 与 `hosttests/cases/test_x86_aslr_exec.c`、修改 `hosttests/mk/sched.mk`、`hosttests/mk/fs.mk`、`hosttests/cases/test_boot_spawn_failure.c`、`hosttests/cases/test_boot_services_vfs.c`、`hosttests/cases/test_boot_services_spawn.c`、`config/test-scenarios.json`、生成目录产物 | `spawn_user_task`/`sys_exec` 接入随机化与 `X86_USER_ADDR_LIMIT`、`fork.c` 继承与常量统一、独立非特权 `mmu.h` 宿主测试 `spawn`/`exec` 双取熵与 fail-closed 回滚、对齐既有 boot3 宿主测试桩 |
| 4 | 新建 `kernel/selftest/test_aslr_mmap.c`、修改 `kernel/selftest/selftest.c`、`user/systest/systest.h`、`user/systest/systest.c`、`user/systest/test_mem.c`、`config/test-scenarios.json` | x86_64 内核自测（STRONG/WEAK/NONE 三档真实 `kernel_random_get_strong`）、`systest` 自重入 `aslr-probe` 与 3 组用户态 E2E 用例、场景目录完整登记 |
| 验收 | 修改 `docs/memory/cow-mmap.md`、`docs/roadmap.md`、`docs/changelog.md` | 全层门禁实跑归档与文档回填 |

---

## 任务 1 — 布局常量、x86 ASLR 核心模块与边界审计

**Files:**
- Modify: `kernel/include/arch/x86_64/user_layout.h:1-26`
- Create: `kernel/include/arch/x86_64/aslr.h`
- Create: `kernel/arch/x86_64/memory/aslr.c`
- Modify: `qemutests/mm_header_boundary.py:43-78`
- Modify: `mk/components/run.mk:170`
- Modify: `hosttests/cases/test_vmm_caller_audit.c:346-353`
- Create: `hosttests/cases/test_x86_aslr_mmap.c`
- Modify: `hosttests/mk/memory.mk`
- Modify: `config/test-scenarios.json`

**Interfaces:**
- Consumes: `bool kernel_random_get_strong(uint8_t out[32])`（`<random/random.h>`）、`mm_t`（`<memory/mm.h>` / `<sched/task.h>`）
- Produces:
  - `kernel/include/arch/x86_64/user_layout.h` 宏：`X86_USER_ADDR_LIMIT`、`X86_USER_MMAP_MAX_ADDR`、`X86_USER_MMAP_RND_BASE`、`X86_USER_MMAP_RND_BITS`、`X86_USER_MMAP_RND_SPAN`、`X86_USER_MMAP_RND_END`
  - `kernel/include/arch/x86_64/aslr.h`：
    - `uint64_t x86_aslr_derive_mmap_base(uint64_t raw_entropy);`
    - `int x86_aslr_init_mmap_base(mm_t *mm);`

- [ ] **Step 1: 编写失败的宿主测试 `hosttests/cases/test_x86_aslr_mmap.c`（Part 1）并注册构建规则**
  在文件头写 `@os01-test` 元数据（`id: test_x86_aslr_mmap`、`layer: host`、`subsystem: memory`、`timing: strict`、`tier: pr`、`isolation: none`、`scenarios: [memory.aslr.x86_mmap]`），使用 `test_framework.h`（`TEST_FUNC` / `TEST_LIST_BEGIN` / `RUN_ALL_TESTS()` / `TEST_RESULTS()`）。提供可控 `bool kernel_random_get_strong(uint8_t out[32])` 桩（记录调用次数、按队列/开关返回预设 32B 缓冲或 `false`）。编写以下断言用例：
  - `test_derive_boundaries_and_mask`：断言 `x86_aslr_derive_mmap_base(0) == 0x0000400000000000ULL`、`derive(1) == 0x0000400000001000ULL`、`derive((1ULL << 28) - 1) == 0x000040fffffff000ULL`、`derive(1ULL << 28) == 0x0000400000000000ULL`（高位掩码剥离）、`derive(UINT64_MAX) == 0x000040fffffff000ULL`，且所有返回值 4 KiB 对齐并落在 `[X86_USER_MMAP_RND_BASE, X86_USER_MMAP_RND_END)` 内。
  - `test_init_null_mm_rejected`：`x86_aslr_init_mmap_base(NULL) == -EINVAL` 且 `kernel_random_get_strong` 调用次数为 `0`。
  - `test_init_strong_success_little_endian`：喂入 32 字节样本（前 8 字节小端为 `0xdeadbeef01234567ULL`），断言返回 `0`、`mm.mmap_base == x86_aslr_derive_mmap_base(0xdeadbeef01234567ULL)`、调用次数为 `1`；再次喂入不同低 28 位字节断言得到不同基址。
  - `test_init_fail_closed_preserves_base`：置 `mm.mmap_base = 0x40000000ULL`（哨兵），桩写入非零脏字节后返回 `false`（模拟 WEAK 硬件路径写脏缓冲后拒收），断言返回 `-EAGAIN`、`mm.mmap_base == 0x40000000ULL` 不变。
  在 `hosttests/mk/memory.mk` 定义独立 `ASLR_MMAP_HOST_CFLAGS`（与 `LIFECYCLE_HOST_CFLAGS` 一致，含 `-include $(TEST_MOCK)/lifecycle/lifecycle_runtime.h`，不依赖 `sched.mk` 包含顺序），新增 `$(TEST_BLD)/aslr_mmap_aslr_production.o`（用 `$(ASLR_MMAP_HOST_CFLAGS)` 编自 `kernel/arch/x86_64/memory/aslr.c`）、`$(TEST_BLD)/test_x86_aslr_mmap.o`（用 `$(ASLR_MMAP_HOST_CFLAGS) $(FRAMEWORK_INC)` 编译，保持 `lifecycle_runtime.h` 先于 `test_platform.h`）与 `$(TEST_BLD)/test_x86_aslr_mmap.elf`（任务 1 即直接链接 `$(TEST_BLD)/lifecycle_stubs.o`，消除 `-include lifecycle_runtime.h` 下的悬空符号风险）。
  在 `config/test-scenarios.json` 按 schema v2 补齐全部必填字段新增场景 `id: "memory.aslr.x86_mmap"`（`owner: "host"`、`component: "memory"`、`behavior: "aslr"`、`trigger`、`assertions`、`failure_modes`、`primary: { layer: "host", source: "hosttests/cases/test_x86_aslr_mmap.c", selector: "test_x86_aslr_mmap", evidence: "ALL_TESTS_PASSED_OR_EXIT_0" }`、`supporting: []`、`overlap_decision: { action: "unique", reason: "..." }`）。

- [ ] **Step 2: 运行目录生成并确认 RED**
  运行：
  ```bash
  make regen-test-catalog
  make PROFILE=x86_64-clang test-host
  ```
  预期：构建 `test_x86_aslr_mmap.elf` 阶段编译失败（缺 `<arch/x86_64/aslr.h>` 与 `kernel/arch/x86_64/memory/aslr.c`）。

- [ ] **Step 3: 实现 `user_layout.h`、`aslr.h`、`aslr.c` 并接入边界审计**
  - 在 `kernel/include/arch/x86_64/user_layout.h` 追加 Spec §3.1 的 6 个 `X86_USER_*` 常量宏（保持零 `#include`，仅用 `ULL`/`U` 字面量）。
  - 创建 `kernel/include/arch/x86_64/aslr.h`，声明 `x86_aslr_derive_mmap_base` 与 `x86_aslr_init_mmap_base`（include guard `_ARCH_X86_64_ASLR_H`）。
  - 创建 `kernel/arch/x86_64/memory/aslr.c`：
    - 放入 Spec §3.3 的 8 条 `_Static_assert` 几何不变式；
    - 实现 `x86_aslr_derive_mmap_base(uint64_t raw_entropy)`；
    - 实现 `static void aslr_wipe_bytes(uint8_t *buf, size_t len)`（`volatile uint8_t *p = buf; for (...) p[i] = 0;`）；
    - 实现 `x86_aslr_init_mmap_base(mm_t *mm)`：检查 `!mm → -EINVAL`；调用 `kernel_random_get_strong(entropy)`；失败分支擦除 `entropy[32]` 并返 `-EAGAIN`；成功分支以小端移位组装 `raw`、立即擦除 `entropy[32]`、写 `mm->mmap_base = x86_aslr_derive_mmap_base(raw)` 并返 `0`。
  - 在 `qemutests/mm_header_boundary.py` 的 `FORBIDDEN_MACROS` 追加 `_ARCH_X86_64_ASLR_H` 与 6 个新增 `X86_USER_*` 宏，在 `FORBIDDEN_SYMBOLS` 追加 `x86_aslr_derive_mmap_base`、`x86_aslr_init_mmap_base`。
  - 在 `mk/components/run.mk:170` 的 `grep -E` 禁止符号正则追加 `x86_aslr_`。
  - 在 `hosttests/cases/test_vmm_caller_audit.c:346-353` 追加 `assert_false(contains(branch, "arch/x86_64/memory/aslr.c"));`。

- [ ] **Step 4: 验证 GREEN 与双架构隔离审计**
  运行：
  ```bash
  make PROFILE=x86_64-clang test-host
  make PROFILE=x86_64-clang test-static CASE=mm-header-boundary
  make PROFILE=aarch64-clang test-static CASE=mm-header-boundary
  make PROFILE=aarch64-clang test-aarch64-audit
  python3 qemutests/scenario_catalog.py --check
  make check-test-catalog
  make test-harness
  ```
  预期：全部退出 `0`，`test_x86_aslr_mmap` 与 `test_vmm_caller_audit` 全部断言 PASS，双架构 `mm-header-boundary` 与 `test-aarch64-audit` PASS，目录零漂移且 `test_scenario_catalog` / `test_scenario_owner` 均 PASS。

- [ ] **Step 5: 提交任务 1**
  ```bash
  git add kernel/include/arch/x86_64/user_layout.h kernel/include/arch/x86_64/aslr.h \
          kernel/arch/x86_64/memory/aslr.c qemutests/mm_header_boundary.py \
          mk/components/run.mk hosttests/cases/test_vmm_caller_audit.c \
          hosttests/cases/test_x86_aslr_mmap.c hosttests/mk/memory.mk \
          config/test-scenarios.json config/test-catalog.json mk/generated/
  git commit -m "feat(x86_64/mm): add mmap ASLR base layout, derivation and boundary audits"
  ```

---

## 任务 2 — 加固 `mmap` / `munmap` / `mprotect` 窗口、冲突与溢出契约

**Files:**
- Modify: `kernel/arch/x86_64/memory/mmap.c:288-688,739-765`
- Modify: `hosttests/cases/test_x86_aslr_mmap.c`
- Modify: `hosttests/mk/memory.mk`

**Interfaces:**
- Consumes: 任务 1 的 `X86_USER_MMAP_MAX_ADDR`、`x86_aslr_derive_mmap_base`、`x86_aslr_init_mmap_base`；既有 `mm_user_range_protected()`、`vm_area_insert_sorted()`
- Produces:
  - `kernel/arch/x86_64/memory/mmap.c` 内部契约：`mmap_effective_limit(uint64_t addr_limit)`、`mmap_find_gap_locked(mm_t *mm, uint64_t length, uint64_t upper_limit, uint64_t *out_addr)`
  - 加固后的 `do_mmap()`、`do_munmap()`、`do_mprotect()` 行为（签名与既有 `<arch/x86_64/vma.h>` 完全一致）

- [ ] **Step 1: 扩展 `hosttests/cases/test_x86_aslr_mmap.c`（Part 2）并链接 `lifecycle` 稀疏 PTE 桩**
  在 `hosttests/mk/memory.mk` 让 `$(TEST_BLD)/test_x86_aslr_mmap.elf` 追加联编生产 `mmap_lifecycle_production.o`、`brk_lifecycle_production.o`、`vma_production.o`、`vm_area_lifecycle_production.o`、`mm_production.o`（任务 1 已链接 `lifecycle_stubs.o`；无需 `--wrap=kmalloc`，`lifecycle_stubs.o` 直接提供 `kmalloc`/`kfree` 及稀疏 32 槽 64 位 VA PTE 登记表，注意每个用例的常驻页槽总数须保持 `< LIFECYCLE_PTE_SLOTS` 即 32）。
  在 `test_x86_aslr_mmap.c` 提供 `reset_mm_fixture()` 辅助（每个用例及多子场景切换前调用）：① `lifecycle_stubs_reset()`；② 清零 `fixture_mm`，调用 `list_init(&fixture_mm.vma_list)`、`spin_init(&fixture_mm.lock)`，设置非空 `fixture_mm.pgdir`（指向静态 `dummy_pgd` 数组）、`fixture_mm.start_code = X86_USER_MIN_ADDR`、`fixture_mm.end_code = 0x500000` 并调 `mm_init_user_heap(&fixture_mm, fixture_mm.end_code)`；③ 显式绑定 `current->mm = &fixture_mm`、`current->addr_limit = X86_USER_ADDR_LIMIT`、`current->files = lifecycle_files_table()`。每个用例或子场景结束时调用 `vma_free_all(&fixture_mm)` 清理残留 VMA。新增以下测试：
  - `test_mmap_high_randomized_base_sequential`：用 `x86_aslr_init_mmap_base` 置 64 TiB 区间基址 `B`，断言连续两次匿名 `do_mmap(0, 0x1000, ...)` 分别返 `B` 与 `B + 0x1000`。
  - `test_mmap_gap_conflicts_and_insufficient_front_gap`（钉死评审重点 1；4 个子场景之间均显式 `vma_free_all(&fixture_mm)` + `reset_mm_fixture()` 重置夹具，避免前序 VMA 污染）：
    - `MAP_FIXED` 置 `[B, B + 0x2000)` → 非固定 `0x1000` 返 `B + 0x2000`；
    - `MAP_FIXED` 跨骑 `[B - 0x1000, B + 0x1000)` → 非固定 `0x1000` 返 `B + 0x1000`；
    - `MAP_FIXED` 置 `[B + 0x1000, B + 0x3000)`（前方仅 1 页空隙）：非固定 `0x2000` 必须跳过该 VMA 返 `B + 0x3000`（且 `vma_list` 中无重叠）；随后非固定 `0x1000` 恰好填入前方精确空隙返 `B`；
    - `MAP_FIXED` 置低址 `[0x40000000, 0x40001000)` → 不影响高址 `B` 的首次非固定分配。
  - `test_mmap_fixed_zero_and_canonical_ceiling`（钉死评审重点 3）：
    - `do_mmap(0, 0x1000, PROT_RW, MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED, -1, 0) == -EINVAL`（封堵零页）；
    - `MAP_FIXED` 于 `0x00007FFFFFFFE000`（1 页）成功并可 `do_mprotect(..., PROT_READ) == 0`、`do_munmap(...) == 0`；
    - 分别在 `addr_limit = X86_USER_ADDR_LIMIT`、`0xffff800000000000ULL`、`UINT64_MAX` 下，断言 `MAP_FIXED` 于 `0x00007FFFFFFFF000`、`0x00007FFFFFFFE000` 跨 2 页、`0x0000800000000000` 均返 `-ENOMEM`，且 `do_munmap` / `do_mprotect` 对 `0x00007FFFFFFFF000` 与 `0x0000800000000000` 均返 `-EINVAL` 且 VMA/PTE/flush 计数零变更。
  - `test_mmap_exhaustion_no_wrap_and_overflows`（钉死评审重点 5）：
    - `mm.mmap_base = X86_USER_MMAP_MAX_ADDR - 0x2000` 且 `[X86_USER_MMAP_MAX_ADDR - 0x1000, X86_USER_MMAP_MAX_ADDR)` 已占 → 非固定 `0x2000` 返 `-ENOMEM`（绝不回绕到低址），非固定 `0x1000` 返 `X86_USER_MMAP_MAX_ADDR - 0x2000`；
    - `length = UINT64_MAX` / `UINT64_MAX - 0x100` 返 `-EINVAL`；`length = 0x0000400000000000ULL`（64 TiB，超剩余窗口）返 `-ENOMEM`；`addr = 0x40000000, length = 0xfffffffff0000000ULL` 返 `-EINVAL`。
  - `test_mmap_bad_low_base_clamps_without_overlap`（钉死评审重点 2）：
    - 对 3 组异常低基址（`0`、`0x500000`、`X86_USER_STACK_BASE`）各自独立执行：`vma_free_all` + `reset_mm_fixture()` → 预置 `MAP_FIXED` VMA 于 `[X86_USER_STACK_END, X86_USER_STACK_END + 0x2000)` → 手动置低基址 → 断言非固定 `do_mmap(0, 0x1000, ...)` 返回 `X86_USER_STACK_END + 0x2000`（不重叠、不落保护区）；另独立重置测未对齐 `mm.mmap_base = B + 0x123` 向上对齐返 `B + 0x1000`，以及 `mm.mmap_base = UINT64_MAX - 0x100` 返 `-ENOMEM`。
  - `test_mmap_advisory_hint_does_not_bypass_aslr`：置高址随机 `B`，非固定传入 `addr = 0x40000000` 与 `addr = 0x500000`，断言均忽略 hint 并从 `B` 分配。
  - `test_device_mmap_high_base_and_exhaustion_refcount`（钉死评审重点 5）：通过 `lifecycle_files_install` 注册带 `.mmap` 回调的伪设备节点，验证高随机基址 `B` 下非固定设备 `do_mmap` 成功发布 `VMA_IO`；当 `[B, limit)` 耗尽时返回 `-ENOMEM`、回调调用次数为 `0`、`lifecycle_state.vfs_get_calls == lifecycle_state.vfs_put_calls` 且 `files_get_calls == files_put_calls`。

- [ ] **Step 2: 运行测试确认 RED**
  运行：`make PROFILE=x86_64-clang test-host`
  预期：`test_x86_aslr_mmap` FAIL（旧 `mmap.c` 放行 `MAP_FIXED(0)`、在 `addr_limit = 0xffff800000000000` 下放行 `0x00007FFFFFFFF000`、在 `mm->mmap_base == 0` 盲覆写 `X86_USER_STACK_END` 导致重叠）。

- [ ] **Step 3: 修改 `kernel/arch/x86_64/memory/mmap.c`**
  - 新增 `static inline uint64_t mmap_effective_limit(uint64_t addr_limit)`（Spec §6.1）。
  - 在 `do_munmap()`（`:307`）与 `do_mprotect()`（`:757`）改用 `mmap_effective_limit(current->addr_limit)` 校验 `end > limit → -EINVAL`（Spec §6.4）。
  - 将 `mmap_find_gap()`（`:450-468`）替换为 Spec §6.3 的 `static int mmap_find_gap_locked(mm_t *mm, uint64_t length, uint64_t upper_limit, uint64_t *out_addr)`，严格按 Spec §6.3 伪代码实现（未命中时无条件推进 `cand`、检查 `v->vm_start >= upper_limit || v->vm_end >= upper_limit`、全程减法防溢出、不回绕）。
  - 更新 `do_mmap()` 参数与 `MAP_FIXED` 预检顺序（Spec §6.2）：先验 `length == 0`、`rounded = PAGE_4K_ALIGN(length); if (rounded < length) return -EINVAL;`、非零 `addr` 的下界与 `addr > UINT64_MAX - rounded` 溢出；在 `flags & MAP_FIXED` 分支强制验 `addr < X86_USER_MIN_ADDR → -EINVAL`、对齐、`addr >= limit || rounded > limit - addr → -ENOMEM`、`mm_user_range_protected → -EINVAL`。
  - 在设备分支（保留 `if (_dev_mmap) {`、`if (!(flags & MAP_FIXED)) {`、`mm_user_range_protected(` 位于 `kmalloc(sizeof(vma_t))` 之前的字面锚点）与普通分支中接入 `mmap_find_gap_locked(..., limit, &addr)` + 防御性 `mm_user_range_protected(...)`（命中返 `-ENOMEM` 并平衡释放引用），删除 `:579-590` 与 `:640-647` 的旧重复 `current->addr_limit` 检查。

- [ ] **Step 4: 验证所有 MM 宿主回归 PASS**
  运行：`make PROFILE=x86_64-clang test-host`
  预期：全量宿主测试 PASS（重点确认 `test_x86_aslr_mmap`、`test_user_protected_ranges` 含源码顺序扫描、`test_process_image_lifecycle`、`test_user_write_cow`、`test_brk_pages`、`test_fork_user_map` 均 PASS）。

- [ ] **Step 5: 提交任务 2**
  ```bash
  git add kernel/arch/x86_64/memory/mmap.c hosttests/cases/test_x86_aslr_mmap.c hosttests/mk/memory.mk
  git commit -m "fix(x86_64/mm): harden mmap gap search, canonical ceiling and MAP_FIXED(0) guard"
  ```

---

## 任务 3 — 接入 `spawn_user_task` / `sys_exec` 随机化、`fork` 继承与 `exec.c` 宿主测试

**Files:**
- Modify: `kernel/arch/x86_64/sched/exec.c:36,427,503-520,701-715`
- Modify: `kernel/arch/x86_64/sched/fork.c:25,509`
- Create: `hosttests/mock/aslr_exec/arch/x86_64/mmu.h`
- Create: `hosttests/cases/test_x86_aslr_exec.c`
- Modify: `hosttests/mk/sched.mk:935-1006`
- Modify: `hosttests/mk/fs.mk:677-690`
- Modify: `hosttests/cases/test_boot_spawn_failure.c:132`
- Modify: `hosttests/cases/test_boot_services_vfs.c:140`
- Modify: `hosttests/cases/test_boot_services_spawn.c:206,242-250,406`
- Modify: `config/test-scenarios.json`

**Interfaces:**
- Consumes: 任务 1 的 `X86_USER_ADDR_LIMIT`、`x86_aslr_init_mmap_base(mm_t *mm)`、`x86_aslr_derive_mmap_base(uint64_t)`；既有 `destroy_unpublished_user_mm()`、`kernel_random_get_strong()`
- Produces:
  - `spawn_user_task()` 与 `sys_exec()` 在建像阶段独立随机化 `mm->mmap_base`，无 STRONG 熵时 fail-closed 返 `-EAGAIN`
  - 宿主测试专用非特权 mock 头 `hosttests/mock/aslr_exec/arch/x86_64/mmu.h`（提供 `void aslr_exec_record_switch_mm(uint64_t *pgdir)` 回调记录）

- [ ] **Step 1: 创建非特权 `<arch/x86_64/mmu.h>` mock 与失败测试 `hosttests/cases/test_x86_aslr_exec.c`**
  - 创建 `hosttests/mock/aslr_exec/arch/x86_64/mmu.h`（include guard `_ARCH_X86_64_MMU_H`，显式 `#include <stdint.h>`、`#include <stdbool.h>`、`#include <stddef.h>` 对齐真实头）：声明 `void aslr_exec_record_switch_mm(uint64_t *pgdir);`，将 `arch_switch_mm(uint64_t *pgdir)` 实现为调用 `aslr_exec_record_switch_mm(pgdir)` 的 `static inline`（无 `movq %cr3` 特权指令），并提供无害的 `arch_get_page_table()` / `arch_flush_tlb_page()` / `arch_virt_to_phys()` / `arch_user_range_accessible()` 内联桩。
  - 创建 `hosttests/cases/test_x86_aslr_exec.c`（`@os01-test`：`id: test_x86_aslr_exec`、`layer: host`、`subsystem: sched`、`timing: strict`、`tier: pr`、`isolation: none`、`scenarios: [memory.aslr.x86_exec]`；使用 `test_framework.h` 的 `TEST_FUNC` / `TEST_LIST_BEGIN` / `RUN_ALL_TESTS()` / `TEST_RESULTS()`）。联编独立编译的生产 `exec.c`、`aslr.c`、`mm.c`（不复用 `boot3_exec_production.o`），并配置完整夹具与账本：
    - **分配回滚账本（`-Wl,--wrap=malloc -Wl,--wrap=calloc`）**：参照 `test_boot_services_spawn.c:171-202` 实现 `__wrap_malloc`（按 `sz >= sizeof(union task_union)` 记 `g_last_raw`、按 `sz == 512 + 16 || sz == 512 + 15` 记 `g_last_fpu`）、`__wrap_calloc`（按 `n * sz == sizeof(mm_t)` 记 `g_last_mm`、按 `n * sz == sizeof(thread_t)` 记 `g_last_thd`，因生产 `mm.c` 走 `calloc(1, sizeof(mm_t))`）与 `size_t kfree(void *p)`（匹配 `<memory/slab.h:67>`，分类累计 `kfree_mm_calls`/`kfree_old_mm_calls`/`kfree_thd_calls`/`kfree_raw_calls`/`fpu_free_calls` 后调用 `free(p)`）；桩内自用缓冲走 `__real_calloc` 避免污染账本（`alloc_pages()` 用 `__real_calloc(1, 0x210000)` 覆盖 `setup_user_stack` 写入偏移 `0x1ffff0`；`vmm_alloc_map()` 用 `__real_calloc(1, 4096)` 覆盖 `memcpy(&new_pgd[256], ...)`；`init_mm.pgdir` 指向 4096 字节静态缓冲；`sys_exec` 测试的 `old_mm` 亦由 `__real_calloc(1, sizeof(mm_t))` 分配并记入 `g_old_mm`）。
    - **刚性夹具重置 `reset_exec_fixture()`（每用例前调用）**：`BOOT3_HOST_CFLAGS` 拉入真实 x86 自旋锁（`.lock == 0` 为已锁态），须显式 `spin_init(&task_list_lock)`、`spin_init(&percpu_data[i].rq_lock)`、`list_init(&init_task_union.task.list)`、设置 `num_cpus = 1`、`percpu_data[0].online = 1`、`percpu_data[0].scheduler_ok = 1`（使 `exec.c:344` 的 `static bool spawn_boot_target_allowed(0)` 恒为 `true`）、清空 `user_init_task = NULL` / `user_init_pid = 0` 与全部计数；提供 `.type = VFS_FILE` 的伪节点、`arch_auxv_platform()`（返 `"x86_64"`）/`arch_auxv_payload_size()`（返 `7`）、`vfs_lookup`/`vfs_lookup_at`/`task_comm_apply_exec`、最小调度桩（`vfs_bootstrap_active()` 返 `false`、`sched_pick_cpu_mask(uint64_t)` 返 `0`、`sched_boot_creation_open()` 返 `true`、`arch_this_cpu_ptr()` 返 `&percpu_data[0]`、计数 `enqueue_task()`、无害 `sched_notify_remote()`）及可按调用序号控制成功/失败和熵序列的 `bool kernel_random_get_strong(uint8_t out[32])`；`spawn` 用例置 `g_current.files = &fake_files`（非空以覆盖 `exec.c:449` 的 `files_dup`/`files_unpin`）；`sys_exec` 用例置 `g_current.thread = &fake_thread`（非空供 `exec.c:743` 写 `cr3`）、`g_current.mm = old_mm` 与有效 `pt_regs_t`。
    - 编写 6 项测试：
      1. `test_spawn_aslr_strong_success`：第 1 次取熵给值 `R1`、第 2 次给值 `R2`（`R1 != R2`），断言 `spawn_user_task` 返正 PID、共调用 2 次 `kernel_random_get_strong`（独立于 `AT_RANDOM`）、已发布任务 `user_init_task->mm->mmap_base == x86_aslr_derive_mmap_base(R1)` 且 `user_init_task->addr_limit == X86_USER_ADDR_LIMIT`。
      2. `test_spawn_aslr_entropy_fail_closed`（评审重点 4）：第 1 次取熵返 `false`，断言 `spawn_user_task` 返 `-EAGAIN`、不分配用户栈（`alloc_pages_calls == 0`）、不入队/不发布（`enqueue_calls == 0 && user_init_task == NULL`），且 7 项回滚账本全为 `1`（`files_unpin_calls`/`fpu_free_calls`/`vma_free_all_calls`/`vmm_free_user_map_calls`/`kfree_mm_calls`/`kfree_thd_calls`/`kfree_raw_calls` 均 `== 1`）。
      3. `test_spawn_at_random_fail_after_aslr`（评审重点 4）：第 1 次取熵成功、第 2 次返 `false`，断言 `spawn_user_task` 返 `-EAGAIN`、不入队/不发布，且上述 7 项回滚账本同样全为 `1`。
      4. `test_exec_aslr_strong_success`：准备 `__real_calloc` 分配的 `old_mm`（`old_mm->mmap_base = 0x40000000ULL`、`old_mm->pgdir != NULL`），`sys_exec` 第 1/2 次取熵给 `R1`/`R2`，断言返回 `0`、共调用 2 次 `kernel_random_get_strong`、`current->mm != old_mm`、`current->mm->mmap_base == x86_aslr_derive_mmap_base(R1)`、`arch_switch_mm` 被调用 1 次且入参为 `current->mm->pgdir`、旧 `old_mm` 经 `vma_free_all`+`vmm_free_user_map`+`kfree` 回收。
      5. `test_exec_aslr_entropy_fail_preserves_old_mm`（评审重点 4）：第 1 次取熵返 `false`，断言 `sys_exec` 返 `-EAGAIN`、`arch_switch_mm` 调用 `0` 次、`current->mm == old_mm`、`old_mm->mmap_base` 原值不变、`task_comm_apply_exec` 未调用、`new_mm` 已走 `destroy_unpublished_user_mm` 且 `kfree_mm_calls == 1`（仅释放 `new_mm`）。
      6. `test_exec_at_random_fail_preserves_old_mm`（评审重点 4）：第 1 次成功、第 2 次返 `false`，断言 `sys_exec` 返 `-EAGAIN`、`arch_switch_mm` 调用 `0` 次、`current->mm == old_mm`、原 `mmap_base` 不变且 `kfree_mm_calls == 1`（仅释放 `new_mm`）。
  - 在 `hosttests/mk/sched.mk` 新增独立 `ASLR_EXEC_HOST_CFLAGS := -I$(TEST_MOCK)/aslr_exec $(BOOT3_HOST_CFLAGS)`，将 `aslr_exec_{exec,aslr,mm}_production.o` 均以 `$(ASLR_EXEC_HOST_CFLAGS) -ffunction-sections -fdata-sections` 编译，新增 `test_x86_aslr_exec.o`，并将 `test_x86_aslr_exec.elf` 链接命令显式写为 `$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=malloc -Wl,--wrap=calloc -o $@ $^`。
  - 在 `config/test-scenarios.json` 按 schema v2 补齐全部必填字段新增场景 `id: "memory.aslr.x86_exec"`（`owner: "host"`、`component: "memory"`、`behavior: "aslr"`、`trigger`、`assertions`、`failure_modes`、`primary: { layer: "host", source: "hosttests/cases/test_x86_aslr_exec.c", selector: "test_x86_aslr_exec", evidence: "ALL_TESTS_PASSED_OR_EXIT_0" }`、`supporting: []`、`overlap_decision: { action: "unique", reason: "..." }`）。

- [ ] **Step 2: 运行目录生成并确认 RED**
  运行：
  ```bash
  make regen-test-catalog
  make PROFILE=x86_64-clang test-host
  ```
  预期：`test_x86_aslr_exec` FAIL（`exec.c` 尚未调用 `x86_aslr_init_mmap_base()`，导致仅调用 1 次 `kernel_random_get_strong` 且 `mm->mmap_base` 仍为 `0x40000000`）。

- [ ] **Step 3: 接线 `exec.c`、`fork.c` 并同步既有 boot3 宿主测试**
  - `kernel/arch/x86_64/sched/exec.c`：
    - `#include <arch/x86_64/aslr.h>`；
    - `:427` 改为 `tsk->addr_limit = X86_USER_ADDR_LIMIT;`；
    - 在 `spawn_user_task()` 的 `mm_init_user_heap(mm, mm->end_code)` 成功之后、`alloc_pages(...)` 之前调用 `int aslr_rc = x86_aslr_init_mmap_base(mm); if (aslr_rc != 0)`：释放 `tsk->files`（`files_unpin`）、`tsk->fpu_save`、`destroy_unpublished_user_mm(mm)`、`mm`/`thd`/`raw_alloc`，透传返回 `aslr_rc`（熵失败时为 `-EAGAIN`）；
    - 在 `sys_exec()` 的 `mm_init_user_heap(new_mm, new_mm->end_code)` 成功之后、`alloc_pages(...)` 之前调用 `int aslr_rc = x86_aslr_init_mmap_base(new_mm); if (aslr_rc != 0)`：调用 `destroy_unpublished_user_mm(new_mm)`、`kfree(new_mm)`，透传返回 `aslr_rc`（熵失败时为 `-EAGAIN`）。
  - `kernel/arch/x86_64/sched/fork.c`：
    - 补充 `#include <arch/x86_64/user_layout.h>`；
    - `:509` 改为 `tsk->addr_limit = X86_USER_ADDR_LIMIT;`；确认 `fork_mm_copy()` 沿用 `memcpy(child_mm, parent_mm, sizeof(mm_t))` 不调 ASLR 接口。
  - 在 `hosttests/mk/sched.mk` 新增 `$(TEST_BLD)/boot3_aslr_production.o`（以 `$(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections` 编自 `kernel/arch/x86_64/memory/aslr.c`）并链接到 `test_boot_spawn_failure.elf`、`test_boot_services_spawn.elf`，在 `hosttests/mk/fs.mk` 链接到 `test_boot_services_vfs.elf`。
  - 修正 `hosttests/cases/test_boot_spawn_failure.c:132` 与 `hosttests/cases/test_boot_services_vfs.c:140` 的桩签名为 `bool kernel_random_get_strong(uint8_t out[32])`（返回 `false`，`memset(out, 0, 32)`）；将 `hosttests/cases/test_boot_services_spawn.c:242` 签名对齐为 `bool kernel_random_get_strong(uint8_t out[32])`、更新第 406 行触发时机注释，并将第 206 行 `128` 改为 `sizeof(mm_t)`。

- [ ] **Step 4: 验证全量宿主与框架回归 PASS**
  运行：
  ```bash
  make PROFILE=x86_64-clang test-host
  python3 qemutests/scenario_catalog.py --check
  make test-harness
  ```
  预期：全部 PASS（重点确认 `test_x86_aslr_exec`、`test_boot_spawn_failure`、`test_boot_services_vfs`、`test_boot_services_spawn`、`test_process_image_lifecycle`、`test_fork_user_map` 均 PASS），零编译警告与链接错误。

- [ ] **Step 5: 提交任务 3**
  ```bash
  git add kernel/arch/x86_64/sched/exec.c kernel/arch/x86_64/sched/fork.c \
          hosttests/mock/aslr_exec/arch/x86_64/mmu.h hosttests/cases/test_x86_aslr_exec.c \
          hosttests/mk/sched.mk hosttests/mk/fs.mk \
          hosttests/cases/test_boot_spawn_failure.c hosttests/cases/test_boot_services_vfs.c \
          hosttests/cases/test_boot_services_spawn.c \
          config/test-scenarios.json config/test-catalog.json mk/generated/
  git commit -m "feat(x86_64/sched): randomize user mmap_base on spawn/exec and keep fork inheritance"
  ```

---

## 任务 4 — 内核自测（`kernel_selftest`）与用户态 E2E（`systest`）

**Files:**
- Create: `kernel/selftest/test_aslr_mmap.c`
- Modify: `kernel/selftest/selftest.c:166,316`
- Modify: `user/systest/systest.h`
- Modify: `user/systest/systest.c:90-165,455-485`
- Modify: `user/systest/test_mem.c`
- Modify: `config/test-scenarios.json`

**Interfaces:**
- Consumes: 任务 1–3 的全部生产路径、`kernel_random_test_set_pool_quality()`（`#ifdef OS01_SELFTEST`）、`parse_hex16_line()`（`user/systest/test_mem.c:310`）
- Produces:
  - 内核自测入口 `int test_aslr_mmap(void);`（注册名 `"aslr_mmap"`）
  - 泛化后的 `static int exec_capture_argv(const char *path, char *const argv[], char *out, size_t outsz, int *out_status)`（`user/systest/test_mem.c:107`，原 `exec_capture()` 委托给它）
  - `systest` 自重入探针 `argv[0] == "aslr-probe"`（打印 `MMAP %016lx\n`）及 3 项用例：`aslr_mmap_exec_randomized`、`aslr_mmap_fork_inherit`、`aslr_mmap_fixed_and_bounds`

- [ ] **Step 1: 实现 `kernel/selftest/test_aslr_mmap.c` 并注册到 `kernel/selftest/selftest.c`**
  - 全文件包在 `#ifdef OS01_SELFTEST` 内（x86_64 `kernel/Makefile:206` 通配自动收纳，aarch64 不编译），参照 `kernel/selftest/test_at_random_strong_only.c` 模式：
    - 入口保存 `saved_q = random_get_pool_quality()`、`saved_ready = random_is_ready()`，探测一次 `arch_has_strong = arch_random_get_strong(probe)` 并清零 `probe`；每条失败早退宏均先恢复 `(saved_q, saved_ready)` 再返 `-1`；
    - 断言 `x86_aslr_init_mmap_base(NULL) == -EINVAL`；
    - 设 `ARCH_ENTROPY_STRONG, true`：对临时 `mm_t` 连续调用 8 次 `x86_aslr_init_mmap_base(&mm)`，断言每次返 `0`、4 KiB 对齐、`X86_USER_MMAP_RND_BASE <= base < X86_USER_MMAP_RND_END`，且 8 次结果不恒定（至少 7 个不同值）；
    - 分别设 `(ARCH_ENTROPY_WEAK, true)` 与 `(ARCH_ENTROPY_NONE, false)`、预置 `mm.mmap_base = 0x40000000ULL`：
      - 若 `!arch_has_strong`：断言返回 `-EAGAIN` 且 `mm.mmap_base == 0x40000000ULL`；
      - 若 `arch_has_strong`：断言返回 `0` 且 `mm.mmap_base` 4 KiB 对齐并落在 `[X86_USER_MMAP_RND_BASE, X86_USER_MMAP_RND_END)`；
    - 恢复 `(saved_q, saved_ready)` 并返回 `0`。
  - 在 `kernel/selftest/selftest.c` 的 `#ifdef OS01_SELFTEST` 声明区与 `#if !defined(__aarch64__)` 注册区加入 `selftest_register("aslr_mmap", test_aslr_mmap);`。

- [ ] **Step 2: 独立运行内核自测验证 PASS**
  运行：`make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`
  预期：退出 `0`，协议输出中 `aslr_mmap`、`at_random_strong_only`、`at_random_entropy` 全部 `PASS`。

- [ ] **Step 3: 实现 `systest` `aslr-probe`、3 组用户态 E2E 用例并补齐跨层场景目录**
  - 在 `user/systest/systest.c` 的 `main()` 中紧跟 `startup_probe(argc, argv);` 之后调用新增的 `aslr_probe(argc, argv);`：当 `argc >= 1 && argv && argv[0] && strcmp(argv[0], "aslr-probe") == 0` 时，调用 `mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`，若 `p == MAP_FAILED` 或写 `0xA5` 后回读 `p[0] != 0xA5`（验证高 64 位 VA 真实按需缺页）均 `_exit(99)`，否则 `printf("MMAP %016lx\n", (unsigned long)(uintptr_t)p); fflush(stdout);` 并 `_exit(0)`（不跑测试表）。
  - 在 `user/systest/systest.h` 声明并在 `user/systest/systest.c` `tests[]` 追加 3 条显式 `id != name`、`required = 1, quick = 0` 的条目（满足 `qemutests/test_systest_protocol.py:188-195`）：
    - `{"aslr_mmap_exec_randomized", "aslr mmap exec randomized", test_aslr_mmap_exec_randomized, 1, 0}`
    - `{"aslr_mmap_fork_inherit", "aslr mmap fork inherit", test_aslr_mmap_fork_inherit, 1, 0}`
    - `{"aslr_mmap_fixed_and_bounds", "aslr mmap fixed and bounds", test_aslr_mmap_fixed_and_bounds, 1, 0}`
  - 在 `user/systest/test_mem.c:107` 将 `exec_capture()` 泛化为 `static int exec_capture_argv(const char *path, char *const argv[], char *out, size_t outsz, int *out_status)`（原 `exec_capture()` 保留为薄封装，以 `{ (char *)path, NULL }` 委托调用 `exec_capture_argv()`，避免破坏既有 `canary_dump` 调用方），并实现 3 项用例：
    1. `test_aslr_mmap_exec_randomized`：循环 10 次以 `char *argv[] = { (char *)"aslr-probe", NULL };` 调 `exec_capture_argv("/bin/systest", argv, ...)` 并复用 `parse_hex16_line(buf, "MMAP", &addr)`；断言每次 `WIFEXITED && WEXITSTATUS == 0`、`(addr & 0xFFF) == 0`、`addr >= 0x0000400000000000UL && addr < 0x0000410000000000UL`、`addr != 0x40000000UL`，且 10 次样本中唯一地址数 `>= 9`。
    2. `test_aslr_mmap_fork_inherit`：父进程先映射 1 页 `p0`（验证落在 `[64 TiB, 65 TiB)`）并写哨兵 `0x5A`，建 pipe 后 `fork()`（至下一次匿名 `mmap` 前父子均不做额外映射）；子进程断言读回 `p0[0] == 0x5A`、写 `p0[0] = 0xC3`（触发 COW），再调匿名 1 页 `mmap` 得 `c1` 并经 pipe 写给父进程后退出；父进程调匿名 1 页 `mmap` 得 `p1`，断言 `p0[0] == 0x5A` 且 `c1 == p1`（证明 `fork` 原样继承 `mmap_base` 与布局）；再以 `char *argv[] = { (char *)"aslr-probe", NULL };` 调 `exec_capture_argv("/bin/systest", argv, ...)` 得 `e1`，断言 `e1 ∈ [64 TiB, 65 TiB)` 且 `e1 != p0 && e1 != p1`（证明 `exec` 重新随机化），最后 `munmap(p0)`/`munmap(p1)`。
    3. `test_aslr_mmap_fixed_and_bounds`：
       - `MAP_FIXED` 于 `0x40000000UL`（1 页）读写、`mprotect(PROT_READ)`、`munmap` 全成功；
       - `MAP_FIXED` 于 `0x0` 返 `MAP_FAILED` 且 `errno == EINVAL`；
       - `MAP_FIXED` 于顶有效页 `0x00007FFFFFFFE000UL`（1 页）读写、`mprotect(PROT_READ)`、`munmap` 全成功；
       - `MAP_FIXED` 于 `0x00007FFFFFFFF000UL` 与 `0x0000800000000000UL` 均返 `MAP_FAILED` 且 `errno == ENOMEM`；`munmap` 与 `mprotect` 对该两地址均返 `-1` 且 `errno == EINVAL`；
       - 匿名映射 3 页高随机区 `q`（`[q, q + 0x3000)`），**在拆页替换与 `munmap` 之前**立即读 `/proc/self/maps`，断言包含按 `kernel/fs/procfs.c:134` `"%012lx-%012lx"` 格式呈现的 `q` 到 `q + 0x3000` 区间行；随后写入页 0/1/2 哨兵、用 `MAP_FIXED` 替换 `q + 0x1000` 并验证页 1 归零且页 0/2 哨兵不变，最后 `munmap(q, 0x3000)`；
       - 非固定大映射 `0x0000400000000000UL`（64 TiB）返 `MAP_FAILED` 且 `errno == ENOMEM`（不回绕）。
  - 在 `config/test-scenarios.json`（此时全部源文件均已落盘，满足 `scenario_catalog.py:138` 存在性校验）完成完整 schema v2 登记与跨层关联（Spec §8）：
    - 新增 4 项独立场景（每条补齐 `id`、`owner`、`component: "memory"`、`behavior: "aslr"`、`trigger`、`assertions`、`failure_modes`、`primary`、`supporting: []`、`overlap_decision`）：
      - `memory.aslr.x86_selftest`（`owner: "kernel_selftest"`、`primary: { layer: "kernel_selftest", source: "kernel/selftest/test_aslr_mmap.c", selector: "aslr_mmap", evidence: "SELFTEST_PASS" }`）；
      - `memory.aslr.exec_randomized`、`memory.aslr.fork_inherit`、`memory.aslr.fixed_and_bounds`（`owner: "systest"`、`primary: { layer: "systest", source: "user/systest/test_mem.c", selector: "aslr_mmap_exec_randomized" / "aslr_mmap_fork_inherit" / "aslr_mmap_fixed_and_bounds", evidence: "SYSTEST_PASS" }`）。
    - 为任务 1、3 建立的两项宿主主责场景补齐跨层 `supporting` + 非空 `distinct_risk`：
      - `memory.aslr.x86_mmap.supporting`：挂接 `kernel_selftest`（`kernel/selftest/test_aslr_mmap.c` / `aslr_mmap` / `SELFTEST_PASS`，`distinct_risk` 说明驱动真实内核 ChaCha20 熵池与 `arch_random_get_strong` 后备，弥补宿主桩盲区）与 `systest`（`user/systest/test_mem.c` / `aslr_mmap_fixed_and_bounds` / `SYSTEST_PASS`，`distinct_risk` 说明在真实 4 级硬件页表、按需缺页、TLB 与 `/proc/self/maps` 上验证高随机区与上界）；
      - `memory.aslr.x86_exec.supporting`：挂接 `systest`（`aslr_mmap_exec_randomized` 与 `aslr_mmap_fork_inherit`，`SYSTEST_PASS`，`distinct_risk` 分别说明真实 syscall `fork+exec` 跨进程随机化与真实 `fork` 页表复制/COW 继承）及 `qemu_e2e`（`qemutests/test_entropy_facade.sh` / `tier1-default` / `PASS_MARKER`，`distinct_risk` 说明真实 QEMU 启动链路验证无 virtio-rng 下 PID 1 init fail-closed 与有 virtio-rng 下正常启动）。

- [ ] **Step 4: 独立运行 `systest` 与场景目录校验**
  运行（严禁与 `KERNEL_SELFTEST=1` 同设）：
  ```bash
  make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall
  python3 qemutests/scenario_catalog.py --check
  make test-harness
  ```
  预期：`test-syscall` 全量 PASS（含 3 项新增 ASLR 用例与全部既有 mmap/COW/protected-ranges/SSP 用例），`scenario_catalog.py --check` 与 `test-harness` 退出 `0`。

- [ ] **Step 5: 提交任务 4**
  ```bash
  git add kernel/selftest/test_aslr_mmap.c kernel/selftest/selftest.c \
          user/systest/systest.h user/systest/systest.c user/systest/test_mem.c \
          config/test-scenarios.json
  git commit -m "test(x86_64/aslr): add kernel selftest and systest E2E for mmap_base ASLR"
  ```

---

## 合入前全量验收与文档回填

- [ ] **全量门禁复核（逐项独立执行并记录实测结果）**
  ```bash
  make PROFILE=x86_64-clang test-host
  make PROFILE=x86_64-clang test-static
  make PROFILE=x86_64-clang test-static CASE=mm-header-boundary
  make PROFILE=aarch64-clang test-static CASE=mm-header-boundary
  make PROFILE=aarch64-clang test-aarch64-audit
  make check-test-catalog
  python3 qemutests/scenario_catalog.py --check
  make test-harness
  make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
  make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall
  make PROFILE=x86_64-clang test-qemu SUITE=phase-0
  bash qemutests/test_entropy_facade.sh tier1-default
  ```
  预期：全部命令退出 `0`；`tier1-default-no-virtio`（NONE）下 init 继续 fail-closed，`tier1-default-virtio`（STRONG）下正常启动至 shell。（注：`test_entropy_facade.sh` 复用已构建的正常产品态 `disk.img`，按上表紧跟 `SUITE=phase-0` 即可满足；若单独重跑该脚本，须先执行 `make PROFILE=x86_64-clang disk.img`。）

- [ ] **文档回填并提交**
  全部门禁通过后更新 `docs/memory/cow-mmap.md`（新增 x86_64 `mmap_base` ASLR 窗口、fail-closed 语义与 `mmap_effective_limit`/`mmap_find_gap_locked` 契约）、`docs/roadmap.md`（移除已完成项 `ASLR-A：mmap 基址` 并记入近期闭环索引，继续保留 `ASLR-B` 与栈随机化边界说明）与 `docs/changelog.md`（记录实测门禁结果），单独提交文档变更。
