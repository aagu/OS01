#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <sys/types.h>   // size_t, ssize_t

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

#ifdef __cplusplus
extern "C" {
#endif

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif // _SYS_RANDOM_H
