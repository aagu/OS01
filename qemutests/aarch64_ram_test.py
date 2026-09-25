#!/usr/bin/env python3
"""Behavioural coverage for the AArch64 UEFI RAM map normalizer.

The runner is a small C program that compiles against the production
`kernel/arch/aarch64/memory/ram_core.c`. Each test case synthesises UEFI
descriptor bytes via bytewise little-endian helpers (no struct
casts) and asserts both the negative-error path and the success-path
invariants — alignment, sortedness, disjointness, and "outside all
exclusions".

The four wire-format constants stay enforced via `_Static_assert`
(Task 1 contract) and the eight-parameter signature stays linkable
against `ram_core.c`. Task 2 adds the behavioural matrix.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


RUNNER = r'''
#include <core/bootinfo.h>
#include <arch/aarch64/ram.h>
/* Public RAM normalization contract. */
#include <arch/aarch64/ram_core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Compile-time contracts pinned by the spec. Any drift between the
 * spec values and the header macros aborts the translation; runtime
 * checks cannot catch a header-only regression because they execute
 * against the in-memory constants, not the preprocessor tokens. */
_Static_assert(AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE == 32u, "wire prefix");
_Static_assert(AARCH64_EFI_CONVENTIONAL_MEMORY == 7u, "UEFI type");
_Static_assert(AARCH64_RAM_GRANULE == (UINT64_C(1) << 21), "granule");
_Static_assert(AARCH64_RAM_MAX_RANGES == 16, "map capacity");

/* ── Bytewise little-endian helpers ─────────────────────────── */
/* The spec says never cast raw descriptor bytes to a struct — the
 * test mirrors that by writing every field byte by byte. The wire
 * offsets (Type=0 u32, PhysicalStart=8 u64, NumberOfPages=24 u64)
 * are fixed regardless of the per-descriptor stride. */
static void put_le32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)(v & 0xffu);
    buf[1] = (uint8_t)((v >> 8) & 0xffu);
    buf[2] = (uint8_t)((v >> 16) & 0xffu);
    buf[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *buf, uint64_t v)
{
    buf[0] = (uint8_t)(v & 0xffu);
    buf[1] = (uint8_t)((v >> 8) & 0xffu);
    buf[2] = (uint8_t)((v >> 16) & 0xffu);
    buf[3] = (uint8_t)((v >> 24) & 0xffu);
    buf[4] = (uint8_t)((v >> 32) & 0xffu);
    buf[5] = (uint8_t)((v >> 40) & 0xffu);
    buf[6] = (uint8_t)((v >> 48) & 0xffu);
    buf[7] = (uint8_t)((v >> 56) & 0xffu);
}

/* Write one descriptor at `buf` using the wire offsets described
 * above. Remaining bytes (up to `stride`) are zeroed so padding
 * does not accidentally look like a Type byte when the next
 * descriptor is read. */
static void make_descriptor(uint8_t *buf, uint32_t stride,
                            uint32_t type, uint64_t phys_start,
                            uint64_t num_pages)
{
    uint32_t i;
    for (i = 0u; i < stride; ++i)
        buf[i] = 0u;
    put_le32(buf + 0u,  type);
    put_le64(buf + 8u,  phys_start);
    put_le64(buf + 24u, num_pages);
}

/* ── Output invariant checker ───────────────────────────────── */
/* Returns 1 on success — every published range granule-aligned,
 * strictly ascending, disjoint from its neighbours, and outside
 * every supplied exclusion. */
static int invariants_ok(const struct aarch64_ram_map *out,
                         const struct aarch64_ram_interval *exclude,
                         uint32_t exclude_count)
{
    uint32_t i, j;
    if (out->count == 0u || out->count > AARCH64_RAM_MAX_RANGES)
        return 0;
    for (i = 0u; i < out->count; ++i) {
        uint64_t s = out->ranges[i].start;
        uint64_t e = out->ranges[i].end;
        if ((s & (AARCH64_RAM_GRANULE - 1u)) != 0u) return 0;
        if ((e & (AARCH64_RAM_GRANULE - 1u)) != 0u) return 0;
        if (s >= e) return 0;
        if (i > 0u && s <= out->ranges[i - 1u].end) return 0;
        for (j = 0u; j < exclude_count; ++j) {
            if (s < exclude[j].end && e > exclude[j].start)
                return 0;
        }
    }
    return 1;
}

static int out_is_zero(const struct aarch64_ram_map *out)
{
    uint32_t i;
    if (out->count != 0u)
        return 0;
    for (i = 0u; i < AARCH64_RAM_MAX_RANGES; ++i) {
        if (out->ranges[i].start != 0u) return 0;
        if (out->ranges[i].end   != 0u) return 0;
    }
    return 1;
}

static int check(int cond) { return cond ? 0 : 1; }

int main(void)
{
    /* Fixed widths: 2 MiB = 0x200000, 4 KiB = 0x1000. */
    const uint64_t M2 = UINT64_C(0x200000);
    const uint64_t M4 = UINT64_C(0x1000);

    /* Test cases 0–15. Each case ends in a unique `return N;`
     * label so a regression points straight at the failed case. */

    {
        /* Case 1: zero entry_count → ERR_ARGUMENT, out zeroed. */
        struct aarch64_ram_map out;
        uint8_t scratch[32];
        int rc;
        memset(&out, 0xAA, sizeof(out));
        memset(scratch, 0xCC, sizeof(scratch));
        rc = aarch64_ram_normalize(scratch, 0u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_ARGUMENT)) return 1;
        if (check(out.count == 0u)) return 1;
        if (check(out_is_zero(&out))) return 1;
    }

    {
        /* Case 2: entry_size < 32 → ERR_GEOMETRY. */
        struct aarch64_ram_map out;
        uint8_t scratch[32];
        int rc;
        memset(&out, 0xAA, sizeof(out));
        memset(scratch, 0, sizeof(scratch));
        rc = aarch64_ram_normalize(scratch, 1u, 31u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_GEOMETRY)) return 2;
        if (check(out.count == 0u)) return 2;
        if (check(out_is_zero(&out))) return 2;
    }

    {
        /* Case 3: format != BOOT_MEMORY_FORMAT_UEFI_RAW → ERR_FORMAT. */
        struct aarch64_ram_map out;
        uint8_t scratch[32];
        int rc;
        memset(&out, 0xAA, sizeof(out));
        memset(scratch, 0, sizeof(scratch));
        rc = aarch64_ram_normalize(scratch, 1u, 32u, 0u, 1u,
                                   (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_FORMAT)) return 3;
        if (check(out.count == 0u)) return 3;
        if (check(out_is_zero(&out))) return 3;
    }

    {
        /* Case 4: descriptor_version != 1 → ERR_VERSION. */
        struct aarch64_ram_map out;
        uint8_t scratch[32];
        int rc;
        memset(&out, 0xAA, sizeof(out));
        memset(scratch, 0, sizeof(scratch));
        rc = aarch64_ram_normalize(scratch, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   2u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_VERSION)) return 4;
        if (check(out.count == 0u)) return 4;
        if (check(out_is_zero(&out))) return 4;
    }

    {
        /* Case 5: exclude_count > 0 && exclude == NULL → ERR_ARGUMENT. */
        struct aarch64_ram_map out;
        uint8_t scratch[32];
        int rc;
        memset(&out, 0xAA, sizeof(out));
        memset(scratch, 0, sizeof(scratch));
        rc = aarch64_ram_normalize(scratch, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   1u, &out);
        if (check(rc == AARCH64_RAM_ERR_ARGUMENT)) return 5;
        if (check(out.count == 0u)) return 5;
        if (check(out_is_zero(&out))) return 5;
    }

    {
        /* Case 6: invalid exclusion (end <= start) → ERR_ARGUMENT. */
        struct aarch64_ram_interval bad = { 50u * M2, 40u * M2 };
        struct aarch64_ram_map out;
        uint8_t scratch[32];
        int rc;
        memset(&out, 0xAA, sizeof(out));
        memset(scratch, 0, sizeof(scratch));
        rc = aarch64_ram_normalize(scratch, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, &bad, 1u, &out);
        if (check(rc == AARCH64_RAM_ERR_ARGUMENT)) return 6;
        if (check(out.count == 0u)) return 6;
        if (check(out_is_zero(&out))) return 6;
    }

    {
        /* Case 7: NumberOfPages == 0 → ERR_ARGUMENT. */
        uint8_t buf[32];
        struct aarch64_ram_map out;
        int rc;
        memset(&out, 0xAA, sizeof(out));
        make_descriptor(buf, 32u, AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), UINT64_C(0));
        rc = aarch64_ram_normalize(buf, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_ARGUMENT)) return 7;
        if (check(out_is_zero(&out))) return 7;
    }

    {
        /* Case 8: NumberOfPages * 4096 overflow → ERR_OVERFLOW. */
        uint8_t buf[32];
        struct aarch64_ram_map out;
        uint64_t huge;
        int rc;
        memset(&out, 0xAA, sizeof(out));
        /* Largest page count that does NOT overflow: UINT64_MAX / 4096.
         * Add 1 so the multiplication wraps. */
        huge = (UINT64_C(0) - UINT64_C(1)) / M4 + UINT64_C(1);
        make_descriptor(buf, 32u, AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), huge);
        rc = aarch64_ram_normalize(buf, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_OVERFLOW)) return 8;
        if (check(out_is_zero(&out))) return 8;
    }

    {
        /* Case 9: 17 disjoint type-7 descriptors → ERR_CAPACITY.
         * Each descriptor is exactly 4 MiB followed by a 4 MiB gap,
         * so the aligned fragments stay disjoint — no two fragments
         * touch or overlap. After 16 emissions the discovery probe
         * finds a 17th range and the normalizer returns
         * AARCH64_RAM_ERR_CAPACITY. */
        uint8_t buf[17u * 32u];
        struct aarch64_ram_map out;
        int rc;
        uint32_t i;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        for (i = 0u; i < 17u; ++i) {
            uint64_t start = (uint64_t)i * 8u * M2;
            uint64_t pages = (4u * M2) / M4;
            make_descriptor(buf + i * 32u, 32u,
                            AARCH64_EFI_CONVENTIONAL_MEMORY,
                            start, pages);
        }
        rc = aarch64_ram_normalize(buf, 17u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_ERR_CAPACITY)) return 9;
        if (check(out_is_zero(&out))) return 9;
    }

    {
        /* Case 10 (positive): non-type-7 descriptor silently dropped. */
        uint8_t buf[2u * 32u];
        struct aarch64_ram_map out;
        int rc;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        make_descriptor(buf + 0u * 32u, 32u,
                        /* type 4 = EfiReservedMemoryType */ 4u,
                        UINT64_C(0), (100u * M2) / M4);
        make_descriptor(buf + 1u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), (100u * M2) / M4);
        rc = aarch64_ram_normalize(buf, 2u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_OK)) return 10;
        if (check(out.count == 1u)) return 10;
        if (check(out.ranges[0].start == UINT64_C(0))) return 10;
        if (check(out.ranges[0].end == 100u * M2)) return 10;
        if (check(invariants_ok(&out,
                                (const struct aarch64_ram_interval *)0, 0u)))
            return 10;
    }

    {
        /* Case 11 (positive): unsorted adjacent type-7 descriptors
         * merge into a single range. Descriptor order is reversed
         * so the smaller-start descriptor sits at index 1. */
        uint8_t buf[2u * 32u];
        struct aarch64_ram_map out;
        int rc;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        /* D0: [50 MiB, 75 MiB) — comes first in the buffer. */
        make_descriptor(buf + 0u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        50u * M2, (25u * M2) / M4);
        /* D1: [0, 60 MiB) — overlaps D0. */
        make_descriptor(buf + 1u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), (60u * M2) / M4);
        rc = aarch64_ram_normalize(buf, 2u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_OK)) return 11;
        if (check(out.count == 1u)) return 11;
        if (check(out.ranges[0].start == UINT64_C(0))) return 11;
        if (check(out.ranges[0].end == 75u * M2)) return 11;
        if (check(invariants_ok(&out,
                                (const struct aarch64_ram_interval *)0, 0u)))
            return 11;
    }

    {
        /* Case 12 (positive): deliberately unaligned stride = 40
         * bytes. The fields still live at offsets 0/8/24; the
         * extra 8 bytes per descriptor are zeroed padding. */
        uint8_t buf[40u];
        struct aarch64_ram_map out;
        int rc;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        make_descriptor(buf, 40u, AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), (100u * M2) / M4);
        rc = aarch64_ram_normalize(buf, 1u, 40u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_OK)) return 12;
        if (check(out.count == 1u)) return 12;
        if (check(out.ranges[0].start == UINT64_C(0))) return 12;
        if (check(out.ranges[0].end == 100u * M2)) return 12;
        if (check(invariants_ok(&out,
                                (const struct aarch64_ram_interval *)0, 0u)))
            return 12;
    }

    {
        /* Case 13 (positive): fragment below one 2 MiB granule
         * dropped. D0 starts at 1 MiB and spans only 1 MiB, which
         * rounds inward to a 0-byte aligned interval and is
         * therefore discarded. D1 survives as the sole published
         * range. */
        uint8_t buf[2u * 32u];
        struct aarch64_ram_map out;
        int rc;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        /* 1 MiB / 4 KiB = 256 pages. */
        make_descriptor(buf + 0u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        M2, UINT64_C(256));
        make_descriptor(buf + 1u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        10u * M2, (10u * M2) / M4);
        rc = aarch64_ram_normalize(buf, 2u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_OK)) return 13;
        if (check(out.count == 1u)) return 13;
        if (check(out.ranges[0].start == 10u * M2)) return 13;
        if (check(out.ranges[0].end == 20u * M2)) return 13;
        if (check(invariants_ok(&out,
                                (const struct aarch64_ram_interval *)0, 0u)))
            return 13;
    }

    {
        /* Case 14 (positive): a single range split by an
         * exclusion. The descriptor [0, 100 MiB) minus the
         * closed-open interval [40 MiB, 60 MiB) becomes two
         * aligned ranges. */
        uint8_t buf[32];
        struct aarch64_ram_interval excl[1];
        struct aarch64_ram_map out;
        int rc;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        make_descriptor(buf, 32u, AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), (100u * M2) / M4);
        excl[0].start = 40u * M2;
        excl[0].end   = 60u * M2;
        rc = aarch64_ram_normalize(buf, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, excl, 1u, &out);
        if (check(rc == AARCH64_RAM_OK)) return 14;
        if (check(out.count == 2u)) return 14;
        if (check(out.ranges[0].start == UINT64_C(0))) return 14;
        if (check(out.ranges[0].end == 40u * M2)) return 14;
        if (check(out.ranges[1].start == 60u * M2)) return 14;
        if (check(out.ranges[1].end == 100u * M2)) return 14;
        if (check(invariants_ok(&out, excl, 1u))) return 14;
    }

    {
        /* Case 15 (positive): later bridging descriptor proves
         * capacity is checked only after final merging. Without
         * bridging, D0 [0, 100 MiB), D1 [200 MiB, 300 MiB),
         * D2 [150 MiB, 250 MiB) would produce 3 fragments, but
         * the repeated-scan algorithm merges them into
         * [0, 100 MiB) and [150 MiB, 300 MiB) — 2 final ranges. */
        uint8_t buf[3u * 32u];
        struct aarch64_ram_map out;
        int rc;
        memset(buf, 0, sizeof(buf));
        memset(&out, 0xAA, sizeof(out));
        make_descriptor(buf + 0u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), (100u * M2) / M4);
        make_descriptor(buf + 1u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        200u * M2, (100u * M2) / M4);
        make_descriptor(buf + 2u * 32u, 32u,
                        AARCH64_EFI_CONVENTIONAL_MEMORY,
                        150u * M2, (100u * M2) / M4);
        rc = aarch64_ram_normalize(buf, 3u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, (const struct aarch64_ram_interval *)0,
                                   0u, &out);
        if (check(rc == AARCH64_RAM_OK)) return 15;
        if (check(out.count == 2u)) return 15;
        if (check(out.ranges[0].start == UINT64_C(0))) return 15;
        if (check(out.ranges[0].end == 100u * M2)) return 15;
        if (check(out.ranges[1].start == 150u * M2)) return 15;
        if (check(out.ranges[1].end == 300u * M2)) return 15;
        if (check(invariants_ok(&out,
                                (const struct aarch64_ram_interval *)0, 0u)))
            return 15;
    }

    {
        /* Case 16: exclude_count >= RAM_FRAG_SCRATCH_MAX →
         * ERR_ARGUMENT. The normalizer's fragment-scratch buffer is
         * bounded at RAM_FRAG_SCRATCH_MAX (8); a larger exclude
         * list would silently overflow the buffer and violate the
         * spec's O(1)-scratch contract. The cap must fail closed. */
        struct aarch64_ram_interval excl[RAM_FRAG_SCRATCH_MAX];
        uint8_t buf[32];
        struct aarch64_ram_map out;
        uint32_t i;
        int rc;
        for (i = 0u; i < RAM_FRAG_SCRATCH_MAX; ++i) {
            excl[i].start = (uint64_t)i * M2;
            excl[i].end   = excl[i].start + M2;
        }
        memset(&out, 0xAA, sizeof(out));
        make_descriptor(buf, 32u, AARCH64_EFI_CONVENTIONAL_MEMORY,
                        UINT64_C(0), (100u * M2) / M4);
        rc = aarch64_ram_normalize(buf, 1u, 32u,
                                   (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                                   1u, excl, RAM_FRAG_SCRATCH_MAX, &out);
        if (check(rc == AARCH64_RAM_ERR_ARGUMENT)) return 16;
        if (check(out_is_zero(&out))) return 16;
    }

    {
        /* Case 17: publish_once happy path. A valid one-range
         * candidate is copied into the destination and the
         * initialized flag is raised. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        int initialized = 0;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xAA, sizeof(published));
        candidate.count = 1u;
        candidate.ranges[0].start = 2u * M2;
        candidate.ranges[0].end   = 4u * M2;
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc == 0)) return 17;
        if (check(initialized != 0)) return 17;
        if (check(published.count == 1u)) return 17;
        if (check(published.ranges[0].start == 2u * M2)) return 17;
        if (check(published.ranges[0].end   == 4u * M2)) return 17;
    }

    {
        /* Case 18: publish_once second-call guard. The first
         * publication leaves the flag non-zero; a subsequent call
         * returns a distinct negative code and does not change
         * the destination map. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        int initialized = 0;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xAA, sizeof(published));
        candidate.count = 1u;
        candidate.ranges[0].start = 2u * M2;
        candidate.ranges[0].end   = 4u * M2;
        (void)aarch64_ram_publish_once(&candidate, &published, &initialized);
        /* Now present a different valid candidate; the helper must
         * refuse without touching `published` or `initialized`. */
        candidate.ranges[0].start = 8u * M2;
        candidate.ranges[0].end   = 10u * M2;
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc == -2)) return 18;
        if (check(published.count == 1u)) return 18;
        if (check(published.ranges[0].start == 2u * M2)) return 18;
        if (check(published.ranges[0].end   == 4u * M2)) return 18;
        if (check(initialized != 0)) return 18;
    }

    {
        /* Case 19: publish_once rejects count == 0. The destination
         * and flag must be left untouched. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        struct aarch64_ram_map saved_published;
        int initialized = 0;
        int saved_init = initialized;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xBB, sizeof(published));
        saved_published = published;
        candidate.count = 0u;
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc < 0)) return 19;
        if (check(initialized == 0)) return 19;
        if (check(initialized == saved_init)) return 19;
        if (check(published.count == saved_published.count)) return 19;
        if (check(published.ranges[0].start == saved_published.ranges[0].start))
            return 19;
        if (check(published.ranges[0].end   == saved_published.ranges[0].end))
            return 19;
    }

    {
        /* Case 20: publish_once rejects count > AARCH64_RAM_MAX_RANGES.
         * A 17-range candidate must fail closed without changing
         * the destination. We build only the first 16 ranges (the
         * struct's capacity) and rely on the rejected count to
         * keep us from indexing past the end. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        struct aarch64_ram_map saved_published;
        int initialized = 0;
        int saved_init = initialized;
        int rc;
        uint32_t i;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xBB, sizeof(published));
        saved_published = published;
        candidate.count = (uint32_t)AARCH64_RAM_MAX_RANGES + 1u;
        for (i = 0u; i < (uint32_t)AARCH64_RAM_MAX_RANGES; ++i) {
            candidate.ranges[i].start = (uint64_t)i * 2u * M2;
            candidate.ranges[i].end   = candidate.ranges[i].start + 2u * M2;
        }
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc < 0)) return 20;
        if (check(initialized == 0)) return 20;
        if (check(initialized == saved_init)) return 20;
        if (check(published.count == saved_published.count)) return 20;
    }

    {
        /* Case 21: publish_once rejects unaligned start. A candidate
         * whose first range starts at a non-granule address must
         * fail closed without changing the destination. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        struct aarch64_ram_map saved_published;
        int initialized = 0;
        int saved_init = initialized;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xBB, sizeof(published));
        saved_published = published;
        candidate.count = 1u;
        candidate.ranges[0].start = 2u * M2 + M4; /* not 2 MiB aligned */
        candidate.ranges[0].end   = 4u * M2;
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc < 0)) return 21;
        if (check(initialized == 0)) return 21;
        if (check(initialized == saved_init)) return 21;
        if (check(published.count == saved_published.count)) return 21;
        if (check(published.ranges[0].start == saved_published.ranges[0].start))
            return 21;
    }

    {
        /* Case 22: publish_once rejects unaligned end. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        struct aarch64_ram_map saved_published;
        int initialized = 0;
        int saved_init = initialized;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xBB, sizeof(published));
        saved_published = published;
        candidate.count = 1u;
        candidate.ranges[0].start = 2u * M2;
        candidate.ranges[0].end   = 4u * M2 + M4; /* not 2 MiB aligned */
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc < 0)) return 22;
        if (check(initialized == 0)) return 22;
        if (check(initialized == saved_init)) return 22;
        if (check(published.count == saved_published.count)) return 22;
        if (check(published.ranges[0].end == saved_published.ranges[0].end))
            return 22;
    }

    {
        /* Case 23: publish_once rejects descending order. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        struct aarch64_ram_map saved_published;
        int initialized = 0;
        int saved_init = initialized;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xBB, sizeof(published));
        saved_published = published;
        candidate.count = 2u;
        candidate.ranges[0].start = 8u * M2;
        candidate.ranges[0].end   = 10u * M2;
        candidate.ranges[1].start = 2u * M2; /* descending */
        candidate.ranges[1].end   = 4u * M2;
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc < 0)) return 23;
        if (check(initialized == 0)) return 23;
        if (check(initialized == saved_init)) return 23;
        if (check(published.count == saved_published.count)) return 23;
    }

    {
        /* Case 24: publish_once rejects touching/overlapping
         * ranges. A second range starting exactly at the previous
         * end is not "strictly greater than prev_end" and must be
         * rejected. */
        struct aarch64_ram_map candidate;
        struct aarch64_ram_map published;
        struct aarch64_ram_map saved_published;
        int initialized = 0;
        int saved_init = initialized;
        int rc;
        memset(&candidate, 0, sizeof(candidate));
        memset(&published, 0xBB, sizeof(published));
        saved_published = published;
        candidate.count = 2u;
        candidate.ranges[0].start = 2u * M2;
        candidate.ranges[0].end   = 4u * M2;
        candidate.ranges[1].start = 4u * M2; /* touching */
        candidate.ranges[1].end   = 6u * M2;
        rc = aarch64_ram_publish_once(&candidate, &published, &initialized);
        if (check(rc < 0)) return 24;
        if (check(initialized == 0)) return 24;
        if (check(initialized == saved_init)) return 24;
        if (check(published.count == saved_published.count)) return 24;
    }

    return 0;
}
'''


def build(tmp):
    """Compile the runner with the host C compiler and the production
    ram_core.c. -I. and -Ikernel/include keep the relative
    `<kernel/...>` paths from the production headers honest."""
    runner_c = tmp / 'runner.c'
    runner_c.write_text(RUNNER)
    executable = tmp / 'runner'
    cmd = [
        os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I.', '-Ikernel/include',
        str(ROOT / 'kernel' / 'arch' / 'aarch64' / 'memory' / 'ram_core.c'),
        str(runner_c),
        '-o', str(executable),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return executable


def main():
    with tempfile.TemporaryDirectory(prefix='aarch64-ram-') as tmp:
        tmp = Path(tmp)
        executable = build(tmp)
        result = subprocess.run([str(executable)], cwd=ROOT,
                               capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise SystemExit(f'aarch64_ram_test: runner exit {result.returncode}')
    print('aarch64_ram_test: contracts and behaviour ok')


if __name__ == '__main__':
    main()
