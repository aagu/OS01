# SMP Load Balancing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable AP cores to pick up and run tasks, with automatic load balancing using EEVDF's `nr_running` metric and per-schedule pull/steal.

**Architecture:** Four new static functions in `kernel/sched/task.c` — `sched_pick_cpu()` (least-loaded CPU selection at task creation), `sched_notify_remote()` (IPI + need_resched for remote wakeup), `sched_balance()` (tail-steal from busiest CPU with oscillation guard), and a retry-enabled `task_wake()` (lock-hold re-check of `t->cpu`/`t->on_rq` against concurrent migration). Two new rbtree functions (`rbtree_last`, `rbtree_prev`) in `libc/rbtree/`. One new field `nr_running` in `percpu_t`. Five prerequisites address existing SMP-unsafe code paths plus the `task_wake` race.

**Tech Stack:** C (kernel), x86_64 asm (no changes needed), QEMU with `-smp 2+` for testing.

## Global Constraints

- Target: `NR_CPUS ≤ 8` — O(num_cpus) scans acceptable.
- `nr_running` as primary load metric, `min_vruntime` as tiebreaker.
- Steal from rbtree tail (largest deadline), gate with `src.nr_running > local.nr_running + 1`.
- `arch_local_irq_save/restore` around double-lock; no IRQ nesting inside `schedule()`.
- All lockless `nr_running` reads use `*(volatile uint32_t *)` pattern.
- Must pass `systest` (70 tests) on `-smp 2` QEMU.
- Spec reference: `docs/superpowers/specs/2026-07-29-smp-load-balance-design.md`

---

## Prerequisites (Phase 1)

### Task 0: Fix task_wake retry-on-migrate race

**Files:**
- Modify: `kernel/sched/task.c:114-145`

**Interfaces:**
- Produces: `task_wake()` with lock-hold retry for `t->cpu`/`t->on_rq` migration

**Why now:** `sched_balance` modifies `t->cpu` while holding two `rq_lock`s.
`task_wake` reads `t->cpu` *before* acquiring `rq_lock`, creating a window
where the task is enqueued on the wrong CPU's rbtree.  This is a
prerequisite for T8 (`sched_balance`).

- [ ] **Step 1: Rewrite task_wake with retry pattern**

Replace the existing `task_wake` (line 114-145) with:

```c
/* ── task_wake: mark RUNNING + enqueue (exported) ─── */
void task_wake(task_t *t)
{
    /*
     * Never enqueue the idle task — it is always RUNNING on its
     * CPU and must never appear on a runqueue.
     */
    if (t == percpu_data[t->cpu].idle)
        return;

    t->state = TASK_RUNNING;

retry:
    /*
     * Read t->cpu locklessly — sched_balance may change it concurrently.
     * We re-check both t->cpu and t->on_rq under the acquired rq_lock
     * to close the race window.
     */
    percpu_t *rq = &percpu_data[*(volatile uint32_t *)&t->cpu];
    uint64_t flags = spin_lock_irqsave(&rq->rq_lock);

    /* Re-check on_rq under lock — sched_balance may have enqueued it */
    if (t->on_rq) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        return;
    }

    /* Re-check t->cpu under lock — sched_balance may have migrated it */
    if ((uintptr_t)rq != (uintptr_t)&percpu_data[*(volatile uint32_t *)&t->cpu]) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        goto retry;
    }

    /* Wakeup boost: prevent starvation by raising vruntime floor */
    uint64_t wake_vruntime = rq->min_vruntime > EEVDF_LATENCY
        ? rq->min_vruntime - EEVDF_LATENCY : 0;
    if (t->vruntime < wake_vruntime)
        t->vruntime = wake_vruntime;

    enqueue_task(t, rq);
    t->cpu = rq->cpu_id;  // keep t->cpu in sync with actual rq

    spin_unlock_irqrestore(&rq->rq_lock, flags);

    if ((int)t->cpu != (int)cpu_id())
        rq->need_resched = 1;
}
```

**Key changes:**
1. `retry` loop: if `t->cpu` changed between lockless read and lock
   acquire, release lock and retry with the new CPU.
2. `on_rq` check under lock: if `sched_balance` enqueued the task
   between our lockless read and lock acquire, we return immediately —
   the task is already scheduled.
3. `t->cpu = rq->cpu_id` after enqueue: keeps `t->cpu` consistent with
   the actual runqueue.  Without this, a subsequent `task_wake` call
   could enqueue to the wrong CPU.
4. Volatile reads of `t->cpu` via `*(volatile uint32_t *)&t->cpu`.

- [ ] **Step 2: Build**

```bash
cd /home/aagu/OS01 && make clean && make -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add kernel/sched/task.c
git commit -m "fix(sched): close task_wake migration race with retry loop

sched_balance modifies t->cpu while holding two rq_locks.
task_wake read t->cpu before acquiring rq_lock, creating
a window where the task could be enqueued to the wrong CPU.

Fix: retry pattern — re-read t->cpu and on_rq under lock,
retry if cpu changed between read and lock acquire.
Also set t->cpu = rq->cpu_id after enqueue to maintain
the cpu↔runqueue invariant.

Prerequisite for sched_balance (T8).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 1a: PMM alloc_pages/free_pages SMP protection

**Files:**
- Modify: `kernel/memory/pmm.c`

**Interfaces:**
- Produces: thread-safe `alloc_pages()` / `free_pages()` via global `spinlock_T pmm_lock`

**Why:** PMM bitmaps (`PMMngr.bits_map`) and `page_using_count`/`page_free_count`
counters use non-atomic RMW operations.  `spawn_user_task`, `fork_mm_copy`,
`do_exit`, and `sys_exec` all call `alloc_pages`/`free_pages` without any
synchronisation.  With SMP, two CPUs simultaneously modifying the same
bitmap word will corrupt the free-page tracking.

- [ ] **Step 1: Add pmm_lock and include**

```c
// kernel/memory/pmm.c — add after #include lines (near file top)
#include <kernel/arch/spinlock.h>

