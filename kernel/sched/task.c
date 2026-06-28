#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/slab.h>

#include <fs/vfs.h>
#include <fs/elf.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <uapi/time.h>

void __switch_to(task_t *prev, task_t *next)
{
    // Use per-CPU TSS — each CPU has its own TSS descriptor
    // with rsp0 pointing to the current task's kernel stack.
    percpu_t *cpu = this_cpu();
    cpu->tss->rsp0 = next->thread->rsp0;

    set_tss64(cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
              cpu->tss->ist1, cpu->tss->ist2, cpu->tss->ist3,
              cpu->tss->ist4, cpu->tss->ist5, cpu->tss->ist6,
              cpu->tss->ist7);

    // Save/restore FS selector (used by kernel threads).
    // GS base is per-CPU and set ONCE via MSR — never
    // touch it here (loading a non-null GS selector would
    // reload the base from the GDT, clobbering the MSR).
    __asm__ __volatile__("movq %%fs, %0 \n\t":"=a"(prev->thread->fs));
    __asm__ __volatile__("movq %0, %%fs \n\t"::"a"(next->thread->fs));

    // Switch page table if the next task has its own address space
    if (next->thread->cr3 && next->thread->cr3 != prev->thread->cr3) {
        __asm__ __volatile__("movq %0, %%cr3" :: "r"(next->thread->cr3) : "memory");
    }
}

// ── Preemption flag ──────────────────────────────────────
// Now per-CPU (percpu_t.need_resched, offset 8 from GS base).
// Set by timer IRQ on every tick, cleared by schedule() after
// a context switch.  entry.S reads it via %gs:8.

// Global PID counter — atomic because spawn/fork/exec may
// race on different CPUs.
static volatile uint64_t pid_counter = 1;

// ── User-space init task pointer ─────────────────────────
// Set by spawn_user_task() the first time it creates a user task.
// do_exit() uses this to reparent orphans and protect the init process.
static task_t *user_init_task = NULL;
static int64_t  user_init_pid = 0;

// Per-CPU scheduler guard — set to 1 by task_init() on each CPU.
// schedule() returns immediately before this point (ticks before
// the scheduler is set up are harmless no-ops).

void schedule(void)
{
    task_t *next;

    if (!this_cpu()->scheduler_ok)
        return;

    this_cpu()->schedule_count++;

    // ── Early return: current still has quantum ────────────
    // Safe without lock — only touches per-CPU 'current'.
    if (current->counter > 0)
        current->counter--;

    if (current->state == TASK_RUNNING && current->counter > 0) {
        this_cpu()->need_resched = 0;
        return;
    }

    // ── Zombie reaping + round-robin scan (serialised) ─────
    // Both the scan and the zombie reaper traverse the global
    // task list.  Holding the lock across both prevents an SMP
    // race where CPU 1 kfree's a zombie while CPU 0 is following
    // its list.next pointer.
    {
        static spinlock_T reap_lock = { .lock = 1L };
        uint64_t reap_flags = spin_lock_irqsave(&reap_lock);

        // Pass 1: collect zombies to reap
        task_t *reap_list[64];
        int reap_count = 0;

    {
        list_t *pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list && reap_count < 64) {
            task_t *t = container_of(pos, task_t, list);
            pos = pos->next;
            if (t->state != TASK_ZOMBIE || t == current)
                continue;

            int reap = 0;
            if (t->flags & PF_REAPED) {
                reap = 1;
            } else if (t->flags & PF_KTHREAD) {
                reap = 1;
            } else if (t->parent == NULL) {
                reap = 1;
            } else {
                if (t->parent->state == TASK_ZOMBIE)
                    reap = 1;
            }

            if (reap)
                reap_list[reap_count++] = t;
        }
    }

    // Pass 2: orphan children of zombies being reaped
    if (reap_count > 0) {
        list_t *pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *child = container_of(pos, task_t, list);
            pos = pos->next;
            if (!child->parent) continue;
            for (int i = 0; i < reap_count; i++) {
                if (child->parent == reap_list[i]) {
                    child->parent = NULL;
                    break;
                }
            }
        }
    }

    // Pass 3: unlink and free zombie resources.
    // NOTE: kfree(t->stack_alloc_base) is DEFERRED — it frees
    // the task_t itself (embedded in the stack allocation).
    // Doing so corrupts subsequent allocations from the same
    // slab cache (the freed memory is re-used before list
    // consumers have finished with the stale node pointers).
    // TODO: add a deferred-free work queue for stack_alloc_base.
    for (int i = 0; i < reap_count; i++) {
        task_t *t = reap_list[i];
        list_del(&t->list);
        if (t->thread)
            kfree(t->thread);
        if (t->files)
            files_free(t->files);
    }

    // ── Round-robin scan (inside lock) ─────────────────
    next = container_of(init_task_union.task.list.next, task_t, list);
    while (next != &init_task_union.task) {
        if (next->state == TASK_RUNNING &&
            next->cpu == (int)this_cpu()->cpu_id) {
            spin_unlock_irqrestore(&reap_lock, reap_flags);
            goto do_switch;
        }
        next = container_of(next->list.next, task_t, list);
    }

    spin_unlock_irqrestore(&reap_lock, reap_flags);
    }

    // No other RUNNING task found — give current a fresh quantum
    if (current->state == TASK_RUNNING) {
        current->counter = current->priority > 0 ? current->priority : 1;
        this_cpu()->need_resched = 0;
        return;
    }

    // Last resort: this CPU's idle task
    {
        percpu_t *me = this_cpu();
        if (me->idle && me->idle->state == TASK_RUNNING) {
            next = me->idle;
            goto do_switch;
        }
    }

    return;  // nothing to switch to

