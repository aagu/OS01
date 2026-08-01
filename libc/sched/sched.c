#include <sched.h>         /* includes <unistd.h> -> pid_t, function declarations */
#include <errno.h>

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
    { (void)pid; (void)cpusetsize; (void)mask; errno = ENOSYS; return -1; }

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask)
    { (void)pid; (void)cpusetsize; (void)mask; errno = ENOSYS; return -1; }

int sched_get_priority_max(int policy) { (void)policy; return 0; }
int sched_get_priority_min(int policy) { (void)policy; return 0; }

int sched_setscheduler(pid_t pid, int policy, const void *param)
    { (void)pid; (void)policy; (void)param; return 0; }

int sched_getscheduler(pid_t pid) { (void)pid; return SCHED_NORMAL; }
int sched_yield(void) { return 0; }
