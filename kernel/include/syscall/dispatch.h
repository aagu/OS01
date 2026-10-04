#ifndef _SYSCALL_DISPATCH_H
#define _SYSCALL_DISPATCH_H
#include <stdbool.h>
#include <stdint.h>
typedef struct syscall_ctx {
    uint64_t nr;
    uint64_t args[6];
    void *arch_frame;
    bool suppress_writeback;
} syscall_ctx_t;
typedef int64_t (*syscall_handler_t)(syscall_ctx_t *ctx);
bool syscall_has_handler(uint64_t nr);
int64_t syscall_dispatch(syscall_ctx_t *ctx);
#endif
