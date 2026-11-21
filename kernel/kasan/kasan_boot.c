// kernel/kasan/kasan_boot.c — KASan boot entry: low-VA handoff isolation
//
// head.S jumps here instead of kernel_main when OS01_KASAN is defined.
//
// The UEFI handoff struct lives in the LOW identity map (PA < 32 MiB, VA
// below 0x0000800000000000).  The kernel-address shadow translation
// shadow(VA) = (VA >> 3) + 0xdffffc0000000000 maps ANY low-half address to
// a NON-canonical shadow VA (the constant only canonicalizes the kernel
// half), so an instrumented dereference of bootctx would take a #GP before
// the first check even completes.  The linear mapping mathematically
// cannot cover both halves with one constant — the Linux kernel avoids the
// problem by never touching low VAs from instrumented C at all.
//
// This TU is compiled WITHOUT instrumentation (object-specific rule in
// kernel/Makefile): the struct copy below is a raw load/store pair, then
// instrumented code only ever sees the kernel-image copy.  bootctx must
// sit inside the boot identity window (PA < 32 MiB) — the same constraint
// the uninstrumented kernel_main boot path already has, since the early
// page tables only map that window.

#include <kasan/kasan.h>

#if defined(OS01_KASAN) || defined(OS01_UBSAN)

#include <core/bootinfo.h>
#include <arch/cpu.h>
#include <percpu/percpu.h>

// kernel_main is instrumented; declared here to tail-call with the copy.
extern int kernel_main(const struct boot_context *bootctx);

static struct boot_context kasan_boot_ctx_copy;

void kasan_boot_entry(const struct boot_context *bootctx)
{
    // Raw copy of the 200-byte handoff struct from the low identity map
    // into the kernel image.  No shadow checks happen in this TU.
    kasan_boot_ctx_copy = *bootctx;

    // Pre-seed the BSP GS base with the (static, zero-initialized)
    // per-CPU array before ANY instrumented C runs.  Instrumented
    // per-CPU accesses shadow-check the GS-derived address: with GS=0
    // (the pre-percpu_init state) this_cpu() reads %gs:0 = low VA,
    // whose kernel-address shadow is non-canonical → #GP (observed in
    // tlb_shootdown→cpu_id during vmm_init, boot stage 2).  The normal
    // kernel survives the same read through the identity map; the
    // instrumented kernel must see a canonical high address instead.
    // Zero-initialized self=0 would make cpu_id() check shadow(0x8c)
    // (the loaded self pointer + offsetof cpu_id) — still low → #GP.
    // Point self at itself: this_cpu() then yields the canonical high
    // percpu_data[0] address, cpu_id reads the zeroed cpu_id = 0 — the
    // correct BSP id — until arch_boot_percpu() re-programs GS with the
    // real BSP base (its own late verify expects self == &percpu_data[0],
    // so the pre-seed is consistent with the boot contract).
    percpu_data[0].self = (uint64_t)&percpu_data[0];
    arch_set_percpu_base(&percpu_data[0]);

    kernel_main(&kasan_boot_ctx_copy);
    for (;;)
        ;
}

#endif /* OS01_KASAN || OS01_UBSAN */
