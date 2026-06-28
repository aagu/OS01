#include <kernel/completion.h>

void completion_init(completion_t *c)
{
    c->done = 0;
    wait_queue_init(&c->wq);
}

void wait_for_completion(completion_t *c)
{
    // Double-check pattern: if complete() already fired,
    // consume one count without sleeping.
    while (c->done == 0)
        wait_queue_sleep(&c->wq);
    c->done--;
}

void complete(completion_t *c)
{
    // Atomic increment — safe even if check-and-sleep races
    // with a concurrent complete() from IRQ context.
    __sync_fetch_and_add(&c->done, 1);
    wait_queue_wake_one(&c->wq);
}
