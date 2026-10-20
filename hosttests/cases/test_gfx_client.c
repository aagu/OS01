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
#include <sys/mman.h>

#include <gfx.h>
#include <uapi/gfx.h>

/* internal.h exposes the opaque handle's layout to hosttests so the
 * pixel-pointer snapshot test can read back what gfx.c stored.  This
 * mirrors test_gfx_primitives.c (Task 4).  internal.h is NOT shipped
 * to the sysroot; the test reaches it via -I$(libgfx_dir) on the
 * host compile line in hosttests/Makefile. */
#include "internal.h"

/* ── Mock state ───────────────────────────────────────────────
 * Call counters and the values each mock returns.  reset_mocks()
 * restores every default at the start of a test so individual
 * cases never leak state into each other. */
static int   mock_open_count;
static int   mock_close_count;
static int   mock_ioctl_count;
/* mock_alloc_count covers __wrap_malloc and __wrap_calloc (for handle).
 * mock_mmap_count / mock_munmap_count cover anonymous mmap / munmap
 * (for pixels buffer). */
static int   mock_alloc_count;
static int   mock_free_count;
static int   mock_mmap_count;
static int   mock_munmap_count;
static int   mock_mmap_fail;

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
static int   mock_ioctl_acquire_rv;           /* default 0 */
static int   mock_ioctl_release_rv;           /* default 0 */
static int   mock_ioctl_register_fallback_rv; /* default 0 */
static int   mock_ioctl_get_display_state_rv; /* default 0 */
static int   mock_ioctl_replace_fallback_rv;  /* default 0 */
static int   mock_ioctl_present_rect_rv;      /* default 0 (Task 2) */

static gfx_info_t mock_ioctl_get_info_value;  /* default 4x4 */
static gfx_display_state_t mock_ioctl_display_state_value;
static gfx_replace_fallback_t mock_ioctl_replace_fallback_passed;
static gfx_bind_input_t mock_ioctl_bind_value;      /* the kernel's answer */
static gfx_bind_input_t mock_ioctl_bind_passed;     /* what the client sent */
static int mock_ioctl_bind_rv;
static int mock_ioctl_unbind_rv;

#define MAX_IOCTL_LOG 16
static struct {
    int fd;
    unsigned long cmd;
    void *arg;
    uint64_t raw_arg;
} mock_ioctl_log[MAX_IOCTL_LOG];

static void reset_mocks(void)
{
    mock_open_count = 0;
    mock_close_count = 0;
    mock_ioctl_count = 0;
    mock_alloc_count = 0;
    mock_free_count = 0;
    mock_mmap_count = 0;
    mock_munmap_count = 0;
    mock_mmap_fail = 0;
    mock_alloc_fail_at = 0;
    mock_open_fd_to_return = 7;
    mock_open_errno = 0;
    mock_ioctl_create_view_rv = 0;
    mock_ioctl_get_info_rv = 0;
    mock_ioctl_present_rv = 0;
    mock_ioctl_acquire_rv = 0;
    mock_ioctl_release_rv = 0;
    mock_ioctl_register_fallback_rv = 0;
    mock_ioctl_get_display_state_rv = 0;
    mock_ioctl_replace_fallback_rv = 0;
    mock_ioctl_present_rect_rv = 0;
    mock_ioctl_bind_rv = 0;
    mock_ioctl_unbind_rv = 0;
    mock_ioctl_get_info_value.width = 4;
    mock_ioctl_get_info_value.height = 4;
    mock_ioctl_get_info_value.stride = 16;
    mock_ioctl_get_info_value.format = GFX_FORMAT_RGB32;
    memset(&mock_ioctl_display_state_value, 0, sizeof(mock_ioctl_display_state_value));
    mock_ioctl_display_state_value.generation = 1;
    mock_ioctl_display_state_value.flags = GFX_DISPLAY_STATE_FOREGROUND;
    mock_ioctl_display_state_value.reserved = 0;
    memset(&mock_ioctl_replace_fallback_passed, 0, sizeof(mock_ioctl_replace_fallback_passed));
    memset(&mock_ioctl_bind_value, 0, sizeof(mock_ioctl_bind_value));
    memset(&mock_ioctl_bind_passed, 0, sizeof(mock_ioctl_bind_passed));
    memset(mock_ioctl_log, 0, sizeof(mock_ioctl_log));
}