static spinlock_T pmm_lock = { .lock = 1L };
```

- [ ] **Step 2: Wrap alloc_pages**

In `alloc_pages()` (around line 262), add lock/unlock.  Find the function
signature and add the lock immediately after variable declarations:

```c
struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    // ... existing function body (all code paths) ...
    // Before EVERY return statement, add:
    //     spin_unlock_irqrestore(&pmm_lock, flags);
}
```

The function has multiple return points — wrap each with unlock before return:
- `return NULL` paths → `spin_unlock_irqrestore(&pmm_lock, flags); return NULL;`
- Success path → `spin_unlock_irqrestore(&pmm_lock, flags); return page;`

- [ ] **Step 3: Wrap free_pages**

```c
void free_pages(struct Page * page, int32_t number)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    // ... existing function body ...
    spin_unlock_irqrestore(&pmm_lock, flags);
}
```

- [ ] **Step 4: Lock ordering verification**

The new lock hierarchy is:
```
slab_lock → pmm_lock   (kmalloc_create → alloc_pages, kfree → free_pages)
subpage_lock → pmm_lock (alloc_4k_page → alloc_pages)
```

No code acquires `pmm_lock` then `subpage_lock` — no AB-BA deadlock.
`vmm_free_user_map` calls `page_cow_put` (subpage_lock) then `free_pages`
(pmm_lock) sequentially, not nested. ✓

- [ ] **Step 5: Build**

```bash
cd /home/aagu/OS01 && make clean && make -j$(nproc)
```

- [ ] **Step 6: Commit**

```bash
git add kernel/memory/pmm.c
git commit -m "fix(pmm): add SMP spinlock for alloc_pages/free_pages

Global pmm_lock serialises bitmap operations and page counters.
Lock hierarchy: slab_lock → pmm_lock, subpage_lock → pmm_lock.
Protects spawn_user_task, fork_mm_copy, do_exit, sys_exec
from concurrent PMM corruption.

Prerequisite for SMP load balancing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 1: Slab allocator SMP safety

**Files:**
- Modify: `kernel/memory/slab.c:1-273`

**Interfaces:**
- Produces: thread-safe `kmalloc()` / `kfree()` via global `spinlock_T slab_lock`

- [ ] **Step 1: Add global spinlock**

```c
// kernel/memory/slab.c — add after #include lines (around line 26)
#include <kernel/arch/spinlock.h>

static spinlock_T slab_lock = { .lock = 1L };
```

- [ ] **Step 2: Wrap kmalloc critical section**

In `kmalloc()` (line 127), add lock/unlock around the entire function body after the size check:

```c
void * kmalloc(size_t size)
{
    uint32_t i, j;
    struct Slab * slab = NULL;
    if (size > 1048576)
    {
        color_printk(RED,BLACK,"kmalloc() ERROR: kmalloc size too long:%08d\n",size);
        return NULL;
    }

    uint64_t flags = spin_lock_irqsave(&slab_lock);  // ← NEW

    for (i = 0; i < 16; i++)
    {
        if (kmalloc_cache_size[i].size >= size)
            break;
    }
    slab = kmalloc_cache_size[i].cache_pool;

    if (kmalloc_cache_size[i].total_free != 0)
    {
        do
        {
            if (slab->free_count == 0)
                slab = container_of(list_next(&slab->list), struct Slab, list);
            else
                break;
        } while (slab != kmalloc_cache_size[i].cache_pool);
    }
    else
    {
        slab = kmalloc_create(kmalloc_cache_size[i].size);
        if (slab == NULL)
        {
            color_printk(RED, BLACK, "kmalloc()->kmalloc_create()=>slab == NULL\n");
            spin_unlock_irqrestore(&slab_lock, flags);  // ← NEW
            return NULL;
        }
        kmalloc_cache_size[i].total_free += slab->color_count;
        list_add_to_before(&kmalloc_cache_size[i].cache_pool->list,&slab->list);
    }
    
    for (j = 0; j < slab->color_count; j++)
    {
        if (*(slab->color_map + (j >> 6)) == 0xffffffffffffffffUL)
        {
            j += 63;
            continue;
        }
        if ((*(slab->color_map + (j >> 6)) & (1UL << (j % 64))) == 0)
        {
            *(slab->color_map + (j >> 6)) |= 1UL << (j % 64);
            slab->using_count++;
            slab->free_count--;
            kmalloc_cache_size[i].total_free--;
            kmalloc_cache_size[i].total_using++;

            spin_unlock_irqrestore(&slab_lock, flags);  // ← NEW
            return (void *)((char *)slab->address + kmalloc_cache_size[i].size * j);
        }
    }

    spin_unlock_irqrestore(&slab_lock, flags);  // ← NEW
    color_printk(BLUE,BLACK,"kmalloc() ERROR: no memory can alloc\n");
    return NULL;
}
```

**Note:** `kmalloc_create` is called *inside* the lock.  The recursive-call guard
(`kmalloc_creating` flag) prevents re-entry — but since we now hold a spinlock,
the recursive case would deadlock anyway.  Verify that recursive `kmalloc`
never happens under the lock (the `kmalloc_creating` flag should prevent it).

- [ ] **Step 3: Wrap kfree critical section**

In `kfree()` (line 193), add lock/unlock around the body after the NULL check:

