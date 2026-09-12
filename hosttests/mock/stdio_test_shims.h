#ifndef _STDIO_TEST_SHIMS_H
#define _STDIO_TEST_SHIMS_H 1

#include <stddef.h>
#include <sys/types.h>

/* Scripted behaviour for the next (armed) write() call made by the code under
 * test. Used to exercise write_all() short-write / EINTR / zero / error paths. */
typedef enum {
    WRITE_MODE_NORMAL = 0,
    WRITE_MODE_SHORT,   /* return a short count once */
    WRITE_MODE_EINTR,   /* return -1, errno=EINTR once */
    WRITE_MODE_ZERO,    /* return 0 while bytes remain once */
    WRITE_MODE_ERROR    /* return -1, errno=EIO once */
} write_mode_t;

typedef struct {
    int   fd;
    int   len;
    char  data[8192];
} write_rec_t;

void            shim_write_reset(void);
void            shim_write_push(write_mode_t mode, int short_len);
int             shim_write_count(void);
const write_rec_t *shim_write_rec(int idx);
long            shim_write_total_bytes(void);

#endif /* _STDIO_TEST_SHIMS_H */
