/* hosttests/include/gfx_client_errno.h
 *
 * errno ABI shim for the libgfx client hosttest (Task 3, 2D graphics
 * API plan).  Force-included into the libgfx production TU and the
 * test TU by hosttests/Makefile's GFX_CLIENT_HOST_CFLAGS; pairs with
 * `-D'errno=(*__errno_location())'` set in the same CFLAGS so the
 * compiled libgfx.o matches glibc's TLS-errno contract.
 *
 * Why this exists
 * ---------------
 * libgfx's include path puts `kernel/include` first so that
 * `<uapi/gfx.h>` (kernel ABI header) resolves.  That directory also
 * ships a freestanding `errno.h` that defines only the EPERM/ENOENT/
 * ... constants — it never declares `errno` as a symbol, because in
 * the kernel the syscall ABI threads the error code through a
 * register instead of an lvalue.
 *
 * The host build, however, links libgfx.o against host glibc's
 * libc.so, which provides a TLS `errno` (glibc defines errno as
 * `(*__errno_location())` and exports the function).  Without this
 * shim, libgfx.o references `errno` as a plain int lvalue while
 * libc.so defines it in section `.tbss` — the linker rejects the
 * mismatch with:
 *
 *   errno: TLS definition in /usr/lib/libc.so.6 section .tbss
 *   mismatches non-TLS reference in libgfx_gfx.o
 *
 * Mechanism
 * ---------
 * `-D'errno=(*__errno_location())'` (set on the command line by
 * hosttests/Makefile) is processed by the preprocessor BEFORE any
 * `#include`.  The macro survives the kernel/include/errno.h pass
 * (that header defines only constants, never `#undef errno` and
 * never `#define errno`).  At every reference, `errno` expands to
 * `(*__errno_location())` — a function call the linker resolves to
 * glibc's exported symbol.  This declaration of __errno_location()
 * gives the compiler a prototype so the call type-checks and the
 * library can be linked without implicit-declaration warnings.
 *
 * Scope
 * -----
 * This shim is ONLY included by hosttests/Makefile for the libgfx
 * client test.  Other hosttest suites that don't need libgfx keep
 * their existing include paths unchanged — there is no risk of
 * leakage, because `-include` is per-recipe.
 */
#ifndef _GFX_CLIENT_ERRNO_H
#define _GFX_CLIENT_ERRNO_H

/* Prototype for glibc's TLS errno accessor.  Marked __const__ so the
 * compiler can fold repeated calls to a single load; glibc's own
 * declaration in <errno.h> uses the same attribute. */
#ifdef __cplusplus
extern "C" {
#endif
extern int *__errno_location(void) __attribute__((__const__));
#ifdef __cplusplus
}
#endif

#endif /* _GFX_CLIENT_ERRNO_H */
