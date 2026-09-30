/*
 * test_gfx_client.c — libgfx client lifecycle (Task 3)
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §3 ("Components and kernel interfaces") + §4 ("ABI and userspace
 * API") + §5 ("Errors and lifecycle"):
 *   - gfx_open(x,y,w,h) opens /dev/gfx0, issues GFX_CREATE_VIEW,
 *     issues GFX_GET_INFO, allocates a zeroed width*height*4 buffer,
 *     and returns an opaque gfx_handle_t.
 *   - The handle is opaque to callers: the public gfx.h does NOT
 *     expose the pixels field. libgfx/internal.h is the shared
 *     private header (never installed to sysroot).
 *   - ENOENT from open("/dev/gfx0", ...) is normalized to ENODEV
 *     (the device simply does not exist in the OS).
 *   - gfx_close frees the pixels buffer, closes the fd, and frees
 *     the handle. On every failure path of gfx_open, the resources
 *     already acquired are released before returning NULL.
 *   - gfx_get_info(NULL) returns a zero gfx_info_t and sets
 *     errno=EINVAL.
 *   - gfx_set_clip stores clip state in the handle; it does NOT
 *     issue any ioctl (the GFX_SET_CLIP ioctl is not part of the
 *     ABI; clip is library-local state per spec §4).
 *   - gfx_present issues exactly ONE ioctl (GFX_PRESENT).
 *
 * Build:
 *   - The REAL libgfx/gfx.c is host-compiled with HOST_CC (no
 *     --target, no sysroot) and linked together with this TU.
 *   - libc's open / ioctl / close / malloc / free are routed
 *     through -Wl,--wrap=... so this TU's __wrap_* stubs observe
 *     every call libgfx makes without disturbing libc internals
 *     (those resolve intra-libc.so and do not flow through the
 *     .elf's wrapped symbols).
 *   - The test exercises the real libgfx public ABI: gfx_open,
 *     gfx_close, gfx_get_info, gfx_set_clip, gfx_present.
 */

#include "test_framework.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gfx.h>
#include <uapi/gfx.h>

/* ── Mock state ───────────────────────────────────────────────
 * Call counters and the values each mock returns.  reset_mocks()
 * restores every default at the start of a test so individual
 * cases never leak state into each other. */
static int   mock_open_count;
static int   mock_close_count;
static int   mock_ioctl_count;
/* mock_alloc_count covers BOTH __wrap_malloc and __wrap_calloc; the
 * two wraps share a counter so callers can reason about libgfx's
 * total allocation pressure at any point (1 = handle, 2 = handle+pixels
 * on the happy path).  Previously this counter only tracked malloc,
 * which made the happy-path assertion break after Task 3's gfx.c
 * switched to calloc() for the zeroed pixels buffer. */
static int   mock_alloc_count;
static int   mock_free_count;

/* Failure injection: 0 = never fail, N = fail on the Nth allocation
 * (counts both malloc and calloc).  Default 0 means the happy path
 * always succeeds; tests that need to exercise the cleanup branch
 * set mock_alloc_fail_at before calling gfx_open. */
static int   mock_alloc_fail_at;

static int   mock_open_fd_to_return;          /* default 7 */
static int   mock_open_errno;                 /* 0 = success */
static int   mock_ioctl_create_view_rv;       /* default 0 */
static int   mock_ioctl_get_info_rv;          /* default 0 */
static int   mock_ioctl_present_rv;           /* default 0 */

static gfx_info_t mock_ioctl_get_info_value;  /* default 4x4 */

#define MAX_IOCTL_LOG 16
static struct {
    unsigned long cmd;
    void *arg;
} mock_ioctl_log[MAX_IOCTL_LOG];

static void reset_mocks(void)
{
    mock_open_count = 0;
    mock_close_count = 0;
    mock_ioctl_count = 0;
    mock_alloc_count = 0;
    mock_free_count = 0;
    mock_alloc_fail_at = 0;
    mock_open_fd_to_return = 7;
    mock_open_errno = 0;
    mock_ioctl_create_view_rv = 0;
    mock_ioctl_get_info_rv = 0;
    mock_ioctl_present_rv = 0;
    mock_ioctl_get_info_value.width = 4;
    mock_ioctl_get_info_value.height = 4;
    mock_ioctl_get_info_value.stride = 16;
    mock_ioctl_get_info_value.format = GFX_FORMAT_RGB32;
    memset(mock_ioctl_log, 0, sizeof(mock_ioctl_log));
}

