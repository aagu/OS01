/* kernel/include/arch/aarch64/handoff_layout.h
 *
 * Shared physical-address contract between the AArch64 UEFI bootloader
 * (boot/uefi/arch/aarch64) and the kernel. The loader dynamically
 * allocates `boot_context` v3, the firmware FDT copy, and the raw UEFI
 * memory map buffer below AARCH64_BOOT_MAP_END (disjoint from the boot
 * block) and passes their physical addresses via x0 (`boot_context *`),
 * `ctx->firmware.dtb`, and `ctx->memory.entries`. The trampoline page
 * sits just below the end of the boot block and carries the
 * `aarch64_handoff_stub` the BSP jumps through to drop to EL1.
 *
 * The handoff window constants are owned by boot_layout.h as bare
 * build-time constants (no C types/suffixes) shared with the
 * preprocessed linker script; this header stays the C consumers'
 * interface. Values fit in 32 bits, so bare-constant arithmetic in
 * uint64_t contexts promotes correctly. The UEFI loader has no
 * `-Ikernel/include`, so quoted-include resolution from this header's
 * own directory is what keeps the loader build working.
 */
#ifndef OS01_AARCH64_HANDOFF_LAYOUT_H
#define OS01_AARCH64_HANDOFF_LAYOUT_H

#include "boot_layout.h"

#endif /* OS01_AARCH64_HANDOFF_LAYOUT_H */
