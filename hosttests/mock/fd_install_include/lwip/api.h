#ifndef OS01_FD_INSTALL_LWIP_API_H
#define OS01_FD_INSTALL_LWIP_API_H

/* kernel/fs/file.c includes "lwip/api.h" by name.  For the ARCH-10
 * Task 6 host test the lwIP surface comes from the force-included
 * fd_install_test_runtime.h, so this header only forwards to it and
 * keeps the two views of struct netconn / struct netbuf identical. */
#include "fd_install_test_runtime.h"

#endif /* OS01_FD_INSTALL_LWIP_API_H */
