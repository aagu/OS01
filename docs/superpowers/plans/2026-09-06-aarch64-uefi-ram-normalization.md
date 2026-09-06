# AArch64 UEFI RAM Normalization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a safe, 2 MiB-granular AArch64 physical-RAM map from the raw UEFI descriptor handoff before GIC/SMP initialization.

**Architecture:** A host-linkable `ram_core.c` parses raw descriptor bytes without allocation or aligned structure casts, subtracts explicit exclusions, and emits at most 16 merged ranges. Target-only `ram.c` validates the physical handoff window, builds the linker/handoff exclusions, maps descriptor bytes through the direct map, publishes the successful result once, and makes `aarch64_main()` fail closed before touching GIC, SMP, or timers.

**Tech Stack:** Freestanding C11, AArch64 LLVM cross-build, UEFI v1 descriptor wire format, Python 3 host tests, existing QEMU `virt` PSCI SMP harness.

**Spec:** `docs/superpowers/specs/2026-09-06-aarch64-uefi-ram-normalization-design.md`

## Global Constraints

- Do not link `kernel/memory/pmm.c`, `kernel/memory/slab.c`, or change PMM structures.
- Do not change the v2 `boot_context` ABI or convert the map in the UEFI loader.
- Only descriptor wire type `AARCH64_EFI_CONVENTIONAL_MEMORY == 7u` is allocatable.
- Accept only `BOOT_MEMORY_FORMAT_UEFI_RAW`, descriptor version 1, and a 32-byte-or-larger descriptor stride.
- Parse descriptor fields with bytewise little-endian loads; never cast raw descriptor bytes to a C struct.
- Use `AARCH64_RAM_GRANULE == (UINT64_C(1) << 21)` rather than `kernel/pmm.h` or `PAGE_2M_SIZE`.
- The core must use O(1) scratch storage and no dynamic allocation; its output is sorted, non-empty, non-overlapping, non-adjacent, 2 MiB aligned, and capped at 16 ranges.
- AArch64 boot failures print `UEFI-A64: RAM map invalid` and halt before `gic_init()`.
- After any build-flag change, run `make PROFILE=aarch64-clang clean` before rebuilding.

---

## File structure

| File | Responsibility |
|---|---|
| `kernel/include/kernel/arch/aarch64/handoff_layout.h` | Shared, fixed physical addresses for the UEFI handoff data region and trampoline boundary. |
| `kernel/include/kernel/arch/aarch64/ram.h` | Immutable public normalized-RAM-map type and boot-facing API. |
| `kernel/arch/aarch64/ram_core.h` | Internal core constants, exclusion type, core/parser API, and publish-once API; usable from host tests. |
| `kernel/arch/aarch64/ram_core.c` | Host-linkable UEFI raw-map normalization and publication validation. |
| `kernel/arch/aarch64/ram.c` | AArch64-only physical-handoff validation, linker exclusions, direct-map conversion, and static publication. |
| `kernel/arch/aarch64/main.c` | Calls RAM initialization and emits summary before GIC/SMP. |
| `boot/uefi/arch/aarch64/boot.c` | Imports the shared handoff layout constants instead of owning duplicate values. |
| `boot/uefi/arch/aarch64/loader.h` | Imports the shared handoff layout constants used by the loader. |
| `boot/uefi/Makefile`, `mk/components/uefi.mk` | Track the shared handoff-layout header as an AArch64 UEFI rebuild dependency. |
| `tests/aarch64_ram_test.py` | Compiles/runs a C host runner against `ram_core.c`. |
| `tests/aarch64_uefi_smp.py` | Requires exactly one valid RAM summary in normal and injected-degraded QEMU logs. |

### Task 1: Establish shared and core-facing contracts

**Files:**
- Create: `kernel/include/kernel/arch/aarch64/handoff_layout.h`
- Create: `kernel/include/kernel/arch/aarch64/ram.h`
- Create: `kernel/arch/aarch64/ram_core.h`
- Modify: `boot/uefi/arch/aarch64/loader.h`
- Modify: `boot/uefi/arch/aarch64/boot.c:19-29`
- Modify: `boot/uefi/Makefile:106-112`
- Modify: `mk/components/uefi.mk:148-150`
- Test: `tests/aarch64_ram_test.py`

