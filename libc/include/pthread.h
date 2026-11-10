#ifndef _PTHREAD_H
#define _PTHREAD_H

typedef int pthread_mutex_t;

#ifdef __cplusplus
extern "C" {
#endif

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

#ifdef __cplusplus
}
#endif

#endif
