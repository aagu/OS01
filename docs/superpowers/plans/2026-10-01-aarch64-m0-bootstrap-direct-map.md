# AArch64 M0 Bootstrap Direct Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing AArch64 `0x40000000..0x80000000` bootstrap direct map present before MMU enable, with correct PXN/UXN bits, in both normal and selftest images.

**Architecture:** Keep the six static boot page-table pages and shared TTBR0/TTBR1 root. Correct the execution-permission mask and fill `PMD_low1[0..511]` in `head.S::build_pagetables`; remove the later C fixup. A selftest inspects the installed descriptors before any code can repair them, then the existing 4 KiB smoke proves access through a newly mapped block.

**Tech Stack:** AArch64 assembly, freestanding C, Clang/LLD, QEMU `virt,gic-version=2`, existing Python QEMU harness.

**Spec:** `docs/superpowers/specs/2026-10-01-aarch64-m0-bootstrap-direct-map-design.md`

## Global Constraints

- Scope is M0's fixed `0x40000000..0x80000000` window; RAM above that address, sparse mapping, Slab, VMM, VMA and EL0 remain M1–M4 work.
- Keep `ARCH_PAGE_OFFSET=0xffff000000000000`, the 24 KiB/six-page boot-table layout, shared TTBR0/TTBR1 root, TCR, MAIR and existing `dsb sy`/`isb` enable order.
- L2 block entries use `bit[1:0]=01`, Normal low flags `0x705`, PXN bit 53, UXN bit 54, and bit 52 clear. `PMD_low1[0]` has UXN=1/PXN=0; `PMD_low1[1..511]` has UXN=PXN=1.
- Preserve `PMD_low0` Normal/Device attributes and physical routing; keep `AARCH64_PT_SELFTEST_VA` initially unmapped and the existing `UEFI-A64: pt map smoke OK` contract.
- Do not add a page-table allocator, change `kernel/memory/vmm.c`, or alter x86_64 build inputs. No struct layout changes are planned; `make clean` is not required by this plan.

## Review Focus

These failure modes are easy to miss even when a 1-CPU boot appears healthy; the named task tests must pin each one:

1. **Bit 52 left set on low0 Normal or MMIO blocks:** Task 1's installed-descriptor checks require bit 52 clear and the correct AttrIndx on both.
2. **Kernel-image block made PXN or left UXN-clear:** Task 1 checks both bits on `PMD_low1[0]` and boots into high-half C.
3. **A middle or final `PMD_low1` slot omitted:** Task 2 checks the PA and full descriptor for every index 1..511 before the old fixup can run.
4. **Selftest passes only because C repairs the table:** Task 2 runs the descriptor check before the fixup, deletes the fixup, and boots a separate ordinary image.
5. **4 KiB smoke only touches the old first block or unsupported high RAM:** Task 2 asserts `0x40200000 <= PA < 0x80000000` before treating its read/write result as M0 evidence.

---

## File responsibilities

| File | Change |
|------|--------|
| `kernel/arch/aarch64/head.S` | Own the bit 53/54 no-execute mask and all fixed-window `PMD_low1` block descriptors before MMU enable. |
| `kernel/arch/aarch64/boot/main.c` | In `OS01_SELFTEST`, inspect the actual active boot tables and bound the 4 KiB smoke PA; remove the late fixup call. |
| `kernel/arch/aarch64/boot/boot_fixup.c` | Delete after `head.S` owns the mapping. |
| `qemutests/aarch64_uefi_smp.py` | Reuse its existing normal/selftest completion parser; no change expected. |

### Task 1: Correct and prove bootstrap execute permissions

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c` (`#if OS01_SELFTEST` block after `pmm_init()`)
- Modify: `kernel/arch/aarch64/head.S` (`PT_UXN_MASK`, `x20`, `PMD_low1[0]`)
- Test: existing `qemutests/aarch64_uefi_smp.py` 1-CPU selftest mode

**Interfaces:**
- Consumes: live `arch_get_page_table()` TTBR0 value, `ARCH_PAGE_OFFSET`, and the existing selftest boot sequence.
- Produces: `static void aarch64_boot_map_selftest(void)` in `main.c`; Task 2 expands this same check to the remaining 511 slots.

- [ ] **Step 1: Add the failing installed-descriptor test.** Place `aarch64_boot_map_selftest()` after the PMM alloc/free smoke and before `aarch64_extend_direct_map()`. Validate TTBR0 base as `aarch64_pt_smoke_test()` does, then follow actual `PGD[0] → PUD[0] → PMD_low0` and `PUD[1] → PMD_low1` table descriptors through `ARCH_PAGE_OFFSET`. Fail with `[smp] FATAL: aarch64 boot map selftest` and halt before GIC/SMP on malformed table links or mismatches. Assert exact block type, PA, low flags and execution bits for `PMD_low0[0]` (`PA=0`, Normal `0x705`, PXN|UXN), `PMD_low0[0x40]` (`PA=0x08000000`, Device `0x401`, PXN|UXN), and `PMD_low1[0]` (`PA=0x40000000`, Normal `0x705`, UXN only). Require bit 52 clear on all three; read actual entries, not a constructor's result. Keep the existing PMM/page-table success markers unchanged.
- [ ] **Step 2: Verify the test is red on the current head.S.** Build `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi`, then run:

  ```bash
  python3 qemutests/aarch64_uefi_smp.py --cpus 1 --repeat 1 --timeout 30 \
    --expect-selftest --diagnostic-dtb auto \
    --firmware build/aarch64-clang/image/selftest/QEMU_EFI.fd \
    --image build/aarch64-clang/image/selftest/aarch64-uefi.img \
    --qemu qemu-system-aarch64 --log-dir /tmp/os01-aarch64-m0-t1-red
  ```

  Expect a nonzero harness result and exactly one boot-map FATAL before the topology marker. A timeout alone is not red evidence; inspect the saved log.
