# Deferred-Free Kthread — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a kernel-thread-based deferred-free work queue that lets zombie reaping (and future subsystems) safely free memory outside the task-list traversal, eliminating the 32 KB/process `stack_alloc_base` leak.

**Architecture:** A new `deferred_free.c` file with a spinlock-protected linked-list work queue and a reaper kthread that sleeps via `blocker_wait`, drains the queue in batch, and invokes free callbacks outside the lock. The `schedule()` zombie reaper enqueues `stack_alloc_base` and `files` via `deferred_free()` instead of freeing them inline.

**Tech Stack:** C (kernel code), x86_64; existing slab, blocker framework, `create_kthread`.

## Global Constraints

- New files in `kernel/sched/` (wildcard-discovered — no Makefile change needed)
- New header in `kernel/include/kernel/`
- Add `BLOCKER_DEFERRED_FREE = 2` to `kernel/include/kernel/task.h`
- Modify `schedule()` Pass 3 + `task_init()` in `kernel/sched/task.c`
- Selftest uses existing `#ifdef OS01_SELFTEST` + `selftest_register()` pattern
- Build and test with `make KERNEL_SELFTEST=1`

---

### Task 1: Add `BLOCKER_DEFERRED_FREE` to task.h

**Files:**
- Modify: `kernel/include/kernel/task.h:16`

**Interfaces:**
- Consumes: nothing
- Produces: `#define BLOCKER_DEFERRED_FREE 2` — used by `blocker_wait()` calls in Task 2

- [ ] **Step 1: Add the define**

```c
// Line 16 currently reads:
// #define BLOCKER_WAITPID   1
//
// Add after it:
#define BLOCKER_DEFERRED_FREE 2
```

- [ ] **Step 2: Verify compilation**

Run:
```bash
cd kernel && make KERNEL_SELFTEST=1 -j$(nproc)
```
Expected: Compiles clean (no functional change yet).

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/task.h
git commit -m "feat: add BLOCKER_DEFERRED_FREE blocker type for deferred-free kthread

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Create `deferred_free.h` and `deferred_free.c`

**Files:**
- Create: `kernel/include/kernel/deferred_free.h`
- Create: `kernel/sched/deferred_free.c`

**Interfaces:**
- Consumes:
  - `BLOCKER_DEFERRED_FREE` (Task 1, kernel/include/kernel/task.h)
  - `blocker_wait()`, `blocker_wake()` (existing, kernel/sched/task.c)
  - `kfree()`, `kmalloc()` (existing, kernel/memory/slab.c)
  - `files_free()` (existing, kernel/fs/file.c)
  - `create_kthread()` (existing, kernel/sched/task.c)
  - `spinlock_T`, `spin_lock_irqsave`, `spin_unlock_irqrestore` (existing)
  - `list_t`, `list_init`, `list_add_to_before`, `list_del`, `list_is_empty`, `container_of` (existing)
- Produces:
  - `deferred_work_t` struct — `{ list_t node; deferred_fn_t fn; void *ptr; }`
  - `typedef void (*deferred_fn_t)(void *ptr)`
  - `void deferred_free(deferred_fn_t fn, void *ptr)` — enqueue a free for later execution
  - `void deferred_kfree(void *ptr)` — convenience: `deferred_free(kfree_wrapper, ptr)`, for kernel slab allocations
  - `void deferred_files_free(struct files_struct *fs)` — convenience: `deferred_free(files_free_wrapper, fs)`, for fd tables
  - `task_t *deferred_free_spawn(void)` — spawn the reaper kthread (called once from task_init)
  - Internal: `kfree_wrapper()`, `files_free_wrapper()` — thin shims (static, called via the convenience functions above)

- [ ] **Step 1: Write the header**

Create `kernel/include/kernel/deferred_free.h`:

