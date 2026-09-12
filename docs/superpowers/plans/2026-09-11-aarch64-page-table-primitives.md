# AArch64 Page-Table Primitives Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add pre-SMP AArch64 4 KiB page-table primitives and prove one active-root map/query/unmap round trip during the existing QEMU self-test.

**Architecture:** `page_table.c` owns AArch64 stage-1 descriptor encoding and walks an explicit high-half direct-map root through PGD/PUD/PMD/PTE. It allocates missing table pages from the existing PMM 4 KiB allocator. `aarch64_main()` runs one map/query/unmap smoke test before DTB/SMP initialization; the current QEMU parser accepts that single additional ordered line.

**Tech Stack:** Freestanding C11, AArch64 EL1 stage-1 translation tables, existing PMM 4 KiB allocator, LLVM cross-build, existing Python/QEMU PSCI harness.

**Spec:** `docs/superpowers/specs/2026-09-11-aarch64-page-table-primitives-design.md`

## Global Constraints

- Do not compile or modify generic `kernel/memory/vmm.c`, VMA, ELF, COW, or user-task code for AArch64.
- Do not modify `head.S` boot mappings, MAIR_EL1, TCR_EL1, TTBR ownership, or `arch_get_page_table()`.
- Primitives accept direct-map virtual root/table pointers; `arch_get_page_table()` returns a physical base and must be converted only after validation.
- Require 4 KiB alignment, canonical 48-bit low/high virtual addresses, and physical/table addresses below 1 TiB.
- Only L3 4 KiB leaves may be created. Existing PUD/PMD blocks return `AARCH64_PT_ECONFLICT` without modification.
- Descriptor bits live only in `page_table.c`; do not include x86 `PAGE_*` flags from `kernel/vmm.h`.
- Active-root use is BSP/pre-SMP-only. The self-test runs after `pmm_init()` and before `dtb_init()`/`smp_boot_aps()`.
- Do not reclaim linked intermediate tables in this increment.
- Validation remains limited to the existing QEMU self-test/parser; do not add host emulators, callback injection, or broad synthetic test frameworks.

---

## File structure

| File | Responsibility |
|---|---|
| `kernel/include/kernel/arch/aarch64/page_table.h` | Public AArch64 page-table permission/result API and self-test VA constant. |
| `kernel/arch/aarch64/page_table.c` | Descriptor encode/decode, four-level walk, PMM-backed table allocation, local TLB invalidation, and map/query/unmap/range APIs. |
| `kernel/arch/aarch64/main.c` | Pre-SMP active-root smoke test and its stable serial messages. |
| `tests/aarch64_uefi_smp.py` | Minimal self-test parser requirement for PMM then page-table smoke evidence. |
| `mk/components/run.mk` | Passes `--expect-selftest` for the existing self-test build target. |

### Task 1: Define AArch64 table API and descriptor/walk implementation

**Files:**
- Create: `kernel/include/kernel/arch/aarch64/page_table.h`
- Create: `kernel/arch/aarch64/page_table.c`
- Verify: `kernel/arch/aarch64/make.config`
- Verify: `kernel/include/kernel/arch/mmu.h:113-194`
- Verify: `kernel/include/kernel/pmm.h`

**Interfaces:**
- Consumes: `alloc_4k_page()`, `free_4k_page()`, `ARCH_PAGE_OFFSET`, and the boot-installed 4-level TTBR regime.
- Produces: `aarch64_pt_map_4k`, `aarch64_pt_query_4k`, `aarch64_pt_unmap_4k`, and `aarch64_pt_range_accessible` for Task 2.

- [ ] **Step 1: Create the public header with exact API**

Create `page_table.h` with the permission and result enums from the spec and:

```c
#define AARCH64_PT_SELFTEST_VA UINT64_C(0xffff800000000000)

int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                       uint32_t perm);
int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                         uint64_t *pa_out, uint32_t *perm_out);
int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                         uint64_t *pa_out, uint32_t *perm_out);
bool aarch64_pt_range_accessible(const uint64_t *root, uint64_t va,
                                 uint64_t length, bool write, bool user);
```

Add comments that `root` is a high-half direct-map pointer, that callers may
modify an active root only pre-SMP, and that the self-test VA's shared-root
lower-48 alias is not permanent.

- [ ] **Step 2: Implement private descriptor helpers**

In `page_table.c`, define private named constants for L0-L3 indices, 4 KiB
alignment, 40-bit PA mask, valid/table/page, AF, AttrIndx, SH, AP, PXN, and
UXN fields. Add helpers that:

- validate a root, VA, and PA against the spec's canonical/TCR/IPS rules;
- accept exactly one kernel/user and one RO/RW permission class;
- reject unknown permission bits and `AARCH64_PT_DEVICE | AARCH64_PT_EXEC`;
- encode normal versus device L3 leaf descriptors and decode a leaf back to
  the public permission word.

