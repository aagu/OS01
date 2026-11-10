#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long          ssize_t;
#if defined(__PTRDIFF_MAX__) && !defined(__cplusplus)
/* <limits.h>'s SSIZE_MAX is __PTRDIFF_MAX__; the pairing with the
 * ssize_t width is pinned here, where both sides are in scope (limits.h
 * cannot do it itself: sys/types.h includes stdint.h, which includes
 * limits.h BEFORE this typedef is reached). */
_Static_assert(sizeof(ssize_t) == sizeof(__PTRDIFF_MAX__),
               "SSIZE_MAX (__PTRDIFF_MAX__) must match the ssize_t width");
#endif
typedef long          off_t;
typedef unsigned int  mode_t;
typedef int           pid_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int  nlink_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
typedef long          time_t;

/* For select() */
typedef struct {
    long __fds_bits[16];
} fd_set;

#ifdef __cplusplus
}
#endif

#endif
