#include <kernel/task.h>
#include <kernel.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>

#include <string.h>
#include <stdlib.h>
#include <driver/serial.h>

uint64_t init(uint64_t arg);

void __switch_to(task_t *prev, task_t *next)
{
    // Do NOT update TSS.rsp0 — keep dedicated exception stack (0xFFFF800000007C00)
    // Exception handler must not share stack with running task.
    init_tss[0].rsp0 = 0xffff800000007c00;

    set_tss64(init_tss[0].rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);
    
    __asm__ __volatile__("movq %%fs, %0 \n\t":"=a"(prev->thread->fs));
    __asm__ __volatile__("movq %%gs, %0 \n\t":"=a"(prev->thread->gs));

    __asm__ __volatile__("movq %0, %%fs \n\t"::"a"(next->thread->fs));
    __asm__ __volatile__("movq %0, %%gs \n\t"::"a"(next->thread->gs));
}

uint64_t do_exit(uint64_t exit_code)
{
    color_printk(RED, BLACK, "exit task running, arg %#018lx\n", exit_code);
    while (1)
        ;
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

uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags, uint64_t stack_start, uint64_t stack_size)
{
    task_t *tsk = (task_t *)malloc(sizeof(task_t));
    thread_t *thd = (thread_t *)malloc(sizeof(thread_t));

    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));

    *tsk = *current;

    list_init(&tsk->list);
    list_add_to_before(&init_task_union.task.list, &tsk->list);
    tsk->pid++;
    tsk->state = TASK_UNINTERRUPTIBLE;

    tsk->thread = thd;

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
    regs.rflags = 0; /* IF=0: disable interrupts initially */
    regs.rip = (uint64_t)kernel_thread_func;

    return do_fork(&regs, flags, 0, 0);
}

void task_init()
{
    task_t *p = NULL;

    init_mm.pml4 = get_cr3();

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

    p = container_of(current->list.next, task_t, list);

    switch_to(current, p);
}

uint64_t init(uint64_t arg)
{
    serial_printk("init task is running, arg: %#018lx\n", arg);
    return 1;
}