/* ── Wrapped libc functions (--wrap=open --wrap=close
 *    --wrap=ioctl --wrap=malloc --wrap=free --wrap=mmap --wrap=munmap).
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
 * --wrap=calloc does the same for calloc.  --wrap=mmap and --wrap=munmap
 * intercept anonymous mmap/munmap for the pixels buffer. */
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void  __real_free(void *);
extern void *__real_mmap(void *, size_t, int, int, int, off_t);
extern int   __real_munmap(void *, size_t);

int __wrap_open(const char *path, int flags, ...)
{
    mock_open_count++;
    (void)path; (void)flags;
    if (mock_open_errno) {
        errno = mock_open_errno;
        return -1;
    }
    return mock_open_fd_to_return++;
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
    uint64_t raw_arg = va_arg(ap, uint64_t);
    void *arg = (void *)(uintptr_t)raw_arg;
    va_end(ap);
    if (mock_ioctl_count < MAX_IOCTL_LOG) {
        mock_ioctl_log[mock_ioctl_count].fd = fd;
        mock_ioctl_log[mock_ioctl_count].cmd = request;
        mock_ioctl_log[mock_ioctl_count].arg = arg;
        mock_ioctl_log[mock_ioctl_count].raw_arg = raw_arg;
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
    case GFX_PRESENT_RECT:
        rv = mock_ioctl_present_rect_rv;
        break;
    case GFX_ACQUIRE:
        rv = mock_ioctl_acquire_rv;
        break;
    case GFX_RELEASE:
        rv = mock_ioctl_release_rv;
        break;
    case GFX_REGISTER_FALLBACK:
        rv = mock_ioctl_register_fallback_rv;
        break;
    case GFX_GET_DISPLAY_STATE:
        rv = mock_ioctl_get_display_state_rv;
        if (rv == 0 && arg) {
            memcpy(arg, &mock_ioctl_display_state_value,
                   sizeof(mock_ioctl_display_state_value));
        }
        break;
    case GFX_REPLACE_FALLBACK:
        rv = mock_ioctl_replace_fallback_rv;
        if (arg) {
            memcpy(&mock_ioctl_replace_fallback_passed, arg,
                   sizeof(mock_ioctl_replace_fallback_passed));
        }
        break;
    case GFX_BIND_INPUT:
        rv = mock_ioctl_bind_rv;
        if (arg) {
            /* Keep what the client sent, then answer with the lease
             * result the kernel would have written back. */
            memcpy(&mock_ioctl_bind_passed, arg, sizeof(mock_ioctl_bind_passed));
            if (rv == 0) {
                memcpy(arg, &mock_ioctl_bind_value, sizeof(mock_ioctl_bind_value));
            }
        }
        break;
    case GFX_UNBIND_INPUT:
        rv = mock_ioctl_unbind_rv;
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

void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    mock_mmap_count++;
    if (mock_mmap_fail) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(addr, length, prot, flags, fd, offset);
}

int __wrap_munmap(void *addr, size_t length)
{
    mock_munmap_count++;
    return __real_munmap(addr, length);
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
    /* 1 heap allocation for handle (malloc) + 1 anonymous mmap for pixels */
    assert_eq(1, mock_alloc_count);
    assert_eq(1, mock_mmap_count);
    assert_eq(0, mock_free_count);
    assert_eq(0, mock_munmap_count);
    /* closing cleans up */
    gfx_close(h);
    assert_eq(1, mock_close_count);
    assert_eq(1, mock_free_count);
    assert_eq(1, mock_munmap_count);
    assert_eq(1, mock_alloc_count);  /* no new allocs on close */
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

TEST_FUNC(test_open_enfile_is_preserved)
{
    TEST_SUITE("open: fd table full preserves ENFILE");
    reset_mocks();
    mock_open_errno = ENFILE;
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(ENFILE, errno);
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
    TEST_SUITE("open: pixel buffer mmap failure");
    reset_mocks();
    /* Force the pixel-buffer mmap to fail (returns MAP_FAILED with errno=ENOMEM). */
    mock_mmap_fail = 1;
    errno = 0;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_null(h);
    assert_eq(ENOMEM, errno);
    /* Handle was allocated (count==1) and the pixel mmap was
     * attempted (count==1, returned MAP_FAILED).  Cleanup closes the fd
     * and frees the handle (handle->pixels was never mapped, so
     * munmap is not called). */
    assert_eq(1, mock_alloc_count);
    assert_eq(1, mock_mmap_count);
    assert_eq(1, mock_free_count);
    assert_eq(0, mock_munmap_count);
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

TEST_FUNC(test_present_propagates_switch_errnos)
{
    TEST_SUITE("present: EAGAIN / ESTALE / EIO propagated unchanged");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    /* The resolution-switcher contract (spec §3.3) depends on libgfx
     * forwarding these three errnos verbatim; a wrapper that swallowed
     * or remapped them would break the client retry/exit policy. */
    const int errs[] = { EAGAIN, ESTALE, EIO };
    for (int i = 0; i < 3; i++) {
        mock_ioctl_present_rv = -errs[i];
        errno = 0;
        int rc = gfx_present(h);
        assert_eq(-1, rc);
        assert_eq(errs[i], errno);
    }
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
    assert_eq(0, mock_mmap_count);
    assert_eq(0, mock_munmap_count);
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

TEST_FUNC(test_ownership_invalid_arguments_no_ioctl)
{
    TEST_SUITE("ownership: invalid arguments return EINVAL without ioctl");
    reset_mocks();
    gfx_display_state_t st;
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int ioctls_after_open = mock_ioctl_count;

    /* gfx_acquire(NULL) */
    errno = 0;
    int rc = gfx_acquire(NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_release(NULL) */
    errno = 0;
    rc = gfx_release(NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_register_fallback(NULL) */
    errno = 0;
    rc = gfx_register_fallback(NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_get_display_state(NULL, &st) */
    errno = 0;
    rc = gfx_get_display_state(NULL, &st);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_get_display_state(h, NULL) */
    errno = 0;
    rc = gfx_get_display_state(h, NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_replace_fallback(NULL, h) */
    errno = 0;
    rc = gfx_replace_fallback(NULL, h);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_replace_fallback(h, NULL) */
    errno = 0;
    rc = gfx_replace_fallback(h, NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* gfx_replace_fallback(NULL, NULL) */
    errno = 0;
    rc = gfx_replace_fallback(NULL, NULL);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    gfx_close(h);
}

TEST_FUNC(test_ownership_no_arg_commands_explicit_zero_arg)
{
    TEST_SUITE("ownership: no-arg commands pass explicit (uint64_t)0");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int idx = mock_ioctl_count;

    /* GFX_ACQUIRE */
    int rc = gfx_acquire(h);
    assert_eq(0, rc);
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq((int)GFX_ACQUIRE, (int)mock_ioctl_log[idx].cmd);
    assert_eq(0ULL, mock_ioctl_log[idx].raw_arg);
    idx++;

    /* GFX_RELEASE */
    rc = gfx_release(h);
    assert_eq(0, rc);
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq((int)GFX_RELEASE, (int)mock_ioctl_log[idx].cmd);
    assert_eq(0ULL, mock_ioctl_log[idx].raw_arg);
    idx++;

    /* GFX_REGISTER_FALLBACK */
    rc = gfx_register_fallback(h);
    assert_eq(0, rc);
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq((int)GFX_REGISTER_FALLBACK, (int)mock_ioctl_log[idx].cmd);
    assert_eq(0ULL, mock_ioctl_log[idx].raw_arg);

    gfx_close(h);
}

TEST_FUNC(test_ownership_success_preserves_errno)
{
    TEST_SUITE("ownership: success preserves caller errno");
    reset_mocks();
    gfx_handle_t *h1 = gfx_open(0, 0, 4, 4);
    gfx_handle_t *h2 = gfx_open(0, 0, 4, 4);
    assert_not_null(h1);
    assert_not_null(h2);

    /* acquire */
    errno = ENOENT;
    int rc = gfx_acquire(h1);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);

    /* release */
    errno = ENOENT;
    rc = gfx_release(h1);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);

    /* register_fallback */
    errno = ENOENT;
    rc = gfx_register_fallback(h1);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);

    /* get_display_state */
    gfx_display_state_t st;
    errno = ENOENT;
    rc = gfx_get_display_state(h1, &st);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);

    /* replace_fallback */
    errno = ENOENT;
    rc = gfx_replace_fallback(h1, h2);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);

    gfx_close(h1);
    gfx_close(h2);
}

TEST_FUNC(test_replace_fallback_arguments_and_reserved)
{
    TEST_SUITE("replace_fallback: old/new FD routing and reserved=0");
    reset_mocks();
    gfx_handle_t *old_h = gfx_open(0, 0, 4, 4);
    gfx_handle_t *new_h = gfx_open(0, 0, 4, 4);
    assert_not_null(old_h);
    assert_not_null(new_h);
    /* old_h got first open (fd 7), new_h got second open (fd 8) */
    int old_fd = 7;
    int new_fd = 8;

    int idx = mock_ioctl_count;
    int rc = gfx_replace_fallback(old_h, new_h);
    assert_eq(0, rc);
    assert_eq(idx + 1, mock_ioctl_count);

    /* Verify ioctl was called on new_h->fd */
    assert_eq((int)GFX_REPLACE_FALLBACK, (int)mock_ioctl_log[idx].cmd);
    assert_eq(new_fd, mock_ioctl_log[idx].fd);

    /* Verify payload carries old_h->fd and reserved is 0 */
    assert_eq(old_fd, mock_ioctl_replace_fallback_passed.old_fd);
    assert_eq(0u, mock_ioctl_replace_fallback_passed.reserved);

    gfx_close(old_h);
    gfx_close(new_h);
}

TEST_FUNC(test_ownership_error_propagation)
{
    TEST_SUITE("ownership: ioctl errors propagated unchanged");
    reset_mocks();
    gfx_handle_t *h1 = gfx_open(0, 0, 4, 4);
    gfx_handle_t *h2 = gfx_open(0, 0, 4, 4);
    assert_not_null(h1);
    assert_not_null(h2);

    /* acquire: EBUSY */
    mock_ioctl_acquire_rv = -EBUSY;
    errno = 0;
    int rc = gfx_acquire(h1);
    assert_eq(-1, rc);
    assert_eq(EBUSY, errno);

    /* release: EINVAL */
    mock_ioctl_release_rv = -EINVAL;
    errno = 0;
    rc = gfx_release(h1);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);

    /* register_fallback: EBUSY */
    mock_ioctl_register_fallback_rv = -EBUSY;
    errno = 0;
    rc = gfx_register_fallback(h1);
    assert_eq(-1, rc);
    assert_eq(EBUSY, errno);

    /* get_display_state: EINVAL */
    mock_ioctl_get_display_state_rv = -EINVAL;
    gfx_display_state_t st;
    errno = 0;
    rc = gfx_get_display_state(h1, &st);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);

    /* replace_fallback: EBADF */
    mock_ioctl_replace_fallback_rv = -EBADF;
    errno = 0;
    rc = gfx_replace_fallback(h1, h2);
    assert_eq(-1, rc);
    assert_eq(EBADF, errno);

    gfx_close(h1);
    gfx_close(h2);
}

TEST_FUNC(test_get_display_state_populates_output)
{
    TEST_SUITE("get_display_state: populates output struct");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);

    mock_ioctl_display_state_value.generation = 42;
    mock_ioctl_display_state_value.flags =
        GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;
    mock_ioctl_display_state_value.reserved = 0;

    gfx_display_state_t st;
    memset(&st, 0xff, sizeof(st));
    int rc = gfx_get_display_state(h, &st);
    assert_eq(0, rc);
    assert_eq(42ULL, st.generation);
    assert_eq((unsigned)(GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK),
              st.flags);
    assert_eq(0u, st.reserved);

    gfx_close(h);
}

/* Task 2: GFX_PRESENT_RECT client wrapper.
 *
 * Spec (docs/superpowers/specs/2026-10-12-terminal-partial-present-design.md)
 * §4: libgfx exposes `int gfx_present_rect(handle, x, y, w, h_)`, validates
 * inputs up-front with subtraction-form geometry, returns -1/EINVAL
 * without ioctl on bad input, and re-forms the UAPI request on every
 * call (no user-pointer caching).  Full-view rectangles must call the
 * old gfx_present (one GFX_PRESENT ioctl); other valid rectangles must
 * call the new ioctl (one GFX_PRESENT_RECT ioctl).  Successful calls
 * must NOT modify errno; failed calls must propagate the ioctl errno
 * (EFAULT/EACCES/EAGAIN/ESTALE/EIO/ENOTTY/...) verbatim.
 *
 * Test plan:
 *   - The handle is a 4×4 view, so stride = 16 and the full-view rect
 *     is exactly (0,0,4,4).  Req fields pins the source pixels, the
 *     stride = view_w*4, reserved=0, and the exact (x,y,w,h_) passed.
 *   - gfx_set_clip has no influence on the present_rect request (the
 *     clip is library-local drawing state per spec §4).
 *   - NULL handle / zero w / zero h_ / out-of-range / UINT32_MAX w or
 *     h_ all return -1/EINVAL with mock_ioctl_count unchanged.
 *   - Full-view (0,0,4,4) issues a single GFX_PRESENT (NOT
 *     GFX_PRESENT_RECT) ioctl.
 *   - Pre-set errno=EDOM is preserved across a successful
 *     gfx_present_rect call.
 *   - mocked ioctl return values -EACCES/-EAGAIN/-ESTALE/-EIO/-ENOTTY
 *     propagate verbatim (rc=-1, errno=the same value). */
TEST_FUNC(test_present_rect_request_and_errno)
{
    TEST_SUITE("present_rect: request fields, errno, full-view dispatch");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);

    /* Snapshot the handle's pixels pointer and stride; the wrapper
     * must place these into the UAPI request verbatim on every call
     * (no caching). */
    const struct gfx_handle *internal = (const struct gfx_handle *)h;
    uint64_t expected_pixels = (uint64_t)(uintptr_t)internal->pixels;
    assert_true(expected_pixels != 0u);
    uint32_t expected_stride = 4u * 4u;  /* view_w * 4 */

    /* ── Partial-rect happy path: source/stride/reserved/rect/ioctl count ── */
    int idx = mock_ioctl_count;
    errno = 0;
    int rc = gfx_present_rect(h, 1u, 1u, 2u, 1u);
    assert_eq(0, rc);
    /* Exactly ONE new ioctl and it is the new GFX_PRESENT_RECT command. */
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq((int)GFX_PRESENT_RECT, (int)mock_ioctl_log[idx].cmd);

    gfx_present_rect_req_t *req =
        (gfx_present_rect_req_t *)mock_ioctl_log[idx].arg;
    assert_not_null(req);
    /* Source base == handle's full pixels buffer. */
    assert_eq((long)expected_pixels, (long)req->pixels);
    /* Stride == view_w * 4. */
    assert_eq((long)expected_stride, (long)req->stride);
    /* reserved MUST be 0. */
    assert_eq(0u, req->reserved);
    /* Rect fields equal what the caller passed. */
    assert_eq(1u, req->x);
    assert_eq(1u, req->y);
    assert_eq(2u, req->w);
    assert_eq(1u, req->h);

    /* ── gfx_set_clip must NOT affect the rect request (clip is drawing-only) ── */
    int clip_idx = mock_ioctl_count;
    gfx_set_clip(h, 0u, 0u, 3u, 3u);  /* different "region" */
    errno = 0;
    rc = gfx_present_rect(h, 2u, 0u, 2u, 1u);
    assert_eq(0, rc);
    assert_eq(clip_idx + 1, mock_ioctl_count);
    gfx_present_rect_req_t *clip_req =
        (gfx_present_rect_req_t *)mock_ioctl_log[clip_idx].arg;
    assert_not_null(clip_req);
    assert_eq(2u, clip_req->x);
    assert_eq(0u, clip_req->y);
    assert_eq(2u, clip_req->w);
    assert_eq(1u, clip_req->h);

    /* ── NULL handle: -1/EINVAL, no ioctl increment ── */
    int before_null = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(NULL, 1u, 1u, 2u, 1u);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(before_null, mock_ioctl_count);

    /* ── zero w: -1/EINVAL, no ioctl increment ── */
    int before_zw = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(h, 1u, 1u, 0u, 1u);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(before_zw, mock_ioctl_count);

    /* ── zero h_: -1/EINVAL, no ioctl increment ── */
    int before_zh = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(h, 1u, 1u, 2u, 0u);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(before_zh, mock_ioctl_count);

    /* ── out-of-range x + w > view_w (subtraction form): -1/EINVAL, no ioctl ── */
    int before_oor = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(h, 3u, 0u, 2u, 1u);  /* x=3+w=2 > view_w=4 */
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(before_oor, mock_ioctl_count);

    /* ── UINT32_MAX w overflow rejected by subtraction-form check ── */
    int before_uw = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(h, 0u, 0u, UINT32_MAX, 1u);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(before_uw, mock_ioctl_count);

    /* ── UINT32_MAX h_ overflow rejected by subtraction-form check ── */
    int before_uh = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(h, 0u, 0u, 1u, UINT32_MAX);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(before_uh, mock_ioctl_count);

    /* ── Full-view rect must dispatch to gfx_present (GFX_PRESENT), NOT
     *    GFX_PRESENT_RECT. The test pins this distinction directly. ── */
    int before_full = mock_ioctl_count;
    errno = 0;
    rc = gfx_present_rect(h, 0u, 0u, 4u, 4u);  /* == entire 4x4 view */
    assert_eq(0, rc);
    assert_eq(before_full + 1, mock_ioctl_count);
    assert_eq((int)GFX_PRESENT, (int)mock_ioctl_log[before_full].cmd);
    assert_true((int)GFX_PRESENT_RECT !=
                (int)mock_ioctl_log[before_full].cmd);

    /* ── Success preserves caller errno (wrapper does NOT touch errno). ── */
    errno = EDOM;
    rc = gfx_present_rect(h, 1u, 1u, 2u, 1u);
    assert_eq(0, rc);
    assert_eq(EDOM, errno);

    /* ── Failed-call errno passthrough verbatim. ── */
    const int errs[] = { EACCES, EAGAIN, ESTALE, EIO, ENOTTY };
    for (int i = 0; i < 5; i++) {
        mock_ioctl_present_rect_rv = -errs[i];
        errno = 0;
        rc = gfx_present_rect(h, 1u, 1u, 2u, 1u);
        assert_eq(-1, rc);
        assert_eq(errs[i], errno);
    }
    /* Reset mock so gfx_close (and any incidental cleanup) see success. */
    mock_ioctl_present_rect_rv = 0;

    gfx_close(h);
}

