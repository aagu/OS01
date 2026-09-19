#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stddef.h>
#include <stdbool.h>

struct boot_context;

// Linux urandom per-call ceiling; larger requests are truncated, not errored.
#define RANDOM_MAX_LEN 33554431UL

void random_init(const struct boot_context *bootctx);  // boot-time, once (BSP); reads UEFI GetRNG entropy
void get_random_bytes(void *buf, size_t len);          // any context (IRQ-safe); fail-closed if !random_ready
bool random_is_ready(void);                            // pool health probe (selftests, /dev/random read path)
/* Pool mix entry point — feeds userland /dev/random writes into the
 * pool. Called with the caller's data buffer + length. No-op until
 * random_init() succeeds; concurrent callers serialize on pool_lock. */
void random_add_entropy(const void *data, size_t len);

#endif // _KERNEL_RANDOM_H
