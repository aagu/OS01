// kernel/arch/aarch64/runtime/printk_fb.c — aarch64 kernel-side console
// implementation (spec 2026-10-06).
//
// On aarch64, kernel/core/printk.c is NOT compiled (it pulls in x86-isms
// like driver/serial.c and an x86-64-only font embed, and depends on libc
// symbols not linked into the aarch64 kernel).  This TU provides the
// arch-side equivalent: it owns the aarch64 `Pos` global, an inline PSF
// font reference (via the lld-generated `_binary_kernel_font_psf_*`
// symbols that the kernel/Makefile produces for both arches), and the
// `color_printk` / `putchark` / `putchar_at` implementations.
//
// Differences from kernel/core/printk.c:
//   - color_printk iterates the format string verbatim (no %d/%s/etc.
//     substitution; the aarch64 kernel has no vsprintf).  This matches
//     the long-standing design (see kernel/arch/aarch64/runtime/
//     serial_printk.c, which does the same for serial_printk).
//   - frame_buffer_init uses aarch64_pt_map_* with KERNEL_RW | DEVICE
//     permissions to install the ramfb region.  QEMU aarch64 with
//     `-device ramfb` (run.mk run-aarch64-uefi) wires the FB through
//     edk2 RamfbDxe; the bootloader captures the base/size in
//     boot_context->graphics, which aarch64_boot_fb_init (in
//     kernel/arch/aarch64/boot/main.c) writes into Pos before calling
//     frame_buffer_init().
//   - frame_buffer_early_init is intentionally a no-op: PL011 is the
//     canonical aarch64 boot console, so no early FB map is needed.

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <core/printk.h>                    /* Pos, frame_buffer_init decl */

/* `Pos` lives here on aarch64 (kernel/core/printk.c is not compiled into
 * the aarch64 build). The x86_64 build's Pos definition is in printk.c. */
position Pos;
#include <driver/font.h>                     /* psf2_t                            */
#include <memory/pmm.h>                     /* PAGE_2M_SIZE / PAGE_4K_SIZE       */
#include <arch/aarch64/page_table.h>        /* aarch64_pt_map_2m_block,
                                               aarch64_pt_map_4k_ext,
                                               AARCH64_PT_KERNEL_RW,
                                               AARCH64_PT_DEVICE,
                                               AARCH64_FB_VIRT_BASE,
                                               AARCH64_TTBR_BASE_MASK            */
#include <arch/aarch64/boot_direct_map.h>   /* aarch64_read_ttbr1                 */
#include <arch/aarch64/boot_log.h>          /* kputs                             */
#include <arch/mmu.h>                       /* ARCH_PAGE_OFFSET                  */
#include <arch/spinlock.h>                  /* spinlock_T                        */

// Forward decl: the font is embedded by lld via `ld -r -b binary` (rule
// in kernel/Makefile).  The symbol derives its name from the *path
// string* passed to lld; the Makefile copies the PSF to
// $(KERNEL_BUILD_DIR)/generated/kernel/font.psf and invokes lld from
// that directory so the path string is exactly kernel/font.psf on
// both arches.
extern volatile unsigned char _binary_kernel_font_psf_start;
extern volatile unsigned char _binary_kernel_font_psf_end;

psf2_t *font = (psf2_t*)&_binary_kernel_font_psf_start;

/* ── Frame-buffer drawing (mirror of kernel/core/printk.c::putchark/putchar_at,
 *    adapted for aarch64's MMIO mapping at AARCH64_FB_VIRT_BASE) ──── */