```c
size_t kfree(void * address)
{
    if (!address) return 0;

    uint64_t flags = spin_lock_irqsave(&slab_lock);  // ← NEW

    uint32_t i, index;
    struct Slab * slab = NULL;
    void * page_base_address = (void *)((uint64_t)address & PAGE_2M_MASK);

    for (i = 0;i < 16; i++)
    {
        slab = kmalloc_cache_size[i].cache_pool;
        if (!slab) continue;
        do
        {
            if (slab->address == page_base_address)
            {
                index = (address - slab->address) / kmalloc_cache_size[i].size;
                uint64_t *word = slab->color_map + (index >> 6);
                uint64_t bit = 1UL << (index % 64);
                if (!(*word & bit)) {
                    color_printk(RED, BLACK, "kfree: double free %p\n", address);
                    spin_unlock_irqrestore(&slab_lock, flags);  // ← NEW
                    return 1;
                }
                *word &= ~bit;
                slab->free_count++;
                slab->using_count--;
                kmalloc_cache_size[i].total_free++;
                kmalloc_cache_size[i].total_using--;
                if ((slab->using_count==0) && (kmalloc_cache_size[i].total_free >= slab->color_count * 3 / 2) && kmalloc_cache_size[i].cache_pool != slab)
                {
                    switch (kmalloc_cache_size[i].size)
                    {
                    case 32: case 64: case 128: case 256: case 512:
                        list_del(&slab->list);
                        kmalloc_cache_size[i].total_free -= slab->color_count;
                        page_clean(slab->page);
                        free_pages(slab->page, 1);
                        break;
                    default:
                        list_del(&slab->list);
                        kmalloc_cache_size[i].total_free -= slab->color_count;
                        kfree(slab->color_map);       // NOTE: recursive kfree!
                        page_clean(slab->page);
                        free_pages(slab->page, 1);
                        kfree(slab);                  // NOTE: recursive kfree!
                        break;
                    }
                }
                spin_unlock_irqrestore(&slab_lock, flags);  // ← NEW
                return 1;
            }
            else
                slab = container_of(list_next(&slab->list), struct Slab, list);
        } while (slab != kmalloc_cache_size[i].cache_pool);
    }

    spin_unlock_irqrestore(&slab_lock, flags);  // ← NEW
    color_printk(RED, BLACK, "kfree() ERROR: can not free memory %p\n", address);
    return 0;
}
```

**Critical:** `kfree()` has recursive calls at lines 246 and 249 (frees slab metadata
via `kfree(slab->color_map)` and `kfree(slab)`).  With a non-recursive spinlock,
these will deadlock.  **Fix:** use a per-CPU recursion counter:

```c
// kernel/memory/slab.c — at file scope (replaces the global slab_lock_depth)
// Per-CPU recursion depth: each CPU tracks its own nesting level.
// IRQs are disabled during kmalloc/kfree, so the current CPU can't
// be preempted — per-CPU indexing via cpu_id() is safe without atomics.
static uint32_t slab_lock_depth[NR_CPUS];

// Replace spin_lock_irqsave/spin_unlock_irqrestore with these helpers:
static inline uint64_t slab_lock_acquire(void) {
    uint64_t flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    uint32_t cpu = cpu_id();
    if (slab_lock_depth[cpu]++ == 0)
        spin_lock(&slab_lock);
    return flags;
}
static inline void slab_lock_release(uint64_t flags) {
    uint32_t cpu = cpu_id();
    if (--slab_lock_depth[cpu] == 0)
        spin_unlock(&slab_lock);
    if (flags & (1UL << 9))  // RFLAGS_IF
        __asm__ __volatile__("sti" ::: "memory");
}
```

Use `slab_lock_acquire()` / `slab_lock_release(flags)` in place of
`spin_lock_irqsave` / `spin_unlock_irqrestore` for both `kmalloc` and `kfree`.
The per-CPU depth counter is safe because IRQs are disabled during the
locked section — the current CPU won't be preempted, so no other code on
this CPU can touch `slab_lock_depth[me]`.

- [ ] **Step 4: Build and boot test**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

Run: `qemu-system-x86_64 -cdrom os01.iso -smp 2 -m 2G -serial stdio -no-reboot`
Expected: boots to shell, no slab corruption.

- [ ] **Step 5: Commit**

```bash
git add kernel/memory/slab.c
git commit -m "fix(slab): add recursive SMP spinlock to kmalloc/kfree

Global slab_lock protects cache_pool lists, bitmap operations,
and total_free/total_using counters from concurrent CPU access.
Recursive acquisition allowed because kfree() frees slab metadata
via nested kmalloc/kfree calls.

Prerequisite for SMP load balancing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Softirq status atomic + timer list locking

**Files:**
- Modify: `kernel/intr/softirq.c:1-47`
- Modify: `kernel/include/kernel/softirq.h:1-26`
- Modify: `kernel/time/timer.c:1-73`
- Modify: `kernel/driver/pit.c:1-68`

**Interfaces:**
- Produces: atomic `softirq_status` operations, spinlock-protected `timer_list_head`, AP timer IRQ triggers `TIMER_SIRQ`

- [ ] **Step 0: Make LAPIC timer trigger TIMER_SIRQ on APs**

In `kernel/intr/apic/lapic_timer.c`, first add the include:

```c
// kernel/intr/apic/lapic_timer.c — add near other #include lines
#include <kernel/softirq.h>
```

Then modify `lapic_timer_handler` (around line 119-128):

```c
void lapic_timer_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    if (cpu_id() != 0) {
        this_cpu()->need_resched = 1;
        set_softirq_status(TIMER_SIRQ);  // ← NEW: APs process timer softirqs
    }
    /* watchdog_counter incremented for ALL CPUs */
    this_cpu()->watchdog_counter++;
    lapic_eoi();
}
```

**Why:** Without this, only the BSP processes timer callbacks via `do_softirq`.
With two CPUs both handling `TIMER_SIRQ`, `do_softirq` → `do_timer` may run
concurrently on both — but `timer_lock` (added in step 3) serialises access.

- [ ] **Step 1: Make softirq_status atomic**

In `kernel/intr/softirq.c`, replace non-atomic bit operations with inline asm:

```c
// kernel/intr/softirq.c
uint64_t softirq_status;

void set_softirq_status(uint64_t status)
{
    __asm__ __volatile__("lock orq %0, softirq_status(%rip)"
                         :: "r"(status) : "memory");
}

void do_softirq()
{
    int i;
    for(i = 0; i < 64 && softirq_status; i++)
    {
        if(softirq_status & (1 << i))
        {
            softirq_vector[i].action(softirq_vector[i].data);
            __asm__ __volatile__("lock andq %0, softirq_status(%rip)"
                                 :: "r"(~(1ULL << i)) : "memory");
        }
    }
}
```

- [ ] **Step 2: Add timer_list_lock**

In `kernel/time/timer.c`, add spinlock and protect all list accesses:

```c
// kernel/time/timer.c — add after #include lines
#include <kernel/arch/spinlock.h>

