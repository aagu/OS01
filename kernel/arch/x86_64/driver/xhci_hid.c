/* kernel/arch/x86_64/driver/xhci_hid.c — xHCI Boot HID runtime service */
#include <driver/xhci_hid.h>
#include <driver/xhci.h>
#include <driver/usb_hid_report.h>
#include <driver/input_route.h>
#include <errno.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <subsys/subsys.h>
#include <sync/mutex.h>

#ifndef OS01_HOST_TEST
#include <arch/clocksource.h>
#include <sched/task.h>
extern bool vfs_sched_ready(void);
#else
__attribute__((weak)) int mutex_trylock(mutex_t *m)
{
    if (!m || m->owner != 0)
        return 0;
    m->owner = 1;
    return 1;
}
int kernel_thread_on_cpu(uint64_t (*fn)(uint64_t), uint64_t arg, unsigned cpu);
#endif

#define XHCI_HID_SET_PROTOCOL_DEADLINE_NS 500000000ULL  /* 500 ms */
#define XHCI_HID_WORKER_PERIOD_NS         10000000ULL   /* 10 ms */
#define XHCI_HID_HUB_THROTTLE_NS          100000000ULL  /* 100 ms */
#define XHCI_HID_EVENT_BUDGET             64U
#define XHCI_HID_REPORT_BUF_CAP           64U
#define XHCI_HID_MON_BUF_CAP              64U
#define XHCI_HID_MAX_CONTROLLERS          8U

struct xhci_hid_dev {
    bool active;
    bool released;
    bool retire_logged;
    enum xhci_hid_kind kind;
    struct xhci_controller *ctrl;
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint8_t slot_id;
    uint8_t iface;
    uint8_t dci;
    uint16_t mps;
    uint8_t root_port;
    uint8_t depth;
    uint8_t parent_slot;
    uint8_t parent_port;
    uint32_t route;
    usb_hid_keyboard_t kbd_state;
    uint8_t last_mouse_buttons;
};

struct xhci_hid_hub_mon {
    bool active;
    struct xhci_controller *ctrl;
    uint8_t hub_slot;
    uint8_t hub_dci;
    uint8_t root_port;
    uint8_t nports;
    uint64_t next_submit_ns;
};

struct xhci_hid_ctrl_state {
    struct xhci_controller *ctrl;
};

struct xhci_hid_pending_report {
    struct xhci_hid_dev *dev;
    uint16_t len;
    uint8_t data[XHCI_HID_REPORT_BUF_CAP];
};

static mutex_t s_hid_service_lock;
static bool s_hid_lock_inited = false;
static bool s_worker_started = false;
static bool s_kind_bound[XHCI_HID_KINDS];
static struct xhci_hid_dev s_hid_devs[XHCI_HID_KINDS];
static struct xhci_hid_hub_mon s_hub_monitors[XHCI_HID_KINDS];
static struct xhci_hid_ctrl_state s_ctrl_states[XHCI_HID_MAX_CONTROLLERS];
static struct xhci_hid_pending_report s_pending_rep[XHCI_HID_KINDS];
static input_ps2_frame_t s_scratch_frames[USB_HID_MAX_FRAMES];
static uint8_t s_scratch_buf[XHCI_HID_REPORT_BUF_CAP];

static inline bool xhci_hid_can_lock(void)
{
#ifdef OS01_HOST_TEST
    return s_hid_lock_inited;
#else
    return s_hid_lock_inited && s_worker_started && vfs_sched_ready() &&
           current && current->pid != 0;
#endif
}

static inline bool xhci_hid_service_lock(void)
{
    if (!xhci_hid_can_lock())
        return false;
    mutex_lock(&s_hid_service_lock);
    return true;
}

static inline void xhci_hid_service_unlock(bool locked)
{
    if (locked)
        mutex_unlock(&s_hid_service_lock);
}

static bool xhci_hid_has_bound_devices(void)
{
    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        if (s_hid_devs[i].active)
            return true;
    }
    return false;
}

bool xhci_hid_keyboard_bound(void)
{
    return s_kind_bound[XHCI_HID_KEYBOARD];
}

