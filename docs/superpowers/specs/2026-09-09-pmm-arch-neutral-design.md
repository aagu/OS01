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
   (`struct MEMORY_RANGE[]`) on every architecture, with a stable C
   signature `void pmm_init(const struct boot_context *ctx)`.
2. Define a thin per-arch adapter (`pmm_arch_normalize`) that converts
   the boot context's native memory-map format into that representation.
   On AArch64, the adapter reads the already-published
   `aarch64_ram_map` from `aarch64_ram_map_get()` (single source of
   truth; the boot-time `aarch64_ram_init` continues to publish it).
   On x86_64, the adapter translates E820 entries directly.
3. Preserve every public allocator entry point (`alloc_pages`,
   `free_pages`, `alloc_4k_page`, `free_4k_page`, `page_cow_get`,
   `page_cow_put`, `page_cow_refs`, `page_init`, `page_clean`)
   byte-for-byte.
4. Reproduce the AArch64 explicit-exclusion policy on x86_64: kernel
   image LMA, the boot handoff window, and the SMP trampoline region are
   subtracted before any range becomes allocatable.
5. Make the 4 GiB `ZONE_UNMAPPED_INDEX` boundary arch-supplied instead
   of hard-coded inside `pmm_init`, so AArch64 (which identity-maps all
   DRAM at the current `ARCH_PAGE_OFFSET`) reports the boundary above
   its highest RAM range.
6. Verify the new path on x86_64 with the existing systest/nettest
   suites, on AArch64 with the existing QEMU virt UEFI SMP profile plus
   an extended evidence requirement, and on the host with a synthetic
   fixture unit test compiled from the adapter source.

## Non-goals

- Performing the long-term merge of x86_64 and AArch64 system entries
  beyond PMM. The boot sequence, exception-vector code, syscall entry,
  and SMP bring-up are explicitly out of scope and remain per-arch.
- Changing the 2 MiB allocation granule, the `bits_map` /
  `pages_struct` / `zones_struct` layout, or any consumer of those
  layouts.
- Replacing `arch_task_init_early` or any other SMP entry with an
  arch-neutral path.
- Reclassifying UEFI descriptor types, re-defining the `boot_context` v2
  ABI, or moving the UEFI raw format handling into `kernel/memory/`.
- Decoding ESR_EL1 / FAR_EL1 sync exceptions (the existing `trap.c`
  placeholder) or wiring the Generic Timer driver into the kernel tick
  path.
- Calling `aarch64_ram_init` from x86_64, or vice versa.
- Removing `struct E820_ENTRY` from `bootinfo.h` (still used by the
  bootloader); only the legacy internal `struct E820` plus the
  `e820_entrys[32]` field in `Physical_Memory_Manager` are removed.

## Considered approaches

### A. Rename `aarch64_ram_normalize` to a shared module and route both arches through it

The existing normalizer is UEFI-specific (it walks type-7 descriptors
with `AARCH64_EFI_CONVENTIONAL_MEMORY`). Forcing x86_64 E820 through it
would require fabricating a UEFI descriptor stream from E820 entries,
which leaks UEFI terminology into the x86_64 path and complicates future
ACPI SRAT support. Rejected.

### B. Change `pmm_init` signature to take `struct MEMORY_RANGE *` directly

This pushes the conversion burden onto every caller. Today there is
exactly one caller (`kernel_main` on x86_64); the long-term merge goal
is to make `boot_context` the only handoff object between bootloader
and kernel. Keeping `pmm_init` consuming `boot_context` directly
matches that direction and centralises the conversion in one place.
Rejected.

### C. Introduce an arch-neutral internal representation and per-arch adapters

`pmm_init(boot_context)` calls a per-arch adapter that produces a
`MEMORY_RANGE[]`; the body of `pmm_init` walks that array uniformly. The
AArch64 adapter reads the already-published `aarch64_ram_map`. The
x86_64 adapter is a new translation from E820 to `MEMORY_RANGE[]` with
explicit kernel-LMA + handoff + trampoline excludes. The legacy
internal `struct E820` and the `e820_entrys[32]` field disappear from
`Physical_Memory_Manager`; the hard-coded 4 GiB zone split becomes an
arch-supplied value. Chosen.

## Architecture