static spinlock_T timer_lock = { .lock = 1L };
```

- [ ] **Step 3: Lock add_timer, del_timer, do_timer**

```c
void add_timer(timer_t * timer)
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    timer_t * tmp = container_of(list_next(&timer_list_head.list), timer_t, list);
    if (!list_is_empty(&timer_list_head.list))
    {
        while (tmp->expire_jiffies < timer->expire_jiffies)
            tmp = container_of(list_next(&tmp->list), timer_t, list);
    }
    list_add_to_behind(&tmp->list, &timer->list);
    spin_unlock_irqrestore(&timer_lock, flags);
}

void del_timer(timer_t * timer)
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    list_del(&timer->list);
    spin_unlock_irqrestore(&timer_lock, flags);
}

void do_timer(void * data __attribute__((unused)))
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    timer_t * timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    while ((!list_is_empty(&timer_list_head.list)) && (timer->expire_jiffies <= jiffies))
    {
        del_timer(timer);      // NOTE: now calls spin_lock_irqsave on timer_lock → DEADLOCK
        timer->func(timer->data);
        timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    }
    spin_unlock_irqrestore(&timer_lock, flags);
}
```

**Critical:** `del_timer` also acquires `timer_lock` — recursive deadlock.
Fix `do_timer` to use `list_del` directly inside the already-held lock:

```c
void do_timer(void * data __attribute__((unused)))
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    timer_t * timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    while ((!list_is_empty(&timer_list_head.list)) && (timer->expire_jiffies <= jiffies))
    {
        list_del(&timer->list);                     // direct list_del, not del_timer()
        spin_unlock_irqrestore(&timer_lock, flags); // release before callback

        timer->func(timer->data);                   // callback runs unlocked

        flags = spin_lock_irqsave(&timer_lock);      // re-acquire
        timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    }
    spin_unlock_irqrestore(&timer_lock, flags);
}
```

- [ ] **Step 4: Protect PIT handler's peek**

In `kernel/driver/pit.c`, wrap the timer list peek:

```c
void pit_handler(uint64_t nr __attribute__((unused)), uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
    jiffies++;
    // ... poll timeout ...

    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;
    serial_poll();

    // Locked peek: only set TIMER_SIRQ if a timer is actually due
    extern spinlock_T timer_lock;  // declared in timer.c
    {
        uint64_t f = spin_lock_irqsave(&timer_lock);
        if (!list_is_empty(&timer_list_head.list)) {
            timer_t *first = container_of(list_next(&timer_list_head.list), timer_t, list);
            if (first->expire_jiffies <= jiffies)
                set_softirq_status(TIMER_SIRQ);
        }
        spin_unlock_irqrestore(&timer_lock, f);
    }
}
```

But `timer_lock` is `static` in `timer.c`.  Add an accessor:

```c
// kernel/time/timer.c — add:
int timer_has_expired(uint64_t now)
{
    if (list_is_empty(&timer_list_head.list)) return 0;
    timer_t *first = container_of(list_next(&timer_list_head.list), timer_t, list);
    return first->expire_jiffies <= now;
}
```

Declare in `kernel/include/device/timer.h`:
```c
// kernel/include/device/timer.h — add:
int timer_has_expired(uint64_t now);
```

Then in `pit.c`:
```c
if (timer_has_expired(jiffies))
    set_softirq_status(TIMER_SIRQ);
```

**Note:** timer_has_expired doesn't lock — it does a lockless peek.  The
worst case is a false positive (setting TIMER_SIRQ when the timer was just
removed) or false negative (missing a timer that was just added).  Both are
benign: TIMER_SIRQ triggers `do_timer` which re-checks under lock.  No lock
needed for the peek.

Revert to simpler approach: skip the PIT handler lock entirely — just keep
the existing lockless peek.  The `do_timer` path is now serialised by
`timer_lock` so the only concurrent access is PIT handler reading
`timer_list_head.list.next` while `do_timer`/`add_timer`/`del_timer` modify
it under lock.  On x86_64, aligned pointer reads are atomic, so PIT handler
sees either the old or new `->next` pointer — a false positive or false
negative, both benign at 100 Hz.

**Final approach for pit.c: no changes needed.**  The lockless peek is safe.

- [ ] **Step 5: Build and boot test**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
qemu-system-x86_64 -cdrom os01.iso -smp 2 -m 2G -serial stdio -no-reboot
```
Expected: boots to shell, timer softirqs work, no corruption.

- [ ] **Step 6: Commit**

```bash
git add kernel/intr/softirq.c kernel/time/timer.c kernel/include/device/timer.h kernel/intr/apic/lapic_timer.c
git commit -m "fix: atomic softirq_status + spinlock-protected timer list + AP TIMER_SIRQ

softirq_status: lock orq/andq for concurrent set/clear from
multiple CPUs in ret_from_intr paths.

timer list: global timer_lock protects add_timer/del_timer/do_timer.
do_timer releases lock during callback to avoid recursive deadlock.

Prerequisites for SMP load balancing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: COW TLB shootdown in fork_mm_copy

**Files:**
- Modify: `kernel/sched/task.c:1234-1236`

**Interfaces:**
- Produces: `fork_mm_copy()` uses `tlb_shootdown()` instead of local `flush_tlb()`

- [ ] **Step 1: Replace flush_tlb with tlb_shootdown**

In `fork_mm_copy()` (around line 1234-1236), find:

```c
    // TLB flush: parent's in-memory PTEs were modified (R/W->R/O+COW).
    // Only the current CPU runs the parent's mm — local flush is sufficient.
    flush_tlb();
```

Replace with:

```c
    // TLB shootdown: parent's in-memory PTEs were modified (R/W → R/O+COW).
    // With SMP load balancing the parent may run on any CPU — must
    // invalidate ALL cores' TLBs, not just the local one.
    tlb_shootdown();
```

- [ ] **Step 2: Build and boot test**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
qemu-system-x86_64 -cdrom os01.iso -smp 2 -m 2G -serial stdio -no-reboot
```
Expected: boots to shell, fork works, no COW corruption.

- [ ] **Step 3: Commit**

