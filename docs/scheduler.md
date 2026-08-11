# Task system and EEVDF scheduler

**EEVDF (Earliest Eligible Virtual Deadline First) scheduler with per-CPU rbtree runqueues** — Linux 6.6-style O(log n) fair scheduling with SMP work-stealing load balancing. The old O(n) round-robin global-list design has been replaced.

> **Complexity note**: EEVDF is overkill for 2 CPUs / single-digit task counts but was implemented for correctness and future-proofing. See [scheduler-complexity.md](scheduler-complexity.md) for an assessment of which parts are mandatory vs. optional, and when a refactor is worth it.

## How preemption works

1. **PIT hardirq** (`driver/pit.c` — `pit_handler`): fires at 100Hz, increments `jiffies`, sets `this_cpu()->need_resched = 1`
2. **LAPIC timer IRQ** (APs only): fires at 100Hz, sets `this_cpu()->need_resched = 1`
3. **Interrupt return** (`arch/x86_64/entry.S` — `ret_from_intr`): reads `%gs:8` (percpu `need_resched`), calls `schedule()` if set
4. **`schedule()`**: acquires per-CPU `rq_lock` with `spin_lock_irqsave`, runs `update_curr`/`dequeue_task`/zombie reaper/`sched_balance`/`pick_eevdf`/`switch_to`

## Key structures

```c
task_t:
    list_t list;              // linked into global task list (for waitpid/zombie scanning only)
    volatile int64_t state;   // TASK_RUNNING/INTERRUPTIBLE/UNINTERRUPTIBLE/ZOMBIE/STOPPED
    uint64_t flags;           // PF_KTHREAD (bit 0), PF_PROCESS (bit 1), PF_THREAD (bit 2),
                              // PF_LINUX_ABI (bit 3), PF_REAPED (bit 4)
    mm_t *mm;                 // memory descriptor (NULL for pure kthreads)
    thread_t *thread;         // saved context for switch_to
    uint64_t addr_limit;      // 0x00007FFFFFFFFFFF for user, 0xffff800000000000 for kernel
    int64_t pid;              // process ID (atomic counter)
    int64_t signal;           // pending signal mask
    int64_t blocked;          // blocked signal mask (bit N = signal N+1 blocked)
    uint32_t cpu;             // CPU affinity — which CPU owns this task (for SMP)
    uint32_t on_cpu;          // 1 if currently executing on a CPU
    uint32_t on_rq;           // 1 if enqueued in per-CPU rbtree runqueue
    uint32_t in_schedule;     // guard: 1 while schedule() holds rq_lock
    blocker_t blocker;        // BLOCKER_NONE/BLOCKER_WAITPID/etc.
    blocker_data_t blocker_data; // waited_child, waited_pid
    void *fpu_save;           // 512-byte FXSAVE area
    void *stack_alloc_base;   // original malloc ptr (before STACK_SIZE alignment)
    struct files_struct *files; // per-process fd table
    struct task_struct *parent; // parent process
    int64_t exit_code;
    list_t wait_list;         // tasks waiting on this process
    struct sigaction sighand[NSIG]; // signal handlers

    // EEVDF fields
    uint64_t vruntime;        // virtual runtime, advanced 1 per tick while running
    uint64_t deadline;        // vruntime + EEVDF_MIN_SLICE, used as rbtree sort key
    rbtree_node_t rb_node;    // node in per-CPU runqueue rbtree

thread_t:
    uint64_t rsp0;            // TSS.rsp0 — kernel stack base for ring-3→ring-0
    uint64_t rip;             // saved instruction pointer
    uint64_t rsp;             // saved stack pointer (top of stack)
    uint64_t fs;              // FS segment selector
    uint64_t gs;              // GS selector (NOT restored — GS base is per-CPU via MSR)
    uint64_t cr3;             // page table base (physical address)
    uint64_t cr2;             // faulting address from page fault
    uint64_t trap_nr;         // exception/interrupt number
    uint64_t error_code;      // hardware error code

mm_t:
    uint64_t *pml4;           // PML4 physical address for CR3
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;   // heap boundaries
    uint64_t start_stack;          // user stack base (0x800000)
    list_t vma_list;              // sorted by vm_start
    uint64_t mmap_base;           // start search for mmap
```

