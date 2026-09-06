# AArch64 UEFI RAM Normalization Design

## Status

Proposed. This document defines one deliberately small AArch64 memory-management
increment: turn the raw UEFI memory map in `boot_context` into a deterministic,
safe list of allocatable 2 MiB physical ranges. It does **not** enable the
legacy PMM, virtual-memory allocator, or user address spaces on AArch64.

## Problem

The AArch64 boot loader correctly preserves the UEFI descriptor stream as
`BOOT_MEMORY_FORMAT_UEFI_RAW`. The phase-1 kernel validates the handoff, parses
the DTB, starts SMP, and then idles. It has no architecture-owned interpretation
of which physical memory may be allocated.

The existing `kernel/memory/pmm.c` cannot simply be called as the next step:

- the AArch64 kernel Makefile intentionally compiles only `arch/aarch64/*`;
- `pmm.c` consumes E820 `type == 1`, puts its metadata at `PMMngr.start_brk`,
  and assumes x86 direct-map/layout behavior;
- adding it would pull in slab and further generic-kernel dependencies, turning
  this increment into a PMM port rather than a memory-map task.

The next safe seam is therefore a tested, architecture-owned normalized RAM
map. A later PMM-port specification may consume it without re-parsing UEFI
descriptors or rediscovering boot reservations.

## Goals

1. Accept only a structurally valid AArch64 raw-UEFI memory map.
2. Produce a bounded, sorted, non-overlapping list of allocatable physical
   ranges, each aligned to 2 MiB and at least 2 MiB long.
3. Never emit a range intersecting AArch64 kernel or handoff-owned physical
   memory.
4. Make malformed input fail closed before the kernel enables allocation.
5. Verify the pure transformation on the host and verify its boot-time result
   in the existing AArch64 QEMU regression path.

## Non-goals

- Calling `pmm_init()`, compiling `memory/pmm.c`, or changing PMM data
  structures.
- Allocating a physical page, building new page tables, changing TTBRs, or
  modifying the boot MMU mappings.
- Reclassifying UEFI descriptors in the boot loader or changing the v2
  `boot_context` ABI.
- Preserving reclaimable loader/boot-services pages as allocatable memory.
  This phase chooses the conservative policy of accepting only
  `EfiConventionalMemory`.

## Considered approaches

### A. Convert to E820 in the UEFI loader

This matches the x86 loader but duplicates a kernel policy in a firmware
binary, commits the ABI to an x86-shaped format, and makes kernel-reservation
subtraction awkward: the loader does not own the final kernel high-half layout
or early page-table footprint. Rejected.

### B. Teach the legacy PMM to read raw UEFI descriptors directly

This mixes descriptor validation, reservation accounting, and PMM metadata
placement into one large port. It is difficult to test independently and
cannot be safely limited to this increment. Rejected.

### C. Add an AArch64 normalized-RAM boundary in the kernel

The normalizer has one input (`BOOT_MEMORY_FORMAT_UEFI_RAW`) and one output
(ranges suitable for future 2 MiB PMM allocation). It is host-testable and
does not change the loader ABI. Chosen.

## Architecture

```text
raw UEFI descriptors in boot_context
              |
              v
aarch64_ram_normalize(raw descriptor bytes, metadata, exclusions, out)
  validate -> select ConventionalMemory -> subtract exclusions
  -> align inward to 2 MiB -> sort/merge -> bounded output
              |
              v
published read-only aarch64 RAM map + serial summary
              |
              v
future AArch64 PMM adapter (outside this specification)
```

The implementation is split in two. `ram_core.c` is host-linkable and contains
only the pure normalizer plus a generic one-time map-publication helper.
`ram.c` is AArch64-target-only; it validates the physical handoff window,
builds production exclusions from linker symbols, converts the descriptor
physical address through the direct map, and invokes the core. Neither module
allocates memory or touches page tables. The core decodes descriptor fields from
byte offsets with explicit little-endian fixed-width loads; it must not cast
arbitrary-stride descriptor bytes to a C structure. Final output lives in fixed
static storage owned by the AArch64 kernel image.

## Input contract

`aarch64_ram_init(const struct boot_context *handoff)` is called by
`aarch64_main()` after `boot_context_valid()` and before `gic_init()`.

It requires all of the following:

- `BOOT_CONTEXT_HAS_MEMORY_MAP` is set;
- `memory.format == BOOT_MEMORY_FORMAT_UEFI_RAW`;
- `memory.entries != 0`, `entry_count != 0`, and `entry_size` is at least
  `AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE` (32 bytes);
- `descriptor_version == 1`; version 1 is the only version this increment
  decodes;
