/* hosttests/mock/arch9/usb_msc_mutex.h — glibc pthread_mutex_t wrapper for usb_msc host tests */
#ifndef ARCH9_USB_MSC_MUTEX_H
#define ARCH9_USB_MSC_MUTEX_H

#include <stdint.h>
#include <stddef.h>

#define _KERNEL_MUTEX_H 1

extern int pthread_mutex_init(void *mutex, const void *attr);
extern int pthread_mutex_lock(void *mutex);
extern int pthread_mutex_unlock(void *mutex);

typedef struct {
    uint64_t opaque[8];
} mutex_t;

static inline void mutex_init(mutex_t *m)
{
    if (m)
        pthread_mutex_init(m->opaque, NULL);
}

static inline void mutex_lock(mutex_t *m)
{
    if (m)
        pthread_mutex_lock(m->opaque);
}

static inline void mutex_unlock(mutex_t *m)
{
    if (m)
        pthread_mutex_unlock(m->opaque);
}

#endif /* ARCH9_USB_MSC_MUTEX_H */
