# PMM Arch-Neutral Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `kernel/memory/pmm_init()` work on AArch64 by introducing an arch-neutral `MEMORY_RANGE[]` representation and per-arch adapters, while preserving every existing public allocator entry point byte-for-byte on x86_64.

**Architecture:** `pmm_init(boot_context)` consumes a uniform `MEMORY_RANGE[]` produced by a per-arch adapter (`pmm_arch_normalize`). Weak default `pmm_arch_normalize` returns 0 (fatal); weak default `pmm_arch_zone_split` returns `SIZE_MAX`. x86_64 strong-override translates E820 entries; aarch64 strong-override reads the already-published `aarch64_ram_map`. The PMM body uses RAM-relative indexing (`pages_struct + ((start - lowest_ram) >> 21)`) and clamps the kernel-image walk to skip aarch64 (where kernel LMA is excluded from RAM). A second commit chain unifies the `log_err/warn/info` macros behind a single `kernel/log.h` with gate-wrapped per-arch `_log_*_impl` functions.

**Tech Stack:** GNU Make + Clang/LLVM, freestanding aarch64 (-nostdlib), x86_64 UEFI bootloader, aarch64 QEMU virt SMP regression harness. C11 freestanding + minimal GNU extensions. Host-side host C compiler for unit tests.

**Spec:** `docs/superpowers/specs/2026-09-09-pmm-arch-neutral-design.md`

---

## Global Constraints

These constraints come from the spec and apply to every task. Read them before starting any task.

1. **Branch discipline**: create a git worktree for this work before editing any source file (per project memory: "use git worktrees"). The plan runs on a feature branch; do not commit to master.
2. **Commit messages**: end every commit with `Co-Authored-By: Claude <noreply@anthropic.com>`. Use `git -c user.email=claude@anthropic.com -c user.name=Claude commit ...`.
3. **Build**: `make PROFILE=x86_64-clang kernel.bin` and `make PROFILE=aarch64-clang aarch64-uefi-kernel` must both succeed after each task that touches shared code. (There is no `x86_64-kernel` target — the canonical x86_64 artifact is `kernel.bin` or `kernel.elf`.)
4. **No libc on aarch64**: aarch64 builds use `-nostdlib`. No `vsnprintf`/`vsprintf`/`memset`/`memcpy` from libc. All helpers must be in-tree or skipped.
5. **Naming**: kernel-wide types are UPPER_CASE (`MEMORY_RANGE`, `MEMORY_TYPE`). Per-arch types remain snake_case (existing convention).
6. **Style**: 4-space indent in C, ALL_CAPS for enums and macros, lowercase with underscores for functions and variables. Comments are sentence case with a period.
7. **No format-string expansion on aarch64**: `log_err/warn/info` on aarch64 calls `kputs(fmt)` and ignores extra args (per round-10 design decision).
8. **Existing public surface preserved**: `alloc_pages`, `free_pages`, `alloc_4k_page`, `free_4k_page`, `page_cow_get`, `page_cow_put`, `page_cow_refs`, `page_init`, `page_clean` must compile and behave byte-for-byte identically to today.

---

## File Structure

Files created by this plan (all paths relative to repo root):

| Path | Purpose |
|------|---------|
| `kernel/include/kernel/memory_map.h` | Arch-neutral `MEMORY_RANGE[]` types and constants |
| `kernel/memory/pmm_arch.c` | Weak default `pmm_arch_normalize` and `pmm_arch_zone_split` |
| `kernel/include/kernel/arch/x86_64/handoff_layout.h` | x86_64 handoff constants + `_text`/`_edata` externs |
| `kernel/arch/x86_64/pmm_arch.c` | x86_64 E820 → `MEMORY_RANGE[]` translator with kernel-LMA + handoff + trampoline excludes |
| `kernel/arch/aarch64/pmm_arch.c` | aarch64 reader for already-published `aarch64_ram_map` |
| `kernel/arch/aarch64/printk_stub.c` | aarch64 `color_printk` → `kputs(fmt)` forwarder |
| `kernel/arch/aarch64/memset.c` | aarch64 freestanding `memset` byte-fill loop |
| `kernel/arch/aarch64/slab_stub.c` | aarch64 `slab_init`/`kmalloc`/`kfree`/`kzalloc`/`ksize` no-op stubs |
| `kernel/arch/aarch64/log_impl.c` | aarch64 `_log_err_impl` / `_log_warn_impl` / `_log_info_impl` (→ `kputs`) |
| `tests/pmm_arch_test_runner.c` | C runner for host-side adapter tests |
| `tests/pmm_arch_test.py` | Python driver for the host-side test |

Files modified:

| Path | Change |
|------|--------|
| `kernel/include/kernel/pmm.h` | Drop `struct E820`; drop `e820_entrys[32]` field from `Physical_Memory_Manager`; drop `pmm_init` declaration |
| `kernel/include/kernel/memory.h` | Update `pmm_init` signature to `void pmm_init(const struct boot_context *ctx)` |
| `kernel/memory/pmm.c` | Rewrite `pmm_init` body with RAM-relative indexing; replace 2 private `color_printk` calls with `log_err`; add real `pmm_initialized` guard |
| `kernel/include/kernel/log.h` | Add gate-wrapped `log_err/warn/info` macros; add `_log_*_impl` prototypes; add `_log_writev` declaration |
| `kernel/kernel/log.c` | Add `_log_err_impl` / `_log_warn_impl` / `_log_info_impl` wrappers; split `_log_write` into `...` + internal `_log_writev` |
| `kernel/kernel/main.c` | Update `pmm_init` call site to `pmm_init(bootctx)` |
| `kernel/arch/aarch64/main.c` | Replace `boot_log.h` include with `kernel/log.h`; add `PMMngr` prelude; add `pmm_init(handoff)` call; add `#if OS01_SELFTEST` smoke block |
| `kernel/Makefile` | aarch64 branch: `KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c` |
| `mk/components/run.mk` | `test-aarch64-uefi-smp` rule passes `KERNEL_SELFTEST=1` |
| `tests/aarch64_uefi_smp.py` | Add `args.expect_selftest` to argparse; thread into `passed()`; anchor smoke line on `[smp] topology source=uefi-dtb cpus=` line |

---

### Task 1: Foundation types and weak default dispatcher

**Files:**
- Create: `kernel/include/kernel/memory_map.h`
- Create: `kernel/memory/pmm_arch.c`

**Interfaces:**
- Consumes: `struct boot_context` (from `kernel/include/kernel/bootinfo.h`)
- Produces:
  - `enum MEMORY_TYPE`, `struct MEMORY_RANGE`, `MEMORY_RANGE_MAX`, `MEMORY_RANGE_GRANULE` constants
  - Weak default `size_t pmm_arch_normalize(const struct boot_context *, struct MEMORY_RANGE *)` returning 0
  - Weak default `uint64_t pmm_arch_zone_split(void)` returning `SIZE_MAX`

- [ ] **Step 1: Create the memory_map.h header**

Create `kernel/include/kernel/memory_map.h` with:

```c
#ifndef _KERNEL_MEMORY_MAP_H
#define _KERNEL_MEMORY_MAP_H

#include <stdint.h>
#include <stddef.h>

#define MEMORY_RANGE_MAX      64u
#define MEMORY_RANGE_GRANULE  (1u << 21)   /* 2 MiB, matches PAGE_2M_SIZE */

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
```

- [ ] **Step 2: Create the weak default pmm_arch.c**

Create `kernel/memory/pmm_arch.c` with:

```c
/* kernel/memory/pmm_arch.c — weak default dispatcher.
 *
 * The default pmm_arch_normalize returns 0 (fatal-no-ranges); the
 * default pmm_arch_zone_split returns SIZE_MAX (no unmapped zones).
 * These defaults contain no architecture-specific symbols, so they
 * link cleanly on every architecture. Each architecture provides
 * its strong override in kernel/arch/<arch>/pmm_arch.c.
 */

#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>

__attribute__((weak))
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;
    (void)out;
    return 0;   /* fatal: caller sees 0 and halts via [smp] FATAL */
}

__attribute__((weak))
uint64_t pmm_arch_zone_split(void)
{
    return SIZE_MAX;
}
```

