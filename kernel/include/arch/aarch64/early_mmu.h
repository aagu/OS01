#ifndef OS01_AARCH64_EARLY_MMU_H
#define OS01_AARCH64_EARLY_MMU_H

#include <stdint.h>

void aarch64_build_boot_page_tables(uint64_t zeroed_tables[6][512],
                                    uint64_t boot_base);

#endif
