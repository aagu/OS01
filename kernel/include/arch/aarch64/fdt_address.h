#ifndef _ARCH_AARCH64_FDT_ADDRESS_H
#define _ARCH_AARCH64_FDT_ADDRESS_H

#include <stdbool.h>
#include <stdint.h>

struct aarch64_fdt_bus {
    const uint8_t *ranges;
    uint32_t ranges_len;
    uint32_t child_cells;
    uint32_t parent_cells;
    uint32_t size_cells;
    bool has_ranges;
};

static inline uint32_t aarch64_fdt_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static inline uint64_t aarch64_fdt_decode_cells(const uint8_t *p, uint32_t cells)
{
    if (cells == 2u)
        return ((uint64_t)aarch64_fdt_be32(p) << 32) | aarch64_fdt_be32(p + 4);
    return (uint64_t)aarch64_fdt_be32(p);
}

/* Translate a child bus interval [*address, *address + size) one bus level
 * up through `bus`.  Missing `ranges` (!has_ranges) means no mapping (false);
 * empty `ranges` (has_ranges && ranges_len == 0) is an identity mapping (true).
 * Requires full span coverage by a single ranges entry and checks parent-side
 * addition overflow. */
static inline bool aarch64_fdt_translate_reg_step(
    const struct aarch64_fdt_bus *bus, uint64_t size, uint64_t *address)
{
    uint32_t stride;
    uint32_t off;

    if (!bus || !address || size == 0u)
        return false;
    if (size > UINT64_MAX - *address)
        return false;
    if (!bus->has_ranges)
        return false;
    if (bus->ranges_len == 0u)
        return true;
    if (!bus->ranges)
        return false;
    if ((bus->child_cells != 1u && bus->child_cells != 2u) ||
        (bus->parent_cells != 1u && bus->parent_cells != 2u) ||
        (bus->size_cells != 1u && bus->size_cells != 2u))
        return false;

    stride = (bus->child_cells + bus->parent_cells + bus->size_cells) * 4u;
    if (bus->ranges_len % stride != 0u)
        return false;

    for (off = 0u; off < bus->ranges_len; off += stride) {
        const uint8_t *entry = bus->ranges + off;
        uint64_t child = aarch64_fdt_decode_cells(entry, bus->child_cells);
        uint64_t parent = aarch64_fdt_decode_cells(
            entry + bus->child_cells * 4u, bus->parent_cells);
        uint64_t rsize = aarch64_fdt_decode_cells(
            entry + (bus->child_cells + bus->parent_cells) * 4u,
            bus->size_cells);
        uint64_t delta;

        if (*address < child)
            continue;
        delta = *address - child;
        if (delta >= rsize || size > rsize - delta)
            continue;
        if (parent > UINT64_MAX - delta)
            return false;
        if (size > UINT64_MAX - (parent + delta))
            return false;
        *address = parent + delta;
        return true;
    }

    return false;
}

#endif
