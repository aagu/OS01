/* poll_clocksource_stub.c — host-test stub for clocksource globals.
 * The real poll/select paths call clocksource_read_ns() (inline) which
 * references these exported globals.  In the host harness we keep the
 * clocksource inactive so it falls back to the jiffies timeline. */
#include "poll_test_runtime.h"

spinlock_T task_list_lock = { .lock = 1 };
poll_test_task_union_t init_task_union = {
    .task = {
        .list = { &init_task_union.task.list, &init_task_union.task.list },
    },
};

list_t *task_list_next(list_t *pos)
{
    return pos->next;
}

int signal_pgrp(pid_t target, int sig)
{
    (void)target;
    (void)sig;
    return 0;
}

bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    (void)writable;
    return len == 0 || addr != 0;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault;
    (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault;
    (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

bool     clocksource_active = false;
uint32_t clocksource_mult   = 0;
uint32_t clocksource_shift  = 0;
