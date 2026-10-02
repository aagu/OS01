/* user/test_gfx.c — Ring-3 QEMU gfx E2E test (Task 5 of the
 * 2D graphics API plan).
 *
 * Substitutes the Task 3 libgfx link-smoke program.  Runs as
 * ``/bin/test_gfx`` from the BusyBox prompt and exercises every
 * libgfx primitive end-to-end inside QEMU.  The OS01-side runner
 * (``qemutests/run_test.py test_gfx``) just types
 * ``/bin/test_gfx`` and waits for ``[GFX TEST] PASS`` on the
 * serial log; every assertion below is made on the user side so
 * the test is a single self-contained binary.
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §6 — present-based assertions rather than byte-dumping the
 * framebuffer:
 *
 *   1. Full-screen view (0, 0, W, H):
 *        - paint the LEFT half solid red,
 *        - paint the RIGHT half solid green,
 *        - paint the y=x diagonal solid white,
 *        - gfx_present().
 *      Then sample THREE coordinates by re-opening /dev/fb and
 *      reading from its existing mmap (no second mmap; the fb
 *      kernel driver keeps the mapping alive across opens):
 *        - on the diagonal -> WHITE,
 *        - off-diagonal LEFT side -> RED,
 *        - off-diagonal RIGHT side -> GREEN.
 *      This is the "white diagonal covers red+green" check; we do
 *      NOT just count red vs green pixels (which the spec §6
 *      Review Focus #5 explicitly forbids).
 *
 *   2. Central small view (CW, CH):
 *        - paint it solid blue + present.
 *        - BEFORE the small-view present, paint a frame of
 *          sentinel pixels (yellow) around the outside of the
 *          small view in the FULL-SCREEN view's buffer; the small
 *          view's present must NOT touch those outside pixels.
 *        - After present, re-read sentinel pixels and assert they
 *          are still yellow (spec §6 — kernel limits a present to
 *          the view rectangle).
 *
 *   3. Negative cases (no QEMU drawing involved):
 *        - gfx_open(unconfigured_fd,...) returns NULL with errno.
 *        - gfx_open with out-of-bounds rect (x=W+1) returns NULL
 *          with errno=EINVAL.
 *
 *   4. PASS / FAIL marker:
 *      Exactly one of:
 *          [GFX TEST] PASS
 *          [GFX TEST] FAIL: <reason>
 *      on stdout (which becomes the serial line under -serial
 *      stdio in QEMU).
 *
 * The runner types /bin/test_gfx at the shell prompt, so this
 * program MUST NOT emit banner text, prompt, or noise lines
 * before the marker — the runner only reads the serial log and
 * looks for the prefix.  We therefore gate every output line on
 * the PASS/FAIL state; the only stdout lines this program emits
 * are the marker (and any pre-marker diagnostic, gated on FAIL).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <gfx.h>

/* ── Framebuffer metadata (must match kernel definition) ────── */
struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));
#define FBIOSURRENDER  0x00004601

/* ── Colors (0xAARRGGBB, host-endian RGB32) ────────────────────── */
#define COLOR_RED    0x00FF0000u
#define COLOR_GREEN  0x0000FF00u
#define COLOR_BLUE   0x000000FFu
#define COLOR_WHITE  0x00FFFFFFu
#define COLOR_YELLOW 0x00FFFF00u
#define COLOR_BLACK  0x00000000u

/* ── FAIL macro — prints the reason and exits non-zero ─────────
 * Use macro so the marker is exactly one line and the reason is
 * captured at the call site.  We deliberately do NOT free libgfx
 * handles on FAIL — the runner only checks the marker, and the
 * program exits immediately anyway.  Memory leaks inside the
 * kernel VM that just reboots are not a concern. */
#define FAIL(...) do {                                       \
    printf("[GFX TEST] FAIL: ");                             \
    printf(__VA_ARGS__);                                     \
    printf("\n");                                            \
    exit(1);                                                 \
} while (0)

#define PASS() do {                                          \
    printf("[GFX TEST] PASS\n");                             \
    exit(0);                                                 \
} while (0)

