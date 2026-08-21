#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stddef.h>

// Linux urandom per-call ceiling; larger requests are truncated, not errored.
#define RANDOM_MAX_LEN 33554431UL

void random_init(void);                     // boot-time, once (BSP)
void get_random_bytes(void *buf, size_t len); // any context (IRQ-safe)

#endif // _KERNEL_RANDOM_H