do_switch:
    next->counter = next->priority > 0 ? next->priority : 1;
    this_cpu()->need_resched = 0;
    switch_to(current, next);
}

// ── Task exit ──────────────────────────────────────────────
// do_exit() frees user page tables and physical pages, then
// marks the task ZOMBIE. thread_t and task_union are freed
// later by the zombie reaper in schedule() (deferred because
// __switch_to dereferences current->thread, and we're running
// on the kernel stack inside task_union).
//
uint64_t do_exit(uint64_t exit_code)
{
    serial_printk("task %d exiting with code %#018lx\n", current->pid, exit_code);

    // ── Init process protection ──────────────────────────
    // The user-space init process (PID 1) must never exit.
    // Check by pid rather than pointer — after fork, child
    // task structs are copies and pointer comparison fails.
    if (current->pid == user_init_pid) {
        serial_printk("PANIC: init (pid=%d) attempted to exit with code %#018lx\n",
                      (int)current->pid, exit_code);
        while (1) { __asm__ __volatile__("hlt"); }
    }

    // ── Reparent children to init ────────────────────────
    if (user_init_task && current->pid != user_init_pid) {
        list_t *cpos = init_task_union.task.list.next;
        while (cpos != &init_task_union.task.list) {
            task_t *child = container_of(cpos, task_t, list);
            cpos = cpos->next;
            if (child->parent == current) {
                child->parent = user_init_task;
                serial_printk("reparent: child %d → init (pid=%d)\n",
                              (int)child->pid, (int)user_init_task->pid);
            }
        }
    }

    // ── Send SIGCHLD to parent ───────────────────────────
    // NOTE: we write-protect parent->state here because we are
    // about to become ZOMBIE.  After this point, the parent may
    // run and reap us via do_waitpid → PF_REAPED.  The scheduler's
    // zombie reaper will later list_del + kfree.
    if (current->parent && !(current->parent->flags & PF_KTHREAD)) {
        current->parent->signal |= (1ULL << SIGCHLD);
        if (current->parent->state == TASK_INTERRUPTIBLE)
            current->parent->state = TASK_RUNNING;
    }

    if (!(current->flags & PF_KTHREAD) && current->mm) {
        // Skip vmm_free_user_map for now — in do_fork the child
        // shares pml4 with the parent, so freeing would corrupt
        // the parent's address space.  Will be fixed with mm refcounting.
        kfree(current->mm);
        current->mm = NULL;
    }

    // Close all file descriptors
    if (current->files) {
        files_free(current->files);
        current->files = NULL;
    }

    current->exit_code = exit_code;
    current->state = TASK_ZOMBIE;

    // ── Direct switch to parent ─────────────────────────────
    // By the time we reach ZOMBIE the parent is either already
    // RUNNING (SIGCHLD woke it) or still INTERRUPTIBLE in
    // do_waitpid.  In either case we want to switch directly to
    // avoid schedule()'s round-robin scan which may pick the
    // idle task instead (task list order changes after zombie
    // reaping inside schedule() can cause this).
    task_t *parent = current->parent;
    int parent_woken = 0;

    if (parent) {
        uint64_t ps = parent->state;
        if (ps == TASK_INTERRUPTIBLE) {
            parent->state = TASK_RUNNING;
            parent_woken = 1;
        } else if (ps == TASK_RUNNING) {
            // SIGCHLD already woke the parent.
            parent_woken = 1;
        } else if (ps == TASK_UNINTERRUPTIBLE) {
            // Parent is in an unkillable sleep — unlikely for
            // waitpid but handle gracefully: leave it, the
            // scheduler's zombie reaper will eventually clean
            // us up if parent never comes back.
            serial_printk("exit: p%d parent p%d UNINTERRUPTIBLE (%ld), "
                          "skipping direct switch\n",
                          current->pid, parent->pid, ps);
        }
    }

    serial_printk("task %d now ZOMBIE (parent=%d w=%d ps=%ld)\n",
                  current->pid,
                  parent ? (int)parent->pid : -1,
                  parent_woken,
                  parent ? (long)parent->state : -1);

    // Transfer directly to the woken parent to avoid scheduler
    // scan races.  parent_woken is always 1 for normal exit
    // (parent in waitpid = INTERRUPTIBLE or RUNNING).
    if (parent_woken) {
        serial_printk("exit: p%d→p%d direct\n", current->pid, parent->pid);
        switch_to(current, parent);
        // unreachable — switch_to never returns
    }
    schedule();
    return 0;  // never reached — schedule() does switch_to
}