static void draw_glyph(int x, int y, unsigned int FRcolor, unsigned int BKcolor,
                       unsigned char c)
{
    if (!Pos.FB_addr) return;
    const unsigned char *glyph = (const unsigned char *)&_binary_kernel_font_psf_start
                                  + font->headersize
                                  + (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;
    uint32_t base = ((uint32_t)y * font->height) * (uint32_t)Pos.XResolution
                   + (uint32_t)x * font->width;
    for (uint32_t row = 0; row < font->height; row++) {
        uint32_t *line = (uint32_t *)((uintptr_t)Pos.FB_addr
                                      + (uintptr_t)(base + row * Pos.XResolution) * sizeof(uint32_t));
        unsigned char bits = glyph[row];
        for (uint32_t col = 0; col < font->width; col++) {
            line[col] = (bits & 0x80) ? FRcolor : BKcolor;
            bits <<= 1;
        }
    }
}

void putchark(unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    /* NULL-safe: when no framebuffer is configured (boot path without
     * ramfb, or early-boot before Pos is populated), skip the pixel
     * write.  color_printk's outer spin_lock still serialises per-char. */
    if (!Pos.FB_addr) return;

    uint32_t base = ((uint32_t)Pos.YPosition * font->height) * (uint32_t)Pos.XResolution
                   + (uint32_t)Pos.XPosition * font->width;
    const unsigned char *glyph = (const unsigned char *)&_binary_kernel_font_psf_start
                                  + font->headersize
                                  + (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;
    for (uint32_t row = 0; row < font->height; row++) {
        uint32_t *line = (uint32_t *)((uintptr_t)Pos.FB_addr
                                      + (uintptr_t)(base + row * Pos.XResolution) * sizeof(uint32_t));
        unsigned char bits = glyph[row];
        for (uint32_t col = 0; col < font->width; col++) {
            line[col] = (bits & 0x80) ? FRcolor : BKcolor;
            bits <<= 1;
        }
    }
}

void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
    if (!Pos.FB_addr) return;
    int max_rows = (int)(Pos.YResolution / font->height);
    int max_cols = (int)(Pos.XResolution / font->width);
    if (row < 0 || row >= max_rows) return;
    if (col < 0 || col >= max_cols) return;
    draw_glyph(col, row, FRcolor, BKcolor, c);
}

/* ── Frame-buffer init: install the ramfb region with aarch64_pt_map_* ── */

void frame_buffer_early_init(void)
{
    /* no-op: see file-level comment (PL011 is the aarch64 boot console) */
}

void frame_buffer_init(void)
{
    uint64_t pa = (uint64_t)Pos.Phy_addr;
    uint64_t len = Pos.FB_length;
    uint64_t va = AARCH64_FB_VIRT_BASE;
    uint32_t perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE;

    if (pa == 0 || len == 0) {
        kputs("[fb] no-op: Pos.Phy_addr or Pos.FB_length is zero\n");
        Pos.FB_addr = NULL;
        return;
    }

    /* Compute the live TTBR1 root in direct-map form. Mirrors arch_vmm_init
     * in kernel/arch/aarch64/memory/vmm_backend.c. */
    uint64_t ttbr_raw = aarch64_read_ttbr1();
    uint64_t root_pa  = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *root    = (uint64_t *)(uintptr_t)(root_pa + ARCH_PAGE_OFFSET);

    /* Walk [pa, pa+len) using 2 MiB blocks where PA, VA, and the remaining
     * length are all 2 MiB-aligned; fall back to 4 KiB leaves otherwise.
     * aarch64_pt_map_2m_block requires BOTH va and pa to be 2 MiB-aligned
     * (see root_valid / va_canonical in kernel/arch/aarch64/memory/
     * page_table.c); a misaligned head or tail would EINVAL out. */
    uint64_t cur_pa = pa;
    uint64_t cur_va = va;

    while (cur_pa < pa + len) {
        uint64_t this_len = pa + len - cur_pa;
        bool pa_aligned = ((cur_pa & (PAGE_2M_SIZE - 1)) == 0);
        bool va_aligned = ((cur_va & (PAGE_2M_SIZE - 1)) == 0);
        bool big_enough = (this_len >= PAGE_2M_SIZE);

        int rc = 0;
        if (pa_aligned && va_aligned && big_enough) {
            /* Round down to 2 MiB so we don't overrun. */
            uint64_t blocks = this_len / PAGE_2M_SIZE;
            uint64_t off = 0;
            while (off < blocks * PAGE_2M_SIZE) {
                rc = aarch64_pt_map_2m_block(root, cur_va + off, cur_pa + off, perm);
                if (rc != AARCH64_PT_OK) break;
                off += PAGE_2M_SIZE;
            }
            if (rc == AARCH64_PT_OK) {
                cur_pa += blocks * PAGE_2M_SIZE;
                cur_va += blocks * PAGE_2M_SIZE;
            }
            /* On success, fall through to map any remaining tail via 4 KiB
             * leaves. On failure, the outer `if (rc != AARCH64_PT_OK)` at
             * the bottom of the iteration returns immediately. */
        }

        if (rc == AARCH64_PT_OK) {
            /* Tail (or entire segment, if unaligned): 4 KiB leaves. */
            uint64_t tail = pa + len - cur_pa;
            uint64_t off = 0;
            while (off < tail) {
                int r = aarch64_pt_map_4k_ext(root, cur_va + off,
                                              cur_pa + off, perm, 0);
                if (r != AARCH64_PT_OK) { rc = r; break; }
                off += PAGE_4K_SIZE;
            }
            if (rc == AARCH64_PT_OK) {
                cur_pa += tail;
                cur_va += tail;
            }
        }

        if (rc != AARCH64_PT_OK) {
            kputs("[fb] map failed; Pos.FB_addr=NULL, color_printk disabled\n");
            Pos.FB_addr = NULL;
            return;
        }
    }

    Pos.FB_addr = (uint32_t *)AARCH64_FB_VIRT_BASE;
}

/* ── color_printk: format-string iteration without libc vsprintf ──
 *
 * The aarch64 kernel is linked without libc and has no `vsprintf`. We
 * iterate the format string char-by-char (no %d/%s/etc. substitution),
 * mirror of kernel/arch/aarch64/runtime/serial_printk.c's idiom. */

int color_printk(unsigned int FRcolor, unsigned int BKcolor, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    int chars = 0;
    if (!fmt) { va_end(args); return 0; }

    /* NULL-safe: when no framebuffer is configured (frame_buffer_init
     * failed, or BOOT_CONTEXT_HAS_FRAMEBUFFER was never set), skip the
     * entire draw loop.  Without this guard, the scroll trigger would
     * dereference NULL in memmove/memset.  spin_lock is also avoided so a
     * downstream slab.c color_printk call doesn't deadlock if Pos.lock
     * was somehow left uninitialized. */
    if (!Pos.FB_addr) {
        va_end(args);
        return (int)strlen(fmt);
    }

    spin_lock(&Pos.lock);

    for (const char *p = fmt; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            Pos.YPosition++;
            Pos.XPosition = 0;
        } else if (c == '\r') {
            Pos.XPosition = 0;
        } else if (c == '\b') {
            Pos.XPosition--;
            if (Pos.XPosition < 0) {
                Pos.XPosition = (Pos.XResolution / font->width - 1) * font->width;
                Pos.YPosition--;
                if (Pos.YPosition < 0)
                    Pos.YPosition = (Pos.YResolution / font->height - 1) * font->height;
            }
            putchark(FRcolor, BKcolor, ' ');
        } else if (c == '\t') {
            int target = ((Pos.XPosition + 8) & ~(8 - 1)) - Pos.XPosition;
            for (int i = 0; i < target; i++) {
                putchark(FRcolor, BKcolor, ' ');
                Pos.XPosition++;
            }
        } else if (c == '%') {
            /* Minimal printf-style: %u, %d, %x, %X. Other specifiers
             * are rendered literally (mirrors what kernel/core/printk.c
             * gets via vsprintf for unsupported specifiers — keeps the
             * format string usable for the common integer cases without
             * pulling vsprintf into aarch64). */
            char spec = *(++p);
            char numbuf[24];
            int nlen = 0;
            if (spec == 'u') {
                unsigned int v = va_arg(args, unsigned int);
                if (v == 0) numbuf[nlen++] = '0';
                while (v > 0) {
                    numbuf[nlen++] = (char)('0' + v % 10);
                    v /= 10;
                }
            } else if (spec == 'd' || spec == 'i') {
                int v = va_arg(args, int);
                unsigned int mag = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
                if (mag == 0) numbuf[nlen++] = '0';
                while (mag > 0) {
                    numbuf[nlen++] = (char)('0' + mag % 10);
                    mag /= 10;
                }
                if (v < 0) numbuf[nlen++] = '-';
            } else if (spec == 'x' || spec == 'X') {
                unsigned int v = va_arg(args, unsigned int);
                const char *hex = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                if (v == 0) numbuf[nlen++] = '0';
                while (v > 0) {
                    numbuf[nlen++] = hex[v & 0xF];
                    v >>= 4;
                }
            } else if (spec == 'l') {
                /* %lu / %ld / %lx / %lX — long variants used by slab.c
                 * and other kernel callers. Promote to uintmax_t for
                 * uniform handling; 64-bit aarch64 makes this exact. */
                char subspec = *(++p);
                uintmax_t v = va_arg(args, uintmax_t);
                if (subspec == 'u') {
                    if (v == 0) numbuf[nlen++] = '0';
                    while (v > 0) {
                        numbuf[nlen++] = (char)('0' + v % 10);
                        v /= 10;
                    }
                } else if (subspec == 'd' || subspec == 'i') {
                    intmax_t sv = (intmax_t)v;
                    uintmax_t mag = (sv < 0) ? (uintmax_t)(-sv) : (uintmax_t)sv;
                    if (mag == 0) numbuf[nlen++] = '0';
                    while (mag > 0) {
                        numbuf[nlen++] = (char)('0' + mag % 10);
                        mag /= 10;
                    }
                    if (sv < 0) numbuf[nlen++] = '-';
                } else if (subspec == 'x' || subspec == 'X') {
                    const char *hex = (subspec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                    if (v == 0) numbuf[nlen++] = '0';
                    while (v > 0) {
                        numbuf[nlen++] = hex[v & 0xF];
                        v >>= 4;
                    }
                } else {
                    numbuf[nlen++] = '%';
                    numbuf[nlen++] = 'l';
                    numbuf[nlen++] = subspec;
                }
            } else if (spec == 'p') {
                /* %p — pointer. Render as "0x" + lowercase hex of the
                 * pointer value. Cast through uintptr_t to silence any
                 * -Wpedantic about integer/pointer conversion. */
                uintptr_t v = (uintptr_t)va_arg(args, void *);
                numbuf[nlen++] = '0';
                numbuf[nlen++] = 'x';
                if (v == 0) numbuf[nlen++] = '0';
                while (v > 0) {
                    numbuf[nlen++] = "0123456789abcdef"[v & 0xF];
                    v >>= 4;
                }
            } else if (spec == '0') {
                /* Width / zero-pad prefix (e.g. %08d, %08lu). Parse the
                 * decimal width and recurse on the spec after '0' so
                 * the existing %u / %d / %lu paths handle the value.
                 * We don't honour left-adjust / '+' / ' '/ '#', which
                 * the kernel's color_printk callers don't use today. */
                int width = 0;
                while (p[1] >= '0' && p[1] <= '9') {
                    width = width * 10 + (*(++p) - '0');
                }
                /* Emit width - leading-zeros (we don't actually know the
                 * post-format length yet). For caller simplicity, fall
                 * through to the spec after the digits, which goes
                 * through the normal %u/%d path; leading zeros are a
                 * best-effort. */
                char spec_after = *(++p);
                (void)width; /* suppress unused warning; leading zeros are
                               best-effort only. */
                /* Push back: re-handle `spec_after` as a fresh format
                 * character by recursing once. */
                numbuf[nlen++] = '%';
                for (int w = 0; w < width; w++) numbuf[nlen++] = '0';
                if (spec_after == 'd' || spec_after == 'i') {
                    int v = va_arg(args, int);
                    unsigned int mag = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
                    if (mag == 0) numbuf[nlen++] = '0';
                    while (mag > 0) { numbuf[nlen++] = (char)('0' + mag % 10); mag /= 10; }
                    if (v < 0) numbuf[nlen++] = '-';
                } else if (spec_after == 'u') {
                    unsigned int v = va_arg(args, unsigned int);
                    if (v == 0) numbuf[nlen++] = '0';
                    while (v > 0) { numbuf[nlen++] = (char)('0' + v % 10); v /= 10; }
                } else if (spec_after == 'l') {
                    char subspec = *(++p);
                    uintmax_t v = va_arg(args, uintmax_t);
                    if (subspec == 'u') {
                        if (v == 0) numbuf[nlen++] = '0';
                        while (v > 0) { numbuf[nlen++] = (char)('0' + v % 10); v /= 10; }
                    } else if (subspec == 'd' || subspec == 'i') {
                        intmax_t sv = (intmax_t)v;
                        uintmax_t mag = (sv < 0) ? (uintmax_t)(-sv) : (uintmax_t)sv;
                        if (mag == 0) numbuf[nlen++] = '0';
                        while (mag > 0) { numbuf[nlen++] = (char)('0' + mag % 10); mag /= 10; }
                        if (sv < 0) numbuf[nlen++] = '-';
                    } else {
                        numbuf[nlen++] = 'l'; numbuf[nlen++] = subspec;
                    }
                } else if (spec_after == 'p') {
                    uintptr_t v = (uintptr_t)va_arg(args, void *);
                    numbuf[nlen++] = '0'; numbuf[nlen++] = 'x';
                    if (v == 0) numbuf[nlen++] = '0';
                    while (v > 0) {
                        numbuf[nlen++] = "0123456789abcdef"[v & 0xF]; v >>= 4;
                    }
                } else {
                    numbuf[nlen++] = spec_after;
                }
            } else {
                /* Unknown specifier: emit literally. */
                numbuf[nlen++] = '%';
                numbuf[nlen++] = spec;
            }
            /* Emit digits in correct order (reversed from how we built). */
            for (int i = nlen - 1; i >= 0; i--) {
                putchark(FRcolor, BKcolor, (unsigned char)numbuf[i]);
                Pos.XPosition++;
                chars++;
                if (Pos.XPosition >= (int32_t)(Pos.XResolution / font->width)) {
                    Pos.YPosition++;
                    Pos.XPosition = 0;
                }
            }
            continue;
        } else {
            putchark(FRcolor, BKcolor, c);
            Pos.XPosition++;
        }

        if (Pos.XPosition >= (int32_t)(Pos.XResolution / font->width)) {
            Pos.YPosition++;
            Pos.XPosition = 0;
        }
        if (Pos.YPosition >= (int32_t)(Pos.YResolution / font->height)) {
            /* Scroll up one char row. */
            int rows = (int)(Pos.YResolution / font->height);
            uint32_t pitch = (uint32_t)Pos.XResolution * sizeof(uint32_t);
            int row_bytes = (int)(pitch * font->height);
            uint8_t *fb = (uint8_t *)Pos.FB_addr;
            memmove(fb, fb + row_bytes, (uintptr_t)row_bytes * (rows - 1));
            memset(fb + (uintptr_t)row_bytes * (rows - 1), 0, (uintptr_t)row_bytes);
            Pos.YPosition = rows - 1;
        }
        chars++;
    }

    spin_unlock(&Pos.lock);
    va_end(args);
    return chars;
}
