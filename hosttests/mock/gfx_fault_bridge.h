/* hosttests/mock/gfx_fault_bridge.h — host SIGSEGV bridge for
 * /dev/gfx0 present path mid-fault testing.
 *
 * Background
 * ----------
 * gfx_ioctl_present (and the new gfx_ioctl_present_rect) copies a
 * per-row chunk from a user pointer via an INLINE memcpy guarded by
 * a single setjmp/longjmp in current->fault_jmp.  The host harness
 * cannot observe faults by simply passing a bad pointer: the kernel's
 * fault_jmp path is invisible from the user side.
 *
 * This bridge lets a host test simulate a real SIGSEGV inside the
 * inline memcpy.  It uses mmap(PROT_NONE) to make a chosen region
 * fault when touched, and a SIGSEGV handler that longjmps back into
 * the production code's fault_jmp (read through a small
 * get_jump callback the test installs).
 *
 * Constraints (spec §5 + plan Task 1)
 * ----------------------------------
 * 1. NO <signal.h> or sigaction type leaks into this header — the
 *    bridge .c is the ONLY place that talks to glibc's signal ABI.
 *    The header declares two plain functions, so the test TU never
 *    sees the OS01 sigaction layout.
 * 2. The bridge .c is compiled with HOST_CC/HOST_CFLAGS only — no
 *    GFX_HOST_CFLAGS, no LIBC_INC, no force-include, no kernel/user/
 *    libc include path.  It pulls in only host system headers.
 * 3. The bridge is linked ONLY into test_gfx_device.elf — no other
 *    host test gets the host SIGSEGV handler installed by accident.
 *
 * Usage
 * -----
 *   // 1. test TU installs a get_jump callback that returns
 *   //    current->fault_jmp (or NULL when not armed).
 *   // 2. test mmap()s a region with PROT_NONE on the user buffer's
 *   //    "later rows" and tells the bridge about it via
 *   //    gfx_host_fault_install(&get_jump, protected_start, length).
 *   // 3. test issues GFX_PRESENT; the inline memcpy trips on the
 *   //    protected page; the SIGSEGV handler longjmps to the
 *   //    production code's fault_jmp slot.
 *   // 4. test must call gfx_host_fault_restore() on every return
 *   //    path (including ASSERT-failure exits) and munmap() the
 *   //    protected region before re-using it.
 */

#ifndef OS01_GFX_FAULT_BRIDGE_H
#define OS01_GFX_FAULT_BRIDGE_H

#include <stddef.h>      /* size_t */

/* Type-erased pointer to the test's get_jump thunk.  The bridge
 * dereferences this on every SIGSEGV: returning non-NULL with a
 * matching si_addr triggers the longjmp; returning NULL or a
 * mismatched si_addr passes the signal to glibc's default handler. */
typedef void **(*gfx_host_fault_get_jump_t)(void *user_ctx);

/* Install a SIGSEGV handler that longjmps into the slot returned by
 * `get_jump` whenever the faulting address falls in
 * [protected_start, protected_start + length).  Saves the previous
 * SIGSEGV handler + signal mask so gfx_host_fault_restore can put
 * them back.  Returns 0 on success, non-zero (and aborts the test
 * via a host-side abort) on install failure — a bridge install
 * failure is a real test-harness problem, never a test outcome. */
int gfx_host_fault_install(gfx_host_fault_get_jump_t get_jump,
                           void *user_ctx,
                           void *protected_start,
                           size_t length);

/* Restore the SIGSEGV handler + signal mask captured by the matching
 * gfx_host_fault_install().  Idempotent: a second call after the
 * first is a no-op (so test cleanup is safe to call twice). */
void gfx_host_fault_restore(void);

#endif /* OS01_GFX_FAULT_BRIDGE_H */