```c
#ifndef _KERNEL_DEFERRED_FREE_H
#define _KERNEL_DEFERRED_FREE_H

#include <list.h>
#include <stdint.h>
#include <kernel/task.h>

// Forward-declare files_t (full definition in kernel/file.h — not
// included here to keep this header light for consumers).
struct files_struct;

// Callback type for deferred-free work items.
typedef void (*deferred_fn_t)(void *ptr);

typedef struct deferred_work {
    list_t        node;
    deferred_fn_t fn;
    void         *ptr;
} deferred_work_t;

// Enqueue a deferred free. Safe from any context including IRQs-off
// (only takes a spinlock, never sleeps).
void deferred_free(deferred_fn_t fn, void *ptr);

// Convenience wrappers that avoid UB from casting kfree/files_free
// to deferred_fn_t. All three functions enqueue the free onto the
// reaper kthread's work queue.
void deferred_kfree(void *ptr);                           // kfree a slab allocation
void deferred_files_free(struct files_struct *fs);        // free an fd table

// Spawn the reaper kthread. Called once from task_init() before
// activating the scheduler. Returns the kthread's task_t* or NULL.
task_t *deferred_free_spawn(void);

#endif // _KERNEL_DEFERRED_FREE_H
```

- [ ] **Step 2: Write the implementation**

Create `kernel/sched/deferred_free.c`:

```c
#include <kernel/deferred_free.h>
#include <kernel/slab.h>
#include <kernel/task.h>
#include <kernel/file.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <stdbool.h>

// ── Queue state ──────────────────────────────────────────────
static spinlock_T  df_lock = { .lock = 1L };
static list_t      df_queue;
static task_t     *df_kthread;

// ── Thin wrappers ────────────────────────────────────────────
// Avoid UB from casting kfree (returns size_t) to
// deferred_fn_t (void (*)(void*)). Both calling conventions are
// identical on x86_64 SysV, but the C standard forbids the cast.
static void kfree_wrapper(void *p)       { kfree(p); }
static void files_free_wrapper(void *p)  { files_free((files_t *)p); }

// ── Blocker condition —────────────────────────────────────────
// Reads df_queue.next/prev without df_lock — intentional; on x86_64
// aligned pointer reads are atomic, and a stale "empty" read at
// worst causes one extra blocker_wait cycle (benign).
static bool df_queue_has_work(task_t *self)
{
    (void)self;
    return !list_is_empty(&df_queue);
}

// ── Public API ────────────────────────────────────────────────

void deferred_free(deferred_fn_t fn, void *ptr)
{
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

// ── Convenience API —─────────────────────────────────────────
// Thin wrappers that keep kfree_wrapper/files_free_wrapper
// static while providing a clean interface for callers.

void deferred_kfree(void *ptr)
{
    deferred_free(kfree_wrapper, ptr);
}

void deferred_files_free(files_t *fs)
{
    deferred_free(files_free_wrapper, fs);
}

// ── Reaper main loop ──────────────────────────────────────────

static uint64_t df_reaper_main(uint64_t arg)
{
    (void)arg;
    for (;;) {
        blocker_wait(df_queue_has_work, BLOCKER_DEFERRED_FREE, false);

        uint64_t flags = spin_lock_irqsave(&df_lock);
        while (!list_is_empty(&df_queue)) {
            deferred_work_t *w =
                container_of(df_queue.next, deferred_work_t, node);
            list_del(&w->node);
            spin_unlock_irqrestore(&df_lock, flags);

            w->fn(w->ptr);       // e.g. kfree_wrapper(stack_alloc_base)
            kfree(w);            // free the work item itself

            flags = spin_lock_irqsave(&df_lock);
        }
        spin_unlock_irqrestore(&df_lock, flags);
    }
    return 0;
}

// ── Initialization ────────────────────────────────────────────

task_t *deferred_free_spawn(void)
{
    list_init(&df_queue);
    df_kthread = create_kthread(df_reaper_main, 0, "df-reaper");
    return df_kthread;
}
```

- [ ] **Step 3: Verify compilation**

Run:
```bash
cd kernel && make KERNEL_SELFTEST=1 -j$(nproc)
```
Expected: Compiles clean. New symbols in kernel.bin (`df_reaper_main`, `deferred_free`, `deferred_free_spawn`).

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/deferred_free.h kernel/sched/deferred_free.c
git commit -m "feat: add deferred-free work queue with reaper kthread

