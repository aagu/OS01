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
#include <driver/serial.h>

uint64_t init(uint64_t arg);

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

// Per-CPU scheduler guard — set to 1 by task_init() on each CPU.
// schedule() returns immediately before this point (ticks before
// the scheduler is set up are harmless no-ops).

void schedule(void)
{
    task_t *next;

    if (!this_cpu()->scheduler_ok)
        return;

    this_cpu()->schedule_count++;

    // ── Reap zombies that nobody will wait for ──
    // Protected by a global spinlock because both CPUs can
    // enter schedule() concurrently and would otherwise
    // double-free the same zombie tasks.
    {
        static spinlock_T reap_lock = { .lock = 1L };
        spin_lock(&reap_lock);

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
            if (t->flags & PF_KTHREAD) {
                reap = 1;
            } else if (t->parent == NULL) {
                reap = 1;
            } else {
                // Check if parent is also ZOMBIE without dangling risk:
                // (a) parent is still on the task list (not yet freed)
                // (b) parent->state == TASK_ZOMBIE
                // If parent freed earlier, the iteration above would have
                // orphaned t by now (t->parent = NULL).  We haven't freed
                // any parent this round yet, so parent is safe to deref.
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

    // Pass 3: free the zombies
    for (int i = 0; i < reap_count; i++) {
        task_t *t = reap_list[i];
        list_del(&t->list);
        if (t->thread)
            kfree(t->thread);
        if (t->files)
            files_free(t->files);
        if (t->stack_alloc_base)
            kfree(t->stack_alloc_base);
    }

    spin_unlock(&reap_lock);
    }

    // Decrement current task's quantum (one tick per call)
    if (current->counter > 0)
        current->counter--;

    // If current still has quantum, keep running
    if (current->state == TASK_RUNNING && current->counter > 0) {
        this_cpu()->need_resched = 0;
        return;
    }

    // Round-robin: scan forward from current for a RUNNING task
    next = container_of(current->list.next, task_t, list);
    while (next != &init_task_union.task && next != current) {
        if (next->state == TASK_RUNNING && next->cpu == this_cpu()->cpu_id)
            goto do_switch;
        next = container_of(next->list.next, task_t, list);
    }

    // Wrap past the list head and try the rest
    if (next == &init_task_union.task) {
        next = container_of(next->list.next, task_t, list);
        while (next != &init_task_union.task && next != current) {
            if (next->state == TASK_RUNNING && next->cpu == this_cpu()->cpu_id)
                goto do_switch;
            next = container_of(next->list.next, task_t, list);
        }
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

    if (!(current->flags & PF_KTHREAD) && current->mm) {
        // Free user page tables and user physical pages
        vmm_free_user_map((mmap)Phy_To_Virt((uint64_t)current->mm->pml4));
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

    // Wake up the parent if it's waiting in waitpid
    if (current->parent && current->parent->state == TASK_INTERRUPTIBLE)
        current->parent->state = TASK_RUNNING;

    serial_printk("task %d now ZOMBIE (parent=%d)\n",
                  current->pid, current->parent ? (int)current->parent->pid : -1);

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
    // Note: caller has already copied user_status from user space.
    // We pass NULL if the user didn't provide a status pointer.

    for (;;) {
        task_t *child = NULL;
        list_t *pos;

        // Scan for a matching child that is ZOMBIE
        pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            if (t->parent == current &&
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

            // Write exit status to user space
            if (user_status) {
                // xv6 convention: exit_code << 8
                int status = (int)((exit_code & 0xFF) << 8);
                // Bounds check — user_status must be in user space
                if ((uint64_t)user_status < current->addr_limit)
                    *user_status = status;
            }

            // Reap the zombie
            list_del(&child->list);
            if (child->thread)
                kfree(child->thread);
            if (child->files)
                files_free(child->files);
            if (child->stack_alloc_base)
                kfree(child->stack_alloc_base);

            serial_printk("waitpid: pid=%d reaped child %d (exit=%d)\n",
                          (int)current->pid, (int)child_pid, (int)exit_code);
            return child_pid;
        }

        // Check if the child even exists (still running)
        int child_exists = 0;
        pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            if (t->parent == current && (pid == -1 || t->pid == pid)) {
                child_exists = 1;
                break;
            }
            pos = pos->next;
        }

        if (!child_exists)
            return -ECHILD;

        // WNOHANG: return 0 immediately
        if (options & WNOHANG)
            return 0;

        // Sleep until a child exits
        current->state = TASK_INTERRUPTIBLE;
        schedule();
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
int64_t spawn_user_task(const char *path)
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
    thread_t *thd = (thread_t *)calloc(sizeof(thread_t));
    mm_t *mm = (mm_t *)calloc(sizeof(mm_t));
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

    // Heap starts just after code within the 2MB page at 0x400000
    mm->start_brk = USER_CODE_ADDR + 0x1000;
    mm->end_brk   = mm->start_brk;

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

    // 6. Map the user stack page (separate 2MB page at 0x600000)
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        vmm_free_user_map(user_pml4);
        kfree(mm); kfree(thd); kfree(raw_alloc);
        return -1;
    }
    vmm_map_page(user_pml4, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_Page);
    mm->start_stack = USER_STACK_BASE;

    // 7. Set up pt_regs for iretq to ring 3
    pt_regs_t *regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
    memset(regs, 0, sizeof(pt_regs_t));
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = USER_STACK_TOP;
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1

    // 8. Thread context for switch_to / __switch_to
    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;
    thd->rip  = (uint64_t)ret_from_intr;   // first entry via RESTORE_ALL → iretq

    tsk->state = TASK_RUNNING;

    serial_printk("spawn: pid=%d '%s' entry=%p rsp=%p cr3=%p\n",
                  tsk->pid, path, entry_point, regs->rsp, thd->cr3);

    return tsk->pid;
}

// ── sys_exec(path, regs) ───────────────────────────────────
// Replaces the current process image with a new ELF loaded from
// the filesystem. Called from do_system_call (SYS_exec).
// regs is the pt_regs frame on the kernel stack that will be
// restored by RESTORE_ALL → iretq.
int64_t sys_exec(const char *path, pt_regs_t *regs)
{
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
    mm_t *new_mm = (mm_t *)calloc(sizeof(mm_t));
    if (!new_mm) {
        kfree(new_pml4);
        vfs_node_put(node);
        return -ENOMEM;
    }
    new_mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)new_pml4);
    new_mm->start_brk = USER_CODE_ADDR + 0x1000;
    new_mm->end_brk   = new_mm->start_brk;

    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, new_mm, &entry_point) != 0) {
        vmm_free_user_map(new_pml4);
        kfree(new_mm);
        vfs_node_put(node);
        return -ENOEXEC;
    }
    vfs_node_put(node);

    // 6. Map the user stack page
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        vmm_free_user_map(new_pml4);
        kfree(new_mm);
        return -ENOMEM;
    }
    vmm_map_page(new_pml4, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_Page);
    new_mm->start_stack = USER_STACK_BASE;

    // 7. Free the OLD user address space (but keep current's mm + PML4
    //    alive for this function's remaining execution — we switch CR3
    //    after freeing so all kernel code/data stays mapped via the
    //    shared high-half kernel entries 256—511).
    if (current->mm) {
        vmm_free_user_map((mmap)Phy_To_Virt((uint64_t)current->mm->pml4));
        kfree(current->mm);
    }

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
    regs->rsp     = USER_STACK_TOP;
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1

    serial_printk("exec: pid=%d entry=%p rsp=%p cr3=%p\n",
                  current->pid, entry_point, regs->rsp, current->thread->cr3);

    return 0;
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
    tsk->exit_code = 0;

    thd->cr3 = current->thread->cr3;

    if ((regs->cs & 3) == 3) {
        tsk->flags &= ~PF_KTHREAD;
        tsk->addr_limit = 0x00007FFFFFFFFFFF;

        // Child must NOT share the parent's mm — exec would
        // free the parent's user page tables.  Give the child
        // a NULL mm; sys_exec will skip the old-user-map free
        // and allocate a fresh address space.
        tsk->mm = NULL;
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
    task_t *p = NULL;

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

    kernel_thread(init, 10, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);

    init_task_union.task.state = TASK_RUNNING;
    this_cpu()->scheduler_ok = 1;

    p = container_of(current->list.next, task_t, list);
    switch_to(current, p);
}

