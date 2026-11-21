// kernel/include/kasan/kasan.h — KASan runtime ABI (clang kernel-address)
//
// Geometry is dictated by the compiler: with -fsanitize=kernel-address clang
// emits inline shadow checks of the form
//     shadow = (addr >> 3) + KASAN_SHADOW_OFFSET
// where the offset constant below matches clang 23's x86_64 kernel mapping
// (same value the Linux kernel uses).  Changing the constant here WITHOUT a
// matching -mllvm -asan-mapping-offset breaks every check.
//
// ⚠ Shadow memory must only be touched from uninstrumented code: any
// load/store in an instrumented TU is itself shadow-checked, and the shadow
// of the shadow window is unmapped — an instrumented shadow access would
// page-fault before the check even completes.  All shadow-touching helpers
// therefore live in kernel/kasan/kasan.c / kernel/arch/x86_64/memory/
// kasan_shadow.c, which kernel/Makefile compiles with
// -fno-sanitize=undefined,address (object-specific rules).
//
// Two-phase shadow (x86_64):
//   1. head.S static tables map PML4 slot KASAN_SHADOW_INDEX to a 1 GiB
//      window of read-only 2 MiB pages, all pointing at one reserved zero
//      frame — every check passes until the kernel installs real shadow.
//   2. kasan_shadow_install() (called from x86_64_boot_memory after the
//      direct map is up) allocates real 2 MiB shadow frames from the PMM,
//      builds a fresh PUD chain off-line and swaps it into kernel_map
//      atomically, then flushes TLBs.  Poisoning (slab hooks) only starts
//      after that.
#ifndef _KERNEL_KASAN_H
#define _KERNEL_KASAN_H

#include <stdint.h>

struct boot_context;

#ifdef __cplusplus
extern "C" {
#endif

// Shadow translation constant — MUST match clang's kernel-address mapping.
#define KASAN_SHADOW_OFFSET 0xdffffc0000000000UL

// Kernel direct-map band base (same value as ARCH_PAGE_OFFSET; spelled
// locally so this header works in host tests without arch includes).
#define KASAN_KERNEL_VA_BASE 0xffff800000000000UL

// Shadow VA of the direct-map base = start of the shadow window.
#define KASAN_SHADOW_BASE ((KASAN_KERNEL_VA_BASE >> 3) + KASAN_SHADOW_OFFSET)

// PML4 slot holding the shadow window: KASAN_SHADOW_BASE >> 39 & 511 = 472.
#define KASAN_SHADOW_PML4_INDEX ((KASAN_SHADOW_BASE >> 39) & 511)

// head.S phase-1 static window: 1 GiB of shadow VAs (= 8 GiB of kernel VA
// space) covered by two static page directories.  KASan builds support
// physical spans <= 8 GiB; kasan_shadow_install() refuses past that bound.
#define KASAN_PHASE1_SHADOW_BYTES 0x40000000UL

// Framebuffer bands instrumented code writes through (printk console and
// panic rendering).  The early window is a fixed PA band inside the
// direct-map band; the remapped window is its own PML4 slot whose shadow
// gets a second chain at install time.  16 MiB covers every QEMU VBE mode
// the build supports.
#define KASAN_FB_EARLY_PA       0x3F000000UL   /* VIRT_FRAMEBUFFER_EARLY pa */
#define KASAN_FB_EARLY_BYTES    0x01000000UL
#define KASAN_FB_OFFSET_VA      0xFFFF900000000000UL
#define KASAN_FB_OFFSET_BYTES   0x01000000UL
#define KASAN_FB_PML4_INDEX     476            /* slot of shadow(KASAN_FB_OFFSET_VA) = 0xffffee0000000000 */

// Reserved 2 MiB zero frame (inside the kernel image via head.S .org) that
// every phase-1 shadow PDE points at.
#define KASAN_ZERO_FRAME_PA 0x200000UL

// Shadow byte vocabulary (compiler-rt kasan values).
#define KASAN_SHADOW_VALID        0x00
#define KASAN_SHADOW_PARTIAL_MAX  0x07
#define KASAN_SHADOW_STACK_MID    0xf2
#define KASAN_SHADOW_STACK_RIGHT  0xf3
#define KASAN_SHADOW_STACK        0xf8
#define KASAN_SHADOW_ALLOCA_LEFT  0xca
#define KASAN_SHADOW_HEAP_LEFT    0xfa
#define KASAN_SHADOW_HEAP_FREED   0xfb
#define KASAN_SHADOW_HEAP_RIGHT   0xfc

#ifdef OS01_KASAN

// Slab hooks — called from kernel/memory/slab.c only.  req_size is the
// caller's requested size; cache_size is the owning kmalloc cache's object
// size (kmalloc_cache_size[i].size).  kmalloc: unpoison [ptr, ptr+req),
// redzone [ptr+req, ptr+cache).  kfree: poison the whole object 0xfb.
void kasan_kmalloc(void *ptr, unsigned long req_size, unsigned long cache_size);
void kasan_kfree(void *ptr, unsigned long cache_size);

// Whole-page unpoison when a 2 MiB slab frame returns to the PMM (the
// shadow of reused RAM must read 0 or every later user of the frame
// trips on stale 0xfb).
void kasan_kunpoison_page(void *va);

// Phase-2 real shadow installation (arch/x86_64/memory/kasan_shadow.c).
// bootctx supplies the framebuffer / early-MMIO physical spans instrumented
// code writes through.
void kasan_shadow_install(const struct boot_context *bootctx);

// Arms the slab poison hooks once the real shadow is live (called by
// kasan_shadow_install after the TLB flush).
void kasan_poison_arm(void);

// Runtime mapping hook: called from x86_vmm_map_2m / x86_vmm_map_4k AFTER
// the mapping is installed, so instrumented code that then touches the
// newly-mapped VA (device MMIO, ioremapped BARs) finds shadow present.
// No-op for user-half VAs (their shadow would be non-canonical — only the
// de-instrumented uaccess path touches those).  RAM spans are pre-mapped,
// so the common case finds the shadow page already present.
void kasan_shadow_note_mapping(uintptr_t va, unsigned long size);

// Report path — also used by the sanitizer probe fixture.  Always fatal.
void kasan_report_access(unsigned long addr, unsigned long size, int is_write);

#else /* !OS01_KASAN — byte-identical no-ops for the normal build */

static inline void kasan_kmalloc(void *ptr, unsigned long req_size,
                                 unsigned long cache_size)
{
    (void)ptr; (void)req_size; (void)cache_size;
}
static inline void kasan_kfree(void *ptr, unsigned long cache_size)
{
    (void)ptr; (void)cache_size;
}
static inline void kasan_kunpoison_page(void *va)
{
    (void)va;
}
static inline void kasan_shadow_install(const struct boot_context *bootctx)
{
    (void)bootctx;
}
static inline void kasan_shadow_note_mapping(uintptr_t va, unsigned long size)
{
    (void)va; (void)size;
}

#endif /* OS01_KASAN */

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_KASAN_H */