Introduces deferred_free(fn, ptr) for safely deferring kfree/free
calls to a dedicated kernel thread. The reaper sleeps via blocker_wait
until work arrives, drains the queue in batch, and invokes callbacks
outside the spinlock.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Hook into `schedule()` zombie reaper and `task_init()`

**Files:**
- Modify: `kernel/sched/task.c:305-314` (Pass 3 — add `deferred_free` calls)
- Modify: `kernel/sched/task.c:1405-1411` (after spawn, before scheduler_ok)

**Interfaces:**
- Consumes: `deferred_kfree()`, `deferred_files_free()`, `deferred_free_spawn()` (Task 2)
- Produces: No new APIs — zombie reaping now defers `stack_alloc_base` and `files`; `task_init()` spawns reaper before scheduler activation.

- [ ] **Step 1: Add `#include` at the top of `task.c`**

```c
// kernel/sched/task.c, after the existing includes (around line 22), add:
#include <kernel/deferred_free.h>
```

The exact insertion point is after line 22 (`#include <uapi/time.h>`):

```c
#include <uapi/time.h>
#include <kernel/deferred_free.h>    // deferred_free() for zombie reaping
```

- [ ] **Step 2: Replace Pass 3 with deferred_free calls**

Current code at lines 305-314:
```c
    for (int i = 0; i < reap_count; i++) {
        task_t *t = reap_list[i];
        list_del(&t->list);
        if (t->thread)
            kfree(t->thread);
        if (t->files)
            files_free(t->files);
        if (t->fpu_save)
            kfree(t->fpu_save);
    }
```

Replace with:
```c
    for (int i = 0; i < reap_count; i++) {
        task_t *t = reap_list[i];
        list_del(&t->list);
        if (t->thread)
            kfree(t->thread);
        if (t->files)
            deferred_files_free(t->files);
        if (t->fpu_save)
            kfree(t->fpu_save);
        if (t->stack_alloc_base)
            deferred_kfree(t->stack_alloc_base);
    }
```

Also remove the TODO comment block at lines 298-304:
```c
    // Pass 3: unlink and free zombie resources.
    // NOTE: kfree(t->stack_alloc_base) is DEFERRED — it frees
    // the task_t itself (embedded in the stack allocation).
    // Doing so corrupts subsequent allocations from the same
    // slab cache (the freed memory is re-used before list
    // consumers have finished with the stale node pointers).
    // TODO: add a deferred-free work queue for stack_alloc_base.
```

Replace with:
```c
    // Pass 3: unlink and free zombie resources.
    // thread and fpu_save are separate kmalloc allocations — freed inline.
    // stack_alloc_base and files are deferred via deferred_free() to avoid
    // use-after-free (stack embeds task_t+list node; files_free is slow).
```

- [ ] **Step 3: Add deferred_free_spawn() call in task_init()**

Current code at lines 1405-1411:
```c
    int64_t init_pid = spawn_user_task("/bin/init", NULL);
    debug_task("init: spawned user-space init, pid=%d\n", (int)init_pid);

    // Activate the scheduler and enter the idle loop.
    // schedule() picks up the user init (PID 1) naturally.
    current->state = TASK_RUNNING;
    this_cpu()->scheduler_ok = 1;
```

Replace with:
```c
    int64_t init_pid = spawn_user_task("/bin/init", NULL);
    debug_task("init: spawned user-space init, pid=%d\n", (int)init_pid);

    // Spawn the deferred-free reaper kthread BEFORE activating the
    // scheduler.  This guarantees the reaper exists before any zombie
    // can be produced (schedule() returns early while scheduler_ok==0).
    {
        task_t *df = deferred_free_spawn();
        if (df) df->cpu = 0;
    }

    // Activate the scheduler and enter the idle loop.
    // schedule() picks up the user init (PID 1) naturally.
    current->state = TASK_RUNNING;
    this_cpu()->scheduler_ok = 1;
```

