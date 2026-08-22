# 多架构抽象收尾设计

> **日期**: 2026-07-14
> **状态**: 设计 v2（根据 Code Review 修订）
> **目标**: 完成所有架构无关代码的 x86_64 依赖清理，建立 aarch64 桩，使 `-target aarch64-unknown-none` 能编译到链接期
> **参考**: Multi-arch abstraction plan (`docs/superpowers/plans/2026-07-11-multi-arch-abstraction-plan.md`)
> **Review**: `/tmp/multi-arch-cleanup-design-review.md`（11 条全部成立 + 2 条补充）

---

## 1. 现状

基于 **Phase A-E 已完成的抽象层**（11 个通用 `arch/*.h` 头文件 + 子系统注册框架 + 通用代码 `arch_` 迁移），仍有 5 类残留问题阻止 aarch64 编译。

### 1.1 引用关系图（当前）

```
通用头文件:
  memory.h ────┬── get_cr3() inline x86 asm ❌
               └── PAGE_OFFSET 0xffff800000000000 写死 ❌
  file.h ────────── arch/x86_64/spinlock.h ❌
  printk.h ──────── arch/x86_64/spinlock.h ❌
  tty.h ─────────── arch/x86_64/spinlock.h ❌
  wait.h ────────── arch/x86_64/spinlock.h ❌
  interrupt.h ───── arch/x86_64/linkage.h ❌ (ENTRY 宏)
  task.h ────────── arch/x86_64/linkage.h ❌ (ENTRY 宏)

  ext2.h ────────── arch/x86_64/spinlock.h ❌ (未在 v1 清单)

通用 .c 文件:
  main.c ─────── arch/x86_64/spinlock.h ❌ + arch/x86_64/cpu.h ❌ (入口文件)
  deferred_free.c ─ arch/x86_64/spinlock.h ❌ (未在 v1 清单)
  mutex.c ───────── arch/x86_64/cpu.h ❌      (未在 v1 清单 — atomic_cas/atomic_write)
  futex.c ───────── arch/x86_64/cpu.h ❌      (未在 v1 清单 — atomic_cas/atomic_write)

arch/ 通用头文件:
  cpu.h ──── aarch64: #error 未实现 ❌
  irq.h ──── aarch64: #error 未实现 ❌
  mmu.h ──── aarch64: #error 未实现 ❌ | x86_64 arch_virt_to_phys 硬编码 0xffff800... ❌
  atomic.h ─ aarch64: #error 未实现 ❌
  thread.h ─ aarch64: #error 未实现 ❌
  cache.h ── aarch64: 空桩(需要真实 dcache 操作) ❌
  segment.h ─ 纯 x86 值, aarch64 需要 0 ❌
  gate.h ─── 不存在 dispatch 头文件 ❌ (set_tss64 — task_init + __switch_to 需要)

test/include/ 镜像（本次同步迁移）:
  file.h ────────── arch/x86_64/spinlock.h ❌
  printk.h ──────── arch/x86_64/spinlock.h ❌
  interrupt.h ───── arch/x86_64/linkage.h + arch/x86_64/regs.h ❌
  task.h ────────── arch/x86_64/linkage.h + arch/x86_64/cpu.h + arch/x86_64/regs.h ❌
  trace.h ───────── arch/x86_64/regs.h ❌

构建系统:
  kernel/Makefile: HOSTARCH := x86_64 硬编码 ❌
  kernel/Makefile:164: objcopy -I elf64-x86-64 硬编码 ❌ (未使用 OBJFORMAT)
```

### 1.2 全量 spinlock.h 引用统计

15 个站点（不含 `docs/`、`arch/x86_64/` 内部）：

