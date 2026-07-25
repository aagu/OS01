# EEVDF 公平调度器 — 设计 Spec

> **日期**: 2026-07-24
> **基准**: `be6046e` (v10 roadmap, select/pselect complete, 118/118 systest)
> **状态**: Draft

---

## 1. 目标与非目标

### 目标

将 OS01 调度器从 **O(n) 全局链表 max-counter 扫描**升级为 **per-CPU 红黑树 EEVDF**（Earliest Eligible Virtual Deadline First），提供：

- **公平性**：vruntime 跟踪确保每个任务获得平等的 CPU 时间
- **反饿死**：睡眠后唤醒的任务 vruntime 被推到 min_vruntime 附近，不会因"欠账"而被饿死
- **O(log n)** 插入/删除/选择，替代当前 O(n) 链表扫描
- **Per-CPU runqueue**：每个 CPU 独立红黑树，无跨核锁竞争

### 非目标 (v1)

- nice 值权重映射（`prio_to_weight[]`）
- cgroup 层级调度
- SMP 跨核迁移 / 负载均衡
- TSC 精度 vruntime（用 tick 计数）
- `update_curr()` 细粒度统计（只累加 tick 数）

---

## 2. 当前状态

### 2.1 调度器现状 (`kernel/sched/task.c`)

```
schedule():
  1. hang detector reset
  2. counter > 0 且 TASK_RUNNING → counter--, return (不切换)
  3. zombie reaper (遍历全局链表, 3 passes)
  4. sched_unblock_blocked (遍历全局链表, 检查 blocker 条件)
  5. 全局链表 O(n) 扫描: 找最高 counter 的 TASK_RUNNING 任务
  6. 找不到 → idle
  7. switch_to(current, next)
```

- `counter` 兼作时间片和优先级：高 priority → 大 counter → 优先选中
- `task_t.counter` 用完时 `schedule()` 重新赋值为 `priority`
- `percpu_t.run_queue` 字段已存在（`list_t`）但**从未被使用**——所有任务挂在全局 `init_task_union.task.list` 上

### 2.2 Timer tick 路径

```
PIT (IRQ0, 100Hz)  → pit_handler()      → need_resched=1, watchdog_counter++
LAPIC timer (100Hz) → lapic_timer_handler() → need_resched=1, watchdog_counter++
                                                      │
                                            ret_from_intr → schedule()
```

10ms per tick，`jiffies` 为全局 tick 计数器。

### 2.3 任务生命周期

```
fork/exec → TASK_RUNNING, counter=priority, 挂全局链表
  ↓
schedule() 选中 → switch_to
  ↓
阻塞 (tty/pipe/blocker) → TASK_INTERRUPTIBLE, 脱离 CPU 但在全局链表
  ↓
被唤醒 → TASK_RUNNING（仍在全局链表，等待 schedule() 扫描）
  ↓
do_exit → TASK_ZOMBIE → schedule() zombie reaper 回收
```

---

## 3. 架构设计

### 3.1 新 schedule() 流程

```
schedule():
  1. hang detector reset（不变）
  2. update_curr(current)              // vruntime += 1
  3. if RUNNING && !idle → enqueue_task(current, rq)  // rbtree_insert(deadline)
  4. zombie reaper（不变，仍遍历全局 list）

  ── rq_lock ──
  5. sched_unblock_blocked（不变）
  6. next = pick_eevdf(rq)            // min deadline eligible
  7. if next != idle → dequeue_task(next, rq)
  ── unlock ──

  8. if !next → next = idle
  9. update min_vruntime
 10. need_resched = 0 ; switch_to(current, next)
```

### 3.2 调用图

```
timer tick (pit_handler / lapic_timer_handler)
  │
  └── need_resched = 1                // [现有，不改动]
           │
     ret_from_intr → schedule()
           │
           ├── update_curr(current)    // [新增] vruntime += 1
           │     └── if vruntime >= deadline → need_resched = 1
           ├── enqueue_task(current)
           ├── zombie reaper（不变）
           ├── sched_unblock_blocked（不变）
           ├── next = pick_eevdf(rq)   // O(log n)
           └── switch_to(current, next)

task_wake(t) [新增/重构]:
  t->state = TASK_RUNNING
  t->vruntime = max(t->vruntime, rq.min_vruntime - LATENCY)
  enqueue_task(t, &percpu_data[t->cpu])
  percpu_data[t->cpu].need_resched = 1

do_fork():
  tsk->vruntime  = current->vruntime  // [新增] 子进程继承
  tsk->state     = TASK_UNINTERRUPTIBLE // [修改] 先不设 RUNNING
  tsk->on_rq     = false
  rbtree_node_init(&tsk->rb_node)
  // 不设 TASK_RUNNING — 调用方（trap.c）在返回用户态前设 state=RUNNING
  // 首次 schedule() 将子进程入队

do_exit():
  state = TASK_ZOMBIE                  // [不变]
  schedule() 看到 state == ZOMBIE && on_rq → 先 dequeue
  // zombie reaper 从全局链表回收
```

