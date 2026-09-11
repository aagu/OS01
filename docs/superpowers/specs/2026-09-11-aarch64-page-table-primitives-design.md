# AArch64 Page-Table Primitives Design

## Status

Proposed. This design adds the minimal, architecture-owned translation-table
operations needed after AArch64 PMM bring-up. It does not port the generic VMM,
start EL0, or introduce syscalls.

## Context

AArch64 now boots through UEFI, publishes normalized RAM ranges, initializes
the physical memory manager, and can allocate/free a 2 MiB page in the
`KERNEL_SELFTEST` path. The recent PGD/PUD/PMD/PTE naming cleanup made the
generic hierarchy readable across architectures, but it deliberately left
`kernel/include/kernel/vmm.h`'s descriptor bits and `kernel/memory/vmm.c`
x86_64-specific.

The active AArch64 boot tables in `head.S` are static, shared by TTBR0_EL1 and
TTBR1_EL1, and only establish the early identity/direct/MMIO mappings. The
architecture currently has no safe way to add or remove a 4 KiB mapping after
PMM starts, and `arch_user_range_accessible()` is intentionally fail-closed.

## Goal

Provide a small AArch64-only API to create intermediate translation tables and
map, query, unmap, and permission-check 4 KiB stage-1 mappings in a
caller-supplied root. Demonstrate it against the active kernel root during the
existing AArch64 self-test boot, without changing the boot table layout.

## Non-goals

- Do not compile or adapt `kernel/memory/vmm.c`, `vma.c`, ELF loading, COW,
  `mmap`, or user task creation for AArch64.
- Do not alter TTBR0_EL1/TTBR1_EL1 ownership, TCR_EL1, MAIR_EL1, or the static
  mappings made by `head.S`.
- Do not expose the x86 `PAGE_*` bit constants to AArch64 code.
- Do not create an EL0 mapping, exception-return path, SVC handler, or change
  the fail-closed public syscall/uaccess policy in this increment.
- Do not free intermediate tables on unmap; table reclamation requires an
  ownership/lifetime policy and is a later task.

## Considered approaches

### Adapt `kernel/memory/vmm.c` directly

This would require separating x86 descriptor encoding, fixed `kernel_map`
physical assumptions, `calloc`/slab integration, user-root copying, and x86
TLB shootdown behavior at once. It is too large and makes the first dynamic
mapping hard to diagnose. Rejected.

### Grow dynamic mappings in `head.S`

Assembly can construct the boot tables but cannot use PMM allocations or
express normal runtime failure handling. It would entangle boot-only and
runtime mappings. Rejected.

### Add a standalone AArch64 table primitive layer

An AArch64-only C module can encode descriptors centrally, allocate 4 KiB
table pages from PMM, and accept an explicit root pointer. It enables a small
kernel self-test now and gives a later generic-VMM adapter a stable target.
Chosen.

## Interfaces

Create `kernel/include/kernel/arch/aarch64/page_table.h` and
`kernel/arch/aarch64/page_table.c`.

```c
enum aarch64_pt_perm {
    AARCH64_PT_KERNEL_RO = 1u << 0,
    AARCH64_PT_KERNEL_RW = 1u << 1,
    AARCH64_PT_USER_RO   = 1u << 2,
    AARCH64_PT_USER_RW   = 1u << 3,
    AARCH64_PT_EXEC      = 1u << 4,
    AARCH64_PT_DEVICE    = 1u << 5,
};

enum aarch64_pt_result {
    AARCH64_PT_OK = 0,
    AARCH64_PT_EINVAL = -1,
    AARCH64_PT_EEXIST = -2,
    AARCH64_PT_ENOENT = -3,
    AARCH64_PT_ENOMEM = -4,
    AARCH64_PT_ECONFLICT = -5,
};

int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                       uint32_t perm);
int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                         uint64_t *pa_out, uint32_t *perm_out);
int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                         uint64_t *pa_out, uint32_t *perm_out);
bool aarch64_pt_range_accessible(const uint64_t *root, uint64_t va,
                                 uint64_t length, bool write,
                                 bool user);
```

All roots and table pointers are high-half direct-map virtual addresses. `pa`
is physical. `arch_get_page_table()` remains unchanged and returns the raw
TTBR0_EL1 physical base; callers convert it to
`(uint64_t *)(ttbr_pa + ARCH_PAGE_OFFSET)` only after validating 4 KiB
alignment and `ttbr_pa < (UINT64_C(1) << 40)`.

`root`, `va`, and `pa` must be 4 KiB aligned; null roots fail. The installed
`T0SZ=T1SZ=16` accepts only canonical low VAs in
`[0, 0x0000ffffffffffff]` and canonical high VAs in
`[0xffff000000000000, UINT64_MAX]`; the intervening hole is invalid. The
installed `IPS=40` requires both leaf and table physical addresses below
1 TiB. Descriptor construction masks the output address field to that
40-bit physical range. Invalid inputs fail before any table modification.

`map_4k` creates only missing PUD/PMD/PTE tables using `alloc_4k_page()`;
each table is zeroed through its high-half direct-map address before it is
linked. It rejects an existing leaf (`EEXIST`) and a valid block descriptor at
PUD or PMD (`ECONFLICT`), with no modification to pre-existing tables. A
failure after allocating an unlinked table returns that page with
`free_4k_page()`; successfully linked parents and their table pages are
retained, even if a later level allocation fails.

