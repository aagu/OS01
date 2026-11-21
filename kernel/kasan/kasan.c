// kernel/kasan/kasan.c — KASan runtime: poison helpers + report handlers
//
// Compiled WITHOUT instrumentation (object-specific rule in kernel/Makefile):
// every load/store here touches shadow memory, and an instrumented shadow
// access would page-fault (the shadow of the shadow window is unmapped).
// See kernel/include/kasan/kasan.h for the geometry contract.
//
// Handlers implement the clang kernel-address runtime ABI (probe-verified
// against clang 23.1.5, -O2, -mllvm -asan-globals=0):
//   __asan_report_{load,store}{1,2,4,8,16}{,_noabort}
//   __asan_report_{load,store}_n[_noabort]   (unusual sizes / misaligned)
//   __asan_handle_no_return                  (stack unpoison before noreturn)
//   __asan_alloca_poison / __asan_allocas_unpoison (dynamic allocas)
// Every report is fatal: log_err with the shadow-byte classification, then
// kpanic (the fatal path's exception trace then shows the backtrace).

#include <kasan/kasan.h>

#ifdef OS01_KASAN
// Everything below touches shadow memory and must only exist in
// instrumented builds; outside OS01_KASAN the file compiles to an empty
// TU so a stray compilation can never conflict with the header's
// static-inline no-op hooks.

#include <log/log.h>
#include <core/panic.h>
#include <memory/memory.h>       /* Phy_To_Virt */
#include <arch/x86_64/vmm.h>     /* kernel_map for shadow diagnostics */
#include <stddef.h>
#include <string.h>              /* memset for the set_shadow family */

// ── shadow translation ───────────────────────────────────────

static inline unsigned char *kasan_shadow_byte(unsigned long va)
{
    return (unsigned char *)((va >> 3) + KASAN_SHADOW_OFFSET);
}

// Poison gate: slab hooks stay no-ops until kasan_shadow_install() swaps
// the real writable shadow in.  Before that the shadow window is the
// phase-1 read-only zero frame — poison writes would #PF (and a shared
// writable frame would corrupt every other VA's shadow bytes instead).
// Pre-install allocations therefore stay unpoisoned (shadow reads 0 = all
// valid): no false positives, no lost post-install coverage.
static int kasan_poison_ready;

// Armed by kasan_shadow_install() after the real writable shadow is live.
void kasan_poison_arm(void)
{
    kasan_poison_ready = 1;
}

// ── poison helpers ───────────────────────────────────────────
//
// Granule model: shadow byte n in [1,7] means the first n bytes of the
// 8-byte granule are valid; 0 = fully valid; the poison values (0xfa/0xfb/
// 0xfc/0xf8/...) mean nothing in the granule is valid.  All heap call shapes
// are granule-aligned by construction (slab objects sit at cache_size
// stride from a 2 MiB-aligned slab base), so the helpers below only handle
// whole granules plus one exact partial tail for the requested-size case.

static void kasan_poison_aligned(unsigned long addr, unsigned long size,
                                 unsigned char value)
{
    unsigned long end = addr + size;
    while (addr + 8 <= end) {
        *kasan_shadow_byte(addr) = value;
        addr += 8;
    }
    if (addr < end)
        *kasan_shadow_byte(addr) = value; // callers keep size granule-aligned
}

// Unpoison a payload whose base is granule-aligned; the tail granule keeps
// `size & 7` valid bytes (0 → fully valid).
static void kasan_unpoison_payload(unsigned long addr, unsigned long size)
{
    unsigned long end = addr + size;
    while (addr + 8 <= end) {
        *kasan_shadow_byte(addr) = KASAN_SHADOW_VALID;
        addr += 8;
    }
    if (addr < end)
        *kasan_shadow_byte(addr) = (unsigned char)(end - addr);
}