bool xhci_hid_mouse_bound(void)
{
    return s_kind_bound[XHCI_HID_MOUSE];
}

static bool xhci_hid_ctrl_usable(const struct xhci_controller *ctrl)
{
    return ctrl && ctrl->running && !ctrl->poisoned && !ctrl->halted_confirmed;
}

static bool xhci_hid_root_port_ok(const struct xhci_controller *ctrl, uint8_t root_port)
{
    if (!ctrl || !ctrl->op_regs || root_port < 1 || root_port > ctrl->max_ports)
        return false;
    uint32_t portsc = xhci_read32(ctrl->op_regs + 0x400 +
                                  (uint32_t)(root_port - 1U) * 0x10U);
    return (portsc & XHCI_PORTSC_CCS) != 0 &&
           (portsc & XHCI_PORTSC_PED) != 0 &&
           (portsc & XHCI_PORTSC_CSC) == 0;
}

static bool xhci_hid_slot_usable(const struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!xhci_hid_ctrl_usable(ctrl))
        return false;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS)
        return false;
    const struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot || !slot->active || !slot->configured || slot->disabled || slot->poisoned)
        return false;
    return xhci_slot_transport_usable(ctrl, slot_id);
}

static bool xhci_hid_ring_valid(const struct xhci_slot *slot, uint8_t dci, uint16_t max_mps)
{
    if (!slot || dci < XHCI_MIN_ENDPOINT_DCI || dci > slot->max_dci || !slot->ep_rings)
        return false;
    const struct xhci_ep_ring *ring = slot->ep_rings[dci];
    if (!ring || !ring->active || !ring->ring_virt || !ring->ring_phys)
        return false;
    if (ring->ep_type != XHCI_EP_TYPE_INTR_IN)
        return false;
    if (ring->max_packet_size == 0 || ring->max_packet_size > max_mps)
        return false;
    return true;
}

static struct xhci_hid_hub_mon *xhci_hid_find_monitor(const struct xhci_controller *ctrl,
                                                      uint8_t hub_slot)
{
    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        if (s_hub_monitors[i].active &&
            s_hub_monitors[i].ctrl == ctrl &&
            s_hub_monitors[i].hub_slot == hub_slot) {
            return &s_hub_monitors[i];
        }
    }
    return NULL;
}

static bool xhci_hid_monitor_needed(const struct xhci_hid_hub_mon *mon)
{
    if (!mon || !mon->active)
        return false;
    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        if (s_hid_devs[i].active &&
            s_hid_devs[i].ctrl == mon->ctrl &&
            s_hid_devs[i].depth == 1 &&
            s_hid_devs[i].parent_slot == mon->hub_slot) {
            return true;
        }
    }
    return false;
}

static void xhci_hid_register_ctrl_state(struct xhci_controller *ctrl)
{
    for (unsigned i = 0; i < XHCI_HID_MAX_CONTROLLERS; i++) {
        if (s_ctrl_states[i].ctrl == ctrl)
            return;
    }
    for (unsigned i = 0; i < XHCI_HID_MAX_CONTROLLERS; i++) {
        if (!s_ctrl_states[i].ctrl) {
            s_ctrl_states[i].ctrl = ctrl;
            return;
        }
    }
}

