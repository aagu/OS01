/* user/gfx_client_policy.c — see gfx_client_policy.h. */
#include "gfx_client_policy.h"

#include <errno.h>

int gfx_client_present_policy(int err)
{
    if (err == 0) return 0;        /* present succeeded */
    if (err == EAGAIN) return 1;   /* transient drain timeout: retry */
    /* ESTALE (stale view after a mode switch), EIO (device failure) and
     * any other unexpected errno are permanent: release and exit. */
    return -1;
}