| 分类 | 文件 | 行号 |
|------|------|------|
| 通用头文件 | `kernel/include/kernel/file.h` | 5 |
| | `kernel/include/kernel/printk.h` | 5 |
| | `kernel/include/kernel/tty.h` | 7 |
| | `kernel/include/kernel/wait.h` | 6 |
| | `kernel/include/fs/ext2.h` | 8 |
| 通用 .c 文件 | `kernel/kernel/main.c` | 8 |
| | `kernel/kernel/log.c` | 3 |
| | `kernel/kernel/printk.c` | 6 |
| | `kernel/memory/pmm.c` | 8 |
| | `kernel/driver/keyboard.c` | 11 |
| | `kernel/sched/deferred_free.c` | 6 |
| | `kernel/sched/task.c` | 5 |
| | `kernel/test/selftest.c` | 8 |
| test 镜像 | `test/include/kernel/file.h` | 5 |
| | `test/include/kernel/printk.h` | 5 |

### 1.3 全量 `arch/x86_64/cpu.h` 直接引用（需迁移的）

所有文件均应改为 `<kernel/arch/cpu.h>`（dispatch 已存在）：

| 文件 | 行号 | 实际使用 |
|------|------|----------|
| `kernel/mutex.c` | 3 | `atomic_cas`, `atomic_write` |
| `kernel/futex.c` | 8 | `atomic_cas`, `atomic_write` |

### 1.4 test/include/ 镜像同步清单

`test/include/kernel/` 镜像了若干内核头文件，需与内核侧同步迁移：

| 文件 | 引用 | 迁移目标 |
|------|------|----------|
| `file.h:5` | `arch/x86_64/spinlock.h` | `arch/spinlock.h` |
| `printk.h:5` | `arch/x86_64/spinlock.h` | `arch/spinlock.h` |
| `interrupt.h:5-6` | `arch/x86_64/regs.h` + `arch/x86_64/linkage.h` | 保持 linkage.h 不变；regs.h → `arch/thread.h` |
| `task.h:6-8` | `arch/x86_64/cpu.h` + `arch/x86_64/regs.h` + `arch/x86_64/linkage.h` | cpu.h → `arch/cpu.h`；regs.h → `arch/thread.h`；linkage.h 保持 |
| `trace.h:4` | `arch/x86_64/regs.h` | `arch/thread.h` |

---

## 2. 设计原则

1. **零行为变更** — 所有 refactor 必须 `make clean && make` 0 warning，`make test-syscall` 70/70
2. **逐步可提交** — 每个 Task 可独立 git commit，bisectable
3. **保持向后兼容** — `spinlock_T` 类型名、`cli/sti` 宏等不对外接口不变
4. **最小化影响范围** — 明确豁免：`apic/`, `intr/pic/`, `pit.c`, `sched/smp.c`, `kernel/kernel/main.c`（入口文件，纯 x86 启动序列，aarch64 有自己的启动流程）；`kernel/sched/task.c` 不豁免——**拆分**（x86 `__switch_to` / 线程入口 asm 移出，其余架构无关逻辑保留）
5. **test/include/ 同步** — 镜像文件跟随内核头文件同步迁移，保持一致

---

## 3. 设计决策

### 3.1 `get_cr3()` 处理

**决定**: 从 `memory.h` 删除内联函数，移至 `arch/mmu.h` 作为 `static inline uint64_t *arch_get_page_table(void)`

**理由**:
- `get_cr3()` 只在 3 个文件中调用（`vmm.c` debug, `smp.c` AP boot, `task.c` init_mm）
- `smp.c` 已豁免；`task.c` 的 `task_init` 中 `get_cr3()` 通过 `arch_get_page_table()` 调用
- `vmm.c` 的调用在 `#ifdef DEBUG` 块内

**实现**:
- `arch/mmu.h` x86_64 分支新增 `arch_get_page_table()` → `movq %%cr3, %0`
- `memory.h` 删除 `get_cr3()` 定义
- `memory.h` 内联函数改为 `#define get_cr3() arch_get_page_table()` 向后兼容别名

### 3.2 `PAGE_OFFSET` 架构化

**决定**: 在现有 `arch/mmu.h` 中新增 `#define ARCH_PAGE_OFFSET`

