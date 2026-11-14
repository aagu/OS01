# 静态审计处置表 (Audit Dispositions)

本文档依据 [`docs/testing/testing-policy.md`](testing-policy.md) 第 7 节「断言分层与源码断言治理」与「最弱充分断言」原则，对全量 14 项静态审计（以及相关源码扫描测试）逐项登记处置结论（`替换` / `豁免` / `降级`）、技术依据及 `config/assertion-exemptions.json` 中的对应豁免条目。用户按批次评审推进。

---

## 1. 14 项静态审计处置总表

| # | 审计名称 | 脚本 / 目标 | 处置分类 | 依据 | 对应豁免条目 (`config/assertion-exemptions.json`) | 批次 / 优先级 |
|---|---|---|---|---|---|---|
| 1 | `runtime-audit` | `qemutests/runtime_audit.py` | **豁免**（无需登记：允许钉） | 仅检查 `llvm-nm` / `llvm-readobj` ELF 符号与链接回执，不读取生产源码做文本匹配 | 无（不触发 lint） | 无需处理 |
| 2 | `stack-canary-audit` | `qemutests/stack_canary_audit.py` | **豁免**（无需登记：允许钉） | 仅检查 `.o` 与 `kernel.elf` 的 `llvm-readelf` / `llvm-objdump` 反汇编与重定位 | 无（不触发 lint） | 无需处理 |
| 3 | `validate-kernel` | `make validate-kernel` | **豁免**（无需登记：允许钉） | 校验构建产物 `kernel.elf` / `kernel.bin` 节区与引导签名，不涉及 Python 源码正则断言 | 无（不触发 lint） | 无需处理 |
| 4 | `runtime-link-order` | `qemutests/runtime_link_order_test.py` | **豁免**（无需登记：允许钉） | 检查构建链接顺序契约与 `clang` 链接产物，不读取生产源码 | 无（不触发 lint） | 无需处理 |
| 5 | `kernel-runtime-link` | `qemutests/kernel_runtime_link_test.py` | **替换**（待执行） | 除检查链接回执外还读取源码校验引用；应完全收敛为 `kernel-link.receipt` + `llvm-nm` 未定义符号检查 | `qemutests/kernel_runtime_link_test.py` (`pending`) | Batch 2 / P2 (`runtime`) |
| 6 | `syscall-boundary` | `qemutests/syscall_boundary_audit.py` | **豁免**（待裁决结构不变量或替换为编译探针） | 检查 `uapi/syscall.h` 与内核/用户态 syscall 边界；需核实是否可转为独立 TU 编译探针 | `qemutests/syscall_boundary_audit.py` (`pending`) | Batch 2 / P1 (`syscall`) |
| 7 | `kernel-layout` | `qemutests/x86_64_kernel_layout_test.py` | **豁免**（无需登记：允许钉） | 仅使用 `llvm-nm` / `llvm-readelf` 检查内核虚拟地址与段布局 | 无（不触发 lint） | 无需处理 |
| 8 | `kernel-canary-contract` | `qemutests/kernel_canary_contract_test.py` | **豁免**（无需登记：允许钉） | 检查 stack canary 运行时契约，不读取生产源码 | 无（不触发 lint） | 无需处理 |
| 9 | `header-object-audit` | `qemutests/header_object_audit.py` | **豁免**（无需登记：允许钉） | 对 `kernel/include` 头文件做独立 `clang` 编译与 `llvm-nm` 零符号定义检查 | 无（不触发 lint） | 无需处理 |
| 10 | `stack-frame-audit` | `qemutests/stack_frame_audit.py` | **豁免**（无需登记：允许钉） | 仅解析 `llvm-objdump` 反汇编栈帧大小（<= 512B） | 无（不触发 lint） | 无需处理 |
| 11 | `driver-model-boundary` | `qemutests/driver_model_boundary_audit.py` | **豁免**（待裁决结构不变量） | 扫描 `kernel/` 禁止驱动直接绕过子系统注册边界；待核实 `docs/subsys/subsys.md` 指针后裁决 | `qemutests/driver_model_boundary_audit.py` (`pending`) | Batch 2 / P1 (`drivers`) |
| 12 | `gui-build-audit` | `qemutests/gui_build_audit.py` | **替换**（样板已落地 1 项，其余待替换） | 公共头文件 LVGL 措辞 grep 已替换为 `clang` `#ifdef LVGL_H` + `typedef` 编译探针；其余源码/Makefile grep 子检查按批次替换为符号/链接检查 | `qemutests/gui_build_audit.py` (`pending`) | Batch 1 样板 / P1 (`gui`) |
| 13 | `intr-dispatch-audit` | `qemutests/intr_dispatch_audit.py` | **豁免**（结构不变量，已裁决） | 结合 `llvm-nm`/`llvm-objdump` 与汇编/C 入口表校验 `set_intr_gate_raw` / `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER` 跨文件结构不变量 | `qemutests/intr_dispatch_audit.py` (`structural-invariant`) | **Batch 1 已裁决** (`intr`) |
| 14 | `arch10-capacity-audit` | `qemutests/arch10_capacity_audit.py` | **降级 / 替换**（待执行） | 通过源码正则读取容量与 SMP 常量；应降级或替换为编译期 `sizeof`/`_Static_assert` 或运行时 selftest 校验 | `qemutests/arch10_capacity_audit.py` (`pending`) | Batch 2 / P1 (`sched/mm`) |

