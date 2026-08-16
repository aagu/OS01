# wait 驱动回收 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把任务回收从「调度器 reaper 扫描 + df-kthread 异步释放」收敛为「用户任务由 waitpid 同步收割、kthread 由 __switch_to epilogue 自收割」，彻底删除 `deferred_free` 与 `schedule()` 内的 zombie reaper。

**Architecture:** 用户任务退出后由父进程 `do_waitpid` 直接 `list_del` + `kfree`（门控 `on_cpu==0`）；kthread 在 `do_exit` 里原子地 `list_del` + 置 `PF_SELF_REAP` + 置 `ZOMBIE`，由 `__switch_to` 在 next 栈上释放 `thread/fpu_save/stack`。孤儿经现有 reparent 到 init 兜底。

**Tech Stack:** C（x86_64 kernel），EEVDF 调度器，slab 分配器，自旋锁，QEMU（`-smp 2`）验证。

**Spec:** `docs/superpowers/specs/2026-08-15-wait-driven-reaping-design.md`

## Global Constraints

- **`make clean` 强制**：删除 `.c` 文件后必须 `make clean`（Makefile 无 header 依赖追踪，wildcard 自动发现源文件；见 `AGENTS.md` gotcha #2）。
- **不改任何 struct 布局**：本次只增删 `#define` 标志位（`PF_SELF_REAP`/`PF_REAPED`）与 blocker 常量，**不触碰 `task_t`/`thread_t`/`files_t` 字段**，因此无 `sizeof` ABI 漂移风险。
- **`on_cpu` 读写必须是原子**：写用 `__atomic_store_n(..., __ATOMIC_RELEASE)`，读用 `__atomic_load_n(..., __ATOMIC_ACQUIRE)`——`task.h:173` 的既有协议，新代码照抄（`AGENTS.md` gotcha 提到 `on_rq/on_cpu` 非原子访问 = lost update）。
- **收割只 `kfree` 三个纯 slab 对象**：`thread`、`fpu_save`、`stack_alloc_base`。绝不在此路径碰 `files`/`mm`（它们在 `do_exit` 前段已处理）。
- **`kfree` 返回 `size_t`**（`slab.h:38`），签名是 `size_t kfree(void*)`，不是 `void`。
- 构建 selftest 用 `make KERNEL_SELFTEST=1 ...`；systest 用 `make OS01_SYSTEST=1 test-syscall`（顶层 flag 不可省，`AGENTS.md` gotcha #3）。

---

## 文件结构

| 文件 | 职责 | 变更 |
|------|------|------|
| `kernel/include/kernel/task.h` | 任务标志位 + blocker 常量 | +`PF_SELF_REAP`；−`PF_REAPED`、−`BLOCKER_DEFERRED_FREE` |
| `kernel/sched/task.c` | 调度器 / `do_exit` / `do_waitpid` | kthread 自收割分支、`do_waitpid` 重写、孤儿自 reparent、删 reaper 块、移 `sched_unblock_blocked` |
| `kernel/arch/x86_64/switch.c` | `__switch_to` | epilogue 增 `PF_SELF_REAP` 自收割 + include slab.h |
| `kernel/fs/file.c` | fd 表引用协议 | `files_unpin` 同步化 |
| `kernel/sched/deferred_free.c` | df-kthread | **删除** |
| `kernel/include/kernel/deferred_free.h` | df 头文件 | **删除** |
| `kernel/test/test_deferred_free.c` | 旧 df 测试 | **删除** |
| `kernel/test/test_kthread_self_reap.c` | kthread 自收割测试 | **新增** |
| `kernel/test/test_fd_refcount.c` | fd 引用协议竞态测试 | 仅更新头部注释 |
| `user/systest.c` | 系统测试 | +孤儿 reparent 断言 |
| `user/nettest.c` | 网络回归 | +socket-exit 断言 |

---

### Task 1: kthread 自收割（`do_exit` 分支 + `__switch_to` epilogue）

