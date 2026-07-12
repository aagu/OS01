# Futex-based Mutex Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a futex-like synchronization primitive to OS01: kernel-internal sleepable mutex (`mutex_t`), `SYS_futex` syscall, and userspace `pthread_mutex_t`.

**Architecture:** Kernel mutex uses existing `wait_queue_t` with atomic CAS fast path. `SYS_futex` uses a fixed 64-bucket hash table keyed by `(mm->pml4, uaddr)`. Userspace `pthread_mutex_t` is a 3-state int (0=free, 1=locked, 2=locked+waiters) using `atomic_xchg` + `futex()` syscall on contention.

**Tech Stack:** C (kernel + libc), x86_64 asm (atomic ops), existing wait_queue_t infrastructure.

## Global Constraints

- `SYS_futex` number MUST be 47 (follows `SYS_munmap=46`)
- `FUTEX_WAIT=0`, `FUTEX_WAKE=1` (defined in `kernel/include/uapi/futex.h`)
- `mutex_t.owner` type: `volatile int64_t` (matches `task_t.pid`)
- Kernel mutex API: `mutex_init`, `mutex_lock`, `mutex_trylock`, `mutex_unlock`, `mutex_lock_interruptible`
- `TASK_INTERRUPTIBLE` MUST be set BEFORE releasing any lock in wait/sleep paths (prevents SMP lost-wakeup)
- No `copy_from_user` — use `user_va_to_phys` + `Phy_To_Virt` for userspace memory access
- `atomic_xchg` returns OLD value; `atomic_cas` returns `!= 0` on success; both from `kernel/include/kernel/arch/x86_64/cpu.h`

---

### Task 1: Fix wait_queue_sleep SMP lost-wakeup race

**Files:**
- Modify: `kernel/intr/wait.c:11-31`

**Interfaces:**
- Consumes: existing `wait_queue_t`, `spin_lock_irqsave`/`spin_unlock_irqrestore`, `current->io_wait_node`
- Produces: race-free `wait_queue_sleep` (atomic state set before releasing wq lock)

- [ ] **Step 1: Move `TASK_INTERRUPTIBLE` before `spin_unlock_irqrestore`**

Replace lines 18-23 in `kernel/intr/wait.c`:

```c
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    list_add_to_before(&wq->head, &current->io_wait_node);
    current->state = TASK_INTERRUPTIBLE;  // ← set before unlock
    spin_unlock_irqrestore(&wq->lock, flags);

    schedule();
```

- [ ] **Step 2: Build-check**

```bash
make clean
make kernel.bin 2>&1 | tail -20
```

Expected: compiles clean, no warnings.

- [ ] **Step 3: Commit**

```bash
git add kernel/intr/wait.c
git commit -m "fix: set TASK_INTERRUPTIBLE before releasing wq lock in wait_queue_sleep

Prevents SMP lost-wakeup race where a concurrent wait_queue_wake_one
sets the task RUNNING after the lock is released but before the task
sets itself INTERRUPTIBLE, causing a permanent sleep."
```

---

### Task 2: Kernel mutex header and implementation

**Files:**
- Create: `kernel/include/kernel/mutex.h`
- Create: `kernel/mutex.c`

**Interfaces:**
- Produces: `mutex_t` type and 5 API functions
- Consumes: `wait_queue_t`, `atomic_cas` from `cpu.h`, `current->pid`, `current->signal`

- [ ] **Step 1: Write `kernel/include/kernel/mutex.h`**

```c
#ifndef _KERNEL_MUTEX_H
#define _KERNEL_MUTEX_H

#include <kernel/wait.h>
#include <stdint.h>

typedef struct {
    volatile int64_t owner;  // 0=free, >0=holder PID
    wait_queue_t wq;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_trylock(mutex_t *m);       // returns 1 on success
void mutex_unlock(mutex_t *m);
int  mutex_lock_interruptible(mutex_t *m);  // returns -EINTR on signal

#endif
```