**Interfaces:**
- Consumes: `struct boot_context` and `BOOT_MEMORY_FORMAT_UEFI_RAW` from `kernel/include/kernel/bootinfo.h`.
- Produces: shared handoff bounds plus all types and function declarations consumed by Tasks 2 and 3.

- [ ] **Step 1: Write the failing host contract test**

Create `tests/aarch64_ram_test.py` with a temporary C runner that includes
`kernel/arch/aarch64/ram_core.h` and `kernel/arch/aarch64/ram.h`. Make its
first assertions compile-time contracts:

```c
_Static_assert(AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE == 32u, "wire prefix");
_Static_assert(AARCH64_EFI_CONVENTIONAL_MEMORY == 7u, "UEFI type");
_Static_assert(AARCH64_RAM_GRANULE == (UINT64_C(1) << 21), "granule");
_Static_assert(AARCH64_RAM_MAX_RANGES == 16, "map capacity");
```

Have Python invoke:

```python
subprocess.run([
    os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-I.", "-Ikernel/include", str(ROOT / "kernel/arch/aarch64/ram_core.c"),
    str(runner), "-o", str(executable),
], cwd=ROOT, check=True)
```

- [ ] **Step 2: Run the contract test and verify it fails**

Run: `python3 tests/aarch64_ram_test.py`

Expected: compilation fails because the RAM headers and `ram_core.c` do not exist.

- [ ] **Step 3: Add the minimal headers and loader layout reuse**

Create `handoff_layout.h` as a fixed-width, dependency-free header:

```c
#ifndef OS01_AARCH64_HANDOFF_LAYOUT_H
#define OS01_AARCH64_HANDOFF_LAYOUT_H
#include <stdint.h>
#define AARCH64_HANDOFF_BASE UINT64_C(0x401e0000)
#define AARCH64_HANDOFF_END UINT64_C(0x40200000)
#define AARCH64_TRAMPOLINE_BASE UINT64_C(0x401ff000)
#endif
```

Define the public map in `ram.h`; define the internal wire constants,
`struct aarch64_ram_interval`, the exact `aarch64_ram_normalize(...)`
signature from the spec, and `aarch64_ram_publish_once(...)` in `ram_core.h`.
Use a forward declaration of `struct aarch64_ram_map` in `ram_core.h` or
include `ram.h` there.

Replace the duplicate handoff-base/trampoline constants in AArch64 loader
sources with the shared header. `boot/uefi` has no `-Ikernel/include`, so
`boot/uefi/arch/aarch64/loader.h` must include it exactly as:

```c
#include "../../../../kernel/include/kernel/arch/aarch64/handoff_layout.h"
```

After the architecture-selection conditional in `mk/components/uefi.mk`, define
the x86-safe baseline and one AArch64-only addition exactly as:

```make
UEFI_BOOT_INPUTS := $(UEFI_BOOT_SRCS) boot/uefi/arch/arch.h \
                    kernel/include/kernel/bootinfo.h
ifeq ($(UEFI_ARCH_FAMILY),aarch64)
UEFI_BOOT_INPUTS += kernel/include/kernel/arch/aarch64/handoff_layout.h
endif
```

Replace both `sha256sum $(UEFI_BOOT_SRCS)` in the runtime-receipt digest and
the EFI artifact prerequisite list with `$(UEFI_BOOT_INPUTS)`. This keeps every
x86 input present and adds the shared header only for AArch64. Add the same
shared header to the
`$(OUTDIR)/$(TARGET)` prerequisites in `boot/uefi/Makefile`. This makes a
header-only layout change invalidate both the staged-runtime receipt and the
EFI artifact.

- [ ] **Step 4: Add a linkable empty core and run the contract test**

Create `ram_core.c` with definitions for both declared functions that return a
non-zero error after zeroing a non-null `out`. This is only the first green
increment; it proves host linkage and header ownership, not behavior.

Run: `python3 tests/aarch64_ram_test.py`

Expected: PASS for the compile-time contracts and runner startup.

- [ ] **Step 5: Cross-build the unchanged AArch64 kernel and UEFI artifact**

Run:

```bash
make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang aarch64-uefi
```

Expected: both artifacts build; no x86 or PMM object appears in the AArch64 link.

- [ ] **Step 6: Commit the contract boundary**

