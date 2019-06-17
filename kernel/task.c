#include "task.h"
#include "descriptor.h"
#include "timer.h"

struct TASKCTL *taskctl;
struct TIMER *task_timer;

struct TASK *task_init(struct MEMMAN *memman)
{
    int i;
	struct TASK *task;
	taskctl = (struct TASKCTL *) memman_alloc_4k(memman, sizeof (struct TASKCTL));

	for (i = 0; i < MAX_TASK; i++) {
		taskctl->task0[i].flags = TASK_EMPTY;
		taskctl->task0[i].selector = (TSS0 + i) * 8;
        set_tssldt2_gdt(TSS0 + i, (int) &taskctl->task0[i].tss, 0x89);
	}

	task = task_alloc();
	task->flags = TASK_RUNNING; /*活动中标志*/
	task->priority = 2;
	taskctl->running = 1;
	taskctl->now = 0;
	taskctl->task[0] = task;
	load_tr(task->selector);
	task_timer = timer_alloc();
	timer_settime(task_timer, task->priority*100);
	return task;
}

struct TASK *task_alloc(void)
{
    int i;
	struct TASK *task;
	for (i = 0; i < MAX_TASK; i++) {
		if (taskctl->task0[i].flags == TASK_EMPTY) {
			task = &taskctl->task0[i];
			task->flags = TASK_CREATED;   /*正在使用的标志*/
			task->tss.eflags = 0x00000202; /* IF = 1; */
			task->tss.eax = 0; /*这里先置为0*/
			task->tss.ecx = 0;
			task->tss.edx = 0;
			task->tss.ebx = 0;
			task->tss.ebp = 0;
			task->tss.esi = 0;
			task->tss.edi = 0;
			task->tss.es = 0;
			task->tss.ds = 0;
			task->tss.fs = 0;
			task->tss.gs = 0;
			task->tss.ldtr = 0;
			task->tss.iomap = 0x40000000;
			task->priority = 2; /* 默认优先级 */
			return task;
		}
	}
	return 0; /*全部正在使用*/
}

void task_run(struct TASK *task, int priority)
{
	if (priority > 0) {
		task->priority = priority;
	}
	if (task->flags != TASK_RUNNING) {
		task->flags = TASK_RUNNING; /*活动中标志*/
		taskctl->task[taskctl->running] = task;
		taskctl->running++;
	}
	return;
}

void task_switch(void)
{
    struct TASK *task;
	taskctl->now++;
	if (taskctl->now == taskctl->running) {
		taskctl->now = 0;
	}
	task = taskctl->task[taskctl->now];
	timer_settime(task_timer, task->priority*100);
	if (taskctl->running >= 2) {
		taskswitch(0, task->selector);
	}
    return;
}

void task_sleep(struct TASK *task)
{
	int i;
	char ts = 0;
	if (task->flags == TASK_RUNNING) { /*如果指定任务处于唤醒状态*/
		if (task == taskctl->task[taskctl->now]) {
			ts = 1; /*让自己休眠的话，稍后需要进行任务切换*/
		}
		/*寻找task所在的位置*/
		for (i = 0; i < taskctl->running; i++) {
			if (taskctl->task[i] == task) {
				/*在这里*/
				break;
			}
		}
		taskctl->running--;
		if (i < taskctl->now) {
			taskctl->now--; /*需要移动成员，要相应地处理*/
		}
		/*移动成员*/
		for (; i < taskctl->running; i++) {
			taskctl->task[i] = taskctl->task[i + 1];
		}
		task->flags = 1; /*不工作的状态*/
		if (ts != 0) {
			/*任务切换*/
			if (taskctl->now >= taskctl->running) {
				/*如果now的值出现异常，则进行修正*/
				taskctl->now = 0;
			}
			taskswitch(0, taskctl->task[taskctl->now]->selector);
		}
	}
	return;
}

struct TASK *task_now(void)
{
	return taskctl->task[taskctl->now];
}