**理由**:
- `PAGE_OFFSET` 用于 `Phy_To_Virt()`/`Virt_To_Phy()`，是 higher-half 核心常量
- x86_64: `0xffff800000000000`; aarch64 典型值: `0xffff000000000000`
- 放在 `arch/mmu.h` 而非新建文件，少一个文件，且 `arch_virt_to_phys` 已在同一文件中

**实现**:
- `arch/mmu.h` x86_64 分支新增 `#define ARCH_PAGE_OFFSET 0xffff800000000000ULL`
- `memory.h` 改为 `#define PAGE_OFFSET ARCH_PAGE_OFFSET`
- `arch/mmu.h` 的 `arch_virt_to_phys` 内三处字面量 `0xffff800000000000ULL` 替换为 `ARCH_PAGE_OFFSET`

### 3.3 spinlock.h 抽象化

**决定**: 新增 `kernel/include/kernel/arch/spinlock.h`

**理由**:
- 15 个站点（含 test 镜像）直接引用 `arch/x86_64/spinlock.h`
- 用 `#ifdef` dispatch 是最小变更方案

**实现**:

```c
// kernel/include/kernel/arch/spinlock.h
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/spinlock.h>
#elif defined(__aarch64__)
#include <kernel/arch/aarch64/spinlock.h>
#else
#error "Unsupported architecture"
#endif

#endif
```

**影响**: 全部 15 个站点的 include 路径改为 `<kernel/arch/spinlock.h>`

### 3.4 linkage.h + asmlinkage 处理

**决定**: 删除 `asmlinkage`；linkage.h 不做 dispatch

**理由**:
- `asmlinkage` 定义为 `__attribute__((regparm(0)))`，全代码库 **0 处调用**（grep 确认）
- `regparm` 是 x86 特有 attribute，aarch64 clang 产生 `-Wignored-attributes` warning
- `ENTRY` / `SYMBOL_NAME` 宏只用于 x86 汇编入口点声明（`interrupt.h`、`task.h`），语义上不需要跨架构 dispatch
- aarch64 需要时自己提供 `arch/aarch64/linkage.h`，路径相同即可

**实现**:
- `arch/x86_64/linkage.h` 中删除 `#define asmlinkage __attribute__((regparm(0)))` 行
- `interrupt.h`、`task.h` 保持 `#include <kernel/arch/x86_64/linkage.h>` 不变
- 本次不新增 `arch/linkage.h` dispatch 头文件

### 3.5 Build 系统参数化

**决定**: `ARCH` 变量从根 Makefile 进入

**实现**:
- `kernel/Makefile`: `HOSTARCH := x86_64` → `ARCH ?= x86_64`
- `ARCHDIR := arch/$(ARCH)`
- `BUILD_DIR := ../build/$(ARCH)/kernel`
- CC/LD 根据 ARCH 选择 target triple:
  ```makefile
  ifeq ($(ARCH),x86_64)
  CC := clang -target x86_64-unknown-none
  LD := ld.lld -m elf_x86_64
  OBJFORMAT := elf64-x86-64
  else ifeq ($(ARCH),aarch64)
  CC := clang -target aarch64-unknown-none
  LD := ld.lld -m aarch64elf
  OBJFORMAT := elf64-littleaarch64
  endif
  ```
- `kernel/Makefile:164` 的 binary 生成规则中硬编码 `elf64-x86-64` 替换为 `$(OBJFORMAT)`
- `kernel/arch/aarch64/make.config` 创建新文件

### 3.6 arch 头文件 aarch64 桩

每个 `arch/*.h` 的 aarch64 分支实现方案：