- both `entry_count * entry_size` and `entries + entry_count * entry_size`
  are checked for overflow;
- the complete physical byte interval lies in
  `[AARCH64_HANDOFF_BASE, AARCH64_TRAMPOLINE_BASE)`, whose exact constants are
  exported by a new AArch64 handoff-layout header shared by the loader and
  kernel;
- every selected descriptor has a non-zero page count and
  `NumberOfPages * 4096` and `PhysicalStart + byte_length` do not overflow.

`AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE` is exactly 32. The byte offsets are
`Type=0`, padding=4, `PhysicalStart=8`, `VirtualStart=16`, and
`NumberOfPages=24`; no host `uintn_t`, C packing rule, or aligned load may be
used. `aarch64_ram_init()` converts the validated physical descriptor address
to the existing high-half direct map before passing bytes to the pure function.

An unsupported descriptor version, malformed descriptor geometry, or an
invalid required map is fatal. Unknown UEFI memory *types* are unavailable and
ignored, just like known non-conventional types. The failure message begins
`UEFI-A64: RAM map invalid` and the BSP halts with interrupts disabled. No
partial map is published.

## Selection and reservation policy

Only descriptors whose `Type == AARCH64_EFI_CONVENTIONAL_MEMORY` are
candidates. `ram_core.h` defines that kernel-owned wire constant as `7u`, the
UEFI `EfiConventionalMemory` type value; the core must not include UEFI loader
headers. All other descriptor types are unavailable, including loader,
boot-services, and runtime-services memory. This avoids accidentally releasing
UEFI allocations whose lifetime may still matter to this early kernel.

Candidates are subtracted against these closed-open physical intervals:

| Reservation | Source |
|---|---|
| Loaded kernel image | `[(uint64_t)(uintptr_t)_boot_start, (uint64_t)(uintptr_t)_kernel_lma_end)`; both are physical linker values and must be checked as monotonic |
| UEFI handoff allocation | `[0x401e0000, 0x40200000)` |
| Early boot page tables and boot per-CPU stacks | already contained in the loaded-kernel interval above; no separate conditional reservation exists |
| DTB and raw descriptor buffer | contained by the handoff allocation |

Subtraction happens before alignment. Every surviving fragment is rounded up
at its start and down at its end to `AARCH64_RAM_GRANULE`; fragments with `end <=
start` are discarded. This explicitly sacrifices up to 2 MiB at each boundary
in return for the present PMM's allocation granularity.

## Pure normalization and output contract

`ram_core.c` exposes this pure, allocation-free function for host tests and
for the boot wrapper through the internal
`kernel/arch/aarch64/ram_core.h` header:

```c
#define AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE 32u
#define AARCH64_EFI_CONVENTIONAL_MEMORY 7u
#define AARCH64_RAM_GRANULE (UINT64_C(1) << 21)

struct aarch64_ram_interval { uint64_t start, end; };

int aarch64_ram_normalize(const uint8_t *bytes, uint32_t entry_count,
                          uint32_t entry_size, uint32_t format,
                          uint32_t descriptor_version,
                          const struct aarch64_ram_interval *exclude,
                          uint32_t exclude_count,
                          struct aarch64_ram_map *out);
```

`AARCH64_RAM_GRANULE` is the only 2 MiB size constant used by `ram_core.c` and
`ram.c`; neither file includes `kernel/pmm.h` or depends on legacy PMM types.

It accepts a host pointer only after its caller has established byte-range
accessibility. A zero `entry_count`, `entry_size < 32`, a format other than
`BOOT_MEMORY_FORMAT_UEFI_RAW`, `descriptor_version != 1`, a null byte or output
pointer, an invalid exclusion (`end <= start`), arithmetic overflow, or more
than 16 post-merge ranges returns a distinct negative error code. `out` is
zeroed on every failure when `out` is non-null. It decodes all descriptors
before publishing any range.

`ram_core.c` also defines `aarch64_ram_publish_once()` in that same internal
header, so the C runner may invoke it without linking `ram.c`. It takes an
explicit candidate map, destination map, and initialized flag. Before copying,
it validates that candidate `count` is in `1..AARCH64_RAM_MAX_RANGES` and every
range is non-empty, 2 MiB aligned, strictly ordered, and separated from its
neighbours; otherwise it returns a negative error and changes neither state nor
destination. It copies only a valid complete candidate, and returns `-EALREADY`
without changing the destination on a second call. The boot wrapper validates
the physical handoff range, creates exactly the kernel and handoff exclusions
above, calls the pure function into a stack-local map, and gives that candidate
to this helper. Its published pointer is null until that final copy.

