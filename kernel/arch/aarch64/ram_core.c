/* kernel/arch/aarch64/ram_core.c
 *
 * Pure, host-linkable normalizer for the AArch64 UEFI raw memory
 * map. Implemented in Task 2 of the AArch64 UEFI RAM normalization
 * plan. The companion publisher (`aarch64_ram_publish_once()`)
 * arrives in Task 3 — for now it stays the linkable empty core
 * that returns a non-zero error and leaves the destination zeroed.
 *
 * Host-linkable AND freestanding-target-linkable: this file uses
 * only `<stddef.h>` and `<stdint.h>` (no `<errno.h>`, no `<string.h>`,
 * no `<stdlib.h>`) so the same translation unit compiles both under
 * `cc -std=c11` for the contract test and under the
 * `aarch64-none-elf` clang for the kernel build.
 *
 * Algorithm (spec §"Pure normalization and output contract"):
 *
 *   1. Validate the descriptor stream metadata and every selected
 *      descriptor (type 7, non-zero page count, no overflow on
 *      NumberOfPages * 4096 or PhysicalStart + byte_length). Any
 *      failure leaves `*out` cleared and returns a distinct negative
 *      error code.
 *
 *   2. For each descriptor whose `Type == 7` (`EfiConventionalMemory`):
 *      build `[PhysicalStart, PhysicalStart + NumberOfPages * 4096)`,
 *      subtract the closed-open exclusion intervals into one or more
 *      surviving fragments, round each fragment inward to
 *      `AARCH64_RAM_GRANULE`, and discard empty results. Fragments
 *      are NOT retained across descriptors — the discovery scans
 *      recompute them per pass (O(1) scratch).
 *
 *   3. Run the spec's repeated-scan algorithm: for the next output
 *      range, scan every descriptor and each of its aligned fragments
 *      to pick the lowest fragment beginning at or after the previous
 *      emitted end; rescan all fragments to extend that candidate's
 *      end for every overlapping or touching fragment until a full
 *      scan makes no expansion; emit the merged candidate. After 16
 *      emissions, do one more discovery scan and fail with
 *      `AARCH64_RAM_ERR_CAPACITY` if it finds another range.
 *
 *   4. Reject a successful-but-empty map as an error.
 *
 * Descriptor fields are decoded via bytewise little-endian loads at
 * fixed offsets (Type=0 u32, PhysicalStart=8 u64, NumberOfPages=24
 * u64). We never cast raw descriptor bytes to a C structure — the
 * `entry_size` may be 32, 40, or any value the firmware decides to
 * use, and C packing rules and alignment assumptions would corrupt
 * the parse.
 */
#include <stddef.h>
#include <stdint.h>

#include <kernel/bootinfo.h>

/* ram_core.h is kernel-internal (kernel/arch/aarch64/), not in
 * kernel/include/, so the kernel's `-Iinclude` search path does not
 * find it under `<kernel/arch/aarch64/ram_core.h>`. Include it
 * relative to this file (matching the style of boot_percpu.c, smp.c,
 * and the other aarch64 C files). The header itself pulls in
 * kernel/include/kernel/arch/aarch64/ram.h. */
#include "ram_core.h"

/* ── Constants local to this TU ──────────────────────────────── */
#define RAM_PAGE_BYTES      UINT64_C(4096)
#define RAM_GRANULE_BYTES   ((uint64_t)AARCH64_RAM_GRANULE)
#define RAM_GRANULE_MASK    (RAM_GRANULE_BYTES - UINT64_C(1))
/* Sentinel meaning "no candidate seen yet" for `tent_start`. Using
 * UINT64_MAX is safe because no real fragment can start there — a
 * fragment with start == UINT64_MAX would be empty (end must be
 * > start). */
#define RAM_NO_CANDIDATE    (UINT64_C(0) - UINT64_C(1))
/* Per-descriptor scratch cap for exclusion subtraction. After k
 * exclusions a `[start, end)` interval splits into at most k + 1
 * surviving fragments; the kernel hands the normalizer exactly two
 * exclusions (kernel image + handoff allocation), so 8 is comfortable
 * headroom. */