```text
bootloader (per-arch) -> boot_context v2 with .memory.{entries,format,...}
                              |
                              v
caller (per-arch)
   x86_64: kernel/kernel/main.c
   aarch64: kernel/arch/aarch64/main.c (NEW: adds pmm_init call)
                              |
                              v
pmm_init(&boot_context)        // single entry point, kernel/memory/pmm.c
                              |
                              v
kernel/memory/pmm_arch.c       // weak default symbols
   pmm_arch_normalize(boot_context, &out_ranges)   // __attribute__((weak))
   pmm_arch_zone_split(void)                        // __attribute__((weak))
                              |
              +---------------+---------------+
              |                               |
              v                               v
kernel/arch/x86_64/pmm_arch.c  kernel/arch/aarch64/pmm_arch.c
   E820 -> MEMORY_RANGE[]       reads aarch64_ram_map_get()
   excludes:                    -> MEMORY_RANGE[]
     - kernel image LMA          (aarch64_ram_init already subtracted
     - handoff window             kernel LMA + handoff window when it
     - SMP trampoline             published the map)
                              |
                              v
kernel/memory/pmm.c
   walk MEMORY_RANGE[] -> build bits_map, pages_struct, zones_struct
   -> zones sized by total RAM
   -> ZONE_UNMAPPED split at pmm_arch_zone_split()
                              |
                              v
existing public surface: alloc_pages, free_pages, alloc_4k_page,
free_4k_page, page_cow_{get,put,refs}, page_init, page_clean
```

The adapter dispatch uses GCC `__attribute__((weak))`:

- `kernel/memory/pmm_arch.c` defines `pmm_arch_normalize` and
  `pmm_arch_zone_split` as **weak** symbols that implement the x86_64
  behaviour as a default.
- `kernel/arch/x86_64/pmm_arch.c` and `kernel/arch/aarch64/pmm_arch.c`
  define them as **strong** symbols; the per-arch strong definition wins
  over the weak default at link time.

This makes x86_64 the "free" default and gives the aarch64 build a
single, obvious override site. The Makefile wiring is symmetric: every
profile that builds `kernel/memory/pmm.c` also builds
`kernel/memory/pmm_arch.c`.

## Input contract

`pmm_init(const struct boot_context *ctx)` is called from
`kernel/kernel/main.c` on x86_64 and from `kernel/arch/aarch64/main.c`
on AArch64, after `boot_context_valid(ctx)` returns true and before any
allocator consumer. It requires:

- `ctx != 0`, `boot_context_valid(ctx)` is true;
- `(ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) != 0`;
- `ctx->memory.entries != 0`, `ctx->memory.entry_count != 0`;
- `ctx->memory.format` is one of `BOOT_MEMORY_FORMAT_E820` or
  `BOOT_MEMORY_FORMAT_UEFI_RAW`;
- `ctx->memory.entry_size` is at least the **per-format minimum**:
  - `BOOT_MEMORY_FORMAT_E820`: `entry_size >= sizeof(struct E820_ENTRY)`
    (20 bytes; the size of the bootloader-produced record);
  - `BOOT_MEMORY_FORMAT_UEFI_RAW`: `entry_size >= 32` (matching
    `AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE`).

A single global `entry_size >= 32` check would reject every E820 boot,
because `struct E820_ENTRY` is 20 bytes. The check must therefore be
format-specific. The adapter itself may additionally enforce a tighter
stride if needed.

The function returns `void`. A real (non-assert) guard inside `pmm.c`
enforces single-call semantics and is set only on the successful path:

```c
#include <kernel/arch/cpu.h>   /* arch_cpu_halt */
static int pmm_initialized;
void pmm_init(const struct boot_context *ctx) {
    if (pmm_initialized) {
        log_err("[smp] FATAL: pmm_init called twice\n");
        arch_cpu_halt();          /* never returns */
    }
    /* ... validate, build bits_map/pages_struct/zones_struct ... */
    pmm_initialized = 1;          /* only on success */
}
```

On invalid input or any internal error, `pmm_init` logs a fatal line
prefixed with `[smp] FATAL: pmm_init ...` and halts the BSP with
interrupts disabled. The same behaviour holds on every architecture.

## New types

`kernel/include/kernel/memory_map.h` defines the arch-neutral surface:

```c
#define MEMORY_RANGE_MAX             64u
#define MEMORY_RANGE_GRANULE         (1u << 21)   /* 2 MiB, matches PAGE_2M_SIZE */

enum MEMORY_TYPE {
    MEMORY_TYPE_RAM          = 1u,   /* usable */
    MEMORY_TYPE_RESERVED     = 2u,   /* firmware-reserved */
    MEMORY_TYPE_ACPI_RECLAIM = 3u,   /* reclaimable after ACPI init */
    MEMORY_TYPE_ACPI_NVS     = 4u,   /* non-volatile ACPI */
    MEMORY_TYPE_DEVICE       = 5u,   /* MMIO: GIC, UART, framebuffer, ... */
};

struct MEMORY_RANGE {
    uint64_t        phys_start;   /* inclusive, granule-aligned */
    uint64_t        phys_end;     /* exclusive, granule-aligned */
    enum MEMORY_TYPE type;
};
```

