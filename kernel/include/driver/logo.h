#ifndef _KERNEL_LOGO_H
#define _KERNEL_LOGO_H

// Draw the OS01 boot logo (ASCII art) on the framebuffer.
// Must be called after frame_buffer_early_init() so Pos.FB_addr is valid.
// Advances Pos.YPosition/XPosition below the logo so subsequent
// color_printk output continues beneath it.
void boot_logo_show(void);

#endif
