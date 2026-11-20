#include <pthread.h>
#include <sys/futex.h>
#include <errno.h>
#include <libc_arch/atomic.h>

int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    *m = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (libc_atomic_cas32(m, 0, 1) == 0)
        return 0;

    do {
        int old = libc_atomic_xchg32(m, 2);
        if (old == 0)
            return 0;
        futex(m, FUTEX_WAIT, 2);
    } while (1);
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (libc_atomic_cas32(m, 1, 0) == 1)
        return 0;

    libc_atomic_store32_release(m, 0);
    futex(m, FUTEX_WAKE, 1);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    return libc_atomic_cas32(m, 0, 1) == 0 ? 0 : EBUSY;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}