Invariants enforced by `pmm_arch_normalize`:

- `1 <= out_count <= MEMORY_RANGE_MAX`.
- Ranges are sorted by `phys_start` ascending.
- No empty range (`phys_end > phys_start`).
- Ranges are disjoint and strictly ordered (`out[i].phys_end <=
  out[j].phys_start` for `i < j`).
- Each range is `MEMORY_RANGE_GRANULE` aligned at both ends.

Excluded regions (kernel LMA, handoff window, trampoline) are
**subtracted** from the input and never appear in the output. There is
no `MEMORY_TYPE_KERNEL` or `MEMORY_TYPE_HANDOFF` enum value because the
adapter expresses exclusion through fragmentation, not classification.

## New conversion layer

```c
/* kernel/memory/pmm_arch.c — weak default (x86_64 behaviour). */
__attribute__((weak))
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out);

__attribute__((weak))
uint64_t pmm_arch_zone_split(void);
```

The contract is one-way: `pmm_arch_normalize` returns either 0 (fatal
input) or `n` with `1 <= n <= MEMORY_RANGE_MAX`. There is no `out_cap`
parameter; the bound is fixed by `MEMORY_RANGE_MAX`.

`pmm_arch_zone_split` returns the address above which a zone is
classified `ZONE_UNMAPPED`. x86_64 returns `0x100000000ULL` (4 GiB,
preserving historical behaviour). AArch64 returns `SIZE_MAX` so no zone
is unmapped.

### x86_64 adapter (`kernel/arch/x86_64/pmm_arch.c`)

Strong overrides of both weak symbols. Translates E820 entries to
`MEMORY_RANGE[]`:

| E820 type | `MEMORY_RANGE.type` |
|---|---|
| 1 (Usable) | `MEMORY_TYPE_RAM` |
| 2 (Reserved) | `MEMORY_TYPE_RESERVED` |
| 3 (ACPI Reclaim) | `MEMORY_TYPE_ACPI_RECLAIM` |
| 4 (ACPI NVS) | `MEMORY_TYPE_ACPI_NVS` |
| other | `MEMORY_TYPE_RESERVED` |

After type mapping, the adapter subtracts three closed-open intervals:

- **kernel image LMA**: derived from the x86_64 linker symbols
  `_text` and `_edata` via `Virt_To_Phy()`. The x86_64 linker script
  places these at high-half virtual addresses
  (`0xffff800000100000 + offset`); `Virt_To_Phy` is the existing
  macro (`vaddr - 0xffff800000000000`) that converts them to the
  physical addresses the bootloader actually loaded the kernel at.
  Concretely:
  ```c
  extern char _text[], _edata[];
  uint64_t lma_start = Virt_To_Phy((uint64_t)&_text);
  uint64_t lma_end   = Virt_To_Phy((uint64_t)&_edata);
  ```
  `_text`/`_edata` are declared in the new
  `kernel/include/kernel/arch/x86_64/handoff_layout.h`; `Virt_To_Phy`
  is the existing x86_64 macro. (Subtracting the raw VMAs
  `0xffff800000100000+...` directly would be a no-op — the values are
  above any physical RAM range.)
- **boot handoff window**: `X86_64_HANDOFF_BASE = 0x60000`,
  `X86_64_HANDOFF_END = 0x64000`, declared in the new
  `kernel/include/kernel/arch/x86_64/handoff_layout.h`. These values
  mirror the bootloader's `X86_HANDOFF_BASE = 0x60000` and
  `X86_HANDOFF_PAGES = 4` defined in
  `boot/uefi/arch/x86_64/boot.c:13-20`.
- **SMP trampoline region**: `[TRAMPOLINE_BASE, TRAMPOLINE_BASE +
  sizeof(trampoline_bin))`, where `TRAMPOLINE_BASE = 0x8000` and the
  size comes from the embedded trampoline blob symbols
  (`_binary_arch_x86_64_trampoline_bin_start` / `_end`) declared in
  `kernel/arch/x86_64/trampoline.h`.

Surviving fragments are rounded inward to `MEMORY_RANGE_GRANULE`; empty
fragments are dropped. Output is sorted and merged. Capacity
exhaustion (more than `MEMORY_RANGE_MAX` disjoint, granule-aligned
RAM ranges after exclusion) returns 0.

The weak default in `kernel/memory/pmm_arch.c` is byte-for-byte the
same x86_64 implementation, so a build that forgets to include the
per-arch file still gets correct x86_64 behaviour. The strong
per-arch override exists so future per-arch tweaks (e.g. an aarch64
override of zone split) have an obvious landing site even on x86_64.

### AArch64 adapter (`kernel/arch/aarch64/pmm_arch.c`)