- [ ] **Step 3: Verify the build still works**

Run: `make PROFILE=x86_64-clang kernel.bin`
Expected: builds successfully. The x86_64 build links the weak defaults; `pmm_arch_normalize` returns 0 but is not yet called from anywhere, so the build succeeds without behavior change. systest 268/268 must still pass:

```bash
python3 tests/run_test.py systest
python3 tests/run_test.py network
```

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/memory_map.h kernel/memory/pmm_arch.c
git commit -m "pmm: add arch-neutral memory_map.h and weak default pmm_arch.c

Foundation for arch-neutral PMM init. The weak defaults return
0/SIZE_MAX and contain no arch-specific symbols, so every build
links cleanly. Per-arch strong overrides in kernel/arch/<arch>/
pmm_arch.c will replace these in subsequent tasks.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: x86_64 adapter (handoff layout + strong override)

**Files:**
- Create: `kernel/include/kernel/arch/x86_64/handoff_layout.h`
- Create: `kernel/arch/x86_64/pmm_arch.c`

**Interfaces:**
- Consumes: `struct boot_context` (with `BOOT_MEMORY_FORMAT_E820`), `struct E820_ENTRY` (`kernel/include/kernel/bootinfo.h`), `_text`/`_edata` (existing x86_64 linker symbols), `_binary_arch_x86_64_trampoline_bin_{start,end}` (`kernel/include/kernel/arch/x86_64/trampoline.h`), `Virt_To_Phy` macro (`kernel/include/kernel/memory.h`)
- Produces: strong `pmm_arch_normalize` translating E820 to `MEMORY_RANGE[]` with kernel-LMA + handoff + trampoline excludes; strong `pmm_arch_zone_split` returning `0x100000000ULL`

- [ ] **Step 1: Create the x86_64 handoff_layout.h header**

Create `kernel/include/kernel/arch/x86_64/handoff_layout.h` with:

```c
#ifndef _KERNEL_ARCH_X86_64_HANDOFF_LAYOUT_H
#define _KERNEL_ARCH_X86_64_HANDOFF_LAYOUT_H

/* Boot handoff window. Mirrors boot/uefi/arch/x86_64/boot.c
 * X86_HANDOFF_BASE = 0x60000 and X86_HANDOFF_PAGES = 4, giving
 * [0x60000, 0x64000). The kernel must subtract this from any
 * E820 RAM range before producing MEMORY_RANGE[] output. */
#define X86_64_HANDOFF_BASE  0x60000UL
#define X86_64_HANDOFF_END    0x64000UL

/* Linker symbols. Single-char style matches kernel/kernel/main.c:40-43
 * and kernel/include/kernel/task.h:48,51. */
extern char _text;
extern char _edata;

#endif /* _KERNEL_ARCH_X86_64_HANDOFF_LAYOUT_H */
```

- [ ] **Step 2: Create the x86_64 pmm_arch.c strong override**

Create `kernel/arch/x86_64/pmm_arch.c` with:

```c
/* kernel/arch/x86_64/pmm_arch.c — strong overrides for x86_64.
 *
 * Translates E820 entries to MEMORY_RANGE[] with kernel-LMA
 * (Virt_To_Phy(_text/_edata)), boot handoff (0x60000..0x64000),
 * and SMP trampoline (TRAMPOLINE_BASE..TRAMPOLINE_BASE+blob size)
 * excluded. Surviving fragments rounded inward to 2 MiB,
 * sorted/merged. Returns 0 on input error or capacity overflow.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>
#include <kernel/arch/x86_64/handoff_layout.h>
#include <kernel/arch/x86_64/trampoline.h>

extern uint64_t Virt_To_Phy(uint64_t vaddr);
extern void arch_cpu_halt(void);

#define E820_TYPE_RAM  1

static uint64_t round_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}
static uint64_t round_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

/* Subtract closed-open intervals from a MEMORY_RANGE fragment.
 * Outputs a list of surviving fragments (caller-provided buffer).
 * Returns the number of surviving fragments (0..2). */
static size_t subtract_range(uint64_t in_start, uint64_t in_end,
                             uint64_t ex_start, uint64_t ex_end,
                             uint64_t out_starts[2], uint64_t out_ends[2])
{
    size_t n = 0;
    if (in_start < ex_start && in_end > ex_start) {
        out_starts[n] = in_start;
        out_ends[n]   = (ex_end < in_end) ? ex_end : in_end;
        n++;
    }
    if (ex_end < in_end && ex_end > in_start) {
        out_starts[n] = (ex_start > in_start) ? ex_start : in_start;
        out_ends[n]   = in_end;
        n++;
    }
    (void)out_ends; (void)out_starts;
    return n;
}

__attribute__((weak))   /* override the weak default; both are weak-override-eligible */
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    if (!ctx || !out) return 0;
    if ((ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0) return 0;
    if (ctx->memory.format != BOOT_MEMORY_FORMAT_E820) return 0;
    if (ctx->memory.entry_size < sizeof(struct E820_ENTRY)) return 0;
    if (ctx->memory.entry_count == 0) return 0;

    /* Compute exclusion intervals. The kernel-LMA exclude spans
     * _text to _edata (text + rodata + data); BSS pages between
     * _edata and _end are deliberately left in the output range so
     * that Step 7's `end_of_struct` walk has somewhere to mark
     * them allocated. Excluding them with `_end` would create a
     * sparse pages_struct that the walk can't reach. */
    uint64_t kernel_lma_start = Virt_To_Phy((uint64_t)&_text);
    uint64_t kernel_lma_end   = Virt_To_Phy((uint64_t)&_edata);
    if (kernel_lma_start >= kernel_lma_end) return 0;
    uint64_t handoff_start = X86_64_HANDOFF_BASE;
    uint64_t handoff_end   = X86_64_HANDOFF_END;
    uint64_t tramp_start = TRAMPOLINE_BASE;
    uint64_t tramp_end   = TRAMPOLINE_BASE +
        ((uint64_t)&_binary_arch_x86_64_trampoline_bin_end -
         (uint64_t)&_binary_arch_x86_64_trampoline_bin_start);

    struct E820_ENTRY *entries =
        (struct E820_ENTRY *)(uintptr_t)ctx->memory.entries;
    size_t out_count = 0;

    for (uint32_t i = 0; i < ctx->memory.entry_count; i++) {
        enum MEMORY_TYPE t;
        switch (entries[i].type) {
        case 1: t = MEMORY_TYPE_RAM; break;
        case 2: t = MEMORY_TYPE_RESERVED; break;
        case 3: t = MEMORY_TYPE_ACPI_RECLAIM; break;
        case 4: t = MEMORY_TYPE_ACPI_NVS; break;
        default: t = MEMORY_TYPE_RESERVED; break;
        }
        /* pmm_init's Step 2 walks only MEMORY_TYPE_RAM ranges, so the
         * non-RAM entries below are reserved for future consumers
         * (e.g. ACPI reclaim after init). They are still emitted so the
         * full MEMORY_RANGE[] surface is available. */
        uint64_t s = entries[i].address;
        uint64_t e = entries[i].address + entries[i].length;
        /* Walk through up to 4 exclusions (kernel, handoff, trampoline) */
        struct { uint64_t s, e; } frags[8];
        size_t fcount = 1;
        frags[0].s = s; frags[0].e = e;
        const struct { uint64_t s, e; } excl[3] = {
            {kernel_lma_start, kernel_lma_end},
            {handoff_start, handoff_end},
            {tramp_start, tramp_end},
        };
        for (size_t k = 0; k < 3 && fcount > 0; k++) {
            struct { uint64_t s, e; } next[16];
            size_t ncount = 0;
            for (size_t j = 0; j < fcount; j++) {
                uint64_t a = frags[j].s, b = frags[j].e;
                if (b <= excl[k].s || a >= excl[k].e) {
                    if (ncount < 16) next[ncount++] = frags[j];
                } else {
                    if (a < excl[k].s && ncount < 16) {
                        next[ncount].s = a; next[ncount].e = excl[k].s; ncount++;
                    }
                    if (b > excl[k].e && ncount < 16) {
                        next[ncount].s = excl[k].e; next[ncount].e = b; ncount++;
                    }
                }
            }
            fcount = ncount;
            for (size_t j = 0; j < fcount; j++) frags[j] = next[j];
        }
        /* Emit surviving fragments rounded to MEMORY_RANGE_GRANULE. */
        for (size_t j = 0; j < fcount; j++) {
            uint64_t rs = round_up(frags[j].s, MEMORY_RANGE_GRANULE);
            uint64_t re = round_down(frags[j].e, MEMORY_RANGE_GRANULE);
            if (re <= rs) continue;
            if (out_count >= MEMORY_RANGE_MAX) return 0;
            out[out_count].phys_start = rs;
            out[out_count].phys_end   = re;
            out[out_count].type       = MEMORY_TYPE_RAM;
            out_count++;
        }
    }
    /* Sort by phys_start ascending. n is small (≤64); insertion sort. */
    for (size_t i = 1; i < out_count; i++) {
        struct MEMORY_RANGE tmp = out[i];
        size_t j = i;
        while (j > 0 && out[j-1].phys_start > tmp.phys_start) {
            out[j] = out[j-1]; j--;
        }
        out[j] = tmp;
    }
    /* Merge adjacent/overlapping ranges. */
    size_t w = 0;
    for (size_t i = 0; i < out_count; i++) {
        if (w == 0 || out[i].phys_start > out[w-1].phys_end) {
            out[w++] = out[i];
        } else {
            if (out[i].phys_end > out[w-1].phys_end)
                out[w-1].phys_end = out[i].phys_end;
        }
    }
    return w;
}

__attribute__((weak))   /* override the weak default */
uint64_t pmm_arch_zone_split(void)
{
    return 0x100000000ULL;   /* 4 GiB threshold */
}
```