**关键语义**：
- `update_curr()` **只在 `schedule()` 中调用**，timer handler 除了设 `need_resched=1` 之外不做任何调度操作。这保证了每个 tick 恰好一次 vruntime 累加，BSP 和 AP 行为一致。
- `schedule()` 在 ret_from_intr 路径上每个 tick 被调用一次，`update_curr` 累加 1 tick。如果 vruntime 还没到 deadline，`schedule()` 立即返回（earliest deadline 仍是当前任务），无上下文切换。

---

## 4. 数据结构

### 4.1 红黑树 (`libc/rbtree/`)

```c
// libc/include/rbtree.h

typedef struct rbtree_node {
    struct rbtree_node *left;
    struct rbtree_node *right;
    struct rbtree_node *parent;
    unsigned long color;         // 0 = black, 1 = red
} rbtree_node_t;

typedef struct rbtree_root {
    rbtree_node_t *rb_node;      // NULL when tree is empty
} rbtree_root_t;

// 初始化 root 为空树
static inline void rbtree_init(rbtree_root_t *root) {
    root->rb_node = NULL;
}

// 初始化一个 detached node。所有指针清零。
// memset 零的 task_t 已自动满足此状态，但显式调用更安全。
void rbtree_node_init(rbtree_node_t *node);

// 插入节点。返回 NULL 成功，返回非 NULL 表示 key 冲突（同 deadline）。
// cmp(a, b) 返回 <0 表示 a 应在 b 之前，>0 则 a 在 b 之后。
rbtree_node_t *rbtree_insert(rbtree_root_t *root, rbtree_node_t *node,
                             int (*cmp)(rbtree_node_t *a, rbtree_node_t *b));

// 从树中删除节点。node 必须当前在树中。
void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node);

// 返回按 cmp 顺序的最小节点（最左），树空返回 NULL
rbtree_node_t *rbtree_first(rbtree_root_t *root);

// 返回按 cmp 顺序的下一个节点，无后继返回 NULL
rbtree_node_t *rbtree_next(rbtree_node_t *node);

// 树是否为空
static inline int rbtree_empty(rbtree_root_t *root) {
    return root->rb_node == NULL;
}
```

**实现**：经典红黑树，参考 linux `lib/rbtree.c`，约 160 行。插入 O(log n)、删除 O(log n)。

**放置位置**：`libc/include/rbtree.h` + `libc/rbtree/rbtree.c`，与 `list_t` 同模式。需在 `libc/Makefile` 的源文件发现列表中加一行 `$(wildcard rbtree/*.c)`。编译进 `libk.a`（内核态 `-D__is_libk`）和 `libc.a`（用户态 systest 可直接测试）。

### 4.2 task_t 新增字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `rb_node` | `rbtree_node_t` | 挂载在 per-CPU runqueue 红黑树上 |
| `vruntime` | `uint64_t` | 累积虚拟运行时间，单位 ticks |
| `deadline` | `uint64_t` | `vruntime + EEVDF_MIN_SLICE`，rbtree 的排序 key |
| `on_rq` | `bool` | 当前是否在某个 CPU 的 runqueue 上 |

**保留兼容字段**：`counter` 和 `priority` 字段保持不变但 EEVDF v1 不再读取。以下位置有残留赋值，对正确性无害但为死代码：
- `spawn_user_task()`: `tsk->counter = 1`, `tsk->priority = 5`
- `create_idle_task()`: `tsk->priority = 2; tsk->counter = 2`
- `INIT_TASK` 宏: `.counter = 0, .priority = 2`

建议清理以消除维护者困惑（但不影响正确性）。

### 4.3 percpu_t 变更

| 字段 | 旧类型 | 新类型 | 说明 |
|------|--------|--------|------|
| `run_queue` | `list_t` | `rbtree_root_t` | 可运行任务红黑树 |
| `min_vruntime` | — | `uint64_t` (新增) | 该 CPU 的最小 vruntime 跟踪 |
| `rq_lock` | — | `spinlock_T` (新增) | 自旋锁，保护 rbtree 操作。初始值 `{ .lock = 1L }` (unlocked) |