```bash
git add kernel/sched/task.c
git commit -m "fix: use tlb_shootdown in fork_mm_copy for SMP safety

Replaces local flush_tlb() with tlb_shootdown() to invalidate
COW page table changes on all CPUs.  With SMP load balancing
the parent may be running on any core after fork.

Prerequisite for SMP load balancing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Core Implementation (Phase 2)

### Task 4: rbtree_last + rbtree_prev

**Files:**
- Modify: `libc/rbtree/rbtree.c:142-169` (add after `rbtree_next`)
- Modify: `libc/include/rbtree.h:43-47`

**Interfaces:**
- Produces: `rbtree_node_t *rbtree_last(rbtree_root_t *root)` — O(log n), rightmost node
- Produces: `rbtree_node_t *rbtree_prev(rbtree_node_t *node)` — amortized O(1), inorder predecessor

- [ ] **Step 1: Implement rbtree_last and rbtree_prev**

In `libc/rbtree/rbtree.c`, add after `rbtree_next` (line 169):

```c
/* ── Tree maximum ──────────────────────────────────────── */

rbtree_node_t *rbtree_last(rbtree_root_t *root)
{
    rbtree_node_t *n = root->rb_node;
    if (!n) return NULL;
    while (n->right)
        n = n->right;
    return n;
}

/* ── Inorder predecessor ───────────────────────────────── */

rbtree_node_t *rbtree_prev(rbtree_node_t *node)
{
    if (node->left) {
        node = node->left;
        while (node->right)
            node = node->right;
        return node;
    }
    rbtree_node_t *p = node->parent;
    while (p && node == p->left) {
        node = p;
        p = p->parent;
    }
    return p;
}
```

- [ ] **Step 2: Add declarations**

In `libc/include/rbtree.h`, add after line 47 (`rbtree_next` declaration):

```c
/* Return the rightmost (maximum) node, or NULL if tree is empty. */
rbtree_node_t *rbtree_last(rbtree_root_t *root);

/* Return the inorder predecessor, or NULL if node is the leftmost. */
rbtree_node_t *rbtree_prev(rbtree_node_t *node);
```

- [ ] **Step 3: Build**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

- [ ] **Step 4: Commit**

```bash
git add libc/rbtree/rbtree.c libc/include/rbtree.h
git commit -m "feat(rbtree): add rbtree_last and rbtree_prev

Symmetrical to rbtree_first/rbtree_next.  Both O(log n).
Needed for sched_balance tail-steal traversal.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Systest rbtree_last/rbtree_prev verification

**Files:**
- Modify: `user/systest.c:936-1004` (add after existing rbtree tests)

- [ ] **Step 1: Add rbtree reverse-traversal test**

```c
// user/systest.c — add after test_rbtree_stress_100 (line 1004)

static int test_rbtree_last(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[3];
    n[0].key = 10; n[1].key = 20; n[2].key = 30;
    for (int i = 0; i < 3; i++)
        rbtree_insert(&root, &n[i].node, test_cmp);

    rbtree_node_t *last = rbtree_last(&root);
    if (!last || ((test_rb_node_t *)last)->key != 30) return 1;
    return 0;
}

static int test_rbtree_prev_traversal(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[5];
    int keys[] = {30, 10, 50, 20, 40};
    for (int i = 0; i < 5; i++) {
        n[i].key = keys[i];
        rbtree_insert(&root, &n[i].node, test_cmp);
    }

    // Reverse traversal: should visit 50, 40, 30, 20, 10
    int expected[] = {50, 40, 30, 20, 10};
    int idx = 0;
    for (rbtree_node_t *cur = rbtree_last(&root); cur; cur = rbtree_prev(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (idx >= 5 || tn->key != expected[idx]) return 1;
        idx++;
    }
    if (idx != 5) return 1;
    return 0;
}

static int test_rbtree_prev_null(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[2];
    n[0].key = 10; n[1].key = 20;
    rbtree_insert(&root, &n[0].node, test_cmp);
    rbtree_insert(&root, &n[1].node, test_cmp);

    // prev of first should be NULL
    rbtree_node_t *first = rbtree_first(&root);
    if (rbtree_prev(first) != NULL) return 1;

    // last of empty tree should be NULL
    rbtree_root_t empty;
    rbtree_init(&empty);
    if (rbtree_last(&empty) != NULL) return 1;

    return 0;
}

// Wrappers:
static void test_wrap_rbtree_last(void)
{ CHECK3(test_rbtree_last() == 0, "rbtree_last", "rightmost is max key"); }

static void test_wrap_rbtree_prev_traversal(void)
{ CHECK3(test_rbtree_prev_traversal() == 0, "rbtree_prev", "reverse inorder traversal"); }

static void test_wrap_rbtree_prev_null(void)
{ CHECK3(test_rbtree_prev_null() == 0, "rbtree_prev_null", "prev of first is NULL, last of empty is NULL"); }
```

- [ ] **Step 2: Register new tests in main()**

Find the existing rbtree test registrations (search for `test_wrap_rbtree` in systest.c) and add after them:

```c
    test_wrap_rbtree_last();
    test_wrap_rbtree_prev_traversal();
    test_wrap_rbtree_prev_null();
```

- [ ] **Step 3: Build and run systest**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
qemu-system-x86_64 -cdrom os01.iso -m 2G -serial stdio -no-reboot
# In shell: /systest.elf
```

Expected: all rbtree tests pass including the 3 new ones.

- [ ] **Step 4: Commit**

```bash
git add user/systest.c
git commit -m "test(systest): add rbtree_last/rbtree_prev unit tests

Verifies rightmost-node, reverse inorder traversal, and
edge cases (prev-of-first, last-of-empty).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: percpu_t nr_running field + enqueue/dequeue update

**Files:**
- Modify: `kernel/include/kernel/percpu.h:25-47`
- Modify: `kernel/sched/task.c:80-99` (enqueue_task, dequeue_task)
- Modify: `test/include/kernel/percpu.h:59-69`
- Modify: `test/include/kernel/task.h` (sync if needed)

