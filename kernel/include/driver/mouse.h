#ifndef _DRIVER_MOUSE_H
#define _DRIVER_MOUSE_H

#include <stdint.h>
#include <fs/vfs.h>
#include <fs/poll.h>

/* Phase 6 PS/2 aux initialization. */
int mouse_init(void);
int mouse_ready(void);
int mouse_devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer);
uint32_t mouse_poll_dev(void *priv, uint32_t requested, poll_table_t *pt);
uint64_t mouse_dropped_events(void);

/* The /dev/mouse device operations table.  It lives with the driver so
 * the file-aware hooks (open / read_file / poll_file / release_file)
 * can reach the input route; the platform only registers it. */
struct devfs_ops;
extern const struct devfs_ops mouse_ops;

#endif
