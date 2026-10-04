#include <errno.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>
typedef struct syscall_entry {
    syscall_handler_t handler;
    const char *name;
} syscall_entry_t;
/* Handlers are added here as syscall families migrate out of trap.c. */
static const syscall_entry_t syscall_table[SYS_fstatat + 1] = { 0 };
bool syscall_has_handler(uint64_t nr)
{
    return nr < (uint64_t)(sizeof(syscall_table) / sizeof(syscall_table[0])) &&
           syscall_table[nr].handler != 0;
}
int64_t syscall_dispatch(syscall_ctx_t *ctx)
{
    if (!ctx || !syscall_has_handler(ctx->nr))
        return -EINVAL;
    return syscall_table[ctx->nr].handler(ctx);
}
