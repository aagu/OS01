// kernel/compiler_rt/memset.c — kernel-side byte-fill memset fallback.
//
// P2-5: previously this TU lived under kernel/arch/aarch64/memset.c as a
// hard (non-weak) symbol. That worked for the aarch64 phase-1 build (no
// libc sysroot → no alternative memset symbol) but two problems followed:
//
//   1. The byte-fill is fundamentally arch-neutral — aarch64 did not gain
//      anything by owning it. Moving to kernel/compiler_rt/ keeps the
//      byte-fill next to the other freestanding helpers (udivti3,
//      stack_chk_guard).
//
//   2. The hard symbol made it impossible for an arch that *does* have
//      an optimised version (x86_64 with rep stosb, aarch64 with STP
//      zero-fill, etc.) to override it without a #ifdef maze. The weak
//      attribute lets any arch override by simply providing a strong
//      memset in its own arch/<arch>/memset.S or .c — the linker picks
//      the strong one if both are linked.
//
// Why not just link libc/string/memset.c into the kernel?
//   libc/string/memset.c implements the same byte-fill loop. On x86_64
//   the kernel already gets a strong memset from libk.a
//   (libc/string/memset.c compiled with -D__is_libk), and the weak
//   symbol here loses by standard ELF resolution — dead code on that
//   arch. On aarch64 phase 1 the build has no libc sysroot at all
//   (kernel/arch/aarch64/make.config: -nostdlib, no libk.a), so we
//   cannot reach libc/string/memset.c from the kernel side. Keeping
//   the kernel-side byte-fill here means each arch links its own
//   representation without a Makefile override:
//     - aarch64 phase 1: weak fallback (this file) is the only
//       definition → used.
//     - x86_64: strong libk.a memset wins → this TU contributes nothing.
//     - future arch: provide a strong memset in arch/<arch>/; link
//       order does not matter for ELF weak-vs-strong resolution.
//
// When the aarch64 build gains a libc sysroot (issue AAGU-2 next phase),
// this TU can be deleted entirely — the kernel will pick memset from
// the libk.a link the same way x86_64 does today.

#include <stddef.h>

__attribute__((weak)) void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}