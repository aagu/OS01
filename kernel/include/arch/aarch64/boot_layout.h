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

/* The M3.5 shootdown probe's scratch PA (spec §7.3).  QEMU virt device-gap
 * layout, same platform D above describes:
 *   [0x08000000, 0x0a000000)  boot device window (D)
 *   [0x0a000000, 0x0a200000)  virtio-mmio window (one 2 MiB block)
 *   [0x10000000, 0x40000000)  PCIe MMIO32 bus window (2 MiB pre-map)
 *   [0x4010000000, 0x4020000000) PCIe ECAM window
 * The 2 MiB block right above the virtio-mmio window is the only free run
 * inside the device gap; the probe maps its two scratch pages there and
 * REQUIRES that block to stay unmapped (map_4k_new cannot split an
 * occupied 2 MiB block).  Keep it 2 MiB-aligned. */
#define AARCH64_SHOOTDOWN_PROBE_SCRATCH_PA 0x0a200000
#ifndef AARCH64_BOOT_MAP_END
#if AARCH64_BOOT_BLOCK_BASE >= 0x80000000
#define AARCH64_BOOT_MAP_END      0xC0000000
#else
#define AARCH64_BOOT_MAP_END      0x80000000
#endif
#endif
#define AARCH64_BOOT_MAP_L2_COUNT (AARCH64_BOOT_MAP_END >> 30)
#define AARCH64_BOOT_PT_COUNT     (2 + AARCH64_BOOT_MAP_L2_COUNT + 2)
#ifndef AARCH64_BOOT_HIGH_END
/* How far above the identity map the boot tables still reach: the one
 * 512 GiB span their single L0 entry covers. A Device window or a console
 * block placed there gets its own L2 table (head.S, early_mmu.c). */
#define AARCH64_BOOT_HIGH_END     0x8000000000
#endif

#define AARCH64_BOOT_BLOCK_END    (AARCH64_BOOT_BLOCK_BASE + AARCH64_BOOT_BLOCK_SIZE)
#define AARCH64_KERNEL_ENTRY      (AARCH64_BOOT_BLOCK_BASE + 0x80000)
#define AARCH64_TRAMPOLINE_BASE   (AARCH64_BOOT_BLOCK_END - 0x1000)

#endif /* OS01_AARCH64_BOOT_LAYOUT_H */
