// libc/ssp/ssp.c — 用户态栈 canary runtime（spec 2026-09-17 §6.1）。
//
// libk 构建（-D__is_libk）：本文件为空翻译单元。内核的 guard/fail 在
// kernel/core/main.c:63/77 自有定义；libk.a 归档成员若再带一份，
// 内核链接的归档扫描可能在 main.o 之前拉出重复定义。
#include <sys/ssp.h>
#include <signal.h>
#include <unistd.h>

#if !defined(__is_libk)

unsigned long __stack_chk_guard;

__attribute__((noreturn, cold, no_stack_protector))
void __stack_chk_fail(void)
{
    static const char msg[] = "*** stack smashing detected ***: terminated\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    raise(SIGABRT);   /* SIG_DFL → trap.c:838 do_exit(6) → waitpid status 6 */
    _exit(127);       /* SIGABRT 被 block/ignore 时的兜底（glibc 同款） */
}

#endif /* !__is_libk */