Strong override of `pmm_arch_normalize`; the default
`pmm_arch_zone_split` is **kept** (returns `SIZE_MAX`) without an
override.

The AArch64 adapter **reads the already-published `aarch64_ram_map`**
rather than re-running the normalizer. Call ordering is enforced by
the boot path: `aarch64_main()` calls `aarch64_ram_init(handoff)`
*first* (publishing the map with kernel-LMA + handoff excludes
already applied by the existing normalizer), then sets up the
`PMMngr.start_brk` fields, then calls `pmm_init(handoff)`.

Pseudocode for the adapter:

```c
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;  /* map already published by aarch64_ram_init */
    const struct aarch64_ram_map *m = aarch64_ram_map_get();
    if (m == NULL)
        return 0;  /* caller violated ordering */
    size_t n = m->count;
    if (n == 0 || n > MEMORY_RANGE_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        out[i].phys_start = m->ranges[i].start;
        out[i].phys_end   = m->ranges[i].end;
        out[i].type       = MEMORY_TYPE_RAM;
    }
    return n;
}
```

The pure normalizer in `kernel/arch/aarch64/ram_core.c` is unchanged.
The existing boot-time wrapper `aarch64_ram_init` is kept as-is; its
caller chain continues to publish the normalized map for legacy
consumers. (If the boot-time wrapper becomes a no-op in a future spec,
that is explicitly out of scope here.)

## `pmm_init` rewrite

The new body is a uniform walk over `MEMORY_RANGE[]`. The
x86_64-specific `pages_struct[0] = phys 0` quirk is preserved as a
pre-loop step that runs only when the very first RAM range starts at
physical address zero (the historical x86_64 layout after E820 entry 0
covers `[0, 0x9f000)`). For aarch64, no range starts at physical zero,
so the quirk is a no-op.

```text
1. Call pmm_arch_normalize(ctx, scratch_ranges).
   - If 0 returned, fatal with "[smp] FATAL: pmm_arch_normalize returned no ranges".
2. Iterate scratch_ranges; sum (phys_end - phys_start) for MEMORY_TYPE_RAM
   into TotalMem. Track total_pages_link = total / MEMORY_RANGE_GRANULE.
3. Sanity-check TotalMem > 0; fatal otherwise.
4. Allocate bits_map, pages_struct, zones_struct from PMMngr.start_brk
   exactly as today (BSS-end self-allocation; Virt_To_Phy on the caller).
5. For each MEMORY_TYPE_RAM range:
   a. round start up and end down to MEMORY_RANGE_GRANULE;
      if end <= start, skip.
   b. create one Zone { start, end, page_free_count = (end-start)/granule,
      pages_group = pages_struct + (start >> PAGE_2M_SHIFT),
      manager_struct = &PMMngr, ... }.
   c. for j in 0..pages_length: pages_group[j] = { zone=z, phy_address =
      start + j*GRANULE, attribute=0, reference_count=0, age=0 };
      toggle bits_map bit free.
6. If pages_struct[0].phy_address == 0 (legacy x86_64 layout):
      pages_struct[0].attribute |= PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel;
      pages_struct[0].reference_count = 1;
7. Mark kernel-owned pages: for j in 1..(Virt_To_Phy(end_of_struct) >> 21):
      tmp = pages_struct + j;
      page_init(tmp, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
      bits_map[bit] = 1; tmp->zone->page_using_count++; page_free_count--;
8. Compute ZONE_DMA_INDEX, ZONE_NORMAL_INDEX, ZONE_UNMAPPED_INDEX using
   pmm_arch_zone_split() as the threshold. The initial values
   ZONE_DMA_INDEX=0, ZONE_NORMAL_INDEX=zones_size-1, ZONE_UNMAPPED_INDEX=0
   are preserved; the post-loop scan only updates ZONE_UNMAPPED_INDEX and
   ZONE_NORMAL_INDEX if a zone crosses the threshold (true only on x86_64
   with high-RAM zones). On AArch64, no zone crosses SIZE_MAX, so the
   initial values stick.
9. slab_init(); list_init(&subpage_pools).
```

Step 6 is conditional (`pages_struct[0].phy_address == 0`), so the
x86_64 historical layout is preserved while the aarch64 layout (which
does not start at physical zero) does not get a fake page at address 0.

Step 4 relies on `PMMngr.start_brk` being set by the caller before
`pmm_init`. Today only `kernel/kernel/main.c:155-159` sets it; the
spec adds the equivalent prelude in `kernel/arch/aarch64/main.c` so
that `aarch64_main` populates `PMMngr.start_code`, `end_code`,
`end_data`, `end_rodata`, and `start_brk` from the aarch64 VMA
linker symbols before calling `pmm_init`. Concretely:

