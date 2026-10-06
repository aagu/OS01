/*
 * test_setres.c — setres CLI + long-running gfx client policy
 * (QEMU resolution switcher, Task 8).
 *
 * Spec (docs/superpowers/specs/2026-10-06-qemu-resolution-switcher-design.md):
 *   §7 bullet 1+2 — CLI `-l` / `-h` / `W H` / `WxH`, decimal-only
 *     parsing (reject empty field / negative / zero / overflow /
 *     trailing character / extra args), help opens no device,
 *     `-l` issues ONE GET_MODES(capacity=16) and prints the current
 *     mode independently (even when it is not in the whitelist),
 *     prints the generation, and distinguishes ENODEV/EIO/EBUSY
 *     (EBUSY mentions the needed restart).
 *   §3.3 — long-running gfx clients (desktop / test_lvgl / tetris)
 *     keep their view and retry (>= 250 ms) on transient EAGAIN,
 *     but release resources and exit non-zero on stale/permanent
 *     ESTALE / EIO, without letting cleanup clobber the diagnostic
 *     errno.
 *
 * Build: the REAL user/setres.c (compiled with -DSETRES_NO_MAIN),
 * user/setres_parse.c and user/gfx_client_policy.c are host-compiled
 * and linked here.  libc open/close/ioctl are routed through
 * -Wl,--wrap so the test observes exactly which device ops the CLI
 * performs (help must open NOTHING).
 */

#include "test_framework.h"
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_client_policy.h"
#include "setres_parse.h"
#include <uapi/fb.h>

/* Production entry point of the CLI core (the real TU defines main()
 * only when SETRES_NO_MAIN is not defined). */
extern int setres_run(int argc, char *const argv[], FILE *out, FILE *err);

/* ── Mock device state ──────────────────────────────────────── */
static int mock_open_count;
static int mock_close_count;
static int mock_ioctl_count;
static int mock_open_errno;   /* non-zero -> open() fails */
static int mock_fd;           /* fd returned by open() */

static int mock_get_modes_rv;
static int mock_get_state_rv;
static int mock_set_mode_rv;

static struct fb_modes_req mock_modes;   /* canned device mode list */
static struct fb_state     mock_state;   /* canned current state   */

static int      mock_seen_get_modes;
static uint32_t mock_seen_capacity;
static int      mock_set_calls;
static struct fb_set_mode_req mock_set_req;

static void reset_mocks(void)
{
    mock_open_count = 0;
    mock_close_count = 0;
    mock_ioctl_count = 0;
    mock_open_errno = 0;
    mock_fd = 7;

    mock_get_modes_rv = 0;
    mock_get_state_rv = 0;
    mock_set_mode_rv = 0;

    mock_seen_get_modes = 0;
    mock_seen_capacity = 0;
    mock_set_calls = 0;
    memset(&mock_set_req, 0, sizeof(mock_set_req));

    /* A device list that does NOT include the current 1366x768 boot
     * mode: the CLI must still print the current mode separately. */
    memset(&mock_modes, 0, sizeof(mock_modes));
    mock_modes.capacity = FB_MAX_MODES;
    mock_modes.total = 3;
    mock_modes.count = 3;
    mock_modes.modes[0].width = 800;  mock_modes.modes[0].height = 600;
    mock_modes.modes[0].stride = 800 * 4;  mock_modes.modes[0].bpp = 32;
    mock_modes.modes[1].width = 1024; mock_modes.modes[1].height = 768;
    mock_modes.modes[1].stride = 1024 * 4; mock_modes.modes[1].bpp = 32;
    mock_modes.modes[2].width = 1280; mock_modes.modes[2].height = 720;
    mock_modes.modes[2].stride = 1280 * 4; mock_modes.modes[2].bpp = 32;

    memset(&mock_state, 0, sizeof(mock_state));
    mock_state.info.width = 1366;
    mock_state.info.height = 768;
    mock_state.info.stride = 1366 * 4;
    mock_state.info.bpp = 32;
    mock_state.info.format = FB_FORMAT_RGB32;
    mock_state.reserved = 0;
    mock_state.generation = 42;
}

/* ── Wrapped libc (observe / control the device ops) ────────── */
int __wrap_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    mock_open_count++;
    if (mock_open_errno) { errno = mock_open_errno; return -1; }
    return mock_fd;
}