void kasan_kmalloc(void *ptr, unsigned long req_size, unsigned long cache_size)
{
    unsigned long base = (unsigned long)ptr;

    if (!kasan_poison_ready)
        return;
    if (cache_size == 0 || cache_size < req_size)
        return; // unexpected cache geometry — never poison speculatively

    // Payload valid up to the exact requested size (partial tail granule
    // expresses req_size & 7); the boundary granule byte is exactly the
    // "first (req & 7) bytes valid" state the redzone must preserve.
    kasan_unpoison_payload(base, req_size);

    // Redzone between requested size and cache object size, starting at
    // the first fully-poisonable granule after the payload.
    unsigned long red_start = (base + req_size + 7UL) & ~7UL;
    if (base + cache_size > red_start)
        kasan_poison_aligned(red_start, base + cache_size - red_start,
                             KASAN_SHADOW_HEAP_RIGHT);
}

void kasan_kfree(void *ptr, unsigned long cache_size)
{
    if (!kasan_poison_ready || cache_size == 0)
        return;
    kasan_poison_aligned((unsigned long)ptr, cache_size,
                         KASAN_SHADOW_HEAP_FREED);
}

void kasan_kunpoison_page(void *va)
{
    // A whole 2 MiB frame is granule aligned — straight 0 sweep.
    kasan_poison_aligned((unsigned long)va, 0x200000UL, KASAN_SHADOW_VALID);
}

// ── report path ──────────────────────────────────────────────

static const char *kasan_classify(unsigned char shadow)
{
    switch (shadow) {
    case KASAN_SHADOW_HEAP_LEFT:
        return "heap left redzone (before object)";
    case KASAN_SHADOW_HEAP_FREED:
        return "freed heap object (use-after-free)";
    case KASAN_SHADOW_HEAP_RIGHT:
        return "heap redzone (past requested size)";
    case KASAN_SHADOW_STACK:
    case KASAN_SHADOW_STACK_MID:
    case KASAN_SHADOW_STACK_RIGHT:
    case KASAN_SHADOW_ALLOCA_LEFT:
        return "stack/alloca redzone";
    default:
        if (shadow >= 1 && shadow <= KASAN_SHADOW_PARTIAL_MAX)
            return "partial granule (past valid bytes)";
        return "invalid shadow state";
    }
}

void kasan_report_access(unsigned long addr, unsigned long size, int is_write)
{
    unsigned long shadow_va = (addr >> 3) + KASAN_SHADOW_OFFSET;
    unsigned char shadow = *kasan_shadow_byte(addr);
    log_err("kasan: %s of %lu byte(s) at %#lx: shadow=%#02x (%s)\n",
            is_write ? "WRITE" : "READ", size, addr, shadow,
            kasan_classify(shadow));
    // Diagnostic: which physical frame backs this shadow VA, and the
    // neighbouring shadow bytes — identifies who poisoned the page.
    unsigned long pml4i = (shadow_va >> 39) & 511;
    unsigned long root = kernel_map ? kernel_map[pml4i] : 0;
    if (root & 1) {
        unsigned long *pud =
            (unsigned long *)Phy_To_Virt(root & ~((1UL << 12) - 1));
        unsigned long pud_e = pud[(shadow_va >> 30) & 511];
        if (pud_e & 1) {
            unsigned long *pd =
                (unsigned long *)Phy_To_Virt(pud_e & ~((1UL << 12) - 1));
            unsigned long pd_e = pd[(shadow_va >> 21) & 511];
            log_err("kasan: shadow_va=%#lx shadow frame pa=%#lx\n",
                    shadow_va, pd_e & ~((1UL << 21) - 1));
            unsigned char *sb = kasan_shadow_byte(addr & ~7UL);
            log_err("kasan: shadow bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    sb[-4], sb[-3], sb[-2], sb[-1],
                    sb[0], sb[1], sb[2], sb[3]);
        }
    }
    kpanic("kasan: invalid memory access detected\n");
}

// ── handler ABI ──────────────────────────────────────────────
// The inline checks call the _noabort spelling (kernel mode is recoverable
// by default and -fno-sanitize-recover=kernel-address is driver-rejected);
// the plain spellings are aliases so an abort-style build links unchanged.

#define KASAN_REPORT_FIXED(name, size, is_write)                            \
    void __asan_report_##name(unsigned long addr)                           \
    {                                                                       \
        kasan_report_access(addr, (size), (is_write));                      \
    }