// ── do_waitpid ────────────────────────────────────────────
// Block until a child with <pid> exits, or return immediately
// if WNOHANG is set and no child is ready.
//
// pid > 0  → wait for specific child
// pid == -1 → wait for any child
// Returns child PID on success, -ECHILD if no such child, -EINTR if interrupted.
int64_t do_waitpid(int64_t pid, int *user_status, int options)
{
    for (;;) {
        task_t *child = NULL;
        list_t *pos;

        // Pass 1: scan for a matching, unreaped ZOMBIE child
        pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            if (!(t->flags & PF_REAPED) &&
                t->parent == current &&
                t->state == TASK_ZOMBIE &&
                (pid == -1 || t->pid == pid)) {
                child = t;
                break;
            }
            pos = pos->next;
        }

        if (child) {
            int64_t child_pid = child->pid;
            int64_t exit_code = child->exit_code;

            if (user_status) {
                int status = (int)((exit_code & 0xFF) << 8);
                if ((uint64_t)user_status < current->addr_limit)
                    *user_status = status;
            }
            // Mark reaped — schedule()'s zombie reaper will list_del+kfree.
            // We MUST NOT touch child->list here: on SMP the child may still
            // be running schedule() → container_of(current->list.next, …),
            // and list_del/list_add_to_before would corrupt that.
            __sync_fetch_and_or(&child->flags, PF_REAPED);

            serial_printk("waitpid: pid=%d reaped child %d (exit=%d)\n",
                          (int)current->pid, (int)child_pid, (int)exit_code);
            return child_pid;
        }

        serial_printk("waitpid: pid=%d search for child pid=%d\n",
                      current ? (int)current->pid : -1, (int)pid);
        // Check if the child even exists (unreaped, still running)
        int child_exists = 0;
        int child_count = 0;
        serial_printk("waitpid: child-list cur=%d pid=%lld:", current->pid, (long long)pid);
        pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            if (t->parent == current && !(t->flags & PF_REAPED)) {
                child_count++;
                serial_printk(" [%d s=%d f=%lx p=%lx]",
                    t->pid, t->state, t->flags, (unsigned long)t->parent);
                if (pid == -1 || t->pid == pid)
                    child_exists = 1;
            }
            pos = pos->next;
        }
        serial_printk(" count=%d exist=%d\n", child_count, child_exists);

        if (!child_exists) {
            serial_printk("waitpid: pid=%d returning ECHILD (no child pid=%lld)\n",
                          current ? (int)current->pid : -1, (long long)pid);
            return -ECHILD;
        }

        serial_printk("do_waitpid: chk wnohang pid=%d opt=%d\n", current->pid, options); if (options & WNOHANG)
            return 0;

        // Sleep until a child exits.
        // Double-check after setting INTERRUPTIBLE to prevent lost-wakeup.
        serial_printk("do_waitpid: pid=%d about to sleep options=%d\n", current->pid, options);

        pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            if (!(t->flags & PF_REAPED) &&
                t->parent == current &&
                t->state == TASK_ZOMBIE &&
                (pid == -1 || t->pid == pid)) {
                child = t;
                break;
            }
            pos = pos->next;
        }

        if (child) {
            current->state = TASK_RUNNING;
            continue;
        }

        // Sleep until a child exits or a signal arrives.
        // do_exit() sets parent->state = TASK_RUNNING when the child
        // becomes ZOMBIE.
        current->state = TASK_INTERRUPTIBLE;
        schedule();
        current->state = TASK_RUNNING;
        serial_printk("wait: p%d woke\n", current->pid);
    }
}