/* ── View sizes ───────────────────────────────────────────────
 * Task 8 of the user-heap/ELF-isolation plan: the "full-screen"
 * view is now the actual framebuffer width and height (QEMU
 * stdvga reports 1440×900 — 5,184,000 bytes at 32 bpp).  The
 * per-process heap ceiling is 0x13ff000 (USER_CODE_ADDR +
 * USER_PAGE_SIZE - 0x1000 = 0x5FF000 was the old ceiling before
 * the 16 MiB user VA bump landed; the 16 MiB window now covers
 * the full-screen pixels buffer plus 64 KiB of program headroom).
 *
 * The full-screen view therefore equals the kernel-reported fb
 * dimensions (discovered at runtime from /dev/fb's info struct).
 * The "small central view" stays small — its purpose is to prove
 * that a small present does not stomp the surrounding sentinels,
 * and a tiny rectangle achieves that regardless of the surrounding
 * full-screen view's actual size.  Two view slots open at the
 * same time (full + small), well below the kernel's 16-slot limit.
 */
#define SMALL_W 32u
#define SMALL_H 32u

/* Spec §6 + the Task 8 brief: the QEMU stdvga fb is 1440×900
 * RGB32 = 5,184,000 bytes.  This is the smallest acceptable
 * "full-screen view" allocation for the integration assertion. */
#define MIN_FB_W       1440u
#define MIN_FB_H       900u
#define MIN_FB_BYTES   5184000u   /* 1440 * 900 * 4 */

/* Heap headroom contract (Task 8 brief): after the libgfx pixels
 * buffer is allocated by gfx_open, the program must still have at
 * least 64 KiB of free heap above the current break for its own
 * subsequent allocations (e.g. libc printf buffers, scratch).
 *
 * The check is performed BEFORE gfx_open — the brief asserts that
 * the *pre-allocation* headroom accommodates (framebuffer_bytes
 * + 64 KiB).  That guarantees the calloc inside gfx_open won't
 * collide with the 0x13ff000 heap limit. */
#define HEAP_LIMIT     0x13ff000ul
#define HEAP_HEADROOM  65536u

/* ── Helpers ─────────────────────────────────────────────────── */

/* Open /dev/fb, query its framebuffer info struct, and mmap the
 * pixels.  Used BOTH to verify the kernel-allocated fb region
 * (the secondary mapping the spec §6 wants for read-only
 * verification) AND to read the gfx0-presented frame for the
 * diagonal assertion.  Returns the fd; *out_fb receives the
 * mapping, *out_info the metadata.  Caller closes + munmaps. */
static int fb_open_and_map(uint32_t **out_fb, struct fb_info *out_info)
{
    int fd = open("/dev/fb", O_RDWR);
    if (fd < 0)
        FAIL("cannot open /dev/fb (errno=%d)", errno);
    if (read(fd, out_info, sizeof(*out_info)) !=
        (ssize_t)sizeof(*out_info))
        FAIL("short read on /dev/fb info struct");
    if (out_info->width == 0 || out_info->height == 0 ||
        out_info->stride == 0)
        FAIL("/dev/fb reports zero dimensions (w=%u h=%u stride=%u)",
             out_info->width, out_info->height, out_info->stride);
    size_t bytes = (size_t)out_info->height * (size_t)out_info->stride;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        FAIL("mmap of /dev/fb failed (errno=%d)", errno);
    *out_fb = p;
    return fd;
}

/* Read the pixel at full-screen (x, y) from the fb mmap.  The
 * kernel writes pixels as host-endian uint32; same encoding as
 * libgfx's RGB32. */
static uint32_t fb_read(uint32_t *fb, uint32_t stride,
                        uint32_t x, uint32_t y)
{
    return fb[(size_t)y * (stride / 4) + x];
}

/* ── Test 1: full-screen view — left red, right green, white diagonal */

