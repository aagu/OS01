# AArch64 UEFI Bootloader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the existing AArch64 phase-1 kernel through AAVMF/UEFI and
handoff a validated `boot_context` while retaining the direct `-kernel` boot
target as a regression path.

**Architecture:** A private AArch64 posix-uefi build produces
`BOOTAA64.EFI`; it validates and fixed-address-loads `kernel.elf`, captures
firmware state into `0x401e0000..0x40200000`, exits boot services, normalizes
EL1 translation state in a physical trampoline, and branches to `0x40080000`.
The kernel identifies UEFI only by the fixed handoff address, otherwise
retains the direct FDT ABI.

**Tech Stack:** freestanding C and AArch64 assembly, posix-uefi,
clang/lld ARM64 PE/COFF, QEMU `virt`, AAVMF, FAT ESP tools (`mkfs.fat`, mtools).

**Spec:** `docs/superpowers/specs/2026-08-30-aarch64-uefi-bootloader-design.md`

## Global Constraints

- `BOOT_CONTEXT_VERSION` becomes `2` when the `magic` field is added; all ABI
  fields are fixed-width and host tests assert the new layout.
- A UEFI handoff is selected only by `x0 == 0x401e0000`; a bad magic/version/
  size at that address must print through PL011 and halt, never fall back.
- `run-aarch64-uefi` uses `-smp 1`; the UEFI kernel path must skip
  `smp_boot_aps()`.  `run-aarch64` remains a direct-boot regression target.
- The loader may pass only copied FDT bytes in the mapped handoff area, never
  a firmware-owned FDT pointer.
- Before entering the kernel, the trampoline supplies EL1 with `M=0`, masked
  DAIF, and invalidated EL1 stage-1 TLB state.  When the loader is running at
  EL2, these writes target the EL1 state inherited by the kernel; do not clear
  `SCTLR_EL2`.  The short EL2 path remains firmware-mapped until `head.S`
  performs its existing EL2-to-EL1 drop.
- Loader diagnostics used by tests write QEMU virt PL011 at `0x09000000`;
  UEFI `ConOut` is not test evidence under `-display none`.
- Do not modify or stage `thirdpart/busybox-1.36.1` or the untracked
  `thirdpart/posix-uefi` checkout.  The wrapper must use a per-build private
  runtime copy to avoid posix-uefi's architecture-mixed `uefi/*.o` wildcard.

---

## File Structure

| File | Responsibility |
|---|---|
| `kernel/include/kernel/bootinfo.h` | v2 boot-context magic and raw-UEFI map format |
| `test/cases/test_bootinfo_abi.c` | fixed-size ABI and constructor tests |
| `boot/uefi/aarch64/loader.h` | fixed addresses, ELF/FDT limits, loader-only declarations |
| `boot/uefi/aarch64/elf.c` | bounded ELF validation, page interval computation, segment load |
| `boot/uefi/aarch64/main.c` | UEFI protocols, FDT/GOP/map handoff, EBS transaction |
| `boot/uefi/aarch64/handoff.S` | physical MMU/TLB teardown and kernel branch |
| `boot/uefi/aarch64/Makefile` | isolated ARM64 posix-uefi wrapper |
| `test/cases/test_aarch64_elf_loader.c` | host tests for overflow, ELF and interval helpers |
| `kernel/arch/aarch64/main.c` | dual-mode context validation and UEFI single-BSP policy |
| `Makefile` | ARM64 ESP, AAVMF copy, `run-aarch64-uefi` target |

### Task 1: Version the boot-context ABI

**Files:**
- Modify: `kernel/include/kernel/bootinfo.h`
- Modify: `test/cases/test_bootinfo_abi.c`
- Modify: `test/Makefile`

**Interfaces:**
- Produces `#define BOOT_CONTEXT_MAGIC UINT32_C(0x4f533031)`,
  `#define BOOT_CONTEXT_VERSION 2u`, and
  `BOOT_MEMORY_FORMAT_UEFI_RAW = 3`.
