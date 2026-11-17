#ifndef OS01_AARCH64_M1_SELFTEST_H
#define OS01_AARCH64_M1_SELFTEST_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <memory/memory_map.h>
int aarch64_page_table_smoke_cleanup(uint64_t *root,uint64_t data_pa);
int aarch64_page_table_selftest(uint64_t root_pa);
int aarch64_page_table_probe_prepare(void);
int aarch64_page_table_ap_verify(unsigned int cpu);
void aarch64_page_table_probe_finish(bool all_requested_acked);
/* Pre-switch prune warm-up: selects the M1 prune probe candidate
 * (prefer 0x00200000, otherwise the first M0 2 MiB block outside the
 * configured B/D and the ram ranges about to be installed) and
 * verifies the candidate is still translated by the boot root.
 * Called before the TTBR1 switch with the SAME ram list the M1 tree
 * consumes. */
void aarch64_page_table_prune_warm(const struct MEMORY_RANGE *ram, size_t count);
/* Enumerate the exact PoC publication ranges; caller uses CTR_EL0 line size. */
void aarch64_publish_cache_ranges(void (*clean)(uint64_t,uint64_t,uint64_t),uint64_t line_size);
#endif
