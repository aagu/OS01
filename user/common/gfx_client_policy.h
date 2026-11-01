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
 * This helper is the SINGLE source of truth for BOTH halves of the
 * client policy:
 *
 *   - the verdict:  gfx_client_classify() maps a gfx_present errno to
 *     the class of the failure (ok / retry / lost ownership / display
 *     changed / fatal);
 *   - the wording:   gfx_client_verdict_reason() maps a verdict to the
 *     human-readable reason string that every client prints.
 *
 * No client may classify an errno or spell a reason on its own: libgui
 * and the user-space clients all go through this file, so the same
 * failure is reported the same way everywhere.
 *
 * This TU is NOT a standalone program: user/Makefile excludes it from
 * C_SOURCES and each client links it explicitly (the same pattern as
 * terminal_display.c). */
#ifndef _GFX_CLIENT_POLICY_H
#define _GFX_CLIENT_POLICY_H

/* Minimum interval between retries after a transient present failure.
 * Callers own the deadline (now + GFX_CLIENT_RETRY_MS). */
#define GFX_CLIENT_RETRY_MS 250u

/* The verdict classes of a gfx_present result. */
typedef enum {
    GFX_CLIENT_OK              = 0,  /* present succeeded */
    GFX_CLIENT_RETRY           = 1,  /* EAGAIN: transient, keep the view, back off */
    GFX_CLIENT_LOST_OWNERSHIP  = 2,  /* EACCES: lost the display foreground */
    GFX_CLIENT_DISPLAY_CHANGED = 3,  /* ESTALE: the view went stale after a mode switch */
    GFX_CLIENT_FATAL           = -1, /* EIO and every other errno */
} gfx_client_verdict_t;

/* Classify a gfx_present errno:
 *   0   success (err == 0)
 *   1   transient (err == EAGAIN): keep the view, retry after at least
 *       GFX_CLIENT_RETRY_MS — do NOT reopen the view, do NOT exit
 *   2   lost ownership (err == EACCES): the display foreground is gone
 *   3   display changed (err == ESTALE): the view is stale after a mode
 *       switch
 *  -1   fatal (EIO: device failure; or any other unexpected errno)
 *
 * Pure: it never issues a syscall and never touches errno, so the
 * caller's failing errno survives for the diagnostic message. */
gfx_client_verdict_t gfx_client_classify(int err);

/* The human-readable reason of a verdict — the exact wording every
 * client prints.  Never NULL: an unknown verdict reports the fatal
 * reason. */
const char *gfx_client_verdict_reason(gfx_client_verdict_t verdict);

/* Coarse policy of a gfx_present result, for the callers that only need
 * retry-vs-give-up:
 *   0   success (err == 0)
 *   1   transient (err == EAGAIN): keep the view, retry after at least
 *       GFX_CLIENT_RETRY_MS — do NOT reopen the view, do NOT exit
 *  -1   permanent (EACCES: lost foreground ownership/not display owner;
 *       ESTALE: the view is stale after a mode switch; EIO: device failure;
 *       or any other unexpected errno): release the graphics resources
 *       and exit non-zero without busy retrying.
 *
 * Implemented on top of gfx_client_classify(): the verdict is the single
 * decision, this is only its coarse projection. */
int gfx_client_present_policy(int err);

#endif /* _GFX_CLIENT_POLICY_H */