- [ ] **Step 3: Verify x86_64 build still works**

Run: `make PROFILE=x86_64-clang kernel.bin && python3 tests/run_test.py systest && python3 tests/run_test.py network`
Expected: builds and all tests pass. The strong overrides shadow the weak defaults, but `pmm_init` doesn't yet call `pmm_arch_normalize` so behavior is unchanged.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/arch/x86_64/handoff_layout.h kernel/arch/x86_64/pmm_arch.c
git commit -m "pmm: add x86_64 strong override for pmm_arch_normalize/zone_split

Translates E820 entries to MEMORY_RANGE[] with kernel-LMA + handoff +
trampoline excludes; 2 MiB granule rounding; sort + merge; 4 GiB zone
split threshold. Backed by handoff_layout.h with single-char _text/
_edata externs matching kernel/kernel/main.c style.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: aarch64 adapter (strong override reading aarch64_ram_map)

**Files:**
- Create: `kernel/arch/aarch64/pmm_arch.c`

**Interfaces:**
- Consumes: `aarch64_ram_map_get()` (declared in `kernel/include/kernel/arch/aarch64/ram.h`), `struct boot_context` (used only for the published-map check ordering — `ctx` itself is ignored)
- Produces: strong `pmm_arch_normalize` that reads `aarch64_ram_map_get()` and copies each range into `MEMORY_RANGE[]` with `type = MEMORY_TYPE_RAM`; returns 0 if the map is not published (caller violated ordering)

- [ ] **Step 1: Create the aarch64 pmm_arch.c strong override**

Create `kernel/arch/aarch64/pmm_arch.c` with:

```c
/* kernel/arch/aarch64/pmm_arch.c — strong override for aarch64.
 *
 * Reads the already-published aarch64_ram_map (single source of truth;
 * aarch64_ram_init published it before pmm_init is called). Each emitted
 * range becomes a MEMORY_TYPE_RAM MEMORY_RANGE. The kernel-LMA + handoff
 * excludes are already baked into the published map by aarch64_ram_init.
 *
 * Returns 0 if aarch64_ram_map_get() returns NULL — caller violated
 * ordering (aarch64_ram_init must be invoked before pmm_init).
 */

#include <stddef.h>
#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>
#include <kernel/arch/aarch64/ram.h>

__attribute__((weak))   /* override the weak default */
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;
    const struct aarch64_ram_map *m = aarch64_ram_map_get();
    if (m == NULL) return 0;
    if (m->count == 0 || m->count > MEMORY_RANGE_MAX) return 0;
    for (size_t i = 0; i < m->count; i++) {
        out[i].phys_start = m->ranges[i].start;
        out[i].phys_end   = m->ranges[i].end;
        out[i].type       = MEMORY_TYPE_RAM;
    }
    return m->count;
}
```

- [ ] **Step 2: Verify aarch64 build still works (no behavior change)**

Run: `make PROFILE=aarch64-clang aarch64-uefi-kernel`
Expected: builds. The new TU compiles but nothing calls `pmm_arch_normalize` yet, so behavior is unchanged.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/aarch64/pmm_arch.c
git commit -m "pmm: add aarch64 strong override reading aarch64_ram_map

Single source of truth: the published aarch64_ram_map already has
kernel-LMA + handoff excludes applied by aarch64_ram_init. Adapter
copies ranges into MEMORY_RANGE[] as MEMORY_TYPE_RAM; returns 0 if
the map is unpublished (caller violated ordering).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Header changes — pmm.h and memory.h

**Files:**
- Modify: `kernel/include/kernel/pmm.h` (drop legacy `struct E820`, `e820_entrys[32]` field, `pmm_init` declaration)
- Modify: `kernel/include/kernel/memory.h` (update `pmm_init` declaration signature)

- [ ] **Step 1: Modify pmm.h**

In `kernel/include/kernel/pmm.h`:
- Delete the `struct E820` declaration (lines 35–40)
- Delete the `struct E820 e820_entrys[32];` field from `Physical_Memory_Manager` (line 43)
- Delete the `uint64_t e820_length;` field (line 44)
- Delete the `void pmm_init(const struct BOOT_MEMORY_MAP *map);` declaration (if present; the canonical one lives in `memory.h` per the round-2 review)
- Add `#include <kernel/memory_map.h>` if not already present (for `MEMORY_RANGE_GRANULE` consumers in this file)

- [ ] **Step 2: Update pmm_init signature in memory.h**

In `kernel/include/kernel/memory.h` (line 26 per round-2 review), change:

```c
void pmm_init(const struct BOOT_MEMORY_MAP *map);
```

to:

```c
void pmm_init(const struct boot_context *ctx);
```

