# Futex-based Mutex Design

**Date:** 2026-07-12
**Status:** Draft

## Overview

Add a futex-like synchronization primitive to OS01, providing both a kernel-internal
sleepable mutex (`mutex_t`) and a userspace mutex (`pthread_mutex_t`) backed by a
single `SYS_futex` syscall. The underlying blocking mechanism reuses the existing
`wait_queue_t` infrastructure.

## Architecture

```
 Userspace                     Kernel
 ─────────                     ──────
 pthread_mutex_lock()          
   CAS 0→1 (fast path)         
   CAS 1→2 → SYS_futex(WAIT)  → futex_wait() → wait_queue_sleep() → schedule()
                               
 pthread_mutex_unlock()        
   CAS 1→0 (fast, no waiters) 
   atomic 2→0 → SYS_futex(WAKE) → futex_wake() → dequeue loop (bucket lock)
                               
                               Kernel mutex:
                               mutex_lock() → CAS + wait_queue_sleep()
                               mutex_unlock() → atomic clear + wait_queue_wake_one()
```

Both paths share the same `wait_queue_t` primitive; the futex adds a hash-table
lookup to map (address space, user address) → wait queue.

## 1. Kernel mutex

### Header: `kernel/include/kernel/mutex.h`

```c
typedef struct {
    volatile int64_t owner;  // 0=free, >0=holder PID (matches task_t.pid type)
    wait_queue_t wq;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_trylock(mutex_t *m);       // returns 1 on success
void mutex_unlock(mutex_t *m);
int  mutex_lock_interruptible(mutex_t *m);  // returns -EINTR on signal
```

### Implementation: `kernel/mutex.c`

- **mutex_lock**: atomic CAS `owner 0→current->pid`. If fails,
  `wait_queue_sleep(&m->wq)`, then retry. Loop until acquired.
- **mutex_trylock**: single CAS, return 1/0.
- **mutex_unlock**: atomic store 0 to `owner`, then `wait_queue_wake_one(&m->wq)`.
  No external lock needed — `wait_queue_wake_one` is self-locking via IRQ-safe spinlock.
- **mutex_lock_interruptible**: Checks `current->signal` before sleeping and after
  each wakeup. If signal pending while not owning the lock, returns `-EINTR`.
  The signal delivery is handled by `do_signal_delivery` on syscall return.
  ```c
   int mutex_lock_interruptible(mutex_t *m) {
       while (atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) != 0) {
           if (current->signal)
               return -EINTR;
           wait_queue_sleep(&m->wq);
           if (current->signal)
               return -EINTR;
       }
       return 0;
   }
  ```

### Constraints

- Must be called from process context (can schedule). Not safe in IRQ handlers.
- unlock is IRQ-safe (can wake waiters from IRQ context).
- No recursion — locking an already-held mutex will deadlock (as designed).

## 2. Futex

### Syscall: `SYS_futex` (number 47)

```c
long futex(int *uaddr, int op, int val);
// Returns 0 on success, -errno on error
// FUTEX_WAIT: 0 if woken normally, -EAGAIN if *uaddr != val, -EINTR if interrupted
// FUTEX_WAKE: number of tasks woken (may be less than val)
```

### Operations

| op | Description |
|----|-------------|
| `FUTEX_WAIT (0)` | If `*uaddr == val`, block. Otherwise return -EAGAIN. |
| `FUTEX_WAKE (1)` | Wake up to `val` waiters waiting on `uaddr`. Returns count. |

### Hash table: `kernel/futex.c`

- Fixed 64 buckets, no dynamic allocation.
- Each bucket: `{ spinlock_T lock; wait_queue_t wq; }`
- Key derivation:
  ```
  key = (uint64_t)mm->pml4 ^ (PAGE_4K_ALIGN(uaddr) >> 12)
  bucket = key & 63
  (简单 identity hash; 64 桶下碰撞率可接受; 后续可升级为 jhash)
  ```
- `mm->pml4` is the physical address of the process PML4 — unique per address space.
- `uaddr` is page-aligned before hashing so multiple futex words on the same page
  share a bucket (Linux behaviour).

### FUTEX_WAIT flow

1. Validate `uaddr` is in userspace range (`< addr_limit`) and 4-byte aligned.
2. Pin bucket lock (IRQ-save).
3. Walk the user page table via `user_va_to_phys()` to locate the physical page
   backing `uaddr`. If the page is not present → unlock bucket, return `-EFAULT`.
4. Read `*uaddr` through the kernel mapping:
   ```
   phys = user_va_to_phys(user_pml4, PAGE_4K_ALIGN(uaddr));
   kaddr = Phy_To_Virt(phys) + (offset_in_page(uaddr));
   futex_val = *(volatile int *)kaddr;
   ```
5. If `futex_val != val`: unlock bucket, return `-EAGAIN`.
6. Add current task to bucket's wait queue — link `io_wait_node` into
   `bucket->wq.head` while holding bucket lock.
7. Set `TASK_INTERRUPTIBLE` — still holding bucket lock, so a concurrent
   WAKE cannot see RUNNING before we're truly about to sleep.
8. Unlock bucket (IRQ-restore).
9. `schedule()` — since we're already INTERRUPTIBLE, any WAKE that
   acquired the bucket lock after step 8 has already set us RUNNING,
   so schedule() returns immediately.
