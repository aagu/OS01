#include <kernel/futex.h>
#include <kernel/arch/irq.h>
#include <kernel.h>           // container_of
#include <kernel/task.h>      // task_t, current
#include <kernel/wait.h>      // wait_queue_t
#include <kernel/memory.h>    // Phy_To_Virt
#include <kernel/percpu.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/arch/mmu.h> // arch_virt_to_phys (4KB/2MB full leaf)
#include <kernel/uaccess.h>  // syscall_check_user_range

#include <uapi/futex.h>
#include <list.h>
#include <errno.h>

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

static struct futex_bucket *futex_hash(void *pml4, const int *uaddr)
{
    uint64_t key = (uint64_t)pml4 ^ ((uint64_t)uaddr >> 12);
    return &futex_buckets[key & (FUTEX_BUCKETS - 1)];
}

static uint64_t offset_in_page(uint64_t va)
{
    return va & 0xFFF;
}

int do_futex_wait(int *uaddr, int val)
{
    // 1. Validate address (alignment, range, perms) BEFORE acquiring
    //    the bucket lock.  check_user_range is a snapshot but catches
    //    the easy null-pointer, kernel-ptr, and unmapped cases.
    //    Use _ft under the lock for the actual read so a racing
    //    munmap doesn't blow up here.
    if (((uint64_t)uaddr & 3))
        return -EFAULT;
    if (!syscall_check_user_range((uint64_t)uaddr, 4, true))
        return -EFAULT;

    task_t *self = current;
    struct futex_bucket *bucket = futex_hash(self->mm->pml4, uaddr);

    uint64_t flags = spin_lock_irqsave(&bucket->lock);

    // 2. Walk page table — use arch_virt_to_phys (full-leaf translation
    //    handling 4KB + 2MB correctly).  The old user_va_to_phys was
    //    2MB-only and would return the wrong page for 4KB/COW entries
    //    (see plan v9).
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)self->mm->pml4);
    uint64_t page_phys = arch_virt_to_phys(user_pml4, (uint64_t)uaddr & ~0xFFFULL);
    if (!page_phys) {
        spin_unlock_irqrestore(&bucket->lock, flags);
        return -EFAULT;
    }

    // 3. Read *uaddr via kernel mapping.  We translate the page ONCE
    //    (under the lock) and read the in-page offset directly.  This
    //    is NOT a copy_*_ft — we have the physical page mapped, so a
    //    user-side munmap can't reach this read; the worst case is a
    //    transient stale value, which the bucket-spinlock + val retry
    //    loop already covers (the standard futex FUTEX_WAIT semantic).
    void *kaddr = (void *)Phy_To_Virt(page_phys) + offset_in_page((uint64_t)uaddr);
    int futex_val = *(volatile int *)kaddr;

    if (futex_val != val) {
        spin_unlock_irqrestore(&bucket->lock, flags);
        return -EAGAIN;
    }

    // 4. Block: add to wq, set INTERRUPTIBLE (under bucket lock), then unlock + schedule
    list_add_to_before(&bucket->wq.head, &self->io_wait_node);
    self->state = TASK_INTERRUPTIBLE;
    spin_unlock_irqrestore(&bucket->lock, flags);

    schedule();
    arch_local_irq_enable();

    // 5. Cleanup on return
    if (!list_is_empty(&self->io_wait_node)) {
        uint64_t f2 = spin_lock_irqsave(&bucket->lock);
        list_del_init(&self->io_wait_node);
        spin_unlock_irqrestore(&bucket->lock, f2);
    }
    self->state = TASK_RUNNING;

    if (self->signal)
        return -EINTR;

    return 0;
}

int do_futex_wake(int *uaddr, int val)
{
    // Fast-reject range check (defends against kernel-pointer/null).
    // do_futex_wait also does this; do_futex_wake is the companion
    // path that doesn't dereference user memory (just hashes uaddr
    // and wakes wakers), so the check is purely a hardening
    // measurement.
    if ((uint64_t)uaddr < USER_MIN_ADDR ||
        (uint64_t)uaddr >= current->addr_limit)
        return -EFAULT;

    struct futex_bucket *bucket = futex_hash(current->mm->pml4, uaddr);

    uint64_t flags = spin_lock_irqsave(&bucket->lock);

    int woken = 0;
    while (woken < val && !list_is_empty(&bucket->wq.head)) {
        list_t *node = bucket->wq.head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        task_wake(t);
        woken++;
    }

    spin_unlock_irqrestore(&bucket->lock, flags);
    return woken;
}
