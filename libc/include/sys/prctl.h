#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H 1

#define PR_SET_NAME 15
#define PR_GET_NAME 16

int prctl(int option, ...);
#endif
