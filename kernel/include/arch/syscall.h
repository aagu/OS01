#ifndef _ARCH_SYSCALL_H
#define _ARCH_SYSCALL_H
#include <stdint.h>
int64_t arch_syscall_putchar(uint64_t ch);
__attribute__((noreturn)) void arch_syscall_reboot(int cmd);
/* Frame-dependent process operations: only architecture code interprets
 * arch_frame. Exec parameters have already been copied into kernel memory.
 * Sigreturn returns zero after restoring the frame, or a negative error. */
int64_t arch_syscall_fork(void *arch_frame);
int64_t arch_syscall_exec(void *arch_frame, const char *path,
                          const char *const *argv, const char *const *envp);
int64_t arch_syscall_sigreturn(void *arch_frame);
#endif
