#ifndef _UAPI_RESOURCE_H
#define _UAPI_RESOURCE_H

#include <stdint.h>

#define OS01_RLIMIT_NOFILE   7
#define OS01_RLIM_INFINITY   UINT64_MAX

#ifndef _OS01_RLIMIT64_DEFINED
#define _OS01_RLIMIT64_DEFINED
struct os01_rlimit64 {
    uint64_t cur;
    uint64_t max;
};
#endif

#endif /* _UAPI_RESOURCE_H */
