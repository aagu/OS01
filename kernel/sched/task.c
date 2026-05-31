#include <kernel/task.h>
#include <kernel.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>

#include <string.h>
#include <stdlib.h>
#include <driver/serial.h>

uint64_t init(uint64_t arg);

void __switch_to(task_t *prev, task_t *next)
{
    // Use per-task kernel stack for ring-3→ring-0 transitions
    init_tss[0].rsp0 = next->thread->rsp0;

    set_tss64(init_tss[0].rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    __asm__ __volatile__("movq %%fs, %0 \n\t":"=a"(prev->thread->fs));
    __asm__ __volatile__("movq %%gs, %0 \n\t":"=a"(prev->thread->gs));

    __asm__ __volatile__("movq %0, %%fs \n\t"::"a"(next->thread->fs));
    __asm__ __volatile__("movq %0, %%gs \n\t"::"a"(next->thread->gs));

    // Switch page table if the next task has its own address space
    if (next->thread->cr3 && next->thread->cr3 != prev->thread->cr3) {
        __asm__ __volatile__("movq %0, %%cr3" :: "r"(next->thread->cr3) : "memory");
    }
}

// ── Preemption flag ──────────────────────────────────────
// Set by pit_handler (timer IRQ) on every tick,
// cleared by schedule() after a context switch.
uint64_t need_resched = 0;

// Global PID counter
static uint64_t pid_counter = 1;

// Set to 1 once task_init() completes; schedule() returns
// immediately before this point (timer ticks before tasking
// are harmless no-ops).
static int scheduler_initialized = 0;

void schedule(void)
{
    task_t *next;

    if (!scheduler_initialized)
        return;

    // Decrement current task's quantum (one tick per call)
    if (current->counter > 0)
        current->counter--;

    // If current still has quantum, keep running
    if (current->state == TASK_RUNNING && current->counter > 0) {
        need_resched = 0;
        return;
    }

    // Round-robin: scan forward from current for a RUNNING task
    next = container_of(current->list.next, task_t, list);
    while (next != &init_task_union.task && next != current) {
        if (next->state == TASK_RUNNING)
            goto do_switch;
        next = container_of(next->list.next, task_t, list);
    }

    // Wrap past the list head and try the rest
    if (next == &init_task_union.task) {
        next = container_of(next->list.next, task_t, list);
        while (next != &init_task_union.task && next != current) {
            if (next->state == TASK_RUNNING)
                goto do_switch;
            next = container_of(next->list.next, task_t, list);
        }
    }

    // No other RUNNING task found — give current a fresh quantum
    if (current->state == TASK_RUNNING) {
        current->counter = current->priority > 0 ? current->priority : 1;
        need_resched = 0;
        return;
    }

    // Last resort: idle task
    if (init_task_union.task.state == TASK_RUNNING) {
        next = &init_task_union.task;
        goto do_switch;
    }

    return;  // nothing to switch to

do_switch:
    next->counter = next->priority > 0 ? next->priority : 1;
    need_resched = 0;
    switch_to(current, next);
}

// ── Known issue: task resource leak ──────────────────────────
// do_exit() sets state=TASK_ZOMBIE and switches away, but never
// frees the task's resources:
//
//  Leaked per user task:
//    - task_union       (~64KB, from malloc)
//    - thread_t         (~72B,  from malloc)
//    - mm_t             (~80B,  from malloc)
//    - PML4 page        (4KB,   from vmm_alloc_map/calloc)
//    - PDPT+PDE pages   (~8KB,  from vmm_map_page/calloc)
//    - user 2MB page    (2MB,   from alloc_pages + vmm_map_page)
//
//  These accumulate until a reaper thread is implemented.
//  free(current) before schedule() is unsafe because
//  switch_to(current, next) dereferences current->thread->rsp.
//
uint64_t do_exit(uint64_t exit_code)
{
    serial_printk("task %d exiting with code %#018lx\n", current->pid, exit_code);
    current->state = TASK_ZOMBIE;
    schedule();
    return 0;  // never reached — schedule() does switch_to
}

extern void kernel_thread_func(void);
__asm__(
    "kernel_thread_func:\n\t"
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
    "   movq %rax, %gs \n\t"
    "   popq %rax    \n\t" // restore RAX
    "   addq $0x38, %rsp \n\t"
    "   movq %rdx, %rdi \n\t"  // arg
    "   callq *%rbx \n\t"      // call fn(arg)
    "   movq %rax, %rdi \n\t"
    "   callq do_exit \n\t"
);

// ── User code stub (position-independent, copied to user page at 0x400000) ──
__asm__(
    ".globl user_code_start\n\t"
    "user_code_start:\n\t"
    "   lea msg1(%rip), %rdi\n\t"     // rdi = string
    "   movl $1, %eax\n\t"            // SYS_write = 1
    "   int $0x80\n\t"
    "   movl $'!', %edi\n\t"          // rdi = '!'
    "   movl $0, %eax\n\t"            // SYS_putchar = 0
    "   int $0x80\n\t"
    "   movl $10, %edi\n\t"           // rdi = '\n'
    "   movl $0, %eax\n\t"
    "   int $0x80\n\t"
    "   movl $42, %edi\n\t"           // rdi = exit code
    "   movl $2, %eax\n\t"            // SYS_exit = 2
    "   int $0x80\n\t"
    "1:\n\t"
    "   pause\n\t"
    "   jmp 1b\n\t"
    ".globl msg1\n\t"
    "msg1:\n\t"
    "   .asciz \"Hello from user space\"\n\t"
    ".globl user_code_end\n\t"
    "user_code_end:\n\t"
);

uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags, uint64_t stack_start, uint64_t stack_size)
{
    // Allocate with extra space for STACK_SIZE alignment
    task_t *tsk = (task_t *)malloc(sizeof(union task_union) + STACK_SIZE);
    tsk = (task_t *)(((uint64_t)tsk + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)malloc(sizeof(thread_t));

    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));

    *tsk = *current;

    list_init(&tsk->list);
    list_add_to_before(&init_task_union.task.list, &tsk->list);
    tsk->pid = pid_counter++;
    tsk->state = TASK_UNINTERRUPTIBLE;
    tsk->priority = 3;        /* default quantum = 30 ms at 100 Hz */

    tsk->thread = thd;

    // Inherit parent's page table by default (overridden for user tasks)
    thd->cr3 = current->thread->cr3;

    // Detect user task from CS Register Permission Level
    if ((regs->cs & 3) == 3) {
        tsk->flags &= ~PF_KTHREAD;
        tsk->addr_limit = 0x00007FFFFFFFFFFF;
    }

    memcpy((void *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t)), regs, sizeof(pt_regs_t));

    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs = KERNEL_DS;
    thd->gs = KERNEL_DS;

    thd->rip = regs->rip;
    if (! (tsk->flags & PF_KTHREAD))
        thd->rip = regs->rip = (uint64_t)ret_from_intr;

    tsk->state = TASK_RUNNING;

    return 0;
}