- [ ] **Step 2: Write `kernel/mutex.c`**

```c
#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/arch/x86_64/cpu.h>
#include <errno.h>

void mutex_init(mutex_t *m)
{
    m->owner = 0;
    wait_queue_init(&m->wq);
}

void mutex_lock(mutex_t *m)
{
    while (atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) == 0) {
        wait_queue_sleep(&m->wq);
    }
}

int mutex_trylock(mutex_t *m)
{
    return atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid);
}

void mutex_unlock(mutex_t *m)
{
    atomic_write((volatile uint64_t *)&m->owner, 0);  // xchgq provides full barrier
    wait_queue_wake_one(&m->wq);
}

int mutex_lock_interruptible(mutex_t *m)
{
    while (atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) == 0) {
        if (current->signal)
            return -EINTR;
        wait_queue_sleep(&m->wq);
        if (current->signal)
            return -EINTR;
    }
    return 0;
}
```

- [ ] **Step 3: Build-check**

```bash
make clean
make kernel.bin 2>&1 | tail -20
```

Expected: compiles clean. `mutex.c` is auto-discovered by `$(wildcard *.c)` in kernel/Makefile.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/mutex.h kernel/mutex.c
git commit -m "feat: add kernel sleepable mutex (mutex_t)

Provides mutex_init/lock/trylock/unlock/lock_interruptible using
atomic CAS fast path with wait_queue_t fallback."
```

---

### Task 3: Futex constants and kernel futex infrastructure

**Files:**
- Create: `kernel/include/uapi/futex.h` (shared userspace+kernel constants)
- Create: `kernel/include/kernel/futex.h` (kernel-internal API)
- Create: `kernel/futex.c` (hash table, do_futex_wait, do_futex_wake)

**Interfaces:**
- Produces: `futex_init()`, `do_futex_wait(uaddr, val)`, `do_futex_wake(uaddr, val)`
- Consumes: `wait_queue_t`, `PAGE_4K_ALIGN`, `Phy_To_Virt`, `user_va_to_phys`, `current->mm->pml4`, `current->io_wait_node`, `current->signal`

- [ ] **Step 1: Write `kernel/include/uapi/futex.h`**

```c
#ifndef _UAPI_FUTEX_H
#define _UAPI_FUTEX_H

#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

#endif
```

- [ ] **Step 2: Write `kernel/include/kernel/futex.h`**

```c
#ifndef _KERNEL_FUTEX_H
#define _KERNEL_FUTEX_H

void futex_init(void);
int do_futex_wait(int *uaddr, int val);
int do_futex_wake(int *uaddr, int val);

#endif
```

- [ ] **Step 3: Make `user_va_to_phys` visible**

Both changes MUST be done together before build-check:

**(a)** In `kernel/arch/x86_64/trap.c`, remove the `static` qualifier from `user_va_to_phys` (line 45):

```c
// line 45 before:
static uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va)