```c
extern char _text_start[], _text_end[];
extern char _rodata_start[], _rodata_end[];
extern char _data_start[], _data_end[];
extern char _kernel_end[];

PMMngr.start_code  = (uint64_t)&_text_start;
PMMngr.end_code    = (uint64_t)&_text_end;
PMMngr.end_data    = (uint64_t)&_data_end;
PMMngr.end_rodata  = (uint64_t)&_rodata_end;
PMMngr.start_brk   = (uint64_t)&_kernel_end;
```

These are the VMA symbols already defined in
`kernel/arch/aarch64/linker.ld:83-119` (note: aarch64 does **not**
define `_text`/`_edata`/`_end` — those names are x86_64-only). The
values are high-half VMAs; `pmm.c` later calls `Virt_To_Phy` on
`start_brk`-derived pointers, so storing VMAs is correct (matches the
x86_64 convention). Do **not** use the LMA helpers
(`aarch64_boot_image_start_addr` / `aarch64_kernel_lma_end_addr`)
here — those return physical addresses and would put `start_brk` in
the wrong address space, breaking step 7.

`alloc_pages`, `free_pages`, `alloc_4k_page`, `free_4k_page`,
`page_cow_get`, `page_cow_put`, `page_cow_refs`, `page_init`, and
`page_clean` are not modified.

## Error handling

- Invalid handoff / unsupported `memory.format` / zero `entry_count` /
  `entry_size < 32`: fatal with `[smp] FATAL: pmm_init invalid handoff`
  and halt BSP with interrupts disabled.
- `pmm_arch_normalize` returns 0: fatal with
  `[smp] FATAL: pmm_arch_normalize returned no ranges`.
- AArch64 normalizer reports `AARCH64_RAM_ERR_CAPACITY` during the
  boot-time `aarch64_ram_init` call: fatal there (already implemented
  in `ram.c`); `pmm_init` never sees it.
- AArch64 normalizer reports any other negative error during
  `aarch64_ram_init`: fatal there; `pmm_init` never sees it.
- `TotalMem == 0` after the range walk: fatal with
  `[smp] FATAL: no usable RAM after exclusions`.

All paths must leave `PMMngr` in the same state as before the call on
failure (no partial zones, no partial `bits_map`). The
`pmm_initialized` guard is set to 1 only on the successful path.

## Files touched

