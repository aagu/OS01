# Arch-Neutral Physical Memory Manager Init Design

## Status

Proposed. This document defines one PMM-architecture increment: make
`kernel/memory/pmm_init()` work on AArch64 by introducing an arch-neutral
memory-range representation and per-arch adapters, while preserving every
existing public allocator entry point on x86_64. It is a prerequisite for
the long-term goal of merging x86_64 and AArch64 system entries; this
specification does **not** itself perform that merge.

## Problem

Today there are two parallel bring-up paths for physical memory, and only
one of them reaches the PMM.

- **x86_64**: `kernel/kernel/main.c` calls `pmm_init(const struct BOOT_MEMORY_MAP *map)`.
  `pmm.c` reads the E820 entries directly from the boot context, hard-codes
  `type == 1` for usable RAM, copies the entries into the global
  `PMMngr.e820_entrys[32]`, builds `bits_map` / `pages_struct` /
  `zones_struct` from `start_brk` onward, partitions zones around a fixed
  4 GiB boundary, and trusts E820 ordering to keep kernel/handoff memory
  out of the allocatable set.
- **AArch64**: `aarch64_main()` calls `aarch64_ram_init(boot_context)`. The
  raw UEFI descriptor stream is fed through the pure normalizer in
  `ram_core.c` and published as an immutable `aarch64_ram_map`; the legacy
  `pmm_init()` is never called, so `PMMngr` is never populated on AArch64.
  Any consumer of `alloc_pages()`, `alloc_4k_page()`, or the COW helpers
  fails on AArch64 today.

The two paths duplicate the same policy in two different languages (E820
type field vs UEFI `EfiConventionalMemory`), two different normalization
strategies (none on x86_64, repeated scan on AArch64), and two different
exclusion strategies (implicit on x86_64, explicit kernel-LMA + handoff on
AArch64). Future boot sources (ACPI SRAT, RISC-V OpenSBI, etc.) would each
re-introduce the same duplication.

## Goals

1. Make `pmm_init()` consume a single arch-neutral representation
   (`struct memory_range[]`) on every architecture, with a stable C signature
   `void pmm_init(const struct boot_context *ctx)`.
2. Define a thin per-arch adapter (`pmm_arch_normalize`) that converts the
   boot context's native memory-map format into that representation. Reuse
   the existing AArch64 normalizer (`aarch64_ram_normalize`) verbatim as the
   UEFI descriptor adapter.
3. Preserve every public allocator entry point
   (`alloc_pages`, `free_pages`, `alloc_4k_page`, `free_4k_page`,
   `page_cow_get`, `page_cow_put`, `page_cow_refs`, `page_init`,
   `page_clean`) byte-for-byte.
4. Reproduce the AArch64 explicit-exclusion policy on x86_64: kernel image
   LMA and the boot handoff window are subtracted before any range becomes
   allocatable.
5. Make the 4 GiB `ZONE_UNMAPPED_INDEX` boundary arch-supplied instead of
   hard-coded inside `pmm_init`, so AArch64 (which identity-maps all DRAM at
   the current `ARCH_PAGE_OFFSET`) reports the boundary above its highest
   RAM range.
6. Verify the new path on x86_64 with the existing systest/nettest suites,
   on AArch64 with the existing QEMU virt UEFI SMP profile plus an
   extended evidence requirement, and on the host with a synthetic-fixture
   unit test compiled from the adapter source.

## Non-goals

- Performing the long-term merge of x86_64 and AArch64 system entries
  beyond PMM. The boot sequence, exception-vector code, syscall entry, and
  SMP bring-up are explicitly out of scope and remain per-arch.
- Changing the 2 MiB allocation granule, the `bits_map` / `pages_struct` /
  `zones_struct` layout, or any consumer of those layouts.
- Replacing `arch_task_init_early` or any other SMP entry with an arch-neutral
  path.
- Reclassifying UEFI descriptor types, re-defining the `boot_context` v2
  ABI, or moving the UEFI raw format handling into `kernel/memory/`.
- Decoding ESR_EL1 / FAR_EL1 sync exceptions (the existing `trap.c`
  placeholder) or wiring the Generic Timer driver into the kernel tick
  path.
- Calling `aarch64_ram_init` from x86_64, or vice versa.

## Considered approaches

### A. Rename `aarch64_ram_normalize` to a shared module and route both arches through it