static void test_fullscreen_view(uint32_t *fb, const struct fb_info *info,
                                 uint32_t fw, uint32_t fh)
{
    /* Task 8: the "full-screen" view is now the actual framebuffer
     * width × height (QEMU stdvga → 1440×900 RGB32 = 5,184,000
     * bytes).  We open at (0, 0) so the view's top-left corner is
     * the framebuffer's top-left corner; every pixel we write
     * goes to a known offset in fb. */
    gfx_handle_t *h = gfx_open(0, 0, fw, fh);
    if (!h)
        FAIL("gfx_open(0,0,%u,%u) returned NULL (errno=%d)",
             fw, fh, errno);

    gfx_info_t gi = gfx_get_info(h);
    if (gi.width != fw || gi.height != fh)
        FAIL("gfx_get_info returned w=%u h=%u (expected %u %u)",
             gi.width, gi.height, fw, fh);

    /* Spec §6 (Task 8 brief): the QEMU 1440×900 RGB32 case
     * allocates a libgfx pixels buffer of at least 5,184,000
     * bytes.  Confirm the kernel-driven calloc inside gfx_open
     * actually returned a buffer of that size — the integration
     * gate for "ELF + heap isolation supports a full-screen
     * framebuffer". */
    uint64_t pixels_bytes = (uint64_t)gi.width *
                            (uint64_t)gi.height * 4ull;
    if (pixels_bytes < MIN_FB_BYTES)
        FAIL("gfx pixels buffer too small: %u×%u = %llu bytes, "
             "need >= %u bytes (1440×900×4)",
             gi.width, gi.height,
             (unsigned long long)pixels_bytes, MIN_FB_BYTES);

    /* Paint the full buffer black first (gfx_present must be
     * idempotent — the kernel copies every row regardless of the
     * previous frame).  gfx_fill_rect over the whole view is the
     * simplest way. */
    gfx_fill_rect(h, 0, 0, gi.width, gi.height, COLOR_BLACK);

    /* LEFT half red — entire left column band from x=0 up to
     * mid-x. */
    uint32_t mid = gi.width / 2;
    gfx_fill_rect(h, 0, 0, mid, gi.height, COLOR_RED);

    /* RIGHT half green — from mid to end of width. */
    gfx_fill_rect(h, (int32_t)mid, 0, gi.width - mid, gi.height,
                  COLOR_GREEN);

    /* White diagonal y = x — only the points ON the diagonal need
     * to be white.  Bresenham is the right tool here (libgfx
     * implements it via gfx_line). */
    int32_t dmax = (int32_t)(gi.height < gi.width ? gi.height : gi.width);
    gfx_line(h, 0, 0, dmax - 1, dmax - 1, COLOR_WHITE);

    /* Push the frame to the kernel. */
    if (gfx_present(h) != 0)
        FAIL("gfx_present returned %d errno=%d", -1, errno);

    /* The /dev/fb mapping is updated by the kernel's per-row
     * fault-tolerant copy.  Give the kernel a brief moment to
     * finish — present() returns after the last row is queued,
     * and the kernel's copy may still be draining. */
    for (volatile int s = 0; s < 500000; s++) { /* spin briefly */ }

    /* Sample three points: one on the diagonal, one off-diagonal
     * in the LEFT half, one off-diagonal in the RIGHT half.
     * The view lives at full-screen (0,0) so these fb offsets
     * match the view-local coordinates exactly. */
    uint32_t left_pt  = fb_read(fb, info->stride, 10u, 11u);
    uint32_t diag_pt  = fb_read(fb, info->stride,
                                (uint32_t)(dmax / 2),
                                (uint32_t)(dmax / 2));
    uint32_t right_pt = fb_read(fb, info->stride,
                                fw - 10u, fh - 11u);

    /* Spec §6 + Review Focus #5: the assertion is "white diagonal
     * covers red+green", NOT "exactly half red / half green".
     * We check the DIAGONAL pixel is white, and that the two
     * sides hold their colours.  A "red/green are 50/50" check
     * would fail here by design. */
    if (diag_pt != COLOR_WHITE)
        FAIL("diagonal pixel (%u,%u) = 0x%08X, expected WHITE 0x%08X",
             (uint32_t)(dmax / 2), (uint32_t)(dmax / 2),
             diag_pt, COLOR_WHITE);
    if (left_pt != COLOR_RED)
        FAIL("off-diagonal LEFT (10,11) = 0x%08X, expected RED 0x%08X",
             left_pt, COLOR_RED);
    if (right_pt != COLOR_GREEN)
        FAIL("off-diagonal RIGHT (w-10,h-11) = 0x%08X, expected GREEN 0x%08X",
             right_pt, COLOR_GREEN);

    gfx_close(h);
}

