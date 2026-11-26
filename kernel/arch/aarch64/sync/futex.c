/* kernel/arch/aarch64/sync/futex.c — aarch64 futex(2) implementation.
 *
 * Uses the 64-bucket wait queue hash and resolves user virtual addresses
 * through the AArch64 EL0 leaf walker (arch_user_vm_query inside a
 * detached user-root transaction).
 */

#include <sync/futex.h>
#include <arch/irq.h>
#include <kernel.h>
#include <sched/task.h>
#include <sync/wait.h>
#include <memory/memory.h>
#include <arch/aarch64/mmu.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/user_vm_internal.h>
#include <arch/aarch64/user_vm_leaf.h>
#include <errno.h>
#include <list.h>

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

static struct futex_bucket *futex_hash(void *pgd, const int *uaddr)
{
    uint64_t key = (uint64_t)pgd ^ ((uint64_t)uaddr >> 12);
    return &futex_buckets[key & (FUTEX_BUCKETS - 1)];
}

int do_futex_wait(int *uaddr, int val)
{
    uint64_t va = (uint64_t)uaddr;
    if ((va & 3ULL) != 0)
        return -EFAULT;

    task_t *self = current;
    if (!self || !self->mm || !self->mm->pgdir)
        return -EFAULT;
    if (va < AARCH64_USER_MIN || va >= self->addr_limit ||
        va > self->addr_limit - 4ULL)
        return -EFAULT;

    struct user_vm_leaf leaf = {0};
    int rc = aarch64_vm_edit_begin(self->mm);
    if (rc != 0)
        return -EFAULT;
    int qrc = arch_user_vm_query(aarch64_user_vm_root_va(self->mm),
                                 va & ~0xFFFULL, &leaf);
    aarch64_vm_edit_end();
    if (qrc != 0 || leaf.state != USER_VM_PRESENT)
        return -EFAULT;

    struct futex_bucket *bucket = futex_hash(self->mm->pgdir, uaddr);
    uint64_t flags = spin_lock_irqsave(&bucket->lock);

    void *kaddr = (void *)((uintptr_t)Phy_To_Virt(leaf.pa) + (va & 0xFFFULL));
    int futex_val = *(volatile int *)kaddr;
    if (futex_val != val) {
        spin_unlock_irqrestore(&bucket->lock, flags);
        return -EAGAIN;
    }

    list_add_to_before(&bucket->wq.head, &self->io_wait_node);
    self->state = TASK_INTERRUPTIBLE;
    spin_unlock_irqrestore(&bucket->lock, flags);

    schedule();
    arch_local_irq_enable();

    if (!list_is_empty(&self->io_wait_node)) {
        uint64_t f2 = spin_lock_irqsave(&bucket->lock);
        list_del_init(&self->io_wait_node);
        spin_unlock_irqrestore(&bucket->lock, f2);
    }
    self->state = TASK_RUNNING;

    if (__atomic_load_n(&self->signal, __ATOMIC_ACQUIRE))
        return -EINTR;

    return 0;
}

int do_futex_wake(int *uaddr, int val)
{
    uint64_t va = (uint64_t)uaddr;
    task_t *self = current;
    if (!self || !self->mm || !self->mm->pgdir)
        return -EFAULT;
    if (va < AARCH64_USER_MIN || va >= self->addr_limit)
        return -EFAULT;

    struct futex_bucket *bucket = futex_hash(self->mm->pgdir, uaddr);
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