The existing normalizer is UEFI-specific (it walks type-7 descriptors with
`AARCH64_EFI_CONVENTIONAL_MEMORY`). Forcing x86_64 E820 through it would
require fabricating a UEFI descriptor stream from E820 entries, which leaks
UEFI terminology into the x86_64 path and complicates future ACPI SRAT
support. Rejected.

### B. Change `pmm_init` signature to take `struct memory_range *` directly

This pushes the conversion burden onto every caller. Today there is exactly
one caller (`kernel_main`); the long-term merge goal is to make
`boot_context` the only handoff object between bootloader and kernel.
Keeping `pmm_init` consuming `boot_context` directly matches that
direction and centralises the conversion in one place. Rejected.

### C. Introduce an arch-neutral internal representation and per-arch adapters

`pmm_init(boot_context)` calls a per-arch adapter that produces a
`memory_range[]`; the body of `pmm_init` walks that array uniformly. The
AArch64 adapter is a thin wrapper around the existing
`aarch64_ram_normalize`. The x86_64 adapter is a new translation from E820
to `memory_range[]` with explicit excludes. The legacy `E820` internal
struct, the 32-entry cap, and the hard-coded 4 GiB zone split all
disappear from the body of `pmm_init`. Chosen.

## Architecture

```text
bootloader (per-arch) -> boot_context v2 with .memory.{entries,format,...}
                              |
                              v
kernel/kernel/main.c
   pmm_init(&boot_context)               // single entry point
                              |
                              v
kernel/memory/pmm.c
   pmm_arch_normalize(boot_context, &out_ranges)
                              |
              +---------------+---------------+
              |                               |
              v                               v
kernel/arch/x86_64/pmm_arch.c       kernel/arch/aarch64/pmm_arch.c
   E820 -> memory_range[]           aarch64_ram_normalize(...)
   + kernel LMA exclude             -> memory_range[]
   + handoff window exclude         (kernel LMA + handoff window already
                                     excluded inside the normalizer)
                              |
                              v
kernel/memory/pmm.c
   walk memory_range[] -> build bits_map, pages_struct, zones_struct
   -> zones sized by total RAM
   -> ZONE_UNMAPPED split at pmm_arch_zone_split()
                              |
                              v
existing public surface: alloc_pages, free_pages, alloc_4k_page,
free_4k_page, page_cow_{get,put,refs}, page_init, page_clean
```

The adapter boundary is a function-pointer table or a per-arch `.c` file
picked at link time. AArch64 keeps its pure normalizer as a separate
host-linkable translation unit; only the thin glue that converts its
output to `memory_range[]` is new.

## Input contract

`pmm_init(const struct boot_context *ctx)` is called from
`kernel/kernel/main.c` after `boot_context_valid(ctx)` returns true and
before any allocator consumer (`frame_buffer_init`, scheduler bring-up,
`init`, etc.). It requires:

- `ctx != 0`, `boot_context_valid(ctx)` is true;
- `(ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) != 0`;
- `ctx->memory.entries != 0`, `ctx->memory.entry_count != 0`,
  `ctx->memory.entry_size >= MEMORY_RANGE_DESCRIPTOR_MIN_SIZE`
  (32 bytes, matching `AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE` from the
  upstream spec);
- `ctx->memory.format` is one of `BOOT_MEMORY_FORMAT_E820` or
  `BOOT_MEMORY_FORMAT_UEFI_RAW`.

On invalid input, `pmm_init` logs a fatal line and halts the BSP with
interrupts disabled. The same behaviour holds on every architecture.

The function returns `void`. It must succeed exactly once per boot. The
body must not be re-entered; a duplicate call is a developer bug and
asserts in debug builds.

## New types

`kernel/include/kernel/memory_map.h` defines the arch-neutral surface:

