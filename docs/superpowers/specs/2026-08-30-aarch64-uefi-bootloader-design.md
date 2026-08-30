# AArch64 UEFI bootloader design

**Date:** 2026-08-30  
**Status:** approved design; implementation pending plan review

## Goal

Make the AArch64 QEMU target boot through AAVMF/UEFI rather than QEMU's
direct `-kernel` protocol.  The new path is:

```text
QEMU virt + AAVMF -> /EFI/BOOT/BOOTAA64.EFI -> ExitBootServices()
                         -> AArch64 kernel head.S -> aarch64_main()
```

The loader performs the architecture-specific early work: it loads the kernel
ELF at its fixed physical addresses, captures the final firmware state, exits
boot services, and supplies a physical `boot_context *` in `x0`.

This milestone deliberately keeps the current AArch64 phase-1 kernel alive.
The subsequent convergence slice will use this handoff to enter the shared
`kernel_main(const struct boot_context *)` path.

## Scope and non-goals

Included:

- An ARM64 PE/COFF UEFI application named `BOOTAA64.EFI`.
- ESP creation and QEMU `virt`/AAVMF launch support.
- Fixed-address ELF loading and a no-allocation `ExitBootServices()` handoff.
- Passing firmware data and the boot CPU MPIDR to the existing AArch64 entry.
- Build and QEMU smoke coverage for the UEFI path.

Deferred:

- Relocatable or PIE kernel loading.
- Replacing AArch64 phase-1 subsystem initialization with shared
  `kernel_main`.
- Normalizing the UEFI memory map into the kernel's allocator format.
- AArch64 user mode, SMP bring-up, interrupts, drivers, and storage mounting.

## Build and firmware contract

The project will use the local `thirdpart/posix-uefi` runtime, which has an
`ARCH=aarch64` ARM64 PE/COFF build path.  The OS01 wrapper must ensure that
objects from x86_64 and AArch64 are never shared: the upstream runtime's broad
object wildcard makes a stale object of the other architecture a link error.
Architecture-specific output/cleaning is therefore part of the integration,
not an operator convention.

The generated application is copied into the ESP at the UEFI removable-media
fallback path:

```text
/EFI/BOOT/BOOTAA64.EFI
```

`make run-aarch64-uefi` (final target name may follow the existing Makefile
naming style) will use a writable copy of the host AAVMF firmware, normally
`/usr/share/edk2/aarch64/QEMU_EFI.fd`, rather than writing that system file.
The baseline QEMU topology is:

```text
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 4 -m 512M \
  -drive if=pflash,format=raw,file=<writable-QEMU_EFI.fd> \
  -drive if=none,file=<aarch64-esp.img>,format=raw,id=disk \
  -device virtio-blk-device,drive=disk -serial stdio -display none
```

The firmware boot disk is a standard ESP image; the loader must not rely on
QEMU's direct-boot `x0` DTB convention.

## Fixed physical layout

The existing kernel linker script is an `ET_EXEC` image with entry physical
address `0x40080000`.  The loader retains this layout for the first UEFI
milestone.

| Region | Physical range | Owner |
|---|---:|---|
| QEMU `virt` RAM base | `0x40000000` | platform |
| kernel image/load window | beginning at `0x40080000` | ELF `PT_LOAD` segments |
| boot handoff area | `0x401e0000..0x40200000` (128 KiB) | loader/kernel ABI |

Before loading each segment, the loader reserves the exact page-aligned
physical interval with UEFI `AllocatePages(AllocateAddress, ...)`.  It copies
file bytes to `p_paddr` and zeroes `p_memsz - p_filesz`; no segment is loaded
at its virtual address.  It reserves the handoff area separately before the
final memory-map transaction.  Segment ranges must neither overlap each other
nor overlap the handoff area.

The loader accepts only a bounded, little-endian, 64-bit ELF with:

- ELF magic and a valid program-header table within the loaded file;
- `ET_EXEC` and `EM_AARCH64`;
- `PT_LOAD` records where `p_filesz <= p_memsz` and file/physical ranges do
  not overflow; and
- an entry point in the loaded low physical kernel interval (expected current
  entry: `0x40080000`).