| File | Change |
|---|---|
| `kernel/include/kernel/memory_map.h` | **NEW** — `enum MEMORY_TYPE`, `struct MEMORY_RANGE`, `MEMORY_RANGE_MAX`, `MEMORY_RANGE_GRANULE` (no `MEMORY_RANGE_DESCRIPTOR_MIN_SIZE` macro; per-format minimums are checked in `pmm_init` directly) |
| `kernel/include/kernel/memory.h` | Update `pmm_init` declaration to `void pmm_init(const struct boot_context *ctx)` |
| `kernel/include/kernel/pmm.h` | `pmm_init` declaration removed (now in `memory.h`); `struct E820` declaration (currently lines 35–40) removed; `e820_entrys[32]` field removed from `Physical_Memory_Manager`; remaining public surface: `PAGE_*_SHIFT/SIZE/MASK/ALIGN`, `ZONE_*` macros, page attribute flags, `Physical_Memory_Manager`, `Zone`, `Page`, and the public allocator entry points |
| `kernel/memory/pmm.c` | Rewrite `pmm_init` body per §"`pmm_init` rewrite"; add real `pmm_initialized` guard. **7 `color_printk` call sites** in the current code are: `get_page_attribute` (1), `set_page_attribute` (1), `alloc_pages` (3), `free_pages` (2). `page_init`/`page_clean` have no `color_printk` calls today. The private-helper calls (`get_page_attribute`, `set_page_attribute`) are replaced with arch-portable `log_err` from `kernel/arch/boot_log.h` (single-string signature, 2 sites). The public `alloc_pages`/`free_pages` `color_printk` calls (5 sites) are **kept as-is** to preserve the public surface byte-for-byte; they resolve at link time against `kernel/kernel/printk.c`'s `color_printk` on x86_64 and against `kernel/arch/aarch64/printk_stub.c`'s forwarder on aarch64 (see that row). |
| `kernel/memory/pmm_arch.c` | **NEW** — weak default `pmm_arch_normalize` and `pmm_arch_zone_split` (x86_64 behaviour) |
| `kernel/arch/x86_64/pmm_arch.c` | **NEW** — strong override; E820 to `MEMORY_RANGE[]` translation with kernel-LMA + handoff + trampoline excludes |
| `kernel/arch/x86_64/handoff_layout.h` | **NEW** (path: `kernel/include/kernel/arch/x86_64/handoff_layout.h`) — declares `X86_64_HANDOFF_BASE`, `X86_64_HANDOFF_END`, and `extern char _text[], _edata[]` |
| `kernel/arch/aarch64/pmm_arch.c` | **NEW** — strong override of `pmm_arch_normalize` only; reads `aarch64_ram_map_get()` |
| `kernel/arch/aarch64/printk_stub.c` | **NEW** — `color_printk(...)` forwarder that vsprintf's its variadic args into a static 256-byte buffer and calls `kputs(buf)`. Cannot forward directly to `log_err` because `log_err(const char *)` takes a single string (the aarch64 signature in `kernel/arch/boot_log.h`), while `color_printk` is variadic. Required because the public `alloc_pages`/`free_pages` in `pmm.c` keep their 5 `color_printk` calls (preserved by Goal 3), and the existing `kernel/kernel/printk.c` cannot link on aarch64 (references `_binary_kernel_font_psf_start`, framebuffer `Pos.FB_addr`). |
| `kernel/arch/aarch64/slab_stub.c` | **NEW** — `slab_init()` no-op (and `kmalloc`/`kfree`/`kzalloc`/`ksize` stubs in case any later TU links against them). Required because `kernel/memory/slab.c` has x86-only references at **file scope outside** `slab_init`: `slab_lock_acquire` (L38, `pushfq; popq %0; cli` inline asm), `slab_lock_release` (L54, `sti` inline asm), the `RFLAGS_IF` macro `(1UL << 9)`, and 8 `color_printk` calls. Guarding only `slab_init` leaves the rest un-compilable on aarch64. The actual approach: wrap `slab.c` body in `#ifdef __x86_64__ ... #endif` and provide the aarch64 stub TU here. |
| `kernel/arch/aarch64/main.c` | Insertion order is pinned: after `aarch64_ram_init(handoff)` returns successfully → set `PMMngr.start_*`/`end_*`/`start_brk` prelude (5 lines) → `pmm_init(handoff)` → `#if OS01_SELFTEST` smoke-test block (alloc/free roundtrip) → `dtb_init(handoff)` → `gic_init(handoff)` → `smp_boot_aps(handoff)` → `arch_tick_start()` → halt. The smoke-test block sits between `pmm_init` and `dtb_init` so it runs only when `pmm_init` is known-good, and `dtb_init`/`gic_init`/`smp_boot_aps` (the first allocator consumers on aarch64) follow it. |
| `kernel/kernel/main.c` | Update call site from `pmm_init(&bootctx->memory)` to `pmm_init(bootctx)` |
| `kernel/include/kernel/bootinfo.h` | Keep `BOOT_MEMORY_MAP`, `BOOT_MEMORY_FORMAT_*`, `E820_ENTRY`; add comment pointing to `memory_map.h` for the arch-neutral type |
| `kernel/Makefile` | x86_64 branch: `$(wildcard memory/*.c)` already picks up `pmm_arch.c`; aarch64 branch: the current `KERNEL_C_SOURCES :=` is empty, so the patch **creates** the list: ```make ifeq ($(ARCH),aarch64) KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c memory/slab.c ARCH_C_SOURCES += $(ARCHDIR)/printk_stub.c endif ``` The wildcard `$(ARCH_C_SOURCES := $(wildcard $(ARCHDIR)/*.c))` already picks up `pmm_arch.c` and `printk_stub.c`. This makes future `kernel/memory/*.c` additions require an explicit Makefile update; a follow-up could move back to a wildcard. |
| `mk/components/run.mk` | `test-aarch64-uefi-smp` rule must pass `KERNEL_SELFTEST=1` so the new `#if OS01_SELFTEST` block is compiled in. Mirror `test-kernel-selftest` at `run.mk:350`. |
| `tests/aarch64_uefi_smp.py` | Add `args.expect_selftest` to the existing argparse (around line 436–446); thread it into both `passed()` and `degraded_passed()`. The `passed()` predicate requires: if `args.expect_selftest` is true and the log contains the success summary `UEFI-A64: RAM ranges=...`, then the log MUST contain `UEFI-A64: pmm alloc smoke OK` **between that summary line and the first `[smp] topology source=uefi-dtb cpus=` line**. Anchoring on the specific topology line (rather than a naive `[smp]` substring search) avoids false-positives on `[smp-test] FATAL: ...` lines that appear later in the log. The `degraded_passed()` path is unchanged and never requires the smoke line (the no-ACK build does not pass `KERNEL_SELFTEST=1`). |

Files **not** modified:

- `kernel/arch/aarch64/ram.c`, `ram_core.c`, `ram.h`, `handoff_layout.h`
  — kept verbatim. They remain the boot-time source of truth for the
  normalized map that legacy consumers (if any) read via
  `aarch64_ram_map_get()`.