/* ── Wrapped libc functions (--wrap=open --wrap=close
 *    --wrap=ioctl --wrap=malloc --wrap=free).
 *
 * Each stub records the call and returns its configured value.
 * __wrap_malloc / __wrap_free call through to __real_malloc /
 * __real_free so internal libc users (printf, ...) that resolve
 * inside libc.so are not affected.  Only calls originating from
 * libgfx flow through the wrappers; call counts therefore measure
 * libgfx's behaviour precisely.
 *
 * __real_malloc / __real_free are forward-declared below because
 * the --wrap linker option generates them only at link time — the
 * compiler must be told they exist. */

/* Forward-declarations for the linker-generated fallthrough symbols.
 * --wrap=malloc rewrites `malloc` calls inside libgfx to `__wrap_malloc`
 * and renames the libc malloc to `__real_malloc`; --wrap=free likewise.
 * --wrap=calloc does the same for calloc.  These externs must appear
 * BEFORE the wrap definitions to satisfy the prototype match (an
 * implicit declaration produces a conflicting type when the explicit
 * one follows). */
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void  __real_free(void *);

int __wrap_open(const char *path, int flags, ...)
{
    mock_open_count++;
    (void)path; (void)flags;
    if (mock_open_errno) {
        errno = mock_open_errno;
        return -1;
    }
    return mock_open_fd_to_return;
}

int __wrap_close(int fd)
{
    mock_close_count++;
    (void)fd;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    (void)fd;
    if (mock_ioctl_count < MAX_IOCTL_LOG) {
        mock_ioctl_log[mock_ioctl_count].cmd = request;
        mock_ioctl_log[mock_ioctl_count].arg = arg;
    }
    mock_ioctl_count++;
    int rv;
    switch ((int)request) {
    case GFX_CREATE_VIEW:
        rv = mock_ioctl_create_view_rv;
        break;
    case GFX_GET_INFO:
        rv = mock_ioctl_get_info_rv;
        if (rv == 0 && arg) {
            memcpy(arg, &mock_ioctl_get_info_value,
                   sizeof(mock_ioctl_get_info_value));
        }
        break;
    case GFX_PRESENT:
        rv = mock_ioctl_present_rv;
        break;
    default:
        rv = -ENOTTY;
        break;
    }
    if (rv < 0) {
        errno = -rv;
        return -1;
    }
    return rv;
}

void *__wrap_malloc(size_t size)
{
    mock_alloc_count++;
    /* Failure injection: when mock_alloc_fail_at == N and this is the
     * Nth allocation overall (counting malloc+calloc), return NULL
     * with errno=ENOMEM.  libgfx's malloc-failure paths translate the
     * result into NULL return with errno=ENOMEM, which is what the
     * cleanup contract tests assert. */
    if (mock_alloc_fail_at != 0 && mock_alloc_count == mock_alloc_fail_at) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    mock_alloc_count++;
    /* Same failure-injection contract as __wrap_malloc.  Used by
     * gfx_open()'s pixel-buffer allocation (libgfx uses calloc to
     * get a zeroed buffer; the contract test needs a deterministic
     * way to make it fail without relying on Linux overcommit). */
    if (mock_alloc_fail_at != 0 && mock_alloc_count == mock_alloc_fail_at) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(nmemb, size);
}

void __wrap_free(void *ptr)
{
    if (ptr) mock_free_count++;
    __real_free(ptr);
}

/* ── Test cases ─────────────────────────────────────────────── */

TEST_FUNC(test_open_happy_path)
{
    TEST_SUITE("open: happy path");
    reset_mocks();
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    assert_eq(1, mock_open_count);
    assert_eq(0, mock_close_count);
    assert_eq(2, mock_ioctl_count);
    /* open -> CREATE_VIEW -> GET_INFO */
    assert_eq((int)GFX_CREATE_VIEW, (int)mock_ioctl_log[0].cmd);
    assert_eq((int)GFX_GET_INFO,    (int)mock_ioctl_log[1].cmd);
    /* 2 allocations: handle (malloc) + pixels (calloc) — both must
     * be observed by the shared mock_alloc_count counter. */
    assert_eq(2, mock_alloc_count);
    assert_eq(0, mock_free_count);
    /* closing cleans up */
    gfx_close(h);
    assert_eq(1, mock_close_count);
    assert_eq(2, mock_free_count);
    assert_eq(2, mock_alloc_count);  /* no new allocs on close */
}