```bash
git add kernel/include/kernel/arch/aarch64/handoff_layout.h \
  kernel/include/kernel/arch/aarch64/ram.h kernel/arch/aarch64/ram_core.h \
  kernel/arch/aarch64/ram_core.c boot/uefi/arch/aarch64/loader.h \
  boot/uefi/arch/aarch64/boot.c boot/uefi/Makefile mk/components/uefi.mk \
  tests/aarch64_ram_test.py
git commit -m "feat(aarch64): define UEFI RAM map contracts"
```

### Task 2: Implement and host-test raw UEFI normalization

**Files:**
- Modify: `kernel/arch/aarch64/ram_core.c`
- Modify: `kernel/arch/aarch64/ram_core.h`
- Modify: `tests/aarch64_ram_test.py`

**Interfaces:**
- Consumes: `aarch64_ram_normalize()` input bytes, metadata, exclusions, and output map from Task 1.
- Produces: fully normalized `struct aarch64_ram_map` or a negative error with a zeroed output map.

- [ ] **Step 1: Add failing behavioral cases to the host runner**

Write byte-buffer helpers that store UEFI descriptor fields at offsets 0, 8,
and 24 in little-endian form. Define named core errors in `ram_core.h`, for
example `AARCH64_RAM_ERR_ARGUMENT`, `AARCH64_RAM_ERR_FORMAT`,
`AARCH64_RAM_ERR_GEOMETRY`, `AARCH64_RAM_ERR_OVERFLOW`, and
`AARCH64_RAM_ERR_CAPACITY`, each with a distinct negative value. Add these
assertions before implementing the algorithm:

```c
assert(aarch64_ram_normalize(bytes, 0, 32,
       BOOT_MEMORY_FORMAT_UEFI_RAW, 1, NULL, 0, &out) == AARCH64_RAM_ERR_ARGUMENT);
assert(out.count == 0);
assert(aarch64_ram_normalize(bytes, 1, 31,
       BOOT_MEMORY_FORMAT_UEFI_RAW, 1, NULL, 0, &out) == AARCH64_RAM_ERR_GEOMETRY);
assert(aarch64_ram_normalize(bytes, 1, 32, 0, 1, NULL, 0, &out) < 0);
```

Add positive fixtures for: ignored non-type-7 descriptor; unsorted adjacent
type-7 descriptors that merge; a deliberately unaligned stride of 40 bytes;
a fragment below one 2 MiB granule; a range split by `[0x401e0000,
0x40200000)`; and a later bridging descriptor that proves capacity is checked
only after final merging. Add distinct negative-result cases for wrong format,
wrong descriptor version, `exclude_count != 0 && exclude == NULL`, zero page
count, descriptor span multiplication overflow, physical-end addition
overflow, invalid exclusion endpoints, and seventeenth final range. For every
failure assert the specific named error and `out.count == 0` with every output
slot zeroed. Assert every successful output range has aligned endpoints, strict
order, and no intersection with exclusions.

- [ ] **Step 2: Run the host test and verify it fails**

Run: `python3 tests/aarch64_ram_test.py`

Expected: at least one positive-normalization assertion fails because the
Task-1 core only returns errors.

- [ ] **Step 3: Implement byte decoding, checked arithmetic, and fragment generation**

In `ram_core.c`, implement `load_le32()`/`load_le64()` from `uint8_t` bytes.
Reject null pointers, zero count, stride below 32, wrong format/version,
`exclude_count != 0 && exclude == NULL`, invalid exclusion intervals,
multiplication/addition overflow, zero selected `NumberOfPages`, and
`NumberOfPages * 4096` overflow using the named errors from Step 1. Ignore
every descriptor whose type is not 7.

For each selected descriptor, subtract every exclusion interval into
closed-open fragments, align surviving fragments inward with
`AARCH64_RAM_GRANULE`, and discard empty fragments. Do not allocate or retain
a descriptor-sized list.

- [ ] **Step 4: Implement exact final-range discovery**

Implement the repeated-scan algorithm required by the spec:

1. scan all normalized fragments at or after the previous emitted end and pick
   the smallest start;
2. repeatedly rescan all fragments and extend that candidate end for each
   overlapping or touching fragment until no extension occurs;
3. append the merged candidate;
4. after 16 appends, perform one more discovery scan and fail only if another
   final range exists.

Clear `out` before work and clear it again on every error. Reject a successful
empty map.

- [ ] **Step 5: Run the full core suite**

Run: `python3 tests/aarch64_ram_test.py`

