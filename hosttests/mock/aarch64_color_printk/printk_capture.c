/* hosttests/mock/aarch64_color_printk/printk_capture.c
 *
 * Host capture/stub layer for test_aarch64_color_printk_abi, which links
 * the REAL production kernel/core/printk.c (the shared console used by
 * both x86_64 and aarch64; see kernel/Makefile's aarch64 KERNEL_C_SOURCES).
 *
 * The production TU references a handful of symbols the host harness does
 * not otherwise provide:
 *
 *   - serial_lock / write_serial_unlocked: serial_printk's sink.  Here
 *     write_serial_unlocked appends to a buffer the test reads back.
 *   - fb_writer_begin / fb_writer_end: the x86_64 framebuffer writer-lease
 *     surface (PRINTK_FB_WRITER_LEASE==1 when host-compiling on x86_64).
 *     fb_writer_begin returns -1 ("no framebuffer mapped yet"), so
 *     color_printk takes its serial-only fallback path — the same
 *     observable behaviour as an aarch64 boot before frame_buffer_init().
 *   - _binary_kernel_font_psf_*: the embedded PSF font blob.  Only its
 *     address is taken; it is never read because the unmapped framebuffer
 *     makes putchark_snap() return early.
 */
#include <stddef.h>
#include <stdint.h>

#include <driver/serial.h>     /* shadow: serial_lock, write_serial_unlocked */
#include <driver/fb_state.h>   /* real: fb_lease_t, fb_writer_begin/end  */

/* ── serial sink ────────────────────────────────────────────────── */
spinlock_T serial_lock;

static char   s_serial[1024];
static size_t s_serial_len;

void write_serial_unlocked(char c)
{
    if (s_serial_len + 1 < sizeof(s_serial))
        s_serial[s_serial_len++] = c;
    s_serial[s_serial_len] = '\0';
}

void mock_serial_reset(void)
{
    s_serial_len = 0;
    s_serial[0] = '\0';
}

const char *mock_serial_buf(void)
{
    return s_serial;
}

size_t mock_serial_len(void)
{
    return s_serial_len;
}

/* ── framebuffer writer-lease: no framebuffer in the harness ────── */
int fb_writer_begin(fb_lease_t *lease, uint64_t expected_generation)
{
    (void)lease;
    (void)expected_generation;
    return -1;   /* take the serial-only fallback path */
}

void fb_writer_end(fb_lease_t *lease)
{
    (void)lease;
}

/* ── embedded font blob (address only) ──────────────────────────── */
volatile unsigned char _binary_kernel_font_psf_start[64];
volatile unsigned char _binary_kernel_font_psf_end[1];