KASAN_REPORT_FIXED(load1_noabort,  1, 0)
KASAN_REPORT_FIXED(load2_noabort,  2, 0)
KASAN_REPORT_FIXED(load4_noabort,  4, 0)
KASAN_REPORT_FIXED(load8_noabort,  8, 0)
KASAN_REPORT_FIXED(load16_noabort, 16, 0)
KASAN_REPORT_FIXED(store1_noabort,  1, 1)
KASAN_REPORT_FIXED(store2_noabort,  2, 1)
KASAN_REPORT_FIXED(store4_noabort,  4, 1)
KASAN_REPORT_FIXED(store8_noabort,  8, 1)
KASAN_REPORT_FIXED(store16_noabort, 16, 1)

KASAN_REPORT_FIXED(load1,  1, 0)
KASAN_REPORT_FIXED(load2,  2, 0)
KASAN_REPORT_FIXED(load4,  4, 0)
KASAN_REPORT_FIXED(load8,  8, 0)
KASAN_REPORT_FIXED(load16, 16, 0)
KASAN_REPORT_FIXED(store1,  1, 1)
KASAN_REPORT_FIXED(store2,  2, 1)
KASAN_REPORT_FIXED(store4,  4, 1)
KASAN_REPORT_FIXED(store8,  8, 1)
KASAN_REPORT_FIXED(store16, 16, 1)

void __asan_report_load_n_noabort(unsigned long addr, unsigned long size)
{
    kasan_report_access(addr, size, 0);
}

void __asan_report_store_n_noabort(unsigned long addr, unsigned long size)
{
    kasan_report_access(addr, size, 1);
}

void __asan_report_load_n(unsigned long addr, unsigned long size)
{
    kasan_report_access(addr, size, 0);
}

void __asan_report_store_n(unsigned long addr, unsigned long size)
{
    kasan_report_access(addr, size, 1);
}

// Emitted before noreturn calls: unpoison the current frame's stack
// redzones.  The kernel does not use fake stacks (detect_stack_use_after_
// return off by default), so the real shadow stays valid — a no-op.
void __asan_handle_no_return(void)
{
}

// Dynamic allocas (VLAs): poison the redzones around [addr, addr+size).
// Only fully-aligned granules are poisoned; the partial granule at the VLA
// end keeps whatever validity it had (fresh shadow = valid) so no valid VLA
// byte can ever trip a report — at the cost of missing checks on its first
// few redzone bytes.
void __asan_alloca_poison(unsigned long addr, unsigned long size)
{
    // Left redzone: 32 bytes before the (32-aligned) VLA.
    if (addr >= 32)
        kasan_poison_aligned(addr - 32, 32, KASAN_SHADOW_ALLOCA_LEFT);
    // Right redzone: from the first granule past the end, 32 bytes.
    unsigned long end = addr + size;
    unsigned long right = (end + 7UL) & ~7UL;
    kasan_poison_aligned(right, 32, KASAN_SHADOW_STACK_RIGHT);
}

void __asan_allocas_unpoison(unsigned long top, unsigned long bottom)
{
    if (bottom > top)
        kasan_unpoison_payload(top, bottom - top);
}

// ── fast constant poisoning ──────────────────────────────────
// For larger stack frames clang emits calls to __asan_set_shadow_<pat>
// instead of inline stores: the arguments are the ALREADY-TRANSLATED
// shadow address and a size in shadow bytes, so the handler is a raw
// memset with the pattern byte (compiler-rt asan_poisoning.cpp).

#define KASAN_SET_SHADOW_FUNC(name, byte)                                   \
    void __asan_set_shadow_##name(unsigned long addr, unsigned long size)   \
    {                                                                       \
        memset((void *)addr, (byte), size);                                 \
    }

KASAN_SET_SHADOW_FUNC(00, 0x00)
KASAN_SET_SHADOW_FUNC(f1, 0xf1)
KASAN_SET_SHADOW_FUNC(f2, 0xf2)
KASAN_SET_SHADOW_FUNC(f3, 0xf3)
KASAN_SET_SHADOW_FUNC(f5, 0xf5)
KASAN_SET_SHADOW_FUNC(f8, 0xf8)
KASAN_SET_SHADOW_FUNC(ca, 0xca)

#endif /* OS01_KASAN */
