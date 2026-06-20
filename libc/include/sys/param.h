#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H 1

#include <sys/cdefs.h>
#include <limits.h>

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define MAXSYMLINKS 20
#define NGROUPS_MAX 32
#define NOFILE      256
#define NZERO       20

#endif