- [ ] **Step 3: Verify both builds still compile (will fail at link time on x86_64 — that's expected)**

Run: `make PROFILE=x86_64-clang kernel.bin 2>&1 | head -50`
Expected: compile fails at `kernel/kernel/main.c` because the new signature doesn't match the existing call site `pmm_init(&bootctx->memory)`. **This is intentional** — Task 9 fixes the call site.

The aarch64 build should still compile (no caller of `pmm_init` there yet). Confirm:

Run: `make PROFILE=aarch64-clang aarch64-uefi-kernel 2>&1 | tail -20`
Expected: succeeds.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/pmm.h kernel/include/kernel/memory.h
git commit -m "pmm: drop legacy struct E820 + update pmm_init signature

pmm.h loses struct E820 declaration, e820_entrys[32] and e820_length
fields. memory.h updates pmm_init signature to take the full
boot_context. x86_64 build will fail until Task 9 updates main.c;
aarch64 build unaffected (no caller yet).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: aarch64 stubs — printk, memset, slab

**Files:**
- Create: `kernel/arch/aarch64/printk_stub.c`
- Create: `kernel/arch/aarch64/memset.c`
- Create: `kernel/arch/aarch64/slab_stub.c`

- [ ] **Step 1: Create printk_stub.c (color_printk → kputs)**

Create `kernel/arch/aarch64/printk_stub.c` with:

```c
/* kernel/arch/aarch64/printk_stub.c — color_printk forwarder.
 *
 * The 5 preserved color_printk call sites in pmm.c (alloc_pages ×3,
 * free_pages ×2) all pass plain string literals with no format
 * specifiers, so kputs(fmt) is sufficient. Future variadic callers
 * add a small number() helper alongside kputu rather than pulling
 * in libc vsprintf.
 */

#include <stdarg.h>
#include <kernel/arch/aarch64/boot_log.h>

void color_printk(const char *fmt, ...)
{
    /* Drop variadic args; all current callers pass plain strings. */
    kputs(fmt);
}
```

(If `kputs` is not declared in `boot_log.h`, include the header that declares it — `kernel/arch/aarch64/pl011.h` or similar. Verify with: `grep -rn "void kputs" kernel/arch/aarch64/`.)

- [ ] **Step 2: Create memset.c (freestanding byte-fill)**

Create `kernel/arch/aarch64/memset.c` with:

```c
/* kernel/arch/aarch64/memset.c — freestanding memset.
 *
 * AArch64 kernel links with -nostdlib; libc memset is unavailable.
 * This is a simple byte-fill loop. Replace with a more efficient
 * version if perf matters (this is the brief spec's call sites only).
 */

#include <stddef.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}
```

- [ ] **Step 3: Create slab_stub.c (no-op slab_init + kmalloc/kfree/kzalloc/ksize)**

Create `kernel/arch/aarch64/slab_stub.c` with signatures **matching `kernel/include/kernel/slab.h` exactly** (verify by reading the header before writing — the exact arity and return types are hard compile errors if mismatched). At the time of writing this plan the header declares:

```c
size_t slab_init(void);
void *kmalloc(size_t size);
void  kfree(const void *address);
void *kzalloc(size_t size);
size_t ksize(const void *address);
```

So the stub is:

```c
/* kernel/arch/aarch64/slab_stub.c — slab_init + allocator stubs.
 *
 * The aarch64 build does not compile kernel/memory/slab.c (it has
 * file-scope x86-only references: pushfq/sti inline asm, RFLAGS_IF
 * macro, 8 color_printk calls). This stub provides the symbols so
 * pmm.c's slab_init() / list_init() calls resolve.
 *
 * Signatures MUST mirror kernel/include/kernel/slab.h exactly.
 * Verify with: grep -n "^size_t slab_init\|^void \*kmalloc\|^void  kfree\|^void \*kzalloc\|^size_t ksize" kernel/include/kernel/slab.h
 */

#include <stddef.h>

size_t slab_init(void) { return 0; /* no-op on aarch64 */ }

void *kmalloc(size_t size)            { (void)size; return NULL; }
void  kfree(const void *address)     { (void)address; }
void *kzalloc(size_t size)           { (void)size; return NULL; }
size_t ksize(const void *address)    { (void)address; return 0; }
```

- [ ] **Step 4: Verify aarch64 build still works**

Run: `make PROFILE=aarch64-clang aarch64-uefi-kernel`
Expected: builds. The new TUs are picked up by `$(wildcard $(ARCHDIR)/*.c)` in `kernel/Makefile`.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/aarch64/printk_stub.c kernel/arch/aarch64/memset.c kernel/arch/aarch64/slab_stub.c
git commit -m "pmm: add aarch64 stubs (color_printk, memset, slab_init)

aarch64 -nostdlib link needs in-tree implementations:
- printk_stub.c: color_printk -> kputs(fmt); ignores variadic args
  (5 preserved call sites in pmm.c pass plain string literals).
- memset.c: freestanding byte-fill loop.
- slab_stub.c: no-op slab_init + kmalloc/kfree/kzalloc/ksize stubs
  (kernel/memory/slab.c not compiled on aarch64 due to file-scope
  x86-only references).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Log API unification — headers and x86_64 implementation

**Files:**
- Modify: `kernel/include/kernel/log.h`
- Modify: `kernel/kernel/log.c`

- [ ] **Step 1: Understand the current log.h/log.c layout**

Read `kernel/include/kernel/log.h` and `kernel/kernel/log.c` to confirm:
- Current `log_err/warn/info` macro form (lines 23–25 per round-10 review)
- Current `g_log_level` / `log_set_level` / `log_get_level` / `log_debug` infrastructure
- Current `_log_write` signature in `log.c` (line 64)

Do not modify until you understand the existing layout.

- [ ] **Step 2: Modify kernel/log.h to add gate-wrapped macros and prototypes**

In `kernel/include/kernel/log.h`, after the existing infrastructure, add:

```c
/* Gate-wrapped convenience macros — single source of truth on every arch.
 * The naive unwrapped form `#define log_err(...) _log_err_impl(__VA_ARGS__)`
 * is explicitly NOT used because it would bypass the g_log_level gate. */
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void _log_err_impl(const char *fmt, ...);
void _log_warn_impl(const char *fmt, ...);
void _log_info_impl(const char *fmt, ...);
void _log_writev(int level, const char *fmt, va_list args);

#define log_err(...) do { if (LOG_ERR <= g_log_level) _log_err_impl(__VA_ARGS__); } while (0)
#define log_warn(...) do { if (LOG_WARN <= g_log_level) _log_warn_impl(__VA_ARGS__); } while (0)
#define log_info(...) do { if (LOG_INFO <= g_log_level) _log_info_impl(__VA_ARGS__); } while (0)

#ifdef __cplusplus
}
#endif
```

(Use the exact `LOG_ERR` / `LOG_WARN` / `LOG_INFO` constants already defined in this file. Verify with: `grep -n "LOG_ERR\|LOG_WARN\|LOG_INFO" kernel/include/kernel/log.h`.)

- [ ] **Step 3: Modify kernel/log.c — split _log_write and add wrappers**

In `kernel/kernel/log.c`:

1. **Extract** the body of `_log_write(level, fmt, ...)` (which uses `va_start`/`va_end` and calls vsnprintf+serial) into a new static `void _log_writev(int level, const char *fmt, va_list args)`.
2. **Add** a public `void _log_writev(int level, const char *fmt, va_list args)` declaration (move it out of static if needed).
3. **Rewrite** `_log_write` as a thin forwarder:

```c
void _log_write(int level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(level, fmt, args);
    va_end(args);
}
```

4. **Add** the three thin wrappers:

```c
void _log_err_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(LOG_ERR, fmt, args);
    va_end(args);
}

void _log_warn_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(LOG_WARN, fmt, args);
    va_end(args);
}

void _log_info_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(LOG_INFO, fmt, args);
    va_end(args);
}
```

(Ensure `#include <stdarg.h>` is at the top of `log.c` if not already present.)

- [ ] **Step 4: Verify both builds**

Run: `make PROFILE=x86_64-clang kernel.bin && make PROFILE=aarch64-clang aarch64-uefi-kernel`
Expected: both build successfully. On aarch64, `_log_*_impl` are unresolved because the aarch64 build doesn't link `kernel/kernel/log.c`; the build will fail. **This is intentional** — Task 7 adds `log_impl.c` to provide them on aarch64.

Confirm the x86_64 build still passes systest/nettest:

Run: `python3 tests/run_test.py systest && python3 tests/run_test.py network`
Expected: 268/268 + 6/6 pass.

- [ ] **Step 5: Commit**

```bash
git add kernel/include/kernel/log.h kernel/kernel/log.c
git commit -m "log: unify macros with gate-wrapped _log_*_impl form

Convenience macros log_err/log_warn/log_info now expand to
per-arch _log_*_impl() calls guarded by g_log_level. On x86_64
the impls forward through the existing _log_writev (split from
the old _log_write) so format-specifier drivers (e1000,
virtio-net, pci, socket, net) keep working.

aarch64 build will fail at link time until log_impl.c is added
in the next task.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: aarch64 log_impl.c

**Files:**
- Create: `kernel/arch/aarch64/log_impl.c`

- [ ] **Step 1: Create log_impl.c**

Create `kernel/arch/aarch64/log_impl.c` with:

```c
/* kernel/arch/aarch64/log_impl.c — _log_*_impl implementations.
 *
 * AArch64 kernel links with -nostdlib; no vsnprintf. All current
 * aarch64 callers pass plain string literals with no format specifiers,
 * so kputs(fmt) is sufficient. Variadic args are ignored.
 *
 * If a future caller needs specifiers, grow a small number() helper
 * alongside kputu rather than pulling in libc.
 */

#include <stdarg.h>
#include <kernel/log.h>
#include <kernel/arch/aarch64/boot_log.h>   /* for kputs */

