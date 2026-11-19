#ifndef _UAPI_GFX_H
#define _UAPI_GFX_H

#include <stddef.h>      /* offsetof for ABI static_asserts */
#include <stdint.h>

/* /dev/gfx0 — 2D graphics view UAPI.
 *
 * All structs are fixed-width and must NOT change field order or
 * size (libgfx and the kernel build against this header).  reserved
 * fields must be transmitted as 0; the kernel returns EINVAL when
 * they are not.  Every ioctl argument is either copied in/out via
 * copy_{from,to}_user_ft or staged line-by-line through kernel
 * bounce (never dereferenced directly). */

#define GFX_FORMAT_RGB32 0u

/* ioctl numbers — see kernel/fs/devfs.c devfs_ioctl_node dispatch. */
#define GFX_CREATE_VIEW  0x4701
#define GFX_GET_INFO     0x4702
#define GFX_PRESENT      0x4703
#define GFX_ACQUIRE           0x4704
#define GFX_RELEASE           0x4705
#define GFX_REGISTER_FALLBACK 0x4706
#define GFX_GET_DISPLAY_STATE 0x4707
#define GFX_REPLACE_FALLBACK  0x4708
/* Terminal partial-present plan (Task 1): leased partial-rectangle
 * present.  View-local coordinates; pixels points to the full view's
 * private buffer; stride MUST equal view_w*4; reserved=0.  See
 * docs/superpowers/specs/2026-10-12-terminal-partial-present-design.md
 * §4 (UAPI) + §5 (kernel execution) for the contract. */
#define GFX_PRESENT_RECT 0x4709
/* GUI-2 plan Task 4: the physical-input lease.  GFX_BIND_INPUT binds
 * the caller's keyboard / mouse file descriptors to the caller's
 * DISPLAY VIEW, so the lease lives and dies with the display
 * ownership.  GFX_UNBIND_INPUT releases the caller's own lease. */
#define GFX_BIND_INPUT   0x470A
#define GFX_UNBIND_INPUT 0x470B

#define GFX_DISPLAY_STATE_FOREGROUND (1u << 0)
#define GFX_DISPLAY_STATE_FALLBACK   (1u << 1)

/* GFX_GET_DISPLAY_STATE — output; queries current ownership generation and
 * foreground/fallback role flags for the caller's view cookie. reserved is 0. */
typedef struct {
    uint64_t generation;
    uint32_t flags;
    uint32_t reserved;
} gfx_display_state_t;

_Static_assert(sizeof(gfx_display_state_t) == 16,
               "gfx_display_state_t must be 16 bytes (ABI)");

/* GFX_REPLACE_FALLBACK — input; atomic fallback transfer from old_fd to the
 * calling handle. reserved must be 0. */
typedef struct {
    int32_t old_fd;
    uint32_t reserved;
} gfx_replace_fallback_t;

_Static_assert(sizeof(gfx_replace_fallback_t) == 8,
               "gfx_replace_fallback_t must be 8 bytes (ABI)");

/* GFX_CREATE_VIEW — input; describes the full-screen rectangle the
 * caller wants the kernel to render into.  x, y are full-screen
 * coordinates; w, h must fit within the framebuffer and not overflow
 * when multiplied (kernel checks `x <= fb_w && w <= fb_w - x`). */
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} gfx_view_desc_t;

_Static_assert(sizeof(gfx_view_desc_t) == 16,
               "gfx_view_desc_t must be 16 bytes (ABI)");

/* GFX_GET_INFO — output; describes the configured view's local
 * coordinate frame.  stride is `width * 4` in bytes; format is
 * GFX_FORMAT_RGB32 today. */
typedef struct {
    uint32_t width;       /* configured view's local width  */
    uint32_t height;      /* configured view's local height */
    uint32_t stride;      /* bytes per scanline = width * 4 */
    uint32_t format;      /* GFX_FORMAT_RGB32 */
} gfx_info_t;

_Static_assert(sizeof(gfx_info_t) == 16,
               "gfx_info_t must be 16 bytes (ABI)");

/* GFX_PRESENT — input; user supplies the framebuffer pointer and
 * stride for this present.  The kernel never caches the pointer;
 * the next call must re-supply it.  reserved must be 0. */
