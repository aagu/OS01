# 架构评审 — Group 4: 调度器

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/sched/task.c`, `deferred_free.c`, `kernel/arch/x86_64/switch.c`, `kernel/include/kernel/task.h`, `kernel/include/kernel/deferred_free.h`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P0 | fork | `fork_mm_copy` OOM fallback 让父子进程共享 PDE，写入互相影响 | 已修复 |
| 2 | P0 | sched | 全局任务列表 `list_add_to_before` 在 SMP 下无锁 | 已修复 |
| 3 | P1 | exec | `sys_exec` 不释放旧进程页表页（`vmm_free_user_map` 因 fork 共享跳过的已知问题） | 待处理 |
| 4 | P1 | sched | zombie reaper 每个 `schedule()` 调用都 O(n) 扫描全局任务列表 | 待处理 |
| 5 | P1 | sched | `deferred_free` OOM 时静默泄漏 ptr | 待处理 |
| 6 | P1 | blocker | `blocker_wait` 设置 `TASK_INTERRUPTIBLE` 和条件检查之间缺少内存屏障 | 待处理 |
| 7 | P1 | sched | `sys_exec` POSIX 标准：exec 不应重置 SIG_IGN 信号 | 待处理 |
| 8 | P2 | sched | DF reaper kthread 固定到 CPU 0 | 待处理 |
| 9 | P2 | sched | FPU 每次上下文切换都 eager save/restore（不使用 CR0.TS 惰性优化） | 待处理 |
| 10 | P2 | sched | zombie reaper 每次最多回收 64 个 zombie | 待处理 |
| 11 | P2 | sched | `__switch_to` GS base 修复：注释只说了不动 GS selector，未处理 `swapgs` | 待处理 |

---

### [P0] 1. `fork_mm_copy` OOM fallback 共享页表

- **位置**: `kernel/sched/task.c:1122-1131, 1162-1177`
- **现象**: 当 `calloc(child_pte)` 或 `alloc_pages` 失败时，子进程直接复用父进程的 PML2E（OOM fallback）：
  ```c
  // 4KB PTE 表 OOM: child_pml2[l2] = pml2e;  // OOM: share PDE
  ```
  父子进程的页表项指向**同一个页表页**。此后任一方对用户页的写入都会影响另一方。
- **风险**: 安全隐患（父子进程互相修改内存）+ 稳定性问题（COW 预期失效）
- **建议**: 去掉静默 fallback，让 `fork_mm_copy` 失败返回 NULL，让 `do_fork` 处理 ENOMEM
- **修复**: `c14d2a3` — 添加 `fail` 标签遍历 `child_pml4` 释放已分配 PML3/PML2/PTE 页。失败时返回 NULL，`do_fork` 处理 ENOMEM。不再静默共享父页表。

### ~~[P0] 2. `fork_mm_copy` 错误使用 `kfree` 释放页表页~~  FALSE ALARM

- **位置**: `kernel/sched/task.c:1196`
- **原始审查**: `child_pml4` 通过 `vmm_alloc_map()` 分配，错误路径使用 `kfree(child_pml4)` 释放——认为 `kfree` 用于 slab 分配，页表页应用 `kfree_4k`/`free_pages`
- **复核结论**: **FALSE ALARM**。`vmm_alloc_map()` → `calloc(1, PAGE_4K_SIZE)` → `malloc(4096)` → `kmalloc(4096)`，返回 slab 分配器管理的指针。`kfree()` 是释放 `kmalloc` 内存的正确方式。
- **修正**: 此条应标记为 FALSE ALARM，不纳入问题清单。

### [P0] 2. 全局任务列表 SMP 无锁

- **位置**: `kernel/sched/task.c` 多处
- **现象**: `list_add_to_before(&init_task_union.task.list, &tsk->list)` 在 `do_fork` (1255) 和 `spawn_user_task` (731) 中无锁调用。`sched_unblock_blocked` (188) 和 `do_waitpid` (529, 563) 无锁遍历列表。SMP 下并发 fork 会破坏链表
- **建议**: 添加 `task_list_lock` spinlock，在写入侧和读取侧均加锁保护
- **修复**: `c14d2a3` — 添加全局 `task_list_lock`，保护 `list_add_to_before`（spawn_user_task/do_fork）、`list_del`（do_exit）、遍历（sched_unblock_blocked/do_waitpid/SYS_kill/zombie reaper）。替换 zombie reaper 中原 `reap_lock`。

### [P1] 3. `sys_exec` 泄漏旧进程页表

- **位置**: `kernel/sched/task.c:1021-1032`
- **现象**: `sys_exec` 调用 `vma_free_all` 释放 VMA 页、`kfree(current->mm)`，但**不释放旧页表的 PML4/PDPT/PD/PT 页面**。代码注释承认因为 fork 共享页表，不能调 `vmm_free_user_map`。这意味着每次 exec 泄漏一个完整页表层次结构
- **建议**: 在 exec 前检查 `current->mm` 是否被其他 task 共享。如果唯一拥有者，调用 `vmm_free_user_map`；否则减小引用计数

### [P1] 4. Zombie reaper 每 tick O(n) 全量扫描

- **位置**: `kernel/sched/task.c:319-353`
- **现象**: 每次 `schedule()`（每 10ms/tick）都遍历全局任务列表查找 zombie。100Hz 下，100 个任务意味着每秒 100 × 100 = 10000 次空遍历
- **建议**: 维护 zombie 队列（exit 时加入，reaper 从队列取），避免全量扫描

### [P1] 5. `deferred_free` OOM 静默泄漏

- **位置**: `kernel/sched/deferred_free.c:35-36`
- **现象**: `kmalloc(sizeof(*w))` 失败时直接 `return`，调用者的待释放指针被静默泄漏
- **建议**: 预留 emergency work item 或 fallback 到同步释放

### [P1] 6. `blocker_wait` 缺少内存屏障

- **位置**: `kernel/sched/task.c:250`
- **现象**:
  ```c
  self->state = TASK_INTERRUPTIBLE;       // 250
  if (check(self)) { ... }                // 255  双检
  schedule();                              // 265
  ```
  250 的 store 和 255 的 load 之间没有内存屏障。在 x86_64 上 store-buffer 可能让另一个 CPU 看到 `state == RUNNING`，而本 CPU 看到 `state == INTERRUPTIBLE`。x86 TSO 下 store 对其他 CPU 可见需要 mfence 或 locked 指令。
- **建议**: 在 state store 后加上 `__asm__ __volatile__("mfence" ::: "memory")`，或使用 `atomic_store`/`atomic_load` 带 `__ATOMIC_SEQ_CST`

### [P1] 7. `sys_exec` POSIX 信号处理合规性

- **位置**: `kernel/sched/task.c:1045-1046`
- **现象**: `for (int sig = 1; sig < NSIG; sig++) current->sighand[sig].sa_handler = SIG_DFL;`
  POSIX 要求 `exec()` 后 SIG_IGN 信号应保持 SIG_IGN。当前代码将所有信号重置为 SIG_DFL。注释记录这是一个已知的 shell-child-signal-inheritance 问题的实用修复
- **建议**: 考虑将 SIG_IGN 判断加入重置逻辑，或明确记录为已知偏差

### [P2] 8. DF reaper 固定到 CPU 0

- **位置**: `kernel/sched/task.c:1414`
- **现象**: `deferred_free_spawn` 后设置 `df->cpu = 0`，所有 `deferred_kfree` 只能由 CPU 0 处理。其他 CPU 无法在本地回收
- **建议**: 每个 CPU 分配一个 DF reaper，或允许 DF reaper 迁移

### [P2] 9. FPU 上下文切换 eager save/restore

- **位置**: `kernel/arch/x86_64/switch.c:44-59`
- **现象**: `__switch_to` 在每个上下文切换中执行 `clts; fxsave64/fxrstor64`，从不设置 CR0.TS。失去惰性 FPU 优化的性能收益
- **建议**: 保留 CR0.TS 标志，在 #NM handler 中惰性恢复 FPU 状态。或确认对性能影响可接受后保持当前行为

### [P2] 10. Zombie reaper 回收上限 64

- **位置**: `kernel/sched/task.c:315, 320`
- **现象**: `reap_list[64]` 栈数组，每次最多回收 64 个 zombie。大规模进程退出可能需要多轮
- **建议**: 用动态分配或链表接收集合的 zombie

### [P2] 11. `__switch_to` GS base 注释

- **位置**: `kernel/arch/x86_64/switch.c:29-33`
- **现象**: 注释正确指出 GS base 通过 MSR 设置一次后不应再加载 GS selector。但未提及 `swapgs` 指令——`ret_from_intr`/`syscall_entry` 在进入 C 前执行 `swapgs` 切换 GS base。注释应为每 CPU 的 GS base 提供 swapgs 策略说明
- **建议**: 补充注释说明 swapgs 在 entry.S 中的使用，以及为何上下文切换中不涉及