```c
#define MEMORY_RANGE_MAX            64u
#define MEMORY_RANGE_DESCRIPTOR_MIN_SIZE 32u

typedef enum {
    MEMORY_TYPE_RAM          = 1u,   /* usable */
    MEMORY_TYPE_RESERVED     = 2u,   /* firmware-reserved */
    MEMORY_TYPE_ACPI_RECLAIM = 3u,   /* reclaimable after ACPI init */
    MEMORY_TYPE_ACPI_NVS     = 4u,   /* non-volatile ACPI */
    MEMORY_TYPE_DEVICE       = 5u,   /* MMIO: GIC, UART, framebuffer, ... */
    MEMORY_TYPE_KERNEL       = 6u,   /* loaded kernel image LMA (excluded) */
    MEMORY_TYPE_HANDOFF      = 7u,   /* boot handoff window (excluded) */
} memory_type_t;

struct memory_range {
    uint64_t       phys_start;   /* inclusive, granule-aligned */
    uint64_t       phys_end;     /* exclusive, granule-aligned */
    memory_type_t  type;
};
```

Invariants enforced by `pmm_arch_normalize`:

- `out_count <= MEMORY_RANGE_MAX`.
- Ranges are sorted by `phys_start` ascending.
- No empty range (`phys_end > phys_start`).
- Ranges are disjoint and strictly ordered (`out[i].phys_end <=
  out[j].phys_start` for `i < j`).
- Each range is `MEMORY_RANGE_GRANULE` aligned at both ends, where
  `MEMORY_RANGE_GRANULE == (1u << PAGE_2M_SHIFT) == 0x200000`.

## New conversion layer

`kernel/memory/pmm_arch.c` is the single dispatch site; per-arch
implementations live in `kernel/arch/<arch>/pmm_arch.c` and are linked
unconditionally. The default weak fallback is x86_64; AArch64 overrides it
via the build profile.

```c
/* kernel/memory/pmm_arch.c */
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct memory_range *out,
                          size_t out_cap);

uint64_t pmm_arch_zone_split(void);
/* x86_64 returns 0x100000000ULL (4 GiB).
 * AArch64 returns SIZE_MAX so all RAM is ZONE_NORMAL. */
```

### x86_64 adapter (`kernel/arch/x86_64/pmm_arch.c`)

Translates the existing E820 bring-up to `memory_range[]`:

| E820 type | `memory_range.type` |
|---|---|
| 1 (Usable) | `MEMORY_TYPE_RAM` |
| 2 (Reserved) | `MEMORY_TYPE_RESERVED` |
| 3 (ACPI Reclaim) | `MEMORY_TYPE_ACPI_RECLAIM` |
| 4 (ACPI NVS) | `MEMORY_TYPE_ACPI_NVS` |
| other | `MEMORY_TYPE_RESERVED` |

After type mapping, the adapter subtracts two exclude intervals:

- kernel image LMA: from per-arch linker symbols `_kernel_load_start` and
  `_kernel_load_end` (must exist for both archs; x86_64 already has them);
- boot handoff window: a constant pair supplied by the x86_64 handoff
  layout (`X86_64_HANDOFF_BASE`, `X86_64_HANDOFF_END`).

Surviving fragments are rounded inward to `MEMORY_RANGE_GRANULE`; empty
fragments are dropped. Output is sorted and merged. The adapter must cap
its output at `MEMORY_RANGE_MAX` and return `0` on capacity exhaustion
(treated as fatal by `pmm_init`).

### AArch64 adapter (`kernel/arch/aarch64/pmm_arch.c`)

A thin wrapper around `aarch64_ram_normalize`:

1. Build excludes for the kernel image and handoff window using
   `aarch64_boot_image_start_addr()` /
   `aarch64_kernel_lma_end_addr()` and the existing
   `AARCH64_HANDOFF_BASE` / `AARCH64_HANDOFF_END` constants. This matches
   the present-day behaviour of `aarch64_ram_init` exactly.
2. Call `aarch64_ram_normalize` into a stack-local `aarch64_ram_map`.
3. For each emitted range in the normalized map, append a
   `struct memory_range` with `type = MEMORY_TYPE_RAM`. Treat
   `AARCH64_RAM_ERR_CAPACITY` as fatal (matches the existing fatal
   handling).
4. `pmm_arch_zone_split()` returns `SIZE_MAX`.

The pure normalizer in `kernel/arch/aarch64/ram_core.c` is unchanged. The
existing boot-time wrapper `aarch64_ram_init` is kept as-is: its caller
chain (`aarch64_main` → `aarch64_ram_init`) continues to publish the
normalized map for legacy consumers, and the new adapter adds the
`memory_range[]` publication on top. (If the boot-time wrapper becomes a
no-op in a future spec, that is explicitly out of scope here.)

## `pmm_init` rewrite

The new body is a uniform walk over `memory_range[]`:

