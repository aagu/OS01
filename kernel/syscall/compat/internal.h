#ifndef _SYSCALL_COMPAT_INTERNAL_H
#define _SYSCALL_COMPAT_INTERNAL_H

#include <syscall/compat.h>
#include <uapi/syscall.h>

extern const compat_syscall_nr_t linux_x86_64_table[LINUX_X86_64_NR_MAX];

/* Domain adapters */
int64_t compat_sys_rt_sigaction(syscall_ctx_t *ctx);
int64_t compat_sys_wait4(syscall_ctx_t *ctx);

#endif /* _SYSCALL_COMPAT_INTERNAL_H */