int kernel_thread(uint64_t (* fn)(uint64_t), uint64_t arg, uint64_t flags)
{
    pt_regs_t regs;
    memset(&regs, 0, sizeof(pt_regs_t));

    regs.rbx = (uint64_t)fn;
    regs.rdx = (uint64_t)arg;

    regs.ds = KERNEL_DS;
    regs.es = KERNEL_DS;
    regs.cs = KERNEL_CS;
    regs.ss = KERNEL_DS;
    regs.rflags = (1 << 9); /* IF=1: enable interrupts */
    regs.rip = (uint64_t)kernel_thread_func;

    return do_fork(&regs, flags, 0, 0);
}

void task_init()
{
    task_t *p = NULL;

    init_mm.pml4 = get_cr3();
    init_thread.cr3 = (uint64_t)init_mm.pml4;  // physical address of kernel PML4

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

    kernel_thread(init, 10, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);

    init_task_union.task.state = TASK_RUNNING;

    // Allow schedule() to run now that we have tasks and a valid current
    scheduler_initialized = 1;

    p = container_of(current->list.next, task_t, list);

    switch_to(current, p);
}

// ──────────────────────────────────────────
//  User task creation
// ──────────────────────────────────────────

#define USER_CODE_ADDR   0x400000UL
#define USER_STACK_TOP   (USER_CODE_ADDR + 0x200000UL - 8)  // within 2MB page