- Produces `bool boot_context_valid(const struct boot_context *ctx)` which
  requires matching magic/version and `size == sizeof(*ctx)`.

- [ ] **Step 1: Write the failing ABI tests**

  Add assertions that `boot_context_init()` sets `magic`, version 2 and exact
  size; mutate each of magic, version, and size and assert
  `boot_context_valid()` is false.  Assert `BOOT_MEMORY_FORMAT_UEFI_RAW == 3`
  so the numeric ABI cannot silently drift.

- [ ] **Step 2: Run the test red**

  Run: `make -C test build/test_bootinfo_abi.elf`

  Expected: compilation failure because the new constants, member, and
  validator do not yet exist.

- [ ] **Step 3: Implement the ABI change**

  Place `uint32_t magic` before `version` in `struct boot_context`; update
  `boot_context_init()` to zero the struct then write magic, version and size.
  Implement the validator without libc.  Do not add UEFI headers or UEFI
  structs to this kernel header.

- [ ] **Step 4: Run ABI and x86 regressions**

  Run: `make -C test build/test_bootinfo_abi.elf build/test_pmm_basic.elf`

  Expected: both succeed.

- [ ] **Step 5: Commit**

  Run: `git add kernel/include/kernel/bootinfo.h test/cases/test_bootinfo_abi.c test/Makefile && git commit -m "boot: version boot context handoff ABI"`

### Task 2: Create testable fixed-address ELF helpers

**Files:**
- Create: `boot/uefi/aarch64/loader.h`
- Create: `boot/uefi/aarch64/elf.c`
- Create: `test/cases/test_aarch64_elf_loader.c`
- Modify: `test/Makefile`

**Interfaces:**
- Produces `int aarch64_elf_validate(const void *image, uint64_t image_size,
  uint64_t *entry_out)` and
  `int aarch64_page_interval(uint64_t paddr, uint64_t memsz,
  uint64_t *start_out, uint64_t *pages_out)`.
- `0` means valid; negative values distinguish malformed headers, overflow,
  bad entry, and collision/range failure.

- [ ] **Step 1: Write failing host tests**

  Build a 64-byte in-memory ELF64 header and program-header array.  Cover:
  valid `ET_EXEC`/`EM_AARCH64` entry `0x40080000`; bad magic; `p_filesz >
  p_memsz`; `e_phoff + e_phnum * e_phentsize` overflow; segment file range
  outside image; and two byte ranges that share a page producing identical
  page intervals.

- [ ] **Step 2: Run the test red**

  Run: `make -C test build/test_aarch64_elf_loader.elf`

  Expected: target/source missing.

- [ ] **Step 3: Implement pure helpers**

  Define local ELF64 wire structs in `loader.h`; validate ELF magic/class/LSB,
  `ET_EXEC`, `EM_AARCH64`, exact ELF/program header sizes, and all checked
  additions/multiplications.  Require entry `0x40080000` and loaded physical
  intervals below `0x401e0000`.  Round with `start = paddr & ~0xfffULL` and
  `end = (paddr + memsz + 0xfffULL) & ~0xfffULL` only after overflow checks.

- [ ] **Step 4: Run the helper tests**

  Run: `make -C test build/test_aarch64_elf_loader.elf`

  Expected: PASS.

- [ ] **Step 5: Commit**

  Run: `git add boot/uefi/aarch64/loader.h boot/uefi/aarch64/elf.c test/cases/test_aarch64_elf_loader.c test/Makefile && git commit -m "boot: validate aarch64 kernel elf ranges"`

### Task 3: Build an isolated ARM64 EFI application and ESP

**Files:**
- Create: `boot/uefi/aarch64/Makefile`
- Create: `boot/uefi/aarch64/main.c` (temporary PL011 build marker only)
- Modify: `Makefile`