TEST_FUNC(test_bind_input_lifecycle)
{
    TEST_SUITE("bind_input: the lease lifecycle of one view");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);

    /* The kernel's answer: a lease generation and the modifier
     * snapshot (bit0 = Caps Lock). */
    mock_ioctl_bind_value.generation = 7;
    mock_ioctl_bind_value.initial_modifiers = 1;

    int idx = mock_ioctl_count;
    gfx_input_state_t state;
    memset(&state, 0, sizeof(state));
    errno = ENOENT;
    int rc = gfx_bind_input(h, 3, 4, &state);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq((int)GFX_BIND_INPUT, (int)mock_ioctl_log[idx].cmd);
    assert_eq(7, mock_ioctl_log[idx].fd);

    /* The descriptors travel with the request ... */
    assert_eq(3, mock_ioctl_bind_passed.keyboard_fd);
    assert_eq(4, mock_ioctl_bind_passed.mouse_fd);
    /* ... and the input result fields are 0 on entry. */
    assert_eq(0, mock_ioctl_bind_passed.flags);
    assert_eq(0, mock_ioctl_bind_passed.reserved);
    assert_eq(0, mock_ioctl_bind_passed.generation);
    assert_eq(0, mock_ioctl_bind_passed.initial_modifiers);
    assert_eq(0, mock_ioctl_bind_passed.reserved2);

    /* The lease result lands in the caller's state. */
    assert_eq(7, (long)state.generation);
    assert_eq(1, (long)state.initial_modifiers);
    assert_eq(0, (long)state.reserved);

    /* A keyboard-only lease (mouse_fd = -1) is a legal request. */
    idx = mock_ioctl_count;
    memset(&state, 0, sizeof(state));
    rc = gfx_bind_input(h, 3, -1, &state);
    assert_eq(0, rc);
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq(3, mock_ioctl_bind_passed.keyboard_fd);
    assert_eq(-1, mock_ioctl_bind_passed.mouse_fd);

    /* The release closes the lease of THIS view only. */
    idx = mock_ioctl_count;
    errno = ENOENT;
    rc = gfx_unbind_input(h);
    assert_eq(0, rc);
    assert_eq(ENOENT, errno);
    assert_eq(idx + 1, mock_ioctl_count);
    assert_eq((int)GFX_UNBIND_INPUT, (int)mock_ioctl_log[idx].cmd);
    assert_eq(7, mock_ioctl_log[idx].fd);

    gfx_close(h);
}

