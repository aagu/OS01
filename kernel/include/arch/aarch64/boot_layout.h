/* kernel/include/arch/aarch64/boot_layout.h
 *
 * Build-time AArch64 boot block layout (spec
 * docs/superpowers/specs/2026-11-17-aarch64-kernel-lma-design.md §2).
 *
 * One physical boot block B is selected per build via
 * AARCH64_BOOT_BLOCK_BASE (profile-validated, forwarded as -D); every
 * other interval is derived here. All values are BARE preprocessor
 * constants — no C types, no ULL suffixes — so the same macros expand
 * identically in kernel C, preprocessed assembly and the preprocessed
 * linker script (kernel/arch/aarch64/linker.ld →
 * linker.generated.ld). Consumers needing a C integer type must cast
 * explicitly (see handoff_layout.h).
 *
 * All intervals are half-open [start, end). Defaults describe the
 * QEMU virt layout: B = [0x40000000, 0x40200000), M0 covers [0,
 * 0x80000000), the boot device window D = [0x08000000, 0x0a000000).
 * D is the COMPILE-TIME FALLBACK: the UEFI loader derives the real
 * window from the firmware FDT and hands it to the kernel via the
 * boot_context early_mmio fields (see device_window.h).
 * Host tests may override any constant via -D; the #ifndef guards keep
 * that override path effective.
 */
#ifndef OS01_AARCH64_BOOT_LAYOUT_H
#define OS01_AARCH64_BOOT_LAYOUT_H

/* The ONLY configured input; everything below derives from it. */
#ifndef AARCH64_BOOT_BLOCK_BASE
#define AARCH64_BOOT_BLOCK_BASE 0x40000000
#endif

/* Higher-half direct-map base. Defined exactly once for AArch64 (the
 * C facade arch/mmu.h derives ARCH_PAGE_OFFSET from this). */
#ifndef AARCH64_PAGE_OFFSET
#define AARCH64_PAGE_OFFSET 0xffff000000000000
#endif

#define AARCH64_BOOT_BLOCK_SIZE   0x200000
#define AARCH64_BOOT_DEVICE_BASE  0x08000000
#define AARCH64_BOOT_DEVICE_END   0x0a000000
#define AARCH64_BOOT_MAP_END      0x80000000

#define AARCH64_BOOT_BLOCK_END    (AARCH64_BOOT_BLOCK_BASE + AARCH64_BOOT_BLOCK_SIZE)
#define AARCH64_KERNEL_ENTRY      (AARCH64_BOOT_BLOCK_BASE + 0x80000)
#define AARCH64_TRAMPOLINE_BASE   (AARCH64_BOOT_BLOCK_END - 0x1000)

#endif /* OS01_AARCH64_BOOT_LAYOUT_H */