**Files:**
- Modify: `kernel/include/kernel/task.h`（在 `PF_REAPED` 后新增 `PF_SELF_REAP`）
- Modify: `kernel/sched/task.c`（`do_exit` 增 kthread 分支）
- Modify: `kernel/arch/x86_64/switch.c`（include slab.h + epilogue）
- Delete: `kernel/test/test_deferred_free.c`
- Create: `kernel/test/test_kthread_self_reap.c`
- Modify: `kernel/sched/task.c`（task_init 里 `test_deferred_free` → `test_kthread_self_reap`）

**Interfaces:**
- Consumes: 现有 `do_exit(uint64_t)`（`task.h:312`）、`schedule()`（`task.h:311`）、`__switch_to(task_t*,task_t*)`（`switch.c:18`）、`kfree`（`slab.h:38`）、`kernel_thread`（`task.h:326`）。
- Produces: `PF_SELF_REAP`（`task.h`）——被 `do_exit` 置位、被 `__switch_to` epilogue 消费。`test_kthread_self_reap()`（`test_kthread_self_reap.c`）——被 `task_init` 调用。

- [ ] **Step 1: 加 `PF_SELF_REAP` 标志**

`kernel/include/kernel/task.h`，在第 68 行 `#define PF_REAPED (1 << 4) ...` 之后新增一行：

```c
#define PF_SELF_REAP (1 << 5)   // kthread exited; __switch_to epilogue frees it
```

（`PF_REAPED` 暂时保留——`schedule()` 的 reaper 在 Task 3 才删除，仍在引用它。）

- [ ] **Step 2: `do_exit` 增 kthread 自收割分支**

`kernel/sched/task.c`，定位 `do_exit`（约 870 行）的 `current->exit_code = exit_code;`，在其后、`// NOTE: we stay TASK_RUNNING ...` 注释之前插入：

```c
    current->exit_code = exit_code;

    // ── kthread self-reap ──────────────────────────────────
    // Kernel threads have no waitpid consumer. Reclaim them here by
    // removing them from the global list and marking PF_SELF_REAP so
    // __switch_to's epilogue frees thread/fpu_save/stack after the
    // final switch. The list_del + PF_SELF_REAP + ZOMBIE transition is
    // atomic under task_list_lock (IRQs off): a tick firing between any
    // two steps could schedule() and either re-enqueue a still-RUNNING
    // task whose stack __switch_to is about to free (UAF) or switch
    // away without freeing (leak).
    //
    // Note: this branch runs AFTER do_exit's earlier vma_free_all(mm)
    // and files_unpin(fs). A kthread's mm is shared (do_fork: tsk->mm =
    // current->mm), so vma_free_all here is a no-op on the shared
    // (empty-vma) init_mm — pre-existing behavior, not introduced by
    // this change. The self-reap epilogue below still only kfree's the
    // three standalone slabs (thread/fpu_save/stack); files/mm were
    // already handled above.
    if (current->flags & PF_KTHREAD) {
        uint64_t fl = spin_lock_irqsave(&task_list_lock);
        list_del(&current->list);
        current->list.next = NULL;
        current->list.prev = NULL;
        current->flags |= PF_SELF_REAP;
        current->state = TASK_ZOMBIE;
        spin_unlock_irqrestore(&task_list_lock, fl);
        schedule();   // switch_to → __switch_to epilogue frees this task
        // Defensive: a ZOMBIE task is never re-enqueued, so schedule()
        // always switches away. But if a future early-return path
        // (scheduler_ok==0, or in_schedule) ever let us fall through, we
        // are off-list + ZOMBIE + PF_SELF_REAP — halt rather than return
        // into kernel_thread_func's epilogue, which would do_exit() again
        // and double-list_del our already-NULL'd list node.
        for (;;) __asm__ __volatile__("hlt");
    }

    // NOTE: we stay TASK_RUNNING through the cleanup below and only
```

（下方 user-task 的 direct-switch-to-parent + `state=ZOMBIE` + `schedule()` 原样保留，只服务非 kthread 任务。）

- [ ] **Step 3: `__switch_to` epilogue 增自收割**

`kernel/arch/x86_64/switch.c`，顶部 include 区（第 1-6 行）加一行 `#include <kernel/slab.h>`：