- `kernel/arch/aarch64/head.S`, `entry.S` — kept verbatim. Their
  existing identity-map of MMIO and VBAR_EL1 install is unaffected.
- Any file under `kernel/arch/x86_64/` other than the new
  `pmm_arch.c` and `handoff_layout.h`. The trampoline, GDT/IDT, AP
  boot, and lapic code remain x86_64-owned.

## Testing and acceptance

Add `tests/pmm_arch_test.py` (host-side, mirrors
`tests/aarch64_ram_test.py`):

- compiles `kernel/memory/pmm_arch.c`, the per-arch `pmm_arch.c`
  selected by `PROFILE`, and a small C runner using the host C
  compiler;
- links against `kernel/arch/aarch64/ram_core.c` when the AArch64
  profile is selected (so the runner can publish a synthetic
  `aarch64_ram_map` before invoking the adapter).

**External symbols the host TU must satisfy** (enumerated so the
implementer does not discover them at link time):

For the **x86_64 adapter**, the host TU must provide:

- `_text`, `_edata` — `char` arrays (the kernel's `_text[]`/`_edata[]`
  linker symbols). The stub defines them with
  `_text = (char*)0xffff800000200000;` and
  `_edata = (char*)0xffff800000300000;` so `Virt_To_Phy` yields a
  1 MiB `[0x200000, 0x300000)` exclude. Combined with the test
  fixture's E820 type-1 entry spanning `[0, 0x40000000)`, the
  adapter must produce two MEMORY_RANGE fragments — `[0, 0x200000)`
  (2 MiB) and `[0x300000, 0x40000000)` — so the test exercises the
  granule-aligned multi-fragment output path, not just the
  single-range happy path;
- `Virt_To_Phy` — the existing x86_64 macro
  (`(vaddr) - 0xffff800000000000UL`). If the host TU cannot pull in
  the macro (because the x86_64 kernel headers are not host-safe),
  the runner compiles a tiny inline definition: `#define Virt_To_Phy(v) ((v) - 0xffff800000000000UL)`;
- `_binary_arch_x86_64_trampoline_bin_{start,end}` — the embedded
  trampoline blob symbols. The stub declares them with the same
  signature as `kernel/include/kernel/arch/x86_64/trampoline.h`
  (`extern char _binary_arch_x86_64_trampoline_bin_start[]; extern
  char _binary_arch_x86_64_trampoline_bin_end[];`) and defines them
  as `_binary_arch_x86_64_trampoline_bin_start =
  _binary_arch_x86_64_trampoline_bin_end = (char*)0;` so the
  trampoline exclude interval is empty;
- `X86_64_HANDOFF_BASE`, `X86_64_HANDOFF_END` — the stub overrides
  these to fixed test values. **Concrete values that exercise the
  two-fragment output path** are `X86_64_HANDOFF_BASE = 0x204000`
  and `X86_64_HANDOFF_END = 0x208000`. The handoff sits inside the
  kernel-LMA exclude gap `[0x200000, 0x300000)`, so it does not
  perturb the surviving fragments `[0, 0x200000)` and
  `[0x300000, 0x40000000)`. (Using `0x100000`/`0x104000` would
  shrink the first fragment to `[0, 0x100000)`, which then rounds
  to empty under 2 MiB granule alignment, leaving only one
  fragment — i.e. would silently fail to exercise the multi-fragment
  path.)

For the **AArch64 adapter**, the host TU needs only:

- `aarch64_ram_init`, `aarch64_ram_map_get`, `aarch64_ram_normalize`,
  `aarch64_ram_publish_once` — all already provided by the linked
  `ram_core.c`. No stubs needed.

The C runner (`tests/pmm_arch_test_runner.c`) shape:

```c
#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern size_t pmm_arch_normalize(const struct boot_context *,
                                  struct MEMORY_RANGE *);
extern uint64_t pmm_arch_zone_split(void);

/* For AArch64: aarch64_ram_init is called by the runner to publish a
 * synthetic map before invoking pmm_arch_normalize. */

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    return 1; } } while (0)

int main(void) {
    struct MEMORY_RANGE out[MEMORY_RANGE_MAX];

    /* Invalid format -> 0 ranges. */
    {
        struct boot_context ctx = {0};
        CHECK(pmm_arch_normalize(&ctx, out) == 0);
    }
    /* Minimal E820 single type-1 entry spanning low RAM. */
    {
        struct E820_ENTRY e[] = { { .address = 0, .length = 0x40000000,
                                     .type = 1 } };
        struct boot_context ctx = { .magic = BOOT_CONTEXT_MAGIC,
                                     .version = BOOT_CONTEXT_VERSION,
                                     .size = sizeof(ctx),
                                     .flags = BOOT_CONTEXT_HAS_MEMORY_MAP,
                                     .memory = { .entries = (uintptr_t)e,
                                                 .entry_count = 1,
                                                 .entry_size = sizeof(struct E820_ENTRY),
                                                 .format = BOOT_MEMORY_FORMAT_E820 } };
        size_t n = pmm_arch_normalize(&ctx, out);
        CHECK(n >= 1);
        for (size_t i = 0; i < n; i++) {
            CHECK(out[i].phys_end > out[i].phys_start);
            CHECK((out[i].phys_start & (MEMORY_RANGE_GRANULE - 1)) == 0);
            CHECK((out[i].phys_end & (MEMORY_RANGE_GRANULE - 1)) == 0);
            CHECK(out[i].type == MEMORY_TYPE_RAM);
        }
    }
    /* E820 with reserved hole. */
    /* UEFI_RAW with three type-7 descriptors (AArch64 profile only). */

    return 0;
}
```

