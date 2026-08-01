#include <signal.h>
int sigsuspend(const sigset_t *mask) { (void)mask; return -1; }
