#ifndef _ARCH_AARCH64_UEFI_MEMORY_MAP_H
#define _ARCH_AARCH64_UEFI_MEMORY_MAP_H

#include <stdbool.h>
#include <stdint.h>

#define AARCH64_UEFI_MEM_DESC_MIN_SIZE 40u
#define AARCH64_UEFI_PAGE_SHIFT        12u

/* UEFI memory types (fixed specification values). */
#define AARCH64_UEFI_MEM_RESERVED               0u
#define AARCH64_UEFI_MEM_LOADER_CODE            1u
#define AARCH64_UEFI_MEM_LOADER_DATA            2u
#define AARCH64_UEFI_MEM_BOOT_SERVICES_CODE     3u
#define AARCH64_UEFI_MEM_BOOT_SERVICES_DATA     4u
#define AARCH64_UEFI_MEM_RUNTIME_SERVICES_CODE  5u
#define AARCH64_UEFI_MEM_RUNTIME_SERVICES_DATA  6u
#define AARCH64_UEFI_MEM_CONVENTIONAL           7u
#define AARCH64_UEFI_MEM_UNUSABLE               8u
#define AARCH64_UEFI_MEM_ACPI_RECLAIM           9u
#define AARCH64_UEFI_MEM_ACPI_NVS               10u
#define AARCH64_UEFI_MEM_MMIO                   11u
#define AARCH64_UEFI_MEM_MMIO_PORT_SPACE        12u
#define AARCH64_UEFI_MEM_PAL_CODE               13u

/* UEFI memory attributes (fixed specification values). */
#define AARCH64_UEFI_ATTR_WB UINT64_C(0x0000000000000008)
#define AARCH64_UEFI_ATTR_RP UINT64_C(0x0000000000002000)

struct aarch64_uefi_mem_desc {
    uint32_t type;
    uint64_t phys_start;
    uint64_t num_pages;
    uint64_t attribute;
};

static inline uint32_t aarch64_uefi_read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t aarch64_uefi_read_u64(const uint8_t *p)
{
    return (uint64_t)aarch64_uefi_read_u32(p) |
           ((uint64_t)aarch64_uefi_read_u32(p + 4) << 32);
}

static inline bool aarch64_uefi_decode_desc(
    const void *map, uint32_t entry_size, uint32_t index,
    struct aarch64_uefi_mem_desc *out, uint64_t *end_out)
{
    const uint8_t *p = (const uint8_t *)map + (uint64_t)index * entry_size;
    uint64_t bytes;

    out->type = aarch64_uefi_read_u32(p + 0);
    out->phys_start = aarch64_uefi_read_u64(p + 8);
    out->num_pages = aarch64_uefi_read_u64(p + 24);
    out->attribute = aarch64_uefi_read_u64(p + 32);

    if (out->num_pages > (UINT64_MAX >> AARCH64_UEFI_PAGE_SHIFT))
        return false;
    bytes = out->num_pages << AARCH64_UEFI_PAGE_SHIFT;
    if (out->phys_start > UINT64_MAX - bytes)
        return false;
    *end_out = out->phys_start + bytes;
    return true;
}

static inline bool aarch64_uefi_desc_is_ram(const struct aarch64_uefi_mem_desc *d)
{
    switch (d->type) {
    case AARCH64_UEFI_MEM_LOADER_CODE:
    case AARCH64_UEFI_MEM_LOADER_DATA:
    case AARCH64_UEFI_MEM_BOOT_SERVICES_CODE:
    case AARCH64_UEFI_MEM_BOOT_SERVICES_DATA:
    case AARCH64_UEFI_MEM_RUNTIME_SERVICES_CODE:
    case AARCH64_UEFI_MEM_RUNTIME_SERVICES_DATA:
    case AARCH64_UEFI_MEM_CONVENTIONAL:
    case AARCH64_UEFI_MEM_ACPI_RECLAIM:
    case AARCH64_UEFI_MEM_ACPI_NVS:
        return true;
    case AARCH64_UEFI_MEM_RESERVED:
        return (d->attribute & AARCH64_UEFI_ATTR_WB) != 0u;
    case AARCH64_UEFI_MEM_MMIO:
    case AARCH64_UEFI_MEM_MMIO_PORT_SPACE:
    default:
        return false;
    }
}

/* Check whether [start, end) overlaps any RAM descriptor in a raw UEFI
 * memory map.  Returns 1 on overlap, 0 on no overlap, or -1 if the map
 * parameters, stride, count*stride, page range, or [start, end) are invalid. */
static inline int aarch64_uefi_range_overlaps_ram(
    const void *map, uint32_t entry_size, uint32_t count,
    uint64_t start, uint64_t end)
{
    bool overlaps = false;

    if (!map || entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE ||
        count == 0u || start >= end)
        return -1;
    if ((uint64_t)count > UINT64_MAX / entry_size)
        return -1;

    for (uint32_t i = 0u; i < count; i++) {
        struct aarch64_uefi_mem_desc desc;
        uint64_t desc_end = 0u;

        if (!aarch64_uefi_decode_desc(map, entry_size, i, &desc, &desc_end))
            return -1;
        if (desc.num_pages == 0u)
            continue;
        if (aarch64_uefi_desc_is_ram(&desc) &&
            desc.phys_start < end && start < desc_end)
            overlaps = true;
    }

    return overlaps ? 1 : 0;
}

#endif