// Allocates per-process resources (see do_exit comment for leak status).
void user_task_create(void)
{
    extern char user_code_start[], user_code_end[];
    size_t code_size = (uint64_t)user_code_end - (uint64_t)user_code_start;

    // ── Allocate task structures ──
    // task_union must be STACK_SIZE-aligned for get_current_task() to work
    task_t *tsk = (task_t *)malloc(sizeof(union task_union) + STACK_SIZE);
    tsk = (task_t *)(((uint64_t)tsk + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)malloc(sizeof(thread_t));
    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));

    tsk->state = TASK_UNINTERRUPTIBLE;
    tsk->flags = 0;                        // NOT PF_KTHREAD → user task
    tsk->addr_limit = 0x00007FFFFFFFFFFF;
    tsk->pid = pid_counter++;
    tsk->counter = 1;
    tsk->signal = 0;
    tsk->priority = 5;        /* user tasks get 50 ms quantum */

    list_init(&tsk->list);
    list_add_to_before(&init_task_union.task.list, &tsk->list);
    tsk->thread = thd;

    // ── Create per-process page table ──
    mm_t *mm = (mm_t *)malloc(sizeof(mm_t));
    memset(mm, 0, sizeof(mm_t));

    // Initialize heap break — heap starts just after code, within the 2MB page
    mm->start_brk = USER_CODE_ADDR + 0x1000;
    mm->end_brk   = mm->start_brk;

    uint64_t *user_pml4 = (uint64_t *)vmm_alloc_map();  // 4KB zeroed PML4

    // Copy kernel entries (256-511) so kernel is accessible in ring 0
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);
    memcpy(&user_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    // Allocate one 2MB page for user code + stack
    struct Page *user_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!user_page) {
        free((void *)mm);
        free((void *)user_pml4);
        free((void *)thd);
        free((void *)tsk);
        return;
    }
    uint64_t page_phys = user_page->phy_address;

    // Map page at USER_CODE_ADDR in user page table with user-accessible flags
    vmm_map_page(user_pml4, page_phys, USER_CODE_ADDR, PAGE_USER_Page);

    // Copy user code stub
    memcpy((void *)Phy_To_Virt(page_phys), user_code_start, code_size);

    // Store physical PML4 address (same convention as init_mm)
    mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)user_pml4);
    tsk->mm = mm;
    thd->cr3 = (uint64_t)mm->pml4;

    // ── Set up pt_regs for iretq to ring 3 ──
    pt_regs_t *regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
    memset(regs, 0, sizeof(pt_regs_t));
    regs->cs      = USER_CS;              // DPL=3 → ring 3 transition
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = USER_STACK_TOP;
    regs->rip     = USER_CODE_ADDR;
    regs->rflags  = (1 << 9);             // IF=1

    // ── Set up thread context ──
    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;
    thd->rip  = (uint64_t)ret_from_intr;  // first entry via RESTORE_ALL → iretq

    tsk->state = TASK_RUNNING;

    serial_printk("user task created: pid=%d rip=%p rsp=%p cr3=%p\n",
                  tsk->pid, regs->rip, regs->rsp, thd->cr3);
}

uint64_t init(uint64_t arg)
{
    serial_printk("init task is running, arg: %#018lx\n", arg);
    user_task_create();
    return 1;
}
