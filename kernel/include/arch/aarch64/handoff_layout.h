/* kernel/include/kernel/arch/aarch64/handoff_layout.h
 *
 * Shared physical-address contract between the AArch64 UEFI bootloader
 * (boot/uefi/arch/aarch64) and the kernel. The loader builds the
 * `boot_context` v2 record inside the handoff window and copies the
 * firmware FDT and raw UEFI memory map after it; the kernel reads
 * those bytes back. The trampoline page sits just below the end of
 * the window and carries the `aarch64_handoff_stub` the BSP jumps
 * through to drop to EL1.
 *
 * Dependency-free: only <stdint.h>. The UEFI loader does not have
 * `-Ikernel/include` on its compile line, so consumers in
 * boot/uefi/arch/aarch64 pull this header via a relative path.
 */
#ifndef OS01_AARCH64_HANDOFF_LAYOUT_H
#define OS01_AARCH64_HANDOFF_LAYOUT_H

#include <stdint.h>

#define AARCH64_HANDOFF_BASE       UINT64_C(0x401e0000)
#define AARCH64_HANDOFF_END        UINT64_C(0x40200000)
#define AARCH64_TRAMPOLINE_BASE    UINT64_C(0x401ff000)

#endif /* OS01_AARCH64_HANDOFF_LAYOUT_H */