| 头文件 | 接口 | aarch64 实现 | 编译验证方法 |
|--------|------|-------------|-------------|
| `cpu.h` | `halt, pause, nop, cycle_counter, enable_nx, set_percpu_base` | WFI, YIELD, NOP, `mrs cntvct_el0`, `msr sctlr_el1[1]=1`, `msr tpidr_el1` | 编译不链接 |
| `irq.h` | `local_irq_enable/disable/save/restore`, `arch_irq_state_t` | `msr daifclr, #2`, `msr daifset, #2`, `mrs x0, daif`, `msr daif, x0` | 编译不链接 |
| `mmu.h` | `flush_tlb_all/page, switch_mm, virt_to_phys`, `get_page_table` | `tlbi vmalle1`, `tlbi vae1`, `msr ttbr0_el1`, 4-level 页表遍历, `mrs ttbr0_el1` | 编译不链接 |
| `atomic.h` | `fetch_add/sub, inc, read, write, cas, xchg` | `ldxr`/`stxr` 独占循环 | 编译不链接 |
| `thread.h` | `pt_regs_t` | aarch64 异常压栈布局（29 个通用寄存器 + SP + PC + PSR） | 编译不链接 |
| `cache.h` | `flush_dcache, inval_dcache` | `dc cvac`, `dc ivac`, `dsb sy` | 编译不链接 |
| `segment.h` | `KERNEL_CS/DS, USER_CS/DS` | 全部为 0（aarch64 无段机制） | 编译不链接 |

### 3.7 gate.h dispatch（新增）

**决定**: 新增 `kernel/include/kernel/arch/gate.h`

**理由**:
- `set_tss64` 定义在 `arch/x86_64/gate.h`，被 `task_init`（task.c）和 `__switch_to`（将移入 `arch/x86_64/`）调用
- aarch64 无 TSS，需空桩

**实现**:

```c
// kernel/include/kernel/arch/gate.h
#ifndef _ARCH_GATE_H
#define _ARCH_GATE_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/gate.h>
#elif defined(__aarch64__)
// aarch64 has no TSS — all TSS operations are no-ops
#define set_tss64(rsp0, rsp1, rsp2, ist1, ist2, ist3, ist4, ist5, ist6, ist7) do{}while(0)
#else
#error "Unsupported architecture"
#endif

#endif
```

### 3.8 task.c 拆分（设计决策 #4）

**决定**: 拆分 `kernel/sched/task.c`，将 x86 特有代码移入 `arch/x86_64/`

**拆分方案**:

| 模块 | 位置 | 内容 |
|------|------|------|
| `__switch_to` | `kernel/arch/x86_64/switch.c` | TSS 操作、FS 段重装、CR3 切换、`fxsave`/`fxrstor`（L26–76） |
| 线程入口 asm | `kernel/arch/x86_64/thread_entry.S` | 内联汇编线程入口，x86 栈帧布局（L611–650） |
| task_init x86 | `kernel/arch/x86_64/task_arch.c` | `set_tss64` 调用、`get_cr3` 调用、`init_tss[].rsp0` 设置 |
| 其余逻辑 | `kernel/sched/task.c` | `schedule`, blocker, `do_exit`, `do_waitpid`, `spawn_user_task`, `sys_exec`, `fork_mm_copy`, `do_fork`, `kernel_thread`, `task_init` 主体 |

**接口设计**:
- `do_switch_to(task_t *prev, task_t *next)` — 架构无关入口，内部调用 `arch_switch_to(prev, next)`
- `arch_switch_to` — 在 `arch/thread.h` x86_64 分支中声明，实现在 `arch/x86_64/switch.c`
- `arch_task_init_platform()` — 在 `arch/thread.h` 中声明，实现在 `arch/x86_64/task_arch.c`，被 `task_init` 调用

**注意**:
- `mutex.c` 中的 `atomic_cas`/`atomic_write` 调用，在将 `#include <kernel/arch/x86_64/cpu.h>` 改为 `#include <kernel/arch/atomic.h>` 后，需同步改为 `arch_atomic_cas`/`arch_atomic_write`（来自 `arch/atomic.h`）
- `futex.c` 的 `#include <kernel/arch/x86_64/cpu.h>` 是**无用包含**——futex.c 不调用任何 `atomic_*` 函数，直接删除该 include 即可

---

## 4. 文件清单

### 4.1 新增文件