TEST_FUNC(test_bind_input_errors_and_arguments)
{
    TEST_SUITE("bind_input: argument checks and errno passthrough");
    reset_mocks();
    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int ioctls_after_open = mock_ioctl_count;

    gfx_input_state_t state;
    memset(&state, 0, sizeof(state));

    /* A NULL handle and a NULL out_state are refused without an
     * ioctl. */
    errno = 0;
    assert_eq(-1, gfx_bind_input(NULL, 3, 4, &state));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gfx_bind_input(h, 3, 4, NULL));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gfx_unbind_input(NULL));
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    /* The kernel's errno reaches the caller unchanged. */
    const int errs[] = { EACCES, ESTALE, EBUSY, EINVAL, EFAULT, EBADF };
    for (int i = 0; i < 6; i++) {
        mock_ioctl_bind_rv = -errs[i];
        errno = 0;
        assert_eq(-1, gfx_bind_input(h, 3, 4, &state));
        assert_eq(errs[i], errno);
    }
    mock_ioctl_bind_rv = 0;

    mock_ioctl_unbind_rv = -EIO;
    errno = 0;
    assert_eq(-1, gfx_unbind_input(h));
    assert_eq(EIO, errno);

    gfx_close(h);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_open_happy_path),
    TEST_ENTRY(test_bind_input_lifecycle),
    TEST_ENTRY(test_bind_input_errors_and_arguments),
    TEST_ENTRY(test_open_enoint_normalized_to_enodev),
    TEST_ENTRY(test_open_enfile_is_preserved),
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
    TEST_ENTRY(test_present_propagates_switch_errnos),
    TEST_ENTRY(test_close_null_is_safe),
    TEST_ENTRY(test_present_req_pixel_pointer_is_buffer),
    TEST_ENTRY(test_ownership_invalid_arguments_no_ioctl),
    TEST_ENTRY(test_ownership_no_arg_commands_explicit_zero_arg),
    TEST_ENTRY(test_ownership_success_preserves_errno),
    TEST_ENTRY(test_replace_fallback_arguments_and_reserved),
    TEST_ENTRY(test_ownership_error_propagation),
    TEST_ENTRY(test_get_display_state_populates_output),
    TEST_ENTRY(test_present_rect_request_and_errno),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