**Interfaces:**
- Produces: `percpu_t.nr_running` (uint32_t), maintained by enqueue/dequeue

- [ ] **Step 1: Add nr_running to percpu_t**

In `kernel/include/kernel/percpu.h`, add after `uint64_t min_vruntime`:

```c
    uint64_t min_vruntime;      // per-CPU tracking of minimum vruntime
    spinlock_T rq_lock;          // protects rbtree operations
    uint32_t nr_running;         // count of tasks on runqueue (excl. idle)
    uint64_t watchdog_counter;  // incremented each timer tick, reset by schedule()
```

- [ ] **Step 2: Update enqueue_task**

In `kernel/sched/task.c`, modify `enqueue_task` (line 80):

```c
static void enqueue_task(task_t *task, percpu_t *rq)
{
    ASSERT(task != rq->idle);

    task->deadline = task->vruntime + EEVDF_MIN_SLICE;
    task->on_rq = true;
    rbtree_node_t *conflict = rbtree_insert(&rq->run_queue, &task->rb_node, cmp_deadline);
    ASSERT(conflict == NULL);
    rq->nr_running++;  // ← NEW
}
```

- [ ] **Step 3: Update dequeue_task**

In `kernel/sched/task.c`, modify `dequeue_task` (line 95):

```c
static void dequeue_task(task_t *task, percpu_t *rq)
{
    rbtree_erase(&rq->run_queue, &task->rb_node);
    task->on_rq = false;
    rq->nr_running--;  // ← NEW
}
```

- [ ] **Step 4: Sync test mock**

In `test/include/kernel/percpu.h`, add `uint32_t nr_running;` to the test `percpu_t` struct (anywhere after `schedule_count`):

```c
    uint64_t schedule_count, tsc_boot;
    uint32_t nr_running;   // ← NEW
```

- [ ] **Step 5: Build (must use make clean — struct layout changed)**

```bash
cd /home/aagu/OS01 && make clean && make -j$(nproc)
```

- [ ] **Step 6: Commit**

```bash
git add kernel/include/kernel/percpu.h kernel/sched/task.c test/include/kernel/percpu.h
git commit -m "feat(sched): add nr_running field to percpu_t

Incremented in enqueue_task, decremented in dequeue_task.
Provides O(1) runqueue load metric for sched_pick_cpu
and sched_balance busiest-CPU selection.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: sched_pick_cpu + sched_notify_remote

**Files:**
- Modify: `kernel/sched/task.c` (add after `cmp_deadline`, around line 77)

**Interfaces:**
- Produces: `static uint32_t sched_pick_cpu(void)` — returns least-loaded online CPU
- Produces: `static void sched_notify_remote(task_t *tsk)` — IPI + need_resched for remote wakeup

- [ ] **Step 1: Add #include and forward declaration**

At the top of `kernel/sched/task.c`, ensure these headers are present (they should already be):
```c
#include <kernel/ipi.h>    // ipi_send, IPI_VECTOR_RESCHED
```

Add forward declaration after `#define EEVDF_LATENCY 40` (around line 55):
```c
/* ── Forward declarations for load balancing ─────────── */
static void sched_balance(percpu_t *rq);
```

- [ ] **Step 2: Implement sched_pick_cpu**

Add after the EEVDF constants (around line 56):

```c
/* ── sched_pick_cpu: choose CPU with fewest nr_running ────
 * Called from do_fork() and spawn_user_task() to place
 * new tasks on the least-loaded CPU.
 * Complexity: O(num_cpus).  Acceptable for NR_CPUS ≤ 8.
 */
static uint32_t sched_pick_cpu(void)
{
    uint32_t me = cpu_id();
    uint32_t best = me;
    uint32_t min_nr = *(volatile uint32_t *)&percpu_data[me].nr_running;

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (!percpu_data[i].online) continue;
        uint32_t nr = *(volatile uint32_t *)&percpu_data[i].nr_running;
        if (nr < min_nr) {
            min_nr = nr;
            best = i;
        }
    }
    return best;
}
```

- [ ] **Step 3: Implement sched_notify_remote**

```c
/* ── sched_notify_remote: wake remote CPU after enqueue ──
 * Sets need_resched and sends reschedule IPI so the remote
 * CPU discovers the task immediately, not up to 10 ms later.
 *
 * If ipi_send() times out (10K ICR poll), the IPI is silently
 * dropped.  need_resched=1 is the fallback: the remote CPU
 * picks it up on the next LAPIC timer tick (≤10 ms).
 *
 * Called from do_fork() and spawn_user_task() after enqueue.
 */
static void sched_notify_remote(task_t *tsk)
{
    if ((int)tsk->cpu == (int)cpu_id())
        return;
    percpu_t *dst = &percpu_data[tsk->cpu];
    dst->need_resched = 1;
    __sync_synchronize();
    ipi_send(dst->arch_processor_id, IPI_VECTOR_RESCHED);
}
```

- [ ] **Step 4: Build**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): add sched_pick_cpu and sched_notify_remote

sched_pick_cpu: O(num_cpus) scan for CPU with fewest nr_running.
Uses volatile reads for lockless access to percpu nr_running.

sched_notify_remote: sets need_resched + sends reschedule IPI
to ensure remote CPU discovers new tasks immediately.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: sched_balance

**Files:**
- Modify: `kernel/sched/task.c` (add after `sched_notify_remote`)

**Interfaces:**
- Produces: `static void sched_balance(percpu_t *rq)` — unified pull/steal

- [ ] **Step 1: Implement sched_balance**

Add after `sched_notify_remote`:

