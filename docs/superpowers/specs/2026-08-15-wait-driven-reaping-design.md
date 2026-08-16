# wait 驱动回收 — 移除调度器与 reaper 双重职责

> **Roadmap**: P0 #3「任务退出/回收收敛 — wait 驱动回收，移除调度器与 reaper 双重职责」
> **决策记录**: roadmap.md #30（原「候选，暂缓」，现触发实施）
> **触发条件**: 满足 `docs/scheduler-complexity.md` §5 触发器 #2「频繁改任务退出/父子语义」
> **日期**: 2026-08-15
> **基线**: `840f3a5`

## 1. 问题

任务回收当前由**三方**协作完成，职责割裂、异步：

1. **`do_waitpid`**（`task.c:958`）— 只做「逻辑收割」：找到 ZOMBIE 子进程、写 exit status、置 `PF_REAPED`。**不释放任何内存**。
2. **`schedule()` 内的 zombie reaper**（`task.c:551-643`）— 每次调度都扫描全局 task list，对 `PF_REAPED` / `PF_KTHREAD` / 孤儿 / 父已亡 的 ZOMBIE 做物理 `list_del` + `deferred_kfree(thread/fpu_save/stack)`。
3. **`deferred_free` df-kthread**（`deferred_free.c`）— 一个常驻 kthread + spinlock 队列，**真正执行** `kfree`/`files_free`（异步），因为 `schedule()` 不能释放自己可能正跑在上面的栈。

这正是 `scheduler-complexity.md` §2 认定的「最大 bug 面」——Round-5 的 4 层 double-book 竞态（commit `f58d1a1`）全部围绕这个异步回收设计展开。

**目标**：把回收收敛为两条清晰路径——用户任务由 `waitpid` 同步收割，kthread 自收割——彻底删除 reaper 扫描 + df-kthread 队列。

## 2. 关键前置事实（已核实）

- **`do_exit` 已在自身路径内 detach fd 表**（`task.c:863` `files_unpin(fs)`）并释放 `mm`（`task.c:842-850`）。所以 reaper 唯一剩余的工作就是释放 `thread`/`fpu_save`/`stack_alloc_base` 三个纯 slab 对象。
- 删掉 reaper 后，子进程的**最后一次 `schedule()` 不再遍历全局 task list**（`schedule()` step 2 只碰 per-CPU rq；`sched_unblock_blocked` 扫描的是「阻塞态」任务，不是 ZOMBIE）。这消除了「final schedule 期间不能碰 `child->list`」这一迫使 defer 的约束。
- `__switch_to` 已经在 `next` 的栈上执行（`switch_to` 宏 `movq %2,%rsp` 之后才 `jmp __switch_to`），并且 `prev` 离开 CPU 后 `on_cpu=0` 的 RELEASE store 是其最后一步——**`__switch_to` 的 epilogue 天然就是「off-stack 上下文」**，无需另起 exit 栈。

## 3. 架构

### 3.1 用户任务 → wait 驱动（`do_waitpid` 直接释放）

`do_waitpid` 成为物理收割者。找到匹配的 ZOMBIE 子进程后：

1. 持 `task_list_lock` 扫描：`parent==current && state==ZOMBIE && (pid==-1||pid==子)`。
2. 用 ACQUIRE load 读 `child->on_cpu`，**必须 == 0** 才能收割（否则子进程仍在 final schedule，栈在用）。
3. `on_cpu==0` 时：`list_del(&child->list)`、读 `exit_code` **与 `child->pid`**（`child->pid` 是返回值，必须先捕获——step 4 的 `kfree(child->stack_alloc_base)` 会释放含 `task_t` 的 `task_union`），释放锁。
4. 锁外同步 `kfree(child->thread)`、`kfree(child->fpu_save)`、`kfree(child->stack_alloc_base)`。
5. 删除 `PF_REAPED` 标志——收割即释放、即离表，天然不会二次收割。

**「ZOMBIE 但 `on_cpu==1`」分支**（避免二义实现）：子进程已 ZOMBIE 但仍在 final schedule（栈在用）时，**既不能收割、也不能返回 `-ECHILD`**。必须视为「存在但未到可收割态」：落入 `child_exists` 检查后继续 `blocker_wait`（`WNOHANG` 时返回 0）。判断优先级——先 `ZOMBIE && on_cpu==0`（收割）→ 再 `ZOMBIE` 或 `RUNNING`（存在但不可收割，继续等）→ 都不匹配才 `-ECHILD`。

