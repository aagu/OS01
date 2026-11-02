#ifndef _KERNEL_SYSCALL_RESOURCE_H
#define _KERNEL_SYSCALL_RESOURCE_H

#include <stdint.h>
#include <syscall/dispatch.h>

int64_t sys_resource_dispatch(syscall_ctx_t *ctx);
int64_t compat_sys_prlimit64(syscall_ctx_t *ctx);

#endif /* _KERNEL_SYSCALL_RESOURCE_H */