/* ── Test 2: small central view — surrounding sentinels must not change */

static void test_small_central_view(uint32_t *fb,
                                    const struct fb_info *info,
                                    uint32_t fw, uint32_t fh)
{
    /* Task 8: the surrounding "full" view is now the actual fb
     * dimensions; the small view stays 32×32 in the centre.  The
     * sentinel prefill happens in a separate handle so the
     * small-view present cannot accidentally include the
     * sentinels in its output. */
    uint32_t cw = SMALL_W, ch = SMALL_H;
    if (cw + 4 > fw || ch + 4 > fh)
        FAIL("view too small for small-view test (w=%u h=%u)",
             fw, fh);
    uint32_t ox = (fw - cw) / 2;
    uint32_t oy = (fh - ch) / 2;

    /* Open the small view FIRST so its present runs after we
     * paint the surrounding frame of sentinels in a SECOND,
     * full-screen view.  The sentinel prefill happens in a
     * separate handle so the small-view present cannot
     * accidentally include the sentinels in its output. */
    gfx_handle_t *small = gfx_open(ox, oy, cw, ch);
    if (!small)
        FAIL("small gfx_open(%u,%u,%u,%u) NULL errno=%d",
             ox, oy, cw, ch, errno);

    /* Full-view handle for the surrounding sentinel band. */
    gfx_handle_t *full = gfx_open(0, 0, fw, fh);
    if (!full)
        FAIL("full gfx_open NULL errno=%d", errno);

    /* Paint the WHOLE full view yellow EXCEPT for the rectangle
     * that the small view will cover — that rectangle is left
     * black, and we'll let the small view overwrite it with
     * blue.  When the small view presents, the kernel must not
     * touch the yellow pixels around its rectangle; the
     * sentinels prove the present is bounded. */
    gfx_fill_rect(full, 0, 0, fw, fh, COLOR_YELLOW);
    /* Clear the small-view rectangle in the full buffer so a
     * sentinel-vs-blue confusion is impossible. */
    gfx_fill_rect(full, (int32_t)ox, (int32_t)oy, cw, ch, COLOR_BLACK);

    /* Push the sentinel frame so the yellow band is in the
     * framebuffer BEFORE the small-view present runs. */
    if (gfx_present(full) != 0)
        FAIL("full gfx_present (sentinels) errno=%d", errno);
    gfx_close(full);

    /* Now paint the small view blue + present it. */
    gfx_fill_rect(small, 0, 0, cw, ch, COLOR_BLUE);
    if (gfx_present(small) != 0)
        FAIL("small gfx_present errno=%d", errno);

    /* Spin briefly so the kernel's row copy drains. */
    for (volatile int s = 0; s < 500000; s++) { /* spin briefly */ }

    /* Verify the sentinels are STILL yellow.  We pick points
     * immediately outside the small view's rectangle on all four
     * sides.  If the kernel wrote outside the rectangle, at
     * least one sentinel will be blue instead of yellow. */
    uint32_t above = fb_read(fb, info->stride, ox + cw / 2, oy - 1);
    uint32_t below = fb_read(fb, info->stride, ox + cw / 2, oy + ch);
    uint32_t left_ = fb_read(fb, info->stride, ox - 1, oy + ch / 2);
    uint32_t right_ = fb_read(fb, info->stride, ox + cw, oy + ch / 2);
    /* And confirm the small view's interior is blue. */
    uint32_t inside = fb_read(fb, info->stride,
                              ox + cw / 2, oy + ch / 2);

    if (above != COLOR_YELLOW)
        FAIL("sentinel ABOVE small view = 0x%08X, expected YELLOW "
             "(kernel wrote outside the view rectangle)", above);
    if (below != COLOR_YELLOW)
        FAIL("sentinel BELOW small view = 0x%08X, expected YELLOW", below);
    if (left_ != COLOR_YELLOW)
        FAIL("sentinel LEFT of small view = 0x%08X, expected YELLOW",
             left_);
    if (right_ != COLOR_YELLOW)
        FAIL("sentinel RIGHT of small view = 0x%08X, expected YELLOW",
             right_);
    if (inside != COLOR_BLUE)
        FAIL("inside small view = 0x%08X, expected BLUE 0x%08X",
             inside, COLOR_BLUE);

    gfx_close(small);
}