extern void kernel_thread_func(void);
__asm__(
    "kernel_thread_func:\n\t"
    "   sti             \n\t"  // re-enable IRQs after first context switch
    "   popq %r15   \n\t"
    "   popq %r14   \n\t"
    "   popq %r13   \n\t"
    "   popq %r12   \n\t"
    "   popq %r11   \n\t"
    "   popq %r10   \n\t"
    "   popq %r9    \n\t"
    "   popq %r8    \n\t"
    "   popq %rbx   \n\t"  // fn pointer
    "   popq %rcx   \n\t"
    "   popq %rdx   \n\t"  // arg
    "   popq %rsi   \n\t"
    "   popq %rdi   \n\t"
    "   popq %rbp   \n\t"
    "   popq %rax   \n\t"  // skip DS slot
    "   popq %rax   \n\t"  // skip ES slot
    "   popq %rax   \n\t"  // original RAX
    "   pushq %rax   \n\t" // save RAX temporarily
    "   movq $0x10, %rax \n\t"
    "   movq %rax, %ds \n\t"
    "   movq %rax, %es \n\t"
    "   movq %rax, %fs \n\t"
    // GS is NOT reloaded — its base is per-CPU, set via
    // IA32_GS_BASE MSR.  Loading a selector would clobber
    // the per-CPU base with the GDT flat descriptor value.
    "   popq %rax    \n\t" // restore RAX
    "   addq $0x38, %rsp \n\t"
    "   movq %rdx, %rdi \n\t"  // arg
    "   callq *%rbx \n\t"      // call fn(arg)
    "   movq %rax, %rdi \n\t"
    "   callq do_exit \n\t"
);

#define USER_CODE_ADDR   0x400000UL

// ── spawn_user_task(path) ──────────────────────────────────
// Loads an ELF from the filesystem, creates a new user task,
// and adds it to the scheduler. Returns the new task's PID or -1 on error.
int64_t spawn_user_task(const char *path, const char *const *argv)
{
    // 1. Open the ELF file via VFS
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        serial_printk("spawn: cannot open '%s'\n", path);
        return -1;
    }
    if (node->type != VFS_FILE) {
        serial_printk("spawn: '%s' is not a file\n", path);
        vfs_node_put(node);
        return -1;
    }

    // 2. Quick ELF validation
    if (elf_validate(node) != 0) {
        serial_printk("spawn: '%s' is not a valid ELF\n", path);
        vfs_node_put(node);
        return -1;
    }

    // 3. Allocate task structures (pattern mirrors old user_task_create)
    void *raw_alloc = malloc(sizeof(union task_union) + STACK_SIZE);
    task_t *tsk = (task_t *)(((uint64_t)raw_alloc + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)calloc(1, sizeof(thread_t));
    mm_t *mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (!raw_alloc || !thd || !mm) {
        if (raw_alloc) kfree(raw_alloc);
        if (thd) kfree(thd);
        if (mm) kfree(mm);
        vfs_node_put(node);
        return -1;
    }

    memset(tsk, 0, sizeof(task_t));
    tsk->stack_alloc_base = raw_alloc;

    tsk->state = TASK_UNINTERRUPTIBLE;
    tsk->flags = 0;                        // NOT PF_KTHREAD → user task
    tsk->addr_limit = 0x00007FFFFFFFFFFF;
    tsk->pid = atomic_fetch_add((volatile uint64_t *)&pid_counter, 1);
    tsk->counter = 1;
    tsk->signal = 0;
    tsk->priority = 5;                     // 50 ms quantum at 100 Hz
    tsk->cpu = cpu_id();                    // created on this CPU

    // Inherit fd table from parent (the init task)
    tsk->parent = current;
    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    tsk->exit_code = 0;
    if (current->files)
        tsk->files = files_dup(current->files);

    list_init(&tsk->list);
    list_add_to_before(&init_task_union.task.list, &tsk->list);
    tsk->thread = thd;

    // 4. Create per-process page table
    uint64_t *user_pml4 = (uint64_t *)vmm_alloc_map();  // 4KB zeroed PML4
    if (!user_pml4) {
        kfree(raw_alloc); kfree(thd); kfree(mm);
        vfs_node_put(node);
        return -1;
    }
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);
    memcpy(&user_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)user_pml4);
    tsk->mm = mm;
    thd->cr3 = (uint64_t)mm->pml4;

    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, mm, &entry_point) != 0) {
        serial_printk("spawn: ELF load failed for '%s'\n", path);
        vmm_free_user_map(user_pml4);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        vfs_node_put(node);
        return -1;
    }
    vfs_node_put(node);

    // Set heap just after the loaded ELF segments
    mm->start_brk = PAGE_4K_ALIGN(mm->end_code);
    mm->end_brk   = mm->start_brk;

    // 6. Map the user stack page (separate 2MB page at 0x600000)
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        vmm_free_user_map(user_pml4);
        kfree(mm); kfree(thd); kfree(raw_alloc);
        return -1;
    }
    vmm_map_page(user_pml4, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_Page | PAGE_XD);
    mm->start_stack = USER_STACK_BASE;

    // ââ 6.5 Set up argv on user stack âââââââââââââââ
    int s_argc = 0;
    uint64_t user_rsp = USER_STACK_TOP;
    uint64_t user_arg_ptr = 0;

    if (argv != NULL) {
        while (argv[s_argc] != NULL) s_argc++;
        char *kstack = (char *)Phy_To_Virt(stack_page->phy_address);
#define KSTACK(va) (kstack + ((va) - USER_STACK_BASE))
        uint64_t str_offset[128];
        for (int i = 0; i < s_argc; i++) {
            size_t len = strlen(argv[i]) + 1;
            user_rsp -= len;
            memcpy(KSTACK(user_rsp), argv[i], len);
            str_offset[i] = user_rsp;
        }
        user_rsp &= ~15ULL;

        // ── Calculate aligned metadata size ───────────────────
        // Layout from bottom (RSP) up: argc | argv[]+NULL |
        // envp_NULL | auxv AT_NULL.  Total must be 16-byte
        // aligned so RSP (pointing to argc) & 0xF == 0.
        // Without padding, total = 24 + (s_argc+1)*8 bytes.
        // When s_argc is even, we need 8 extra bytes.
        int meta_pad = (s_argc & 1) ? 0 : 8;

        // ── auxv: AT_NULL terminator ──────────────────────────
        user_rsp -= 16;
        *(uint64_t *)KSTACK(user_rsp) = 0;      // AT_NULL type
        *(uint64_t *)KSTACK(user_rsp + 8) = 0;  // value

        // ── envp end NULL + optional alignment padding ────────
        user_rsp -= 8 + meta_pad;
        *(uint64_t *)KSTACK(user_rsp) = 0;      // NULL (end of envp)

        // ── argv[] array (NULL-terminated) ────────────────────
        user_rsp -= (s_argc + 1) * 8;
        user_arg_ptr = user_rsp;
        for (int i = 0; i < s_argc; i++)
            *(uint64_t *)KSTACK(user_rsp + i * 8) = str_offset[i];
        *(uint64_t *)KSTACK(user_rsp + s_argc * 8) = 0;  // NULL terminator

        // ── argc ──────────────────────────────────────────────
        user_rsp -= 8;
        *(uint64_t *)KSTACK(user_rsp) = (uint64_t)s_argc;
#undef KSTACK
    }

    // 7. Set up pt_regs for iretq to ring 3
    pt_regs_t *regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
    memset(regs, 0, sizeof(pt_regs_t));
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = (argv != NULL) ? user_rsp : USER_STACK_TOP;
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1
    regs->rdi     = (uint64_t)s_argc;
    regs->rsi     = user_arg_ptr;
    regs->rdx     = 0;                     // envp = NULL

    // 8. Thread context for switch_to / __switch_to
    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;
    thd->rip  = (uint64_t)ret_from_intr;   // first entry via RESTORE_ALL → iretq

    tsk->state = TASK_RUNNING;

    // The first user task we create is "init" — track it globally.
    if (!user_init_task) {
        user_init_task = tsk;
        user_init_pid  = tsk->pid;
    }

    serial_printk("spawn: pid=%d '%s' entry=%p rsp=%p cr3=%p\n",
                  tsk->pid, path, entry_point, regs->rsp, thd->cr3);

    return tsk->pid;
}

