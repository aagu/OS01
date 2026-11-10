#ifndef OS01_APPEND_LWIP_ERR_H
#define OS01_APPEND_LWIP_ERR_H

/* kernel/fs/file.c includes "lwip/err.h" by name.  For the
 * libc-completion T6 host test the lwIP error surface lives in the
 * force-included vfs_append_runtime.h so every translation unit of
 * the host test agrees on the err_t values. */
#include "vfs_append_runtime.h"

#endif /* OS01_APPEND_LWIP_ERR_H */
