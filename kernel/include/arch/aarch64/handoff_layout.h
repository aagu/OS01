/* kernel/include/arch/aarch64/handoff_layout.h
 *
 * Shared physical-address contract between the AArch64 UEFI bootloader
 * (boot/uefi/arch/aarch64) and the kernel. The loader builds the
 * `boot_context` v3 record inside the handoff window and copies the
 * firmware FDT and raw UEFI memory map after it; the kernel reads
 * those bytes back. The trampoline page sits just below the end of
 * the window and carries the `aarch64_handoff_stub` the BSP jumps
 * through to drop to EL1.
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
