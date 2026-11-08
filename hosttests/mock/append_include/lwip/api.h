#ifndef OS01_APPEND_LWIP_API_H
#define OS01_APPEND_LWIP_API_H

/* kernel/fs/file.c includes "lwip/api.h" by name.  See lwip/err.h —
 * the netconn/netbuf surface lives in the force-included
 * vfs_append_runtime.h. */
#include "vfs_append_runtime.h"

#endif /* OS01_APPEND_LWIP_API_H */
