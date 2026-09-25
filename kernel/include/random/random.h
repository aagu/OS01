#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stddef.h>
#include <stdbool.h>
#include <arch/random.h>     /* arch_entropy_source_t for random_get_pool_quality() */

struct boot_context;

// Linux urandom per-call ceiling; larger requests are truncated, not errored.
#define RANDOM_MAX_LEN 33554431UL

void random_init(const struct boot_context *bootctx);  // boot-time, once (BSP); reads UEFI GetRNG entropy
void get_random_bytes(void *buf, size_t len);          // any context (IRQ-safe); fail-closed if !random_ready
bool random_is_ready(void);                            // pool health probe (selftests, /dev/random read path)
/* AAGU-5.6: report the quality the pool was last seeded with. Lets kernel
 * callers (notably AT_RANDOM via setup_user_stack) decide whether the
 * pool is STRONG enough to consume, vs falling back to arch hardware. */
arch_entropy_source_t random_get_pool_quality(void);
/* AAGU-5.6: STRONG-only entropy for AT_RANDOM (spec §6). Pool-aware: uses
 * the pool when it was seeded with STRONG quality (e.g. via UEFI GetRNG
 * or hardware RDSEED/RNDRRS), otherwise falls back to arch hardware. */
bool kernel_random_get_strong(uint8_t out[32]);

/* AAGU-5.7: KERNEL_SELFTEST-only hook — let selftests simulate pool state
 * (quality + ready flag) without depending on real entropy sources.
 * Production builds (no OS01_SELFTEST) do not link this symbol.
 * Lets kernel_random_get_strong run unmodified production code, so tests
 * exercise the actual STRONG-only contract rather than mocking it. */
#ifdef OS01_SELFTEST
void kernel_random_test_set_pool_quality(arch_entropy_source_t q, bool ready);
#endif
/* Pool mix entry point — feeds userland /dev/random writes into the
 * pool. Called with the caller's data buffer + length. No-op until
 * random_init() succeeds; concurrent callers serialize on pool_lock. */
void random_add_entropy(const void *data, size_t len);

#endif // _KERNEL_RANDOM_H
