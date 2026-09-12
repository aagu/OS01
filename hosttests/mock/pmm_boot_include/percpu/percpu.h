#ifndef TEST_PMM_BOOT_PERCPU_H
#define TEST_PMM_BOOT_PERCPU_H
#include <stdint.h>
/* Boot-only test: runtime slab locking/per-CPU paths are discarded at link. */
#define NR_CPUS 1
extern struct test_percpu { int online; } percpu_data[NR_CPUS];
static inline uint32_t cpu_id(void) { return 0; }
#endif