Expected: PASS, including malformed geometry, unaligned stride, overflow,
reserved-range subtraction, deferred capacity detection, and map invariants.

- [ ] **Step 6: Commit core normalization**

```bash
git add kernel/arch/aarch64/ram_core.c kernel/arch/aarch64/ram_core.h \
  tests/aarch64_ram_test.py
git commit -m "feat(aarch64): normalize raw UEFI RAM descriptors"
```

### Task 3: Add one-time publication and early AArch64 boot integration

**Files:**
- Modify: `kernel/arch/aarch64/ram_core.c`
- Create: `kernel/arch/aarch64/ram.c`
- Modify: `kernel/arch/aarch64/main.c:13-30`
- Modify: `tests/aarch64_ram_test.py`

**Interfaces:**
- Consumes: normalized candidate map and the handoff layout/ram headers from Tasks 1-2.
- Produces: `aarch64_ram_init()`, `aarch64_ram_map_get()`, and exactly one serial RAM summary before GIC initialization.

- [ ] **Step 1: Add failing publication-state tests**

In the host C runner, create a valid one-range candidate and assert the
publish helper copies it once, rejects an invalid unaligned candidate without
changing the destination, and returns `-EALREADY` without changing destination
or initialized state on the second valid attempt:

```c
assert(aarch64_ram_publish_once(&candidate, &published, &initialized) == 0);
assert(aarch64_ram_publish_once(&candidate, &published, &initialized) == -EALREADY);
```

- [ ] **Step 2: Run the host test and verify it fails**

Run: `python3 tests/aarch64_ram_test.py`

Expected: publication assertions fail until the helper validates and copies maps.

- [ ] **Step 3: Implement `aarch64_ram_publish_once()` in the core**

Validate `count` is 1 through 16; each range has `start < end`, both endpoints
are aligned to `AARCH64_RAM_GRANULE`, and each next start is strictly greater
than the previous end. On every validation failure leave destination and flag
unchanged. On a second publication return `-EALREADY` unchanged.

- [ ] **Step 4: Implement target-only wrapper and failure path**

In `ram.c`:

- keep static `struct aarch64_ram_map published_map` and an initialized flag;
- validate map flag/format/count/stride/version and checked physical
  `[entries, entries + count*stride)` containment within
  `[AARCH64_HANDOFF_BASE, AARCH64_TRAMPOLINE_BASE)` before dereference;
- create exactly two exclusions: physical
  `[_boot_start, _kernel_lma_end)` after monotonicity validation, and
  `[AARCH64_HANDOFF_BASE, AARCH64_HANDOFF_END)`;
- include `<kernel/arch/mmu.h>` and, only after the checked physical-range
  containment above, pass `(const uint8_t *)(uintptr_t)(handoff->memory.entries
  + ARCH_PAGE_OFFSET)` to the core; do not use `Phy_To_Virt()`, because that
  macro is not defined for the AArch64 profile;
- use stack-local candidate storage and publish only after core success;
- return a negative error for all failures; `aarch64_ram_map_get()` returns
  null before first success and `&published_map` afterward.

In `aarch64_main()`, invoke `aarch64_ram_init(handoff)` after validating the
handoff and setting VBAR, but before `dtb_init()`/`gic_init()`. On failure log
`UEFI-A64: RAM map invalid`, log an `[smp] FATAL:` reason, and halt with
interrupts disabled. On success use `kputs()`/`kputu()` to print exactly:

```text
UEFI-A64: RAM ranges=<n> pages2m=<n> bytes=<n>
```

Calculate both values from the published map using checked accumulation.

- [ ] **Step 5: Run host and cross-build checks**

Run:

```bash
python3 tests/aarch64_ram_test.py
make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Expected: host suite passes, kernel links, and `llvm-nm` shows the new
`aarch64_ram_init` and `aarch64_ram_map_get` symbols in `kernel.elf`.

- [ ] **Step 6: Commit publication and boot hook**

```bash
git add kernel/arch/aarch64/ram.c kernel/arch/aarch64/ram_core.c \
  kernel/arch/aarch64/main.c tests/aarch64_ram_test.py
