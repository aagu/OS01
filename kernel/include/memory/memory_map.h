#ifndef _KERNEL_MEMORY_MAP_H
#define _KERNEL_MEMORY_MAP_H

#include <stdint.h>
#include <stddef.h>

#define MEMORY_RANGE_MAX      64u
#define MEMORY_RANGE_GRANULE  (1UL << 21)  /* 2 MiB, matches PAGE_2M_SIZE; MUST be unsigned long so the round-up mask ~(GRANULE-1) is canonical 64-bit (avoids truncating >4 GiB addresses to 32-bit zero-extended mask). */

enum MEMORY_TYPE {
    MEMORY_TYPE_RAM          = 1u,
    MEMORY_TYPE_RESERVED     = 2u,
    MEMORY_TYPE_ACPI_RECLAIM = 3u,
    MEMORY_TYPE_ACPI_NVS     = 4u,
    MEMORY_TYPE_DEVICE       = 5u,
};

struct MEMORY_RANGE {
    uint64_t        phys_start;   /* inclusive, granule-aligned */
    uint64_t        phys_end;     /* exclusive, granule-aligned */
    enum MEMORY_TYPE type;
};

#endif /* _KERNEL_MEMORY_MAP_H */