Use AP values `00` kernel-RW, `10` kernel-RO, `01` user-RW, and `11` user-RO.
For executable leaves clear only PXN for kernel or UXN for user; default and
device mappings set both execute-never bits.

- [ ] **Step 3: Implement allocation, walking, and local invalidation**

Implement a private `root_valid(root)` before any root dereference: require
4 KiB alignment, no `ARCH_PAGE_OFFSET + 1 TiB` addition overflow, and a root
address in `[ARCH_PAGE_OFFSET, ARCH_PAGE_OFFSET + (UINT64_C(1) << 40))`.
For every valid parent table descriptor, extract its PA, require 4 KiB
alignment and `< 1 TiB`, then form `pa + ARCH_PAGE_OFFSET` only after that
check. An invalid root or parent descriptor returns `AARCH64_PT_EINVAL` and
is never dereferenced.

Implement a private `walk_to_l3(root, va, create, ...)` that calculates
PGD/PUD/PMD/PTE indices. For `create=true`, allocate a missing 4 KiB table via
`alloc_4k_page()`, convert physical to `(void *)(pa + ARCH_PAGE_OFFSET)`, zero
4096 bytes, execute `dsb ishst`, link a table descriptor, execute `dsb ishst`,
and retain the linked table if later allocation fails. Return an unlinked newly
allocated page with `free_4k_page()` on that failure. Return `ECONFLICT` when
a valid non-table PUD/PMD descriptor is encountered. At L0, a valid descriptor
with its table bit clear is invalid/reserved in this 4-level regime, not a
block: return `AARCH64_PT_EINVAL` before extracting its PA or dereferencing it.

Implement a private active-root helper. Define:

```c
#define AARCH64_TTBR_BASE_MASK UINT64_C(0x000000fffffff000)
#define AARCH64_TTBR_ALLOWED_NONBASE (UINT64_C(0xffff000000000000) | UINT64_C(1))
```

It reads the raw `arch_get_page_table()` value, rejects any bit outside
`AARCH64_TTBR_BASE_MASK | AARCH64_TTBR_ALLOWED_NONBASE`, extracts
`ttbr_pa = raw & AARCH64_TTBR_BASE_MASK`, requires a nonzero 4 KiB-aligned
base below 1 TiB, converts it to its direct-map VA, and compares that pointer
to `root`. Only an exact match is active. The helper treats ASID bits [63:48]
and CnP bit 0 as permitted non-address fields; it never includes them in the
physical base.

Implement a private active-root invalidate helper:

```c
__asm__ volatile("tlbi vae1, %0\n\tdsb ish\n\tisb"
                 :: "r"(va >> 12) : "memory");
```

Call it after each active-root parent publication, L3 map, or L3 clear; issue
`dsb ishst` immediately after each descriptor store. Use the active-root
helper for this decision; a non-active root must not issue TLBI. In this
increment `aarch64_main()` is the sole active-root caller, before AP release.

- [ ] **Step 4: Implement public operations**

Make `map_4k` reject an existing leaf with `EEXIST`; `query_4k` and
`unmap_4k` allocate nothing and return `ENOENT` for absent levels. `unmap_4k`
returns the old PA/permission before clearing its L3 entry. Implement
`range_accessible` as a non-allocating page-by-page query with checks for
overflow, requested user access, and requested write access; `length == 0`
returns true.

- [ ] **Step 5: Cross-build the new source**

Run:

```bash
make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Expected: the wildcard AArch64 source discovery compiles and links
`page_table.c`; no generic VMM source is added to the AArch64 build.

- [ ] **Step 6: Commit the primitive layer**

```bash
git add kernel/include/kernel/arch/aarch64/page_table.h \
  kernel/arch/aarch64/page_table.c
git commit -m "feat(aarch64): add page-table primitives"
```

### Task 2: Add the pre-SMP active-root map smoke test

**Files:**
- Modify: `kernel/arch/aarch64/main.c:1-70`
- Modify: `kernel/include/kernel/arch/aarch64/page_table.h`
- Verify: `kernel/arch/aarch64/head.S:563-571`

**Interfaces:**
- Consumes: Task 1 APIs, PMM's `alloc_4k_page()`/`free_4k_page()`, and physical TTBR0 from `arch_get_page_table()`.
- Produces: exactly one `UEFI-A64: pt map smoke OK` on a successful self-test build, or a fatal diagnostic before DTB/GIC/SMP startup.

- [ ] **Step 1: Add a failing self-test expectation to the parser fixture**

In `tests/aarch64_uefi_smp.py`, add the required line after the existing PMM
line and before the topology line in the positive self-test fixture:

```text
UEFI-A64: pmm alloc smoke OK
UEFI-A64: pt map smoke OK
[smp] topology source=uefi-dtb cpus=2
```

Add an explicit self-test assertion that removes only the new page-table line
from that otherwise valid fixture and requires
`passed(..., expect_selftest=True)` to return false. The current parser accepts
that missing-line fixture because it recognizes only the PMM smoke line, making
this deliberately fail before the parser implementation changes.

- [ ] **Step 2: Run parser self-test and verify it fails**

Run: `python3 tests/aarch64_uefi_smp.py --self-test`

Expected: FAIL until `passed()` recognizes the added ordered smoke line.

- [ ] **Step 3: Implement the smoke helper in `main.c`**

Add a static `aarch64_pt_smoke_test()` gated by `#if OS01_SELFTEST`. It must:

