// hosttests/mock/arch6_bcache_runtime.c — task/IRQ/schedule substrate
// for the ARCH-6 bcache host tests.  schedule() defaults to aborting:
// a wait with no scripted future producer is a lost-wake bug in the
// tested code.  The case file installs a script for the windows it
// exercises (see test_bcache.c).
#include "arch6_bcache_runtime.h"
#include <stdio.h>
#include <stdlib.h>

int arch6_bcache_irq_enabled = 1;
task_t arch6_bcache_task = { .pid = 7, .state = TASK_RUNNING };

static void (*s_schedule_script)(void) = NULL;

void arch6_bcache_set_schedule_script(void (*fn)(void))
{
    s_schedule_script = fn;
}

void task_wake(task_t *t)
{
    t->state = TASK_RUNNING;
}

int arch6_bcache_schedule_count = 0;

void schedule(void)
{
    arch6_bcache_schedule_count++;
    if (arch6_bcache_irq_enabled) {
        fprintf(stderr, "bcache-test: schedule() with IRQs enabled\n");
        abort();
    }
    if (!s_schedule_script) {
        fprintf(stderr,
                "bcache-test: unexpected schedule() — no future producer "
                "(pid=%lld state=%lld)\n",
                (long long)current->pid, (long long)current->state);
        abort();
    }
    s_schedule_script();
}