**注意**：`percpu_init()` 用 memset 清零整个 `percpu_data[cpu]`，随后设置 `cpu_id`、`self` 等字段。**必须**在 memset 之后补：
```c
rbtree_init(&percpu_data[cpu].run_queue);
percpu_data[cpu].min_vruntime = 0;
percpu_data[cpu].rq_lock.lock = 1L;  // spinlock_T: lock=1 表示 unlocked
```

这保证了 BSP 和所有 AP 的 runqueue 都被正确初始化。

---

## 5. 核心算法

### 5.0 内核 ASSERT 宏

内核需要自己的 ASSERT 宏（libc `assert.h` 在 `NDEBUG` 下是 no-op 且依赖 `__assert_fail`）：

```c
// kernel/include/kernel/assert.h（新增）
#define ASSERT(x) do { \
    if (!(x)) { \
        log_err("ASSERT failed: %s at %s:%d\n", #x, __FILE__, __LINE__); \
        while (1) { __asm__ __volatile__("hlt"); } \
    } \
} while (0)
```

### 5.1 常量

```c
#define EEVDF_MIN_SLICE  10   // 时间片 = 10 ticks = 100ms（与当前 priority=10 等价）
#define EEVDF_LATENCY    40   // eligibility 延迟窗口 = 40 ticks = 400ms
```

**为什么 LATENCY=40**：2 核 × 每个最多 `EEVDF_MIN_SLICE*2` = 20 ticks 不轮到下一个任务，40 ticks 保证即使满负载也至少每 400ms 调度一次。实际使用中任务数 <5，安全裕量充足。

### 5.2 update_curr()

```c
// 仅在 schedule() 中调用，不在 timer handler 中调用。
// schedule() 在每次 timer tick 的 ret_from_intr 路径上被调用，
// 因此每个 tick 恰好累加一次，BSP 和 AP 一致。
static void update_curr(task_t *task) {
    if (!task || task == this_cpu()->idle)
        return;
    task->vruntime += 1;

    // 时间片用尽 — 标记需要重调度
    if (task->vruntime >= task->deadline)
        this_cpu()->need_resched = 1;
}
```

**调用点**：仅 `schedule()` 内部调用。timer handler（pit_handler、lapic_timer_handler）**不改动**。

### 5.3 enqueue_task() / dequeue_task()

```c
static int cmp_deadline(rbtree_node_t *a, rbtree_node_t *b) {
    task_t *ta = container_of(a, task_t, rb_node);
    task_t *tb = container_of(b, task_t, rb_node);
    if (ta->deadline < tb->deadline) return -1;
    if (ta->deadline > tb->deadline) return 1;
    // 同 deadline：用 pid 打破平局；pid 也相同（不应发生）则比较指针
    if (ta->pid < tb->pid) return -1;
    if (ta->pid > tb->pid) return 1;
    return (uintptr_t)a < (uintptr_t)b ? -1 : 1;
}

static void enqueue_task(task_t *task, percpu_t *rq) {
    task->deadline = task->vruntime + EEVDF_MIN_SLICE;
    task->on_rq = true;
    rbtree_node_t *conflict = rbtree_insert(&rq->run_queue, &task->rb_node, cmp_deadline);
    // conflict 非 NULL 表示 rbtree bug——同 deadline+同 pid 或双重入队
    // 内核 ASSERT：失败时 panic（log_err + hlt），不能静默忽略
    ASSERT(conflict == NULL);
}

static void dequeue_task(task_t *task, percpu_t *rq) {
    rbtree_erase(&rq->run_queue, &task->rb_node);
    task->on_rq = false;
}
```

### 5.4 pick_eevdf()

```c
// 从 per-CPU runqueue 中选择下一个任务。O(log n)。
// 检查 rbtree 最左节点（最小 deadline）：
//   - 如果 eligible (vruntime <= min_vruntime + LATENCY) → 选中它
//   - 否则推进 min_vruntime 到该节点的 vruntime，选中它
// 不需要线性扫描：当所有任务都在 min_vruntime + LATENCY 之后时，
// 最左节点是公平性最差的（vruntime 最小 = 最"亏"），直接给它 CPU。
static task_t *pick_eevdf(percpu_t *rq) {
    if (rbtree_empty(&rq->run_queue))
        return rq->idle;

    rbtree_node_t *node = rbtree_first(&rq->run_queue);
    task_t *t = container_of(node, task_t, rb_node);

    // 如果最左节点不 eligible，推进 min_vruntime
    if (t->vruntime > rq->min_vruntime + EEVDF_LATENCY)
        rq->min_vruntime = t->vruntime;

    return t;
}
```