// ── sys_exec(path, regs, argv, envp) ────────────────────────
// Replaces the current process image with a new ELF loaded from
// the filesystem. Called from do_system_call (SYS_exec).
// regs is the pt_regs frame on the kernel stack that will be
// restored by RESTORE_ALL → iretq.
//
// If argv == NULL: old behavior (no args, rsp=USER_STACK_TOP,
// argc=0, argv=NULL, envp=NULL).
// If argv != NULL: copies argv/envp strings onto the user stack
// and sets up the standard ABI stack layout so the child's
// _start receives argc in %rdi, argv in %rsi, envp in %rdx.
int64_t sys_exec(const char *path, pt_regs_t *regs,
                 const char *const *argv, const char *const *envp)
{
    serial_printk("sys_exec: pid=%d path=%s argv=%p\n", current->pid, path ? path : "(null)", (void*)argv);
    // 1. Look up the ELF file (support relative paths)
    const char *cwd = current->files ? current->files->cwd : "/";
    vfs_node_t *node = vfs_lookup_from(path, cwd);
    if (!node)
        return -ENOENT;
    if (node->type != VFS_FILE) {
        vfs_node_put(node);
        return -EACCES;
    }

    // 2. Quick ELF validation (elf_load does full validation)
    if (elf_validate(node) != 0) {
        vfs_node_put(node);
        return -ENOEXEC;
    }

    // 3. Create a fresh page table for the new process image
    uint64_t *new_pml4 = (uint64_t *)vmm_alloc_map();
    if (!new_pml4) {
        vfs_node_put(node);
        return -ENOMEM;
    }
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);
    memcpy(&new_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    // 4. Create new mm_struct
    mm_t *new_mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (!new_mm) {
        kfree(new_pml4);
        vfs_node_put(node);
        return -ENOMEM;
    }
    new_mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)new_pml4);
    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, new_mm, &entry_point) != 0) {
        vmm_free_user_map(new_pml4);
        kfree(new_mm);
        vfs_node_put(node);
        return -ENOEXEC;
    }
    vfs_node_put(node);

    // Set heap just after the loaded ELF segments
    new_mm->start_brk = PAGE_4K_ALIGN(new_mm->end_code);
    new_mm->end_brk   = new_mm->start_brk;

    // 6. Map the user stack page
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        vmm_free_user_map(new_pml4);
        kfree(new_mm);
        return -ENOMEM;
    }
    vmm_map_page(new_pml4, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_Page | PAGE_XD);
    new_mm->start_stack = USER_STACK_BASE;

    // ── 6.5 Set up argv/envp on user stack ──────────────────
    int s_argc = 0;
    int s_envc = 0;
    uint64_t user_rsp = USER_STACK_TOP;
    uint64_t user_arg_ptr = 0;   // rsi value
    uint64_t user_env_ptr = 0;   // rdx value

    if (argv != NULL) {
        // Count argv
        while (argv[s_argc] != NULL) s_argc++;

        // Count envp
        if (envp != NULL) {
            while (envp[s_envc] != NULL) s_envc++;
        }

        // Access the stack page through kernel mapping
        char *kstack = (char *)Phy_To_Virt(stack_page->phy_address);
        // kstack[0..0x1FFFFF] maps to USER_STACK_BASE..USER_STACK_TOP
        #define KSTACK(va) (kstack + ((va) - USER_STACK_BASE))

        // ── Copy string data to top of stack ──────────────────
        // str_offsets[i] records the virtual address of each string on stack
        uint64_t str_offset[128];  // enough for ~32 argv + envp each
        int si = 0;

        // Copy argv strings
        for (int i = 0; i < s_argc; i++) {
            size_t len = strlen(argv[i]) + 1;
            user_rsp -= len;
            memcpy(KSTACK(user_rsp), argv[i], len);
            str_offset[si++] = user_rsp;
        }

        // Copy envp strings
        for (int i = 0; i < s_envc; i++) {
            size_t len = strlen(envp[i]) + 1;
            user_rsp -= len;
            memcpy(KSTACK(user_rsp), envp[i], len);
            str_offset[si++] = user_rsp;
        }

        // ── 16-byte align ─────────────────────────────────────
        user_rsp &= ~15ULL;

        // ── Calculate aligned metadata size ───────────────────
        // Layout from bottom (RSP) up: argc | argv[]+NULL |
        // envp[]+NULL | auxv AT_NULL.  Total must be 16-byte
        // aligned so RSP (pointing to argc) & 0xF == 0.
        // Without padding, total = 24 + (argc+1)*8 + (envc+1)*8.
        // When (s_argc + s_envc) is even, we need 8 extra bytes.
        int meta_pad = ((s_argc + s_envc) & 1) ? 0 : 8;

        // ── auxv: just AT_NULL terminator ─────────────────────
        user_rsp -= 16;  // {AT_NULL=0, 0}
        *(uint64_t *)KSTACK(user_rsp) = 0;      // AT_NULL
        *(uint64_t *)KSTACK(user_rsp + 8) = 0;

        // ── envp[] array (NULL-terminated) + alignment pad ────
        user_rsp -= (s_envc + 1) * 8 + meta_pad;
        user_env_ptr = user_rsp;
        for (int i = 0; i < s_envc; i++) {
            *(uint64_t *)KSTACK(user_rsp + i * 8) = str_offset[s_argc + i];
        }
        *(uint64_t *)KSTACK(user_rsp + s_envc * 8) = 0;  // NULL terminator

        // ── argv[] array (NULL-terminated) ────────────────────
        user_rsp -= (s_argc + 1) * 8;
        user_arg_ptr = user_rsp;
        for (int i = 0; i < s_argc; i++) {
            *(uint64_t *)KSTACK(user_rsp + i * 8) = str_offset[i];
        }
        *(uint64_t *)KSTACK(user_rsp + s_argc * 8) = 0;  // NULL terminator

        // ── argc ──────────────────────────────────────────────
        user_rsp -= 8;
        *(uint64_t *)KSTACK(user_rsp) = (uint64_t)s_argc;

        #undef KSTACK
    }

    // 7. Free the OLD user address space (mm_t only, not page tables).
    // The old mm may share page tables with the parent (fork), so we
    // must NOT call vmm_free_user_map here — it would destroy the
    // parent's address space.  When the parent exits, do_exit handles
    // the shared page table cleanup.
    if (current->mm) {
        kfree(current->mm);
        current->mm = NULL;
    }

    // 7.5 POSIX: exec() resets caught signal handlers to SIG_DFL.
    // SIG_IGN is supposed to survive exec, but shells (ash) set
    // SIGINT to SIG_IGN to protect themselves from Ctrl-C.  A
    // forked child inherits SIG_IGN, and with POSIX-correct exec
    // SIG_IGN survives — leaving the new process immune to Ctrl-C.
    //
    // Reset ALL handlers to SIG_DFL unconditionally.  This is what
    // Linux does for several signals (SIGCHLD is always reset on
    // exec, even if SIG_IGN), and it's the pragmatic fix for the
    // shell-child-signal-inheritance problem.
    for (int sig = 1; sig < NSIG; sig++)
        current->sighand[sig].sa_handler = SIG_DFL;

    // 8. Install new mm and page table
    current->mm = new_mm;
    current->thread->cr3 = (uint64_t)new_mm->pml4;

    // 9. Switch CR3 to the new page table
    __asm__ __volatile__("movq %0, %%cr3" :: "r"(current->thread->cr3) : "memory");

    // 10. Overwrite pt_regs for RESTORE_ALL → iretq to the new process
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = (argv != NULL) ? user_rsp : USER_STACK_TOP;
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1
    regs->rdi     = (uint64_t)s_argc;      // argc
    regs->rsi     = user_arg_ptr;          // argv
    regs->rdx     = user_env_ptr;          // envp

    serial_printk("exec: pid=%d entry=%p rsp=%p argc=%d cr3=%p\n",
                  current->pid, entry_point, regs->rsp, s_argc, current->thread->cr3);

    return 0;
}

