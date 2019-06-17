#ifndef _TASK_H_
#define _TASK_H_

#include "memory.h"
#include "kernel.h"

#define MAX_TASK 200
#define TSS0 6

#define TASK_EMPTY 0
#define TASK_CREATED 1
#define TASK_RUNNING 2

struct TSS
{
    int backlink, esp0, ss0, esp1, ss1, esp2, ss2, cr3;
    int eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int es, cs, ss, ds, fs, gs;
    int ldtr, iomap;
} __packed;

struct TASK
{
    int selector, flags;
    int priority;
    struct TSS tss;
};

struct TASKCTL
{
    int running;
    int now;
    struct TASK *task[MAX_TASK];
    struct TASK task0[MAX_TASK];
};

struct TASK *task_init(struct MEMMAN *memman);
struct TASK *task_alloc(void);
void task_run(struct TASK *task, int priority);
void task_switch(void);
void task_sleep(struct TASK *task);
struct TASK *task_now(void);
#endif