**`waitpid_should_unblock` 扫描语义**（实现勿漏）：condition 回调遍历子任务时，遇「ZOMBIE 但 `on_cpu==1`」的子任务**必须 `continue` 继续扫下一个**，不能立即 `return false`——否则 `pid==-1` 等待任意子进程时，先撞见一个未到可收割态的子，即使另一个已 `ZOMBIE && on_cpu==0` 的子就绪，父进程也被多压 1 个 tick。

`on_cpu==0` 门是安全性的基石：`__switch_to` 已用 RELEASE store 置 `on_cpu=0`，父进程 ACQUIRE 读到 0 即证明子栈已彻底离开 CPU，且其 `exit_code`/指针写均可见。

### 3.2 kthread → 自收割（`__switch_to` epilogue）

kthread 没有 `waitpid` 消费者，故自释放：

- **`do_exit` kthread 分支**（在 `files_unpin` + `on_cpu=1` 之后，跳过 direct-switch-to-parent 块）：**`list_del`、置 `PF_SELF_REAP`、置 `state=ZOMBIE` 三步必须在同一个 `spin_lock_irqsave(&task_list_lock)` 临界区内原子完成**（IRQ 关闭），解锁后 `schedule()`：

  ```c
  uint64_t fl = spin_lock_irqsave(&task_list_lock);
  list_del(&current->list);
  current->flags |= PF_SELF_REAP;
  current->state  = TASK_ZOMBIE;
  spin_unlock_irqrestore(&task_list_lock, fl);
  schedule();  // switch_to → __switch_to epilogue 自收割
  ```

  **为什么必须原子**（tick 竞态，v2 评审发现的真实 UAF）：`do_exit` 从 syscall/signal 上下文进入，此时 IRQ 已开。若三步非原子，tick 可能落在任意两步之间触发 `schedule()`。最坏情形——`PF_SELF_REAP` 已置位而 `state` 仍是 `TASK_RUNNING`：tick 的 `schedule()` step 2 见 RUNNING 会把任务**重新 enqueue** 再切走，`__switch_to` 却已 `kfree` 掉一个仍在 runqueue 上、RUNNING 的任务 → 后续 `pick_eevdf` 命中已释放的 `rb_node` → UAF。反之（`state=ZOMBIE` 先于 `PF_SELF_REAP`）则 tick 切走后 epilogue 见 `PF_SELF_REAP` 未置位、不 `kfree`，任务已出 list/出 rq 且不再被调度 → 永久泄漏。原子化后，解锁时任务已「离表 + ZOMBIE + PF_SELF_REAP」三者齐备，此后任何 tick 的 `schedule()` 都正确切走并收割。

- **`__switch_to` epilogue**（在 `on_cpu=0` store 之后）：
  ```c
  if (prev->flags & PF_SELF_REAP) {
      if (prev->thread)           kfree(prev->thread);
      if (prev->fpu_save)         kfree(prev->fpu_save);
      if (prev->stack_alloc_base) kfree(prev->stack_alloc_base);
      // 注：kfree(prev->stack_alloc_base) 已释放含 prev 的 task_union，
      // 此后 epilogue 不得再解引用 prev 任何字段（见 §7 不变式 4）。
  }
  ```

此方案是 `scheduler-complexity.md` §5「per-CPU exit stack + self-kfree」的**等价简化实现**：`__switch_to` 已跑在 next 栈上，`current`（=next）全程合法，不需要新增 RSP 切换汇编、不需要 `switch_to_exit` 变体、不需要 per-CPU 静态缓冲区。仅比原 `__switch_to` 多一段 `if`。

### 3.3 孤儿 → reparent 到 init（基本沿用现有）