| 文件 | 内容 | 类型 |
|------|------|------|
| `kernel/include/kernel/arch/spinlock.h` | `#ifdef` dispatch 头文件 | 新增 |
| `kernel/include/kernel/arch/gate.h` | `#ifdef` dispatch 头文件（set_tss64） | 新增 |
| `kernel/include/kernel/arch/aarch64/cpu.h` | aarch64 cpu 操作内联 | 新增 |
| `kernel/include/kernel/arch/aarch64/irq.h` | aarch64 中断控制内联 | 新增 |
| `kernel/include/kernel/arch/aarch64/mmu.h` | aarch64 TLB/MMU 内联 | 新增 |
| `kernel/include/kernel/arch/aarch64/atomic.h` | aarch64 原子操作内联 | 新增 |
| `kernel/include/kernel/arch/aarch64/thread.h` | aarch64 pt_regs_t | 新增 |
| `kernel/include/kernel/arch/aarch64/cache.h` | aarch64 dcache 操作 | 新增 |
| `kernel/include/kernel/arch/aarch64/spinlock.h` | aarch64 spinlock 桩（`#error` 直到实现） | 新增 |
| `kernel/arch/aarch64/` | aarch64 架构目录 | 新建空目录 |
| `kernel/arch/aarch64/make.config` | aarch64 构建配置 | 新增 |
| `kernel/arch/x86_64/switch.c` | `__switch_to` 移出（task.c 拆分） | 新增 |
| `kernel/arch/x86_64/thread_entry.S` | 线程入口 asm（task.c 拆分） | 新增 |
| `kernel/arch/x86_64/task_arch.c` | `arch_task_init_platform()`（task.c 拆分） | 新增 |

### 4.2 修改文件

#### 头文件

| 文件 | 变更 |
|------|------|
| `kernel/include/kernel/memory.h` | 删除 `get_cr3()` 内联；`PAGE_OFFSET` → `ARCH_PAGE_OFFSET` |
| `kernel/include/kernel/arch/mmu.h` | x86_64 分支新增 `arch_get_page_table()`、`ARCH_PAGE_OFFSET`；替换 `arch_virt_to_phys` 内三处字面量为 `ARCH_PAGE_OFFSET` |
| `kernel/include/kernel/arch/cpu.h` | aarch64 分支实现 |
| `kernel/include/kernel/arch/irq.h` | aarch64 分支实现 |
| `kernel/include/kernel/arch/atomic.h` | aarch64 分支实现 |
| `kernel/include/kernel/arch/thread.h` | aarch64 分支实现；x86_64 分支声明 `arch_switch_to`、`arch_task_init_platform` |
| `kernel/include/kernel/arch/cache.h` | aarch64 分支实现 |
| `kernel/include/kernel/arch/segment.h` | aarch64 分支实现（全 0） |
| `kernel/include/kernel/arch/spinlock.h` | spinlock dispatch 头文件（新建，见 4.1） |
| `kernel/include/kernel/arch/gate.h` | gate dispatch 头文件（新建，见 4.1） |
| `kernel/include/kernel/file.h` | `arch/x86_64/spinlock.h` → `arch/spinlock.h` |
| `kernel/include/kernel/printk.h` | 同上 |
| `kernel/include/kernel/tty.h` | 同上 |
| `kernel/include/kernel/wait.h` | 同上 |
| `kernel/include/fs/ext2.h` | 同上（v1 遗漏） |
| `kernel/include/kernel/interrupt.h` | linkage.h 保持引用不变（该文件不含 spinlock.h，无需 spinlock 迁移） |
| `kernel/include/kernel/task.h` | 无需变更 — 已使用 `arch/cpu.h`、`arch/thread.h`、`arch/segment.h` dispatch 头文件；linkage.h 保持引用不变；spinlock 通过 `file.h` 间接获取 |
| `kernel/include/kernel/arch/x86_64/linkage.h` | 删除 `asmlinkage` 定义 |

#### .c 文件

