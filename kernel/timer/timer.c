#include <device/timer.h>
#include <kernel/debug.h>
#include <kernel/softirq.h>
#include <kernel.h>
#include <kernel/printk.h>
#include <stdlib.h>
#include <kernel/task.h>
#include <kernel/arch/spinlock.h>

uint64_t volatile jiffies;
timer_t timer_list_head;
static spinlock_T timer_lock = { .lock = 1L };

void init_timer(timer_t * timer, void (* func)(void * data), void * data, uint64_t expire_jiffies)
{
    list_init(&timer->list);
    timer->func = func;
    timer->data = data;
    timer->expire_jiffies = jiffies + expire_jiffies;
}

timer_t * create_timer(void (* func)(void * data), void * data, uint64_t expire_jiffies)
{
    timer_t * timer = (timer_t *)calloc(1, sizeof(timer_t));
    init_timer(timer, func, data, expire_jiffies);
    return timer;
}

void do_timer(void * data __attribute__((unused)))
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    timer_t * timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    while ((!list_is_empty(&timer_list_head.list)) && (timer->expire_jiffies <= jiffies))
    {
        list_del(&timer->list);                     // direct list_del, not del_timer()
        timer->active = 0;
        spin_unlock_irqrestore(&timer_lock, flags); // release before callback

        timer->func(timer->data);                   // callback runs unlocked

        flags = spin_lock_irqsave(&timer_lock);      // re-acquire
        timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    }
    spin_unlock_irqrestore(&timer_lock, flags);

    // Note: 100 Hz color_printk debug is DISABLED by default — the
    // serial UART TX path (write_serial → poll THRE bit) adds latency
    // that interferes with interactive shell input on every tick.
    // Enable via -DOS01_DEBUG_TIMER in kernel/Makefile CFLAGS when needed.
    // Preemptive scheduling is driven from pit_handler (hardirq) which
    // decrements current->counter and sets need_resched.  The flag is
    // picked up in ret_from_intr (entry.S) which calls schedule() before
    // RESTORE_ALL → iretq, so a context switch happens on the interrupt
    // return path — never inline in the timer handler itself.
    // NOTE: this debug print must stay DISABLED in production — 100 Hz
    // serial output floods the UART and can interfere with interactive
    // shell input handling.
}

void timer_init()
{
    jiffies = 0;
    init_timer(&timer_list_head, NULL, NULL, -1UL);
    register_softirq(0, &do_timer, NULL);
}

void add_timer(timer_t * timer)
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    timer_t * head = &timer_list_head;
    timer_t * tmp;

    if (list_is_empty(&head->list)) {
        list_add_to_behind(&head->list, &timer->list);
        timer->active = 1;
        spin_unlock_irqrestore(&timer_lock, flags);
        return;
    }

    // Find insertion point: first node with expire >= timer->expire.
    // Guard with tmp != head to stop at the tail — the old code let
    // the walk wrap around the head node (expire_jiffies of the head
    // is 0 < any real expire), which either loops forever or inserts
    // behind a stale node so do_timer never reaches the new timer.
    tmp = container_of(list_next(&head->list), timer_t, list);
    while (tmp != head && tmp->expire_jiffies < timer->expire_jiffies)
        tmp = container_of(list_next(&tmp->list), timer_t, list);
    list_add_to_behind(&tmp->list, &timer->list);
    timer->active = 1;
    spin_unlock_irqrestore(&timer_lock, flags);
}

void del_timer(timer_t * timer)
{
    uint64_t flags = spin_lock_irqsave(&timer_lock);
    // Guard against double-removal: do_timer() removes the entry via
    // list_del() before invoking the callback, so a later del_timer()
    // on an already-fired timer would corrupt the list.  list_is_empty
    // on a list_del'd node is UB, so track liveness with a flag on the
    // node instead (see timer_t.active).
    if (timer->active) {
        list_del(&timer->list);
        timer->active = 0;
    }
    spin_unlock_irqrestore(&timer_lock, flags);
}

int timer_has_expired(uint64_t now)
{
    if (list_is_empty(&timer_list_head.list)) return 0;
    timer_t *first = container_of(list_next(&timer_list_head.list), timer_t, list);
    return first->expire_jiffies <= now;
}