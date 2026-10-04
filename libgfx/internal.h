/* libgfx/internal.h — SHARED PRIVATE HEADER (never installed to sysroot).
 *
 * The struct gfx_handle definition lives ONLY here, so every .c inside
 * libgfx/ sees the same layout but the public libgfx/gfx.h keeps the
 * handle fully opaque.  This header is force-included by libgfx/gfx.c
 * via the Makefile rule's -I$(LIBGFX_DIR) — there is no separate
 * #include "internal.h" line in the .c files because the prototype is
 * small and the dependency is local.  Future Task 4 primitives
 * (line.c / sprite.c) will add their own private state through this
 * header too.
 *
 * Rule: this header NEVER ships to the sysroot.  mk/components/sysroot.mk
 * stages libgfx via the `usr/include/gfx.h` + `usr/lib/libgfx.a` manifest
 * entries; nothing else. */
#ifndef _LIBGFX_INTERNAL_H
#define _LIBGFX_INTERNAL_H

#include "gfx.h"
#include <stdint.h>
#include <stddef.h>

struct gfx_handle {
    int         fd;          /* /dev/gfx0 */
    gfx_info_t  info;        /* GFX_GET_INFO snapshot from gfx_open */
    uint32_t   *pixels;      /* private buffer, width*height*4 bytes, zeroed */
    size_t      pixels_bytes;/* byte length of mmap'd pixels buffer */
    int32_t     clip_x;      /* library-local clip (spec §4) */
    int32_t     clip_y;
    uint32_t    clip_w;
    uint32_t    clip_h;
};

#endif /* _LIBGFX_INTERNAL_H */