// line 45 after:
uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va)
```

**(b)** Add an extern declaration at the top of `kernel/futex.c`:

```c
extern uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va);
```

- [ ] **Step 4: Write `kernel/futex.c`**

```c
#include <kernel/futex.h>
#include <kernel/wait.h>
#include <kernel/memory.h>    // Phy_To_Virt
#include <kernel/percpu.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/arch/x86_64/cpu.h>
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
    // 1. Validate address
    if ((uint64_t)uaddr >= current->addr_limit || ((uint64_t)uaddr & 3))
        return -EFAULT;

    task_t *self = current;
    struct futex_bucket *bucket = futex_hash(self->mm->pml4, uaddr);

    uint64_t flags = spin_lock_irqsave(&bucket->lock);

    // 2. Walk page table — page must be present
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)self->mm->pml4);
    uint64_t page_phys = user_va_to_phys(user_pml4, (uint64_t)uaddr & ~0xFFFULL);
    if (!page_phys) {
        spin_unlock_irqrestore(&bucket->lock, flags);
        return -EFAULT;
    }

    // 3. Read *uaddr via kernel mapping
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
    if ((uint64_t)uaddr >= current->addr_limit)
        return -EFAULT;

    struct futex_bucket *bucket = futex_hash(current->mm->pml4, uaddr);

    uint64_t flags = spin_lock_irqsave(&bucket->lock);

    int woken = 0;
    while (woken < val && !list_is_empty(&bucket->wq.head)) {
        list_t *node = bucket->wq.head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
        woken++;
    }

    spin_unlock_irqrestore(&bucket->lock, flags);
    return woken;
}
```

- [ ] **Step 5: Build-check**

```bash
make clean
make kernel.bin 2>&1 | tail -30
```

Expected: compiles clean. `futex.c` is auto-discovered by `$(wildcard *.c)` in kernel/Makefile.

> **Note:** `user_va_to_phys` only handles 2MB huge pages (walks to L2 PDE). OS01's user space currently uses 2MB pages everywhere, so this is correct for MVP. If 4KB page support is added later, this function must be extended to walk L1 PTEs.

- [ ] **Step 6: Commit**

```bash
git add kernel/include/uapi/futex.h kernel/include/kernel/futex.h kernel/futex.c
git commit -m "feat: add futex hash table with do_futex_wait/do_futex_wake

Fixed 64-bucket hash keyed by (mm->pml4, uaddr). Uses user_va_to_phys
for safe userspace memory access (no copy_from_user). Sets TASK_INTERRUPTIBLE
under bucket lock to prevent SMP lost-wakeup."
```

---

### Task 4: Syscall dispatch and init integration

**Files:**
- Modify: `kernel/arch/x86_64/trap.c` (add `case SYS_futex`, `syscall_names[47]`, make `user_va_to_phys` non-static)
- Modify: `kernel/kernel/main.c` (call `futex_init()`)

**Interfaces:**
- Consumes: `do_futex_wait`, `do_futex_wake`, `SYS_futex=47`

- [ ] **Step 1: Add `[47] = "futex"` to syscall_names array**

Locate the `syscall_names` array around line 854 in `kernel/arch/x86_64/trap.c`. Add after the `[46] = "munmap"` entry:

```c
        [47] = "futex",
```

- [ ] **Step 2: Add `#include <uapi/futex.h>` and `#include <kernel/futex.h>` to includes**

In `kernel/arch/x86_64/trap.c`, add after existing includes:

```c
#include <uapi/futex.h>
#include <kernel/futex.h>
```

- [ ] **Step 3: Add `SYS_futex` case to `do_system_call`**

Before the `default:` case (around line 2040), add:

```c
    case SYS_futex: {
        int *uaddr = (int *)regs->rdi;
        int op = (int)regs->rsi;
        int val = (int)regs->rdx;

        if ((uint64_t)uaddr >= current->addr_limit) {
            regs->rax = -EFAULT;
            break;
        }

        switch (op) {
        case FUTEX_WAIT:
            regs->rax = do_futex_wait(uaddr, val);
            break;
        case FUTEX_WAKE:
            regs->rax = do_futex_wake(uaddr, val);
            break;
        default:
            regs->rax = -EINVAL;
        }
        break;
    }
```

- [ ] **Step 4: Call `futex_init()` in `kernel/kernel/main.c`**

Around line 285 (after `selftest_run_all()`, before `task_init()`), add:

```c
    futex_init();                        // init futex hash buckets
```

- [ ] **Step 5: Build-check**

```bash
make clean
make kernel.bin 2>&1 | tail -30
```

Expected: compiles clean.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/trap.c kernel/kernel/main.c
git commit -m "feat: wire up SYS_futex syscall and kernel init