Malformed input, fixed-address allocation failures, or an undersized handoff
area are bootloader errors: print a diagnostic and return to UEFI without
calling `ExitBootServices()`.

## Handoff ABI

At `0x401e0000`, the loader writes a `struct boot_context` from
`kernel/include/kernel/bootinfo.h`; it is deliberately fixed-width and valid
across the UEFI LLP64 and kernel LP64 boundary.  The bootloader enters the
kernel with:

```text
x0 = 0x401e0000        // physical pointer to const struct boot_context
PC = ELF e_entry       // current value 0x40080000
```

`head.S` preserves the physical pointer while enabling the existing mappings,
then passes it to `aarch64_main(const struct boot_context *)`.  The phase-1
kernel continues to use `firmware.dtb` when available and `boot_cpu_id` for
diagnostics; it does not yet consume the memory map.

The context records:

- framebuffer data from GOP when GOP is present, otherwise no framebuffer
  flag;
- the boot CPU's `MPIDR_EL1` value;
- an optional FDT address found through the UEFI configuration tables.  If no
  FDT table is exposed, `firmware.dtb` is zero and the existing QEMU synthetic
  fallback remains valid; and
- the final UEFI memory-map byte sequence, stored after the context inside the
  reserved handoff area, including its descriptor stride and map format.

The first implementation will add an explicit UEFI raw-map format identifier
to `bootinfo.h`, instead of labelling UEFI descriptors as the existing generic
format.  A later core-init slice is responsible for turning that raw map into
the architecture-neutral allocator representation.

## ExitBootServices transaction

The loader must reserve all kernel and handoff pages before collecting the
final map.  It obtains the map size first, validates that a final map plus
required slack fits in the 128 KiB handoff area, then performs the final
transaction as follows:

1. Call `GetMemoryMap()` into the pre-reserved handoff buffer.
2. Populate the adjacent `boot_context` using the returned descriptor size,
   descriptor version, map size, and map key.
3. Call `ExitBootServices(image_handle, map_key)` without any allocation,
   protocol lookup, file I/O, logging, or other boot-service call that can
   alter the map between steps 1 and 3.
4. If `ExitBootServices()` reports an invalid map key, retry from step 1 using
   the same preallocated buffer.  Any other failure is reported while boot
   services are still available and terminates the boot attempt.

After success, neither the loader nor the current phase-1 kernel may access
boot services.  The handoff memory remains reserved and mapped by the initial
kernel page tables.

## Kernel and loader boundaries

The UEFI-specific code owns PE/COFF entry conventions, filesystem access, ELF
validation/loading, GOP/config-table discovery, the UEFI map, and
`ExitBootServices()`.  The kernel owns page-table activation and all later
initialization.  No UEFI headers or UEFI ABI types cross into the kernel.

Existing x86_64 `BOOTX64.EFI` behavior remains unchanged.  Shared boot-context
definitions may be extended only with fixed-width fields, preserving the ABI
rules already enforced by the bootinfo host test.

## Verification and acceptance criteria

Implementation is accepted when all of the following are evidenced:

1. `file` identifies the new loader as an ARM64 EFI application, and the ESP
   contains `/EFI/BOOT/BOOTAA64.EFI`.
2. Host tests cover the new boot-context/map metadata and the pure ELF/range
   validation helpers where practical; existing x86 bootinfo and PMM tests
   still pass.
3. `make -C kernel ARCH=aarch64` remains successful from a clean build.
4. A headless AAVMF QEMU boot emits a loader handoff marker followed by the
   existing AArch64 phase-1 banner and its current spinlock/timer smoke
   output.
5. Invalid ELF metadata and inadequate fixed-memory capacity fail in the
   loader before `ExitBootServices()`, with a serial diagnostic rather than a
   jump to the kernel.
6. The established x86_64 UEFI build path remains buildable and its artifact
   name/location are unchanged.

## Follow-up slices

Once this path is stable, the next design/implementation slice will normalize
the stored UEFI map, select the generic PMM/VMM bootstrap, and make both x86_64
and AArch64 enter `kernel_main(const struct boot_context *)`.  That work is
intentionally separate so a UEFI loading failure and a generic-kernel
initialization failure have distinct, diagnosable boundaries.