| 文件 | 变更 |
|------|------|
| `kernel/kernel/main.c` | `arch/x86_64/spinlock.h` → `arch/spinlock.h`；`arch/x86_64/cpu.h` → `arch/cpu.h`；其余 `arch/x86_64/*` 引用保持（入口文件，原则 4 豁免） |
| `kernel/kernel/log.c` | `arch/x86_64/spinlock.h` → `arch/spinlock.h` |
| `kernel/kernel/printk.c` | 同上 |
| `kernel/memory/pmm.c` | 同上 |
| `kernel/driver/keyboard.c` | 同上 |
| `kernel/sched/deferred_free.c` | 同上（v1 遗漏） |
| `kernel/test/selftest.c` | 同上 |
| `kernel/mutex.c` | `arch/x86_64/cpu.h` → `arch/atomic.h`；`atomic_cas` → `arch_atomic_cas`；`atomic_write` → `arch_atomic_write`（v1 遗漏 — 只需 atomic，不需 cpu dispatch） |
| `kernel/futex.c` | `arch/x86_64/cpu.h` → 删除（无用包含 — futex.c 不调用任何 atomic_* 函数；spinlock 通过 wait.h 间接获取）（v1 遗漏） |
| `kernel/sched/task.c` | spinlock.h → `arch/spinlock.h`；拆分：`__switch_to` → `arch/x86_64/switch.c`，线程入口 asm → `arch/x86_64/thread_entry.S`，`task_init` 的 x86 init → `arch/x86_64/task_arch.c` |

#### test 镜像

| 文件 | 变更 |
|------|------|
| `test/include/kernel/file.h` | `arch/x86_64/spinlock.h` → `arch/spinlock.h` |
| `test/include/kernel/printk.h` | 同上 |
| `test/include/kernel/interrupt.h` | `arch/x86_64/regs.h` → `arch/thread.h`；linkage.h 保持（该文件不含 spinlock.h，无需 spinlock 迁移） |
| `test/include/kernel/task.h` | `arch/x86_64/cpu.h` → `arch/cpu.h`；`arch/x86_64/regs.h` → `arch/thread.h`；linkage.h 保持（该文件不含 spinlock.h，无需 spinlock 迁移） |
| `test/include/kernel/trace.h` | `arch/x86_64/regs.h` → `arch/thread.h` |

#### 构建系统

| 文件 | 变更 |
|------|------|
| `kernel/Makefile` | `HOSTARCH` → `ARCH ?= x86_64`；新增 `OBJFORMAT`；:164 `elf64-x86-64` → `$(OBJFORMAT)` |
| `kernel/arch/x86_64/make.config` | 无变更 |

### 4.3 不需要变的文件（明确排除）

| 文件/目录 | 原因 |
|-----------|------|
| `kernel/intr/apic/`, `kernel/intr/pic/`, `kernel/pit.c` | 原则 4 豁免 |
| `kernel/sched/smp.c` | 原则 4 豁免 |
| `kernel/kernel/main.c`（trap.h, gate.h, asm.h, trampoline.h 引用） | 原则 4 豁免（入口文件 = 纯 x86 启动序列） |
| `kernel/arch/x86_64/` 内所有文件 | 这些是 x86 的实现，不需要动 |
| `kernel/intr/pic/8259A.c:6` | arch 内部代码，豁免 |
| `kernel/kernel/hang.c:7` | hang 使用 pt_regs 但仅 x86 路径调用，豁免 |

---

## 5. 验证策略

| 阶段 | 验证命令 | 预期 |
|------|---------|------|
| 每个 Task 后 | `make clean && make 2>&1 \| grep -c warning` | 0 |
| 每个 Task 后 | `make test` | test 编译通过 |
| Phase 4 | `make test-syscall` | 70/70 PASS |
| Phase 4 | `make run` (5s timeout → Ctrl-C) | 启动到 shell |
| aarch64 编译 | `make ARCH=aarch64 clean && make ARCH=aarch64 2>&1` | 编译到链接期（链接器报 _start 未定义，因为没有 aarch64 head.S） |