**Interfaces:**
- Produces `build/aarch64/uefi/BOOTAA64.EFI` and
  `build/aarch64/disk.img` with `/EFI/BOOT/BOOTAA64.EFI` and `/kernel.elf`.
- Produces `make run-aarch64-uefi` which uses a writable
  `build/aarch64/QEMU_EFI.fd` copy and `-smp 1`.

- [ ] **Step 1: Write build checks before wrapper implementation**

  Add a `check-aarch64-uefi-artifacts` target that runs:

  ```sh
  file build/aarch64/uefi/BOOTAA64.EFI | grep 'PE32+.*ARM64'
  mdir -i build/aarch64/disk.img ::/EFI/BOOT | grep BOOTAA64.EFI
  mdir -i build/aarch64/disk.img :: | grep kernel.elf
  ```

- [ ] **Step 2: Run it red**

  Run: `make check-aarch64-uefi-artifacts`

  Expected: missing artifact failure.

- [ ] **Step 3: Implement private runtime staging and packaging**

  In `boot/uefi/aarch64/Makefile`, copy only the posix-uefi runtime into
  `build/aarch64/uefi-runtime` and invoke its Makefile there with
  `ARCH=aarch64`, `TARGET=BOOTAA64.EFI`, an AArch64-only output directory, and
  an explicit source list:

  ```makefile
  SRCS := $(abspath boot/uefi/aarch64/main.c) \
          $(abspath boot/uefi/aarch64/elf.c) \
          $(abspath boot/uefi/aarch64/handoff.S)
  $(MAKE) -C build/aarch64/uefi-runtime ARCH=aarch64 TARGET=BOOTAA64.EFI \
      OUTDIR=$(abspath build/aarch64/uefi) SRCS="$(SRCS)"
  ```

  This is required because the runtime Makefile otherwise derives `SRCS` from
  its own current working directory.  Do not override `OBJS`: it is derived
  from the explicit `SRCS`.  `OUTDIR` isolates application objects and the EFI
  image only; its `uefi/*.o`, `crt_aarch64.o`, and `libuefi.a` remain in the
  private runtime tree by design.  Delete/recreate that private directory
  before each ARM64 build; do not run `clean` in `boot/uefi/uefi` and do not
  write into `thirdpart/posix-uefi`.
  In the root Makefile create a FAT image, make `EFI/BOOT`, copy the EFI app
  as `BOOTAA64.EFI`, copy `build/aarch64/kernel/kernel.elf` as `kernel.elf`,
  and copy the host combined firmware into `build/aarch64/QEMU_EFI.fd`.

- [ ] **Step 4: Run artifact checks and x86 loader build**

  Run: `make -C kernel ARCH=aarch64 && make check-aarch64-uefi-artifacts && make boot/uefi/BOOTX64.EFI`

  Expected: ARM64 PE/COFF and ESP checks pass; x86 artifact still builds.

- [ ] **Step 5: Commit**

  Run: `git add Makefile boot/uefi/aarch64/Makefile boot/uefi/aarch64/main.c && git commit -m "build: package aarch64 uefi boot image"`

### Task 4: Implement the UEFI handoff transaction

**Files:**
- Modify: `boot/uefi/aarch64/main.c`
- Modify: `boot/uefi/aarch64/elf.c`
- Create: `boot/uefi/aarch64/handoff.S`
- Modify: `boot/uefi/aarch64/Makefile`

**Interfaces:**
- Produces `EFI_STATUS aarch64_load_kernel(...)`,
  `EFI_STATUS aarch64_build_handoff(...)`, and
  `__attribute__((noreturn)) void aarch64_enter_kernel(uint64_t entry,
  uint64_t context_phys)`.

- [ ] **Step 1: Add loader error-path tests/markers**

  Add deterministic test images for a bad ELF header and an oversized FDT/map
  reservation.  The loader must emit `UEFI-A64: bad ELF` or
  `UEFI-A64: handoff overflow` through a minimal PL011 `putc` routine before
  returning an EFI error.  Do not use `printf`/ConOut as the assertion source.