```c
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/arch/spinlock.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/cpu.h>
#include <kernel/printk.h>
#include <kernel/slab.h>   // kfree — for PF_SELF_REAP epilogue
```

再定位函数末尾（第 98-99 行 `__atomic_store_n(&prev->on_cpu, 0, ...)` 之后、`}` 之前），把 `on_cpu=0` 注释也一并更新，替换为：

```c
    // prev has now fully left the CPU: its kernel stack is no longer in
    // use.  Clear on_cpu so do_waitpid / task_wake may reap it (a task
    // that set TASK_ZOMBIE and ran its final schedule()).  RELEASE store
    // paired with ACQUIRE loads.
    __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);

    // kthread self-reap: a PF_SELF_REAP task removed itself from the
    // global list and set ZOMBIE atomically in do_exit.  It has no
    // waitpid consumer, so reclaim its slab objects here.  We run on
    // NEXT's stack (switch_to already switched rsp), so freeing prev's
    // stack is safe.  This MUST stay the last code that touches prev:
    // kfree(prev->stack_alloc_base) frees the task_union containing prev
    // itself — do not dereference prev past this point.
    if (prev->flags & PF_SELF_REAP) {
        if (prev->thread)           kfree(prev->thread);
        if (prev->fpu_save)         kfree(prev->fpu_save);
        if (prev->stack_alloc_base) kfree(prev->stack_alloc_base);
    }
}
```

- [ ] **Step 4: 用 kthread 自收割测试替换旧 df 测试**

删除 `kernel/test/test_deferred_free.c`（旧 `test_deferred_free_kthread` 会读已自收割的 `kt->state`，必然 UAF）。

新建 `kernel/test/test_kthread_self_reap.c`：

```c
#if defined(OS01_SELFTEST)

#include <kernel/task.h>
#include <kernel/slab.h>      // kmalloc_cache_size[] — assert object-level reclaim
#include <device/timer.h>     // jiffies — time-bounded wait (cross-CPU safe)
#include <kernel/printk.h>

// kthread that exits immediately. Its thread/fpu_save/stack must be
// reclaimed by __switch_to's PF_SELF_REAP epilogue, NOT by a reaper.
static uint64_t self_reap_exiter(uint64_t arg)
{
    (void)arg;
    do_exit(0);
    return 0; // unreachable
}

// Spawn N exiting kthreads, yield until their 64KB stacks are reclaimed,
// then assert the slab object count returned to baseline.
//
// A kthread's stack_alloc_base is malloc(sizeof(task_union)+STACK_SIZE)
// == 65536 bytes → the 65536-byte slab cache (index 11). total_using is
// the object-level "in use" counter: +1 per kmalloc, -1 per kfree. It
// returns to baseline only if every stack was actually kfree'd, and it is
// immune to slab page retention. Do NOT assert on PMMngr page_free_count:
// slab.c:260 only returns a slab's 2MB page to the PMM when using_count==0
// && total_free >= color_count*3/2, so pages may legitimately stay cached
// even though every stack object was freed → a page-count assertion would
// false-fail.
//
// The wait MUST be time-bounded (jiffies), not bounded by a schedule()
// count: sched_pick_cpu() may place the 8 kthreads on the OTHER CPU, and
// this selftest runs on the boot CPU inside task_init (before the idle
// loop). The boot CPU's schedule() never runs another CPU's rq, and its
// schedule() calls mostly hit the idle preemption guard and return in
// microseconds — so a fixed number of schedule() spins would exhaust in
// ~100µs while the other CPU is still in hlt waiting for a 10ms tick, and
// total_using would never drop. A jiffies budget lets the other CPU's
// tick actually schedule and reap them.
static int test_kthread_self_reap_once(void)
{
    struct Slab_Cache *sc = &kmalloc_cache_size[11]; // 65536-byte cache
    uint64_t using_before = sc->total_using;

    for (int i = 0; i < 8; i++) {
        int pid = kernel_thread(self_reap_exiter, 0, PF_KTHREAD);
        if (pid < 0) {
            serial_printk("[selftest] kthread_self_reap: "
                          "kernel_thread #%d failed\n", i);
            return -1;
        }
    }

    // Yield until the 8 kthreads run do_exit and are switched out (each
    // __switch_to kfree's its stack on the way out → total_using--). The
    // 100-jiffy budget (1s at 100 Hz) comfortably covers the other CPU's
    // 10ms tick cadence. total_using is read locklessly: another CPU's
    // kfree bumps it under slab_lock, but on x86_64 an aligned u64 read
    // is atomic and this is a benign progress check, not a correctness
    // dependency (the final comparison is also just a leak detector).
    uint64_t start = jiffies;
    while (sc->total_using > using_before && jiffies - start < 100)
        schedule();

    if (sc->total_using > using_before) {
        serial_printk("[selftest] kthread_self_reap: "
                      "leaked stacks: using_before=%lu using_after=%lu\n",
                      (unsigned long)using_before,
                      (unsigned long)sc->total_using);
        return -1;
    }
    return 0;
}

void test_kthread_self_reap(void)
{
    int ok = 0, fail = 0;
    serial_printk("[selftest] kthread_self_reap... ");
    if (test_kthread_self_reap_once() == 0) { ok++; serial_printk("PASS\n"); }
    else { fail++; serial_printk("FAIL\n"); }
    serial_printk("[selftest] kthread_self_reap: %d passed, %d failed\n",
                  ok, fail);
}

#endif // OS01_SELFTEST
```

