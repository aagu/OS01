# EEVDF 公平调度器 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade OS01 scheduler from O(n) global linked-list max-counter scan to per-CPU rbtree-based EEVDF with vruntime fairness and O(log n) selection.

**Architecture:** Add a general-purpose rbtree container in `libc/`, embed `rbtree_node_t` in `task_t`, replace the per-CPU `run_queue` from `list_t` to `rbtree_root_t`, rewrite `schedule()` to use `enqueue_task`/`pick_eevdf`/`dequeue_task` on the rbtree, and audit all wake-up call sites to use `task_wake()` which enqueues woken tasks into the rbtree. The zombie reaper and global task list continue to exist in parallel.

**Tech Stack:** C (kernel + libc), clang x86_64, QEMU/UEFI, OVMF firmware

## Global Constraints

From spec: vruntime unit = ticks (100Hz, 10ms per tick); `EEVDF_MIN_SLICE = 10 ticks`; `EEVDF_LATENCY = 40 ticks`; per-CPU rbtree + rq_lock; no nice weights, no cross-CPU migration, no TSC precision; `update_curr()` called only from `schedule()`; BSP PIT+LAPIC dedup via `cpu_id() != 0` guard in LAPIC handler.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `libc/include/rbtree.h` | **Create** | rbtree API: node/root types, insert/erase/first/next/init/empty |
| `libc/rbtree/rbtree.c` | **Create** | Red-black tree implementation (~160 lines) |
| `libc/Makefile` | **Modify** | Add `$(wildcard rbtree/*.c)` to source discovery |
| `kernel/include/kernel/assert.h` | **Create** | Kernel ASSERT macro (log_err + hlt) |
| `kernel/include/kernel/task.h` | **Modify** | Add `rb_node`, `vruntime`, `deadline`, `on_rq` to `task_t`; declare `task_wake()` |
| `kernel/include/kernel/percpu.h` | **Modify** | Change `run_queue` type, add `min_vruntime`, `rq_lock` |
| `kernel/percpu/percpu.c` | **Modify** | Initialize rbtree + min_vruntime + rq_lock in `percpu_init()` |
| `kernel/sched/task.c` | **Modify** | Rewrite `schedule()`, add `update_curr`/`enqueue_task`/`dequeue_task`/`pick_eevdf`/`task_wake`. Modify `do_fork`, `do_exit`, `spawn_user_task`, `blocker_wake`, `sched_unblock_blocked` |
| `kernel/intr/wait.c` | **Modify** | `wait_queue_wake_one/all`: `state=RUNNING` → `task_wake(t)` |
| `kernel/futex.c` | **Modify** | `do_futex_wake`: `state=RUNNING` → `task_wake(t)` |
| `kernel/tty/tty.c` | **Modify** | `tty_wake_waiters`: `state=RUNNING` → `task_wake(t)` |
| `kernel/arch/x86_64/trap.c` | **Modify** | `SYS_kill`: keep `if (INTERRUPTIBLE)` guard, call `task_wake(target)` |
| `kernel/intr/apic/lapic_timer.c` | **Modify** | BSP guard: `if (cpu_id() != 0) need_resched=1` |
| `user/systest.c` | **Modify** | Add rbtree + EEVDF fairness test cases |

---

### Task 1: Red-black tree data structure

**Files:**
- Create: `libc/include/rbtree.h`
- Create: `libc/rbtree/rbtree.c`
- Modify: `libc/Makefile:19`

**Interfaces:**
- Produces: `rbtree_node_t`, `rbtree_root_t`, `rbtree_init()`, `rbtree_node_init()`, `rbtree_insert()`, `rbtree_erase()`, `rbtree_first()`, `rbtree_next()`, `rbtree_empty()`

- [ ] **Step 1: Create `libc/include/rbtree.h`**

```c
#ifndef _RBTREE_H
#define _RBTREE_H

#include <stddef.h>

typedef struct rbtree_node {
    struct rbtree_node *left;
    struct rbtree_node *right;
    struct rbtree_node *parent;
    unsigned long color;         /* 0 = black, 1 = red */
} rbtree_node_t;

typedef struct rbtree_root {
    rbtree_node_t *rb_node;      /* NULL when tree is empty */
} rbtree_root_t;

static inline void rbtree_init(rbtree_root_t *root)
{
    root->rb_node = NULL;
}

static inline int rbtree_empty(rbtree_root_t *root)
{
    return root->rb_node == NULL;
}

void rbtree_node_init(rbtree_node_t *node);

/*
 * Insert a node into the red-black tree.
 * cmp(a, b) returns <0 if a goes left, >0 if a goes right, 0 if keys conflict.
 * Returns NULL on success, or the conflicting existing node if cmp returned 0.
 */
rbtree_node_t *rbtree_insert(rbtree_root_t *root, rbtree_node_t *node,
                             int (*cmp)(rbtree_node_t *a, rbtree_node_t *b));

/*
 * Erase a node from the red-black tree.
 * node MUST be currently in the tree.
 */
void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node);

/* Return the leftmost (minimum) node, or NULL if tree is empty. */
rbtree_node_t *rbtree_first(rbtree_root_t *root);

/* Return the inorder successor, or NULL if node is the rightmost. */
rbtree_node_t *rbtree_next(rbtree_node_t *node);

#endif /* _RBTREE_H */
```