- [ ] **Step 2: Run the negative checks red**

  Run: `make test-aarch64-uefi-negative`

  Expected: target unavailable or no required PL011 marker.

- [ ] **Step 3: Implement loading and copied handoff state**

  Read `\\kernel.elf`; call the Task-2 validator; reserve each page-rounded
  `PT_LOAD` range with `AllocatePages(AllocateAddress, EfiLoaderData, ...)`;
  copy to `p_paddr`; zero BSS; reject interval overlap.  Reserve exactly
  `0x401e0000..0x40200000` before map collection.  Discover GOP and copy its
  fixed-width values.  Search configuration tables for the FDT GUID, validate
  big-endian FDT magic and `totalsize`, then copy the FDT into the handoff
  region and store that copied physical address.

  Fill `boot_context` with magic/version/size, `BOOT_CONTEXT_HAS_BOOT_CPU_ID`,
  optional framebuffer/FDT flags, and `BOOT_MEMORY_FORMAT_UEFI_RAW` metadata.
  Call `boot_context_valid()` on the completed context before the EBS
  transaction; on failure emit `UEFI-A64: corrupt handoff` via PL011 and
  return an EFI error without jumping to the kernel.
  Obtain the final map directly into remaining handoff storage.  Make no
  allocations, protocol calls, filesystem operations, or output between final
  `GetMemoryMap` and `ExitBootServices`; retry invalid map keys and recheck
  capacity on every retry.

- [ ] **Step 4: Implement the transition trampoline**

  The last loader action calls an assembly routine copied/linked at a physical
  reachable address.  It masks DAIF, executes the architecturally ordered EL1
  sequence `dsb sy; mrs sctlr_el1; bic M; msr sctlr_el1; isb; tlbi vmalle1;
  dsb sy; isb`, sets `x0` to `0x401e0000`, and branches to the physical ELF
  entry.  Its pre-transition branch target must be verified reachable under
  the active firmware mapping; on failure return before EBS.  Do not touch
  `SCTLR_EL2` or `TLBI VMALLE2` in this slice.

- [ ] **Step 5: Run negative and artifact verification**

  Run: `make test-aarch64-uefi-negative && make check-aarch64-uefi-artifacts`

  Expected: malformed inputs stop before EBS with PL011 markers; normal
  image remains ARM64 PE/COFF.

- [ ] **Step 6: Commit**

  Run: `git add boot/uefi/aarch64 && git commit -m "boot: hand off aarch64 kernel from uefi"`

### Task 5: Add dual-mode kernel entry and single-BSP UEFI policy

**Files:**
- Modify: `kernel/arch/aarch64/main.c`
- Modify: `kernel/arch/aarch64/head.S` (comments/signature declaration only if required)

**Interfaces:**
- Consumes `const struct boot_context *` when `x0 == 0x401e0000`; consumes
  the old `uint64_t` FDT semantics otherwise.
- Produces PL011 marker `OS01 aarch64 uefi handoff ok\n` and preserves direct
  boot behavior.

- [ ] **Step 1: Add a host-testable mode selector**

  Add a helper which performs no `mrs` and no context construction:

  ```c
  enum aarch64_boot_mode {
      AARCH64_BOOT_DIRECT_ZERO,
      AARCH64_BOOT_DIRECT_FDT,
      AARCH64_BOOT_UEFI,
      AARCH64_BOOT_CORRUPT,
  };
  enum aarch64_boot_mode aarch64_select_boot_mode(
      uint64_t x0, const struct boot_context *fixed_context);
  ```

  Test all four cases: zero, nonzero direct FDT address, a valid `x0 ==
  0x401e0000` handoff, and that fixed-address mode with damaged magic.  The
  host test passes a stack-resident context as `fixed_context`; production
  passes `(const struct boot_context *)0x401e0000`.  Keep `MPIDR_EL1` reading
  and `boot_context_from_aarch64()` construction in `aarch64_main`, so the
  selector is compilable by the x86 host test.