`query_4k` never allocates. It returns `ENOENT` for an absent level and
`ECONFLICT` for a block descriptor, since this layer only owns 4 KiB leaves.
`unmap_4k` never allocates; it clears only an existing 4 KiB leaf and returns
its prior physical address and decoded permission. Missing/blocked paths do
not change the tree.

## Descriptor policy

The module owns all AArch64 stage-1 descriptor encoding and decoding; no
caller may write descriptor bits directly. It defines named constants for
valid, table, page, access flag, AttrIndx, shareability, AP, PXN, and UXN
fields. Constants must match the MAIR/TCR regime already installed in
`head.S`. AP[2:1] are exactly: kernel-RW `00`, kernel-RO `10`, user-RW `01`,
and user-RO `11`. Execute encoding is exactly: executable kernel mapping has
PXN clear and UXN set; executable user mapping has PXN set and UXN clear;
non-executable mapping has both set.

- normal mappings use the existing normal-memory AttrIndx and inner-shareable
  policy;
- device mappings use the existing device-memory AttrIndx, set both PXN/UXN,
  and reject `AARCH64_PT_EXEC`;
- kernel mappings disallow EL0 access;
- user mappings allow EL0 access;
- mappings are non-executable unless `AARCH64_PT_EXEC` is set;
- a permission word must select exactly one of kernel/user and exactly one of
  read-only/read-write; device-plus-exec and an unknown bit are `EINVAL`.

The first implementation maps only L3 4 KiB pages. It never creates L1/L2
block entries and refuses to walk through blocks installed by boot code.

## Ordering and invalidation

The required write/publication sequences are exact:

- allocating a table: zero it; `dsb ishst`; store the valid parent table
  descriptor; `dsb ishst`; if the root is active, invalidate the target VA;
  then `dsb ish; isb`;
- mapping an absent L3 leaf: store the valid leaf; `dsb ishst`; if the root
  is active, invalidate the target VA; then `dsb ish; isb`;
- unmapping an L3 leaf: clear it; `dsb ishst`; if the root is active,
  invalidate the target VA; then `dsb ish; isb`.

The local invalidation is exactly `tlbi vae1, va >> 12`. A non-active root
requires no TLB invalidation.

The module is BSP/pre-SMP-only in this increment: map/unmap/query on the
active root may be called only before AP release. No caller after
`smp_boot_aps()` may modify the active root; doing so is unsupported and must
not be introduced by this change. Post-SMP mappings require a later IPI/TLB
shootdown design.

## Active-root self-test

Add a self-test gated by `OS01_SELFTEST` after `pmm_init()` and before
`dtb_init()`/SMP release in `aarch64_main()`.

1. Obtain the active root through `arch_get_page_table()`, validate its
   physical base, then convert it to its direct-map virtual address.
2. Require a fixed `AARCH64_PT_SELFTEST_VA` to be initially absent via
   `aarch64_pt_query_4k()`; define it as `0xffff800000000000`, whose lower-48
   L0 index 256 is absent from current boot tables.
3. Allocate one 4 KiB physical page with `alloc_4k_page()`, map it kernel-RW
   and non-executable, write two distinct 64-bit sentinel values through the
   self-test virtual address, and verify reads through both that VA and the
   high-half direct-map alias of its physical address.
4. Query and validate the physical address and decoded kernel-RW/non-exec
   permission; verify a duplicate map returns `EEXIST`.
5. Unmap it, verify query returns `ENOENT`, free the physical page, and emit
   exactly `UEFI-A64: pt map smoke OK`.

Every unexpected result logs `UEFI-A64: pt map smoke FAIL` followed by an
`[smp] FATAL:` reason and halts before hardware/SMP setup. The test leaves no
leaf mapping or data page allocation behind. Intermediate tables may remain,
as specified by the no-reclamation rule.

TTBR0_EL1 and TTBR1_EL1 intentionally point to the same `boot_page_tables`
root. Therefore the self-test's high VA has a corresponding lower-48-bit
TTBR0 alias; it is not a permanent kernel-private address. Its initially
absent L0 slot and explicit unmap prevent either alias from remaining usable.

## Testing

Extend the existing AArch64 QEMU self-test parser only enough to require the
single `UEFI-A64: pt map smoke OK` line after the PMM smoke line. The normal
and injected no-ACK paths remain self-test independent. The existing
`test-aarch64-uefi-smp` target always builds/runs its self-test variant and,
after this change, always passes `--expect-selftest`; it requires the PMM and
page-table smoke lines in that order before topology evidence.

Acceptance commands:

```bash
python3 tests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

The last command must retain valid RAM/PMM smoke evidence, the new page-table
smoke evidence, and existing SMP PASS results. The injected no-ACK test is run
without `KERNEL_SELFTEST=1` and must preserve its established DEGRADED result.

## Follow-on boundary

The next design may adapt generic VMM operations to this API, choose separate
TTBR0 user roots, and wire `arch_user_range_accessible()` to the table walker.
EL0 entry and SVC exception handling require that separate design because they
need task context, exception-frame, and signal ABI decisions in addition to
page mappings.