typedef struct {
    uint64_t pixels;      /* this-process user pointer; per-call */
    uint32_t stride;      /* must equal info.stride */
    uint32_t reserved;    /* must be 0 */
} gfx_present_req_t;

_Static_assert(sizeof(gfx_present_req_t) == 16,
               "gfx_present_req_t must be 16 bytes (ABI)");

/* Diagnostic only: enabled by -DOS01_TEST_GFX_PRESENT_AB in the kernel.
 * wide=0 uses the normal writer; wide=1 uses aligned 32-bit stores.
 * Timings are outputs and cover the two per-row stages, summed over a frame. */
#define GFX_TEST_PRESENT_AB 0x470C
typedef struct {
    gfx_present_req_t present;
    uint32_t wide;
    uint32_t reserved;
    uint64_t copy_ns;
    uint64_t fb_ns;
} gfx_test_present_ab_t;

_Static_assert(sizeof(gfx_test_present_ab_t) == 40, "gfx_test_present_ab_t ABI");

/* GFX_PRESENT_RECT — input; partial-rectangle present (spec §4).
 * Coordinates are VIEW-LOCAL pixels; the framebuffer destination is
 * view origin + (x, y).  pixels is the FULL view private buffer
 * base, NOT a packed rect — stride MUST equal view_w*4.  reserved
 * must be 0.  Per-row copy length is w*4; per-row source address is
 * pixels + (y+i)*stride + x*4.  Drawing-side clip does not affect
 * the present request (no implicit clipping, no fallback on errors). */
typedef struct {
    uint64_t pixels;      /* this-process user pointer; full-view base */
    uint32_t stride;      /* must equal view_w * 4 */
    uint32_t reserved;    /* must be 0 */
    uint32_t x;           /* view-local start column */
    uint32_t y;           /* view-local start row */
    uint32_t w;           /* width in pixels (must be > 0) */
    uint32_t h;           /* height in pixels (must be > 0) */
} gfx_present_rect_req_t;

/* Field offsets are part of the ABI — Task 2's libgfx wrapper mirrors
 * the same struct (and pins the offsets through <uapi/gfx.h> which
 * libgfx already includes from sysroot).  Bumping any field is a
 * breaking UAPI change. */
_Static_assert(sizeof(gfx_present_rect_req_t) == 32,
               "gfx_present_rect_req_t must be 32 bytes (ABI)");
_Static_assert(offsetof(gfx_present_rect_req_t, pixels)   == 0,  "pixels offset");
_Static_assert(offsetof(gfx_present_rect_req_t, stride)   == 8,  "stride offset");
_Static_assert(offsetof(gfx_present_rect_req_t, reserved) == 12, "reserved offset");
_Static_assert(offsetof(gfx_present_rect_req_t, x)       == 16, "x offset");
_Static_assert(offsetof(gfx_present_rect_req_t, y)       == 20, "y offset");
_Static_assert(offsetof(gfx_present_rect_req_t, w)       == 24, "w offset");
_Static_assert(offsetof(gfx_present_rect_req_t, h)       == 28, "h offset");

/* GFX_PRESENT_RECTS — input; damage-region BATCH present (roadmap P3
 * 脏矩形与批量提交, spec
 * docs/superpowers/specs/2026-11-19-gfx-damage-batch-present-design.md).
 * One ioctl submits up to GFX_PRESENT_RECTS_MAX view-local rectangles
 * from ONE full-view buffer, replacing the terminal's per-rect
 * GFX_PRESENT_RECT round.  Per-rect semantics are exactly those of
 * GFX_PRESENT_RECT: view-local coordinates, destination = view origin
 * + (x, y), stride MUST equal view_w*4, no implicit clipping, drawing
 * clip does not affect the request.  Rects are processed in array
 * order and may overlap (later wins).  ANY invalid rect rejects the
 * WHOLE batch with zero framebuffer writes.
 *
 * completed_rects is IN/OUT: the caller must transmit 0 (else EINVAL);
 * on success the kernel writes back count; on failure it writes back
 * the number of rects FULLY completed before the failure point (the
 * faulting rect's partial rows stay unknown).  The write-back is
 * best-effort: if it faults, the ioctl's return value and errno are
 * unchanged and userspace must treat k as unknown (0). */
#define GFX_PRESENT_RECTS      0x470D
#define GFX_PRESENT_RECTS_MAX  64u   /* aligns with TERM_DAMAGE_MAX_RECTS */