### 5.5 task_wake() —— 重构唤醒路径

```c
// 将任务标记为 RUNNING 并入队到其 CPU 的 runqueue。
// 如果目标 CPU 不是当前 CPU，设置 need_resched 使目标 CPU 重调度。
// 调用者必须在该 CPU 的 rq_lock 之外调用。
static void task_wake(task_t *t) {
    percpu_t *rq = &percpu_data[t->cpu];

    t->state = TASK_RUNNING;

    if (t->on_rq)
        return;  // 已经在 runqueue 上

    // 睡眠后的任务 vruntime 可能落后很多 — 追赶到合理范围
    uint64_t wake_vruntime = rq->min_vruntime > EEVDF_LATENCY
        ? rq->min_vruntime - EEVDF_LATENCY
        : 0;
    if (t->vruntime < wake_vruntime)
        t->vruntime = wake_vruntime;

    {
        uint64_t flags = spin_lock_irqsave(&rq->rq_lock);
        enqueue_task(t, rq);
        spin_unlock_irqrestore(&rq->rq_lock, flags);
    }

    // 触发目标 CPU 重调度
    if ((int)t->cpu != (int)cpu_id())
        rq->need_resched = 1;
}
```

**调用点**：现有的 `blocker_wake()` 和 `wait_queue` 唤醒路径调用 `task_wake()` 替代直接 `state = TASK_RUNNING`。

### 5.6 schedule() 完整实现

```c
void schedule(void) {
    percpu_t *rq = this_cpu();
    if (!rq->scheduler_ok)
        return;

    rq->schedule_count++;

    // ── Hang detector ──────────────────────────────────
    if (rq->watchdog_counter >= HANG_THRESHOLD) {
        log_info("[hang] CPU %u recovered (watchdog=%lu ticks)\n",
                 (unsigned)cpu_id(), (unsigned long)rq->watchdog_counter);
        hang_dump_all();
    }
    rq->watchdog_counter = 0;

    // ── 1. Update current task's vruntime ──────────────────
    update_curr(current);

    // ── 2. Re-enqueue current if still runnable; dequeue if zombie ──
    //    Hold rq_lock: task_wake() from another CPU may concurrently
    //    enqueue a task onto this CPU's rbtree.
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        if (current->on_rq)
            dequeue_task(current, rq);       // 无论状态，先从树中摘除
        if (current->state == TASK_RUNNING && current != rq->idle)
            enqueue_task(current, rq);       // 重新入队（刷新 deadline）
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }

    // ── 3. Zombie reaper (unchanged) ────────────────────────
    {
        static spinlock_T reap_lock = { .lock = 1L };
        uint64_t reap_flags = spin_lock_irqsave(&reap_lock);

        task_t *reap_list[64];
        int reap_count = 0;

        {
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list && reap_count < 64) {
                if ((uintptr_t)pos < 0x1000) {
                    log_err("[sched] zombie scan: corrupted list pointer %p\n", (void *)pos);
                    break;
                }
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (t->state != TASK_ZOMBIE || t == current) continue;

                // Skip tasks still on a runqueue — their owning CPU's
                // schedule() hasn't dequeued them yet.  Reaping now would
                // leave a dangling rbtree pointer.
                if (t->on_rq) continue;

                int reap = 0;
                if (t->flags & PF_REAPED) reap = 1;
                else if (t->flags & PF_KTHREAD) reap = 1;
                else if (t->parent == NULL) reap = 1;
                else if (t->parent->state == TASK_ZOMBIE) reap = 1;

                if (reap) reap_list[reap_count++] = t;
            }
        }

        // Orphan children pass (unchanged)
        if (reap_count > 0) {
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                if ((uintptr_t)pos < 0x1000) break;
                task_t *child = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (!child->parent) continue;
                for (int i = 0; i < reap_count; i++) {
                    if (child->parent == reap_list[i]) {
                        child->parent = NULL;
                        break;
                    }
                }
            }
        }

        // Reap zombies (unchanged)
        for (int i = 0; i < reap_count; i++) {
            task_t *t = reap_list[i];
            list_del(&t->list);
            t->list.next = NULL;
            t->list.prev = NULL;
            if (t->thread) kfree(t->thread);
            if (t->files) deferred_files_free(t->files);
            if (t->fpu_save) kfree(t->fpu_save);
            if (t->stack_alloc_base) deferred_kfree(t->stack_alloc_base);
        }

        // ── 4. Unblock blocked tasks ────────────────────────
        sched_unblock_blocked();

        spin_unlock_irqrestore(&reap_lock, reap_flags);
    }

    // ── 5. Pick next task (rbtree O(log n)) ─────────────────
    task_t *next;
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        next = pick_eevdf(rq);
        if (next && next != rq->idle)
            dequeue_task(next, rq);
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }
    
    // ── 6. Fallback to idle ─────────────────────────────────
    if (!next || next->state != TASK_RUNNING) {
        next = rq->idle;
        if (!next) return;
    }

    // ── 7. Update min_vruntime ──────────────────────────────
    if (next != rq->idle && next->vruntime > rq->min_vruntime)
        rq->min_vruntime = next->vruntime;

    rq->need_resched = 0;
    switch_to(current, next);
}
```

