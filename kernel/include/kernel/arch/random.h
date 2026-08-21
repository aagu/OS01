#ifndef _ARCH_RANDOM_H
#define _ARCH_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/random.h>
#elif defined(__aarch64__)
// aarch64 stub: no RDRAND/RDSEED.  Always returns false — the caller falls
// back to arch_cycle_counter().  Future work: RNDR/RNDRRS registers.
static inline bool rdrand64(uint64_t *out) { (void)out; return false; }
static inline bool rdseed64(uint64_t *out) { (void)out; return false; }
#else
#error "Unsupported architecture"
#endif

#endif // _ARCH_RANDOM_H