int __wrap_close(int fd)
{
    (void)fd;
    mock_close_count++;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    (void)fd;
    mock_ioctl_count++;

    int rv = 0;
    switch ((int)request) {
    case FBIOGET_MODES: {
        mock_seen_get_modes++;
        struct fb_modes_req *r = (struct fb_modes_req *)arg;
        if (r) mock_seen_capacity = r->capacity;   /* record request */
        if (mock_get_modes_rv == 0 && r) *r = mock_modes;
        rv = mock_get_modes_rv;
        break;
    }
    case FBIOGET_STATE:
        if (mock_get_state_rv == 0 && arg) *(struct fb_state *)arg = mock_state;
        rv = mock_get_state_rv;
        break;
    case FBIOSET_MODE:
        mock_set_calls++;
        if (arg) mock_set_req = *(struct fb_set_mode_req *)arg;
        rv = mock_set_mode_rv;
        break;
    default:
        rv = -ENOTTY;
        break;
    }
    if (rv < 0) { errno = -rv; return -1; }
    return rv;
}

/* ── CLI capture helpers (open_memstream) ───────────────────── */
static char  *cli_out;
static size_t cli_out_len;
static char  *cli_err;
static size_t cli_err_len;

static int run_cli(int argc, char *const argv[])
{
    FILE *fo = open_memstream(&cli_out, &cli_out_len);
    FILE *fe = open_memstream(&cli_err, &cli_err_len);
    if (!fo || !fe) return -999;
    int rc = setres_run(argc, argv, fo, fe);
    fflush(fo);
    fflush(fe);
    fclose(fo);
    fclose(fe);
    return rc;
}

static bool out_has(const char *needle)
{
    return cli_out && strstr(cli_out, needle) != NULL;
}
static bool err_has(const char *needle)
{
    return cli_err && strstr(cli_err, needle) != NULL;
}

/* ── Parser tests ───────────────────────────────────────────── */
TEST_FUNC(test_parse_wxh_and_two_args)
{
    TEST_SUITE("setres_parse: WxH and W H");
    struct setres_args a;
    char *avx[] = { "setres", "1280x720", NULL };
    assert_eq(0, setres_parse(2, avx, &a));
    assert_eq(SETRES_SET, a.action);
    assert_eq(1280u, a.width);
    assert_eq(720u, a.height);

    char *avs[] = { "setres", "1280", "720", NULL };
    assert_eq(0, setres_parse(3, avs, &a));
    assert_eq(SETRES_SET, a.action);
    assert_eq(1280u, a.width);
    assert_eq(720u, a.height);
}

TEST_FUNC(test_parse_help_and_list)
{
    TEST_SUITE("setres_parse: -h / -l");
    struct setres_args a;
    char *h[] = { "setres", "-h", NULL };
    assert_eq(0, setres_parse(2, h, &a));
    assert_eq(SETRES_HELP, a.action);

    char *l[] = { "setres", "-l", NULL };
    assert_eq(0, setres_parse(2, l, &a));
    assert_eq(SETRES_LIST, a.action);
}

TEST_FUNC(test_parse_rejects_malformed)
{
    TEST_SUITE("setres_parse: malformed input rejected");
    struct setres_args a;

    char *r0[]   = { "setres", "0x0", NULL };
    char *r0b[]  = { "setres", "0", "720", NULL };
    char *r0c[]  = { "setres", "1280", "0", NULL };
    char *rneg[] = { "setres", "-1", "720", NULL };
    char *rneg2[]= { "setres", "1280", "-1", NULL };
    char *rov[]  = { "setres", "4294967296", "720", NULL };
    char *rovx[] = { "setres", "4294967296x720", NULL };
    char *remp1[]= { "setres", "1280x", NULL };
    char *remp2[]= { "setres", "x720", NULL };
    char *rsuf[] = { "setres", "1280x720x", NULL };
    char *rsuf2[]= { "setres", "12a0", "720", NULL };
    char *rsuf3[]= { "setres", "1280", "720abc", NULL };
    char *rextra[]={ "setres", "1280", "720", "extra", NULL };
    char *rmiss[]= { "setres", NULL };
    char *rmiss2[]={ "setres", "1280", NULL };
    char *rhelp[]= { "setres", "-h", "x", NULL };

    assert_eq(-1, setres_parse(2, r0, &a));
    assert_eq(-1, setres_parse(3, r0b, &a));
    assert_eq(-1, setres_parse(3, r0c, &a));
    assert_eq(-1, setres_parse(3, rneg, &a));
    assert_eq(-1, setres_parse(3, rneg2, &a));
    assert_eq(-1, setres_parse(3, rov, &a));
    assert_eq(-1, setres_parse(2, rovx, &a));
    assert_eq(-1, setres_parse(2, remp1, &a));
    assert_eq(-1, setres_parse(2, remp2, &a));
    assert_eq(-1, setres_parse(2, rsuf, &a));
    assert_eq(-1, setres_parse(3, rsuf2, &a));
    assert_eq(-1, setres_parse(3, rsuf3, &a));
    assert_eq(-1, setres_parse(4, rextra, &a));
    assert_eq(-1, setres_parse(1, rmiss, &a));
    assert_eq(-1, setres_parse(2, rmiss2, &a));
    assert_eq(-1, setres_parse(3, rhelp, &a));
}