> 注：`test-static` 另包含 `user-canary`（shell 片段，检查 ELF canary 符号）与 `assertion-policy`（本政策门禁自身），均不涉及业务源码文本断言。

---

## 2. Batch 1 首批裁决与样板落地

1. **`mm_header_boundary` (`qemutests/mm_header_boundary.py`, `CASE=mm-header-boundary`)**：
   - 处置：**无需源码豁免（已符合允许钉 / 结构不变量规范）**。
   - 依据：该审计通过真实目标编译器对独立临时 TU 执行 `#include <memory/<h>.h>` + `#ifdef` 禁止宏 `#error` 探针 + `sizeof` 未声明符号 compile-fail 探针，完全不读取生产源码做文本匹配；其所守护的公共/私有内存头文件边界见 `AGENTS.md#Directory organization` 与 `docs/arch.md`。
2. **`intr_dispatch_audit` (`qemutests/intr_dispatch_audit.py`)**：
   - 处置：**豁免 (`structural-invariant`)**。
   - `policy_ref`：`AGENTS.md#Critical gotchas (set_intr_gate_raw / DEFINE_INTR_STUB + REGISTER_INTR_HANDLER)`。
   - 依据：在 `llvm-nm` / `llvm-objdump` 二进制分析之外，还需交叉核对汇编 stub 宏与 C 注册表之间的一致性，单次编译期 TU 无法独立表达该跨文件调度契约。
3. **`test_brk_source_scan` (`qemutests/test_brk_source_scan.py`)**：
   - 处置：**保持 `pending`（不予结构不变量豁免）**。
   - 依据：该测试校验 `hosttests/cases/test_brk_pages.c` 对 `kernel/syscall/sys_mm.c` 与 `kernel/arch/x86_64/intr/trap.c` 的字面量锚点（如 `case SYS_brk: {`、`if (!(error_code & 0x01)) {`）扫描，属于脆弱的实现细节文本匹配，而非全局结构不变量；后续批次应由 `host` / `systest` 行为测试替代。
4. **`gui_build_audit` (`qemutests/gui_build_audit.py`) 首个「替换」样板**：
   - 将第 6 节公共头文件检查中的纯措辞 grep（`if "lvgl" in code.lower():`）替换为 `consumer.c` 编译期探针（`#ifdef LVGL_H` -> `#error` 与 `typedef int lv_obj_t; typedef int lv_display_t;` 冲突声明检查），由 `host_cc -fsyntax-only -Wall -Wextra -Werror` 直接验证头文件隔离性；其余子检查暂保持 `pending`。

---

## 3. 其余 `pending` 条目清单 (`config/assertion-exemptions.json`)

以下条目已登记 `owner` 与 `priority`，将在后续批次中逐一评审为「替换 / 豁免 / 降级」：

| 文件路径 | `owner` | 优先级 | 说明 |
|---|---|---|---|
| `qemutests/aarch64_smp_test.py` | `arch/aarch64` | P1 | 校验 `kernel/arch/aarch64/` `sync_entry` 源码顺序 |
| `qemutests/syscall_boundary_audit.py` | `syscall` | P1 | 校验 UAPI syscall 边界 |
| `qemutests/driver_model_boundary_audit.py` | `drivers` | P1 | 扫描 `kernel/` 驱动模型边界 |
| `qemutests/arch10_capacity_audit.py` | `sched/mm` | P1 | 扫描容量与 SMP 常量 |
| `qemutests/gui_build_audit.py` | `gui` | P1 | 样板已替换 1 项，其余子检查待清理 |
| `qemutests/test_brk_source_scan.py` | `memory` | P1 | `brk` 源码锚点扫描测试，待替换为行为测试 |
| `qemutests/test_x86_intr_fault.py` | `arch/x86_64` | P1 | 校验 `smp.c` / `trampoline.S` AP NMI 顺序 |
| `qemutests/aarch64_boot_logo.py` | `drivers/fb` | P2 | 读取 `logo.c` 与字库派生预期像素计数 |
| `qemutests/kernel_runtime_link_test.py` | `runtime` | P2 | 检查链接回执与 runtime 引用 |
| `qemutests/test_arch10_capacity_runner.py` | `test-framework` | P2 | capacity runner 单元测试 |
| `qemutests/test_build_incremental.py` | `build` | P2 | 增量构建测试 |
| `qemutests/test_doom_build_contract.py` | `user/doom` | P2 | Doom 构建契约测试 |
| `qemutests/test_intr_dispatch_audit.py` | `intr` | P2 | `intr_dispatch_audit.py` 单元测试 |
| `qemutests/test_monitor_runner.py` | `test-framework` | P2 | 对齐 `user/terminal/*.c` 诊断日志格式 |
| `tools/tests/scan_tests.py` | `test-framework` | P2 | 扫描 `@os01-test` 元数据块 |
| `tools/tests/test_kernel_headers.py` | `build` | P2 | `kernel/include` 头文件依赖检查 |