static void xhci_hid_retire_dev_locked(struct xhci_hid_dev *dev, const char *reason,
                                       struct xhci_hid_dev **pending_rel,
                                       unsigned *pending_rel_cnt)
{
    if (!dev || !dev->active)
        return;

    if (dev->ctrl) {
        xhci_intr_in_retire_locked(dev->ctrl, dev->slot_id, dev->dci);
    }
    dev->active = false;

    if (!dev->retire_logged) {
        dev->retire_logged = true;
        log_info("[xhci-hid] retired bdf=%02x:%02x.%u root=%u route=0x%x slot=%u iface=%u dci=%u kind=%s reason=%s\n",
                 (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn,
                 (unsigned)dev->root_port, (unsigned)dev->route,
                 (unsigned)dev->slot_id, (unsigned)dev->iface, (unsigned)dev->dci,
                 dev->kind == XHCI_HID_KEYBOARD ? "kbd" : "mouse",
                 reason ? reason : "unknown");
    }

    if (!dev->released && pending_rel && pending_rel_cnt &&
        *pending_rel_cnt < XHCI_HID_KINDS) {
        pending_rel[(*pending_rel_cnt)++] = dev;
    }
}

static void xhci_hid_retire_hub_locked(struct xhci_hid_hub_mon *mon, const char *reason,
                                       struct xhci_hid_dev **pending_rel,
                                       unsigned *pending_rel_cnt)
{
    if (!mon || !mon->active)
        return;

    if (mon->ctrl) {
        xhci_intr_in_retire_locked(mon->ctrl, mon->hub_slot, mon->hub_dci);
    }
    mon->active = false;

    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        if (s_hid_devs[i].active &&
            s_hid_devs[i].ctrl == mon->ctrl &&
            s_hid_devs[i].depth == 1 &&
            s_hid_devs[i].parent_slot == mon->hub_slot) {
            xhci_hid_retire_dev_locked(&s_hid_devs[i], reason, pending_rel, pending_rel_cnt);
        }
    }
}

static void xhci_hid_emit_pending_releases(struct xhci_hid_dev **pending_rel, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        struct xhci_hid_dev *dev = pending_rel[i];
        if (!dev || dev->released)
            continue;
        dev->released = true;
        if (dev->kind == XHCI_HID_KEYBOARD) {
            size_t n = usb_hid_keyboard_release(&dev->kbd_state, s_scratch_frames);
            if (n > 0) {
                input_route_keyboard_usb_frames(s_scratch_frames, n);
            }
        } else if (dev->kind == XHCI_HID_MOUSE) {
            if (dev->last_mouse_buttons != 0) {
                mouse_event_t rel;
                memset(&rel, 0, sizeof(rel));
                dev->last_mouse_buttons = 0;
                input_route_mouse_usb_event(&rel);
            }
        }
    }
}

static void xhci_hid_validate_ctrl_locked(struct xhci_controller *ctrl,
                                          struct xhci_hid_dev **pending_rel,
                                          unsigned *pending_rel_cnt)
{
    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        struct xhci_hid_hub_mon *mon = &s_hub_monitors[i];
        if (!mon->active || mon->ctrl != ctrl)
            continue;
        if (!xhci_hid_ctrl_usable(ctrl)) {
            xhci_hid_retire_hub_locked(mon, "controller-unusable", pending_rel, pending_rel_cnt);
        } else if (!xhci_hid_root_port_ok(ctrl, mon->root_port)) {
            xhci_hid_retire_hub_locked(mon, "hub-root-disconnect", pending_rel, pending_rel_cnt);
        } else if (!xhci_hid_slot_usable(ctrl, mon->hub_slot)) {
            xhci_hid_retire_hub_locked(mon, "hub-transport-loss", pending_rel, pending_rel_cnt);
        }
    }

    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        struct xhci_hid_dev *dev = &s_hid_devs[i];
        if (!dev->active || dev->ctrl != ctrl)
            continue;
        if (!xhci_hid_ctrl_usable(ctrl)) {
            xhci_hid_retire_dev_locked(dev, "controller-unusable", pending_rel, pending_rel_cnt);
        } else if (!xhci_hid_root_port_ok(ctrl, dev->root_port)) {
            xhci_hid_retire_dev_locked(dev, "root-port-disconnect", pending_rel, pending_rel_cnt);
        } else if (!xhci_hid_slot_usable(ctrl, dev->slot_id)) {
            xhci_hid_retire_dev_locked(dev, "slot-transport-loss", pending_rel, pending_rel_cnt);
        } else if (dev->depth == 1) {
            struct xhci_hid_hub_mon *mon = xhci_hid_find_monitor(ctrl, dev->parent_slot);
            if (!mon || !mon->active) {
                xhci_hid_retire_dev_locked(dev, "hub-monitor-lost", pending_rel, pending_rel_cnt);
            }
        }
    }
}

