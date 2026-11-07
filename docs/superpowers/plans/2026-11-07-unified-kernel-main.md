# 统一双架构 kernel_main 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 x86_64 与 aarch64 双架构的启动流程编排彻底统一到单一的 `kernel/core/main.c::kernel_main()`，通过特征宏包裹暂不支持的能力，消除 `aarch64_main()`。

**Architecture:** 按照 OS01 的 `weak-default + strong-override` 模式建立跨架构启动 Facade（`kernel/include/arch/boot.h`），在 `kernel/include/arch/features.h` 中提供编译期特性隔离，将 aarch64 早期 arena 预检的大栈帧抽离至独立函数以满足 512 字节通用栈帧门禁，升级 `test-aarch64-audit` 使用 `readelf` 严格审计入口符号。

**Tech Stack:** C (Clang / LLVM), AArch64 / x86_64 Assembly, GNU Make, Python unittest, QEMU.

**Spec:** `docs/superpowers/specs/2026-11-07-unified-kernel-main-design.md`

## Global Constraints

- **跨架构统一入口**：仅保留单一的全局代码入口符号 `kernel_main`，彻底消除 `aarch64_main`。
- **栈帧上限**：`kernel_main()` 与通用 Facade 调度函数栈帧严格限制在 512 字节以内；aarch64 早期物理内存 1536 字节栈数组限定在专用预检辅助函数 `aarch64_early_arena_preflight()` 中。
- **时序绝对保真**：x86_64 生产启动全路径（VFS、TTY、Futex、`/init.elf`）行为 100% 保持；aarch64 早期 Pre-SMP 自测、同步故障探针、时钟源后置分派、多核击落探针顺序 100% 保持。
- **构建独立性**：`make PROFILE=aarch64-clang aarch64-uefi-kernel` 必须独立编译链接成功，不依赖 `rootfs` 能力。

## Review Focus

1. **`AARCH64_READELF` 工具缺失与未定义**：Make recipe 展开必须能定位 `llvm-readelf`，工具缺失或执行非零时必须显式报错退出。
2. **符号审计假阳性/假阴性**：`readelf -Ws` 正则必须严格匹配完整末尾字段，拒绝 `kernel_main.extra` 或仅为 `UND` 的未定义符号。
3. **栈帧门禁回归**：`stack_frame_audit.py` 与 `test_stack_frame_audit.py` 必须精准更新基线例外为 `aarch64_early_arena_preflight: 1600`，`test-static` 与 `test-aarch64-audit` 均绿。
4. **`head.S` 遗留符号**：删除 `.globl aarch64_main` 声明与跳转，防止产物中残留未定义符号 `U aarch64_main`。
5. **契约测试解耦**：`test_double_state_publish.c` 与 `test_kernel_selftest_result.py` 不得依赖整文件全局字符串物理位置，必须按函数作用域断言调用顺序。

---

### Task 1: 特征宏与 Facade 头文件体系建设

**Files:**
- Create: `kernel/include/arch/features.h`
- Create: `kernel/include/arch/boot.h`
- Create: `kernel/include/arch/x86_64/boot_features.h` (或内嵌在 `boot.h`)
- Create: `kernel/include/arch/aarch64/boot_features.h` (或内嵌在 `boot.h`)
- Test: `hosttests/cases/test_boot_facade_headers.c`

- [ ] **Step 1: 编写 Host 单元测试验证特征宏与 Facade 声明**
  在 `hosttests/cases/` 增加 `test_boot_facade_headers.c`，在宿主模拟环境下分别定义 `__x86_64__` 与 `__aarch64__`，断言：
  - `OS01_HAS_VFS` 与 `OS01_HAS_USERSPACE` 在 x86 为 1，在 ARM 为 0；
  - 非法值与依赖不匹配时编译报错；
  - `arch/boot.h` 声明的 8 个核心函数原型可用。
- [ ] **Step 2: 创建 `kernel/include/arch/features.h`**
  实现 Spec §3 所述的特征宏：
  - 架构默认值：x86=1/1，aarch64=0/0；
  - 0/1 显式取值静态断言；
  - `OS01_HAS_USERSPACE` 依赖 `OS01_HAS_VFS` 断言。