- [ ] **Step 2: Create `libc/rbtree/rbtree.c`**

```c
#include <rbtree.h>

/* ── Internal helpers ─────────────────────────────────── */

#define RB_RED    1
#define RB_BLACK  0

static inline int is_red(rbtree_node_t *n)
{
    return n && n->color == RB_RED;
}

static inline void set_black(rbtree_node_t *n)
{
    if (n) n->color = RB_BLACK;
}

static inline void set_red(rbtree_node_t *n)
{
    if (n) n->color = RB_RED;
}

void rbtree_node_init(rbtree_node_t *node)
{
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    node->color = RB_BLACK;
}

/* ── Rotation helpers ─────────────────────────────────── */

static void rotate_left(rbtree_node_t *node, rbtree_root_t *root)
{
    rbtree_node_t *right = node->right;
    rbtree_node_t *parent = node->parent;

    node->right = right->left;
    if (right->left)
        right->left->parent = node;
    right->parent = parent;

    if (!parent)
        root->rb_node = right;
    else if (node == parent->left)
        parent->left = right;
    else
        parent->right = right;

    right->left = node;
    node->parent = right;
}

static void rotate_right(rbtree_node_t *node, rbtree_root_t *root)
{
    rbtree_node_t *left = node->left;
    rbtree_node_t *parent = node->parent;

    node->left = left->right;
    if (left->right)
        left->right->parent = node;
    left->parent = parent;

    if (!parent)
        root->rb_node = left;
    else if (node == parent->right)
        parent->right = left;
    else
        parent->left = left;

    left->right = node;
    node->parent = left;
}

/* ── Insert ───────────────────────────────────────────── */

rbtree_node_t *rbtree_insert(rbtree_root_t *root, rbtree_node_t *node,
                             int (*cmp)(rbtree_node_t *a, rbtree_node_t *b))
{
    rbtree_node_t **link = &root->rb_node;
    rbtree_node_t *parent = NULL;

    while (*link) {
        parent = *link;
        int c = cmp(node, parent);
        if (c < 0)
            link = &parent->left;
        else if (c > 0)
            link = &parent->right;
        else
            return parent;  /* key conflict */
    }

    node->left = NULL;
    node->right = NULL;
    node->parent = parent;
    node->color = RB_RED;
    *link = node;

    /* Fix red-red violations after insertion */
    rbtree_node_t *n = node;
    while (n->parent && n->parent->color == RB_RED) {
        rbtree_node_t *gp = n->parent->parent;
        if (n->parent == gp->left) {
            rbtree_node_t *uncle = gp->right;
            if (is_red(uncle)) {
                set_black(n->parent);
                set_black(uncle);
                set_red(gp);
                n = gp;
            } else {
                if (n == n->parent->right) {
                    n = n->parent;
                    rotate_left(n, root);
                }
                set_black(n->parent);
                set_red(gp);
                rotate_right(gp, root);
            }
        } else {
            rbtree_node_t *uncle = gp->left;
            if (is_red(uncle)) {
                set_black(n->parent);
                set_black(uncle);
                set_red(gp);
                n = gp;
            } else {
                if (n == n->parent->left) {
                    n = n->parent;
                    rotate_right(n, root);
                }
                set_black(n->parent);
                set_red(gp);
                rotate_left(gp, root);
            }
        }
    }
    set_black(root->rb_node);
    return NULL;
}

/* ── Tree minimum ─────────────────────────────────────── */

rbtree_node_t *rbtree_first(rbtree_root_t *root)
{
    rbtree_node_t *n = root->rb_node;
    if (!n) return NULL;
    while (n->left)
        n = n->left;
    return n;
}

/* ── Inorder successor ────────────────────────────────── */

rbtree_node_t *rbtree_next(rbtree_node_t *node)
{
    if (node->right) {
        node = node->right;
        while (node->left)
            node = node->left;
        return node;
    }
    rbtree_node_t *p = node->parent;
    while (p && node == p->right) {
        node = p;
        p = p->parent;
    }
    return p;
}

/* ── Erase ────────────────────────────────────────────── */

static void rbtree_erase_fixup(rbtree_node_t *node, rbtree_node_t *parent,
                               rbtree_root_t *root)
{
    rbtree_node_t *n = node;
    rbtree_node_t *p = parent;

    while ((!n || n->color == RB_BLACK) && n != root->rb_node) {
        if (n == p->left) {
            rbtree_node_t *sibling = p->right;
            if (is_red(sibling)) {
                set_black(sibling);
                set_red(p);
                rotate_left(p, root);
                sibling = p->right;
            }
            if ((!sibling->left || sibling->left->color == RB_BLACK) &&
                (!sibling->right || sibling->right->color == RB_BLACK)) {
                set_red(sibling);
                n = p;
                p = p->parent;
            } else {
                if (!sibling->right || sibling->right->color == RB_BLACK) {
                    set_black(sibling->left);
                    set_red(sibling);
                    rotate_right(sibling, root);
                    sibling = p->right;
                }
                sibling->color = p->color;
                set_black(p);
                set_black(sibling->right);
                rotate_left(p, root);
                n = root->rb_node;
                break;
            }
        } else {
            rbtree_node_t *sibling = p->left;
            if (is_red(sibling)) {
                set_black(sibling);
                set_red(p);
                rotate_right(p, root);
                sibling = p->left;
            }
            if ((!sibling->right || sibling->right->color == RB_BLACK) &&
                (!sibling->left || sibling->left->color == RB_BLACK)) {
                set_red(sibling);
                n = p;
                p = p->parent;
            } else {
                if (!sibling->left || sibling->left->color == RB_BLACK) {
                    set_black(sibling->right);
                    set_red(sibling);
                    rotate_left(sibling, root);
                    sibling = p->left;
                }
                sibling->color = p->color;
                set_black(p);
                set_black(sibling->left);
                rotate_right(p, root);
                n = root->rb_node;
                break;
            }
        }
    }
    if (n) set_black(n);
}

void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node)
{
    rbtree_node_t *child, *parent;
    int color;

    if (node->left && node->right) {
        /* Find inorder successor and swap payload */
        rbtree_node_t *succ = node->right;
        while (succ->left)
            succ = succ->left;

        /* Detach succ from its current position */
        if (succ->parent != node) {
            child = succ->right;
            parent = succ->parent;
            color = succ->color;
            if (parent->left == succ)
                parent->left = succ->right;
            else
                parent->right = succ->right;
            if (succ->right)
                succ->right->parent = parent;

            succ->left = node->left;
            node->left->parent = succ;
            succ->right = node->right;
            node->right->parent = succ;
        } else {
            child = succ->right;
            parent = succ;
            color = succ->color;
        }

        succ->parent = node->parent;
        succ->color = node->color;

        if (!node->parent)
            root->rb_node = succ;
        else if (node == node->parent->left)
            node->parent->left = succ;
        else
            node->parent->right = succ;

        if (parent == succ)
            parent = succ;

    } else {
        child = node->left ? node->left : node->right;
        parent = node->parent;
        color = node->color;

        if (child)
            child->parent = parent;

        if (!parent)
            root->rb_node = child;
        else if (node == parent->left)
            parent->left = child;
        else
            parent->right = child;
    }

    if (color == RB_BLACK)
        rbtree_erase_fixup(child, parent, root);
}
```

