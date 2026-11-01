/* user/common/gfx_client_policy.c — see gfx_client_policy.h. */
#include "gfx_client_policy.h"

#include <errno.h>

gfx_client_verdict_t gfx_client_classify(int err)
{
    if (err == 0) return GFX_CLIENT_OK;                   /* present succeeded */
    if (err == EAGAIN) return GFX_CLIENT_RETRY;            /* transient drain timeout: retry */
    if (err == EACCES) return GFX_CLIENT_LOST_OWNERSHIP;   /* lost the display foreground */
    if (err == ESTALE) return GFX_CLIENT_DISPLAY_CHANGED;  /* stale view after a mode switch */
    /* EIO (device failure) and any other unexpected errno are fatal. */
    return GFX_CLIENT_FATAL;
}

const char *gfx_client_verdict_reason(gfx_client_verdict_t verdict)
{
    switch (verdict) {
    case GFX_CLIENT_OK:              return "ok";
    case GFX_CLIENT_RETRY:           return "display busy, retrying";
    case GFX_CLIENT_LOST_OWNERSHIP:  return "lost display foreground ownership";
    case GFX_CLIENT_DISPLAY_CHANGED: return "display mode changed, restart the app";
    case GFX_CLIENT_FATAL:           return "display device failure";
    }
    return "display device failure";   /* unknown verdict: the safe wording */
}

int gfx_client_present_policy(int err)
{
    switch (gfx_client_classify(err)) {
    case GFX_CLIENT_OK:    return 0;   /* present succeeded */
    case GFX_CLIENT_RETRY: return 1;   /* transient: keep the view and retry */
    default:
        /* EACCES (lost foreground ownership/not display owner), ESTALE (stale
         * view after a mode switch), EIO (device failure) and any other
         * unexpected errno are permanent: release resources and exit. */
        return -1;
    }
}
