#include <errno.h>
#include <stdio.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>

int main(void)
{
    syscall_ctx_t ctx = { .nr = UINT64_MAX };
    if (syscall_has_handler(SYS_getpeername) || syscall_has_handler(UINT64_MAX))
        return 1;
    if (syscall_dispatch(&ctx) != -EINVAL)
        return 1;
    puts("syscall dispatch: PASS");
    return 0;
}