- [ ] **Step 3: 创建 `kernel/include/arch/boot.h`**
  声明 Spec §6.1 中的跨架构通用 Facade 接口：
  - `arch_early_init`
  - `arch_boot_memory`
  - `arch_pre_subsys_selftest`
  - `arch_boot_subsystems`
  - `arch_boot_percpu`
  - `arch_boot_aps`
  - `arch_post_smp_selftest`
  - `arch_boot_device_nodes`
  - `arch_early_panic`
  - `arch_start_kernel_idle`
- [ ] **Step 4: 运行 host 验证测试**
  执行 `make -C hosttests`（或对应目标）确认头文件独立解析与宏校验通过。
- [ ] **Step 5: Commit**
  `git add kernel/include/arch/ hosttests/ && git commit -m "feat(arch): add feature gates and boot facade prototypes"`

---

### Task 2: x86_64 启动强覆盖适配与 Canary 汇编收敛

**Files:**
- Modify: `kernel/arch/x86_64/platform/boot.c`
- Modify: `kernel/arch/x86_64/memory/boot.c`
- Modify: `kernel/arch/x86_64/smp/boot.c`
- Modify: `mk/project.mk`
- Test: `make test-static`

- [ ] **Step 1: 在 `mk/project.mk` 限制 `KERNEL_CANARY_SELFTEST` 仅限 x86_64**
  添加约束：若 `KERNEL_CANARY_SELFTEST=1` 且 `TARGET_TRIPLE` 不以 `x86_64` 开头，显式 `$(error KERNEL_CANARY_SELFTEST=1 is only supported on x86 targets)`。
- [ ] **Step 2: 在 x86_64 boot 模块中实现 Facade 函数**
  - `kernel/arch/x86_64/platform/boot.c`:
    - `arch_early_init`: 调用既有 `x86_64_boot_early()`
    - `arch_boot_subsystems`: 调用既有 `x86_64_boot_subsystems()`
    - `arch_early_panic`: 确保 COM1 可用，写 `*** Corrupt UEFI handoff ***\n` 并 `while (1) arch_cpu_halt();`
  - `kernel/arch/x86_64/memory/boot.c`:
    - `arch_boot_memory`: 调用既有 `x86_64_boot_memory()`
    - `arch_pre_subsys_selftest`: 空操作
  - `kernel/arch/x86_64/smp/boot.c`:
    - `arch_boot_percpu`: 调用既有 `x86_64_boot_percpu()`
    - `arch_boot_aps`: 调用既有 `x86_64_boot_aps()`
    - `arch_post_smp_selftest`:
      ```c
      #ifdef OS01_SELFTEST
          serial_printk("[selftest] running built-in tests...\n");
          selftest_begin_run(arch_selftest_expected_late());
          selftest_run_all();
          serial_printk("[selftest] done\n");
      #endif
      ```
  - `kernel/arch/x86_64/platform/boot.c`:
    - `arch_boot_device_nodes`: 调用 `x86_64_boot_device_nodes()`，返回结果
- [ ] **Step 3: 运行 x86_64 静态检查**
  执行 `make test-static` 确保 x86 符号与函数原型一致。
- [ ] **Step 4: Commit**
  `git add kernel/arch/x86_64/ mk/project.mk && git commit -m "feat(arch/x86_64): implement boot facade hooks and gate canary test"`

---