- 现有 `do_exit` 已将临终任务的孩子 reparent 到 `user_init_task`（`task.c:810-823`），覆盖孤儿场景。
- 新增一处防御性自 reparent：**用户任务**若其 `parent` 为 `PF_KTHREAD` 或 `NULL`，`do_exit` 将 `self->parent = user_init_task`（否则成永久 ZOMBIE 泄漏）。kthread 跳过（自收割）。
- init 的 `reap_children()` 监督循环（`init.c:120`，`waitpid(-1,WNOHANG)`）在 ~100ms 内收割被 reparent 的孤儿。

### 3.4 唤醒路径（保留 `sched_unblock_blocked`）

`do_exit` 仍先 `blocker_wake(parent)`（快路径），但父进程可能过早醒来（子仍 `on_cpu==1`）→ 重新阻塞。此时**唯一**的重唤醒来源是 `schedule()` 内的 `sched_unblock_blocked()`（每个 tick 在每 CPU 上运行一次），它重新评估父的 condition。因此：

- `sched_unblock_blocked()` **必须保留**——它不是 reaper。从 reaper 块中拆出，放入独立的 `task_list_lock` 临界区。
- `waitpid_should_unblock` 的条件从「子 ZOMBIE」收紧为「子 ZOMBIE **且** `on_cpu==0`」，保证 blocker 只在「真可收割」时 latch。

代价：跨 CPU 收割相比现在的 reaper 多至 1 个 tick（10ms）延迟，可接受（当前 reaper 也有同类延迟）。

## 4. 被删除的组件

| 组件 | 位置 |
|------|------|
| `schedule()` zombie reaper 块（扫描 + reap list + files 收集 + `deferred_kfree`） | `task.c:551-643` |
| `deferred_free.c` 整个文件（df-kthread、队列、spinlock） | `kernel/sched/deferred_free.c` |
| `deferred_free.h` 整个文件 | `kernel/include/kernel/deferred_free.h` |
| `BLOCKER_DEFERRED_FREE` | `task.h:17` |
| `deferred_free_spawn()` 调用 | `task.c:1952` |
| `PF_REAPED` 标志 | `task.h:68` + 全部 6 处用法 |
| `files_unpin` 的延迟释放路径 | `file.c:290` |

## 5. 保留的组件（关键，勿误删）

- **`on_cpu` 机制**（`task.h:173`、`schedule()`/`task_wake`/`__switch_to` 的 ACQUIRE-RELEASE）——仍是 `task_wake` double-book 门 + 新 `do_waitpid` 收割门。
- **`sched_unblock_blocked()`**（`task.c:284`）——blocker 唤醒兜底，见 §3.4。
- **`task_send_signal` / `task_files_pin_by_pid`** —— 按 pid 在 list 上查找；kthread `list_del` 后自动不可达，无需改。
- **`do_exit` 的 reparent + SIGCHLD + direct-switch-to-parent**（用户任务路径）。

## 6. 行为变更：`files_unpin` 同步化

删除 `deferred_free` 后，`files_unpin` 的 drop-to-zero 路径从「`deferred_files_free`」改为直接 `files_free`。

**安全性（已核实）**：所有真实调用点都在释放锁之后调用 `files_unpin`（`file.c:126-128` 的既有约束注释），而 `files_free → file_put → file_free` 内部自带锁（pipe/pty/socket cleanup、`netconn_delete`）。同步 `files_free` 的代码路径**已存在**（`deferred_files_free` 的 OOM fallback 即同步执行），但该路径罕见且无测试覆盖——其正确性现由 §8 新增的 socket-exit 自测**显式验证**，而非仅靠既有代码推断。

**影响面（阻塞语义，非仅锁序）**：`file_free` 的 socket 清理 `netconn_delete`（`api_lib.c` → `netconn_prepare_delete` → `netconn_apimsg` → `tcpip_send_msg_wait_sem`）在 `LWIP_TCPIP_CORE_LOCKING=0` + `NO_SYS=0` 下是**阻塞调用**：`sys_mbox_post(&tcpip_mbox,…)` 后 `sys_arch_sem_wait(sem,0)` 等待 `tcpip_thread` 处理完 delete 消息。因此**带打开 socket 的任务 exit(2) 会在 `do_exit` 的 `files_unpin` 处阻塞一轮 lwIP 消息往返**，这是退出路径新增的 liveness 依赖——锁序无反转，但「do_exit 依赖 tcpip_thread 前进」。若 tcpip_thread 被饿死/卡死，exit 挂起（df-kthread 时代该阻塞与退出任务解耦）。