- [ ] **Step 2: Run the test red**

  Run: `make -C test build/test_bootinfo_abi.elf`

  Expected: helper or new invalid-handoff assertions absent.

- [ ] **Step 3: Implement mode selection and policy**

  Change `aarch64_main` to receive `const struct boot_context *handoff`.
  Because `head.S` already saves/restores `x0`, leave its instruction flow
  unchanged.  If the value equals `0x401e0000`, validate the ABI and on any
  failure write `UEFI-A64: corrupt handoff\n` via PL011 then halt.  Otherwise
  use `boot_context_from_aarch64(&scratch, (uint64_t)handoff, mpidr)` to
  preserve direct FDT/zero fallback behavior.  On UEFI, print the marker and
  skip `smp_boot_aps()`; retain GIC/timer initialization and IRQ enable.

- [ ] **Step 4: Run build and direct-boot regression**

  Run: `make -C kernel ARCH=aarch64 && timeout 20 make run-aarch64 AARCH64_SMP=1`

  Expected: direct boot prints `OS01 aarch64 phase1 boot ok` and a tick; it
  must not print `UEFI-A64: corrupt handoff`.

- [ ] **Step 5: Commit**

  Run: `git add kernel/arch/aarch64/main.c kernel/arch/aarch64/head.S test/cases/test_bootinfo_abi.c test/Makefile && git commit -m "boot: accept aarch64 uefi boot context"`

### Task 6: Execute end-to-end QEMU acceptance and document evidence

**Files:**
- Modify: `Makefile`
- Modify: `docs/architecture.md`

- [ ] **Step 1: Add a bounded UEFI smoke target**

  Implement `test-aarch64-uefi-smoke` using `timeout 30` and a log file.  It
  runs `run-aarch64-uefi`, then requires all of:

  ```sh
  grep -F 'OS01 aarch64 uefi handoff ok' build/aarch64/uefi-smoke.log
  grep -F 'OS01 aarch64 phase1 boot ok' build/aarch64/uefi-smoke.log
  grep -F '[tick] 1' build/aarch64/uefi-smoke.log
  ! grep -F '[spinlock] starting' build/aarch64/uefi-smoke.log
  ```

- [ ] **Step 2: Run it red**

  Run: `make test-aarch64-uefi-smoke`

  Expected: no handoff marker before Task 4/5 integration.

- [ ] **Step 3: Document the two AArch64 boot paths**

  Add the AAVMF command, ESP fallback path, fixed physical addresses,
  single-BSP limitation, and direct `run-aarch64` retention to
  `docs/architecture.md`.  State that UEFI raw memory descriptors are not yet
  consumed by PMM and PSCI CPU_ON is a separate next slice.

- [ ] **Step 4: Run full verification**

  Run: `make test-aarch64-uefi-smoke && make -C kernel ARCH=aarch64 && make -C test build/test_bootinfo_abi.elf build/test_pmm_basic.elf && make boot/uefi/BOOTX64.EFI`

  Expected: all commands succeed; smoke log has marker/banner/tick and no SMP
  benchmark line.

- [ ] **Step 5: Commit**

  Run: `git add Makefile docs/architecture.md && git commit -m "test: cover aarch64 uefi boot smoke"`

## Plan self-review

- Spec coverage: Tasks 1/5 cover N1/N2 and dual ABI; Task 4 covers copied FDT,
  raw map, EBS retry, PL011 errors, and MMU handoff; Task 3 covers isolated
  ARM64 PE/ESP/AAVMF packaging; Task 5 covers single-BSP; Task 6 covers every
  acceptance command and direct regression.
- Placeholder scan: no deferred implementation instructions are used; each
  task supplies concrete file paths, interfaces, commands, and expected
  outcomes.
- Type consistency: `boot_context_valid`, `aarch64_elf_validate`,
  `aarch64_page_interval`, and `aarch64_main(const struct boot_context *)`
  are defined before their consumers.