1. read `ttbr_raw = (uint64_t)(uintptr_t)arch_get_page_table()` and reject
   bits outside `AARCH64_TTBR_BASE_MASK | AARCH64_TTBR_ALLOWED_NONBASE`;
2. derive `ttbr_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK`, requiring nonzero,
   4 KiB alignment, and `< (UINT64_C(1) << 40)`;
3. convert only that derived base to
   `(uint64_t *)(uintptr_t)(ttbr_pa + ARCH_PAGE_OFFSET)`;
4. require `aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA, ...)` to return
   `AARCH64_PT_ENOENT`;
5. allocate one 4 KiB physical page; map it as `AARCH64_PT_KERNEL_RW`;
6. store two distinct `uint64_t` sentinels through the self-test VA and verify
   them both through that VA and `(uint64_t *)(pa + ARCH_PAGE_OFFSET)`;
7. query the same PA and kernel-RW/non-exec permission, prove a second map is
   `AARCH64_PT_EEXIST`, then unmap and require a later query is `ENOENT`;
8. free the data page and log exactly `UEFI-A64: pt map smoke OK`.

On every failure, first unmap/free any resource currently owned by the test
when possible, then log `UEFI-A64: pt map smoke FAIL` and `[smp] FATAL: pt map
selftest`, and halt. Do not attempt to reclaim successfully linked intermediate
tables.

Call this helper immediately after the existing PMM allocation smoke and before
`dtb_init()`.

- [ ] **Step 4: Update parser ordering minimally**

Make the existing `expect_selftest` branch locate RAM summary, PMM smoke,
page-table smoke, and topology. Require exactly one PMM and exactly one page
table smoke line in that order between RAM and topology. Reject a missing,
duplicate, misplaced, or `UEFI-A64: pt map smoke FAIL` line. Leave the normal
and no-ACK predicates unchanged when `expect_selftest` is false.

- [ ] **Step 5: Make the QEMU target request the existing self-test check**

In `mk/components/run.mk`, add `--expect-selftest` to the Python invocation in
`test-aarch64-uefi-smp`, which already builds its image through
`$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi`. Do not add this flag to
`test-aarch64-uefi-smp-no-ack`.

- [ ] **Step 6: Run build and parser verification**

Run:

```bash
python3 tests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Expected: parser self-test passes and the normal kernel build includes the
new primitive source without requiring generic VMM code.

- [ ] **Step 7: Commit smoke and parser integration**

```bash
git add kernel/arch/aarch64/main.c \
  tests/aarch64_uefi_smp.py mk/components/run.mk
git commit -m "test(aarch64): smoke test page-table mappings"
```

### Task 3: Run the existing QEMU acceptance paths

**Files:**
- Verify: Task 1 and Task 2 files

**Interfaces:**
- Consumes: new smoke output and existing normal/degraded QEMU harness.
- Produces: runtime evidence that a mapped page is readable, queryable, unmapped, and that established SMP behavior remains intact.

- [ ] **Step 1: Run the normal self-test matrix**

Run:

```bash
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

Expected: every 1-, 2-, and 4-vCPU case includes exactly one valid RAM summary,
one PMM smoke line, one page-table smoke line, and the established SMP PASS
evidence.

- [ ] **Step 2: Run injected no-ACK recovery without self-test**

Run:

```bash
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
```

Expected: the two-vCPU injected run retains RAM evidence and its DEGRADED/SKIP
recovery result without requiring PMM/page-table smoke output; the final command
restores a normal image.

- [ ] **Step 3: Inspect the final scope**

Run:

```bash
git diff --check HEAD~2..HEAD
git diff --name-only HEAD~2..HEAD
```

Expected: no whitespace errors; production changes are limited to the new
AArch64 page-table header/source and the AArch64 boot self-test. No generic
VMM, VMA, ELF, scheduler, or x86 descriptor files appear.

- [ ] **Step 4: Commit a verification correction only if one was necessary**

If verification exposed a production or parser defect, commit only the repair
with `fix(aarch64): ...` or `test(aarch64): ...`. Otherwise do not create an
empty commit.

## Plan self-review

- Spec coverage: Task 1 covers descriptor ownership, validation, walking,
  allocation, conflict handling, and local TLB ordering; Task 2 covers the
  exact active-root mapping proof and parser ordering; Task 3 covers normal
  and degraded QEMU evidence.
- Scope: no generic VMM port, EL0, SVC, user mappings, table reclamation, host
  emulator, or callback-injection framework is included.
- Interface consistency: every later task uses the exact Task-1 API names and
  the `AARCH64_PT_SELFTEST_VA` constant defined in the public header.
