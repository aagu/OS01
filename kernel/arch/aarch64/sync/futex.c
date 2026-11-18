/* kernel/arch/aarch64/sync/futex.c — aarch64 futex subsystem init.
 *
 * Only the boot-5 initializer lives here: the aarch64 syscall table
 * does NOT serve SYS_futex (OS01_SYSCALL_A64_SUBSET hides it — the
 * x86 do_futex_wait/wake pair in arch/x86_64/sync/futex.c owns the
 * page-table walk with the x86 4KB/2MB leaf helper), so the wait
 * queues exist but no syscall reaches them.  Porting futex(2) means
 * moving this file's responsibilities onto the aarch64 leaf walker.
 */

#include <sync/futex.h>
#include <sync/wait.h>
#include <arch/spinlock.h>

#define FUTEX_BUCKETS 64

struct futex_bucket {
    spinlock_T lock;
    wait_queue_t wq;
};

static struct futex_bucket futex_buckets[FUTEX_BUCKETS];

void futex_init(void)
{
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        spin_init(&futex_buckets[i].lock);
        wait_queue_init(&futex_buckets[i].wq);
    }
}