- [ ] **Step 5: 更新 `task_init` 里的测试调用点**

`kernel/sched/task.c`，定位约 1971-1978 行的 `#ifdef OS01_SELFTEST` 块，把 `test_deferred_free` 替换为 `test_kthread_self_reap` 并更新注释：

```c
#ifdef OS01_SELFTEST
    // ── kthread self-reap selftest ───────────────────────────
    // Must run after scheduler_ok=1 so schedule() works.
    {
        extern void test_kthread_self_reap(void);
        test_kthread_self_reap();
    }
#endif
```

- [ ] **Step 6: 构建并跑 selftest**

Run: `make clean && make KERNEL_SELFTEST=1 -j$(nproc)`
（验证能编译；完整 boot 验证在 Task 3 后统一做，但此处至少确认无编译错误。）

- [ ] **Step 7: Commit**

```bash
git add kernel/include/kernel/task.h kernel/sched/task.c kernel/arch/x86_64/switch.c kernel/test/test_kthread_self_reap.c
git rm kernel/test/test_deferred_free.c
git commit -m "feat(sched): kthread self-reap via __switch_to epilogue"
```

---

### Task 2: wait 驱动的用户任务收割（`do_waitpid` + 孤儿 reparent）

**Files:**
- Modify: `kernel/sched/task.c`（`waitpid_should_unblock`、`do_waitpid`、`do_exit` 孤儿自 reparent）

**Interfaces:**
- Consumes: `on_cpu` ACQUIRE 读、`list_del`（`libc/include/list.h:35`）、`kfree`。
- Produces: 无新导出符号；`do_waitpid` 的返回语义不变（子 pid / `-ECHILD` / `-EINTR` / WNOHANG 返回 0）。

- [ ] **Step 1: 重写 `waitpid_should_unblock`**

`kernel/sched/task.c`，替换整个 `waitpid_should_unblock`（约 928-949 行）为：

```c
static bool waitpid_should_unblock(task_t *waiter)
{
    int64_t target_pid = waiter->blocker_data.waited_pid;
    list_t *pos = init_task_union.task.list.next;

    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);

        if (t->parent != waiter)
            continue;
        if (target_pid != -1 && t->pid != target_pid)
            continue;
        // Reapable only once the child fully left the CPU. A ZOMBIE
        // child still on_cpu==1 is mid-final-schedule — skip it and
        // keep scanning (do NOT return false: another child may be
        // ready).
        if (t->state == TASK_ZOMBIE &&
            __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) == 0) {
            return true;   // reapable — do_waitpid re-scans to reap it
        }
    }
    return false;
}
```

- [ ] **Step 2: 重写 `do_waitpid`**