- [ ] **Step 4: Verify compilation**

Run:
```bash
cd kernel && make KERNEL_SELFTEST=1 -j$(nproc)
```
Expected: Compiles clean.

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat: hook deferred-free kthread into zombie reaper and task_init

schedule() Pass 3 now defers stack_alloc_base and files via
deferred_free() instead of leaking stack_alloc_base and freeing
files inline under the reap_lock spinlock. task_init() spawns
the reaper kthread before scheduler_ok=1 to guarantee it exists
before any zombie is produced.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Add kernel selftest

**Files:**
- Modify: `kernel/test/selftest.c` (register new test)
- Create: `kernel/test/test_deferred_free.c`

**Interfaces:**
- Consumes: `deferred_free()`, `kmalloc()`, `kfree()`, `create_kthread()`, `do_exit()` (declared in task.h), `kmalloc_cache_size[]`, `selftest_register()`
- Produces: `test_deferred_free` — selftest entry point

- [ ] **Step 1: Create the selftest file**

Create `kernel/test/test_deferred_free.c`:

```c
#if defined(OS01_SELFTEST)

#include <kernel/selftest.h>
#include <kernel/deferred_free.h>
#include <kernel/slab.h>
#include <kernel/printk.h>
#include <kernel/task.h>

// Forward-declare the wrappers (they're static in deferred_free.c,
// but we reach them through the public deferred_free() API).
// We test through the public API only, using kernel threads.

// ── Test 1: Basic deferred kfree ──────────────────────────────
// Allocate N items, defer-free them, verify the slab total_free
// returns to baseline after the reaper drains the queue.

SELFTEST(deferred_free_basic)
{
    // baseline: free count in the 64-byte slab cache
    struct Slab_Cache *sc = &kmalloc_cache_size[1]; // size=64
    uint64_t baseline_free = sc->total_free;

    // Allocate 4 items
    void *ptrs[4];
    for (int i = 0; i < 4; i++) {
        ptrs[i] = kmalloc(64);
        if (!ptrs[i]) {
            serial_printk("[selftest] deferred_free_basic: kmalloc(64) #%d failed\n", i);
            return -1;
        }
    }

    // Defer-free them
    for (int i = 0; i < 4; i++)
        deferred_kfree(ptrs[i]);

    // The reaper should drain them within a few schedule() ticks.
    // Spin for up to 100 schedule() calls, checking total_free.
    int spins = 0;
    while (sc->total_free < baseline_free && spins < 100) {
        schedule();
        spins++;
    }

    if (sc->total_free < baseline_free) {
        serial_printk("[selftest] deferred_free_basic: total_free=%lu < baseline=%lu after %d spins\n",
                      (unsigned long)sc->total_free, (unsigned long)baseline_free, spins);
        return -1;
    }

    return 0;
}

// ── Test 2: Kernel thread exit (stack reclamation) ──────────
// A kthread allocates stack via create_kthread → do_exit, then
// the reaper frees stack_alloc_base.  We verify by checking PMM
// free page count before and after.

static uint64_t test_kthread_exiter(uint64_t arg)
{
    (void)arg;
    // do_exit triggers do_exit → ZOMBIE → schedule zombie reaper
    // → deferred_free(stack_alloc_base) → reaper drains
    do_exit(0);
    return 0; // unreachable
}

SELFTEST(deferred_free_kthread)
{
    uint64_t pages_before = 0;
    extern struct PageManager PMMngr;
    pages_before = PMMngr.zones_struct->page_free_count;

    for (int i = 0; i < 3; i++) {
        task_t *kt = create_kthread(test_kthread_exiter, 0, "selftest-exiter");
        if (!kt) {
            serial_printk("[selftest] deferred_free_kthread: create_kthread failed\n");
            return -1;
        }
        // Let the kthread run to completion (it calls do_exit → ZOMBIE)
        int spins = 0;
        while (kt->state == TASK_RUNNING && spins < 1000) {
            schedule();
            spins++;
        }
        // Let the zombie reaper process it, then the reaper drain
        for (int j = 0; j < 20; j++)
            schedule();
    }

    // After all 3 kthreads are reaped, pages should be back at baseline
    // (the stack_alloc_base allocations are returned to slab/pmm).
    uint64_t pages_after = PMMngr.zones_struct->page_free_count;
    (void)pages_before;
    (void)pages_after;
    // We can't assert strict equality (slab may hold onto pages),
    // but we can at least verify no catastrophic leak (>4 pages)
    if (pages_after + 4 < pages_before) {
        serial_printk("[selftest] deferred_free_kthread: pages_before=%lu pages_after=%lu\n",
                      (unsigned long)pages_before, (unsigned long)pages_after);
        return -1;
    }

    return 0;
}

#endif // OS01_SELFTEST
```

