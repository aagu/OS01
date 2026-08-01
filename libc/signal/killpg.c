#include <sys/types.h>   /* pid_t (signal.h references it without including) */
#include <signal.h>
#include <errno.h>

int killpg(pid_t pgrp, int sig)
    { (void)pgrp; (void)sig; errno = ENOSYS; return -1; }