---

## 6. 集成点变更

### 6.1 Timer tick 路径

**`kernel/driver/pit.c:pit_handler()`** — 无需修改（维持现有 `need_resched=1`）。

**`kernel/apic/lapic_timer.c:lapic_timer_handler()`** — 1 行修改：
```c
// BSP 已经有 PIT 提供调度 tick，LAPIC timer 不重复设 need_resched。
// AP 只有 LAPIC timer，必须设 need_resched。
if (cpu_id() != 0)
    this_cpu()->need_resched = 1;
this_cpu()->watchdog_counter++;
```

**原理**：BSP 上 PIT（IRQ0/vector 0x20）和 LAPIC timer（vector 0x38）**都**以 100Hz 运行，各设 `need_resched=1`。这导致 BSP 每个 tick 两次进入 `ret_from_intr → schedule()`，vruntime 累加翻倍。关闭 LAPIC timer 在 BSP 上的 `need_resched` 即可消除——PIT 为 BSP 提供唯一的调度 tick，AP 用各自的 LAPIC timer。

**影响**：BSP 上 `watchdog_counter` 从 2/jiffy 降为 1/jiffy，hang detector 阈值需减半（如果需要精确）。AP 不受影响。

**`update_curr()` 调用点**：仅 `schedule()` 内部。每个 tick 恰好调用一次——BSP 上由 PIT 驱动，AP 上由 LAPIC timer 驱动。

### 6.2 `do_fork()`

在 `do_fork` 的 `memset(tsk, 0, ...)` 之后新增（与其他字段初始化并列）：
```c
// 公平起点：子进程 vruntime 不继承 parent 的全部"债务"
// 如果 parent 跑了很久（vruntime >> min_vruntime），子进程不应被迫等待
uint64_t fair_start = percpu_data[cpu_id()].min_vruntime;
tsk->vruntime  = current->vruntime < fair_start ? current->vruntime : fair_start;
tsk->deadline  = 0;                   // 将在 enqueue_task 时计算
tsk->on_rq     = false;               // 尚未入队
rbtree_node_init(&tsk->rb_node);      // detached 状态
```

**并在 `do_fork` 返回前**（替代直接 `state = TASK_RUNNING`）：
```c
tsk->state = TASK_RUNNING;
enqueue_task(tsk, &percpu_data[tsk->cpu]);  // 子进程立即出现在 rbtree 上
```

**同时删除** `do_fork` 中现已死代码的 `tsk->counter = current->counter;`（EEVDF 不再读 counter）。

### 6.3 `do_exit()`

**必须修改**。当前 `do_exit()` 有两处需要改：

**（1）line 442**：当前代码中 `parent->state = TASK_RUNNING` 直接设 state 但不入 rbtree。改为由 §6.3 末尾统一处理：
```c
// do_exit() 中 line 440-446，删除 parent->state = TASK_RUNNING，仅记录 parent_woken：
if (ps == TASK_INTERRUPTIBLE) {
    parent_woken = 1;   // 不再直接设 state=RUNNING
}
```

**（2）末尾**：替换掉直接 `switch_to(parent)`：
```c
// do_exit() 末尾，替换直接 switch_to(parent) 路径：
if (parent_woken) {
    // blocker_wake 已 task_wake(parent)（如果在 blocker 中），
    // on_rq guard 使第二次 task_wake 成为 no-op。
    // 统一用 task_wake 覆盖所有 parent 状态。
    task_wake(parent);
}
current->state = TASK_ZOMBIE;
schedule();  // step 2 dequeue current → pick_eevdf 选下一个
// unreachable
```