`kernel/sched/task.c`，替换整个 `do_waitpid`（约 958-1053 行）为：

```c
int64_t do_waitpid(int64_t pid, int *user_status, int options)
{
    for (;;) {
        task_t   *child      = NULL;
        int64_t   child_pid  = -1;
        int64_t   exit_code  = 0;

        // Pass 1: find a reapable ZOMBIE child (on_cpu==0) and detach it.
        {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);

                if (t->parent != current)
                    continue;
                if (t->state != TASK_ZOMBIE)
                    continue;
                if (pid != -1 && t->pid != pid)
                    continue;
                // Gate on on_cpu==0: __switch_to's RELEASE store
                // guarantees the child's stack/thread/exit_code are no
                // longer in use. A ZOMBIE child still on_cpu==1 is not
                // reapable yet — treat it as "exists, keep waiting".
                if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) != 0)
                    continue;

                // Capture pid/exit_code BEFORE detaching and freeing
                // (kfree(stack_alloc_base) frees the task_union holding
                // the task_t, so t->pid is invalid after step 2 below).
                child      = t;
                child_pid  = t->pid;
                exit_code  = t->exit_code;
                list_del(&t->list);
                t->list.next = NULL;
                t->list.prev = NULL;
                break;
            }
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
        }

        if (child) {
            if (user_status) {
                int status = (int)((exit_code & 0xFF) << 8);
                if ((uint64_t)user_status < current->addr_limit)
                    *user_status = status;
            }

            // Synchronous reclamation (no more schedule() reaper).
            // Order: free the two standalone slabs first, then the
            // task_union (which contains the task_t itself).
            if (child->thread)           kfree(child->thread);
            if (child->fpu_save)         kfree(child->fpu_save);
            if (child->stack_alloc_base) kfree(child->stack_alloc_base);

            debug_task("waitpid: pid=%d reaped child %d (exit=%d)\n",
                          (int)current->pid, (int)child_pid, (int)exit_code);
            return child_pid;
        }

        // No reapable child — check existence for -ECHILD / WNOHANG.
        int child_exists = 0;
        {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (t->parent == current && (pid == -1 || t->pid == pid)) {
                    child_exists = 1;
                    break;
                }
            }
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
        }

        if (!child_exists)
            return -ECHILD;

        if (options & WNOHANG)
            return 0;

        current->blocker_data.waited_pid = pid;

        int ret = blocker_wait(waitpid_should_unblock, BLOCKER_WAITPID, true);
        if (ret == -EINTR)
            continue;   // re-check for children before sleeping again
        // blocker_wait returned 0 → condition met; loop and reap in Pass 1.
        continue;
    }
}
```

- [ ] **Step 3: `do_exit` 增孤儿自 reparent**

`kernel/sched/task.c`，定位 `do_exit` 的 reparent 块（约 810-823 行）。在现有「临终任务的孩子 reparent 到 init」逻辑**之后**（同一函数内、SIGCHLD 块之前）插入自 reparent：

```c
    // Defensive self-reparent: a USER task whose own parent is a kthread
    // or NULL (shouldn't happen in normal flow, but closes the
    // "parent==NULL zombie leak" the old reaper's parent==NULL branch
    // used to handle) becomes init's child so init can reap it.
    //
    // The current->parent read + write is done under task_list_lock, like
    // the child-reparent block above: waitpid_should_unblock and
    // sched_unblock_blocked read t->parent from unlocked contexts, so a
    // lockless write here is a data race with those scans on another CPU.
    // (Note: after reparenting to init, the SIGCHLD block below will
    // deliver to init instead of being skipped — harmless; init ignores it.)
    if (user_init_task && !(current->flags & PF_KTHREAD)) {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        task_t *p = current->parent;
        if (p == NULL || (p->flags & PF_KTHREAD)) {
            current->parent = user_init_task;
            debug_task("reparent: self %d → init (orphan)\n", (int)current->pid);
        }
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }
```

（`user_init_task` / `user_init_pid` 是 task.c 内已有的 static，直接可用。）

- [ ] **Step 4: 构建验证 + systest fork/wait 组**