// ── fork_mm_copy — create private address space for fork child ─
// Builds a new PML4 with a private copy of every user 2MB page.
// This is a full eager copy (like a COW fault on every page).
// Without COW, parent and child would corrupt each other's
// stack/heap when either writes to a shared page.
// Returns new mm_t on success, or NULL on OOM (caller falls back
// to sharing and risk of corruption).
static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)
{
    mm_t *child_mm = (mm_t *)calloc(1, sizeof(mm_t));
    uint64_t *child_pml4 = (uint64_t *)vmm_alloc_map();
    if (!child_mm || !child_pml4)
        goto fail;

    uint64_t *parent_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)parent_mm->pml4);
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);

    memcpy(&child_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    for (int l4 = 0; l4 < 256; l4++) {
        uint64_t pml4e = parent_pml4[l4];
        if (!(pml4e & PAGE_Present)) continue;

        uint64_t *parent_pml3 = (uint64_t *)Phy_To_Virt(pml4e & PAGE_4K_MASK);
        uint64_t *child_pml3  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
        if (!child_pml3) continue;
        child_pml4[l4] = Virt_To_Phy((uint64_t)child_pml3) | PAGE_USER_GDT;

        for (int l3 = 0; l3 < 512; l3++) {
            uint64_t pml3e = parent_pml3[l3];
            if (!(pml3e & PAGE_Present)) continue;

            uint64_t *parent_pml2 = (uint64_t *)Phy_To_Virt(pml3e & PAGE_4K_MASK);
            uint64_t *child_pml2  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
            if (!child_pml2) continue;
            child_pml3[l3] = Virt_To_Phy((uint64_t)child_pml2) | PAGE_USER_Dir;

            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t pml2e = parent_pml2[l2];
                if (!(pml2e & PAGE_Present)) continue;

                uint64_t phys = pml2e & PAGE_2M_MASK;

                // Eager copy — every user page gets a private clone.
                struct Page *s = alloc_pages(ZONE_NORMAL, 1, 0);
                if (s) {
                    memcpy((void *)Phy_To_Virt(s->phy_address),
                           (void *)Phy_To_Virt(phys & ~PAGE_XD),
                           PAGE_2M_SIZE);
                    child_pml2[l2] = s->phy_address
                                   | (pml2e & ~PAGE_2M_MASK);
                } else {
                    child_pml2[l2] = pml2e; // OOM fallback
                }
            }
        }
    }

    memcpy(child_mm, parent_mm, sizeof(mm_t));
    child_mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)child_pml4);
    *cr3_out = (uint64_t)child_mm->pml4;
    return child_mm;