1. Iterate `out_count` from the adapter; sum `phys_end - phys_start` for
   `MEMORY_TYPE_RAM` ranges into `TotalMem`.
2. Sanity-check `TotalMem > 0`; fatal otherwise.
3. Allocate `bits_map`, `pages_struct`, `zones_struct` from
   `PMMngr.start_brk` exactly as today (the BSS-end self-allocation is
   already VA-relative via `Virt_To_Phy` and works on AArch64).
4. For each `MEMORY_TYPE_RAM` range, create one `Zone` with the same
   initialization as today: `zone_start_address`, `zone_end_address`,
   `page_using_count`, `page_free_count`, `pages_group` derived from the
   global `pages_struct` base + `(start >> PAGE_2M_SHIFT)` offset.
5. Mark kernel/handoff pages from `PMMngr.end_of_struct` to the top of the
   kernel image as `PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel`,
   matching the existing `pmm_init` loop semantics.
6. Compute `ZONE_DMA_INDEX`, `ZONE_NORMAL_INDEX`, `ZONE_UNMAPPED_INDEX`
   using `pmm_arch_zone_split()` as the threshold. On AArch64 no zone
   crosses `SIZE_MAX`, so the existing zone-iteration loop leaves
   `ZONE_UNMAPPED_INDEX` at its initial value (0) and `ZONE_NORMAL_INDEX`
   at `zones_size - 1`; this matches x86_64 with no high-RAM zones and
   keeps `ZONE_NORMAL_INDEX` a valid index. The threshold constant
   itself is unchanged on x86_64 (`0x100000000ULL`).
7. Call `slab_init()` and `list_init(&subpage_pools)` as today.

`alloc_pages`, `free_pages`, `alloc_4k_page`, `free_4k_page`,
`page_cow_get`, `page_cow_put`, `page_cow_refs`, `page_init`, and
`page_clean` are not modified.

## Error handling

- Invalid handoff / unsupported `memory.format` / zero entry_count: fatal
  with `[smp] FATAL: pmm_init invalid handoff` and halt BSP with
  interrupts disabled. The message prefix is shared across arches; the
  reason suffix is format-specific.
- Adapter returns `0` or `out_count > MEMORY_RANGE_MAX`: fatal with
  `[smp] FATAL: pmm_arch_normalize returned no ranges`.
- AArch64 normalizer returns `AARCH64_RAM_ERR_CAPACITY`: fatal with
  `[smp] FATAL: normalizer rejected UEFI map (too many ranges)`.
- AArch64 normalizer returns any other negative code: fatal with
  `[smp] FATAL: normalizer rejected UEFI map`.
- `TotalMem == 0` after the range walk: fatal with
  `[smp] FATAL: no usable RAM after exclusions`.

All paths must leave `PMMngr` in the same state as before the call on
failure (no partial zones, no partial `bits_map`). In debug builds,
failure paths also assert this invariant with
`_Static_assert`-friendly checks; the production build simply halts.

## Files touched

| File | Change |
|---|---|
| `kernel/include/kernel/memory_map.h` | **NEW** — `memory_type_t`, `struct memory_range`, `MEMORY_RANGE_MAX`, `MEMORY_RANGE_GRANULE`, `MEMORY_RANGE_DESCRIPTOR_MIN_SIZE` |
| `kernel/include/kernel/pmm.h` | `pmm_init` signature becomes `pmm_init(const struct boot_context *)`; drop `struct E820` (legacy); keep `Physical_Memory_Manager`, `Zone`, `Page`; add `MEMORY_RANGE_GRANULE` shim |
| `kernel/memory/pmm.c` | Rewrite `pmm_init` body to walk `memory_range[]`; remove E820-specific code; remove hard-coded 4 GiB split |
| `kernel/memory/pmm_arch.c` | **NEW** — arch dispatch (`pmm_arch_normalize`, `pmm_arch_zone_split`) |
| `kernel/arch/x86_64/pmm_arch.c` | **NEW** — E820 to `memory_range[]` translation with kernel-LMA + handoff excludes |
| `kernel/arch/aarch64/pmm_arch.c` | **NEW** — wraps `aarch64_ram_normalize`, translates to `memory_range[]` |
| `kernel/kernel/main.c` | Update call site from `pmm_init(&boot_ctx->memory)` to `pmm_init(boot_ctx)` |
| `kernel/include/kernel/bootinfo.h` | Keep `BOOT_MEMORY_MAP`, `BOOT_MEMORY_FORMAT_*`; add comment pointing to `memory_map.h` for the arch-neutral type |
| `Makefile` / kernel Makefiles | Add `kernel/memory/pmm_arch.c`, `kernel/arch/<arch>/pmm_arch.c` to the relevant profile lists |