typedef struct {
    uint32_t x;           /* view-local start column */
    uint32_t y;           /* view-local start row */
    uint32_t w;           /* width in pixels (must be > 0) */
    uint32_t h;           /* height in pixels (must be > 0) */
} gfx_rect_t;

_Static_assert(sizeof(gfx_rect_t) == 16, "gfx_rect_t must be 16 bytes (ABI)");

typedef struct {
    uint64_t pixels;          /* in: this-process user pointer; FULL view base */
    uint32_t stride;          /* in: must equal view_w * 4 */
    uint32_t reserved;        /* in: must be 0 */
    uint64_t rects;           /* in: user pointer to gfx_rect_t[count] */
    uint32_t count;           /* in: 1..GFX_PRESENT_RECTS_MAX */
    uint32_t completed_rects; /* in: 0; out: rects fully written pre-failure */
} gfx_present_rects_req_t;

_Static_assert(sizeof(gfx_present_rects_req_t) == 32,
               "gfx_present_rects_req_t must be 32 bytes (ABI)");
_Static_assert(offsetof(gfx_present_rects_req_t, pixels)          == 0,  "pixels offset");
_Static_assert(offsetof(gfx_present_rects_req_t, stride)          == 8,  "stride offset");
_Static_assert(offsetof(gfx_present_rects_req_t, reserved)        == 12, "reserved offset");
_Static_assert(offsetof(gfx_present_rects_req_t, rects)           == 16, "rects offset");
_Static_assert(offsetof(gfx_present_rects_req_t, count)           == 24, "count offset");
_Static_assert(offsetof(gfx_present_rects_req_t, completed_rects) == 28, "completed_rects offset");

/* GFX_BIND_INPUT — INOUT; binds the physical-input lease to the
 * calling view.  The whole struct must be readable AND writable; the
 * input result fields (generation / initial_modifiers) must be 0 on
 * entry and are written back on success.
 *
 * keyboard_fd   the caller's /dev/keyboard descriptor (required).
 * mouse_fd      the caller's /dev/mouse descriptor; -1 means the
 *               keyboard only.  Every other negative value is a bad
 *               descriptor (-EBADF).
 * flags         v1 must be 0.
 * reserved      must be 0.
 * generation    in: 0; out: the lease generation (non-zero, monotonic).
 * initial_modifiers
 *               in: 0; out: bit0 = Caps Lock, every other bit 0.
 * reserved2     in: 0; out: 0.
 *
 * The errno the bind can return:
 *   EFAULT          the struct is not readable and writable.
 *   EBADF           a descriptor is out of range or not open.
 *   EINVAL          the descriptor is not readable / not the input
 *                   device, a reserved or result field is not 0, or
 *                   the view is not configured.
 *   EACCES          the caller is not the display owner.
 *   ESTALE          the view is stale (the display moved on).
 *   EBUSY           a lease already exists.
 *   EAGAIN          the display is not initialised yet.
 *   EIO             the display backend has failed.
 *   ENODEV          this kernel has no input route. */
typedef struct {
    int32_t  keyboard_fd;
    int32_t  mouse_fd;
    uint32_t flags;
    uint32_t reserved;
    uint64_t generation;
    uint32_t initial_modifiers;
    uint32_t reserved2;
} gfx_bind_input_t;

/* Field offsets are part of the ABI (spec §4.1) — the libgfx wrapper
 * and every client build against this header. */
_Static_assert(sizeof(gfx_bind_input_t) == 32,
               "gfx_bind_input_t must be 32 bytes (ABI)");
_Static_assert(offsetof(gfx_bind_input_t, keyboard_fd)      == 0,  "keyboard_fd offset");
_Static_assert(offsetof(gfx_bind_input_t, mouse_fd)         == 4,  "mouse_fd offset");
_Static_assert(offsetof(gfx_bind_input_t, flags)            == 8,  "flags offset");
_Static_assert(offsetof(gfx_bind_input_t, reserved)         == 12, "reserved offset");
_Static_assert(offsetof(gfx_bind_input_t, generation)       == 16, "generation offset");
_Static_assert(offsetof(gfx_bind_input_t, initial_modifiers) == 24, "initial_modifiers offset");
_Static_assert(offsetof(gfx_bind_input_t, reserved2)        == 28, "reserved2 offset");

#endif /* _UAPI_GFX_H */