TEST_FUNC(test_parse_uint32_boundary)
{
    TEST_SUITE("setres_parse: UINT32_MAX accepted, 2^32 rejected");
    struct setres_args a;
    char *amax[] = { "setres", "4294967295", "1", NULL };
    assert_eq(0, setres_parse(3, amax, &a));
    assert_eq(4294967295u, a.width);
    assert_eq(1u, a.height);
}

/* ── CLI behaviour tests ────────────────────────────────────── */
TEST_FUNC(test_help_opens_no_device)
{
    TEST_SUITE("CLI -h: no device open");
    reset_mocks();
    char *av[] = { "setres", "-h", NULL };
    int rc = run_cli(2, av);
    assert_eq(0, rc);
    assert_eq(0, mock_open_count);
    assert_eq(0, mock_ioctl_count);
    assert_true(out_has("usage"));
}

TEST_FUNC(test_list_capacity_and_current)
{
    TEST_SUITE("CLI -l: single GET_MODES(16) + current mode");
    reset_mocks();
    char *av[] = { "setres", "-l", NULL };
    int rc = run_cli(2, av);
    assert_eq(0, rc);
    assert_eq(1, mock_open_count);
    assert_eq(1, mock_seen_get_modes);
    assert_eq(FB_MAX_MODES, (int)mock_seen_capacity);
    /* current mode is 1366x768 and is NOT in the whitelist: still printed */
    assert_true(out_has("1366"));
    assert_true(out_has("768"));
    assert_true(out_has("CURRENT"));
    /* diagnostics: generation + listed modes */
    assert_true(out_has("42"));
    assert_true(out_has("800"));
    assert_true(out_has("1280"));
}

TEST_FUNC(test_set_supported_mode)
{
    TEST_SUITE("CLI set: supported mode issues SET_MODE");
    reset_mocks();
    char *av[] = { "setres", "1280x720", NULL };
    int rc = run_cli(2, av);
    assert_eq(0, rc);
    assert_eq(1, mock_set_calls);
    assert_eq(1280u, mock_set_req.width);
    assert_eq(720u, mock_set_req.height);
    assert_eq(0u, mock_set_req.bpp);
}

TEST_FUNC(test_set_unsupported_mode_rejected)
{
    TEST_SUITE("CLI set: unsupported mode rejected before SET_MODE");
    reset_mocks();
    char *av[] = { "setres", "1600x900", NULL };   /* not in mock list */
    int rc = run_cli(2, av);
    assert_eq(1, rc);
    assert_eq(0, mock_set_calls);
    assert_true(err_has("not a supported mode"));
}

TEST_FUNC(test_set_ebusy_mentions_restart)
{
    TEST_SUITE("CLI set: EBUSY explains sticky mmap needs restart");
    reset_mocks();
    mock_set_mode_rv = -EBUSY;
    char *av[] = { "setres", "1280x720", NULL };
    int rc = run_cli(2, av);
    assert_eq(1, rc);
    assert_eq(1, mock_set_calls);
    assert_true(err_has("restart"));
}

TEST_FUNC(test_no_device_reports_error)
{
    TEST_SUITE("CLI: missing device is a failure");
    reset_mocks();
    mock_open_errno = ENOENT;
    char *av[] = { "setres", "-l", NULL };
    int rc = run_cli(2, av);
    assert_eq(1, rc);
    assert_true(err_has("/dev/fb"));
}

/* ── Long-running client policy + loop ──────────────────────── */
TEST_FUNC(test_policy_classification)
{
    TEST_SUITE("gfx_client_present_policy: errno classification");
    assert_true(GFX_CLIENT_RETRY_MS >= 250u);
    assert_eq(0,  gfx_client_present_policy(0));
    assert_eq(1,  gfx_client_present_policy(EAGAIN));
    assert_eq(-1, gfx_client_present_policy(ESTALE));
    assert_eq(-1, gfx_client_present_policy(EIO));
    assert_eq(-1, gfx_client_present_policy(EINVAL));
}

