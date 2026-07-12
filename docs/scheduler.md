# Task system and scheduler

**Preemptive round-robin scheduler with CPU affinity** — timer-driven, global task list with per-CPU affinity (analogous to early Linux 2.0 "giant lock" SMP).

## How preemption works

1. **PIT hardirq** (`driver/pit.c` — `pit_handler`): fires at 100Hz, increments `jiffies`, sets `this_cpu()->need_resched = 1`
2. **LAPIC timer IRQ** (APs only): fires at 100Hz, sets `this_cpu()->need_resched = 1`
3. **Interrupt return** (`arch/x86_64/entry.S` — `ret_from_intr`): reads `%gs:8` (percpu `need_resched`), calls `schedule()` if set
4. **`schedule()`**: holds `tasklist_lock` with `spin_lock_irqsave`, reaps zombies, decrements quantum, round-robins the global list filtering by `next->cpu == this_cpu()->cpu_id`, falls back to `percpu_data[me].idle`

## Key structures

```c
task_t:
    list_t list;              // linked into global task list
    volatile int64_t state;   // TASK_RUNNING/INTERRUPTIBLE/UNINTERRUPTIBLE/ZOMBIE/STOPPED
    uint64_t flags;           // PF_KTHREAD (bit 0), PF_PROCESS (bit 1), PF_THREAD (bit 2),
                              // PF_LINUX_ABI (bit 3), PF_REAPED (bit 4)
    mm_t *mm;                 // memory descriptor (NULL for pure kthreads)
    thread_t *thread;         // saved context for switch_to
    uint64_t addr_limit;      // 0x00007FFFFFFFFFFF for user, 0xffff800000000000 for kernel
    int64_t pid;              // process ID (atomic counter)
    int64_t counter;          // time slice remaining (ticks)
    int64_t signal;           // pending signal mask
    int64_t blocked;          // blocked signal mask (bit N = signal N+1 blocked)
    int64_t priority;         // scheduling priority (= quantum in ticks at 100Hz)
    uint32_t cpu;             // CPU affinity — which CPU owns this task
    blocker_t blocker;        // BLOCKER_NONE/BLOCKER_WAITPID
    blocker_data_t blocker_data; // waited_child, waited_pid
    void *fpu_save;           // 512-byte FXSAVE area
    void *stack_alloc_base;   // original malloc ptr (before STACK_SIZE alignment)
    struct files_struct *files; // per-process fd table
    struct task_struct *parent; // parent process
    int64_t exit_code;
    list_t wait_list;         // tasks waiting on this process
    list_t io_wait_node;      // I/O wait queue node
    struct sigaction sighand[NSIG]; // signal handlers

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

## Task states

| State | Value | Meaning |
|-------|-------|---------|
| `TASK_RUNNING` | `1 << 0` | Runnable or currently executing |
| `TASK_INTERRUPTIBLE` | `1 << 1` | Sleeping, can be woken by signal |
| `TASK_UNINTERRUPTIBLE` | `1 << 2` | Sleeping, cannot be interrupted |
| `TASK_ZOMBIE` | `1 << 3` | Exited, awaiting reaping |
| `TASK_STOPPED` | `1 << 4` | Stopped by signal |

## Default time slices (100Hz = 10ms/tick)

| Task type | Priority | Quantum |
|-----------|----------|---------|
| Idle task | 2 | 20ms |
| Kernel threads (do_fork) | 3 | 30ms |
| User tasks | 5 | 50ms |

## Context switching

- `get_current_task()`: `RSP & ~(STACK_SIZE - 1)` — masks lower 15 bits to find task base
- `switch_to(prev, next)`: inline asm saves prev RSP → loads next RSP → pushes next->rip → `jmp __switch_to`
- `__switch_to(prev, next)`: updates per-CPU TSS.rsp0 via `this_cpu()->tss`, swaps FS selector (GS base is per-CPU, never touched), switches CR3 if needed
- **Kernel threads** (PF_KTHREAD): `thd->rip` = `kernel_thread_func` which pops pt_regs from stack
- **User processes**: `thd->rip` = `ret_from_intr` (RESTORE_ALL + iretq path)
- **Per-CPU scheduler guard**: `this_cpu()->scheduler_ok` — set by `ap_entry()`/`task_init()`. Timer ticks before this is set are no-ops.

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
9. Sets `task.cpu = cpu_id()` (affinity), adds to global list under `tasklist_lock`
10. Returns PID on success, -1 on error

### do_fork(regs, clone_flags, stack_start, stack_size)
- Allocates task_union + thread_t
- Copies current task struct to child
- Copies pt_regs from parent to child kernel stack
- For user tasks (`regs->cs & 3`): sets `thd->rip = ret_from_intr`
- Adds to global list under `tasklist_lock`

### kernel_thread(fn, arg, flags)
- Creates a synthetic pt_regs with `rbx=fn`, `rdx=arg`, `rip=kernel_thread_func`
- Calls `do_fork()` → child pops regs and calls `fn(arg)` via `kernel_thread_func`

### create_kthread(fn, arg, name)
- Creates a PF_KTHREAD task (no user address space) that runs `fn(arg)`, then calls `do_exit(0)`
- Returns `task_t*` (or NULL on malloc failure) — lower-level than kernel_thread, no synthetic pt_regs needed
- Used for background kernel services (e.g. mutex test threads)

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

- `blocker_t.type`: `BLOCKER_NONE` (not blocked) or `BLOCKER_WAITPID` (waiting on child)
- `blocker_t.check`: `blocker_check_t` callback — returns `true` when wake condition met
- `blocker_t.signal_can_wake`: if true, a pending signal breaks the block
- `schedule()` skips blocked tasks whose `check` callback returns `false`
- `blocker_data_t`: per-type payload (`waited_child`, `waited_pid`)

### I/O wait queues

Devices (tty, pipe) maintain a `list_t` of tasks blocked on I/O:

- `wait_queue_sleep(task)`: sets task `TASK_INTERRUPTIBLE`, adds `task->io_wait_node` to device queue, calls `schedule()`
- `wait_queue_wake(device_queue)`: iterates the queue, sets each task back to `TASK_RUNNING`, removes from queue
- Used by tty read/write and pipe read/write to block when buffers are empty/full

## Task exit

- `do_exit(exit_code)`: frees user page tables (`vmm_free_user_map` + `kfree(mm)`), marks task `TASK_ZOMBIE`, calls `schedule()` — never returns
- **Zombie reaping**: `schedule()` scans the global list for `TASK_ZOMBIE` with `PF_REAPED` set (skipping current — "we're on its stack"), frees `thread_t*` and `stack_alloc_base`. Tasks with `TASK_ZOMBIE` but not yet `PF_REAPED` are visible to `do_waitpid` and are not freed until reaped.

## Known pitfalls

1. **`~32768` ≠ `~(STACK_SIZE - 1)`**. Always use `~(STACK_SIZE - 1)` or `-STACK_SIZE` to clear all 15 low bits of RSP.
2. **`memcpy(dest, src, size)`** — first arg is destination. Copy pt_regs TO the new task stack.
3. **TSS.rsp0** must point to the current task's kernel stack (base of task_union + STACK_SIZE), set by `__switch_to`. Not a fixed exception stack.
4. **__switch_to** sets TSS.rsp0 to `next->thread->rsp0`, enabling ring-3 interrupts to land on the correct kernel stack.
5. **set_tss64() writes global TSS64_Table** — only safe with `-smp 1`. Per-CPU TSS descriptor needed for `-smp 2+`.
6. **GS must never be reloaded from a selector** — `kernel_thread_func` and `__switch_to` skip GS. Loading a selector clobbers the per-CPU MSR base.
7. **After struct changes**: `make clean` is mandatory (no header dependency tracking in Makefile).
8. **Concurrent spawn with I/O**: `/spin.elf` (exit-only) works 8+ concurrent; `/init.elf` (printf+keyboard) is more sensitive to concurrent spawn count — see [[spawn-ud-crash-syscall-prefault]].

## Related

- [Memory: process-scheduler](/home/aagu/.claude/projects/-home-aagu-OS01/memory/process-scheduler.md)
- [docs/smp.md](smp.md) — SMP scheduler details