static void xhci_hid_process_monitors_locked(struct xhci_controller *ctrl, uint64_t now_ns,
                                             struct xhci_hid_dev **pending_rel,
                                             unsigned *pending_rel_cnt)
{
    uint8_t mon_buf[XHCI_HID_MON_BUF_CAP];

    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        struct xhci_hid_hub_mon *mon = &s_hub_monitors[i];
        if (!mon->active || mon->ctrl != ctrl)
            continue;

        struct xhci_intr_in_result res;
        int rc = xhci_intr_in_take_locked(ctrl, mon->hub_slot, mon->hub_dci,
                                          mon_buf, sizeof(mon_buf), &res);
        if (rc < 0) {
            xhci_hid_retire_hub_locked(mon, "hub-monitor-error", pending_rel, pending_rel_cnt);
            continue;
        }
        if (rc == 0)
            continue;

        uint16_t min_bytes = (uint16_t)((mon->nports + 1U + 7U) / 8U);
        bool ok_cc = (res.cc == XHCI_COMP_SUCCESS || res.cc == XHCI_COMP_SHORT_TX);
        if (!ok_cc || res.bad_length || res.actual < min_bytes) {
            xhci_hid_retire_hub_locked(mon, "hub-monitor-short-or-err", pending_rel, pending_rel_cnt);
            continue;
        }

        if (mon_buf[0] & 0x01U) {
            xhci_hid_retire_hub_locked(mon, "hub-status-change", pending_rel, pending_rel_cnt);
            continue;
        }

        bool any_bit_set = false;
        for (uint16_t b = 0; b < res.actual; b++) {
            if (mon_buf[b] != 0) {
                any_bit_set = true;
                break;
            }
        }

        for (unsigned d = 0; d < XHCI_HID_KINDS; d++) {
            struct xhci_hid_dev *dev = &s_hid_devs[d];
            if (!dev->active || dev->ctrl != ctrl ||
                dev->depth != 1 || dev->parent_slot != mon->hub_slot) {
                continue;
            }
            uint8_t p = dev->parent_port;
            uint16_t byte_idx = (uint16_t)(p / 8U);
            uint8_t bit_mask = (uint8_t)(1U << (p % 8U));
            if (byte_idx < res.actual && (mon_buf[byte_idx] & bit_mask)) {
                xhci_hid_retire_dev_locked(dev, "hub-port-change", pending_rel, pending_rel_cnt);
            }
        }

        if (!xhci_hid_monitor_needed(mon)) {
            xhci_intr_in_retire_locked(ctrl, mon->hub_slot, mon->hub_dci);
            mon->active = false;
        } else if (any_bit_set) {
            mon->next_submit_ns = now_ns + XHCI_HID_HUB_THROTTLE_NS;
        }
    }
}

