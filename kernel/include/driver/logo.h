#ifndef _KERNEL_LOGO_H
#define _KERNEL_LOGO_H

// Draw the OS01 boot logo on the framebuffer.
// Must be called after the framebuffer is mapped: on arches with a display
// coordinator a writer lease must be available, on arches without one
// Pos.FB_addr must be valid.
// Advances Pos.YPosition/XPosition below the logo so subsequent
// color_printk output continues beneath it.
// Returns 0 when the logo was drawn, negative when it was skipped.
int boot_logo_show(void);

#endif
