/* Host shadow of <fs/poll.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * Provides the minimal poll/wait surface keyboard.c uses:
 * poll_wait_entry_t (walked + list_del_init'd by
 * keyboard_wake_pollers), a poll_table_t with the `triggered` short
 * circuit, POLLIN/POLLRDNORM and poll_requested_read().  poll_wait()
 * and wait_queue_wake_all() are mocked in test_i8042_demux.c.
 * spinlock_T comes from the force-included i8042_test_runtime.h. */
#ifndef _KERNEL_POLL_H
#define _KERNEL_POLL_H

#include <stdint.h>
#include <stdbool.h>
#include <list.h>

#define POLLIN     0x001
#define POLLRDNORM 0x040

typedef struct wait_queue {
    int mock;
} wait_queue_t;

typedef struct poll_wait_entry {
    list_t       node;
    wait_queue_t *poll_wq;
    spinlock_T   *fd_lock;
} poll_wait_entry_t;

typedef struct poll_table {
    wait_queue_t wq;
    bool         triggered;
} poll_table_t;

static inline bool poll_requested_read(uint32_t requested)
{
    return (requested & (POLLIN | POLLRDNORM)) != 0;
}

void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock);
void wait_queue_wake_all(wait_queue_t *wq);

#endif /* _KERNEL_POLL_H */
