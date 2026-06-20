#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H

#include <sys/cdefs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tms {
    uint64_t tms_utime;  /* user time */
    uint64_t tms_stime;  /* system time */
    uint64_t tms_cutime; /* user time of children */
    uint64_t tms_cstime; /* system time of children */
};

uint64_t times(struct tms *buf);

#ifdef __cplusplus
}
#endif

#endif