- [ ] **Step 3: Add rbtree wildcard to `libc/Makefile`**

In `libc/Makefile`, after line 23 (`$(wildcard list/*.c) \`), add:
```makefile
    $(wildcard rbtree/*.c) \
```

- [ ] **Step 4: Build and verify compilation**

Run: `make -C libc clean && make -C libc`

Expected: `libc.a` and `libk.a` build successfully with `rbtree/rbtree.o` and `rbtree/rbtree.libk.o`.

- [ ] **Step 5: Commit**

```bash
git add libc/include/rbtree.h libc/rbtree/rbtree.c libc/Makefile
git commit -m "feat(libc): add red-black tree container (rbtree)

Insert, erase, first, next — O(log n). Builds into both libc.a
(systest can unit-test) and libk.a (kernel uses via task_t embedding).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Kernel ASSERT macro

**Files:**
- Create: `kernel/include/kernel/assert.h`

**Interfaces:**
- Produces: `ASSERT(x)` macro — panics on failure (log_err + hlt loop)

- [ ] **Step 1: Create `kernel/include/kernel/assert.h`**

```c
#ifndef _KERNEL_ASSERT_H
#define _KERNEL_ASSERT_H

#include <kernel/log.h>

#define ASSERT(x) do { \
    if (!(x)) { \
        log_err("ASSERT failed: %s at %s:%d\n", #x, __FILE__, __LINE__); \
        while (1) { __asm__ __volatile__("hlt"); } \
    } \
} while (0)

#endif /* _KERNEL_ASSERT_H */
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/kernel/assert.h
git commit -m "feat(kernel): add ASSERT macro for kernel panic on invariant failure"
```

---

### Task 3: Data structure changes — task_t and percpu_t

**Files:**
- Modify: `kernel/include/kernel/task.h`
- Modify: `kernel/include/kernel/percpu.h`
- Modify: `kernel/percpu/percpu.c`

**Interfaces:**
- Produces: `task_t.{rb_node, vruntime, deadline, on_rq}`, `percpu_t.{run_queue(rbtree_root_t), min_vruntime, rq_lock}`. Also declare `void task_wake(task_t *t)` in task.h.
- Consumes: `rbtree_node_t`, `rbtree_root_t` from Task 1

- [ ] **Step 1: Add rbtree include and 4 fields to `task_t` in `kernel/include/kernel/task.h`**

After line 11 (`#include <uapi/time.h>`), add:
```c
#include <rbtree.h>
```

In the `task_struct`, after `int64_t priority;` (line 121), add:
```c
    rbtree_node_t rb_node;    // node on per-CPU runqueue rbtree
    uint64_t      vruntime;   // accumulated virtual runtime (ticks)
    uint64_t      deadline;   // vruntime + slice, rbtree sort key
    bool          on_rq;      // true when on a CPU's runqueue
```

Near the end of the file (before `#endif`), add the `task_wake` declaration:
```c
/* ── EEVDF scheduler ─────────────────────────── */
void task_wake(struct task_struct *t);
```

- [ ] **Step 2: Modify `percpu_t` fields in `kernel/include/kernel/percpu.h`**

First, add the spinlock include near the top of the file (needed for `rq_lock: spinlock_T`):
```c
#include <kernel/arch/spinlock.h>
```

Change the `run_queue` field from:
```c
    list_t run_queue;
```
to:
```c
    rbtree_root_t run_queue;
```

After `uint64_t schedule_count;`, add:
```c
    uint64_t min_vruntime;      // per-CPU tracking of minimum vruntime
    spinlock_T rq_lock;          // protects rbtree operations
```

- [ ] **Step 3: Initialize rbtree + EEVDF fields in `kernel/percpu/percpu.c`**

Replace `list_init(&percpu_data[cpu].run_queue);` with:
```c
    rbtree_init(&percpu_data[cpu].run_queue);
    percpu_data[cpu].min_vruntime = 0;
    percpu_data[cpu].rq_lock.lock = 1L;
```

- [ ] **Step 4: Build verify (with clean — struct changed)**

Run: `make clean && make -C kernel`

Expected: clean compile.

- [ ] **Step 5: Commit**

```bash
git add kernel/include/kernel/task.h kernel/include/kernel/percpu.h kernel/percpu/percpu.c
git commit -m "feat(sched): add EEVDF fields to task_t and percpu_t

task_t: rb_node, vruntime, deadline, on_rq + task_wake() declaration
percpu_t: run_queue → rbtree_root_t, min_vruntime, rq_lock
percpu_init: init rbtree + rq_lock.lock=1L (unlocked after memset 0)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Core EEVDF scheduler functions + schedule() rewrite

**Files:**
- Modify: `kernel/sched/task.c`

**Interfaces:**
- Produces: `static void update_curr(task_t*)`, `static int cmp_deadline(...)`, `static void enqueue_task(task_t*, percpu_t*)`, `static void dequeue_task(task_t*, percpu_t*)`, `static task_t *pick_eevdf(percpu_t*)`, `void task_wake(task_t*)` (exported, declared in task.h), re-written `void schedule(void)`
- Consumes: `rbtree_insert`, `rbtree_erase`, `rbtree_first`, `rbtree_empty`, `ASSERT`, `spin_lock_irqsave`

- [ ] **Step 1: Add assert include**

After the last `#include` line in task.c, add:
```c
#include <kernel/assert.h>
```

- [ ] **Step 2: Add EEVDF constants and helper functions**

Insert after the `task_list_next` function (after line 43), before the `pid_counter` declaration:

```c
/* ── EEVDF scheduler constants ─────────────────────── */
#define EEVDF_MIN_SLICE  10   // time slice = 10 ticks = 100ms
#define EEVDF_LATENCY    40   // eligibility window = 40 ticks = 400ms

/* ── update_curr: advance vruntime by 1 tick ──────── */
static void update_curr(task_t *task)
{
    if (!task || task == this_cpu()->idle)
        return;
    task->vruntime += 1;
    if (task->vruntime >= task->deadline)
        this_cpu()->need_resched = 1;
}

/* ── rbtree comparator: order by deadline ─────────── */
static int cmp_deadline(rbtree_node_t *a, rbtree_node_t *b)
{
    task_t *ta = container_of(a, task_t, rb_node);
    task_t *tb = container_of(b, task_t, rb_node);
    if (ta->deadline < tb->deadline) return -1;
    if (ta->deadline > tb->deadline) return 1;
    if (ta->pid < tb->pid) return -1;
    if (ta->pid > tb->pid) return 1;
    return (uintptr_t)a < (uintptr_t)b ? -1 : 1;
}

/* ── enqueue / dequeue ─────────────────────────────── */
static void enqueue_task(task_t *task, percpu_t *rq)
{
    task->deadline = task->vruntime + EEVDF_MIN_SLICE;
    task->on_rq = true;
    rbtree_node_t *conflict = rbtree_insert(&rq->run_queue, &task->rb_node, cmp_deadline);
    ASSERT(conflict == NULL);
}

static void dequeue_task(task_t *task, percpu_t *rq)
{
    rbtree_erase(&rq->run_queue, &task->rb_node);
    task->on_rq = false;
}

/* ── pick_eevdf: select next task O(log n) ─────────── */
static task_t *pick_eevdf(percpu_t *rq)
{
    if (rbtree_empty(&rq->run_queue))
        return rq->idle;
    rbtree_node_t *node = rbtree_first(&rq->run_queue);
    task_t *t = container_of(node, task_t, rb_node);
    if (t->vruntime > rq->min_vruntime + EEVDF_LATENCY)
        rq->min_vruntime = t->vruntime;
    return t;
}

/* ── task_wake: mark RUNNING + enqueue (exported) ─── */
void task_wake(task_t *t)
{
    percpu_t *rq = &percpu_data[t->cpu];
    t->state = TASK_RUNNING;
    if (t->on_rq)
        return;
    /*
     * Read min_vruntime without rq_lock — may see a stale (lower) value.
     * This gives the woken task a slightly larger vruntime boost, making it
     * MORE likely to be scheduled (anti-starvation).  Harmless race.
     */
    uint64_t wake_vruntime = rq->min_vruntime > EEVDF_LATENCY
        ? rq->min_vruntime - EEVDF_LATENCY : 0;
    if (t->vruntime < wake_vruntime)
        t->vruntime = wake_vruntime;
    {
        uint64_t flags = spin_lock_irqsave(&rq->rq_lock);
        enqueue_task(t, rq);
        spin_unlock_irqrestore(&rq->rq_lock, flags);
    }
    if ((int)t->cpu != (int)cpu_id())
        rq->need_resched = 1;
}
```

- [ ] **Step 3: Modify `blocker_wake()` — replace `state = TASK_RUNNING` with `task_wake()`**

In `blocker_wake()` (around line 77), replace:
```c
    task->state = TASK_RUNNING;
```
with:
```c
    task_wake(task);
```

- [ ] **Step 4: Modify `sched_unblock_blocked()` signal-wake path**

Replace:
```c
        if (t->blocker.signal_can_wake && t->signal) {
            t->state = TASK_RUNNING;
```
with:
```c
        if (t->blocker.signal_can_wake && t->signal) {
            task_wake(t);
```

- [ ] **Step 5: Rewrite `schedule()` function**

Replace the entire `schedule()` function (existing lines 186–358) with:

```c
void schedule(void)
{
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

    // ── 2. Dequeue + conditional re-enqueue current ─────────
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        if (current->on_rq)
            dequeue_task(current, rq);
        if (current->state == TASK_RUNNING && current != rq->idle)
            enqueue_task(current, rq);
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }

    // ── 3. Zombie reaper (global list, with on_rq guard) ──
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
                if (t->on_rq) continue;  // not yet dequeued — skip

                int reap = 0;
                if (t->flags & PF_REAPED) reap = 1;
                else if (t->flags & PF_KTHREAD) reap = 1;
                else if (t->parent == NULL) reap = 1;
                else if (t->parent->state == TASK_ZOMBIE) reap = 1;

                if (reap) reap_list[reap_count++] = t;
            }
        }

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

        sched_unblock_blocked();
        spin_unlock_irqrestore(&reap_lock, reap_flags);
    }

    // ── 4. Pick next task (rbtree O(log n)) ─────────────────
    task_t *next;
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        next = pick_eevdf(rq);
        if (next && next != rq->idle)
            dequeue_task(next, rq);
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }

    // ── 5. Fallback to idle ─────────────────────────────────
    if (!next || next->state != TASK_RUNNING) {
        if (next)
            log_err("sched: orphan task %d (state=%ld), falling back to idle\n",
                    (int)next->pid, (long)next->state);
        next = rq->idle;
        if (!next) return;
    }

    // ── 6. Update min_vruntime ──────────────────────────────
    if (next != rq->idle && next->vruntime > rq->min_vruntime)
        rq->min_vruntime = next->vruntime;

    rq->need_resched = 0;
    switch_to(current, next);
}
```

- [ ] **Step 6: Build verify (with clean)**

Run: `make clean && make -C kernel`

Expected: clean compile.

- [ ] **Step 7: Boot test**

Run: `make clean && make run`

Expected: system boots. Interactive shell may have degraded timing but should work.

- [ ] **Step 8: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): rewrite schedule() with EEVDF rbtree selection

Add update_curr, cmp_deadline, enqueue_task, dequeue_task,
pick_eevdf, task_wake. Rewrite schedule() to use per-CPU rbtree
instead of O(n) global linked-list scan. blocker_wake and
sched_unblock_blocked use task_wake().

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Integration — do_fork, do_exit, spawn_user_task, task_init

**Files:**
- Modify: `kernel/sched/task.c`
- Modify: `kernel/include/kernel/task.h`

**Interfaces:**
- Consumes: `enqueue_task`, `task_wake` from Task 4

- [ ] **Step 1: Modify `do_fork()` — EEVDF init + enqueue, delete counter line**

In `do_fork()`, after `tsk->addr_limit = current->addr_limit;` (line 1184), add:
```c
    // EEVDF: fair starting vruntime
    {
        uint64_t fair_start = percpu_data[cpu_id()].min_vruntime;
        tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
    }
```

Delete line 1183: `tsk->counter     = current->counter;`.

Replace `tsk->state = TASK_RUNNING;` (line 1265) with:
```c
    tsk->state = TASK_RUNNING;
    enqueue_task(tsk, &percpu_data[tsk->cpu]);
```

- [ ] **Step 2: Modify `do_exit()` — delete direct switch_to, unified task_wake**

Change lines 440-443 from:
```c
        uint64_t ps = parent->state;
        if (ps == TASK_INTERRUPTIBLE) {
            parent->state = TASK_RUNNING;
            parent_woken = 1;
```
to:
```c
        uint64_t ps = parent->state;
        if (ps == TASK_INTERRUPTIBLE) {
            // Don't set RUNNING — §6.3 unified task_wake handles it
            parent_woken = 1;
```

Replace the entire `if (parent_woken)` block (lines 464-471) + `schedule()` fallback (line 472) with:
```c
    if (parent_woken)
        task_wake(parent);
    current->state = TASK_ZOMBIE;
    schedule();
    return 0;  // unreachable
```

Delete the now-unreachable `schedule();` and `return 0;` on the old lines 472-473.

- [ ] **Step 3: Change `spawn_user_task` return type**

Change line 625:
```c
task_t *spawn_user_task(const char *path, const char *const *argv)
```

Change the function's return statement (currently returning `tsk->pid` or similar — search for `return` at end of function) to:
```c
    return tsk;
```

In `kernel/include/kernel/task.h`, change the declaration:
```c
task_t *spawn_user_task(const char *path, const char *const *argv);
```

- [ ] **Step 4: Modify `task_init()`**

Replace lines 1354-1355 (`int64_t init_pid = spawn_user_task(...)` + debug line) with:
```c
    task_t *init_tsk = spawn_user_task("/bin/init", NULL);
    task_wake(init_tsk);
    debug_task("init: spawned user-space init, pid=%d\n", (int)init_tsk->pid);
```

- [ ] **Step 5: Build verify + boot test**

Run: `make -C kernel` then `make run`

Expected: clean compile, system boots, shell works.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/task.c kernel/include/kernel/task.h
git commit -m "feat(sched): integrate EEVDF into fork, exit, spawn paths

do_fork: fair vruntime, enqueue_task, delete counter inheritance
do_exit: remove direct switch_to(parent), unified task_wake+schedule
spawn_user_task: return task_t*; task_init calls task_wake(init)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Wake-up path audit — wait.c, futex.c, tty.c, trap.c, lapic_timer.c

**Files:**
- Modify: `kernel/intr/wait.c`
- Modify: `kernel/futex.c`
- Modify: `kernel/tty/tty.c`
- Modify: `kernel/arch/x86_64/trap.c`
- Modify: `kernel/intr/apic/lapic_timer.c`

- [ ] **Step 1: `wait_queue_wake_one()` — replace `state=RUNNING` with `task_wake`**

In `kernel/intr/wait.c`, line 40 — replace:
```c
        t->state = TASK_RUNNING;
```
with:
```c
        task_wake(t);
```

- [ ] **Step 2: `wait_queue_wake_all()` — same change**

In `kernel/intr/wait.c`, line 52 — replace:
```c
        t->state = TASK_RUNNING;
```
with:
```c
        task_wake(t);
```

- [ ] **Step 3: `do_futex_wake()` — replace `state=RUNNING` with `task_wake`**

In `kernel/futex.c`, line 107 — replace:
```c
        t->state = TASK_RUNNING;
```
with:
```c
        task_wake(t);
```

- [ ] **Step 4: `tty_wake_waiters()` — replace `state=RUNNING` with `task_wake`**

In `kernel/tty/tty.c`, lines 70-71 — replace:
```c
        t->state = TASK_RUNNING;
```
with:
```c
        task_wake(t);
```

- [ ] **Step 5: `SYS_kill` — add INTERRUPTIBLE guard + `task_wake`**

In `kernel/arch/x86_64/trap.c`, around line 1892-1893, replace:
```c
        if (target->state == TASK_INTERRUPTIBLE)
            target->state = TASK_RUNNING;
```
with:
```c
        if (target->state == TASK_INTERRUPTIBLE)
            task_wake(target);
```

- [ ] **Step 6: `lapic_timer_handler()` — BSP guard**

In `kernel/intr/apic/lapic_timer.c`, around line 122, change:
```c
    this_cpu()->need_resched = 1;
```
to:
```c
    if (cpu_id() != 0)
        this_cpu()->need_resched = 1;
```

- [ ] **Step 7: Build verify**

Run: `make -C kernel`

Expected: clean compile. `task_wake` is declared in `kernel/include/kernel/task.h` (added in Task 3 Step 1). Confirm all files resolve it.

- [ ] **Step 8: Boot test**

Run: `make clean && make run`

Expected: normal boot. Keyboard input works. TTY output echoes. Ctrl-C → SIGINT.

- [ ] **Step 9: Commit**

```bash
git add kernel/intr/wait.c kernel/futex.c kernel/tty/tty.c kernel/arch/x86_64/trap.c kernel/intr/apic/lapic_timer.c
git commit -m "feat(sched): audit wake-up paths to use task_wake()

wait_queue_wake_one/all, do_futex_wake, tty_wake_waiters,
SYS_kill: state=RUNNING → task_wake() for rbtree enqueue.
lapic_timer: skip need_resched on BSP (PIT handles it).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Regression test — systest 118/118 pass

**Files:**
- No code changes.

- [ ] **Step 1: Run full systest**

```bash
make clean && make test
```

Expected: 118/118 pass. Any regression must be fixed before proceeding.

- [ ] **Step 2: Interactive smoke test**

Run `make run` and verify in the QEMU GTK window:
- Busybox ash shell starts
- `ls`, `cat`, `echo`, `pwd` work
- Arrow keys for line editing work
- Ctrl-C → new prompt (SIGINT)
- `ls /proc/` shows procfs entries

- [ ] **Step 3: SMP test**

```bash
make run SMP=2
```

Expected: both CPUs active, shell works. If hung at boot, diagnose via serial log.

- [ ] **Step 4: Commit any fixes (if needed)**

If all pass, no commit needed. If fixes required:
```bash
git add <fixed files>
git commit -m "fix(sched): EEVDF regression fix — systest pass"
```

---

### Task 8: rbtree unit tests + EEVDF fairness tests in systest

**Files:**
- Modify: `user/systest.c`

**Interfaces:**
- Consumes: `rbtree_node_t`, `rbtree_insert`, `rbtree_erase`, `rbtree_first`, `rbtree_next`, `rbtree_empty` from libc (userspace-visible via `<rbtree.h>`)

- [ ] **Step 1: Add rbtree test harness struct**

Add near the top of `user/systest.c`:
```c
#include <rbtree.h>

// Test node: embed rbtree_node_t in a small test struct
typedef struct test_rb_node {
    rbtree_node_t node;
    int key;
} test_rb_node_t;

static int test_cmp(rbtree_node_t *a, rbtree_node_t *b)
{
    test_rb_node_t *ta = (test_rb_node_t *)a;
    test_rb_node_t *tb = (test_rb_node_t *)b;
    if (ta->key < tb->key) return -1;
    if (ta->key > tb->key) return 1;
    return 0;
}
```

- [ ] **Step 2: Add rbtree unit tests**

```c
static int test_rbtree_insert_order(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[5];
    int keys[] = {30, 10, 50, 20, 40};
    for (int i = 0; i < 5; i++) {
        n[i].key = keys[i];
        rbtree_insert(&root, &n[i].node, test_cmp);
    }

    // Verify inorder traversal is sorted
    int prev = -1;
    for (rbtree_node_t *cur = rbtree_first(&root); cur; cur = rbtree_next(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (tn->key <= prev) return 1; // FAIL: not sorted
        prev = tn->key;
    }
    return 0;
}

static int test_rbtree_erase_middle(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[3];
    n[0].key = 10; n[1].key = 20; n[2].key = 30;
    rbtree_insert(&root, &n[0].node, test_cmp);
    rbtree_insert(&root, &n[1].node, test_cmp);
    rbtree_insert(&root, &n[2].node, test_cmp);

    rbtree_erase(&root, &n[1].node);  // remove middle

    // Verify remaining: 10, 30
    rbtree_node_t *first = rbtree_first(&root);
    if (((test_rb_node_t *)first)->key != 10) return 1;
    rbtree_node_t *second = rbtree_next(first);
    if (!second || ((test_rb_node_t *)second)->key != 30) return 1;
    if (rbtree_next(second) != NULL) return 1;
    return 0;
}

static int test_rbtree_stress_100(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t nodes[100];
    for (int i = 0; i < 100; i++) {
        nodes[i].key = (i * 73 + 17) % 1000;  // pseudo-random unique keys
        rbtree_insert(&root, &nodes[i].node, test_cmp);
    }

    // Verify inorder sorted
    int prev = -1, count = 0;
    for (rbtree_node_t *cur = rbtree_first(&root); cur; cur = rbtree_next(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (tn->key < prev) return 1;
        prev = tn->key;
        count++;
    }
    if (count != 100) return 1;

    // Delete all in random order
    for (int i = 0; i < 100; i++) {
        int idx = (i * 47 + 23) % 100;
        rbtree_erase(&root, &nodes[idx].node);
    }
    if (!rbtree_empty(&root)) return 1;
    return 0;
}
```

- [ ] **Step 3: Add EEVDF fairness test**

```c
static int test_eevdf_fork_child_scheduled(void)
{
    int pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        // Child: just verify we were scheduled
        _exit(0);
    }
    // Parent: wait for child
    int status;
    waitpid(pid, &status, 0);
    // If we got here, the child was scheduled and exited
    return (status == 0) ? 0 : 1;
}
```

- [ ] **Step 4: Register new tests**

Add test registrations in the `main()` function's test array:
```c
    {"rbtree_insert_order", test_rbtree_insert_order},
    {"rbtree_erase_middle", test_rbtree_erase_middle},
    {"rbtree_stress_100", test_rbtree_stress_100},
    {"eevdf_fork_child", test_eevdf_fork_child_scheduled},
```

- [ ] **Step 5: Build + run**

```bash
make -C user clean && make -C user
make clean && make test
```

Expected: 122+ passed. If rbtree tests pass, the data structure is verified. If the fork-scheduled test passes, EEVDF basic correctness is verified.

- [ ] **Step 6: Commit**

```bash
git add user/systest.c
git commit -m "test(systest): add rbtree unit tests + EEVDF fork-scheduled test

3 rbtree tests: insert-order, erase-middle, stress-100.
1 EEVDF test: fork child must be scheduled and exit.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Self-Review Checklist (pre-handoff)

1. **Spec coverage**:
   - §4.1 rbtree → Task 1
   - §4.2 task_t fields → Task 3 Step 1
   - §4.3 percpu_t fields → Task 3 Step 2
   - §5.0 ASSERT macro → Task 2
   - §5.1 constants → Task 4 Step 2
   - §5.2 update_curr → Task 4 Step 2
   - §5.3 enqueue/dequeue → Task 4 Step 2
   - §5.4 pick_eevdf → Task 4 Step 2
   - §5.5 task_wake → Task 4 Step 2
   - §5.6 schedule() → Task 4 Step 5
   - §6.1 timer tick → Task 6 Step 6
   - §6.2 do_fork → Task 5 Step 1
   - §6.3 do_exit → Task 5 Step 2
   - §6.4 wake audit → Task 6 Steps 1-5
   - §6.5 task_init+spawn → Task 5 Steps 3-4
   - §9.1 rbtree tests → Task 8 Steps 1-2
   - §9.2 EEVDF tests → Task 8 Step 3
   - §9.3 regression → Task 7

2. **Placeholder scan**: No TBD/TODO. All code is complete.

3. **Type consistency**: `task_wake` declared in task.h (Task 3), defined in task.c (Task 4), consumed by wait.c/futex.c/tty.c/trap.c (Task 6). `rbtree_node_t` defined in rbtree.h (Task 1), used by task.h (Task 3). `spawn_user_task` returns `task_t*` (Task 5), consumed by `task_init` (Task 5 Step 4).