TEST_FUNC(test_open_enoint_normalized_to_enodev)
{
    TEST_SUITE("open: ENOENT -> ENODEV");
    reset_mocks();
    mock_open_errno = ENOENT;
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(ENODEV, errno);
    /* No further calls were made (no fd to close, no handle allocated). */
    assert_eq(1, mock_open_count);
    assert_eq(0, mock_close_count);
    assert_eq(0, mock_ioctl_count);
    assert_eq(0, mock_alloc_count);
    assert_eq(0, mock_free_count);
}

TEST_FUNC(test_open_other_open_errors_also_enodev)
{
    TEST_SUITE("open: other errors -> ENODEV");
    reset_mocks();
    mock_open_errno = EACCES;   /* any open failure → ENODEV per spec §5 */
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(ENODEV, errno);
    assert_eq(0, mock_ioctl_count);
    assert_eq(0, mock_alloc_count);
}

TEST_FUNC(test_open_create_view_failure_cleanup)
{
    TEST_SUITE("open: CREATE_VIEW ioctl failure");
    reset_mocks();
    mock_ioctl_create_view_rv = -EINVAL;
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(EINVAL, errno);  /* propagated from ioctl wrapper */
    assert_eq(1, mock_open_count);
    assert_eq(1, mock_ioctl_count);  /* only CREATE_VIEW reached */
    /* Handle was malloc'd; cleanup free'd it. */
    assert_eq(1, mock_alloc_count);
    assert_eq(1, mock_free_count);
    assert_eq(1, mock_close_count);  /* fd was opened, then closed */
}

TEST_FUNC(test_open_get_info_failure_cleanup)
{
    TEST_SUITE("open: GET_INFO ioctl failure");
    reset_mocks();
    mock_ioctl_get_info_rv = -EINVAL;
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(EINVAL, errno);
    assert_eq(2, mock_ioctl_count);  /* CREATE then GET_INFO */
    assert_eq(1, mock_alloc_count);
    assert_eq(1, mock_free_count);
    assert_eq(1, mock_close_count);
}

TEST_FUNC(test_open_pixel_alloc_failure_cleanup)
{
    TEST_SUITE("open: pixel buffer alloc failure");
    reset_mocks();
    /* Force the 2nd allocation (the pixel-buffer calloc) to fail.  The
     * 1st allocation is the handle malloc (succeeds), the 2nd is the
     * pixels calloc (returns NULL with errno=ENOMEM).  We use the
     * mock_alloc_fail_at injection knob rather than a 65536x65536
     * overcommit probe: Linux's overcommit makes the 16 GiB
     * allocation succeed silently, so the old test was passing-by-
     * accident and never actually exercised the calloc-failure path. */
    mock_alloc_fail_at = 2;
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(ENOMEM, errno);
    /* Handle was allocated (count==1) and the pixel calloc was
     * attempted (count==2, returned NULL).  Cleanup closes the fd
     * and frees the handle (handle->pixels was never set, so gfx.c's
     * free(h->pixels) is free(NULL) which is safe and does NOT
     * increment mock_free_count). */
    assert_eq(2, mock_alloc_count);
    assert_eq(1, mock_free_count);
    assert_eq(1, mock_close_count);
}

TEST_FUNC(test_get_info_null_returns_zero_with_einval)
{
    TEST_SUITE("get_info(NULL)");
    reset_mocks();
    errno = 0;
    gfx_info_t info = gfx_get_info(NULL);
    assert_eq(0, info.width);
    assert_eq(0, info.height);
    assert_eq(0, info.stride);
    assert_eq(0, info.format);
    assert_eq(EINVAL, errno);
    assert_eq(0, mock_ioctl_count);  /* never issues an ioctl */
}

TEST_FUNC(test_get_info_valid_handle_returns_stored_info)
{
    TEST_SUITE("get_info: valid handle returns stored info");
    reset_mocks();
    mock_ioctl_get_info_value.width = 320;
    mock_ioctl_get_info_value.height = 200;
    mock_ioctl_get_info_value.stride = 320u * 4u;
    mock_ioctl_get_info_value.format = GFX_FORMAT_RGB32;
    gfx_handle_t *h = gfx_open(0, 0, 320, 200);
    assert_not_null(h);
    int ioctls_after_open = mock_ioctl_count;  /* 2 */
    errno = 0;
    gfx_info_t info = gfx_get_info(h);
    assert_eq(320u, info.width);
    assert_eq(200u, info.height);
    assert_eq(320u * 4u, info.stride);
    assert_eq((unsigned)GFX_FORMAT_RGB32, info.format);
    assert_eq(0, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);  /* no new ioctl */
    gfx_close(h);
}