The runner must also exercise `pmm_arch_zone_split` (assert
`pmm_arch_zone_split() >= 0x100000000ULL` on x86_64, `== SIZE_MAX` on
AArch64).

Extend the AArch64 QEMU UEFI test profile to assert:

- `pmm_init` returns successfully (no `[smp] FATAL: pmm_init ...` lines
  appear);
- a new `OS01_SELFTEST`-gated block in `kernel/arch/aarch64/main.c`
  emits a single known log line after `pmm_init` returns, e.g.
  `UEFI-A64: pmm alloc smoke OK`. The block uses the **macro**
  `ZONE_NORMAL` (matching every other call site in the kernel:
  `kernel/memory/slab.c:71`, `pmm.c:480`, `driver/ahci.c:214`), **not**
  the runtime variable `ZONE_NORMAL_INDEX` — the runtime variable is
  the index of `ZONE_NORMAL` within `PMMngr.zones_struct`, not the
  bit-flag value `alloc_pages` switches on:
  ```c
  #if OS01_SELFTEST
      struct Page *p = alloc_pages(ZONE_NORMAL, 1, 0);
      if (p) { free_pages(p, 1); log_info("UEFI-A64: pmm alloc smoke OK\n"); }
      else   { log_err("UEFI-A64: pmm alloc smoke FAIL\n"); }
  #endif
  ```
  The block is enabled by the `KERNEL_SELFTEST=1` make-variable
  knob; `kernel/Makefile:171-174` defines `-DOS01_SELFTEST=1` only
  when `KERNEL_SELFTEST=1` is set. The `test-aarch64-uefi-smp` make
  rule in `mk/components/run.mk:161-170` must be updated to pass
  `KERNEL_SELFTEST=1` (mirroring `test-kernel-selftest`'s convention
  at `run.mk:350`), otherwise the block is compiled out and the
  parser assertion fails. The parser
  (`tests/aarch64_uefi_smp.py:98-115,131-144`) is extended to
  accept/require the new log line.

The x86_64 systest suite must remain green:

- `systest 268/268` (per the symlink-support spec baseline);
- `nettest 6/6`.

The AArch64 no-ACK regression
(`AARCH64_SMP_TEST_NO_ACK_CPU=1`, per `be6e6e1`) must still produce
its established `DEGRADED` status.

Acceptance is:

```bash
# x86_64
make PROFILE=x86_64-clang x86_64-kernel
python3 tests/pmm_arch_test.py
python3 tests/systest.py
python3 tests/nettest.py

# AArch64
make PROFILE=aarch64-clang aarch64-uefi-kernel
python3 tests/aarch64_ram_test.py
python3 tests/pmm_arch_test.py
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 test-aarch64-uefi-smp
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
```

`KERNEL_SELFTEST=1` on the aarch64 lines is required so the new
smoke-test block in `kernel/arch/aarch64/main.c` is compiled in; the
no-ACK line must remain without `KERNEL_SELFTEST` to match the
established `DEGRADED` regression evidence.

The host unit test and the AArch64 QEMU extension are the new
acceptance bars; x86_64 is the regression bar.

## Follow-on boundary

A later, separate specification may:

- unify the per-arch `aarch64_main` / `kernel_main` boot sequences
  behind a single arch-neutral `kernel_main`;
- rebase the AArch64 bring-up so that `pmm_init` is the single source
  of truth and `aarch64_ram_init` becomes a thin shim that only feeds
  the legacy `aarch64_ram_map_get()` reader;
- migrate the AArch64 identity-map of MMIO into a virtual-memory
  layout with a direct map plus high-half kernel, in line with the
  long-term merge goal;
- replace the 2 MiB granule PMM with a 4 KiB / 2 MiB hybrid allocator
  that preserves the existing public surface.

This specification does not commit to any of those directions and
must not be extended to absorb them.