- [ ] **Step 2: Register the test in selftest.c**

In `kernel/test/selftest.c`, after the existing extern declarations (around line 119):

```c
#ifdef OS01_SELFTEST
int ext2_selftest_magic(void);
int ext2_selftest_struct_sizes(void);
int gpt_selftest_crc32(void);
int tmpfs_selftest_mounted(void);
#endif
```

Add after the `#endif`:
```c

// Forward-declared from test_deferred_free.c (OS01_SELFTEST-guarded)
int deferred_free_basic(void);
int deferred_free_kthread(void);
```

Then in the `selftest_run_all()` function body, after the existing registrations (around line 131):

```c
    selftest_register("pipe_basic",        test_pipe_basic);
```

Add after:
```c
    selftest_register("deferred_free_basic",   deferred_free_basic);
    selftest_register("deferred_free_kthread", deferred_free_kthread);
```

- [ ] **Step 3: Verify compilation with selftest enabled**

Run:
```bash
cd kernel && make KERNEL_SELFTEST=1 -j$(nproc)
```
Expected: Compiles clean.

- [ ] **Step 4: Run in QEMU to verify tests pass**

Run:
```bash
make run
```
Expected: In serial output, see:
```
[selftest] deferred_free_basic... PASS
[selftest] deferred_free_kthread... PASS
```

- [ ] **Step 5: Commit**

```bash
git add kernel/test/test_deferred_free.c kernel/test/selftest.c
git commit -m "test: add deferred-free selftest (basic defer + kthread exit)

Two tests:
- deferred_free_basic: alloc N items, defer-free them, verify
  slab total_free returns to baseline after reaper drains.
- deferred_free_kthread: spawn 3 kthreads that do_exit(), verify
  PMM free pages don't leak significantly.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Integration test — run full systest

**Files:**
- No code changes — verification only

**Interfaces:**
- Consumes: Everything from Tasks 1-4

- [ ] **Step 1: Build with selftest**

Run:
```bash
make KERNEL_SELFTEST=1 -j$(nproc)
```
Expected: Clean build.

- [ ] **Step 2: Run full systest**

Run:
```bash
make systest
```
Expected: All systests pass (70/70 currently). Verify serial output shows `deferred_free_basic PASS` and `deferred_free_kthread PASS` among selftests.

- [ ] **Step 3: Check PMM free pages at end of systest**

Look for PMM free page count in the systest output. Compare against a known baseline from a run before this change — the free page count should be **higher** (no more 32 KB leak per process).

- [ ] **Step 4: Commit (if needed)**

```bash
git add -A  # only if any last-minute tweaks were made
git commit -m "chore: final verification — all tests pass"
```

---

### Task 6: Build without selftest (production mode)

**Files:**
- No code changes — verification only

- [ ] **Step 1: Make sure the code compiles clean in release mode**

Run:
```bash
make clean && make -j$(nproc)
```
Expected: Clean build. All `#ifdef OS01_SELFTEST` blocks are correctly excluded.

- [ ] **Step 2: Quick boot test**

Run:
```bash
make run
```
Expected: Kernel boots normally, init.elf runs, shell prompt appears. No regressions.

- [ ] **Step 3: Run systest again in release mode**

```bash
make systest
```
All tests pass.

- [ ] **Step 4: Commit (if needed)**

No commit expected — all changes already committed.