Tasks embed a 32KB stack via `union task_union { task_t task; char stack[32768]; }`, 32KB-aligned in `.data.init_task`.

## Per-CPU runqueue

Each CPU has a private rbtree runqueue in `percpu_t`:

```
run_queue:    rbtree_root_t — sorted by deadline (earliest deadline = first)
rq_lock:      spinlock_T — protects all rbtree operations on this CPU
nr_running:   uint32_t — count of tasks on runqueue (excluding idle)
```

Access pattern: `spin_lock_irqsave(&percpu_data[cpu].rq_lock)` before any `rbtree_insert`/`rbtree_erase`/`rbtree_first`.

## EEVDF algorithm

### Constants

```c
EEVDF_MIN_SLICE = 10   // time slice = 10 ticks = 100ms
EEVDF_LATENCY   = 40   // eligibility window = 40 ticks = 400ms
```

### Core functions

1. **`update_curr`**: advances `current->vruntime += 1` each tick. If `vruntime >= deadline`, re-enqueues with `deadline = vruntime + EEVDF_MIN_SLICE`.
2. **`cmp_deadline(a, b)`**: rbtree comparator — earlier deadline sorts first.
3. **`enqueue_task(task, rq)`**: sets `deadline = vruntime + EEVDF_MIN_SLICE`, calls `rbtree_insert` into the per-CPU `run_queue`.
4. **`dequeue_task(task, rq)`**: calls `rbtree_erase` from the per-CPU `run_queue`.
5. **`pick_eevdf(rq)`**: returns `rbtree_first` — the task with the earliest deadline. If the first task's `vruntime > min_vruntime + EEVDF_LATENCY`, advances `min_vruntime`.

### Task wakeup boost

When a task wakes from blocking, its `vruntime` is raised to `min_vruntime - EEVDF_LATENCY` (floor at 0) to prevent starvation from accumulated sleep time.

## SMP load balancing

`sched_balance(rq)` — called by `schedule()` on the local CPU **before** `pick_eevdf`:

1. Find the busiest CPU (max `nr_running`, tiebreak on max `min_vruntime`)
2. Gate: only pull if `src.nr_running > local.nr_running + 1` (≥2 task gap — oscillation guard)
3. Steal count = `max(1, (src - local) / 2)` — convergence-based, not overcorrection
4. Steal from the **tail** of the source rbtree (largest deadline = least likely to run soon)
5. Double-lock both `rq_lock`s (address-ordered, single IRQ save)
6. Normalize each stolen task's `vruntime` to `max(vruntime, target.min_vruntime)`
7. Enqueue on local runqueue

**CPU selection at creation time**: `sched_pick_cpu()` picks the CPU with the lowest `nr_running`. `sched_notify_remote()` sends an IPI reschedule to the target CPU if a task was placed on a remote idle CPU.

## Task states

| State | Value | Meaning |
|-------|-------|---------|
| `TASK_RUNNING` | `1 << 0` | Runnable or currently executing |
| `TASK_INTERRUPTIBLE` | `1 << 1` | Sleeping, can be woken by signal |
| `TASK_UNINTERRUPTIBLE` | `1 << 2` | Sleeping, cannot be interrupted |
| `TASK_ZOMBIE` | `1 << 3` | Exited, awaiting reaping |
| `TASK_STOPPED` | `1 << 4` | Stopped by signal |

## Context switching