### Task 3: aarch64 启动强覆盖适配、大栈帧隔离与汇编适配

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c`
- Modify: `kernel/arch/aarch64/head.S`
- Modify: `qemutests/stack_frame_audit.py`
- Modify: `qemutests/test_stack_frame_audit.py`
- Test: `python3 -m unittest qemutests.test_stack_frame_audit`

- [ ] **Step 1: 隔离 aarch64 1536 字节栈数组**
  在 `kernel/arch/aarch64/boot/main.c` 中，将原 `aarch64_main()` 内的 `scratch[MEMORY_RANGE_MAX]` 逻辑提取到独立的 `noinline` 辅助函数：
  ```c
  __attribute__((noinline))
  static void aarch64_early_arena_preflight(const struct boot_context *handoff)
  ```
- [ ] **Step 2: 更新 `stack_frame_audit.py` 及测试基线例外**
  - 在 `qemutests/stack_frame_audit.py:26`：将 `"aarch64_main": 1600` 替换为 `"aarch64_early_arena_preflight": 1600`。
  - 在 `qemutests/test_stack_frame_audit.py`：同步替换断言名称。
  - 运行 `python3 -m unittest qemutests.test_stack_frame_audit` 确保单元测试通过。
- [ ] **Step 3: 在 `kernel/arch/aarch64/boot/main.c` 中实现 Facade 接口并移除 `aarch64_main()`**
  - `arch_early_init`: `pl011_init()`, `vbar_el1` 设置, `aarch64_ram_init(bootctx)`
  - `arch_boot_memory`: `aarch64_early_arena_preflight()`, `pmm_init()`, smoke/boot_map selftest, `arch_boot_direct_map_init()`, `arch_vmm_init()`, `aarch64_boot_fb_init()`, `aarch64_page_table_probe_prepare()`
  - `arch_pre_subsys_selftest`: 执行 `selftest_begin_run(arch_selftest_expected_late())` + `selftest_run_all()`，紧随其后运行 `AARCH64_SYNC_FAULT_TEST` 探针
  - `arch_boot_subsystems`: `dtb_init(bootctx)`, `gic_init()`
  - `arch_boot_percpu`: `percpu_install_gs(0)`, `percpu_init(0, mpidr)`, `num_cpus`, `smp_starting_enter()`
  - `arch_boot_aps`: `smp_boot_aps()`, `test_spinlock_smp()`, `arch_register_subsys()`, `subsys_init_phase(4)`, clocksource 标记打印, `softirq_init()`, `arch_tick_start()`, GIC SPI probe, `arch_local_irq_enable()`, `ipi_ready_publish_and_count(0)`, `aarch64_shootdown_probe()`
  - `arch_post_smp_selftest`: 多核页表测试 `test_aarch64_page_table_multicore_run()`, BBM 打印, GIC dispatch probes
  - `arch_boot_device_nodes`: 弱默认返回 0
  - `arch_early_panic`: 内部先调用 `pl011_init()`，输出 `UEFI-A64: corrupt handoff\n[smp] FATAL: invalid UEFI handoff\n`，`for (;;) arch_cpu_halt();`
  - `arch_start_kernel_idle`: 关中断, `aarch64_ap_sched_prepare()`, `arch_aarch64_prepare_bsp_handoff()`, `aarch64_bsp_stack_enter(...)`
- [ ] **Step 4: 修改 aarch64 `head.S`**
  - 删除 line 31: `.globl aarch64_main`
  - 修改 line 303: `ldr x30, =aarch64_main` -> `ldr x30, =kernel_main`
- [ ] **Step 5: Commit**
  `git add kernel/arch/aarch64/ qemutests/stack_frame_audit.py qemutests/test_stack_frame_audit.py && git commit -m "feat(arch/aarch64): implement boot facade hooks, isolate stack frame and update head.S"`

---

### Task 4: 统一 `kernel_main()` 与构建系统集成

**Files:**
- Modify: `kernel/core/main.c`
- Modify: `kernel/Makefile`
- Test: `make PROFILE=aarch64-clang aarch64-uefi-kernel`
- Test: `make PROFILE=x86_64-clang kernel.bin`

- [ ] **Step 1: 改造 `kernel/core/main.c`**
  - 引入 `<arch/boot.h>`
  - 将 `main.c` 中的阶段调用切换为 Facade 接口
  - 包裹 `OS01_HAS_VFS` 与 `OS01_HAS_USERSPACE` 分支
  - 保留 `void arch10_capacity_fixture_init(void);` 局部前向声明
  - 保留 `arch_early_panic("corrupt UEFI handoff")` 校验失败处理
  - 非用户态架构在末尾调用 `arch_start_kernel_idle()`
- [ ] **Step 2: 改造 `kernel/Makefile`**
  - 在 aarch64 的 `KERNEL_C_SOURCES` 列表中加入 `core/main.c`
- [ ] **Step 3: 验证 aarch64 与 x86_64 内核编译链接**
  - 执行 `make PROFILE=aarch64-clang aarch64-uefi-kernel`，检查编译无 error/warning
  - 执行 `make PROFILE=x86_64-clang kernel.bin`，检查编译无 error/warning
- [ ] **Step 4: Commit**
  `git add kernel/core/main.c kernel/Makefile && git commit -m "refactor(core): unify boot orchestration in single kernel_main"`

---

### Task 5: 符号审计门禁与三态测试接入

**Files:**
- Modify: `mk/components/run.mk`
- Create: `qemutests/test_audit_symbol_contract.py`
- Test: `make test-harness`
- Test: `make PROFILE=aarch64-clang test-aarch64-audit`

- [ ] **Step 1: 编写 `test_audit_symbol_contract.py` 覆盖三态场景**
  在 `qemutests/test_audit_symbol_contract.py` 中编写单元测试，使用模拟 readelf 输出测试审计逻辑：
  - 正常 `FUNC GLOBAL DEFAULT 2 kernel_main$`：PASS
  - 缺失 `kernel_main`：FAIL
  - `kernel_main` 仅为 `UND`：FAIL
  - 后缀错误符号（如 `kernel_main.extra`）：FAIL
  - 残留 `aarch64_main`：FAIL
  - 工具返回非零退出码（即使产生文本）：FAIL
  - 工具缺失：FAIL
- [ ] **Step 2: 更新 `mk/components/run.mk`**
  - 定义 `AARCH64_READELF ?= llvm-readelf`
  - 在 `test-aarch64-audit` 中捕获 `syms` 变量并检查退出码，收紧正则为 `[[:space:]]+kernel_main[[:space:]]*$$`
  - 将 `qemutests.test_audit_symbol_contract` 追加到 `TEST_HARNESS_MODULES`
- [ ] **Step 3: 运行 `make test-harness` 与 aarch64 审计**
  - `make test-harness` 全绿
  - `make PROFILE=aarch64-clang test-aarch64-audit` 通过
- [ ] **Step 4: Commit**
  `git add mk/components/run.mk qemutests/test_audit_symbol_contract.py && git commit -m "test(audit): add rigorous symbol audit for aarch64 kernel_main"`

---

### Task 6: 源码契约测试语义化重构

**Files:**
- Modify: `hosttests/cases/test_double_state_publish.c`
- Modify: `qemutests/test_kernel_selftest_result.py`
- Test: `make test-host`
- Test: `make test-harness`

- [ ] **Step 1: 重构 `hosttests/cases/test_double_state_publish.c`**
  - 将全局扫描拆解为作用域断言：
    1. 扫描 `kernel/core/main.c::kernel_main()` 确认 `arch_boot_percpu()` 在 `arch_boot_aps()` 之前调用；
    2. 扫描 `arch_boot_percpu()` 确认 `percpu_init` -> `online=1` -> `num_cpus` -> `smp_starting_enter()`；
    3. 扫描 `arch_boot_aps()` 确认 `smp_boot_aps()` -> `arch_local_irq_enable()` -> `ipi_ready_publish_and_count(0)`；
  - 包含负向测试：函数定义位置交换依然 PASS，实际调用位置交换 FAIL。
- [ ] **Step 2: 重构 `qemutests/test_kernel_selftest_result.py`**
  - 扫描 `kernel/core/main.c::kernel_main()` 确认 `arch_pre_subsys_selftest()` 的存在与调用时序；
  - 扫描 `arch_pre_subsys_selftest()` 确认 `selftest_begin_run` 与 `selftest_run_all` 的顺序。
- [ ] **Step 3: 运行宿主与框架测试**
  - 执行 `make test-host`
  - 执行 `make test-harness`
- [ ] **Step 4: Commit**
  `git add hosttests/cases/test_double_state_publish.c qemutests/test_kernel_selftest_result.py && git commit -m "test(contract): refactor boot sequence assertions to semantic scoped checks"`

---

### Task 7: 全系统回归验证与验收

**Files:**
- None (Verification only)
- Test: All suites

- [ ] **Step 1: 静态审计与宿主测试全量回归**
  - `make test-static`
  - `make PROFILE=aarch64-clang test-aarch64-audit`
  - `make test-host`
  - `make test-harness`
- [ ] **Step 2: x86_64 QEMU 端到端回归**
  - `make test-qemu SUITE=phase-0`
  - `make OS01_SYSTEST=1 test-qemu SUITE=systest`
  - `make KERNEL_SELFTEST=1 test-kernel-selftest`
  - `make CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity`
- [ ] **Step 3: aarch64 QEMU 端到端回归**
  - `make PROFILE=aarch64-clang test-aarch64 MODE=smp`
  - `make PROFILE=aarch64-clang test-aarch64-boot-logo`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=sched`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=no-ack`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi`
  - `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe`
- [ ] **Step 4: 检查 `git status` 与 `git diff` 确认工作区完全干净**