git commit -m "feat(aarch64): publish normalized UEFI RAM map"
```

### Task 4: Require RAM evidence in the QEMU SMP harness

**Files:**
- Modify: `tests/aarch64_uefi_smp.py:20-185`
- Test: `tests/aarch64_uefi_smp.py`

**Interfaces:**
- Consumes: the exact serial summary emitted by Task 3.
- Produces: normal and degraded acceptance predicates that reject missing,
duplicate, malformed, or mathematically inconsistent RAM summaries.

- [ ] **Step 1: Add failing parser fixtures**

Add one valid line to all four positive fixtures — `complete_log_for_4_cpus`,
`complete_degraded_log`, `current_log_for_2_cpus`, and `current_degraded_log`:

```text
UEFI-A64: RAM ranges=1 pages2m=255 bytes=534773760
```

Add self-test mutations for each fixture family that remove it or duplicate it,
plus mutations that set `ranges=0`, set a non-decimal field, and make
`bytes != pages2m * 2097152`. Require each mutation to fail. Keep all four
valid normal and degraded fixtures passing.

- [ ] **Step 2: Run parser self-test and verify it fails**

Run: `python3 tests/aarch64_uefi_smp.py --self-test`

Expected: failure until `passed()` and `degraded_passed()` inspect the RAM line.

- [ ] **Step 3: Implement shared RAM-summary validation**

Add a helper that normalizes CRLF, finds exactly one full-line match for:

```python
r"^UEFI-A64: RAM ranges=(\d+) pages2m=(\d+) bytes=(\d+)$"
```

Require `ranges` in `1..16`, `pages2m > 0`, and
`bytes == pages2m * 2097152`. Call it from both `passed()` and
`degraded_passed()` before their existing SMP-specific checks.

- [ ] **Step 4: Run parser self-test**

Run: `python3 tests/aarch64_uefi_smp.py --self-test`

Expected: `aarch64_uefi_smp: self-test passed`.

- [ ] **Step 5: Run the normal QEMU matrix**

Run: `make PROFILE=aarch64-clang test-aarch64-uefi-smp`

Expected: every 1-, 2-, and 4-vCPU run has exactly one valid RAM summary and
retains established PASS evidence.

- [ ] **Step 6: Run the injected degraded matrix and restore the normal image**

Run:

```bash
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
```

Expected: the injected two-CPU run contains one valid RAM line followed by its
established DEGRADED evidence; the final command leaves a normal image.

- [ ] **Step 7: Commit QEMU acceptance coverage**

```bash
git add tests/aarch64_uefi_smp.py
git commit -m "test(aarch64): require normalized RAM boot evidence"
```

### Task 5: Run the complete regression and inspect the final delta

**Files:**
- Verify: all files from Tasks 1-4
- Verify: `docs/superpowers/specs/2026-09-06-aarch64-uefi-ram-normalization-design.md`

**Interfaces:**
- Consumes: the complete implementation and test commands from Tasks 1-4.
- Produces: evidence that the implementation matches the approved spec without PMM scope creep.

- [ ] **Step 1: Run the complete required command set**

Run:

```bash
python3 tests/aarch64_ram_test.py
python3 tests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
```

Expected: every command exits zero; the normal and degraded QEMU logs meet the
parser predicates; final disk image is built with no fault injection.

- [ ] **Step 2: Check scope and diff hygiene**

Run:

```bash
git diff --check HEAD~4..HEAD
git diff --name-only HEAD~4..HEAD
```

Expected: no whitespace errors; changed production files are limited to the
AArch64 RAM/handoff/boot path and build prerequisites; no PMM, slab, VMM, or
`boot_context` ABI file changes appear.

- [ ] **Step 3: Commit any verification-only correction if needed**

If a command required a production or test correction, add only the corrected
files and commit with a conventional `fix(aarch64): ...` or `test(aarch64): ...`
message. If no correction was needed, do not create an empty commit.

## Plan self-review

- Spec coverage: Tasks 1-3 cover wire validation, raw-map bounds, exclusions,
  safe publication, fixed output capacity, boot ordering, and summary; Task 4
  covers normal and degraded QEMU evidence; Task 5 reruns all acceptance paths.
- Placeholder scan: this plan contains concrete paths, signatures, test data,
  commands, and expected outcomes; it leaves PMM integration explicitly out of
  scope.
- Type consistency: `aarch64_ram_normalize`, `aarch64_ram_publish_once`,
  `aarch64_ram_init`, `aarch64_ram_map_get`, `AARCH64_RAM_GRANULE`, and all map
  structures retain the exact names established by the approved spec.