/* ── Test 3: negative cases ──────────────────────────────────── */

static void test_negative_cases(uint32_t fw, uint32_t fh)
{
    /* Out-of-bounds: x is past the framebuffer's right edge.
     * The kernel validates ``x <= fb_w && w <= fb_w-x``; pick x =
     * UINT32_MAX - 1 so the subtraction ``fb_w - x`` overflows and
     * the kernel rejects with EINVAL regardless of the actual fb
     * resolution (QEMU stdvga is 1440x900 today, but a future
     * resolution bump must not silently pass this test).  A
     * different rejection reason (overflow vs out-of-range) is fine
     * — the contract is "non-NULL must NOT happen". */
    gfx_handle_t *bad = gfx_open(0xFFFFFFFEu, 0u, 4u, 4u);
    if (bad) {
        gfx_close(bad);
        FAIL("gfx_open(x=UINT32_MAX-2,w=4) returned non-NULL — expected EINVAL");
    }
    if (errno != EINVAL)
        FAIL("gfx_open(x=UINT32_MAX-2,w=4) errno=%d, expected EINVAL", errno);

    /* Out-of-bounds: y is past the framebuffer's bottom edge.
     * Same overflow-form rejection as above but on the y axis. */
    gfx_handle_t *bad_y = gfx_open(0u, 0xFFFFFFFEu, 4u, 4u);
    if (bad_y) {
        gfx_close(bad_y);
        FAIL("gfx_open(y=UINT32_MAX-2,h=4) returned non-NULL — expected EINVAL");
    }
    if (errno != EINVAL)
        FAIL("gfx_open(y=UINT32_MAX-2,h=4) errno=%d, expected EINVAL", errno);

    /* Out-of-bounds: w past fb's right edge (using the actual fb
     * width discovered at runtime, so this catches a regression
     * where the kernel stops validating). */
    if (fw < 4u)
        FAIL("framebuffer too narrow for negative-case w overflow (fw=%u)", fw);
    gfx_handle_t *bad_w = gfx_open(0u, 0u, fw + 4u, 4u);
    if (bad_w) {
        gfx_close(bad_w);
        FAIL("gfx_open(w=fb_w+4) returned non-NULL — expected EINVAL");
    }
    if (errno != EINVAL)
        FAIL("gfx_open(w=fb_w+4) errno=%d, expected EINVAL", errno);

    /* Out-of-bounds: h past fb's bottom edge (using the actual fb
     * height discovered at runtime, paired with the w case above). */
    if (fh < 4u)
        FAIL("framebuffer too short for negative-case h overflow (fh=%u)", fh);
    gfx_handle_t *bad_h = gfx_open(0u, 0u, 4u, fh + 4u);
    if (bad_h) {
        gfx_close(bad_h);
        FAIL("gfx_open(h=fb_h+4) returned non-NULL — expected EINVAL");
    }
    if (errno != EINVAL)
        FAIL("gfx_open(h=fb_h+4) errno=%d, expected EINVAL", errno);

    /* Out-of-bounds: x+w wraps past fb's right edge.  Picks
     * x = fb_w - 1, w = 4 so fb_w - x = 1, then w (4) > 1.
     * Catches a kernel that lets the wrap subtraction pass. */
    if (fw >= 4u) {
        gfx_handle_t *bad_wrap = gfx_open(fw - 1u, 0u, 4u, 4u);
        if (bad_wrap) {
            gfx_close(bad_wrap);
            FAIL("gfx_open(x=fb_w-1,w=4) returned non-NULL — "
                 "expected EINVAL (x+w overflows fb)");
        }
        if (errno != EINVAL)
            FAIL("gfx_open(x=fb_w-1,w=4) errno=%d, expected EINVAL", errno);
    }

    /* NULL-handle negative paths — library-side contract from spec
     * §5.  None of these issue an ioctl. */
    gfx_info_t z = gfx_get_info(NULL);
    if (z.width != 0 || z.height != 0 || z.stride != 0)
        FAIL("gfx_get_info(NULL) = (w=%u h=%u stride=%u), expected zeros",
             z.width, z.height, z.stride);
    if (errno != EINVAL)
        FAIL("gfx_get_info(NULL) errno=%d, expected EINVAL", errno);

    if (gfx_present(NULL) != -1)
        FAIL("gfx_present(NULL) returned 0, expected -1");
    if (errno != EINVAL)
        FAIL("gfx_present(NULL) errno=%d, expected EINVAL", errno);

    if (gfx_set_clip(NULL, 0, 0, 4, 4) != -1)
        FAIL("gfx_set_clip(NULL,...) returned 0, expected -1");
    if (errno != EINVAL)
        FAIL("gfx_set_clip(NULL,...) errno=%d, expected EINVAL", errno);
}