#define RAM_FRAG_SCRATCH_MAX 8u

/* ── Tiny helpers ────────────────────────────────────────────── */
static void zero_bytes(void *buffer, size_t size)
{
    volatile uint8_t *cursor = (volatile uint8_t *)buffer;
    size_t index;

    if (!buffer)
        return;
    for (index = 0; index < size; ++index)
        cursor[index] = 0;
}

static uint32_t load_le32(const uint8_t *p)
{
    return  ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t load_le64(const uint8_t *p)
{
    return  ((uint64_t)p[0])
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

/* Round `start` up and `end` down to the 2 MiB granule. Returns 1 if
 * the aligned interval is non-empty (the normalizer keeps it),
 * 0 otherwise (dropped, aligned start >= aligned end). */
static int align_inward(uint64_t start, uint64_t end,
                        uint64_t *aligned_start, uint64_t *aligned_end)
{
    uint64_t s = (start + RAM_GRANULE_MASK) & ~RAM_GRANULE_MASK;
    uint64_t e = end & ~RAM_GRANULE_MASK;
    if (s >= e)
        return 0;
    *aligned_start = s;
    *aligned_end = e;
    return 1;
}

/* ── Fragment iteration with exclusion subtraction ───────────── */
/* Per-fragment callback: `start` and `end` are granule-aligned and
 * have already passed the inward-alignment drop test. The discovery
 * scans invoke this once per surviving fragment, then immediately
 * discard it — no fragment list crosses a descriptor boundary. */
typedef void (*fragment_cb)(uint64_t start, uint64_t end, void *ctx);

/* Subtract the closed-open exclusion list from `[d_s, d_e)` and
 * invoke `cb` for every granule-aligned surviving fragment.
 *
 * Implementation: a small fixed scratch array holds the running set
 * of intervals after each exclusion. For each exclusion we walk the
 * current set and split every interval that overlaps the exclusion;
 * non-overlapping intervals are carried forward unchanged. After
 * processing all exclusions the surviving intervals are aligned
 * inward and emitted via the callback.
 *
 * The scratch holds at most `RAM_FRAG_SCRATCH_MAX` intervals. The
 * spec promises the kernel hands the normalizer exactly two
 * exclusions (kernel image + handoff allocation), so this cap is
 * comfortable headroom. */
static void emit_aligned_fragments(uint64_t d_s, uint64_t d_e,
                                   const struct aarch64_ram_interval *exclude,
                                   uint32_t exclude_count,
                                   fragment_cb cb, void *ctx)
{
    uint64_t buf_start[RAM_FRAG_SCRATCH_MAX];
    uint64_t buf_end[RAM_FRAG_SCRATCH_MAX];
    uint32_t count;
    uint32_t i, j;

    buf_start[0] = d_s;
    buf_end[0] = d_e;
    count = 1u;

    for (i = 0u; i < exclude_count; ++i) {
        uint64_t e_s = exclude[i].start;
        uint64_t e_e = exclude[i].end;
        uint32_t new_count = 0u;

        for (j = 0u; j < count; ++j) {
            uint64_t s = buf_start[j];
            uint64_t e = buf_end[j];

            if (e_s >= e || e_e <= s) {
                /* No overlap: keep as-is */
                if (new_count < RAM_FRAG_SCRATCH_MAX) {
                    buf_start[new_count] = s;
                    buf_end[new_count] = e;
                    ++new_count;
                }
            } else {
                /* Overlap: split into up to two pieces */
                if (s < e_s && new_count < RAM_FRAG_SCRATCH_MAX) {
                    buf_start[new_count] = s;
                    buf_end[new_count] = e_s;
                    ++new_count;
                }
                if (e_e < e && new_count < RAM_FRAG_SCRATCH_MAX) {
                    buf_start[new_count] = e_e;
                    buf_end[new_count] = e;
                    ++new_count;
                }
            }
        }
        count = new_count;
        if (count == 0u)
            break;
    }

    for (j = 0u; j < count; ++j) {
        uint64_t a_s;
        uint64_t a_e;
        if (align_inward(buf_start[j], buf_end[j], &a_s, &a_e))
            cb(a_s, a_e, ctx);
    }
}

/* ── Discovery scan state ───────────────────────────────────── */
/* Captures the previous emitted end and the current tentative
 * range. The two scan modes — find-smallest-start vs.
 * extend-end — share the struct but use disjoint subsets of its
 * fields. */
struct scan_state {
    uint64_t prev_end;
    uint64_t tent_start;
    uint64_t tent_end;
};

/* Mode 0 (discovery): pick the fragment with the lowest start at
 * or after `prev_end`. `tent_start == RAM_NO_CANDIDATE` means no
 * candidate yet. */
static void cb_find_smallest(uint64_t start, uint64_t end, void *ctx)
{
    struct scan_state *s = (struct scan_state *)ctx;
    if (end <= s->prev_end)
        return;
    if (start >= s->prev_end && start < s->tent_start) {
        s->tent_start = start;
        s->tent_end = end;
    }
}

/* Mode 1 (extend): extend `tent_end` for every fragment that
 * overlaps or touches the current tentative range. */
static void cb_extend_end(uint64_t start, uint64_t end, void *ctx)
{
    struct scan_state *s = (struct scan_state *)ctx;
    if (end <= s->tent_start)
        return;
    /* Touching fragments (start == tent_end) qualify because
     * `start <= tent_end`; `end > tent_end` rejects fragments
     * fully inside the candidate. */
    if (start <= s->tent_end && end > s->tent_end)
        s->tent_end = end;
}

/* Walk every descriptor once, compute its aligned fragments, and
 * invoke the supplied fragment callback. Decodes descriptor fields
 * via bytewise little-endian loads and returns `AARCH64_RAM_ERR_*`
 * if a selected descriptor carries an invalid page count or an
 * overflowing byte length. Non-type-7 descriptors are silently
 * skipped. */
static int walk_type7_descriptors(const uint8_t *bytes,
                                  uint32_t entry_count,
                                  uint32_t entry_size,
                                  const struct aarch64_ram_interval *exclude,
                                  uint32_t exclude_count,
                                  fragment_cb cb, void *ctx)
{
    uint32_t i;

    for (i = 0u; i < entry_count; ++i) {
        const uint8_t *d;
        uint32_t type;
        uint64_t phys_start;
        uint64_t num_pages;
        uint64_t byte_length;
        uint64_t end;

        d = bytes + ((size_t)i * (size_t)entry_size);

        type = load_le32(d + 0u);
        if (type != (uint32_t)AARCH64_EFI_CONVENTIONAL_MEMORY)
            continue;

        phys_start = load_le64(d + 8u);
        num_pages = load_le64(d + 24u);

        if (num_pages == UINT64_C(0))
            return AARCH64_RAM_ERR_ARGUMENT;

        if (num_pages > (UINT64_C(0) - RAM_PAGE_BYTES) / RAM_PAGE_BYTES)
            return AARCH64_RAM_ERR_OVERFLOW;
        byte_length = num_pages * RAM_PAGE_BYTES;

        if (phys_start > (UINT64_C(0) - byte_length))
            return AARCH64_RAM_ERR_OVERFLOW;
        end = phys_start + byte_length;

        emit_aligned_fragments(phys_start, end, exclude, exclude_count,
                               cb, ctx);
    }
    return AARCH64_RAM_OK;
}

/* ── Public normalizer ───────────────────────────────────────── */
int aarch64_ram_normalize(const uint8_t *bytes, uint32_t entry_count,
                          uint32_t entry_size, uint32_t format,
                          uint32_t descriptor_version,
                          const struct aarch64_ram_interval *exclude,
                          uint32_t exclude_count,
                          struct aarch64_ram_map *out)
{
    uint64_t prev_end;
    uint32_t ranges_written;
    uint32_t i;

    /* ── Metadata validation. Each branch clears `out` so a
     * failure leaves a defined empty state for the caller. */
    if (entry_count == 0u) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_ARGUMENT;
    }
    if (entry_size < (uint32_t)AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_GEOMETRY;
    }
    if (format != (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_FORMAT;
    }
    if (descriptor_version != 1u) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_VERSION;
    }
    if (bytes == (const uint8_t *)0 || out == (struct aarch64_ram_map *)0)
        return AARCH64_RAM_ERR_ARGUMENT;
    if (exclude_count > 0u
        && exclude == (const struct aarch64_ram_interval *)0) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_ARGUMENT;
    }
    for (i = 0u; i < exclude_count; ++i) {
        if (exclude[i].end <= exclude[i].start) {
            zero_bytes(out, sizeof(*out));
            return AARCH64_RAM_ERR_ARGUMENT;
        }
    }
    /* entry_count * entry_size overflow check. Both fit in u32; the
     * product must fit in u64. */
    if ((uint64_t)entry_count
            > (UINT64_C(0) - 1u) / (uint64_t)entry_size) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_OVERFLOW;
    }

    /* Start of work: clear the output so callers can rely on a
     * defined state regardless of which path we take. */
    zero_bytes(out, sizeof(*out));

    /* ── Repeated-scan final-range discovery ─────────────────────
     * Per spec: scan for the smallest qualifying start, then rescan
     * to extend the tentative end until stable, then emit. After 16
     * emissions, one extra discovery scan checks for a 17th range —
     * the spec's "capacity checked only after final merging" rule. */
    prev_end = UINT64_C(0);
    ranges_written = 0u;

    while (1) {
        struct scan_state s;
        int rc;

        /* Step 1: find smallest qualifying start. */
        s.prev_end = prev_end;
        s.tent_start = RAM_NO_CANDIDATE;
        s.tent_end = UINT64_C(0);
        rc = walk_type7_descriptors(bytes, entry_count, entry_size,
                                    exclude, exclude_count,
                                    cb_find_smallest, &s);
        if (rc != AARCH64_RAM_OK) {
            zero_bytes(out, sizeof(*out));
            return rc;
        }
        if (s.tent_start == RAM_NO_CANDIDATE)
            break; /* no more ranges */

        /* Step 2: rescan and extend until stable. Each pass walks
         * all descriptors; `cb_extend_end` bumps `tent_end` whenever
         * a fragment overlaps or touches the current tentative. We
         * repeat until a full pass makes no expansion. */
        for (;;) {
            uint64_t before_end = s.tent_end;
            rc = walk_type7_descriptors(bytes, entry_count, entry_size,
                                        exclude, exclude_count,
                                        cb_extend_end, &s);
            if (rc != AARCH64_RAM_OK) {
                zero_bytes(out, sizeof(*out));
                return rc;
            }
            if (s.tent_end == before_end)
                break;
        }

        /* Step 3: emit. */
        out->ranges[ranges_written].start = s.tent_start;
        out->ranges[ranges_written].end = s.tent_end;
        ++ranges_written;
        prev_end = s.tent_end;

        if (ranges_written >= (uint32_t)AARCH64_RAM_MAX_RANGES) {
            /* Step 4: one final discovery scan; if it finds a 17th
             * range the boot caller has a fatal capacity error. */
            struct scan_state probe;
            probe.prev_end = prev_end;
            probe.tent_start = RAM_NO_CANDIDATE;
            probe.tent_end = UINT64_C(0);
            rc = walk_type7_descriptors(bytes, entry_count, entry_size,
                                        exclude, exclude_count,
                                        cb_find_smallest, &probe);
            if (rc != AARCH64_RAM_OK) {
                zero_bytes(out, sizeof(*out));
                return rc;
            }
            if (probe.tent_start != RAM_NO_CANDIDATE) {
                zero_bytes(out, sizeof(*out));
                return AARCH64_RAM_ERR_CAPACITY;
            }
            break;
        }
    }

    if (ranges_written == 0u) {
        zero_bytes(out, sizeof(*out));
        return AARCH64_RAM_ERR_ARGUMENT;
    }
    out->count = ranges_written;
    return AARCH64_RAM_OK;
}

/* ── Stub publisher (Task 3 owns the real one) ─────────────────── */
int aarch64_ram_publish_once(const struct aarch64_ram_map *candidate,
                             struct aarch64_ram_map *destination,
                             int *initialized)
{
    (void)candidate;
    zero_bytes(destination, sizeof(*destination));
    if (initialized)
        *initialized = 0;
    return -1;
}