Run:
```bash
make clean && make -j$(nproc)
make OS01_SYSTEST=1 test-syscall
```
Expected: systest 通过 `fork+exec+waitpid`（`test_fork_exec_waitpid`）等所有 fork/wait 相关测试，无 hang、无 crash。

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): wait-driven user-task reaping in do_waitpid"
```

---

### Task 3: 删除 reaper + deferred_free

**Files:**
- Modify: `kernel/sched/task.c`（删 `schedule()` reaper 块、移 `sched_unblock_blocked`、删 `deferred_free_spawn` 调用、删 include）
- Modify: `kernel/include/kernel/task.h`（删 `PF_REAPED`、`BLOCKER_DEFERRED_FREE`）
- Modify: `kernel/fs/file.c`（`files_unpin` 同步化、删 include）
- Delete: `kernel/sched/deferred_free.c`、`kernel/include/kernel/deferred_free.h`

**Interfaces:**
- Consumes: 前两任务已让 reaper 成为 no-op（用户任务被 `do_waitpid` 收割、kthread 自收割），此时删它是安全的。
- Produces: `files_unpin` 变为同步释放（`file.h:130` 语义不变，仅 drop-to-zero 从 defer 变直调）。

- [ ] **Step 1: 删 `schedule()` 的 zombie reaper 块，替换为独立的 `sched_unblock_blocked` 调用**

`kernel/sched/task.c`，定位 `schedule()` 的「step 3 Zombie reaper」整块（约 551-643 行，从 `// ── 3. Zombie reaper (global list, with on_rq guard) ──` 到 `for (int i = 0; i < files_free_count; i++) files_unpin(...)` 的右括号）。**整体替换**为：

```c
    // ── 3. Wake blocked tasks whose condition is now met ──
    // (was inside the old zombie-reaper critical section; now
    // standalone — this is the blocker-wakeup backstop, NOT a reaper.)
    {
        uint64_t ub_flags = spin_lock_irqsave(&task_list_lock);
        sched_unblock_blocked();
        spin_unlock_irqrestore(&task_list_lock, ub_flags);
    }
```

`schedule()` 其余（step 1 update_curr、step 2 dequeue、step 3.5 balance、step 4 pick、step 5 idle fallback、step 6 switch）**不动**。

- [ ] **Step 2: 删 `deferred_free_spawn` 调用 + include**

`kernel/sched/task.c`：
1. 删除第 23 行 `#include <kernel/deferred_free.h>    // deferred_free() for zombie reaping`。
2. 定位约 1948-1953 行，删除整个块：

```c
    // Spawn the deferred-free reaper kthread BEFORE activating the
    // scheduler.  This guarantees the reaper exists before any zombie
    // can be produced (schedule() returns early while scheduler_ok==0).
    {
        task_t *df = deferred_free_spawn();
    }
```

- [ ] **Step 3: 删 `PF_REAPED` 与 `BLOCKER_DEFERRED_FREE`**

`kernel/include/kernel/task.h`：
1. 删除第 68 行 `#define PF_REAPED (1 << 4)   // do_waitpid read this ZOMBIE; schedule() will kfree`（`PF_SELF_REAP` 保留在 bit 5）。
2. 删除第 17 行 `#define BLOCKER_DEFERRED_FREE 2`。

- [ ] **Step 4: `files_unpin` 同步化**

`kernel/fs/file.c`：
1. 删除第 15 行 `#include <kernel/deferred_free.h>`。
2. 替换 `files_unpin`（约 284-291 行，含上方注释）为：

```c
// MUST NOT be called under task_list_lock/fs->lock/rq lock: drop-to-zero
// → synchronous files_free, which may block (netconn_delete → lwIP) and
// take file/pipe/pty locks.
void files_unpin(files_t *fs)
{
    if (!fs) return;
    if (__sync_sub_and_fetch(&fs->refcount, 1) == 0)
        files_free(fs);
}
```

- [ ] **Step 5: 删除 deferred_free 源文件**

```bash
git rm kernel/sched/deferred_free.c kernel/include/kernel/deferred_free.h
```

- [ ] **Step 6: 构建 + 全量验证**