void xhci_hid_service_once(uint64_t now_ns)
{
    bool svc_locked = xhci_hid_service_lock();
    if (!xhci_hid_has_bound_devices()) {
        xhci_hid_service_unlock(svc_locked);
        return;
    }

    for (unsigned ci = 0; ci < XHCI_HID_MAX_CONTROLLERS; ci++) {
        struct xhci_controller *ctrl = s_ctrl_states[ci].ctrl;
        if (!ctrl)
            continue;

        struct xhci_hid_dev *pending_rel[XHCI_HID_KINDS];
        unsigned pending_rel_cnt = 0;
        unsigned pending_rep_cnt = 0;

        if (!xhci_hid_ctrl_usable(ctrl)) {
            for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
                if (s_hub_monitors[i].active && s_hub_monitors[i].ctrl == ctrl)
                    xhci_hid_retire_hub_locked(&s_hub_monitors[i], "controller-lost",
                                               pending_rel, &pending_rel_cnt);
                if (s_hid_devs[i].active && s_hid_devs[i].ctrl == ctrl)
                    xhci_hid_retire_dev_locked(&s_hid_devs[i], "controller-lost",
                                               pending_rel, &pending_rel_cnt);
            }
            s_ctrl_states[ci].ctrl = NULL;
            xhci_hid_emit_pending_releases(pending_rel, pending_rel_cnt);
            continue;
        }

        bool io_locked = false;
        if (ctrl->io_lock_init) {
            if (!mutex_trylock(&ctrl->io_lock))
                continue; /* Controller busy: skip this round */
            io_locked = true;
        }

        /* 1. Validate topology / controller */
        xhci_hid_validate_ctrl_locked(ctrl, pending_rel, &pending_rel_cnt);

        /* 2. Drain up to 64 events */
        unsigned drained = xhci_intr_in_pump_locked(ctrl, XHCI_HID_EVENT_BUDGET);
        if (drained >= XHCI_HID_EVENT_BUDGET) {
            /* Saturated: keep cached results, do not take/publish/re-arm until empty ring */
            if (io_locked)
                mutex_unlock(&ctrl->io_lock);
            xhci_hid_emit_pending_releases(pending_rel, pending_rel_cnt);
            continue;
        }

        /* 3. Process Hub monitors first */
        xhci_hid_process_monitors_locked(ctrl, now_ns, pending_rel, &pending_rel_cnt);

        /* 4. Re-validate topology / controller */
        xhci_hid_validate_ctrl_locked(ctrl, pending_rel, &pending_rel_cnt);

        /* 5. Copy valid HID reports into worker scratch */
        for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
            struct xhci_hid_dev *dev = &s_hid_devs[i];
            if (!dev->active || dev->ctrl != ctrl)
                continue;

            struct xhci_intr_in_result res;
            int rc = xhci_intr_in_take_locked(ctrl, dev->slot_id, dev->dci,
                                              s_scratch_buf, sizeof(s_scratch_buf), &res);
            if (rc < 0) {
                xhci_hid_retire_dev_locked(dev, "hid-take-error", pending_rel, &pending_rel_cnt);
                continue;
            }
            if (rc == 0)
                continue;

            bool ok_cc = (res.cc == XHCI_COMP_SUCCESS || res.cc == XHCI_COMP_SHORT_TX);
            if (!ok_cc || res.bad_length) {
                xhci_hid_retire_dev_locked(dev, "hid-xfer-error", pending_rel, &pending_rel_cnt);
                continue;
            }
            if (res.actual > 0 && res.actual <= XHCI_HID_REPORT_BUF_CAP) {
                struct xhci_hid_pending_report *pr = &s_pending_rep[pending_rep_cnt++];
                pr->dev = dev;
                pr->len = res.actual;
                memcpy(pr->data, s_scratch_buf, res.actual);
            }
        }

        /* 6. Re-arm all required monitors FIRST, then HID TDs */
        for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
            struct xhci_hid_hub_mon *mon = &s_hub_monitors[i];
            if (!mon->active || mon->ctrl != ctrl)
                continue;
            if (!xhci_hid_monitor_needed(mon)) {
                xhci_intr_in_retire_locked(ctrl, mon->hub_slot, mon->hub_dci);
                mon->active = false;
                continue;
            }
            if (now_ns < mon->next_submit_ns)
                continue;
            int s_rc = xhci_intr_in_submit_locked(ctrl, mon->hub_slot, mon->hub_dci);
            if (s_rc != 0 && s_rc != -EBUSY) {
                xhci_hid_retire_hub_locked(mon, "hub-submit-error", pending_rel, &pending_rel_cnt);
            }
        }

        for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
            struct xhci_hid_dev *dev = &s_hid_devs[i];
            if (!dev->active || dev->ctrl != ctrl)
                continue;
            int s_rc = xhci_intr_in_submit_locked(ctrl, dev->slot_id, dev->dci);
            if (s_rc != 0 && s_rc != -EBUSY) {
                xhci_hid_retire_dev_locked(dev, "hid-submit-error", pending_rel, &pending_rel_cnt);
            }
        }

        /* 7. Release controller io_lock BEFORE entering input_route */
        if (io_locked)
            mutex_unlock(&ctrl->io_lock);

        /* 8. Deliver releases and valid HID reports outside ctrl->io_lock */
        xhci_hid_emit_pending_releases(pending_rel, pending_rel_cnt);

        for (unsigned i = 0; i < pending_rep_cnt; i++) {
            struct xhci_hid_dev *dev = s_pending_rep[i].dev;
            if (!dev->active || dev->released)
                continue;
            if (dev->kind == XHCI_HID_KEYBOARD) {
                int n = usb_hid_keyboard_update(&dev->kbd_state,
                                                s_pending_rep[i].data,
                                                s_pending_rep[i].len,
                                                s_scratch_frames);
                if (n > 0) {
                    input_route_keyboard_usb_frames(s_scratch_frames, (size_t)n);
                }
            } else if (dev->kind == XHCI_HID_MOUSE) {
                mouse_event_t ev;
                if (usb_hid_mouse_decode(s_pending_rep[i].data,
                                         s_pending_rep[i].len,
                                         dev->mps, &ev) == 0) {
                    dev->last_mouse_buttons = ev.buttons;
                    input_route_mouse_usb_event(&ev);
                }
            }
        }
    }

    xhci_hid_service_unlock(svc_locked);
}

