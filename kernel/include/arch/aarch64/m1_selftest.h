#ifndef OS01_AARCH64_M1_SELFTEST_H
#define OS01_AARCH64_M1_SELFTEST_H
#include <stdint.h>
#include <stdbool.h>
int aarch64_m1_smoke_cleanup(uint64_t *root,uint64_t data_pa);
int aarch64_m1_selftest(uint64_t root_pa);
int aarch64_m1_probe_prepare(void);
int aarch64_m1_ap_verify(unsigned int cpu);
void aarch64_m1_probe_finish(bool all_requested_acked);
void aarch64_m1_prune_warm(void);
/* Enumerate the exact PoC publication ranges; caller uses CTR_EL0 line size. */
void aarch64_m1_publish_ranges(void (*clean)(uint64_t,uint64_t,uint64_t),uint64_t line_size);
#endif