**原理**：
1. exiting task 可能在 rbtree 上（`on_rq=true`）——`schedule()` step 2 无条件 dequeue current
2. `parent_woken` 时先 `task_wake(parent)` 把 parent 放进 rbtree
3. `schedule()` → `pick_eevdf()` 自然选出 parent（如果 eligible）或 idle
4. zombie reaper 在后续 schedule() 中回收该 ZOMBIE 任务

### 6.4 唤醒路径审计

**所有** `state = TASK_RUNNING` 的赋值点必须改为调用 `task_wake()`，确保被唤醒任务出现在 rbtree 上。

完整审计清单（`grep -rn "state.*TASK_RUNNING" kernel/` + 手动分类）：

| # | 文件:行 | 场景 | 改为 task_wake? |
|---|---------|------|-----------------|
| 1 | `task.c` `blocker_wake()` | 条件满足，唤醒等待者 | ✅ `task_wake(task)` |
| 2 | `task.c` `sched_unblock_blocked()` | 信号打断 blocker | ✅ `task_wake(t)` |
| 3 | `task.c` `blocker_wait()` 二次检查 | 条件已满足，**当前任务自己**不阻塞 | ❌ 无需 — fast path: 跳过 rbtree enqueue，下次 schedule() 自然重新入队 |
| 4 | `task.c` `blocker_wait()` schedule 返回 | **当前任务**由 switch_to 带回 | ❌ 无需 |
| 5 | `task.c` `do_exit()` line 442 | SIGCHLD 唤醒 waitpid parent | ✅ 删除 `parent->state=RUNNING` 直接赋值，改由 §6.3 末尾统一 `task_wake(parent)` |
| 6 | `task.c` `spawn_user_task` | 新任务初始化 | ✅ `task_wake(tsk)` — 见 §6.5 |
| 7 | `task.c` `do_fork` | fork 子进程 | ✅ `enqueue_task(tsk)` — 见 §6.2 |
| 8 | `wait.c` `wait_queue_wake_one()` | 唤醒单个 I/O 等待者 | ✅ `task_wake(task)` |
| 9 | `wait.c` `wait_queue_wake_all()` | 唤醒所有 I/O 等待者 | ✅ `task_wake(task)` |
| 10 | `futex.c` `do_futex_wake()` | futex 唤醒 | ✅ `task_wake(task)` |
| 11 | `tty.c` `tty_wake_waiters()` | TTY 输出 → 唤醒 reader | ✅ `task_wake(task)` |
| 12 | `trap.c` `SYS_kill` | 信号递送唤醒目标（仅当 INTERRUPTIBLE） | ⚠️ 保持 `if (target->state == TASK_INTERRUPTIBLE) task_wake(target);` |
| 13 | `wait.c` `wait_queue_sleep()` schedule 返回 | **当前任务**自己恢复 | ❌ 无需 |
| 14 | `futex.c` `do_futex_wait()` schedule 返回 | **当前任务**自己恢复 | ❌ 无需 |
| 15 | `tty.c` `tty_read()` 二次检查 | **当前任务**继续循环 | ❌ 无需 |
| 16 | `trap.c` `SYS_nanosleep` 超时 | **当前任务**自己恢复 | ❌ 无需 |
| 17 | `task.c` `task_init()` idle loop | idle 初始 RUNNING | ❌ idle 不入 rbtree |
| 18 | `smp.c` `create_idle_task()` | AP idle 初始 RUNNING | ❌ idle 不入 rbtree |

**必须改的 9 个点**（`✅`），**无需改的 9 个点**是当前任务自己恢复运行（已在 rbtree 上被 `schedule()` 的 step 2 处理）。

**锁顺序安全性**：
- `tty_wake_waiters` 持有 tty_lock → 内部 `task_wake` 获取 rq_lock。tty_lock → rq_lock 是安全顺序（仅此方向）。
- `sched_unblock_blocked` 在 `schedule()` 内部，持有 reap_lock → 内部 `task_wake` 获取 rq_lock。reap_lock 和 rq_lock 是不同的锁，且 reap_lock 临界区无 rbtree 操作，`task_wake` 可能获取另一个 CPU 的 rq_lock，无 ABBA 风险。
- `wait_queue_wake_one/all` 在 schedule() 外部调用：`task_wake` 获取 rq_lock 并 enqueue。被唤醒任务从 `schedule()` 返回后自己清理 `io_wait_node`。`task_wake` 的 `on_rq` guard 防止重复入队。