void xhci_hid_detach_controller(struct xhci_controller *ctrl)
{
    if (!ctrl)
        return;

    bool svc_locked = xhci_hid_service_lock();

    struct xhci_hid_dev *pending_rel[XHCI_HID_KINDS];
    unsigned pending_rel_cnt = 0;

    bool io_locked = false;
    if (ctrl->io_lock_init && xhci_hid_can_lock()) {
        mutex_lock(&ctrl->io_lock);
        io_locked = true;
    }

    for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
        if (s_hub_monitors[i].active && s_hub_monitors[i].ctrl == ctrl) {
            xhci_hid_retire_hub_locked(&s_hub_monitors[i], "controller-detach",
                                       pending_rel, &pending_rel_cnt);
        }
        if (s_hid_devs[i].active && s_hid_devs[i].ctrl == ctrl) {
            xhci_hid_retire_dev_locked(&s_hid_devs[i], "controller-detach",
                                       pending_rel, &pending_rel_cnt);
        }
    }

    for (unsigned i = 0; i < XHCI_HID_MAX_CONTROLLERS; i++) {
        if (s_ctrl_states[i].ctrl == ctrl) {
            s_ctrl_states[i].ctrl = NULL;
        }
    }

    if (io_locked)
        mutex_unlock(&ctrl->io_lock);

    xhci_hid_emit_pending_releases(pending_rel, pending_rel_cnt);
    xhci_hid_service_unlock(svc_locked);
}