Adds SYS_futex dispatch (numbers 47, FUTEX_WAIT/FUTEX_WAKE) to
do_system_call, syscall_names[47], and calls futex_init() in
main.c before task_init(). Makes user_va_to_phys non-static."
```

---

### Task 5: Userspace futex wrapper

**Files:**
- Create: `libc/include/sys/futex.h`
- Modify: `libc/include/sys/syscall.h`

**Interfaces:**
- Produces: `futex(uaddr, op, val)` inline wrapper
- Consumes: `SYS_futex` number

- [ ] **Step 1: Add `#define SYS_futex 47` to syscall.h**

In `libc/include/sys/syscall.h`, after `#define SYS_munmap 46` (line 53), add:

```c
#define SYS_futex     47
```

- [ ] **Step 2: Create `libc/include/sys/futex.h`**

```c
#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

#include <sys/syscall.h>

#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

static inline int futex(int *uaddr, int op, int val)
{
    return (int)syscall(SYS_futex, (uint64_t)uaddr, (uint64_t)op, (uint64_t)val);
}

#endif
```

- [ ] **Step 3: Add sysroot install**

Ensure `libc/include/sys/futex.h` is installed to sysroot. The `install-headers` target in `kernel/Makefile` copies from `kernel/include/`. But user libc headers are separate — check `libc/Makefile` for header install.

If `libc/Makefile` has an `install-headers` target, add `include/sys/futex.h` to it. If not (headers are copied manually), note that the file under `libc/include/sys/futex.h` is already in the right place for libc builds.

- [ ] **Step 4: Build-check**

```bash
make 2>&1 | tail -10
```

Expected: compiles clean.

- [ ] **Step 5: Commit**

```bash
git add libc/include/sys/syscall.h libc/include/sys/futex.h
git commit -m "feat: add SYS_futex userspace wrapper

Defines SYS_futex=47 in syscall numbers and futex(uaddr, op, val)
inline wrapper in sys/futex.h with FUTEX_WAIT/FUTEX_WAKE constants."
```

---

### Task 6: pthread_mutex_t userspace implementation

**Files:**
- Create: `libc/include/pthread.h`
- Create: `libc/pthread/mutex.c` (creates `libc/pthread/` directory)
- Modify: `libc/Makefile` (add `$(wildcard pthread/*.c)`)

**Interfaces:**
- Consumes: `futex()` from `sys/futex.h`
- Produces: `pthread_mutex_t` (int: 0=free, 1=locked, 2=locked+waiters), 5 API functions

- [ ] **Step 1: Add pthread wildcard to libc/Makefile**

Around line 28 (after `$(wildcard unistd/*.c)`), add:

```makefile
    $(wildcard pthread/*.c) \
```

- [ ] **Step 2: Create `libc/include/pthread.h`**

```c
#ifndef _PTHREAD_H
#define _PTHREAD_H

typedef int pthread_mutex_t;
// 0 = unlocked
// 1 = locked, no waiters
// 2 = locked, with waiters

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

#endif
```

- [ ] **Step 3: Create directory and write `libc/pthread/mutex.c`**

```bash
mkdir -p libc/pthread
```