Files **not** modified:

- `kernel/arch/aarch64/ram.c`, `ram_core.c`, `ram.h`, `boot_offsets.h` — kept
  verbatim. They remain the boot-time source of truth for the normalized
  map that legacy consumers (if any) read via `aarch64_ram_map_get()`.
- `kernel/arch/aarch64/head.S`, `entry.S` — kept verbatim. Their existing
  identity-map of MMIO and VBAR_EL1 install is unaffected.
- Any file under `kernel/arch/x86_64/` other than the new
  `pmm_arch.c`. The trampoline, GDT/IDT, AP boot, and lapic code remain
  x86_64-owned.

## Testing and acceptance

Add `tests/pmm_arch_test.py` (host-side, mirrors `tests/aarch64_ram_test.py`):

- compiles `kernel/memory/pmm_arch.c`, the per-arch `pmm_arch.c` selected
  by `PROFILE`, and a small C runner using the host C compiler;
- links against `kernel/arch/aarch64/ram_core.c` only when the AArch64
  profile is selected (the existing pure normalizer);
- runs synthetic fixtures:
  - empty boot context, malformed `memory.format`, zero `entry_count`,
    `entry_size < 32` → all return zero ranges;
  - minimal E820 (one type-1 entry covering `[0, 0x40000000)`) →
    exactly one `MEMORY_TYPE_RAM` range with `phys_start == 0`,
    `phys_end == 0x40000000`, after kernel/handoff excludes are applied;
  - E820 with a type-2 reserved hole → `MEMORY_TYPE_RESERVED` plus
    surrounding `MEMORY_TYPE_RAM` ranges, all granule-aligned;
  - UEFI_RAW with three type-7 descriptors plus two excludes → one to
    three `MEMORY_TYPE_RAM` ranges depending on overlap;
  - every output range passes the invariants from the "New types"
    section.

Extend the AArch64 QEMU UEFI test profile to assert:

- `pmm_init` returns successfully (the new `[smp] FATAL` prefixes must
  not appear);
- `alloc_pages(ZONE_NORMAL, 1, 0)` followed by `free_pages(p, 1)` succeeds
  and round-trips the physical address;
- `alloc_4k_page()` returns a physical address inside the published
  `aarch64_ram_map` and `free_4k_page(phys)` succeeds.

The x86_64 systest suite must remain green:

- `systest 268/268` (per the symlink-support spec baseline);
- `nettest 6/6`.

The AArch64 no-ACK regression
(`AARCH64_SMP_TEST_NO_ACK_CPU=1`, per `be6e6e1`) must still produce its
established `DEGRADED` status.

Acceptance is:

```bash
# x86_64
make PROFILE=x86_64-clang x86_64-kernel
python3 tests/systest.py
python3 tests/nettest.py
make PROFILE=x86_64-clang x86_64-uefi

# AArch64
make PROFILE=aarch64-clang aarch64-uefi-kernel
python3 tests/aarch64_ram_test.py
python3 tests/pmm_arch_test.py
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack

# All profiles
python3 tests/pmm_arch_test.py
```

The host unit test and the AArch64 QEMU extension are the new
acceptance bars; x86_64 is the regression bar.

## Follow-on boundary

A later, separate specification may:

- unify the per-arch `aarch64_main` / `kernel_main` boot sequences behind
  a single arch-neutral `kernel_main`;
- rebase the AArch64 bring-up so that `pmm_init` is the single source of
  truth and `aarch64_ram_init` becomes a thin shim that only feeds the
  legacy `aarch64_ram_map_get()` reader;
- migrate the AArch64 identity-map of MMIO into a virtual-memory layout
  with a direct map plus high-half kernel, in line with the long-term
  merge goal;
- replace the 2 MiB granule PMM with a 4 KiB / 2 MiB hybrid allocator
  that preserves the existing public surface.

This specification does not commit to any of those directions and must
not be extended to absorb them.