static int xhci_hid_bind_slot(struct xhci_controller *ctrl, struct xhci_slot *slot)
{
    if (!xhci_hid_slot_usable(ctrl, slot->slot_id))
        return -EINVAL;
    if (slot->msc_iface.active ||
        (slot->iface.active && slot->iface.class_code == 0x08U))
        return -EBUSY;
    if (slot->loc.depth > 1U)
        return -ENOTSUP;

    uint8_t root_port = slot->loc.root_port ? slot->loc.root_port : slot->port_num;
    if (!xhci_hid_root_port_ok(ctrl, root_port))
        return -ENODEV;

    struct xhci_slot *hub = NULL;
    if (slot->loc.depth == 1U) {
        uint8_t pslot = slot->loc.parent_slot;
        if (!xhci_hid_slot_usable(ctrl, pslot))
            return -ENODEV;
        hub = ctrl->slots[pslot];
        if (!hub->is_hub || hub->hub_ss || hub->hub_nports == 0 ||
            slot->loc.parent_port < 1 || slot->loc.parent_port > hub->hub_nports)
            return -ENOTSUP;
        if (!xhci_hid_ring_valid(hub, hub->hub_status_dci, XHCI_HID_MON_BUF_CAP))
            return -EINVAL;
        uint16_t min_mon_bytes = (uint16_t)((hub->hub_nports + 1U + 7U) / 8U);
        if (hub->ep_rings[hub->hub_status_dci]->max_packet_size < min_mon_bytes)
            return -EINVAL;
    }

    /* Collect eligible candidates for this slot */
    enum xhci_hid_kind kinds[XHCI_HID_KINDS];
    const struct xhci_hid_candidate *cands[XHCI_HID_KINDS];
    unsigned ncand = 0;

    for (unsigned k = 0; k < XHCI_HID_KINDS; k++) {
        if (s_kind_bound[k])
            continue;
        if (!slot->hid_candidates[k].valid)
            continue;
        if (!xhci_hid_ring_valid(slot, slot->hid_candidates[k].dci, XHCI_HID_REPORT_BUF_CAP))
            return -EINVAL; /* Mismatched ring: fail-closed for this slot */
        kinds[ncand] = (enum xhci_hid_kind)k;
        cands[ncand] = &slot->hid_candidates[k];
        ncand++;
    }

    if (ncand == 0)
        return 0;

    /* Prepare Hub monitor if needed (reuse existing monitor for same Hub) */
    struct xhci_hid_hub_mon *mon = NULL;
    bool mon_created = false;
    if (slot->loc.depth == 1U) {
        mon = xhci_hid_find_monitor(ctrl, hub->slot_id);
        if (!mon) {
            for (unsigned i = 0; i < XHCI_HID_KINDS; i++) {
                if (!s_hub_monitors[i].active) {
                    mon = &s_hub_monitors[i];
                    break;
                }
            }
            if (!mon)
                return -ENOSPC;
            int rc = xhci_intr_in_prepare_locked(ctrl, hub->slot_id, hub->hub_status_dci);
            if (rc != 0)
                return rc;
            memset(mon, 0, sizeof(*mon));
            mon->active = true;
            mon->ctrl = ctrl;
            mon->hub_slot = hub->slot_id;
            mon->hub_dci = hub->hub_status_dci;
            mon->root_port = root_port;
            mon->nports = hub->hub_nports;
            mon_created = true;
        }
    }

    /* Send SET_PROTOCOL(0) and prepare DMA for all eligible candidates on this slot */
    unsigned prepared = 0;
    for (unsigned i = 0; i < ncand; i++) {
        struct xhci_setup setup = {
            .type = 0x21U,
            .request = 0x0BU,
            .value = 0U,
            .index = cands[i]->iface,
            .length = 0U,
        };
        uint64_t deadline = arch_clocksource_read_ns() + XHCI_HID_SET_PROTOCOL_DEADLINE_NS;
        int rc = xhci_control_no_data_until(ctrl, slot->slot_id, &setup, deadline);
        if (rc != 0) {
            if (slot->loc.tt_hub_slot == 0) {
                bool io_locked = false;
                if (ctrl->io_lock_init && xhci_hid_can_lock()) {
                    mutex_lock(&ctrl->io_lock);
                    io_locked = true;
                }
                slot->poisoned = true;
                slot->dma_retained = true;
                if (io_locked)
                    mutex_unlock(&ctrl->io_lock);
            }
            goto fail_rollback;
        }

        rc = xhci_intr_in_prepare_locked(ctrl, slot->slot_id, cands[i]->dci);
        if (rc != 0)
            goto fail_rollback;
        prepared++;
    }

    /* All candidates + monitor succeeded: commit atomically */
    uint8_t bus = ctrl->pdev ? ctrl->pdev->bus : 0;
    uint8_t dev = ctrl->pdev ? ctrl->pdev->slot : 0;
    uint8_t fn = ctrl->pdev ? ctrl->pdev->fn : 0;

    for (unsigned i = 0; i < ncand; i++) {
        enum xhci_hid_kind k = kinds[i];
        struct xhci_hid_dev *hd = &s_hid_devs[k];
        memset(hd, 0, sizeof(*hd));
        hd->active = true;
        hd->kind = k;
        hd->ctrl = ctrl;
        hd->bus = bus;
        hd->dev = dev;
        hd->fn = fn;
        hd->slot_id = slot->slot_id;
        hd->iface = cands[i]->iface;
        hd->dci = cands[i]->dci;
        hd->mps = slot->ep_rings[cands[i]->dci]->max_packet_size;
        hd->root_port = root_port;
        hd->depth = slot->loc.depth;
        hd->parent_slot = slot->loc.parent_slot;
        hd->parent_port = slot->loc.parent_port;
        hd->route = slot->loc.route;
        s_kind_bound[k] = true;

        log_info("[xhci-hid] bound bdf=%02x:%02x.%u root=%u route=0x%x slot=%u iface=%u dci=%u kind=%s\n",
                 (unsigned)bus, (unsigned)dev, (unsigned)fn,
                 (unsigned)root_port, (unsigned)slot->loc.route,
                 (unsigned)slot->slot_id, (unsigned)cands[i]->iface,
                 (unsigned)cands[i]->dci,
                 k == XHCI_HID_KEYBOARD ? "kbd" : "mouse");
    }

    xhci_hid_register_ctrl_state(ctrl);
    return 0;

fail_rollback:
    for (unsigned i = 0; i < prepared; i++) {
        xhci_intr_in_retire_locked(ctrl, slot->slot_id, cands[i]->dci);
    }
    if (mon_created && mon) {
        xhci_intr_in_retire_locked(ctrl, mon->hub_slot, mon->hub_dci);
        mon->active = false;
    }
    return -EIO;
}

