# Fix Task 9 — exec ENOEXEC (systest hostile-group crash)

## Status: DONE — 227/227 systest, QEMU-verified

## Symptom (as parked in bf49bcf)

`make OS01_SYSTEST=1 test-syscall` ran the hostile group up to
`munmap_write`, then killed systest (`user fault at RIP=0x401956`). init
respawned `/bin/systest` which looped forever returning `-8 (-ENOEXEC)`
because `elf_load` seg 0 hit `alloc_pages` OOM — ~300 LOAD_FAIL prints.

## Actual root cause (not what the handover suspected)

The handover blamed a 2 MiB shared-fallback refcount leak in
`fork_mm_copy`. That was **wrong**. Instrumented runs showed:

- ZONE_NORMAL free stayed flat at 232 pages through all 29 fork children
  — no 2 MiB leak.
- The crash was a **4 KiB demand-paging infinite loop**: the fault
  handler kept allocating a fresh 4 KiB page (`alloc=119850, free=0`),
  draining the whole free pool as subpage pools, then OOM'd.

The bug: `do_page_fault`'s demand-paging paths computed the faulting
page's **base address** with `PAGE_4K_ALIGN(cr2)`.

```c
#define PAGE_4K_ALIGN(addr) (((unsigned long)(addr) + PAGE_4K_SIZE - 1) & PAGE_4K_MASK)
```

`PAGE_4K_ALIGN` rounds **up**, not down. For a fault at `0x40000ff0`
(the `page_tail_nul_open` test writes `/bin/spin` at `base + 0xff0`), the
handler mapped `0x40001000` (the *next* page) instead of `0x40000000`.
The write never resolved → the same instruction re-faulted forever, each
iteration leaking one 4 KiB slot and eventually one 2 MiB subpage pool.

Why earlier tests passed: they only ever demand-paged at page-aligned
offsets, where `PAGE_4K_ALIGN(addr) == addr`. The COW fault path walks
`cr2` directly (correct); only the two P=0→P=1 demand paths mis-aligned.

## Fix

`kernel/arch/x86_64/trap.c` — use align-down in both demand-paging paths
(anon and file-backed):

```c
// before
vmm_map_4k_page(user_pml4, phys, PAGE_4K_ALIGN(cr2), vma->vm_page_prot);
// after
vmm_map_4k_page(user_pml4, phys, cr2 & PAGE_4K_MASK, vma->vm_page_prot);
```

## Verification

```
[SYS TEST] RESULT: 227 passed, 0 failed
```

(three consecutive runs, no instrumentation).

## Notes

- The handover's 2 MiB shared-fallback refcount gap is real code smell but
  is **not** the cause of this defect — both `child_pml2[l2] = pml2e;`
  fallbacks only trigger on OOM, and no OOM occurs before the crash. Left
  untouched.
- `page_clean()` on 2 MiB pages that were never `page_init`'d still
  underflows `reference_count`; pre-existing and out of scope.