10. After `schedule()` returns:
   a. If `io_wait_node` is still linked (woken by signal, not by FUTEX_WAKE):
      Lock bucket, remove self from wq list, unlock bucket.
   b. If `current->signal` is pending: return `-EINTR`.
   c. Otherwise: return 0.

### FUTEX_WAKE flow

1. Validate `uaddr` is in userspace range.
2. Pin bucket lock (IRQ-save).  Bucket lock serialises all futex operations
   on this address; the wq's internal lock is not needed.
3. Open-code wake loop: while `woken < val` and `wq.head` not empty, dequeue
   one task from `bucket->wq.head`, set its state to `TASK_RUNNING`, increment
   `woken`.
4. Unlock bucket, return `woken`.

### Address safety

- `uaddr` validated: must be in userspace range (`< addr_limit`), 4-byte aligned.
- `user_va_to_phys()` + `Phy_To_Virt()` used instead of direct dereference
  (the page may be swapped or not yet allocated, and kernel-mode #PF halts).

### Header: `kernel/include/kernel/futex.h`

```c
void futex_init(void);   // called during kernel init
int do_futex_wait(int *uaddr, int val);
int do_futex_wake(int *uaddr, int val);
```

## 3. Userspace mutex

### Header: `libc/include/pthread.h`

```c
typedef int pthread_mutex_t;
// 0 = unlocked
// 1 = locked, no waiters
// 2 = locked, with waiters

int pthread_mutex_init(pthread_mutex_t *m, void *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);
```

### Implementation: `libc/pthread/mutex.c`

**pthread_mutex_lock:**
```
  atomic CAS 0→1:
    success → return (fast path)
  do:
    // Set to 2 (has-waiters) to tell unlocker to WAKE
    old = atomic_exchange(m, 2)
    if old == 0:
      return   // we got it (owner=2 is fine — unlock does 2→0 + WAKE)
    futex(WAIT, m, 2)   // *m should be 2; if not, returns -EAGAIN
  while (1)
```

**pthread_mutex_unlock:**
```
  atomic CAS 1→0:
    success → return (fast path, no waiters)
  // there are (or were) waiters
  atomic store 0
  // CAS fails → owner was 1 or 2.  If it was 2 there are waiters.
  // If it was 1 but a concurrent locker CAS'd 0→1 between our CAS
  // failure and the store, WAKE becomes a benign spurious wakeup
  // (Linux standard behaviour — no lost wakeups possible).
  futex(WAKE, m, 1)
```

### Wrapper: `libc/include/sys/futex.h`

```c
static inline int futex(int *uaddr, int op, int val) {
    return (int)syscall(SYS_futex, (uint64_t)uaddr, (uint64_t)op, (uint64_t)val);
}
```

## 4. Integration

### New files

| File | Purpose |
|------|---------|
| `kernel/include/kernel/mutex.h` | Kernel mutex type + API |
| `kernel/mutex.c` | Kernel mutex implementation |
| `kernel/include/kernel/futex.h` | Futex internal API |
| `kernel/futex.c` | Futex hash table + wait/wake logic |
| `libc/include/pthread.h` | pthread_mutex_t definition |
| `libc/include/sys/futex.h` | futex() syscall wrapper |
| `libc/pthread/mutex.c` | pthread_mutex implementation |

### Modified files

| File | Change |
|------|--------|
| `kernel/include/uapi/futex.h` | Shared futex constants (`FUTEX_WAIT=0`, `FUTEX_WAKE=1`) |
| `kernel/intr/wait.c` | Fix `wait_queue_sleep` SMP lost-wakeup: set `TASK_INTERRUPTIBLE` before releasing wq lock |
| `kernel/arch/x86_64/trap.c` | `#include <uapi/futex.h>`; add `SYS_futex` case; add `[47] = "futex"` to `syscall_names[]` |
| `libc/include/sys/syscall.h` | Add `#define SYS_futex 47` |
| `kernel/Makefile` | Add `mutex.c` and `futex.c` to `SRC` |
| `libc/Makefile` | Add `$(wildcard pthread/*.c)` to `C_SOURCES` |
| `kernel/kernel/main.c` | Call `futex_init()` after `subsys_init_all()`, before `task_init()` |

### Syscall dispatch (trap.c)

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

## 5. Testing

- Kernel mutex: exercised by a kernel thread that locks/unlocks in a loop
  across 2+ CPUs.
- Futex: user-space test program spawning 2+ threads that contend on a
  `pthread_mutex_t` incrementing a shared counter (no corruption, count
  reaches expected value).
- Signal interruptibility: test that `SYS_kill(SIGINT)` breaks a
  `mutex_lock_interruptible` or `pthread_mutex_lock` wait.

## 6. Future work (out of scope)

- `FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE` (for `pthread_cond_broadcast`)
- `pthread_cond_t` using futex
- `pthread_rwlock_t`
- `FUTEX_WAKE_OP` (Linux optimisation)
- Priority inheritance (PI futex)

## Files

- `kernel/include/kernel/mutex.h`
- `kernel/mutex.c`
- `kernel/include/kernel/futex.h`
- `kernel/futex.c`
- `libc/include/pthread.h`
- `libc/include/sys/futex.h`
- `libc/pthread/mutex.c`