```c
#include <pthread.h>
#include <sys/futex.h>
#include <errno.h>

// atomic_xchg for userspace (inline asm, no kernel dependency)
static inline int atomic_xchg32(volatile int *ptr, int val)
{
    __asm__ __volatile__(
        "xchgl %0, %1"
        : "+r"(val), "+m"(*ptr)
        :
        : "memory"
    );
    return val;  // returns old value after xchg
}

static inline int atomic_cas32(volatile int *ptr, int old, int new)
{
    int ret = old;
    __asm__ __volatile__(
        "lock cmpxchgl %2, %1"
        : "=a"(ret), "+m"(*ptr)
        : "r"(new), "0"(old)
        : "memory"
    );
    return ret;
}

int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    *m = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    // Fast path: CAS 0→1
    if (atomic_cas32(m, 0, 1) == 0)
        return 0;

    // Slow path: set to 2 (has-waiters), block if still contended
    do {
        int old = atomic_xchg32(m, 2);
        if (old == 0)
            return 0;   // acquired (owner=2 is fine)
        futex(m, FUTEX_WAIT, 2);
    } while (1);
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    // Fast path: CAS 1→0 — no waiters
    if (atomic_cas32(m, 1, 0) == 1)
        return 0;

    // Slow path: there are (or were) waiters
    __atomic_store_n(m, 0, __ATOMIC_RELEASE);
    // CAS failure means owner was 2 or a concurrent locker CAS'd 0→1
    // between our failure and the store.  Wake is spurious in the
    // latter case (harmless — Linux standard behaviour).
    futex(m, FUTEX_WAKE, 1);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    return atomic_cas32(m, 0, 1) == 0 ? 0 : EBUSY;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}
```

- [ ] **Step 4: Build-check**

```bash
make clean
make 2>&1 | tail -20
```

Expected: compiles clean (libc builds .a, kernel .bin).

- [ ] **Step 5: Quick sanity test (run in QEMU)**

```bash
make run 2>&1 | head -5
```

Expected: OS boots to shell (pthread_mutex_t is not used yet by any program, so boot should be unchanged).

- [ ] **Step 6: Commit**

```bash
git add libc/include/pthread.h libc/pthread/mutex.c libc/Makefile
git commit -m "feat: add pthread_mutex_t userspace implementation

Three-state futex mutex: 0=free, 1=locked-no-waiters, 2=locked-with-waiters.
Fast path uses atomic CAS; slow path uses atomic_xchg + futex(WAIT/WAKE)."
```

---

### Task 7: Smoke test — kernel mutex selftest

**Files:**
- Create: `kernel/test/test_mutex.c`

**Interfaces:**
- Consumes: `mutex_t`, `kernel_thread` (existing create_kthread), `selftest` framework if available

Note: if no selftest framework exists yet, this task writes a simple inline test that runs in `kernel/kernel/main.c` gated by `#ifdef OS01_SELFTEST`.

- [ ] **Step 1: Check if selftest framework exists**

Look at `kernel/test/` and `kernel/subsys/` for selftest registration macros. If `OS01_SELFTEST` is already used in main.c (line 279), follow that pattern.

- [ ] **Step 2: Write kernel mutex selftest**

```c
#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <kernel/task.h>

static mutex_t test_mtx;
static volatile int shared_counter = 0;

static uint64_t mutex_test_thread(uint64_t arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&test_mtx);
        shared_counter++;
        mutex_unlock(&test_mtx);
    }
    return 0;
}

void test_kernel_mutex(void)
{
    mutex_init(&test_mtx);
    shared_counter = 0;

    // Spawn 2 kernel threads that increment under lock
    create_kthread(mutex_test_thread, 0, "mutex-test-1");
    create_kthread(mutex_test_thread, 0, "mutex-test-2");

    // Wait for completion (spin-wait with yield)
    while (shared_counter < 2000) {
        schedule();
    }

    color_printk(GREEN, BLACK, "[selftest] kernel mutex: PASS (counter=%d)\n",
                 shared_counter);
}
```

- [ ] **Step 3: Register test in init sequence (if using OS01_SELFTEST)**

Or just call it from `main.c` under the `#ifdef OS01_SELFTEST` block.

- [ ] **Step 4: Build + run**

```bash
make clean && make KERNEL_SELFTEST=1 run 2>&1 | tail -30
```

Expected: "PASS" message, no hangs, no crash.

- [ ] **Step 5: Commit**

```bash
git add kernel/test/test_mutex.c kernel/kernel/main.c
git commit -m "test: add kernel mutex SMP selftest

Two kernel threads increment a shared counter under mutex_lock 1000
times each, verifying mutual exclusion."
```
