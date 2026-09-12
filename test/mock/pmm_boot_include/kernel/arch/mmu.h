#ifndef TEST_PMM_BOOT_MMU_H
#define TEST_PMM_BOOT_MMU_H
#include <stdint.h>
/* Map synthetic physical RAM into an ordinary host allocation. */
extern uintptr_t test_direct_map_offset;
#define ARCH_PAGE_OFFSET test_direct_map_offset
#endif