/* Scripted mock gfx_present + canonical client loop (the shape the
 * three long-running clients use: classify via gfx_client_present_policy,
 * throttle EAGAIN retries to >= GFX_CLIENT_RETRY_MS, release + exit on
 * the permanent class). */
static int loop_script[8];
static int loop_script_len;
static int loop_script_i;
static int loop_present_calls;
static int loop_close_calls;

static int mock_present(void)
{
    loop_present_calls++;
    if (loop_script_i < loop_script_len) {
        int e = loop_script[loop_script_i++];
        if (e) { errno = e; return -1; }
    }
    return 0;
}

static void mock_close(void)
{
    loop_close_calls++;
    errno = EBADF;   /* cleanup deliberately clobbers errno */
}

static int client_loop(int *out_errno, unsigned *out_wait_ms)
{
    unsigned wait_ms = 0;
    for (;;) {
        errno = 0;
        if (mock_present() == 0) {
            *out_errno = 0;
            *out_wait_ms = wait_ms;
            return 0;
        }
        int saved = errno;
        if (gfx_client_present_policy(saved) == 1) {
            wait_ms += GFX_CLIENT_RETRY_MS;   /* transient: keep view */
            continue;
        }
        mock_close();                        /* permanent: release ... */
        errno = saved;                       /* ... preserve errno */
        *out_errno = errno;
        *out_wait_ms = wait_ms;
        return 1;
    }
}

static void arm_script(const int *script, int n)
{
    memcpy(loop_script, script, (size_t)n * sizeof(loop_script[0]));
    loop_script_len = n;
    loop_script_i = 0;
    loop_present_calls = 0;
    loop_close_calls = 0;
}

TEST_FUNC(test_loop_eagain_retries_and_succeeds)
{
    TEST_SUITE("client loop: EAGAIN -> keep view, retry >= 250 ms");
    const int script[] = { EAGAIN, EAGAIN, 0 };
    arm_script(script, 3);
    int err = 0;
    unsigned wait = 0;
    int rc = client_loop(&err, &wait);
    assert_eq(0, rc);                 /* did not exit */
    assert_eq(3, loop_present_calls); /* two retries + success */
    assert_eq(0, loop_close_calls);   /* view kept */
    assert_true(wait >= 2u * GFX_CLIENT_RETRY_MS);
    assert_true(wait >= 500u);
}

TEST_FUNC(test_loop_estale_exits_preserving_errno)
{
    TEST_SUITE("client loop: ESTALE -> release + exit, errno preserved");
    const int script[] = { ESTALE };
    arm_script(script, 1);
    int err = 0;
    unsigned wait = 0;
    int rc = client_loop(&err, &wait);
    assert_eq(1, rc);                 /* non-zero exit */
    assert_eq(1, loop_close_calls);   /* resources released */
    assert_eq(ESTALE, err);           /* cleanup did not clobber errno */
}

TEST_FUNC(test_loop_eio_exits_preserving_errno)
{
    TEST_SUITE("client loop: EIO -> release + exit, errno preserved");
    const int script[] = { EIO };
    arm_script(script, 1);
    int err = 0;
    unsigned wait = 0;
    int rc = client_loop(&err, &wait);
    assert_eq(1, rc);
    assert_eq(1, loop_close_calls);
    assert_eq(EIO, err);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_parse_wxh_and_two_args),
    TEST_ENTRY(test_parse_help_and_list),
    TEST_ENTRY(test_parse_rejects_malformed),
    TEST_ENTRY(test_parse_uint32_boundary),
    TEST_ENTRY(test_help_opens_no_device),
    TEST_ENTRY(test_list_capacity_and_current),
    TEST_ENTRY(test_set_supported_mode),
    TEST_ENTRY(test_set_unsupported_mode_rejected),
    TEST_ENTRY(test_set_ebusy_mentions_restart),
    TEST_ENTRY(test_no_device_reports_error),
    TEST_ENTRY(test_policy_classification),
    TEST_ENTRY(test_loop_eagain_retries_and_succeeds),
    TEST_ENTRY(test_loop_estale_exits_preserving_errno),
    TEST_ENTRY(test_loop_eio_exits_preserving_errno),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