- [ ] **Step 3: Fix only the boot execution mask.** Set `PT_UXN_MASK=(1 << 54)` in `head.S`; make `x20` contain only bits 53 and 54. Construct `PMD_low1[0]` with UXN set and PXN cleared. `PMD_low0` Normal and Device blocks must inherit both bits; keep their AttrIndx values and PA routes. Do not fill `PMD_low1[1..511]` yet; the existing selftest C fixup keeps Task 1 bootable.
- [ ] **Step 4: Verify green and commit.** Rebuild the selftest image and rerun Step 2's command with `--expect-gic --expect-clk` and a fresh `--log-dir /tmp/os01-aarch64-m0-t1-green`; expect one PMM smoke, one page-table smoke, SMP PASS and timer ticks, with no boot-map FATAL. Run `git diff --check`, then commit `head.S` and `main.c` as `fix(aarch64): correct bootstrap execute permissions`.

### Task 2: Fill the fixed window before MMU enable and remove the C fixup

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c` (`aarch64_boot_map_selftest()`, `aarch64_pt_smoke_test()`, fixup call)
- Modify: `kernel/arch/aarch64/head.S` (`PMD_low1` construction)
- Delete: `kernel/arch/aarch64/boot/boot_fixup.c`
- Test: existing `qemutests/aarch64_uefi_smp.py` normal/selftest modes; `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi`; sync-fault mode

**Interfaces:**
- Consumes: Task 1's `aarch64_boot_map_selftest()` and corrected `x20` mask.
- Produces: all 512 installed L2 blocks in `PMD_low1` before `write_tcr_mair_ttbr`; no `aarch64_extend_direct_map()` symbol or caller.

- [ ] **Step 1: Extend the test to the full window and PA proof.** In `aarch64_boot_map_selftest()`, walk `PMD_low1[1..511]` and require each actual entry equal `(0x40000000 + i * 0x200000) | 0x60000000000705`; this checks PA, type, Normal attributes, PXN/UXN and bit 52 in one assertion. Keep it immediately before the old C fixup so the baseline fails on slot 1. In `aarch64_pt_smoke_test()`, after a successful `alloc_4k_page()` and after setting `data_owned=true`, require `0x40200000 <= data_pa < 0x80000000`; on failure use the existing cleanup/fatal path. Keep `AARCH64_PT_SELFTEST_VA` initially absent and preserve the current smoke marker.
- [ ] **Step 2: Verify the extended test is red for missing bootstrap slots.** Build the selftest image and run Task 1 Step 2's command with a fresh `--log-dir /tmp/os01-aarch64-m0-t2-red`. Expect nonzero result and boot-map FATAL before the C fixup or topology marker. Check the log distinguishes this from the later 4 KiB smoke.
- [ ] **Step 3: Fill slots in head.S and remove the workaround.** After writing `PMD_low1[0]`, write indices 1..511 using PA `0x40000000 + i * 0x200000`, low flags `x22` (`0x705`), and corrected `x20` (PXN|UXN); preserve the current register and `dsb sy` boot order. Delete `boot_fixup.c`, its declaration and `#if OS01_SELFTEST` call/comment in `main.c`. Leave the new verifier and the existing 4 KiB smoke inside `OS01_SELFTEST`. Build source discovery is wildcard-based; do not add a replacement object.
- [ ] **Step 4: Verify the selftest and other aarch64 variants.** Run `make PROFILE=aarch64-clang test-aarch64 MODE=smp`, `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi`, and `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault`. Require the existing SMP/GIC/Timer success evidence and sync-fault diagnostic; the selftest must pass its descriptor scan and access smoke. Confirm `rg -n 'aarch64_extend_direct_map|boot_fixup' kernel/arch/aarch64` has no remaining production references.
- [ ] **Step 5: Verify the ordinary image separately.** Build `make PROFILE=aarch64-clang aarch64-uefi`, then run:

  ```bash
  python3 qemutests/aarch64_uefi_smp.py --cpus 1 2 --repeat 1 --timeout 90 \
    --expect-clk --diagnostic-dtb auto \
    --firmware build/aarch64-clang/image/QEMU_EFI.fd \
    --image build/aarch64-clang/image/aarch64-uefi.img \
    --qemu qemu-system-aarch64 --log-dir /tmp/os01-aarch64-m0-normal
  ```

  Require complete SMP/tick evidence without `--expect-selftest`. Confirm the normal and selftest image hashes differ and the normal image needs no selftest-only fixup. Run `make PROFILE=x86_64-clang test-static` and `git diff --check` to check the cross-arch boundary.
- [ ] **Step 6: Commit.** Commit `head.S`, `main.c`, and the deletion of `boot_fixup.c` as `fix(aarch64): build bootstrap direct map before MMU enable` after all gates pass.

## Plan completion

The final review compares the resulting diff to every section of the spec and checks that all planned verification commands produced fresh success output. Report M0 only: do not claim that M1 runtime RAM mapping, aarch64 Slab/VMM/VMA, or EL0 are complete.