TEST_FUNC(test_set_clip_null_returns_einval)
{
    TEST_SUITE("set_clip(NULL)");
    reset_mocks();
    errno = 0;
    int rc = gfx_set_clip(NULL, 0, 0, 10, 10);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(0, mock_ioctl_count);
}

TEST_FUNC(test_set_clip_valid_does_no_ioctl)
{
    TEST_SUITE("set_clip: no ioctl");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int ioctls_after_open = mock_ioctl_count;  /* 2 */
    errno = 0;
    int rc = gfx_set_clip(h, 1, 2, 3, 4);
    assert_eq(0, rc);
    assert_eq(0, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);  /* no ioctl */
    gfx_close(h);
}

TEST_FUNC(test_present_issues_exactly_one_ioctl)
{
    TEST_SUITE("present: exactly one ioctl");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int ioctls_after_open = mock_ioctl_count;  /* 2 */
    errno = 0;
    int rc = gfx_present(h);
    assert_eq(0, rc);
    assert_eq(ioctls_after_open + 1, mock_ioctl_count);  /* +1 PRESENT */
    assert_eq((int)GFX_PRESENT, (int)mock_ioctl_log[ioctls_after_open].cmd);
    /* second present is a second ioctl */
    rc = gfx_present(h);
    assert_eq(0, rc);
    assert_eq(ioctls_after_open + 2, mock_ioctl_count);
    gfx_close(h);
}

TEST_FUNC(test_present_null_returns_einval)
{
    TEST_SUITE("present(NULL)");
    reset_mocks();
    errno = 0;
    int rc = gfx_present(NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(0, mock_ioctl_count);
}

TEST_FUNC(test_present_propagates_einval)
{
    TEST_SUITE("present: ioctl EINVAL propagated");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    mock_ioctl_present_rv = -EINVAL;
    errno = 0;
    int rc = gfx_present(h);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    gfx_close(h);
}

TEST_FUNC(test_close_null_is_safe)
{
    TEST_SUITE("close(NULL) safe");
    reset_mocks();
    gfx_close(NULL);
    /* No mock calls (NULL guard). */
    assert_eq(0, mock_open_count);
    assert_eq(0, mock_close_count);
    assert_eq(0, mock_alloc_count);
    assert_eq(0, mock_free_count);
    assert_eq(0, mock_ioctl_count);
}

TEST_FUNC(test_present_req_pixel_pointer_is_buffer)
{
    TEST_SUITE("present: req.pixels is the handle's buffer");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int present_idx = mock_ioctl_count;
    gfx_present(h);
    gfx_present_req_t *req =
        (gfx_present_req_t *)mock_ioctl_log[present_idx].arg;
    assert_not_null(req);
    assert_eq((unsigned)mock_ioctl_get_info_value.stride, req->stride);
    assert_eq(0u, req->reserved);
    /* pixels must be non-NULL and equal to what the handle allocated. */
    assert_true(req->pixels != 0);
    /* stride matches info.stride; libgfx never trusts a user-supplied
     * stride - the kernel validates against info.stride too (spec §4). */
    gfx_close(h);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_open_happy_path),
    TEST_ENTRY(test_open_enoint_normalized_to_enodev),
    TEST_ENTRY(test_open_other_open_errors_also_enodev),
    TEST_ENTRY(test_open_create_view_failure_cleanup),
    TEST_ENTRY(test_open_get_info_failure_cleanup),
    TEST_ENTRY(test_open_pixel_alloc_failure_cleanup),
    TEST_ENTRY(test_get_info_null_returns_zero_with_einval),
    TEST_ENTRY(test_get_info_valid_handle_returns_stored_info),
    TEST_ENTRY(test_set_clip_null_returns_einval),
    TEST_ENTRY(test_set_clip_valid_does_no_ioctl),
    TEST_ENTRY(test_present_issues_exactly_one_ioctl),
    TEST_ENTRY(test_present_null_returns_einval),
    TEST_ENTRY(test_present_propagates_einval),
    TEST_ENTRY(test_close_null_is_safe),
    TEST_ENTRY(test_present_req_pixel_pointer_is_buffer),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