```c
/* ── sched_balance: pull or steal tasks from busiest CPU ──
 *
 * Called from schedule() after zombie reaping, before pick_eevdf().
 *
 * Algorithm:
 *   1. Find busiest CPU (max nr_running, tiebreak max min_vruntime)
 *   2. Gate: proceed if local is idle OR gap ≥ 2 tasks
 *   3. Steal count = max(1, (src - local) / 2) from rbtree tail
 *   4. Double-lock rq_locks (address-ordered), single IRQ save
 *   5. For each task: dequeue from src, normalize vruntime, enqueue to local
 *   6. If src now empty: src.min_vruntime = 0
 *
 * Takes from the tail (largest deadline) — tasks that just used
 * their slice and won't be scheduled again soon.  Preserves source
 * CPU's hot-cache "about to run" tasks.
 */
static void sched_balance(percpu_t *rq)
{
    /* 1. Find busiest online CPU */
    int src_idx = -1;
    uint32_t max_nr = 0;
    uint64_t max_vr = 0;

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (i == rq->cpu_id || !percpu_data[i].online)
            continue;
        uint32_t nr = *(volatile uint32_t *)&percpu_data[i].nr_running;
        if (nr == 0)
            continue;
        uint64_t vr = *(volatile uint64_t *)&percpu_data[i].min_vruntime;
        if (nr > max_nr || (nr == max_nr && vr > max_vr)) {
            max_nr = nr;
            max_vr = vr;
            src_idx = (int)i;
        }
    }
    if (src_idx < 0)
        return;

    /* 2. Gate */
    if (rq->nr_running > 0) {
        /* Non-idle: require ≥ 2 task gap to prevent oscillation */
        if (max_nr <= rq->nr_running + 1)
            return;
    }
    /* rq->nr_running == 0: idle steal — unconditional */

    /* 3. Determine steal count */
    int count = (int)(max_nr - rq->nr_running) / 2;
    if (count < 1) count = 1;

    percpu_t *src_rq = &percpu_data[src_idx];

    /* 4. Double-lock, address-ordered, single IRQ save */
    spinlock_T *lo, *hi;
    if ((uintptr_t)&src_rq->rq_lock < (uintptr_t)&rq->rq_lock) {
        lo = &src_rq->rq_lock; hi = &rq->rq_lock;
    } else {
        lo = &rq->rq_lock; hi = &src_rq->rq_lock;
    }

    uint64_t flags = arch_local_irq_save();
    spin_lock(lo);
    if (lo != hi) spin_lock(hi);

    /* 5. Steal from tail */
    rbtree_node_t *node = rbtree_last(&src_rq->run_queue);
    int taken = 0;

    while (node && taken < count) {
        task_t *t = container_of(node, task_t, rb_node);

        /* Advance BEFORE erase (rbtree_erase invalidates node's
         * parent/left/right pointers used by rbtree_prev) */
        rbtree_node_t *prev = rbtree_prev(node);

        if (t != src_rq->idle) {
            dequeue_task(t, src_rq);   // rbtree_erase + on_rq=false + nr_running--
            t->cpu = rq->cpu_id;

            /* Normalize vruntime to target CPU's timeline */
            if (t->vruntime < rq->min_vruntime)
                t->vruntime = rq->min_vruntime;

            enqueue_task(t, rq);       // deadline set + on_rq=true + rbtree_insert + nr_running++
            taken++;
        }
        node = prev;
    }

    /* 6. Reset min_vruntime if source is now empty */
    if (src_rq->nr_running == 0)
        src_rq->min_vruntime = 0;

    spin_unlock(hi);
    if (lo != hi) spin_unlock(lo);
    arch_local_irq_restore(flags);

    if (taken > 0) {
        debug_sched("balance: CPU%u ← %d tasks from CPU%d (src_nr=%u local_nr=%u)\n",
                    rq->cpu_id, taken, src_idx,
                    (unsigned)src_rq->nr_running, (unsigned)rq->nr_running);
    }
}
```

**Critical details:**
- `rbtree_prev(node)` is called **before** `dequeue_task` erases the node.
  `dequeue_task` → `rbtree_erase` modifies parent/left/right pointers, which
  `rbtree_prev` depends on.  Pre-advancing avoids use-after-free.
- `lo != hi` guard: when both CPUs share the same runqueue (only possible if
  `src_idx == rq->cpu_id`, which we already filtered out), the `lo == hi` case
  is a single lock.  In practice `lo != hi` always, but the guard is defensive.

- [ ] **Step 2: Build**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): implement sched_balance — pull/steal with oscillation guard

Tail-steal from busiest CPU with gate: src.nr_running > local + 1.
Steal count = max(1, (diff)/2) for O(log N) convergence.
Vruntime normalized to target CPU timeline on migration.
Double-lock with address ordering and single IRQ save.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Integration (Phase 3)

### Task 9: do_fork integration

**Files:**
- Modify: `kernel/sched/task.c:1299-1313, 1391-1392`

**Interfaces:**
- Consumes: `sched_pick_cpu`, `sched_notify_remote`

- [ ] **Step 1: Modify do_fork CPU selection and fair_start**

Find `do_fork()` around line 1299.  Current code:
```c
    uint64_t fair_start = percpu_data[cpu_id()].min_vruntime;
    tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
```

Replace with (pick CPU first, then use target's min_vruntime):
```c
    uint32_t target_cpu = sched_pick_cpu();
    uint64_t fair_start = percpu_data[target_cpu].min_vruntime;
    tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
```

- [ ] **Step 2: Replace tsk->cpu assignment**

Find `tsk->cpu = cpu_id();` around line 1313.  Replace with:
```c
    tsk->cpu = target_cpu;
```

- [ ] **Step 3: Lock enqueue + notify**

Find the enqueue at line 1391-1392:
```c
    tsk->state = TASK_RUNNING;
    enqueue_task(tsk, &percpu_data[tsk->cpu]);
```

Replace with locked enqueue + remote notification:
```c
    tsk->state = TASK_RUNNING;
    {
        uint64_t flags = spin_lock_irqsave(&percpu_data[tsk->cpu].rq_lock);
        enqueue_task(tsk, &percpu_data[tsk->cpu]);
        spin_unlock_irqrestore(&percpu_data[tsk->cpu].rq_lock, flags);
    }
    sched_notify_remote(tsk);
```

- [ ] **Step 4: Build**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): integrate do_fork with load balancing

- Use sched_pick_cpu() to choose target CPU for child
- Use target CPU's min_vruntime for fair_start
- Hold target rq_lock during enqueue_task (prevents rbtree
  corruption from concurrent schedule() on remote CPU)
- Call sched_notify_remote() to wake remote CPU immediately

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: spawn_user_task integration

