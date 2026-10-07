/* user/gfx_client_policy.h — shared stale-view policy for long-running
 * libgfx clients (desktop, test_lvgl, tetris).
 *
 * Spec (docs/superpowers/specs/2026-10-06-qemu-resolution-switcher-design.md)
 * §3.3: after a layout-changing mode switch an existing gfx view becomes
 * stale — its next gfx_present fails with ESTALE and the view must be
 * closed and reopened to recover.  A transient drain timeout (EAGAIN)
 * leaves the view valid and must simply be retried without a busy loop.
 * A permanent device failure is EIO.
 *
 * This helper is the single, shared classification of a failed
 * gfx_present errno.  This TU is NOT a standalone program: user/Makefile
 * excludes it from C_SOURCES and each client links it explicitly (the
 * same pattern as terminal_display.c). */
#ifndef _GFX_CLIENT_POLICY_H
#define _GFX_CLIENT_POLICY_H

/* Minimum interval between retries after a transient present failure.
 * Callers own the deadline (now + GFX_CLIENT_RETRY_MS). */
#define GFX_CLIENT_RETRY_MS 250u

/* Classify a gfx_present result:
 *   0   success (err == 0)
 *   1   transient (err == EAGAIN): keep the view, retry after at least
 *       GFX_CLIENT_RETRY_MS — do NOT reopen the view, do NOT exit
 *  -1   permanent (ESTALE: the view is stale after a mode switch; EIO:
 *       device failure; or any other unexpected errno): release the
 *       graphics resources and exit non-zero.
 *
 * Pure: it never issues a syscall and never touches errno, so the
 * caller's failing errno survives for the diagnostic message. */
int gfx_client_present_policy(int err);

#endif /* _GFX_CLIENT_POLICY_H */
