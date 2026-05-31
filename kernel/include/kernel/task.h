#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <list.h>
#include <stdint.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>

#define KERNEL_CS (0x08)
#define KERNEL_DS (0x10)

// RPL=3 (bits 0-1 = 11) required for iretq ring-0→ring-3 transition.
// GDT indices: USER code=5 (0x28), USER data=6 (0x30)
#define USER_CS (0x2b)  // 0x28 | 3
#define USER_DS (0x33)  // 0x30 | 3

#define CLONE_FS (1 << 0)
#define CLONE_FILES (1 << 1)
#define CLONE_SIGNAL (1 << 2)

#define STACK_SIZE (32 * 1024) // 32KB

extern char _text;
extern char _etext;
extern char _data;
extern char _edata;
extern char _rodata;
extern char _erodata;
extern char _bss;
extern char _ebss;
extern char _end;

extern uint64_t _stack_start;

extern void ret_from_intr(void);
extern void idle_resume(void);

#define TASK_RUNNING (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)
#define TASK_UNINTERRUPTIBLE (1 << 2)
#define TASK_ZOMBIE (1 << 3)
#define TASK_STOPPED (1 << 4)

#define PF_KTHREAD (1 << 0)
#define PF_PROCESS (1 << 1)
#define PF_THREAD (1 << 2)

typedef struct mm_struct
{
    uint64_t *pml4; // page map level 4 table, used in virtual memory

    uint64_t start_code, end_code; // start and end address of code segment
    uint64_t start_data, end_data; // start and end address of data segment
    uint64_t start_rodata, end_rodata; // start and end address of read-only data segment
    uint64_t start_brk, end_brk; // start and end address of break segment, used in dynamic memory allocation
    uint64_t start_stack; // base address pointer of stack segment
} mm_t;

typedef struct thread_struct
{
    uint64_t rsp0; // in tss, points to the base of stack
    
    uint64_t rip;
    uint64_t rsp; // current stack pointer, points to the top of stack

    uint64_t fs;
    uint64_t gs;

    uint64_t cr3; // page table base (physical address for CR3)

    uint64_t cr2;
    uint64_t trap_nr;
    uint64_t error_code;
} thread_t;

typedef struct task_struct
{
    list_t list; // link each task in task_list
    volatile int64_t state; // task state, RUNNING, SLEEPING, INTERRUPTIBLE
    uint64_t flags; // task flags, e.g. PF_KTHREAD, PF_PROCESS, PF_THREAD

    mm_t *mm; // memory management struct, e.g. page table
    thread_t *thread; // thread struct, save thread context before switching

    // address limit, e.g. userpace 0x0000000000000000 ~ 0x000000007fffffffffffff, 
    // kernel 0x0000000080000000 ~ 0x00000000ffffffffffffffff
    uint64_t addr_limit;

    int64_t pid; // process id
    int64_t counter; // time slice counter, used in round-robin scheduling
    int64_t signal; // signal mask, e.g. 0x0000000000000001 means SIGINT
    int64_t priority; // priority, used in priority scheduling

    void *stack_alloc_base; // original malloc ptr (before STACK_SIZE alignment)
} task_t;

union task_union
{
    task_t task;
    char stack[STACK_SIZE];
} __attribute__((aligned(8))); // 8 Byte aligned

mm_t init_mm;
thread_t init_thread;

#define INIT_TASK(task)               \
{                                     \
    .state = TASK_UNINTERRUPTIBLE,    \
    .flags = PF_KTHREAD,              \
    .mm = &init_mm,                   \
    .thread = &init_thread,           \
    .addr_limit = 0xffff800000000000, \
    .pid = 0,                         \
    .counter = 0,                     \
    .signal = 0,                      \
    .priority = 2,                    /* idle task quantum = 20 ms */ \
}

union task_union init_task_union __attribute__((__section__(".data.init_task"))) = {INIT_TASK(init_task_union.task)};

task_t *init_task[NR_CPUS] = {&init_task_union.task,0};
mm_t init_mm = {0};
thread_t init_thread =
{
    .rsp0 = (uint64_t)(init_task_union.stack + STACK_SIZE),  // idle task kernel stack
    .rip = (uint64_t)idle_resume,
    .rsp = (uint64_t)(init_task_union.stack + STACK_SIZE),
    .fs = KERNEL_DS,
    .gs = KERNEL_DS,
    .cr2 = 0,
    .trap_nr = 0,
    .error_code = 0,
};

struct tss_struct
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t iomapbaseaddr;
} __attribute__((packed));

#define INIT_TSS \
{ \
    .reserved0 = 0, \
    .rsp0 = 0xffff800000007c00, \
    .rsp1 = 0xffff800000007c00, \
    .rsp2 = 0xffff800000007c00, \
    .reserved1 = 0, \
    .ist1 = 0xffff800000007c00, /* exception stack (1KB from 0x7800) */ \
    .ist2 = 0xffff800000007800, /* IRQ stack */ \
    .ist3 = 0xffff800000007400, /* double fault stack */ \
    .ist4 = 0, \
    .ist5 = 0, \
    .ist6 = 0, \
    .ist7 = 0, \
    .reserved2 = 0, \
    .reserved3 = 0, \
    .iomapbaseaddr = 0 \
}

struct tss_struct init_tss[NR_CPUS] = { [0 ... NR_CPUS - 1] = INIT_TSS };

inline task_t* __attribute__((always_inline)) get_current_task()
{
    task_t *task = NULL;
    // -(int64_t)STACK_SIZE = 0xFFFFFFFFFFFF8000 in 64-bit two's complement
    __asm__ __volatile__("andq %%rsp, %0 \n\t" : "=r"(task) : "0"(-(int64_t)STACK_SIZE));
    return task;
}

#define current get_current_task()

#define GET_CURRENT \
    "movq %rsp, %rbx \n\t" \
    "andq $-32768, %rbx \n\t"

#define switch_to(prev, next) \
    do { \
        __asm__ __volatile__(                \
            "pushq %%rbp \n\t"       \
            "pushq %%rax \n\t"       \
            "cli \n\t"               /* disable IRQs during stack switch */ \
            "movq %%rsp, %0 \n\t"    \
            "movq %2, %%rsp \n\t"    \
            "leaq 1f(%%rip), %%rax \n\t" \
            "pushq %3 \n\t"          \
            "jmp __switch_to \n\t"   \
            "1: \n\t"                \
            "sti \n\t"               /* re-enable IRQs after switch */ \
            "popq %%rax \n\t"        \
            "popq %%rbp \n\t"        \
            : "=m"((prev)->thread->rsp),"=m"((next)->thread->rip) \
            : "m"((next)->thread->rsp), "m"((next)->thread->rip), \
              "D"((uint64_t)(prev)), "S"((uint64_t)(next)) \
            : "memory" \
        ); \
    } while (0)


uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags, uint64_t stack_start, uint64_t stack_size);
void task_init();
int64_t spawn_user_task(const char *path);
int64_t sys_exec(const char *path, pt_regs_t *regs);
void schedule(void);
uint64_t do_exit(uint64_t exit_code);

/* User stack layout (separate 2MB page from code at 0x400000) */
#define USER_STACK_BASE 0x600000UL
#define USER_STACK_TOP  (USER_STACK_BASE + 0x200000UL - 8)

/* Preemption flag — set by timer IRQ, checked in ret_from_intr */
extern uint64_t need_resched;

#endif