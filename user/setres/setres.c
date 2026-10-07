/* setres.c — query / switch the QEMU framebuffer resolution.
 *
 * Spec (docs/superpowers/specs/2026-10-06-qemu-resolution-switcher-design.md)
 * §7: `-l` lists the device mode table and prints the current mode
 * independently (a boot mode may not be in the table); `-h` prints
 * usage without touching the device; `W H` / `WxH` validate the
 * requested size against the device mode table and then issue a
 * transactional FBIOSET_MODE.  Success is 0, failure is 1, and the
 * error text distinguishes ENODEV / EIO / EBUSY (EBUSY explains that a
 * raw /dev/fb mmap makes switching impossible until restart).
 *
 * The production fb ABI (struct fb_info / fb_state / fb_modes_req and
 * the FBIO* numbers) comes from <uapi/fb.h> — this program never
 * redefines it.
 *
 * setres_run() is the testable core (the host fixture links this TU
 * with -DSETRES_NO_MAIN and drives setres_run directly with captured
 * FILE* streams).  main() is a thin wrapper, compiled only for the
 * real program. */
#include "setres_parse.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/fb.h>

#define FB_DEVICE_PATH "/dev/fb"

static const char USAGE[] =
    "usage: setres -l | -h | W H | WxH\n"
    "  -l     list supported modes and the current mode\n"
    "  -h     show this help\n"
    "  W H    set the resolution, e.g. setres 1280 720\n"
    "  WxH    set the resolution, e.g. setres 1280x720\n";

/* Print the matching diagnostic for a SET/GET failure.  Distinguishes
 * the ioctl errno contract; EBUSY is the sticky raw-mmap case. */
static void report_ioctl_error(FILE *err, const char *what, int e)
{
    if (e == ENODEV)
        fprintf(err, "setres: %s: no switchable display backend (errno=ENODEV)\n", what);
    else if (e == EIO)
        fprintf(err, "setres: %s: display hardware failure (errno=EIO)\n", what);
    else if (e == EBUSY)
        fprintf(err, "setres: %s: display is locked by a raw framebuffer mmap; "
                     "a restart is required before switching modes (errno=EBUSY)\n", what);
    else if (e == EAGAIN)
        fprintf(err, "setres: %s: display is busy, try again (errno=EAGAIN)\n", what);
    else
        fprintf(err, "setres: %s failed (errno=%d)\n", what, e);
}

static int cmd_list(FILE *out, FILE *err)
{
    int fd = open(FB_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        int e = errno;
        fprintf(err, "setres: cannot open %s (errno=%d)\n", FB_DEVICE_PATH, e);
        return 1;
    }

    /* One GET_MODES with the fixed maximum capacity (FB_MAX_MODES=16). */
    struct fb_modes_req req;
    memset(&req, 0, sizeof(req));
    req.capacity = FB_MAX_MODES;
    if (ioctl(fd, FBIOGET_MODES, &req) < 0) {
        int e = errno;
        close(fd);
        report_ioctl_error(err, "FBIOGET_MODES", e);
        return 1;
    }

    /* The current mode is a separate, independent query: it may be a
     * boot mode that is NOT in the switch table, and must still be
     * shown.  GET_STATE returns info + generation in one snapshot. */
    struct fb_state st;
    memset(&st, 0, sizeof(st));
    if (ioctl(fd, FBIOGET_STATE, &st) < 0) {
        int e = errno;
        close(fd);
        report_ioctl_error(err, "FBIOGET_STATE", e);
        return 1;
    }
    close(fd);

    fprintf(out, "Available modes (capacity %u, %u listed):\n",
            req.capacity, req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        fprintf(out, "  %ux%u bpp=%u\n",
                req.modes[i].width, req.modes[i].height, req.modes[i].bpp);
    }
    fprintf(out, "Current: %ux%u (CURRENT) bpp=%u stride=%u\n",
            st.info.width, st.info.height, st.info.bpp, st.info.stride);
    fprintf(out, "generation: %llu\n", (unsigned long long)st.generation);
    return 0;
}

static int cmd_set(FILE *out, FILE *err, uint32_t w, uint32_t h)
{
    int fd = open(FB_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        int e = errno;
        fprintf(err, "setres: cannot open %s (errno=%d)\n", FB_DEVICE_PATH, e);
        return 1;
    }

    /* Validate the requested size against the device's OWN mode table
     * (not an arbitrary hard-coded limit). */
    struct fb_modes_req modes;
    memset(&modes, 0, sizeof(modes));
    modes.capacity = FB_MAX_MODES;
    if (ioctl(fd, FBIOGET_MODES, &modes) < 0) {
        int e = errno;
        close(fd);
        report_ioctl_error(err, "FBIOGET_MODES", e);
        return 1;
    }
    bool supported = false;
    for (uint32_t i = 0; i < modes.count; i++) {
        if (modes.modes[i].width == w && modes.modes[i].height == h) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        close(fd);
        fprintf(err, "setres: %ux%u is not a supported mode\n", w, h);
        return 1;
    }

    /* bpp=0 lets the kernel keep the device's own depth (32bpp here). */
    struct fb_set_mode_req req;
    req.width = w;
    req.height = h;
    req.bpp = 0;
    if (ioctl(fd, FBIOSET_MODE, &req) < 0) {
        int e = errno;
        close(fd);
        report_ioctl_error(err, "FBIOSET_MODE", e);
        return 1;
    }
    close(fd);

    fprintf(out, "resolution set to %ux%u\n", w, h);
    return 0;
}

int setres_run(int argc, char *const argv[], FILE *out, FILE *err)
{
    struct setres_args args;
    if (setres_parse(argc, argv, &args) != 0) {
        fputs(USAGE, err);
        return 1;
    }

    switch (args.action) {
    case SETRES_HELP:
        fputs(USAGE, out);          /* no device access */
        return 0;
    case SETRES_LIST:
        return cmd_list(out, err);
    case SETRES_SET:
        return cmd_set(out, err, args.width, args.height);
    }
    return 1;
}

#ifndef SETRES_NO_MAIN
int main(int argc, char **argv)
{
    return setres_run(argc, argv, stdout, stderr);
}
#endif
