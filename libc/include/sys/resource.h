#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdint.h>

#define RLIMIT_CPU   0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA  2
#define RLIMIT_STACK 3
#define RLIMIT_CORE  4
#define RLIMIT_RSS   5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS    9
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE  13
#define RLIMIT_RTPRIO 14
#define RLIMIT_SIGPENDING 15

#define RLIM_INFINITY ((unsigned long)-1)

typedef unsigned long rlim_t;

struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

#ifndef _OS01_RLIMIT64_DEFINED
#define _OS01_RLIMIT64_DEFINED
struct os01_rlimit64 {
    uint64_t cur;
    uint64_t max;
};
#endif

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#endif /* _SYS_RESOURCE_H */
