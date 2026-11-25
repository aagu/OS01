#ifndef OS01_UEFI_AARCH64_LOADER_H
#define OS01_UEFI_AARCH64_LOADER_H

#include <stdint.h>

/* boot/uefi has no -Ikernel/include, so include the shared handoff
 * layout via a relative path that resolves from this header's location.
 * The layout constants (AARCH64_KERNEL_ENTRY, AARCH64_HANDOFF_*,
 * AARCH64_TRAMPOLINE_BASE, AARCH64_PAGE_OFFSET) are bare build-time
 * constants: the profile forwards -DAARCH64_BOOT_BLOCK_BASE to this
 * compile, and boot_layout.h derives everything else — one build loads
 * exactly the kernel that was linked for the same value. */
#include "../../../../kernel/include/arch/aarch64/handoff_layout.h"

#define AARCH64_PAGE_SIZE          UINT64_C(0x1000)

/* The trampoline page must sit one page below the boot block's end.
 * A drift between the shared header's literal and this derived invariant
 * is a layout bug that bricks the BSP jump — lock them at compile time. */
_Static_assert(AARCH64_TRAMPOLINE_BASE ==
               (AARCH64_BOOT_BLOCK_END - AARCH64_PAGE_SIZE),
               "AARCH64_TRAMPOLINE_BASE drifted from boot block end - page size");

#define AARCH64_EI_MAG0            0
#define AARCH64_EI_MAG1            1
#define AARCH64_EI_MAG2            2
#define AARCH64_EI_MAG3            3
#define AARCH64_EI_CLASS           4
#define AARCH64_EI_DATA            5
#define AARCH64_EI_VERSION         6

#define AARCH64_ELFMAG0            0x7f
#define AARCH64_ELFMAG1            'E'
#define AARCH64_ELFMAG2            'L'
#define AARCH64_ELFMAG3            'F'
#define AARCH64_ELFCLASS64         2
#define AARCH64_ELFDATA2LSB        1
#define AARCH64_EV_CURRENT         1
#define AARCH64_ET_EXEC            2
#define AARCH64_EM_AARCH64         183
#define AARCH64_PT_LOAD            1
#define AARCH64_PF_X               1

enum aarch64_elf_error {
    AARCH64_ELF_ERR_HEADER = -1,
    AARCH64_ELF_ERR_OVERFLOW = -2,
    AARCH64_ELF_ERR_ENTRY = -3,
    AARCH64_ELF_ERR_RANGE = -4,
    /* e_entry exists but does not equal the configured boot block
     * entry — the paired-kernel mismatch case gets its own category so
     * the loader can print a dedicated diagnostic. */
    AARCH64_ELF_ERR_ENTRY_MISMATCH = -5,
};

/* ELF64 wire-format records used only by the AArch64 UEFI loader. */
struct aarch64_elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct aarch64_elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

_Static_assert(sizeof(struct aarch64_elf64_ehdr) == 64,
               "ELF64 header wire size");
_Static_assert(sizeof(struct aarch64_elf64_phdr) == 56,
               "ELF64 program-header wire size");

int aarch64_elf_validate(const void *image, uint64_t image_size,
                         uint64_t *entry_out);

/* Early-MMIO handoff values derived from the firmware FDT (roadmap P2
 * hardcode item: the boot Device window must come from the platform,
 * not from compile-time QEMU constants). device_base/device_blocks
 * describe the contiguous 2 MiB-block window covering every matched
 * device register range; console_base is the early UART PA (0 when no
 * PL011 was found — kernel falls back). */
struct aarch64_early_mmio_handoff {
    uint64_t console_base;
    uint64_t device_base;
    uint32_t device_blocks;
    uint32_t reserved;
};

/* Walks a device-tree blob and derives the early-MMIO handoff. Returns
 * 1 and fills *out when at least one known early device (PL011 UART or
 * a GICv2 interrupt controller) was found and the derived window is
 * sane (1..1024 blocks). Returns 0 on any malformed blob, unmatched
 * tree or unusable window — the caller then leaves the boot_context
 * fields unset so the kernel falls back to its compile-time defaults.
 *
 * Devices are matched on any bus; addresses are translated through each
 * enclosing bus's ranges property, and nodes with status != okay are
 * skipped (a disabled PL011 still stands in as the console when no
 * enabled one exists). */
int aarch64_fdt_early_mmio(const void *fdt, uint32_t fdt_size,
                           struct aarch64_early_mmio_handoff *out);

static inline int aarch64_handoff_el_supported(uint64_t current_el)
{
    uint64_t el = (current_el >> 2) & UINT64_C(3);

    return el == UINT64_C(1) || el == UINT64_C(2);
}
/* Page intervals are allocation units.  Valid byte-disjoint PT_LOAD records
 * may share one and are coalesced by the UEFI loading transaction. */
int aarch64_page_interval(uint64_t paddr, uint64_t memsz,
                          uint64_t *start_out, uint64_t *pages_out);

#endif
