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
// Today the kernel uses this byte-fill unconditionally on aarch64
// (because phase 1 has no libc sysroot) and ignores it on x86_64
// (because x86_64 pulls memset from libk.a, which links against
// libc/string/memset.c — a strong global that overrides the weak
// fallback by standard ELF semantics). The link order is therefore
// correct without any Makefile tweaks.
//
// For future arch ports: just provide `void *memset(void *, int,
// size_t);` with a strong definition in arch/<arch>/ — link order
// doesn't matter for ELF weak-vs-strong resolution.

#include <stddef.h>

__attribute__((weak)) void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}