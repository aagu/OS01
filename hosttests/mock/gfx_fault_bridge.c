/* hosttests/mock/gfx_fault_bridge.c — host SIGSEGV bridge.
 *
 * Compiled with HOST_CC/HOST_CFLAGS ONLY.  No KERNEL_INC, no LIBC_INC,
 * no force-include, no production header chain.  This file talks to
 * glibc's signal ABI; the test TU must never see those types.
 *
 * Why we install our own handler
 * --------------------------------
 * The production gfx_ioctl_present / _present_rect path arms
 * current->fault_jmp around an inline __builtin_memcpy and trusts
 * that a page fault longjmps back.  In the kernel, do_page_fault
 * does the longjmp directly.  In the host harness there is no page
 * fault handler at all — the inline memcpy either succeeds (user
 * buffer mapped readable) or SIGSEGVs the test process.
 *
 * We translate that SIGSEGV into the same longjmp: install an
 * SA_SIGINFO handler that checks si_addr against the protected
 * region; if the test's get_jump returns a non-NULL slot, restore
 * the saved signal mask and longjmp there.  Anything else
 * (mismatched si_addr, NULL slot) falls through to the original
 * handler, so a real SIGSEGV elsewhere still aborts the test.
 *
 * The signal-mask restore is load-bearing: glibc's mmap/stack-touch
 * paths block SIGSEGV during page-in, and if we longjmp while
 * SIGSEGV is blocked we'd deadlock the next fault.  We restore
 * before the longjmp.
 */

#include <signal.h>
#include <stddef.h>
#include <stdlib.h>      /* abort() — install/restore failures are harness bugs */
#include <string.h>

#include "gfx_fault_bridge.h"

struct gfx_fault_bridge_state {
    int                       installed;       /* 1 when the handler is live */
    gfx_host_fault_get_jump_t get_jump;
    void                     *user_ctx;
    void                     *protected_start;
    size_t                    length;

    /* saved across install/restore — bridge RETAINS these on restore
     * so a second gfx_host_fault_install after restore re-uses them. */
    struct sigaction          prev_action;
    sigset_t                  prev_mask;
    int                       prev_mask_valid;
};

/* Singleton: only one install is active at a time, matching the
 * production kernel's "current->fault_jmp is per-task, not per-cpu"
 * model.  Multi-call sites must serialize (the test TU does). */
static struct gfx_fault_bridge_state g_state;

static void bridge_sigsegv(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;
    (void)ucontext;
    if (!g_state.installed) {
        /* Not our install — re-raise to the previous handler. */
        raise(SIGSEGV);
        return;
    }

    void *addr = info ? info->si_addr : NULL;
    char *p = (char *)g_state.protected_start;
    /* Only act on faults in the protected region.  A SIGSEGV
     * elsewhere is a real fault, not a fault injection — pass
     * through to the previous handler.  We do this by *removing*
     * our handler and re-raising the same signal: the kernel then
     * dispatches to whatever handler was installed before us. */
    if (!addr || addr < (void *)p ||
        addr >= (void *)(p + g_state.length)) {
        /* Temporarily restore the previous handler, raise to deliver
         * to it, then re-install ours if we get to come back. */
        struct sigaction prev = g_state.prev_action;
        sigaction(SIGSEGV, &prev, NULL);
        raise(SIGSEGV);
        /* If we somehow return, reinstall ours. */
        struct sigaction new_action;
        memset(&new_action, 0, sizeof(new_action));
        new_action.sa_sigaction = bridge_sigsegv;
        new_action.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&new_action.sa_mask);
        sigaction(SIGSEGV, &new_action, NULL);
        return;
    }

    /* Inside the protected region.  Restore the previous mask so the
     * longjmp recipient does not deadlock on the next fault. */
    if (sigprocmask(SIG_SETMASK, &g_state.prev_mask, NULL) != 0) {
        abort();
    }

    void **slot = g_state.get_jump ? g_state.get_jump(g_state.user_ctx) : NULL;
    if (!slot) {
        abort();
    }

    /* 1 is the only longjmp value the production code's
     * __builtin_setjmp branch discriminates on (see kernel
     * __builtin_setjmp wrappers in copy_*_ft / inline memcpy).
     *
     * We use __builtin_longjmp here — the SAME clang compiler builtin
     * the production TU uses for __builtin_setjmp.  Same compiler,
     * same TU compile options, guaranteed layout match.  This is the
     * difference from the earlier inline-asm attempt: the asm had to
     * hardcode the slot offsets and the captured-RIP shape, but
     * clang's __builtin_longjmp is the canonical pair of the
     * producer's __builtin_setjmp and produces the right code by
     * construction. */
    __builtin_longjmp(slot, 1);
}

int gfx_host_fault_install(gfx_host_fault_get_jump_t get_jump,
                           void *user_ctx,
                           void *protected_start,
                           size_t length)
{
    if (g_state.installed) {
        /* The bridge is single-slot; the test is responsible for
         * restore-then-install ordering.  A double install without a
         * matching restore is a harness bug, not a test outcome. */
        abort();
    }

    g_state.get_jump = get_jump;
    g_state.user_ctx = user_ctx;
    g_state.protected_start = protected_start;
    g_state.length = length;

    /* Capture the current signal mask FIRST so the restore at longjmp
     * time puts it back exactly.  We save the mask across the
     * install/restore pair (the install's sigaction installs a handler
     * but does not block SIGSEGV — the handler must run). */
    if (sigprocmask(SIG_BLOCK, NULL, &g_state.prev_mask) != 0) {
        abort();
    }
    g_state.prev_mask_valid = 1;

    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_sigaction = bridge_sigsegv;
    new_action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&new_action.sa_mask);

    if (sigaction(SIGSEGV, &new_action, &g_state.prev_action) != 0) {
        abort();
    }

    g_state.installed = 1;
    return 0;
}

void gfx_host_fault_restore(void)
{
    if (!g_state.installed) return;

    struct sigaction discard;
    if (sigaction(SIGSEGV, &g_state.prev_action, &discard) != 0) {
        abort();
    }

    if (g_state.prev_mask_valid) {
        /* Best-effort restore of the host's pre-install mask.  The
         * test asserts on the post-restore mask in its own TU. */
        (void)sigprocmask(SIG_SETMASK, &g_state.prev_mask, NULL);
    }

    g_state.installed = 0;
    g_state.get_jump = NULL;
    g_state.user_ctx = NULL;
    g_state.protected_start = NULL;
    g_state.length = 0;
    g_state.prev_mask_valid = 0;
    memset(&g_state.prev_action, 0, sizeof(g_state.prev_action));
}