#ifndef _ARCH_SYSCALL_H
#define _ARCH_SYSCALL_H
#include <stdint.h>
int64_t arch_syscall_putchar(uint64_t ch);
__attribute__((noreturn)) void arch_syscall_reboot(int cmd);
/* Terminate the calling task with an already-encoded exit status.
 * The status carries the user exit code in the high byte, exactly the
 * encoding waitpid() decodes.  Never returns: the architecture owns the
 * task teardown (x86 writes current->exit_code and calls do_exit()). */
__attribute__((noreturn)) void arch_syscall_exit(uint64_t status);
/* Frame-dependent process operations: only architecture code interprets
 * arch_frame. Exec parameters have already been copied into kernel memory.
 * Sigreturn returns zero after restoring the frame, or a negative error. */
/* AArch64 subset: the four anonymous-VM syscalls (brk/mmap/mprotect/
 * munmap) are executed inside an authorized, detached-root edit
 * transaction.  Defined in arch/aarch64/memory/user_vm.c; only the
 * aarch64 branch of sys_mm.c calls it. */
int64_t arch_syscall_user_vm(uint64_t nr, const uint64_t args[6]);

int64_t arch_syscall_fork(void *arch_frame);
int64_t arch_syscall_exec(void *arch_frame, const char *path,
                          const char *const *argv, const char *const *envp);
int64_t arch_syscall_sigreturn(void *arch_frame);
#endif
