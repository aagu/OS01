#ifndef _DRIVER_FB_STATE_H
#define _DRIVER_FB_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <uapi/fb.h>

typedef struct {
    struct fb_state state;
    uint32_t *addr;
    uint64_t mapped_size;
} fb_snapshot_t;

typedef struct {
    fb_snapshot_t snapshot;
    bool held;
} fb_lease_t;

void fb_bootstrap_state(uint64_t phys, uint64_t gop_bytes, const struct fb_info *info);
void fb_publish_initial_mapping(uint32_t *addr, uint64_t mapped_size);
int  fb_snapshot_read(fb_snapshot_t *out); /* 短锁，writer可用，不取mutex */
int  fb_writer_begin(fb_lease_t *lease, uint64_t expected_generation);
void fb_writer_end(fb_lease_t *lease); /* held=false为no-op */
int  fb_transition_begin(bool boot_probe); /* boot无等待；runtime有界排空 */
void fb_transition_end(void); /* 故障状态不重新开放 */
void fb_mark_failed(void);

void fb_control_lock(void);
void fb_control_unlock(void);

#ifdef OS01_HOST_TEST
void fb_state__test_set_generation(uint64_t generation);
#endif

#endif /* _DRIVER_FB_STATE_H */