时序安全：该阻塞发生在 `on_cpu=1`（`task.c:868`）与 `ZOMBIE`（`task.c:918`）置位**之前**，此时任务仍是普通 RUNNING，可阻塞、可被再调度，无自旋。kthread 分支同理（`files_unpin` 在 `list_del` 之前）。

**测试缺口**：该路径当前零覆盖（systest 无 socket 测试，`-smp 2` 跑不到 `netconn_delete`-from-do_exit）。§8 补一条显式 socket-exit 测试。

## 7. 不变式（写进实现计划，逐条验证）

1. **收割时 prev 不可达，且三步原子**：kthread 的 `list_del` + 置 `PF_SELF_REAP` + 置 `ZOMBIE` 在**同一个 `spin_lock_irqsave(&task_list_lock)` 临界区内原子完成**（IRQ 关，见 §3.2 的 tick 竞态论证）。解锁时任务已离表 + ZOMBIE + PF_SELF_REAP 三者齐备，此后全局 list 扫描（`sched_unblock_blocked`/`task_send_signal`/`task_files_pin_by_pid`）找不到它，且任何 tick 触发的 `schedule()` 都不会把它重新 enqueue（state≠RUNNING）。
2. **`on_cpu=0` RELEASE store 先于 `kfree`**：`kfree` 是不透明调用，编译器不会把它重排到原子 store 之前；`__ATOMIC_RELEASE` 自带 barrier。
3. **`cli` 覆盖整段收割**：`__switch_to` 全程 IRQ 关闭（`switch_to` 宏 `cli` 起，`ret` 到 `next->rip` 后由调用方恢复），tick 无法在收割中途切入嵌套 schedule。
4. **`__switch_to` 里 `prev->thread` 先写后 free**：`switch_to` 的 asm 先把 rsp/rip 存进 `prev->thread`，`__switch_to` 在 epilogue 前最后一次读 `prev->thread`（fs/cr3，`switch.c:65-70`）与 `prev->fpu_save`（`switch.c:77-84`），之后才 `kfree`——写-then-free，无读后失效。
   - **注释防回归**：`kfree(prev->stack_alloc_base)` 释放的是包含 `prev` 自身的 `task_union`（`raw_alloc = task_union + STACK`，`tsk` 对齐在其内部）。`kfree` 后 epilogue 不得再触碰 `prev` 任何字段——这是靠构造保证的（`ret` 弹 `next->rip`，`rsp` 早已切到 next 栈），实现时加显式注释，防止未来改动在 `kfree` 之后新增对 `prev` 的解引用。
5. **epilogue 只碰三个纯 slab 对象**（`thread`/`fpu_save`/`stack_alloc_base`）。纠正：kthread 的 `mm` 是**共享的、从不 free**（`do_fork` 里 `tsk->mm = current->mm`；`do_exit:842` 对 `PF_KTHREAD` 明确跳过 `mm` 释放）；且 kthread 经 `do_fork:1747` `files_dup` 继承 fd 表，若继承到 socket，`do_exit` 的 `files_unpin` **同样会走进 `netconn_delete` 阻塞**。结论「epilogue 不碰 files/mm、不会在 epilogue 内走到 `file_free`/`netconn_delete`」仍成立——因为 epilogue 在 `files_unpin` **之后**执行，且自收割只 `kfree` 那三个对象——但「mm 已 free」「不会走到 netconn_delete」的表述是错的，特此更正。

## 8. 测试策略

| 层 | 测试 | 说明 |
|----|------|------|
| 内核自测 | 重写 `test_deferred_free.c` → `test_kthread_self_reap` | 创建 kthread → `do_exit(0)` → 验证 `PMMngr` 空闲页回基线（栈已回收） |
| 内核自测 | **新增 `test_socket_exit_reap`** | 任务打开一个 socket（或 netconn）后 `do_exit`，断言退出完成（不挂起）且 `PMMngr` 空闲页回基线——覆盖 §6 的 `netconn_delete` 阻塞路径 |
| 内核自测 | `test_fd_refcount.c` **不变** | pin/unpin 协议未改，仅延迟释放消失 |
| 系统测试 | `systest` fork+exec+waitpid 组（126 项） | 覆盖 wait 驱动路径 |
| 系统测试 | `systest` 增 socket open→close→exit 断言 | 用户态路径的 socket-exit 阻塞覆盖（内核自测若难构造 netconn，用此替代/补充） |
| 系统测试 | `-smp 2` 全量 | 跨 CPU `on_cpu` 竞态（父在 CPU0、子在 CPU1 退出） |
| 回归 | 36/36 gtk+stdio 基线 | `scheduler-complexity.md` 的既有验证基线 |