- `get_current_task()`: `RSP & ~(STACK_SIZE - 1)` — masks lower 15 bits to find task base
- `switch_to(prev, next)`: inline asm saves prev RSP → loads next RSP → pushes next->rip → `jmp __switch_to`
- `__switch_to(prev, next)`: updates per-CPU TSS.rsp0 via `this_cpu()->tss`, swaps FS selector (GS base is per-CPU, never touched), switches CR3 if needed
- **Kernel threads** (PF_KTHREAD): `thd->rip` = `kernel_thread_func` which pops pt_regs from stack
- **User processes**: `thd->rip` = `ret_from_intr` (RESTORE_ALL + iretq path)
- **Per-CPU scheduler guard**: `this_cpu()->scheduler_ok` — set by `ap_entry()`/`task_init()`. Timer ticks before this is set are no-ops.
- **`on_cpu`/`on_rq`/`in_schedule`**: atomic ACQUIRE/RELEASE semantics prevent double-book (one task on two CPUs). See [scheduler-complexity.md](scheduler-complexity.md) for the race analysis.

## Task creation

### spawn_user_task(path, argv)
1. Opens ELF via `vfs_lookup(path)`
2. Validates ELF header with `elf_validate()`
3. Allocates `task_union` (32KB-aligned malloc) + `thread_t` + `mm_t` via malloc/calloc
4. Creates fresh PML4 (vmm_alloc_map), copies kernel entries from init PML4 (slots 256-511)
5. Loads ELF segments into new address space via `elf_load()`
6. Maps user stack page (2MB) at `USER_STACK_BASE` (0x800000); the 2MB page at 0x600000 is left unmapped as a stack guard
7. Sets up `pt_regs` on kernel stack: CS=USER_CS, SS/DS/ES=USER_DS, RSP=USER_STACK_TOP, RIP=entry_point, RFLAGS IF=1
8. Sets `thd->rip = ret_from_intr` → first entry through RESTORE_ALL → iretq to ring 3
9. Calls `sched_pick_cpu()` → picks CPU with lowest nr_running; sets `task.cpu`
10. Enqueues via `enqueue_task()` under target CPU's `rq_lock`
11. Sends IPI reschedule to target CPU if different from current
12. Returns PID on success, -1 on error

### do_fork(regs, clone_flags, stack_start, stack_size)
- Allocates task_union + thread_t
- Copies current task struct to child
- Deep-copies address space: PML4 + page tables via COW (4KB pages: parent PTE marked R/O+COW, child shares; 2MB pages: copy)
- Copies pt_regs from parent to child kernel stack
- For user tasks (`regs->cs & 3`): sets `thd->rip = ret_from_intr`
- Enqueues on current CPU

### kernel_thread(fn, arg, flags)
- Creates a synthetic pt_regs with `rbx=fn`, `rdx=arg`, `rip=kernel_thread_func`
- Calls `do_fork()` → child pops regs and calls `fn(arg)` via `kernel_thread_func`

### create_kthread(fn, arg, name)
- Creates a PF_KTHREAD task (no user address space) that runs `fn(arg)`, then calls `do_exit(0)`
- Returns `task_t*` (or NULL on malloc failure) — lower-level than kernel_thread, no synthetic pt_regs needed
- Used for background kernel services (e.g. deferred-free reaper, mutex test threads)

### sys_exec(path, regs, argv, envp)
- Opens and validates new ELF
- Creates fresh PML4 + mm + loads ELF + maps user stack
- Frees old user address space
- Installs new mm, switches CR3
- Overwrites current pt_regs frame (so iretq lands in new process)
- Copies argv and envp strings to user stack, sets RDI/RSI in pt_regs

### do_waitpid(pid, user_status, options)

- Scans global task list for children of current matching `pid`
- If a child is `TASK_ZOMBIE` without `PF_REAPED`: copies `exit_code` to `user_status`, sets `PF_REAPED`, frees `thread_t*` and `stack_alloc_base`, returns child PID
- If `TASK_ZOMBIE` with `PF_REAPED` already set: returns -ECHILD (already reaped by a prior waiter)
- With `WNOHANG`: returns 0 if no child has exited (does not block)
- Without `WNOHANG`: sets `blocker` to `BLOCKER_WAITPID` and calls `schedule()` — task re-wakes when a child exits (blocker_check_t callback returns true)

