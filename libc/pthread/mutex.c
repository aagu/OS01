#include <pthread.h>
#include <sys/futex.h>
#include <errno.h>

static inline int atomic_xchg32(volatile int *ptr, int val)
{
    __asm__ __volatile__(
        "xchgl %0, %1"
        : "+r"(val), "+m"(*ptr)
        :
        : "memory"
    );
    return val;
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
    if (atomic_cas32(m, 0, 1) == 0)
        return 0;

    do {
        int old = atomic_xchg32(m, 2);
        if (old == 0)
            return 0;
        futex(m, FUTEX_WAIT, 2);
    } while (1);
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (atomic_cas32(m, 1, 0) == 1)
        return 0;

    __atomic_store_n(m, 0, __ATOMIC_RELEASE);
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