// ── init kernel thread ───────────────────────────────────
uint64_t init(uint64_t arg)
{
    serial_printk("init task is running, arg: %#018lx\n", arg);

    // ── Set up fd 0/1/2 for init_task ────────────────────
    // All descendant processes (spawn_user_task → do_fork)
    // will inherit these file descriptors.
    {
        files_t *files = files_alloc();
        if (!files) {
            serial_printk("init: files_alloc failed\n");
            return 1;
        }
        current->files = files;  // attach to *this* task, not idle

        // fd 0: stdin — /dev/keyboard (raw scancodes)
        vfs_node_t *kbd = vfs_lookup("/dev/keyboard");
        if (kbd) {
            file_t *f = file_alloc();
            f->type = FD_DEV;
            f->node = kbd;
            f->flags = O_RDONLY;
            int fd = fd_alloc(files, f);
            serial_printk("init: fd%d = /dev/keyboard (scancode reader)\n", fd);
        }

        // fd 1: stdout — /dev/fb (framebuffer text output)
        vfs_node_t *fb = vfs_lookup("/dev/fb");
        if (fb) {
            file_t *f = file_alloc();
            f->type = FD_DEV;
            f->node = fb;
            f->flags = O_WRONLY;
            int fd = fd_alloc(files, f);
            serial_printk("init: fd%d = /dev/fb (framebuffer writer)\n", fd);
        }

        // fd 2: stderr — /dev/serial (COM1 output)
        vfs_node_t *ser = vfs_lookup("/dev/serial");
        if (ser) {
            file_t *f = file_alloc();
            f->type = FD_DEV;
            f->node = ser;
            f->flags = O_WRONLY;
            int fd = fd_alloc(files, f);
            serial_printk("init: fd%d = /dev/serial (error output)\n", fd);
        }
    }

    // Spawn 2 quick-exit tasks to verify round-robin scheduling.
    for (int i = 0; i < 2; i++) {
        int64_t pid = spawn_user_task("/spin.elf");
        serial_printk("init: spin #%d → pid=%d\n", i, (int)pid);
    }
    // Spawn the interactive shell (replaces init.elf keyboard echo)
    spawn_user_task("/sh.elf");

    return 1;
}