### 6.5 `task_init()` + `spawn_user_task()`

`percpu_data[i].run_queue` 已在 `percpu_init()` 中通过 `rbtree_init()` 初始化，`min_vruntime = 0`，`rq_lock.lock = 1L`。`task_init()` 无需额外操作。

**关键**：`spawn_user_task()` 当前返回 `int64_t pid`。需改为返回 `task_t*`（或新增内部版本），调用 `task_wake()` 将 init 任务放入 rbtree：

```c
// task_init() 中：
task_t *init_tsk = spawn_user_task("/bin/init", NULL);
task_wake(init_tsk);  // 入队到 CPU 0 的 runqueue
```

这样 `scheduler_ok=1` 后，第一次 `schedule()` → `pick_eevdf()` 就能从 rbtree 选中 init 任务。

---

## 7. 边界条件

| 场景 | 处理 |
|------|------|
| **BSP 双 timer 同一 tick 两次 schedule()** | LAPIC timer 在 BSP(cpu_id==0) 上跳过 `need_resched=1`，PIT 为唯一调度 tick 源。每个 CPU 每 tick 恰好一次 schedule() |
| **队列空** | `pick_eevdf()` 返回 `idle`，`schedule()` 直接 `switch_to(idle)` |
| **唤醒任务 vruntime 远落后** | `task_wake()` 将 vruntime 推到 `min_vruntime - LATENCY`，防止刚唤醒的任务因"欠账"长时间占住 CPU |
| **最左节点不 eligible** | `pick_eevdf()` 推进 `min_vruntime` 到该节点的 `vruntime`，返回该节点。不扫描树。 |
| **任务退出 (do_exit)** | `state = TASK_ZOMBIE` → `schedule()` step 2 无条件 dequeue current → zombie reaper 回收 |
| **min_vruntime 高于某些任务 vruntime** | 正常行为——`task_wake()` 将睡眠唤醒任务 vruntime 推到 `min_vruntime - LATENCY`。这些"欠账"任务是优先候选（eligible 恒成立），正的公平调度语义 |
| **idle task** | `update_curr()` 跳过 idle；idle 永不在 rbtree 中 |
| **同 deadline 碰撞** | `cmp_deadline` 用 pid 打破平局，指针比较兜底，`enqueue_task` 有 `ASSERT` 防止重复入队 |
| **当前任务被 task_wake 提前入队** | `schedule()` step 2 先 dequeue 再 enqueue，保证 deadline 用最新 vruntime 计算 |
| **非 timer 路径调 schedule()** | 阻塞 I/O 等主动让出 CPU 的任务 vruntime 仍加 1 tick（虚增）。v1 不做 TSC 精度跟踪，影响小 |

---

## 8. 文件变更清单

| 文件 | 操作 | 估计行数 | 说明 |
|------|------|----------|------|
| `libc/include/rbtree.h` | **新增** | ~55 | rbtree API 声明 + inline helpers |
| `libc/rbtree/rbtree.c` | **新增** | ~160 | insert/erase/first/next 完整实现 |
| `libc/Makefile` | **修改** | +1 | 加 `$(wildcard rbtree/*.c)` |
| `kernel/include/kernel/task.h` | **修改** | +6 | task_t 加 4 字段 + include rbtree.h |
| `kernel/include/kernel/percpu.h` | **修改** | +4 | run_queue 类型变更, min_vruntime, rq_lock |
| `kernel/percpu/percpu.c` | **修改** | +4 | percpu_init() 中加 rbtree_init + rq_lock 初始化 |
| `kernel/sched/task.c` | **重写调度区 + do_fork/do_exit/spawn** | +220/-140 | schedule()/enqueue/dequeue/pick_eevdf/update_curr/task_wake/do_exit 修/do_fork 修/spawn 修 |
| `kernel/intr/wait.c` | **修改** | +2/-2 | wait_queue_wake_one/all → task_wake |
| `kernel/futex.c` | **修改** | +1/-1 | do_futex_wake → task_wake |
| `kernel/tty/tty.c` | **修改** | +1/-1 | tty_wake_waiters → task_wake |
| `kernel/arch/x86_64/trap.c` | **修改** | +1/-1 | SYS_kill → task_wake（保持 INTERRUPTIBLE 检查） |
| `kernel/apic/lapic_timer.c` | **修改** | +2 | BSP 不设 need_resched（§6.1） |
| `kernel/include/kernel/assert.h` | **新增** | ~8 | 内核 ASSERT 宏 |
| `kernel/sched/task.c` (do_fork) | 归入上方 | — | vruntime 公平起点 + enqueue_task + 删除 counter 继承 |
| `kernel/sched/task.c` (spawn_user_task) | 归入上方 | — | 返回 task_t* + task_wake |