Run（顺序执行）：
```bash
make clean && make KERNEL_SELFTEST=1 -j$(nproc)
make OS01_SYSTEST=1 test-syscall
```
Expected:
- selftest 全部 PASS（含 `kthread_self_reap`、`fd_refcount` 相关）。
- systest 全部 PASS（无 zombie 泄漏导致的 hang/`-ECHILD` 误报）。

- [ ] **Step 7: Commit**

```bash
git add kernel/sched/task.c kernel/include/kernel/task.h kernel/fs/file.c
git commit -m "refactor(sched): remove zombie reaper and deferred_free kthread"
```

---

### Task 4: 回归测试（孤儿 reparent + socket-exit）+ 文档

**Files:**
- Modify: `user/systest.c`（孤儿 reparent 断言）
- Modify: `user/nettest.c`（socket-exit 断言）
- Modify: `kernel/test/test_fd_refcount.c`（仅头部注释）
- Modify: `docs/roadmap.md`（标记 P0 #3 完成）

**Interfaces:**
- Consumes: `fork`/`waitpid`/`_exit`/`socket`/`WIFEXITED`（libc 既有 wrapper）。
- Produces: 无新导出符号。

- [ ] **Step 1: systest 增孤儿 reparent 断言**

`user/systest.c`，在 `test_fork_exec_waitpid`（约 93-110 行）之后新增：

```c
// ── orphan reparent: child dies while its own child is alive ──
// The grandchild becomes an orphan, reparented to init (PID 1) by the
// child's do_exit; init's supervision loop (waitpid(-1, WNOHANG)) reaps
// it. Verify: (1) the direct child is reaped normally, and (2) the
// orphaned grandchild is actually reaped by init — it must disappear from
// /proc (a leaked ZOMBIE would still be on the task list and listed under
// /proc/<pid>).
static void test_orphan_reparent(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("orphan_reparent", "pipe failed"); return; }

    int64_t c = fork();
    if (c < 0) { close(fds[0]); close(fds[1]); FAIL("orphan_reparent", "fork failed"); return; }
    if (c == 0) {
        // child: fork grandchild, report its pid up the pipe, exit.
        close(fds[0]);
        int64_t g = fork();
        if (g == 0) { close(fds[1]); _exit(0); }       // grandchild: orphaned
        if (g < 0)  { close(fds[1]); _exit(1); }       // grandchild fork failed
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d", (int)g);
        write(fds[1], buf, (size_t)n + 1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);

    // Read grandchild pid, then reap the direct child.
    char gbuf[32] = {0};
    read(fds[0], gbuf, sizeof(gbuf) - 1);
    close(fds[0]);
    int gpid = 0;
    sscanf(gbuf, "%d", &gpid);

    int status = 0;
    int64_t w = waitpid(c, &status, 0);
    CHECK3(w == c && WIFEXITED(status), "orphan_reparent", "child reaped");

    // Poll for init's supervision loop to reap the orphan, then probe
    // /proc/<gpid>: open resolves via procfs_readdir enumeration, so it
    // returns <0 once the task is off the task list (reaped). A leaked
    // ZOMBIE would still resolve (fd >= 0) and read 0 bytes (mm already
    // freed). Poll rather than a single fixed sleep: init's reap_children
    // cadence is ~100ms and races tty respawn / scheduler jitter, so a
    // one-shot probe could catch the grandchild still-ZOMBIE and false-fail.
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/maps", gpid);

    int probe = -1;
    int tries = 0;
    for (; tries < 50; tries++) {
        probe = open(path, O_RDONLY);
        if (probe < 0) break;          // reaped — gone from the task list
        close(probe);
        struct timespec req = { .tv_sec = 0, .tv_nsec = 20000000 }; // 20ms
        nanosleep(&req, NULL);
    }
    if (probe >= 0) close(probe);
    CHECK3(gpid > 0 && probe < 0, "orphan_reparent", "grandchild reaped by init");
}
```

并在 `tests[]` 数组（约 1274 行 `{"fork+exec+waitpid", test_fork_exec_waitpid},` 之后）注册：