The normalizer must use no dynamic allocation and no descriptor-sized temporary
array. It performs an O(n²), O(1)-scratch repeated scan: for the next output
range, scan every descriptor and each of its fragments after exclusion and
inward 2 MiB alignment to select the lowest fragment beginning at or after the
previous emitted end. Set that as the tentative range, then rescan all
fragments repeatedly, expanding its end for every overlapping or touching
fragment, until a full scan makes no expansion. Emit that final merged range
and repeat. Only after 16 final merged ranges have been emitted may it scan for
another; that successful scan is the capacity error. This rule prevents an
early false overflow when a later, out-of-order descriptor bridges two prior
fragments. The output object is cleared before processing and remains cleared
on every error.

Add `kernel/include/kernel/arch/aarch64/ram.h` with the following public
interface:

```c
#define AARCH64_RAM_MAX_RANGES 16

struct aarch64_ram_range {
    uint64_t start; /* inclusive physical address, 2 MiB aligned */
    uint64_t end;   /* exclusive physical address, 2 MiB aligned */
};

struct aarch64_ram_map {
    uint32_t count;
    struct aarch64_ram_range ranges[AARCH64_RAM_MAX_RANGES];
};

int aarch64_ram_init(const struct boot_context *handoff);
const struct aarch64_ram_map *aarch64_ram_map_get(void);
```

`aarch64_ram_init()` returns zero only when it has published the complete map.
It returns a negative value on invalid input or when more than 16 disjoint,
post-merge ranges remain. The boot caller treats every non-zero return as
fatal. `aarch64_ram_map_get()` returns a non-null pointer after successful
initialization; callers must treat it as immutable.

The output is sorted by ascending `start`, contains no empty ranges, and has
strict gaps: adjacent or overlapping fragments are merged. The maximum is 16
instead of the legacy PMM's 32 because this is an API-specific bounded static
object; overflow is a detectable boot failure, never silent truncation.

`aarch64_main()` logs exactly one success summary:

```text
UEFI-A64: RAM ranges=<n> pages2m=<n> bytes=<n>
```

Values are base-10 unsigned integers; `pages2m` is the sum of
`(end - start) / AARCH64_RAM_GRANULE`, and `bytes == pages2m *
AARCH64_RAM_GRANULE`.
They describe the published map after exclusion and alignment.

## Error handling

- Invalid handoff/map metadata: fatal before GIC, SMP, or timer setup.
- No usable post-alignment RAM: fatal with `UEFI-A64: RAM map invalid`.
- Output capacity exhaustion: fatal with the same stable prefix and a reason
  suffix identifying `too many ranges`.
- Descriptor types not selected by policy: ignored, not errors.

The module must have no fallback to guessed QEMU RAM bounds and no reliance on
the descriptor order returned by firmware.

## Testing and acceptance

Add `tests/aarch64_ram_test.py`, which compiles `ram_core.c` plus a C runner
using the host C compiler and invokes the runner. It must not link `ram.c`,
linker symbols, or AArch64 inline assembly. It uses synthetic descriptor bytes
with both the minimum descriptor size and a larger, deliberately unaligned
stride. It must prove:

1. only `EfiConventionalMemory` survives;
2. unsorted candidates become sorted and adjacent candidates merge;
3. 2 MiB inward alignment drops sub-2 MiB fragments;
4. kernel/handoff reservations split and remove candidates correctly;
5. malformed stride, overflow, zero map, invalid format, and output-capacity
   overflow fail without publishing a partial map;
6. every published range is 2 MiB aligned, non-empty, disjoint, and outside
  all exclusions and `AARCH64_RAM_GRANULE` aligned.
7. failed core calls leave the output zeroed, and direct tests of
   `aarch64_ram_publish_once()` reject invalid candidates and return
   `-EALREADY` on a second valid publication attempt without changing the
   published map.

Extend the AArch64 QEMU regression parser and its self-test fixtures to require
exactly one syntactically valid success summary for every normal one-, two-,
and four-CPU path, and for the injected no-ACK path. The no-ACK parser must
also require its established SMP DEGRADED status after the RAM summary,
proving the new pre-GIC validation did not alter recovery behavior.

Acceptance is:

```bash
make PROFILE=aarch64-clang aarch64-uefi-kernel
python3 tests/aarch64_ram_test.py
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi
```

The normal matrix and injected no-ACK command must retain their established
PASS/DEGRADED evidence and add a valid RAM summary for every successful boot.

## Follow-on boundary

A later, separate specification may add an adapter from
`struct aarch64_ram_map` to an AArch64-safe PMM. It must explicitly decide
where PMM metadata resides, which generic memory/slab sources become part of
the AArch64 link, and how the direct map covers allocated physical pages.
Those decisions are intentionally not implied by this document.