### Blocker framework

Tasks register a blocking condition instead of busy-waiting:

- `blocker_t.type`: `BLOCKER_NONE` (not blocked), `BLOCKER_WAITPID` (waiting on child), or device-specific types
- `blocker_t.check`: `blocker_check_t` callback — returns `true` when wake condition met
- `blocker_t.signal_can_wake`: if true, a pending signal breaks the block
- `schedule()` skips blocked tasks whose `check` callback returns `false`
- `blocker_data_t`: per-type payload (`waited_child`, `waited_pid`)
- `blocker_wait(check, type, signal_can_wake)`: installs blocker, marks TASK_INTERRUPTIBLE, calls schedule(). Returns 0 (condition met) or -EINTR (signal woke us)

## Task exit

- `do_exit(exit_code)`: frees user page tables (`vmm_free_user_map` + `kfree(mm)`), marks task `TASK_ZOMBIE`, wakes parent via blocker, calls `schedule()` — never returns
- **Zombie reaping**: `schedule()` scans the global list for `TASK_ZOMBIE` with `PF_REAPED` set (skipping current — "we're on its stack"), frees `thread_t*` and `stack_alloc_base`. Tasks with `TASK_ZOMBIE` but not yet `PF_REAPED` are visible to `do_waitpid` and are not freed until reaped.
- **Deferred free**: a dedicated kthread (`df-reaper`) handles async freeing for tasks that exited on a remote CPU or during complex teardown paths where direct free isn't safe.

## Key files

| File | Purpose |
|------|---------|
| `kernel/sched/task.c` | EEVDF core, fork/exec/exit, zombie reaper, sched_balance, blocker framework |
| `kernel/sched/deferred_free.c` | Async deferred-free kthread for remote-CPU task teardown |
| `kernel/arch/x86_64/smp.c` | `ap_entry()`, `smp_boot_aps()` — AP bringup and idle loop |
| `kernel/arch/x86_64/entry.S` | `ret_from_intr` → need_resched check → schedule() |
| `kernel/include/kernel/task.h` | `task_t` definition with EEVDF fields |
| `kernel/include/kernel/percpu.h` | `percpu_t` with `run_queue`, `rq_lock`, `nr_running` |

## Known pitfalls

1. **`~32768` ≠ `~(STACK_SIZE - 1)`**. Always use `~(STACK_SIZE - 1)` or `-STACK_SIZE` to clear all 15 low bits of RSP.
2. **`memcpy(dest, src, size)`** — first arg is destination. Copy pt_regs TO the new task stack.
3. **TSS.rsp0** must point to the current task's kernel stack (base of task_union + STACK_SIZE), set by `__switch_to`. Not a fixed exception stack.
4. **__switch_to** sets TSS.rsp0 to `next->thread->rsp0`, enabling ring-3 interrupts to land on the correct kernel stack.
5. **GS must never be reloaded from a selector** — `kernel_thread_func` and `__switch_to` skip GS. Loading a selector clobbers the per-CPU MSR base.
6. **on_cpu/on_rq access must use atomic ACQUIRE/RELEASE** — non-atomic access causes lost-update races across CPUs. The double-book bug (one task on two CPUs) was a 4-layer fix around this.
7. **After struct changes**: `make clean` is mandatory (no header dependency tracking in Makefile).

## Related

- [docs/scheduler-complexity.md](scheduler-complexity.md) — 复杂度评估、风险地图、重构触发条件
- [docs/smp.md](smp.md) — SMP bringup, IPI, percpu, load balancing details
- [docs/signal.md](signal.md) — 信号投递机制（与调度器在 ret_from_intr 交汇）