**Files:**
- Modify: `kernel/sched/task.c:738, 871`

**Interfaces:**
- Consumes: `sched_pick_cpu`, `sched_notify_remote`

- [ ] **Step 1: Replace CPU assignment**

Find `tsk->cpu = cpu_id();` at line 738.  Replace with:
```c
    tsk->cpu = sched_pick_cpu();          // place on least-loaded CPU
```

- [ ] **Step 2: Lock enqueue + notify**

Find line 871:
```c
    enqueue_task(tsk, &percpu_data[tsk->cpu]);
```

Replace with:
```c
    {
        uint64_t flags = spin_lock_irqsave(&percpu_data[tsk->cpu].rq_lock);
        enqueue_task(tsk, &percpu_data[tsk->cpu]);
        spin_unlock_irqrestore(&percpu_data[tsk->cpu].rq_lock, flags);
    }
    sched_notify_remote(tsk);
```

- [ ] **Step 3: Build**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

- [ ] **Step 4: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): integrate spawn_user_task with load balancing

- Use sched_pick_cpu() for spawn target CPU
- Hold target rq_lock during enqueue_task
- Call sched_notify_remote() for immediate remote discovery

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: schedule() integration + remove df->cpu=0

**Files:**
- Modify: `kernel/sched/task.c:288-410` (schedule function)
- Modify: `kernel/sched/task.c:1519` (remove `df->cpu = 0`)

**Interfaces:**
- Consumes: `sched_balance`

- [ ] **Step 1: Add sched_balance call in schedule()**

In `schedule()` (line 288), find the spot after zombie reaping (steps 1-3) and before `pick_eevdf` (step 4).  After the `spin_unlock_irqrestore(&task_list_lock, reap_flags)` at the end of step 3 (line 374), add:

```c
        sched_unblock_blocked();
        spin_unlock_irqrestore(&task_list_lock, reap_flags);
    }

    // ── 3.5 Load balancing ───────────────────────────────
    sched_balance(rq);

    // ── 4. Pick next task (rbtree O(log n)) ─────────────────
    task_t *next;
```

- [ ] **Step 2: Remove df->cpu = 0 override**

Find around line 1519 in `task_init()`:
```c
    task_t *df = deferred_free_spawn();
    if (df) df->cpu = 0;
```

Remove the `if (df) df->cpu = 0;` line:
```c
    task_t *df = deferred_free_spawn();
```

(`deferred_free_spawn()` → `create_kthread()` → `do_fork()` now uses
`sched_pick_cpu()` automatically — no override needed.)

- [ ] **Step 3: Build**

```bash
cd /home/aagu/OS01 && make -j$(nproc)
```

- [ ] **Step 4: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(sched): integrate sched_balance into schedule() + remove df->cpu=0

Call sched_balance(rq) after zombie reaping, before pick_eevdf.
Remove df->cpu=0 override — deferred_free_spawn already uses
sched_pick_cpu via do_fork.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Verification (Phase 4)

### Task 12: Multi-core boot and systest

- [ ] **Step 1: Full build (must use make clean — multiple struct changes)**

```bash
cd /home/aagu/OS01 && make clean && make -j$(nproc)
```

- [ ] **Step 2: Boot with 2 CPUs**

```bash
qemu-system-x86_64 -cdrom os01.iso -smp 2 -m 2G -serial stdio -no-reboot
```

Expected:
- AP boots (check for "SMP: AP 1 (APIC ID ...) booted successfully")
- Shell appears
- No slab corruption, no page faults

- [ ] **Step 3: Run systest**

At the shell:
```
/systest.elf
```

Expected: 70/70 (or current total) tests pass, including the 4 new rbtree tests.

**CAVEAT:** Some systests may implicitly assume single-CPU semantics (e.g.,
`schedule_count` exact values, timing assumptions about `jiffies` which
only increments on BSP).  If any test fails under `-smp 2`, inspect the
failure — it may need tweaking for multi-CPU, or may reveal a real
scheduling bug.  Existing rbtree and syscall tests should be unaffected.

- [ ] **Step 4: Verify load distribution**

At the shell, run a command that forces fork:
```
ls /
```

Add a temporary debug print in `schedule()` to verify distribution:
```c
// In schedule(), after sched_balance(rq):
if (rq->schedule_count % 500 == 0)
    debug_sched("CPU%u: schedule_count=%lu nr_running=%u\n",
                rq->cpu_id, rq->schedule_count, rq->nr_running);
```

Both CPUs should show growing `schedule_count`.  For 2 CPUs with 3+ tasks,
`nr_running` should stabilise near (N/2) ± 1 on each CPU (not oscillating
every tick).

Also verify that `sched_balance` debug output shows occasional migrations
from the debug_sched line at the end of `sched_balance`.

Remove these debug prints before committing (they're 100 Hz noise in production).

- [ ] **Step 5: Commit any debug tweaks**

If debug prints were added for testing, remove them before final commit.

- [ ] **Step 6: Final commit**

```bash
git add -A
git commit -m "test(smp): verify multi-core boot + systest pass with SMP load balancing

Confirmed: AP boots, shell functional, systest 70/70 pass,
schedule_count increments on all CPUs.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task Dependency Graph

```
T0  (task_wake retry) ── prerequisite for T8
T1a (PMM lock) ───────┐
T1  (slab lock) ──────┤
T2  (softirq+timer) ──┼── Prerequisites (parallel, internal order)
T3  (COW tlb) ────────┘
        │
T4 (rbtree_last/prev) ─── rbtree extensions
T5 (systest rbtree) ───── test immediately
        │
T6 (nr_running) ───────── data structure
        │
T7 (pick_cpu+notify) ──── core helpers
T8 (sched_balance) ────── depends on T0, T6, T7
        │
T9  (do_fork integ) ───── depends on T7, T8
T10 (spawn integ) ─────── depends on T7, T8
        │
T11 (schedule integ) ──── depends on T8
        │
T12 (multi-core test) ─── verify everything
```

T0 must complete before T8.
T1a-T3 are independent of each other (modify different subsystems).
T9-T10 are independent of each other.