```c
    {"orphan_reparent",   test_orphan_reparent},
```

- [ ] **Step 2: nettest 增 socket-exit 断言**

`user/nettest.c`，在 `test_wget`（约 159 行）之后新增：

```c
// A task holding an open socket exits WITHOUT closing it: its do_exit →
// files_unpin → file_free → netconn_delete must complete (blocking on a
// tcpip_thread round-trip) without hanging or crashing.
static int test_socket_exit(void)
{
    int64_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);  // open, deliberately not closed
        if (fd < 0) _exit(127);   // socket must succeed to exercise the path
        _exit(0);   // do_exit frees the open socket → netconn_delete
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
```

并在 `main`（约 168 行 `result("wget", test_wget());` 之后）注册：

```c
    result("socket_exit", test_socket_exit());
```

- [ ] **Step 3: 更新 `test_fd_refcount.c` 头部注释**

`kernel/test/test_fd_refcount.c` 第 3-4 行：

```c
// Two scenarios, run from task_init() AFTER deferred_free_spawn()
// (files_unpin can defer-free) and scheduler_ok=1 (kernel_thread +
```

替换为：

```c
// Two scenarios, run from task_init() AFTER scheduler_ok=1 (kernel_thread +
```

（并顺带核对后续注释是否还提到「defer」——若有，同步改成「同步 free」语义。测试逻辑本身不变。）

- [ ] **Step 4: 更新 roadmap 标记**

`docs/roadmap.md` 第 31 行：

```markdown
 3. 任务退出/回收收敛         — wait 驱动回收，移除调度器与 reaper 双重职责
```

改为：

```markdown
 3. 任务退出/回收收敛         ✅ — wait 驱动回收（do_waitpid 直接收割）+ kthread __switch_to 自收割，删除 reaper 与 deferred_free
```

- [ ] **Step 5: 全量验证（含网络 + SMP）**

Run：
```bash
make clean && make -j$(nproc)
make OS01_SYSTEST=1 test-syscall        # systest（含 orphan_reparent）
make test-network                        # nettest（含 socket_exit）
make KERNEL_SELFTEST=1 run 2>&1 | grep -E "selftest|kthread_self_reap"   # 手动确认 selftest PASS
```
Expected: 全部 PASS；`-smp 2`（默认）下无 hang、无 `SWITCH-BAD`/`SCHED-PREV-BAD` 诊断、无页面泄漏。

- [ ] **Step 6: Commit**

```bash
git add user/systest.c user/nettest.c kernel/test/test_fd_refcount.c docs/roadmap.md
git commit -m "test: orphan reparent + socket-exit regression; mark roadmap P0 done"
```

---

## 自审记录

- **Spec 覆盖**：§3.1（do_waitpid 收割 + on_cpu 门 + 捕获 pid）→ Task 2；§3.2（kthread 自收割 + tick 竞态原子化）→ Task 1；§3.3（孤儿 reparent）→ Task 2 step 3 + Task 4 step 1；§3.4（保留 sched_unblock_blocked）→ Task 3 step 1；§4/§6（删 deferred_free、files_unpin 同步）→ Task 3；§7 不变式 → 逐条落在 Task 1/2 的代码注释与原子读写；§8 测试 → Task 1 selftest + Task 4 systest/nettest。无遗漏。
- **占位符扫描**：无 TBD/TODO，所有代码块为完整可编译片段。
- **类型一致性**：`PF_SELF_REAP` 全程用 bit 5（`1<<5`），Task 1 定义、Task 3 保留；`PF_REAPED`/`BLOCKER_DEFERRED_FREE` 在 Task 3 才删除，避免 Task 1/2 中间态编译断裂；`test_kthread_self_reap` 函数名 Task 1 定义与 `task_init` 调用点一致。
- **死字段提示**：`blocker_data_t.waited_child`（`task.h:29`）在本方案后不再被读写（`do_waitpid` 改为 Pass 1 重扫，`waitpid_should_unblock` 只返回 bool）。字段留在 `task_t` 中是无害的（不再改动 struct 布局），实现时如愿意可顺手删，但**非必需**。

