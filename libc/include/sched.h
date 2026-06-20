#ifndef _SCHED_H
#define _SCHED_H

#include <sys/types.h>
#include <unistd.h>

/* CPU affinity set type */
#define __CPU_SETSIZE 1024
#define __NCPUBITS (8 * sizeof(unsigned long))

typedef struct {
    unsigned long __bits[__CPU_SETSIZE / __NCPUBITS];
} cpu_set_t;

#define CPU_SET(cpu, cpusetp) \
    ((cpusetp)->__bits[(cpu) / __NCPUBITS] |= (1UL << ((cpu) % __NCPUBITS)))
#define CPU_ZERO(cpusetp) \
    do { int __i; for (__i = 0; __i < (int)(sizeof((cpusetp)->__bits) / sizeof(*(cpusetp)->__bits)); __i++) (cpusetp)->__bits[__i] = 0; } while (0)
#define CPU_ISSET(cpu, cpusetp) \
    (((cpusetp)->__bits[(cpu) / __NCPUBITS] & (1UL << ((cpu) % __NCPUBITS))) != 0)
#define CPU_CLR(cpu, cpusetp) \
    ((cpusetp)->__bits[(cpu) / __NCPUBITS] &= ~(1UL << ((cpu) % __NCPUBITS)))
#define CPU_AND(dst, src1, src2) \
    do { int __i; for (__i = 0; __i < (int)(sizeof((dst)->__bits) / sizeof(*(dst)->__bits)); __i++) (dst)->__bits[__i] = (src1)->__bits[__i] & (src2)->__bits[__i]; } while (0)
#define CPU_OR(dst, src1, src2) \
    do { int __i; for (__i = 0; __i < (int)(sizeof((dst)->__bits) / sizeof(*(dst)->__bits)); __i++) (dst)->__bits[__i] = (src1)->__bits[__i] | (src2)->__bits[__i]; } while (0)
#define CPU_XOR(dst, src1, src2) \
    do { int __i; for (__i = 0; __i < (int)(sizeof((dst)->__bits) / sizeof(*(dst)->__bits)); __i++) (dst)->__bits[__i] = (src1)->__bits[__i] ^ (src2)->__bits[__i]; } while (0)
#define CPU_EQUAL(cpusetp1, cpusetp2) \
    (memcmp((cpusetp1), (cpusetp2), sizeof(cpu_set_t)) == 0)
#define CPU_COUNT(cpusetp) \
    ({ int __i, __c = 0; for (__i = 0; __i < (int)(sizeof((cpusetp)->__bits) / sizeof(*(cpusetp)->__bits)); __i++) { unsigned long __b = (cpusetp)->__bits[__i]; while (__b) { __c++; __b &= (__b - 1); } } __c; })

/* Scheduling policies */
#define SCHED_NORMAL    0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5

/* Function declarations */
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int sched_yield(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_setscheduler(pid_t pid, int policy, const void *param);
int sched_getscheduler(pid_t pid);

#endif /* _SCHED_H */