/* ── Driver ──────────────────────────────────────────────────── */

int main(void)
{
    /* Open /dev/fb ONCE for tests 1 + 2 (both need to verify the
     * kernel-presented framebuffer).  /dev/fb's mmap is shared
     * across opens, so the kernel's gfx0 present writes are
     * visible to this mapping without a separate mmap syscall. */
    uint32_t *fb = NULL;
    struct fb_info info;
    int fb_fd = fb_open_and_map(&fb, &info);
    if (fb_fd < 0)
        FAIL("fb_open_and_map failed");

    /* Task 8 (user-heap/ELF-isolation plan): the program must
     * demonstrate the heap can hold a full-screen RGB32 pixels
     * buffer (>= 5,184,000 bytes for QEMU 1440×900) plus 64 KiB
     * of program headroom.  Query brk(0) BEFORE gfx_open — the
     * brief explicitly requires this to assert the pre-allocation
     * contract; the actual libgfx pixels buffer is allocated by
     * gfx_open, and Test 1 then confirms its size is at least
     * MIN_FB_BYTES (5,184,000). */
    int64_t cur_brk = syscall(SYS_brk, 0, 0, 0);
    if (cur_brk <= 0)
        FAIL("brk(0) query failed (rc=%ld, errno=%d)",
             (long)cur_brk, errno);
    uint64_t fb_bytes = (uint64_t)info.width *
                        (uint64_t)info.height * 4ull;
    /* Guard: the framebuffer must be at least 1440×900 for the
     * full-screen integration assertion.  A future fb-resolution
     * regression must surface here, not as an opaque kernel-side
     * failure mid-test. */
    if (info.width < MIN_FB_W || info.height < MIN_FB_H)
        FAIL("framebuffer too small for full-screen E2E "
             "(w=%u h=%u, need >= %u×%u)",
             info.width, info.height, MIN_FB_W, MIN_FB_H);
    uint64_t headroom_need = fb_bytes + (uint64_t)HEAP_HEADROOM;
    uint64_t cur_brk_u = (uint64_t)cur_brk;
    if (cur_brk_u + headroom_need > HEAP_LIMIT)
        FAIL("heap headroom insufficient: brk=0x%llx, need=0x%llx "
             "(fb_bytes=0x%llx + headroom=0x%x), limit=0x%lx",
             (unsigned long long)cur_brk_u,
             (unsigned long long)headroom_need,
             (unsigned long long)fb_bytes,
             HEAP_HEADROOM, HEAP_LIMIT);

    /* Tell the kernel we're surrendering the framebuffer so gfx0
     * presents can land without racing terminal.c's writes.  This
     * is the same ioctl terminal.c uses on its way in (see
     * user/terminal.c). */
    (void)ioctl(fb_fd, FBIOSURRENDER, NULL);

    /* Tests in spec order.  Each prints its own FAIL reason. */
    test_negative_cases(info.width, info.height); /* no QEMU drawing needed   */
    test_fullscreen_view(fb, &info,
                         info.width, info.height); /* uses /dev/fb readback    */
    test_small_central_view(fb, &info,
                            info.width, info.height);

    /* Cleanup. */
    close(fb_fd);
    munmap(fb, (size_t)info.height * info.stride);

    PASS();
}