**统计**：~+490 行，~-140 行，5 新文件（含 Makefile 1 行），**10 修改文件**。

---

## 9. 测试策略

### 9.1 rbtree 单元测试（systest 新增）

- `test_rbtree_insert_erase_inorder` — 插入 5 节点，验证 first/next 遍历为升序
- `test_rbtree_erase_middle` — 删除中间节点，验证剩余节点顺序
- `test_rbtree_stress_1000` — 随机插入 1000 节点，按序遍历验证升序，随机逐个删除，验证树结构一致性
- `test_rbtree_rebalance_delete_root` — 删除根节点/叶节点/单子节点验证红黑树再平衡
- `test_rbtree_same_key_roundtrip` — 同 key 插入→erase→再插入，验证树不退化

### 9.2 调度公平性测试（systest 新增）

- `test_eevdf_two_tasks_fair` — fork 2 子进程, 各跑 5s CPU 密集计算（累计 tick 计数 ≈ 500 ticks/task），完成 tick 偏差 ≤5%
- `test_eevdf_sleeper_no_starvation` — 父进程 sleep 后返回, 必须在 2×LATENCY ticks 内被调度
- `test_eevdf_fork_child_scheduled` — fork 后子进程必定出现在 rbtree 上并被调度到
- `test_eevdf_three_tasks_interleaved` — 3 个 CPU 密集任务，验证每个至少获得 N±10% ticks
- `test_eevdf_exit_cleanup` — 任务 exit 后其 rb_node 不在树中，不会引起悬垂指针
- `test_eevdf_fork_large_parent` — 长时间运行的父进程 fork，子进程不被饥饿（验证 fork 公平起点）
- `test_eevdf_wakeup_cross_cpu` — CPU A 唤醒 CPU B 上的任务，B 最终调度到它
- `test_eevdf_empty_rq_all_blocked` — 所有任务阻塞后 pick_eevdf 返回 idle

### 9.3 回归测试

- systest 118/118 全部通过（`make test`）
- busybox ash 交互测试：方向键行编辑、Ctrl-C→SIGINT、管道、重定向
- SMP 验证：`make run SMP=2` 两核正常运行用户任务

---

## 10. 风险与缓解

| 风险 | 严重度 | 缓解 |
|------|--------|------|
| **rbtree bug 导致调度崩溃** | 高 | rbtree 先在 systest 用 1000 节点随机压力测试彻底验证后再接调度器 |
| **唤醒路径遗漏 task_wake** | 高 | 完整审计 18 个赋值点（§6.4），9 个必须改的已列出文件清单 |
| **do_exit 直接 switch_to(parent)** | 高 | 删除直接 switch_to，统一走 schedule()（§6.3） |
| **BSP PIT+LAPIC 同 tick 双调 schedule()** | 高 | LAPIC timer 在 BSP 上跳过 need_resched（§6.1），PIT 为唯一调度 tick 源 |
| **min_vruntime 溢出** | 低 | `uint64_t` 在 100Hz 下需 58 亿年溢出 |
| **LATENCY 参数不当** | 低 | 用 systest 验证 2-4 任务负载，如有过度延迟再调整 |
| **spawn_user_task API 改变** | 中 | 返回值从 pid 改为 task_t*，仅 task_init 调用 |

---

## 11. 与当前调度器的对比

| 维度 | 当前 (max-counter) | EEVDF v1 |
|------|-------------------|----------|
| 选择复杂度 | O(n) 全局扫描 | O(log n) rbtree (insert/erase/first) |
| 公平性 | 无保证（counter 大的先跑） | 每任务等量 CPU ticks (vruntime) |
| 反饿死 | counter 定期刷新（弱保护） | 唤醒 vruntime 追赶 min_vruntime |
| 数据结构 | 全局链表 1 个 | per-CPU rbtree |
| 锁粒度 | 全局 reap_lock + 无 rq 锁 | reap_lock (zombie) + rq_lock (rbtree) |
| 时间片 | priority 决定（2-10 ticks） | 固定 EEVDF_MIN_SLICE=10 ticks |
| counter/priority 字段 | 核心决策依据 | 保留但不使用 |