fail:
    if (child_pml4) kfree(child_pml4);
    if (child_mm)   kfree(child_mm);
    if (cr3_out)    *cr3_out = 0;
    return NULL;
}

// ── do_fork ──────────────────────────────────────────────
uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags,
                 uint64_t stack_start __attribute__((unused)),
                 uint64_t stack_size __attribute__((unused)))
{
    void *raw_alloc = malloc(sizeof(union task_union) + STACK_SIZE);
    task_t *tsk = (task_t *)(((uint64_t)raw_alloc + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)malloc(sizeof(thread_t));

    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));

    tsk->stack_alloc_base = raw_alloc;
    *tsk = *current;
    tsk->signal = 0;   // child must not inherit parent's pending signals

    list_init(&tsk->list);
    list_add_to_before(&init_task_union.task.list, &tsk->list);
    tsk->pid = atomic_fetch_add((volatile uint64_t *)&pid_counter, 1);
    tsk->state = TASK_UNINTERRUPTIBLE;
    tsk->priority = 3;
    tsk->cpu = cpu_id();                    // same CPU as parent
    tsk->thread = thd;

    // ── Inherit fd table (deep copy — refcount++) ──────────
    if (current->files)
        tsk->files = files_dup(current->files);

    // ── Process tree ───────────────────────────────────────
    tsk->parent = current;
    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    tsk->exit_code = 0;

    thd->cr3 = current->thread->cr3;

    if ((regs->cs & 3) == 3) {
        tsk->flags &= ~PF_KTHREAD;
        tsk->addr_limit = 0x00007FFFFFFFFFFF;
        if (current->mm)
            tsk->mm = fork_mm_copy(current->mm, &thd->cr3);
    }

    memcpy((void *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t)),
           regs, sizeof(pt_regs_t));

    // Child process sees fork() return 0
    {
        pt_regs_t *child_regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
        child_regs->rax = 0;
    }

    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;

    thd->rip = regs->rip;
    if (!(tsk->flags & PF_KTHREAD))
        thd->rip = (uint64_t)ret_from_intr;  // child via softirq/preemption check
    // Parent: do NOT change regs->rip — returns via error_code → RESTORE_ALL

    tsk->state = TASK_RUNNING;
    return tsk->pid;   // parent sees child PID
}

