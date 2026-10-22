#ifndef _DRIVER_FB_STATE_H
#define _DRIVER_FB_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <uapi/fb.h>
#include <uapi/gfx.h>

typedef struct fb_snapshot {
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
struct bga_caps;
int  fb_install_backend(const struct bga_caps *caps, uint32_t *addr, uint64_t mapped_size);

void fb_control_lock(void);
void fb_control_unlock(void);

void fb_mark_raw_mmap_seen(void);
bool fb_has_raw_mmap_seen(void);
void fb_mark_raw_writable_mmap_seen(void);
bool fb_has_raw_writable_mmap_seen(void);
bool fb_has_owner_or_fallback(void);
bool fb_is_raw_writable_allowed(void);
void fb_set_raw_writable_allowed_for_test(bool allowed);
void fb_update_console_ownership_locked(void);
uint32_t fb_active_writers_count(void);

int fb_set_mode(const struct fb_set_mode_req *req);
int fb_get_state(struct fb_state *out);
int fb_get_modes(uint32_t capacity, struct fb_modes_req *out);

/* ── Display coordinator exclusive ownership & writer leases (Spec Task 1) ── */
int  fb_gfx_writer_begin(fb_lease_t *lease, uint64_t mode_seq, const void *cookie);
int  fb_owner_acquire(const void *cookie, uint64_t expected_mode);
int  fb_owner_release(const void *cookie);
int  fb_owner_register_fallback(const void *cookie, uint64_t expected_mode);
int  fb_owner_replace_fallback(const void *old_cookie, const void *new_cookie, uint64_t expected_mode);
void fb_owner_detach(const void *cookie);
int  fb_owner_get_state(const void *cookie, gfx_display_state_t *out);

/* ── The physical-input lease (GUI-2 plan Task 4) ────────────────
 * The lease is bound to the DISPLAY OWNER: it is granted inside the
 * display control lock together with the owner / mode check, and it is
 * revoked at every owner / mode state commit point (a new owner, a
 * release, a detach, a fallback handover, a resolution change, a
 * backend failure), so the lease can never outlive the view that owns
 * it.
 *
 * fb_input_bind() takes a TEMPORARY file reference: the caller drops
 * its own reference right after the call, and the lease holds only the
 * route's weak endpoint identities — no reference cycle, so the final
 * close of a bound raw file still fires and still revokes the lease.
 *
 * `expected_mode` is the view's mode sequence; a stale view is
 * refused with -ESTALE.  `generation` receives the lease generation
 * (non-zero, monotonic) and `initial_modifiers` the modifier snapshot
 * the new consumer starts from (bit0 = Caps Lock, every other bit 0).
 *
 * ONLY the Caps latch is published: the route's aggregate
 * Shift/Ctrl/Alt bits never cross the boundary.  The keys that are
 * still held at bind time are isolated and their release is swallowed,
 * so those aggregate bits would describe presses the consumer never
 * saw; the Caps latch is a STATE, not an edge, and the consumer has no
 * other way to learn it.
 *
 * fb_input_unbind() releases the lease of ONE view only: a view that
 * does not hold the lease never touches another view's lease.  A
 * non-zero `generation` must match the live lease exactly; zero is the
 * current view's explicit release.  `reason` is 0 for an explicit
 * cleanup and a revoke reason (EACCES / ESTALE / EIO) when the lease
 * is taken away.
 *
 * The errno fb_input_bind() can return:
 *   0        the lease was granted.
 *   -EINVAL  the arguments are wrong (no view, no keyboard file).
 *   -EAGAIN  the display state is not initialised yet.
 *   -EIO     the display backend has failed.
 *   -EACCES  the caller is not the display owner.
 *   -ESTALE  the view is stale (the display generation moved on).
 *   -EBUSY   a lease already exists (it is never preempted).
 *   -ENODEV  this build has no input route (no PS/2 chain).
 *
 * The route revokes the lease on its own — the final close of a bound
 * raw file revokes it — so the coordinator reconciles its record with
 * the route before it answers, and the two components never disagree
 * about "is there a lease?". */
struct file;
int  fb_input_bind(const void *view, uint64_t expected_mode,
                   struct file *kbd, struct file *mouse,
                   uint64_t *generation, uint32_t *initial_modifiers);
void fb_input_unbind(const void *view, uint64_t generation, int reason);

#ifdef OS01_HOST_TEST
void fb_state__test_set_generation(uint64_t generation);
#endif

#endif /* _DRIVER_FB_STATE_H */