static int xhci_hid_init(void)
{
    mutex_init(&s_hid_service_lock);
    s_hid_lock_inited = true;

    uint8_t nctrl = xhci_controller_count();
    for (uint8_t ci = 0; ci < nctrl; ci++) {
        struct xhci_controller *ctrl = xhci_controller_get(ci);
        if (!xhci_hid_ctrl_usable(ctrl))
            continue;

        for (uint8_t si = 1; si <= XHCI_MAX_MANAGED_SLOTS; si++) {
            if (s_kind_bound[XHCI_HID_KEYBOARD] && s_kind_bound[XHCI_HID_MOUSE])
                return 0;
            struct xhci_slot *slot = ctrl->slots[si];
            if (!slot)
                continue;
            (void)xhci_hid_bind_slot(ctrl, slot);
        }
    }

    return 0;
}

#ifndef OS01_HOST_TEST
static bool xhci_hid_wake_check(struct task_struct *waiter)
{
    return arch_clocksource_read_ns() >= waiter->wakeup_ns;
}

static void xhci_hid_sleep_ns(uint64_t duration_ns)
{
    uint64_t target_ns = arch_clocksource_read_ns() + duration_ns;
    current->wakeup_ns = target_ns;
    do {
        blocker_wait(xhci_hid_wake_check, BLOCKER_NANOSLEEP, false);
    } while (arch_clocksource_read_ns() < target_ns);
    current->wakeup_ns = 0;
}
#endif

static uint64_t xhci_hid_worker_fn(uint64_t arg)
{
    (void)arg;
#ifndef OS01_HOST_TEST
    while (!vfs_sched_ready()) {
        xhci_hid_sleep_ns(XHCI_HID_WORKER_PERIOD_NS);
    }
    for (;;) {
        uint64_t now_ns = arch_clocksource_read_ns();
        xhci_hid_service_once(now_ns);
        xhci_hid_sleep_ns(XHCI_HID_WORKER_PERIOD_NS);
    }
#endif
    return 0;
}

int arch_usb_hid_start_worker(void)
{
    if (s_worker_started)
        return 0;
    if (!xhci_hid_has_bound_devices())
        return 0;

    int pid = kernel_thread_on_cpu(xhci_hid_worker_fn, 0, 0);
    if (pid < 0) {
        log_warn("[xhci-hid] failed to start BSP worker (rc=%d)\n", pid);
        return pid;
    }
    s_worker_started = true;
    log_info("[xhci-hid] BSP worker started, pid=%d\n", pid);
    return 0;
}

#ifndef OS01_HOST_TEST
static const char *const xhci_hid_requires[] = { "device-boot" };

static int xhci_hid_register(void)
{
    return register_subsys_deps("usb-hid", xhci_hid_init,
                                SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL,
                                xhci_hid_requires, 1);
}
SUBSYS_INITCALL(xhci_hid_register);
#else
int xhci_hid__test_init(void)
{
    return xhci_hid_init();
}

void xhci_hid__test_reset(void)
{
    memset(&s_hid_service_lock, 0, sizeof(s_hid_service_lock));
    s_hid_lock_inited = false;
    s_worker_started = false;
    memset(s_kind_bound, 0, sizeof(s_kind_bound));
    memset(s_hid_devs, 0, sizeof(s_hid_devs));
    memset(s_hub_monitors, 0, sizeof(s_hub_monitors));
    memset(s_ctrl_states, 0, sizeof(s_ctrl_states));
}
#endif
