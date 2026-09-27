#ifndef _DRIVER_MOUSE_H
#define _DRIVER_MOUSE_H

/* Phase 6 PS/2 aux initialization. A successful return permits Task 8 to
 * register the character device after devfs becomes available. */
int mouse_init(void);
int mouse_ready(void);

#endif
