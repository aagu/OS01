#include <core/printk.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <arch/spinlock.h>
#include <driver/serial.h>
#include <stdio.h>
#include <driver/font.h>
#include <stddef.h>
#include <string.h>

// Separate per-function buffers — color_printk and serial_printk share
// a single global buffer, which is a race condition: int $0x80 uses a
// trap gate (IF unchanged), so interrupts can fire between vsprintf()
// and the loop that reads buf.  When an interrupt handler calls
// serial_printk(), it overwrites buf and corrupts color_printk's output,
// potentially writing garbage to the framebuffer and clobbering kernel
// data structures (manifested as #UD at RIP=0x40).
static char buf_color[4096];
static char buf_serial[4096];

position Pos;

psf2_t *font = (psf2_t*)&_binary_kernel_font_psf_start;

void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char c)
{
    /* NULL-safe: when no framebuffer is configured (aarch64 boot path
     * without ramfb, or early-boot before Pos is populated), skip the
     * pixel write entirely. color_printk's outer spin_lock still
     * serializes per-char against concurrent IRQ-context callers. */
    if (!Pos.FB_addr) return;

    uint32_t i = 0,j = 0;
	uint32_t * addr = NULL;
	int testval = 0;
	unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start + font->headersize +
            (c>0&&c<font->numglyph?c:0)*font->bytesperglyph;

	for(i = 0; i < font->height;i++)
	{
		addr = Pos.FB_addr + Pos.XResolution * ( Pos.YPosition * font->height + i ) + Pos.XPosition * font->width;
		testval = 0x100;
		for(j = 0;j < font->width;j++)
		{
			testval = testval >> 1;
			if(*glyph & testval)
				*addr = FRcolor;
			else
				*addr = BKcolor;
			addr++;
		}
		glyph++;
	}
}

void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
    if (!Pos.FB_addr) return;

    int i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start
        + font->headersize
        + (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;

    int pixel_row_start = row * (int)font->height;
    int pixel_col_start = col * (int)font->width;
    int max_rows = (int)(Pos.YResolution / font->height);
    int max_cols = (int)(Pos.XResolution / font->width);

    // Clamp to framebuffer bounds
    if (row < 0 || row >= max_rows) return;
    if (col < 0 || col >= max_cols) return;

    for (i = 0; i < (int)font->height; i++) {
        addr = Pos.FB_addr + Pos.XResolution * (pixel_row_start + i)
               + pixel_col_start;
        testval = 0x100;
        for (j = 0; j < (int)font->width; j++) {
            testval = testval >> 1;
            if (*glyph & testval)
                *addr = FRcolor;
            else
                *addr = BKcolor;
            addr++;
        }
        glyph++;
    }
}

int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...)
{
	int i = 0;
	int count = 0;
	int line = 0;
	va_list args;

	spin_lock(&Pos.lock);

	va_start(args, fmt);
	i = vsprintf(buf_color, fmt, args);
	va_end(args);

	for(count = 0;count < i || line;count++)
	{
		if(line > 0)
		{
			count--;
			goto Label_tab;
		}
		if((unsigned char)*(buf_color + count) == '\n')
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(buf_color + count) == '\r')
		{
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(buf_color + count) == '\b')
		{
			Pos.XPosition--;
			if(Pos.XPosition < 0)
			{
				Pos.XPosition = (Pos.XResolution / font->width - 1) * font->width;
				Pos.YPosition--;
				if(Pos.YPosition < 0)
					Pos.YPosition = (Pos.YResolution / font->height - 1) * font->height;
			}
			putchark(FRcolor , BKcolor , ' ');
		}
		else if((unsigned char)*(buf_color + count) == '\t')
		{
			line = ((Pos.XPosition + 8) & ~(8 - 1)) - Pos.XPosition;

Label_tab:
			line--;
			putchark(FRcolor , BKcolor , ' ');
			Pos.XPosition++;
		}
		else
		{
			putchark(FRcolor , BKcolor , (unsigned char)*(buf_color + count));
			Pos.XPosition++;
		}


		if(Pos.XPosition >= (int32_t)(Pos.XResolution / font->width))
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		if(Pos.YPosition >= (int32_t)(Pos.YResolution / font->height))
		{
			// Scroll framebuffer up by one character row.
			// Moves rows 1..N-1 up by one row, then clears the bottom row.
			int rows = (int)(Pos.YResolution / font->height);
			uint32_t pitch = Pos.XResolution * sizeof(uint32_t);
			int row_bytes = (int)(pitch * font->height);
			uint8_t *fb = (uint8_t *)Pos.FB_addr;
			memmove(fb, fb + row_bytes, (uintptr_t)row_bytes * (rows - 1));
			memset(fb + (uintptr_t)row_bytes * (rows - 1), 0, (uintptr_t)row_bytes);
			Pos.YPosition = rows - 1;
		}

	}
	spin_unlock(&Pos.lock);

	return i;
}

void serial_printk(const char * fmt,...)
{
	int i = 0;
	int count = 0;
	va_list args;

	// Hold serial_lock for the whole emission: vsprintf fills
	// buf_serial, then we write it byte-by-byte.  Without this
	// outer lock an IRQ handler (e.g. serial_poll / a debug
	// printk in an IRQ context) could call write_serial between
	// our vsprintf and our loop, clobbering buf_serial and
	// corrupting output.  write_serial() re-acquires the same
	// non-recursive lock — see kernel/driver/serial.c — so we
	// cannot call write_serial() while holding it.  Use the
	// _unlocked helper instead.
	uint64_t sf = spin_lock_irqsave(&serial_lock);

	va_start(args, fmt);
	i = vsprintf(buf_serial, fmt, args);
	va_end(args);

	for(count = 0;count < i ;count++)
	{
		write_serial_unlocked((unsigned char)*(buf_serial + count));
	}

	spin_unlock_irqrestore(&serial_lock, sf);
}
