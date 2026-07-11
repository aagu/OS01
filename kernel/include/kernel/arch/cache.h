#ifndef _ARCH_CACHE_H
#define _ARCH_CACHE_H

#include <stddef.h>

static inline void arch_flush_dcache(void *addr, size_t len) { (void)addr; (void)len; }
static inline void arch_inval_dcache(void *addr, size_t len) { (void)addr; (void)len; }

#endif