int kernel_thread(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t flags)
{
    pt_regs_t regs;
    memset(&regs, 0, sizeof(pt_regs_t));

    regs.rbx = (uint64_t)fn;
    regs.rdx = (uint64_t)arg;

    regs.ds = KERNEL_DS;
    regs.es = KERNEL_DS;
    regs.cs = KERNEL_CS;
    regs.ss = KERNEL_DS;
    regs.rflags = (1 << 9);
    regs.rip = (uint64_t)kernel_thread_func;

    return do_fork(&regs, flags, 0, 0);
}

void task_init()
{

    init_mm.pml4 = get_cr3();
    init_thread.cr3 = (uint64_t)init_mm.pml4;

    init_mm.start_code = PMMngr.start_code;
    init_mm.end_code = PMMngr.end_code;
    init_mm.start_data = (uint64_t)&_data;
    init_mm.end_data = PMMngr.end_data;
    init_mm.start_rodata = (uint64_t)&_rodata;
    init_mm.end_rodata = (uint64_t)&_erodata;
    init_mm.start_brk = 0;
    init_mm.end_brk = PMMngr.start_brk;
    init_mm.start_stack = _stack_start;

    set_tss64(init_thread.rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    init_tss[0].rsp0 = init_thread.rsp0;
    list_init(&init_task_union.task.list);

    // BSP idle task pointer (for the multicore scheduler).
    percpu_data[0].idle = &init_task_union.task;
    init_task_union.task.cpu = 0;

    // ── Set up fd 0/1/2 on the idle task ─────────────────
    // These will be inherited by the first user task (init.elf).
    {
        files_t *files = files_alloc();
        if (files) {
            current->files = files;  // attach to idle task

            // All three fds go through /dev/tty:
            //   read  → keyboard (ASCII-translated scancodes)
            //   write → framebuffer (GTK window) + serial (terminal)
            vfs_node_t *tty = vfs_lookup("/dev/tty");
            if (tty) {
                file_t *f0 = file_alloc();
                if (f0) { f0->type = FD_DEV; f0->node = tty; f0->flags = O_RDWR;  fd_alloc(files, f0); }

                file_t *f1 = file_alloc();
                if (f1) { f1->type = FD_DEV; f1->node = tty; f1->flags = O_WRONLY; fd_alloc(files, f1); }

                file_t *f2 = file_alloc();
                if (f2) { f2->type = FD_DEV; f2->node = tty; f2->flags = O_WRONLY; fd_alloc(files, f2); }
            }
        }
    }

    // pid_counter starts at 1 → first user task becomes PID=1.
    int64_t init_pid = spawn_user_task("/init.elf", NULL);
    serial_printk("init: spawned user-space init, pid=%d\n", (int)init_pid);

    // Activate the scheduler and enter the idle loop.
    // schedule() picks up the user init (PID 1) naturally.
    current->state = TASK_RUNNING;
    this_cpu()->scheduler_ok = 1;

    // ── Idle loop ────────────────────────────────────────────
    // hlt pauses the CPU until the next interrupt (timer tick,
    // keyboard IRQ1, serial IRQ4).  The timer ISR sets
    // need_resched; ret_from_intr calls schedule() before iretq.
    // If a task was woken we switch to it; otherwise we loop
    // back to hlt.
    //
    // serial_poll() (IRQ fallback) moved to pit_handler — runs
    // at 100 Hz on every timer tick, so the serial input path
    // still has a fallback even if IOAPIC routing fails.
    while (1) {
        __asm__ __volatile__("hlt");
        if (this_cpu()->need_resched)
            schedule();
    }
}
