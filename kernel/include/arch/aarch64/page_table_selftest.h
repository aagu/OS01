#ifndef OS01_AARCH64_M1_SELFTEST_H
#define OS01_AARCH64_M1_SELFTEST_H
#include <stdint.h>
#include <stdbool.h>
int aarch64_page_table_smoke_cleanup(uint64_t *root,uint64_t data_pa);
int aarch64_page_table_selftest(uint64_t root_pa);
int aarch64_page_table_probe_prepare(void);
int aarch64_page_table_ap_verify(unsigned int cpu);
void aarch64_page_table_probe_finish(bool all_requested_acked);
void aarch64_page_table_prune_warm(void);
/* Enumerate the exact PoC publication ranges; caller uses CTR_EL0 line size. */
void aarch64_publish_cache_ranges(void (*clean)(uint64_t,uint64_t,uint64_t),uint64_t line_size);
#endif