新增一个 systest 断言：**孤儿 reparent** —— 父进程先退出，孙进程成为 init 孤儿，init 的 `reap_children` 收割之（验证 §3.3 防御性 reparent）。

## 9. 文件变更清单

| 类别 | 文件 | 说明 |
|------|------|------|
| **修改** | `kernel/sched/task.c` | 删 reaper 块；`do_exit` 增 kthread 自收割分支；重写 `do_waitpid` 直接释放；`waitpid_should_unblock` 加 `on_cpu==0`；删 `deferred_free_spawn` 调用 + include；删 `PF_REAPED` 用法；`sched_unblock_blocked` 移出 reaper 块 |
| **修改** | `kernel/arch/x86_64/switch.c` | `__switch_to` epilogue 增 `PF_SELF_REAP` 自收割 |
| **修改** | `kernel/include/kernel/task.h` | 删 `PF_REAPED`、`BLOCKER_DEFERRED_FREE`；增 `PF_SELF_REAP` |
| **修改** | `kernel/fs/file.c` | `files_unpin` → 同步 `files_free`；删 `deferred_free.h` include |
| **删除** | `kernel/sched/deferred_free.c` | 整文件 |
| **删除** | `kernel/include/kernel/deferred_free.h` | 整文件 |
| **修改** | `kernel/test/test_deferred_free.c` | 重写为 kthread 自收割测试 |
| **修改** | `kernel/test/test_fd_refcount.c` | 仅更新头部注释（line 3「AFTER deferred_free_spawn() / files_unpin can defer-free」已过期；测试逻辑本身不变——harness 在两名 worker done 后才 drop 最后一 ref） |
| **修改** | `user/systest.c` | 增孤儿 reparent 断言 + socket open→close→exit 断言 |
| **构建** | wildcard 自动发现 | 删 `deferred_free.c` 即脱离编译；`make clean` 强制（无 header 依赖） |

## 10. 明确不做（YAGNI）

- **不做** 字面的 per-CPU exit 栈 + `switch_to_exit` 汇编（`__switch_to` epilogue 已等价覆盖，见 §3.2）。
- **不做** exit_group / 进程组（本迭代范围外，roadmap 后续）。
- **不做** RT 优先级 / EEVDF 降级（`scheduler-complexity.md` §5 触发器 #1，独立于本项）。
- **不保留** df-kthread 作为「最小 kthread reaper」兜底——kthread 已自收割，无需第二回收方。

## 11. 语义变更记录（评审补充，非阻塞）

- **`waitpid` 对 kthread 子任务的语义变化**：改动前父任务 `waitpid(-1)` 能收割 `PF_KTHREAD` 的 ZOMBIE 子任务（其留在 list 上）；改动后 kthread `list_del` 后即从 list 消失，父任务将得到 `-ECHILD`。代码库当前无依赖收割 kthread 的 `waitpid`（init 的 `reap_children` 只碰 reparent 孤儿），实测无害，但实现计划须记此行为差异。
- **唤醒完全 tick 驱动**：`do_exit:835` 的 fast-path `blocker_wake` 发生在 `ZOMBIE` 置位（`task.c:918`）**之前**，条件不满足、不会提前唤醒；父任务唤醒只能靠 `sched_unblock_blocked`，成本固定 1 tick（≤10ms）。§3.4 已承认，实现时确认无丢失唤醒。
- **`sched_unblock_blocked` 内 `on_cpu` 读必须为原子/ACQUIRE**：condition 回调里用 plain 读可能读到 stale `on_cpu==1`，只推迟唤醒（liveness、可接受）；真正收割时 `do_waitpid` 用 ACQUIRE（correctness）。§3.1 step 2 已写，实现勿漏。