void _log_err_impl(const char *fmt, ...)  { va_list ap; (void)ap; (void)fmt; kputs(fmt); }
void _log_warn_impl(const char *fmt, ...) { va_list ap; (void)ap; (void)fmt; kputs(fmt); }
void _log_info_impl(const char *fmt, ...) { va_list ap; (void)ap; (void)fmt; kputs(fmt); }
```

(The `(void)va_arg` cast suppresses unused-parameter warnings; replace with `va_list ap; va_start(ap, fmt); va_end(ap);` if your compiler is stricter.)

Verify `kputs` is declared in `boot_log.h`. If not, include the right header (e.g. `kernel/arch/aarch64/pl011.h`).

- [ ] **Step 2: Verify both builds**

Run: `make PROFILE=x86_64-clang kernel.bin && make PROFILE=aarch64-clang aarch64-uefi-kernel`
Expected: both succeed.

Run x86_64 regression:
```bash
python3 tests/run_test.py systest
python3 tests/run_test.py network
```
Expected: 268/268 + 6/6 pass.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/aarch64/log_impl.c
git commit -m "log: add aarch64 _log_*_impl (kputs-only, no vsnprintf)

Provides the per-arch implementations referenced by the unified
kernel/log.h macros. All current aarch64 callers pass plain string
literals; variadic args are ignored. Matches printk_stub.c's
constraint: -nostdlib means no libc vsprintf.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Makefile wiring for aarch64 build

**Files:**
- Modify: `kernel/Makefile`

- [ ] **Step 1: Examine current aarch64 branch**

Read `kernel/Makefile` lines 40–70 to understand the aarch64 branch structure. Per round-2 review, the current state is `KERNEL_C_SOURCES :=` (empty).

- [ ] **Step 2: Add the explicit KERNEL_C_SOURCES list for aarch64**

In the `ifeq ($(ARCH),aarch64)` block, replace `KERNEL_C_SOURCES :=` with:

```make
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c
```

Confirm `kernel/arch/aarch64/printk_stub.c`, `memset.c`, `slab_stub.c`, `log_impl.c`, and `pmm_arch.c` are picked up by `$(wildcard $(ARCHDIR)/*.c)` automatically (no explicit additions needed).

- [ ] **Step 3: Verify aarch64 build still works**

Run: `make PROFILE=aarch64-clang aarch64-uefi-kernel`
Expected: builds. (At this point pmm.c still has the old body, but the new TUs link cleanly.)

- [ ] **Step 4: Commit**

```bash
git add kernel/Makefile
git commit -m "build: aarch64 KERNEL_C_SOURCES adds memory/pmm.c memory/pmm_arch.c

The wildcard $(ARCHDIR)/*.c picks up printk_stub.c, memset.c,
slab_stub.c, log_impl.c, pmm_arch.c. slab.c is intentionally NOT
included on aarch64 — it has file-scope x86-only references.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Rewrite pmm.c — RAM-relative indexing + gate-wrapped log_err + clamp

**Files:**
- Modify: `kernel/memory/pmm.c`

- [ ] **Step 1: Read the current pmm.c body**

Read `kernel/memory/pmm.c` in full to understand:
- The existing `pmm_init` body (lines 87–258 per the early exploration)
- The 7 `color_printk` call sites (lines 41, 54, 269, 297, 348, 369, 376)
- The 2 private-helper sites that will be replaced with `log_err` (`get_page_attribute:41`, `set_page_attribute:54`)
- The 5 public-surface sites to be preserved (`alloc_pages:269/297/348`, `free_pages:369/376`)
- The existing `set_page_attribute`, `page_init`, `page_clean` implementations

- [ ] **Step 2: Add `#include <kernel/bootinfo.h>`, `<kernel/log.h>`, `<kernel/arch/cpu.h>`, and `<kernel/memory_map.h>` to pmm.c**

In `kernel/memory/pmm.c`, add to the include block:

```c
#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/arch/cpu.h>     /* arch_cpu_halt — required for fatal paths */
#include <kernel/memory_map.h>
```

(`arch_cpu_halt` is called on every fatal path of the rewritten `pmm_init`; without this include the x86_64 build fails with "implicit declaration of function 'arch_cpu_halt'".)

- [ ] **Step 3: Replace color_printk in get_page_attribute and set_page_attribute with log_err**

In `get_page_attribute` (around line 41), replace:
```c
color_printk(BLACK, WHITE, "get_page_attribute() ERROR: page == NULL\n");
```
with:
```c
log_err("get_page_attribute() ERROR: page == NULL\n");
```

In `set_page_attribute` (around line 54), do the same for its error message.

**Do not** touch the 5 `color_printk` calls in `alloc_pages` / `free_pages` — those are public surface and remain byte-for-byte (they link to `printk_stub.c` on aarch64).

- [ ] **Step 4: Rewrite the pmm_init body**

Replace the entire `pmm_init` function body with:

```c
void pmm_init(const struct boot_context *ctx)
{
    static int pmm_initialized = 0;
    if (pmm_initialized) {
        log_err("[smp] FATAL: pmm_init called twice\n");
        arch_cpu_halt();
    }
    if (!ctx) { log_err("[smp] FATAL: pmm_init null ctx\n"); arch_cpu_halt(); }
    if (!boot_context_valid(ctx)) {
        log_err("[smp] FATAL: pmm_init invalid handoff\n"); arch_cpu_halt();
    }
    if ((ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0) {
        log_err("[smp] FATAL: pmm_init invalid handoff\n"); arch_cpu_halt();
    }
    /* Per-format entry_size check. */
    if (ctx->memory.format == BOOT_MEMORY_FORMAT_E820) {
        if (ctx->memory.entry_size < sizeof(struct E820_ENTRY)) {
            log_err("[smp] FATAL: pmm_init invalid handoff\n"); arch_cpu_halt();
        }
    } else if (ctx->memory.format == BOOT_MEMORY_FORMAT_UEFI_RAW) {
        if (ctx->memory.entry_size < 32u) {
            log_err("[smp] FATAL: pmm_init invalid handoff\n"); arch_cpu_halt();
        }
    } else {
        log_err("[smp] FATAL: pmm_init invalid handoff\n"); arch_cpu_halt();
    }

    /* Step 1: adapter → MEMORY_RANGE[] */
    struct MEMORY_RANGE scratch[MEMORY_RANGE_MAX];
    size_t n = pmm_arch_normalize(ctx, scratch);
    if (n == 0) {
        log_err("[smp] FATAL: pmm_arch_normalize returned no ranges\n");
        arch_cpu_halt();
    }

    /* Step 2: compute TotalMem and lowest_ram/highest_ram */
    uint64_t TotalMem = 0;
    uint64_t lowest_ram  = UINT64_MAX;
    uint64_t highest_ram = 0;
    for (size_t i = 0; i < n; i++) {
        if (scratch[i].type != MEMORY_TYPE_RAM) continue;
        uint64_t s = scratch[i].phys_start;
        uint64_t e = scratch[i].phys_end;
        TotalMem += (e - s);
        uint64_t s_aligned = s & ~(MEMORY_RANGE_GRANULE - 1);
        if (s_aligned < lowest_ram)  lowest_ram  = s_aligned;
        uint64_t e_aligned = (e + MEMORY_RANGE_GRANULE - 1) & ~(MEMORY_RANGE_GRANULE - 1);
        if (e_aligned > highest_ram) highest_ram = e_aligned;
    }
    if (TotalMem == 0) {
        log_err("[smp] FATAL: no usable RAM after exclusions\n");
        arch_cpu_halt();
    }
    uint64_t ram_span_pages = (highest_ram - lowest_ram) / MEMORY_RANGE_GRANULE;
    if (ram_span_pages == 0) ram_span_pages = 1;   /* floor 1 */

    /* Step 3: sanity */
    if (ram_span_pages == 0) {
        log_err("[smp] FATAL: no usable RAM after exclusions\n");
        arch_cpu_halt();
    }

    /* Step 4: allocate bits_map, pages_struct, zones_struct from start_brk.
     * Mirror existing pmm.c:144-161 sizing math, with the new ram_span_pages. */
    PMMngr.bits_map = (uint64_t *)((PMMngr.start_brk + 0xFFF) & ~0xFFFUL);
    PMMngr.bits_size  = ram_span_pages;
    PMMngr.bits_length = ((ram_span_pages + 63) & ~63UL) / 8;
    memset(PMMngr.bits_map, 0xff, PMMngr.bits_length);
    PMMngr.pages_struct = (struct Page *)(((uint64_t)PMMngr.bits_map + PMMngr.bits_length + 0xFFF) & ~0xFFFUL);
    PMMngr.pages_size  = ram_span_pages;
    PMMngr.pages_length = ((ram_span_pages * sizeof(struct Page) + sizeof(long) - 1) & ~(sizeof(long) - 1));
    memset(PMMngr.pages_struct, 0, PMMngr.pages_length);
    PMMngr.zones_struct = (struct Zone *)(((uint64_t)PMMngr.pages_struct + PMMngr.pages_length + 0xFFF) & ~0xFFFUL);
    PMMngr.zones_size = 0;
    PMMngr.zones_length = ((MEMORY_RANGE_MAX * sizeof(struct Zone) + sizeof(long) - 1) & ~(sizeof(long) - 1));
    memset(PMMngr.zones_struct, 0, PMMngr.zones_length);

    /* Step 5: walk RAM ranges, create zones */
    for (size_t i = 0; i < n; i++) {
        if (scratch[i].type != MEMORY_TYPE_RAM) continue;
        uint64_t start = (scratch[i].phys_start + MEMORY_RANGE_GRANULE - 1) & ~(MEMORY_RANGE_GRANULE - 1);
        uint64_t end   = scratch[i].phys_end & ~(MEMORY_RANGE_GRANULE - 1);
        if (end <= start) continue;
        if (PMMngr.zones_size >= MAX_NR_ZONES) continue;
        struct Zone *z = PMMngr.zones_struct + PMMngr.zones_size;
        PMMngr.zones_size++;
        z->zone_start_address = start;
        z->zone_end_address   = end;
        z->zone_length        = end - start;
        z->page_using_count = 0;
        z->page_free_count  = (end - start) >> 21;   /* PAGE_2M_SHIFT */
        z->total_pages_link = 0;
        z->attribute = 0;
        z->manager_struct = &PMMngr;
        z->pages_length = (end - start) >> 21;
        z->pages_group  = (struct Page *)(PMMngr.pages_struct + ((start - lowest_ram) >> 21));
        struct Page *p = z->pages_group;
        for (uint64_t j = 0; j < z->pages_length; j++, p++) {
            p->zone_struct = z;
            p->phy_address = start + ((uint64_t)j << 21);
            p->attribute = 0;
            p->reference_count = 0;
            p->age = 0;
            /* RAM-relative bit index */
            uint64_t rel_idx = (uint64_t)j;
            *(PMMngr.bits_map + (rel_idx >> 6)) ^= 1UL << (rel_idx % 64);
        }
    }

    /* end_of_struct must be assigned BEFORE Step 7, because Step 7
     * computes the kernel-image walk bound from it. Mirror the
     * computation in pmm.c:240 exactly. */
    PMMngr.end_of_struct =
        ((uint64_t)PMMngr.zones_struct + PMMngr.zones_length + sizeof(long) * 32)
        & ~(sizeof(long) - 1);

    /* Step 6: page-0 quirk (x86_64 historical) */
    if (PMMngr.pages_struct->phy_address == 0) {
        PMMngr.pages_struct->zone_struct = PMMngr.zones_struct;
        PMMngr.pages_struct->phy_address = 0UL;
        set_page_attribute(PMMngr.pages_struct,
                           PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
        PMMngr.pages_struct->reference_count = 1;
        PMMngr.pages_struct->age = 0;
    }

    /* Step 7: mark kernel-owned pages — RAM-relative walk with clamp. */
    uint64_t end_phys = Virt_To_Phy(PMMngr.end_of_struct);
    uint64_t walk_pages = (end_phys > lowest_ram)
        ? ((end_phys - lowest_ram) >> 21) : 0;
    for (uint64_t j = 1; j <= walk_pages; j++) {
        struct Page *tmp = PMMngr.pages_struct + j;
        page_init(tmp, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
        uint64_t rel_idx = (tmp->phy_address - lowest_ram) >> 21;
        *(PMMngr.bits_map + (rel_idx >> 6)) |= 1UL << (rel_idx % 64);
        tmp->zone_struct->page_using_count++;
        tmp->zone_struct->page_free_count--;
    }

    /* Step 8: zone index computation */
    ZONE_DMA_INDEX = 0;
    ZONE_NORMAL_INDEX = (PMMngr.zones_size > 0) ? (PMMngr.zones_size - 1) : 0;
    ZONE_UNMAPPED_INDEX = 0;
    uint64_t threshold = pmm_arch_zone_split();
    for (uint32_t zi = 0; zi < PMMngr.zones_size; zi++) {
        struct Zone *z = PMMngr.zones_struct + zi;
        if (z->zone_start_address >= threshold && ZONE_UNMAPPED_INDEX == 0) {
            ZONE_UNMAPPED_INDEX = zi;
            ZONE_NORMAL_INDEX = (zi > 0) ? (zi - 1) : 0;
        }
    }

    /* Step 9: slab + subpage pools */
    slab_init();
    list_init(&subpage_pools);

    pmm_initialized = 1;
}
```

(You'll need to add `static int pmm_initialized = 0;` outside the function or use the local-static pattern shown.)

Preserve the existing `alloc_pages`, `free_pages`, `alloc_4k_page`, `free_4k_page`, `page_cow_get/put/refs`, `page_init`, `page_clean`, `set_page_attribute`, `get_page_attribute` functions byte-for-byte (except for the 2 `color_printk` → `log_err` replacements already made in Step 3).

- [ ] **Step 4: Verify x86_64 build with the rewrite**

Run: `make PROFILE=x86_64-clang kernel.bin 2>&1 | tail -30`
Expected: build succeeds. (main.c still has the old `pmm_init(&bootctx->memory)` call site — that's fixed in Task 11.)

Run systest:
```bash
python3 tests/run_test.py systest
```
Expected: 268/268 pass.

If any test fails, the bug is in the rewrite. Re-check:
- RAM-relative indexing for step 5 (x86_64 lowest_ram = 0, so behavior is unchanged)
- Step 6 page-0 quirk condition
- Step 7 walk bound (`lowest_ram = 0` on x86_64, so the clamp is a no-op and the loop walks `end_phys >> 21` pages — same as the existing `j = 1..=i`)

- [ ] **Step 5: Commit**

```bash
git add kernel/memory/pmm.c
git commit -m "pmm: rewrite pmm_init body with RAM-relative indexing + clamp

- Step 2: compute TotalMem, lowest_ram, highest_ram, ram_span_pages.
- Step 4-5: arrays sized by ram_span_pages (handles sparse RAM);
  pages_group = pages_struct + ((start - lowest_ram) >> 21) — x86_64
  with lowest_ram=0 is byte-for-byte equivalent to old code.
- Step 7: walk_pages = (end_phys > lowest_ram) ? ((end_phys -
  lowest_ram) >> 21) : 0 — prevents unsigned-wrap OOB on aarch64
  where end_phys (kernel LMA ~0x401e0000) < lowest_ram (first
  surviving RAM range starts at 0x40200000).
- 2 private color_printk replaced with log_err (in
  get_page_attribute, set_page_attribute); 5 public-surface
  color_printk in alloc_pages/free_pages preserved byte-for-byte.
- Real pmm_initialized guard (was: assert-based, NDEBUG'd out).
- log_err uses gate-wrapped macros from kernel/log.h.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: Update x86_64 main.c call site

**Files:**
- Modify: `kernel/kernel/main.c`

- [ ] **Step 1: Find the current pmm_init call site**

Search `kernel/kernel/main.c` for `pmm_init(`. The current call (per round-2 review) is `pmm_init(&bootctx->memory)`.

- [ ] **Step 2: Update the call to use the new signature**

Change:
```c
pmm_init(&bootctx->memory);
```
to:
```c
pmm_init(bootctx);
```

- [ ] **Step 3: Verify build and tests**

Run: `make PROFILE=x86_64-clang kernel.bin && python3 tests/run_test.py systest && python3 tests/run_test.py network`
Expected: builds; 268/268 + 6/6 pass. This confirms the rewritten `pmm_init` works correctly on x86_64.

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel/main.c
git commit -m "pmm: update x86_64 main.c call site to new pmm_init signature

pmm_init(bootctx) replaces pmm_init(&bootctx->memory). The
boot_context is now the only handoff object — consistent with the
long-term x86_64 + aarch64 system-entry merge goal.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: aarch64_main.c — include swap + PMMngr prelude + pmm_init + smoke test

**Files:**
- Modify: `kernel/arch/aarch64/main.c`

- [ ] **Step 1: Replace boot_log.h include with kernel/log.h**

In `kernel/arch/aarch64/main.c` line 6 (per round-11 review):
- Remove `#include <kernel/arch/aarch64/boot_log.h>`
- Add `#include <kernel/log.h>`

(The `ram.c` consumer keeps its `boot_log.h` include — that's a legacy caller outside this spec.)

- [ ] **Step 2: Add the PMMngr prelude and pmm_init call after aarch64_ram_init**

Find the line that calls `aarch64_ram_init(handoff)`. After its return-success check, add:

```c
/* Populate PMMngr fields that pmm_init reads. Mirrors the
 * kernel/kernel/main.c:155-159 prelude on x86_64, but uses the
 * aarch64 VMA linker symbols (_text_start/_text_end/.../_kernel_end)
 * because _text/_edata/_end do not exist on aarch64. */
extern char _text_start[], _text_end[];
extern char _rodata_start[], _rodata_end[];
extern char _data_start[], _data_end[];
extern char _kernel_end[];

/* Sanity check: the aarch64 identity map must be active before
 * pmm_init runs (otherwise Virt_To_Phy on high-half VMAs returns
 * nonsense and the kernel-image walk in Step 7 silently corrupts
 * pages_struct[]). head.S installs the identity map before
 * dropping to C. */
if ((uint64_t)&_text_start < ARCH_PAGE_OFFSET) {
    log_err("[smp] FATAL: aarch64 identity map not active\n");
    arch_cpu_halt();
}

PMMngr.start_code  = (uint64_t)&_text_start;
PMMngr.end_code    = (uint64_t)&_text_end;
PMMngr.end_data    = (uint64_t)&_data_end;
PMMngr.end_rodata  = (uint64_t)&_rodata_end;
PMMngr.start_brk   = (uint64_t)&_kernel_end;

pmm_init(handoff);
```

The insertion order must be:
1. `aarch64_ram_init(handoff)` (existing — publishes the map)
2. PMMngr prelude + `pmm_init(handoff)` (new — this task)
3. `#if OS01_SELFTEST` smoke block (next step)
4. `dtb_init(handoff)` (existing)
5. `gic_init(handoff)` (existing)
6. `smp_boot_aps(handoff)` (existing — first allocator consumer)
7. `arch_tick_start()` (existing)
8. halt (existing)

- [ ] **Step 3: Add the OS01_SELFTEST smoke block between pmm_init and dtb_init**

```c
#if OS01_SELFTEST
    {
        struct Page *p = alloc_pages(ZONE_NORMAL, 1, 0);
        if (p) { free_pages(p, 1); log_info("UEFI-A64: pmm alloc smoke OK\n"); }
        else   { log_err("UEFI-A64: pmm alloc smoke FAIL\n"); }
    }
#endif
```

- [ ] **Step 4: Verify aarch64 build with KERNEL_SELFTEST=1**

Run: `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi`
Expected: builds. (Task 13 will update `mk/components/run.mk` so the test rule passes KERNEL_SELFTEST=1 automatically.)

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/aarch64/main.c
git commit -m "pmm: wire pmm_init into aarch64_main + smoke test

- Replace boot_log.h include with kernel/log.h (unified macro API).
- After aarch64_ram_init succeeds: populate PMMngr with the aarch64
  VMA symbols (_text_start/_text_end/_data_end/_rodata_end/
  _kernel_end), then call pmm_init(handoff).
- Add #if OS01_SELFTEST smoke block: alloc_pages(ZONE_NORMAL,1,0)
  + free_pages + log line. Runs only when KERNEL_SELFTEST=1.
- Insertion order: aarch64_ram_init -> PMMngr prelude + pmm_init
  -> smoke -> dtb_init -> gic_init -> smp_boot_aps ->
  arch_tick_start -> halt.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: mk/components/run.mk — KERNEL_SELFTEST=1 for test-aarch64-uefi-smp

**Files:**
- Modify: `mk/components/run.mk`

- [ ] **Step 1: Find the test-aarch64-uefi-smp rule**

In `mk/components/run.mk` around lines 161–170, find the rule that defines `test-aarch64-uefi-smp`. Mirror the pattern of `test-kernel-selftest` at line 350 which passes `KERNEL_SELFTEST=1`.

- [ ] **Step 2: Pass KERNEL_SELFTEST=1 in the recipe**

Modify the recipe to invoke `$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi` (or equivalent, depending on the exact rule syntax). The smoke block in `aarch64_main.c` compiles only when `KERNEL_SELFTEST=1` is set.

- [ ] **Step 3: Verify the rule**

Run: `make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | head -30`
Expected: the rule rebuilds `aarch64-uefi` with `KERNEL_SELFTEST=1`. Verify the binary contains the smoke string:

```bash
strings build/.../aarch64-uefi | grep "UEFI-A64: pmm alloc smoke"
```
Expected: matches.

- [ ] **Step 4: Commit**

```bash
git add mk/components/run.mk
git commit -m "build: test-aarch64-uefi-smp passes KERNEL_SELFTEST=1

Required so the #if OS01_SELFTEST smoke block in aarch64_main.c
is compiled in, allowing tests/aarch64_uefi_smp.py to assert
the new 'UEFI-A64: pmm alloc smoke OK' log line. Mirrors
test-kernel-selftest's KERNEL_SELFTEST=1 convention.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 13: Host test runner C — pmm_arch_test_runner.c

**Files:**
- Create: `tests/pmm_arch_test_runner.c`

- [ ] **Step 1: Create the C runner**

Create `tests/pmm_arch_test_runner.c` with:

```c
/* tests/pmm_arch_test_runner.c — host-side test for pmm_arch. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <kernel/bootinfo.h>
#include <kernel/memory_map.h>

extern size_t pmm_arch_normalize(const struct boot_context *,
                                  struct MEMORY_RANGE *);
extern uint64_t pmm_arch_zone_split(void);

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    return 1; } } while (0)

int main(void)
{
    struct MEMORY_RANGE out[MEMORY_RANGE_MAX];

#if !defined(__aarch64__)
    /* x86_64-only: aarch64 adapter ignores ctx and reads the
     * already-published map (aarch64_ram_map_get), so a zeroed
     * boot_context has no defined meaning on aarch64. */
    {
        struct boot_context ctx = {0};
        CHECK(pmm_arch_normalize(&ctx, out) == 0);
    }

    /* Minimal E820 single type-1 entry spanning low RAM.
     * x86_64 stub provides _text = 0xffff800000200000,
     * _edata = 0xffff800000300000, handoff in kernel-LMA gap.
     * Expected: two surviving MEMORY_TYPE_RAM fragments
     *   [0, 0x200000), [0x300000, 0x40000000). */
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
#endif

#if defined(__x86_64__)
    CHECK(pmm_arch_zone_split() == 0x100000000ULL);
#elif defined(__aarch64__)
    CHECK(pmm_arch_zone_split() == SIZE_MAX);
#endif

    return 0;
}
```

- [ ] **Step 2: Commit (runner only — the Python driver is Task 14)**

```bash
git add tests/pmm_arch_test_runner.c
git commit -m "test: add pmm_arch_test_runner.c skeleton

Stub for the host-side adapter test. The runner's TODO cases
(E820 reserved hole, UEFI_RAW three type-7 descriptors) are
intentionally deferred to the implementer per round-5 design.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 14: Host test driver Python — pmm_arch_test.py

**Files:**
- Create: `tests/pmm_arch_test.py`

- [ ] **Step 1: Create the Python driver**

Create `tests/pmm_arch_test.py` modeled on `tests/aarch64_ram_test.py`. The script:

1. Picks the profile via `os.environ.get('PROFILE', 'x86_64-clang')`
2. Locates the source files to compile (`kernel/memory/pmm_arch.c`, `kernel/arch/<arch>/pmm_arch.c`, plus `tests/pmm_arch_test_runner.c`)
3. For x86_64: provides a stub TU with `_text`, `_edata`, trampoline blob symbols, and `X86_64_HANDOFF_BASE/END = 0x204000/0x208000` test values, plus an inline `#define Virt_To_Phy(v) ((v) - 0xffff800000000000UL)`
4. For aarch64: links `kernel/arch/aarch64/ram.c` AND `kernel/arch/aarch64/ram_core.c` (per round-7 fix); no x86 stubs needed (the weak default is now arch-neutral)
5. Compiles with the host `cc` and runs the runner, asserting exit code 0

Use `tests/aarch64_ram_test.py` as the structural template.

- [ ] **Step 2: Run the test on x86_64**

Run: `python3 tests/pmm_arch_test.py`
Expected: exit 0.

- [ ] **Step 3: Run the test on aarch64**

Run: `python3 tests/pmm_arch_test.py` with `PROFILE=aarch64-clang`
Expected: exit 0 (if the host `cc` can target aarch64-none-elf; otherwise the test reports a clear "host cc cannot target aarch64" failure and is gated to x86_64-only).

- [ ] **Step 4: Commit**

```bash
git add tests/pmm_arch_test.py
git commit -m "test: add pmm_arch_test.py host driver

Mirrors tests/aarch64_ram_test.py structure. Profile-aware: provides
x86_64 stubs (_text/_edata/Virt_To_Phy/trampoline blob/handoff
constants) and links aarch64 ram.c+ram_core.c for the aarch64 path.
Validates pmm_arch_normalize and pmm_arch_zone_split via the C
runner.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 15: AArch64 UEFI test parser — expect_selftest and smoke line

**Files:**
- Modify: `tests/aarch64_uefi_smp.py`

- [ ] **Step 1: Add `args.expect_selftest` to argparse**

Around line 436–446 (per round-2 review), the existing argparse has `--expect-no-ack`. Add:

```python
parser.add_argument("--expect-selftest", action="store_true",
                    help="Require 'UEFI-A64: pmm alloc smoke OK' log line "
                         "between RAM summary and topology line")
```

- [ ] **Step 2: Modify `passed()` to assert the smoke line when `args.expect_selftest`**

Find `passed(text, cpus)` (around line 218). At the appropriate location (after the existing RAM-summary assertion), add:

```python
if args.expect_selftest:
    ram_pos = text.find("UEFI-A64: RAM ranges=")
    topo_pos = text.find(f"[smp] topology source=uefi-dtb cpus={cpus}")
    if ram_pos < 0 or topo_pos < 0 or ram_pos > topo_pos:
        # Existing assertion already failed; let it propagate.
        pass
    else:
        between = text[ram_pos:topo_pos]
        if "UEFI-A64: pmm alloc smoke OK" not in between:
            print(f"FAIL: 'UEFI-A64: pmm alloc smoke OK' missing between "
                  f"RAM summary and topology line (text length {len(text)})")
            return False
```

- [ ] **Step 3: Verify existing acceptance still passes**

Run: `make PROFILE=aarch64-clang test-aarch64-uefi-smp`
Expected: existing PASS/DEGRADED assertions unchanged (since `args.expect_selftest` defaults to False).

- [ ] **Step 4: Commit**

```bash
git add tests/aarch64_uefi_smp.py
git commit -m "test: parser asserts 'UEFI-A64: pmm alloc smoke OK' when expect-selftest

Adds --expect-selftest argparse flag. When set, passed() requires
the new log line between 'UEFI-A64: RAM ranges=' summary and the
'[smp] topology source=uefi-dtb cpus=' line (anchoring on the
specific topology line avoids false-positives on '[smp-test] FATAL'
strings that appear later). Default off so legacy test invocations
unchanged.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 16: Acceptance — run all tests, verify all pass

**Files:** none (verification only)

- [ ] **Step 1: Run x86_64 regression**

```bash
make PROFILE=x86_64-clang kernel.bin
python3 tests/pmm_arch_test.py
python3 tests/run_test.py systest
python3 tests/run_test.py network
```

Expected: kernel.bin builds; pmm_arch_test passes; systest 268/268; nettest 6/6.

- [ ] **Step 2: Run AArch64 evidence tests**

```bash
make PROFILE=aarch64-clang aarch64-uefi-kernel
python3 tests/aarch64_ram_test.py
python3 tests/pmm_arch_test.py
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 test-aarch64-uefi-smp
```

Expected: aarch64-uefi-kernel builds; aarch64_ram_test passes; pmm_arch_test passes (host-side); KERNEL_SELFTEST=1 build succeeds; test-aarch64-uefi-smp reports PASS and the parser sees "UEFI-A64: pmm alloc smoke OK".

- [ ] **Step 3: Run AArch64 no-ACK regression (preserves DEGRADED status)**

```bash
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
```

Expected: DEGRADED status preserved (no new `[smp] FATAL` lines; the no-ACK path does not pass `KERNEL_SELFTEST=1`).

- [ ] **Step 4: Final commit (if any fix-ups were needed)**

If any step revealed a problem, fix it inline and commit. If everything passes, no commit needed — this task is verification-only.

- [ ] **Step 5: Tag the milestone**

```bash
git tag -a pmm-arch-neutral-v1 -m "First working arch-neutral PMM"
git log --oneline -1
```

---

## Self-Review

Verifying the plan against the spec before delivery:

**1. Spec coverage:**

| Spec section | Plan task |
|--------------|-----------|
| Status / Problem / Goals / Non-goals | (no code) |
| Considered approaches | (no code) |
| Architecture (weak default + strong overrides) | Task 1 (weak default) + Task 2 (x86_64) + Task 3 (aarch64) |
| Input contract (per-format entry_size) | Task 9 (Step 9 input validation) |
| New types (memory_map.h) | Task 1 |
| New conversion layer | Task 1 + 2 + 3 |
| `pmm_init` rewrite (RAM-relative, clamp) | Task 9 |
| Log API unification (gate-wrapped macros) | Task 6 + 7 |
| aarch64 stubs (printk, memset, slab) | Task 5 |
| Files touched (incl. Makefile, run.mk, parser) | Tasks 8, 12, 15 |
| Testing (host runner, parser extension) | Tasks 13, 14, 15 |

All spec requirements are covered.

**2. Placeholder scan:**

- "TBD"/"TODO"/"implement later": none
- "Add appropriate error handling": none
- "Write tests for the above": no — Task 13 specifies the exact test code
- "Similar to Task N": no — every task has its own code
- Steps without code blocks for code steps: Task 9 has the full pmm_init body; Task 13 has the full runner; Task 14 references the existing `aarch64_ram_test.py` template explicitly
- References to undefined types/functions: all symbols used in later tasks (`MEMORY_RANGE`, `pmm_arch_normalize`, `_log_*_impl`, `ZONE_NORMAL`, `_text`, `_edata`, etc.) are defined or declared in earlier tasks

**3. Type consistency:**

- `pmm_arch_normalize(const struct boot_context *, struct MEMORY_RANGE *)` declared in Task 1, defined consistently in Tasks 2 and 3 with the same signature
- `pmm_arch_zone_split(void)` declared in Task 1, defined in Tasks 2 and 3 with same signature
- `_log_err_impl(const char *fmt, ...)` declared in Task 6, defined in Tasks 6 (x86_64) and 7 (aarch64) with matching signatures
- `MEMORY_RANGE` fields `phys_start`/`phys_end`/`type` defined in Task 1, used consistently in Tasks 9, 13, 14
- `MEMORY_RANGE_MAX = 64u` and `MEMORY_RANGE_GRANULE = 2 MiB` from Task 1 used consistently

No inconsistencies found.

---

Plan complete. Total: **16 tasks** over 11 file modifications + 11 file creations.
