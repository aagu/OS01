# Deferred-Free Kthread — Design Spec

**Date:** 2026-07-13  
**Status:** approved  
**Scope:** kernel/sched/deferred_free.c + changes to kernel/sched/task.c

## Motivation

When a process exits, `do_exit()` frees user pages (VMA, mm), file descriptors, and marks the task ZOMBIE. The scheduler's zombie reaper in `schedule()` then unlinks the task from the global task list and frees `thread_t`, `files`, and `fpu_save`. However, it **cannot** free `stack_alloc_base` — the kernel stack allocation that embeds `task_t` — because the reaper is currently traversing the global task list, and `kfree`-ing that memory would return it to the slab cache where it could be reallocated immediately, corrupting the list traversal's `pos → pos->next` pointer chase.

This results in a permanent leak of ~32 KB (STACK_SIZE) per exited process. A shell script that forks 100 processes leaks 3.2 MB.

### Why a kthread?

The root cause is that the freeing context (inside `schedule()`'s task-list walk) is the **same** context that must not touch freed memory. A dedicated kernel thread runs on its own stack, outside the task-list traversal, and can safely `kfree` the zombie's stack.

## Design

### 1. Deferred-work queue (general-purpose)

A work-queue abstraction that any kernel subsystem can use to defer a free to a safe context.

**File:** `kernel/include/kernel/deferred_free.h`

```c
typedef void (*deferred_fn_t)(void *ptr);

typedef struct deferred_work {
    list_t        node;
    deferred_fn_t fn;
    void         *ptr;
} deferred_work_t;

// Enqueue a deferred free. Safe to call from any context (IRQs off is OK —
// this only takes a spinlock and never sleeps).
void deferred_free(deferred_fn_t fn, void *ptr);

// Convenience wrappers — avoid UB from casting kfree/files_free.
// The thin shims (kfree_wrapper, files_free_wrapper) are static
// inside deferred_free.c; these are the public entry points.
void deferred_kfree(void *ptr);                       // kfree a slab allocation
void deferred_files_free(struct files_struct *fs);    // free an fd table

// Internal: spawn the reaper kthread (called once from task_init).
task_t *deferred_free_spawn(void);
```

**File:** `kernel/sched/deferred_free.c`

```c
static spinlock_T  df_lock = { .lock = 1L };
static list_t      df_queue;
static task_t     *df_kthread;

// blocker_wait condition: queue is non-empty.
// Reads df_queue.next/prev without df_lock — intentional; on x86_64 aligned
// pointer reads are atomic, and a stale "empty" read at worst causes one
// extra blocker_wait cycle (benign).
static bool df_queue_has_work(task_t *self) {
    (void)self;
    return !list_is_empty(&df_queue);
}

// Thin wrappers — avoid UB from casting kfree (returns size_t) to
// deferred_fn_t (void (*)(void*)).  Both calling conventions are identical
// on x86_64 SysV, but the C standard forbids the direct cast.
static void kfree_wrapper(void *p)       { kfree(p); }
static void files_free_wrapper(void *p)  { files_free((files_t *)p); }

void deferred_free(deferred_fn_t fn, void *ptr) {
    deferred_work_t *w = kmalloc(sizeof(*w));
    if (!w) return;  // OOM: leak the ptr rather than panic
    list_init(&w->node);
    w->fn  = fn;
    w->ptr = ptr;

    uint64_t flags = spin_lock_irqsave(&df_lock);
    list_add_to_before(&df_queue, &w->node);
    spin_unlock_irqrestore(&df_lock, flags);

    if (df_kthread)
        blocker_wake(df_kthread);
}
```

A new blocker type `BLOCKER_DEFERRED_FREE = 2` is added to `kernel/include/kernel/task.h` (alongside `BLOCKER_WAITPID = 1`).

### 2. Reaper kthread main loop

```c
static uint64_t df_reaper_main(uint64_t arg) {
    (void)arg;
    for (;;) {
        blocker_wait(df_queue_has_work, BLOCKER_DEFERRED_FREE, false);

        uint64_t flags = spin_lock_irqsave(&df_lock);
        while (!list_is_empty(&df_queue)) {
            deferred_work_t *w =
                container_of(df_queue.next, deferred_work_t, node);
            list_del(&w->node);
            spin_unlock_irqrestore(&df_lock, flags);

            w->fn(w->ptr);      // e.g. kfree_wrapper(stack_alloc_base)
            kfree(w);           // free the work item itself

            flags = spin_lock_irqsave(&df_lock);
        }
        spin_unlock_irqrestore(&df_lock, flags);
    }
    return 0;
}
```

Key properties:
- **Zero idle overhead** — `blocker_wait` sleeps until work arrives.
- **Lock held only for list_del** — `kfree` runs outside the lock, so slab-internal coalescing never stalls producers.
- **Batch drain** — each wake-up drains the entire queue (in case multiple zombies arrived between wake-ups).

### 3. Changes to `schedule()` zombie reaper

**Before** (kernel/sched/task.c:298–314):

```c
for (int i = 0; i < reap_count; i++) {
    task_t *t = reap_list[i];
    list_del(&t->list);
    if (t->thread)       kfree(t->thread);
    if (t->files)        files_free(t->files);
    if (t->fpu_save)     kfree(t->fpu_save);
    // stack_alloc_base: NOT freed (TODO)
}
```

**After**:

```c
for (int i = 0; i < reap_count; i++) {
    task_t *t = reap_list[i];
    list_del(&t->list);
    if (t->thread)           kfree(t->thread);
    if (t->files)            deferred_files_free(t->files);
    if (t->fpu_save)         kfree(t->fpu_save);
    if (t->stack_alloc_base) deferred_kfree(t->stack_alloc_base);
}
```

Only `stack_alloc_base` and `files` are deferred — `stack_alloc_base` because the `task_t` list node is embedded in it (use-after-free if freed under the list walk), and `files` because `files_free` iterates up to 32 fds and can be slow. `thread` and `fpu_save` are plain `kmalloc`'d slabs — freeing them inline is fast and safe.

`reap_lock` critical section still benefits: the slow `files_free` and `kfree(stack_alloc_base)` move out-of-lock. `thread`/`fpu_save` kfrees stay inline but add trivial overhead.

### 4. Initialization

In `task_init()` (kernel/sched/task.c), after `spawn_user_task` and before `scheduler_ok = 1`:

```c
// Spawn the deferred-free reaper kthread before activating the scheduler
{
    task_t *df = deferred_free_spawn();
    if (df) df->cpu = 0;
}
```

`deferred_free_spawn()` does:

```c
task_t *deferred_free_spawn(void) {
    list_init(&df_queue);
    df_kthread = create_kthread(df_reaper_main, 0, "df-reaper");
    return df_kthread;
}
```

The kthread is created before `scheduler_ok = 1`, guaranteeing it is alive before any zombie can be produced.

Lifecycle: the reaper **never exits** — `signal_can_wake = false` in `blocker_wait`. It is kernel infrastructure, like the idle task.

### 5. Race-condition analysis

| Scenario | Result |
|----------|--------|
| `deferred_free()` called before reaper spawned | Cannot happen — kthread created before `scheduler_ok=1`, and `schedule()` returns early before that |
| `blocker_wake` while reaper is mid-drain | `blocker_wake` sees `TASK_RUNNING`, returns immediately. Reaper naturally drains new items in same loop (condition is `!list_is_empty`) |
| `list_del` then `deferred_free(stack)` — does anyone traverse the zombie? | No. `list_del` removes it from the global task list *before* `deferred_free`. The freed `task_t` is no longer reachable. |
| Slab reuse before drain completes? | Safe. The work-item struct and the `ptr` inside it are separate allocations. `kfree(w)` at the end of each iteration frees the work item; the original `stack_alloc_base` was already freed by `w->fn(w->ptr)` in the same iteration. |
| CPU adds work after reaper drains queue but before `blocker_wait` re-checks | `blocker_wait` step 1 calls `df_queue_has_work` → finds new work → returns 0 immediately without sleeping. No lost wakeup. |

### 6. SMP considerations

The reaper kthread is bound to CPU 0 (`df->cpu = 0`). Work produced on AP CPUs queues into the global `df_queue` and sits there until CPU 0's next `schedule()` wakes the reaper.

- **Latency bound:** Worst case = the reaper's scheduling quantum on CPU 0 (10 ms at priority 1). For typical zombie workloads (hundreds/hour) this is negligible.
- **Producer concurrency:** Multiple CPUs calling `deferred_free()` contend only on `df_lock`. The lock hold time is a single `list_add_to_before` (~20 cycles on locked `xchg`), so contention is minimal even under heavy fork/exit storms.
- **Single reaper simplicity:** One reaper avoids the complexity of per-CPU work stealing, NUMA-aware placement, or priority-inversion bugs. If profiling later shows a bottleneck (unlikely for this workload), a per-CPU queue + IPI wakeup can be added without changing the API.

### 7. Files changed

| File | Change |
|------|--------|
| `kernel/sched/deferred_free.c` | New file (~100 lines) — queue, wrappers, reaper loop |
| `kernel/include/kernel/deferred_free.h` | New file (~15 lines) — `deferred_work_t`, public API |
| `kernel/include/kernel/task.h` | Add `BLOCKER_DEFERRED_FREE = 2` |
| `kernel/sched/task.c` | Add `#include <kernel/deferred_free.h>`; Pass 3: defer `stack_alloc_base` and `files`; `task_init()`: call `deferred_free_spawn()` before `scheduler_ok = 1` |

**Cosmetic note:** `create_kthread` at `task.c:1334` does `(void)name` — the `"df-reaper"` string is discarded. Not a functional issue; if thread naming is added later, this already passes the right value.

### 8. Non-goals (for this change)

- **Per-CPU work queues** — not needed; the single global queue with fine-grained locking is sufficient for the expected throughput (hundreds of exits/second max).
- **Prioritised / ordered free** — all deferred frees are independent; FIFO is correct.
- **Replacing `files_free` semantics** — `deferred_files_free` delegates to `files_free` via a static wrapper; no behavioral change.
- **Freeing 2MB ELF pages** — these are tracked outside VMA and are a separate concern.

## Testing

### Unit / selftest

Add a selftest in `kernel/test/`:

1. Create a kernel thread that allocates and exits, verifying its stack is eventually freed.
2. Allocate K items via `kmalloc`, defer-free them, verify the slab `total_free` count returns to baseline after the reaper runs.
3. Stress: spawn N kthreads that all `do_exit()` in quick succession, then verify no leaked pages (`pmm_free_page_count` stable after all reaped).

### Integration

Existing `systest` suite already exercises `fork` + `exit` + `waitpid` heavily. After this change, verify that the `pmm` free-page count after the full systest run is **higher** (no **32 KB** leak per process). The `test_kernel_mutex` selftest also exercises `create_kthread` → `do_exit` for kernel threads, so it serves as a basic smoke test.
