/* kernel/arch/x86_64/driver/xhci_hub.c — xHCI USB Hub protocol and tiered enumeration */
#include <driver/xhci_hub.h>
#include <arch/clocksource.h>
#include <log/log.h>
#include <memory/slab.h>
#include <errno.h>
#include <string.h>

#ifdef OS01_HOST_TEST
__attribute__((weak)) uint64_t arch_clocksource_read_ns(void) { return 0; }
#endif

static uint16_t le16_at(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int hub_get_status(struct xhci_controller *ctrl, uint8_t slot_id,
                          uint8_t req_type, uint16_t index,
                          uint64_t deadline_ns, uint16_t *out_status,
                          uint16_t *out_change)
{
    struct xhci_setup setup = {
        .type = req_type,
        .request = USB_REQ_GET_STATUS,
        .value = 0,
        .index = index,
        .length = 4
    };
    uint8_t buf[4] = {0};
    int rc = xhci_control_in(ctrl, slot_id, &setup, buf, sizeof(buf), deadline_ns);
    if (rc < 4) return rc < 0 ? rc : -EIO;
    if (out_status) *out_status = le16_at(buf);
    if (out_change) *out_change = le16_at(buf + 2);
    return 0;
}

static int hub_port_feature(struct xhci_controller *ctrl, uint8_t slot_id,
                            uint8_t req, uint16_t feature, uint8_t port,
                            uint64_t deadline_ns)
{
    struct xhci_setup setup = {
        .type = USB_RT_PORT_OUT,
        .request = req,
        .value = feature,
        .index = port,
        .length = 0
    };
    return xhci_control_no_data_until(ctrl, slot_id, &setup, deadline_ns);
}

int xhci_hub_validate_config_dci(const struct xhci_slot *slot, const uint8_t *cfg_buf,
                                 uint16_t cfg_len, uint8_t *out_status_dci)
{
    if (out_status_dci)
        *out_status_dci = 0;
    if (!slot || !cfg_buf || cfg_len < sizeof(struct usb_config_descriptor))
        return -EINVAL;

    uint8_t speed_cls = slot->loc.speed_class ? slot->loc.speed_class : slot->speed;
    uint32_t offset = 0;
    bool in_hub_alt0 = false;
    bool found_hub_iface = false;
    bool found_intr_in = false;
    uint8_t ep_count = 0;
    uint8_t status_dci = 0;

    while (offset + 2 <= cfg_len) {
        uint8_t len = cfg_buf[offset];
        uint8_t type = cfg_buf[offset + 1];
        if (len < 2 || offset + len > cfg_len)
            return -EINVAL;

        if (type == USB_DESC_TYPE_INTERFACE) {
            if (len < sizeof(struct usb_interface_descriptor))
                return -EINVAL;
            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(cfg_buf + offset);
            in_hub_alt0 = (iface->bAlternateSetting == 0 &&
                           iface->bInterfaceClass == USB_CLASS_HUB &&
                           iface->bInterfaceSubClass == 0);
            if (in_hub_alt0) {
                /* Accept FS/SS (proto 0) or HS single-TT (proto 0 or 1); reject multi-TT alt0 (proto 2). */
                if (speed_cls == XHCI_PORT_SPEED_SUPER) {
                    if (iface->bInterfaceProtocol != 0 || iface->bNumEndpoints != 1)
                        return -EINVAL;
                } else {
                    if (iface->bInterfaceProtocol > 1 || iface->bNumEndpoints != 1)
                        return -EINVAL;
                }
                found_hub_iface = true;
            }
        } else if (type == USB_DESC_TYPE_ENDPOINT && in_hub_alt0) {
            if (len < sizeof(struct usb_endpoint_descriptor))
                return -EINVAL;
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(cfg_buf + offset);
            uint8_t ep_xfer = ep->bmAttributes & 3U;
            uint8_t ep_num = ep->bEndpointAddress & 0x0FU;
            bool is_in = (ep->bEndpointAddress & 0x80U) != 0;
            uint16_t mps = ep->wMaxPacketSize & 0x7FFU;
            uint8_t tx_per_uframe = (ep->wMaxPacketSize >> 11) & 0x3U;

            ep_count++;
            if (!is_in || ep_num == 0 || ep_xfer != 3U || ep_count > 1)
                return -EINVAL;
            if (tx_per_uframe == 3U || mps == 0 || mps > 1024)
                return -EINVAL;
            if (speed_cls == XHCI_PORT_SPEED_SUPER) {
                if (tx_per_uframe != 0 || ep->bInterval < 1 || ep->bInterval > 16)
                    return -EINVAL;
                uint32_t comp_off = offset + len;
                if (comp_off + sizeof(struct usb_ss_ep_companion_descriptor) > cfg_len)
                    return -EINVAL;
                uint8_t clen = cfg_buf[comp_off];
                uint8_t ctype = cfg_buf[comp_off + 1];
                if (clen < sizeof(struct usb_ss_ep_companion_descriptor) ||
                    comp_off + clen > cfg_len ||
                    ctype != USB_DESC_TYPE_SS_EP_COMPANION)
                    return -EINVAL;
                const struct usb_ss_ep_companion_descriptor *comp =
                    (const struct usb_ss_ep_companion_descriptor *)(cfg_buf + comp_off);
                if (comp->bMaxBurst > 15 || comp->wBytesPerInterval == 0 ||
                    comp->wBytesPerInterval > 16)
                    return -EINVAL;
            } else if (speed_cls == XHCI_PORT_SPEED_HIGH) {
                if (ep->bInterval < 1 || ep->bInterval > 16)
                    return -EINVAL;
            } else {
                if (tx_per_uframe != 0 || mps > 64 || ep->bInterval == 0)
                    return -EINVAL;
            }
            status_dci = (uint8_t)(ep_num * 2U + 1U);
            found_intr_in = true;
        }
        offset += len;
    }

    if (found_hub_iface && found_intr_in && ep_count == 1) {
        if (out_status_dci)
            *out_status_dci = status_dci;
        return 0;
    }
    return -EINVAL;
}

int xhci_hub_validate_config(const struct xhci_slot *slot, const uint8_t *cfg_buf,
                             uint16_t cfg_len)
{
    return xhci_hub_validate_config_dci(slot, cfg_buf, cfg_len, NULL);
}

static bool hub_psi_is_gen1_x1(const struct xhci_controller *ctrl,
                               const struct xhci_slot *slot)
{
    if (!ctrl || !slot || slot->loc.speed_class != XHCI_PORT_SPEED_SUPER)
        return false;
    uint8_t root_port = slot->loc.root_port;
    uint8_t speed_id = slot->loc.speed_id;
    if (speed_id == 0 || speed_id >= 16)
        return false;

    if (root_port < 256 && ctrl->port_cap_idx[root_port] != 0) {
        const struct xhci_proto_cap *cap =
            &ctrl->proto_caps[ctrl->port_cap_idx[root_port] - 1];
        if (cap->rev_major != 3)
            return false;
        if (cap->psic > 0) {
            const struct xhci_psi_entry *psi = &cap->psi[speed_id];
            if (!psi->valid || psi->speed_class != XHCI_PORT_SPEED_SUPER ||
                psi->plt != 0)
                return false;
            uint64_t kbps = 0;
            switch (psi->psie) {
            case 0: kbps = (uint64_t)psi->psim / 1000ULL; break;
            case 1: kbps = (uint64_t)psi->psim; break;
            case 2: kbps = (uint64_t)psi->psim * 1000ULL; break;
            case 3: kbps = (uint64_t)psi->psim * 1000000ULL; break;
            }
            return kbps == 5000000ULL;
        }
        return speed_id == XHCI_PORT_SPEED_SUPER;
    }
    return speed_id == XHCI_PORT_SPEED_SUPER;
}

int xhci_hub_check_gen1(const struct xhci_controller *ctrl, uint8_t slot_id,
                        const uint8_t *bos, size_t actual)
{
    if (!ctrl || !bos || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id])
        return -EINVAL;
    const struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!hub_psi_is_gen1_x1(ctrl, slot))
        return -ENOTSUP;
    if (actual < 5 || bos[0] != 5 || bos[1] != USB_DT_BOS)
        return -EINVAL;

    uint16_t total_len = le16_at(bos + 2);
    uint8_t num_caps = bos[4];
    if (total_len < 5 || total_len > actual)
        return -EINVAL;

    uint32_t offset = 5;
    uint8_t caps_seen = 0;
    bool saw_ss_cap = false;

    while (offset < total_len) {
        if (offset + 3 > total_len)
            return -EINVAL;
        uint8_t cap_len = bos[offset];
        uint8_t cap_type = bos[offset + 1];
        uint8_t dev_cap_type = bos[offset + 2];
        if (cap_len < 3 || cap_type != USB_DT_DEVICE_CAPABILITY ||
            offset + cap_len > total_len)
            return -EINVAL;

        if (dev_cap_type == USB_CAP_TYPE_SUPERSPEED_USB) {
            if (cap_len < 10)
                return -EINVAL;
            uint16_t speeds = le16_at(bos + offset + 4);
            if ((speeds & 0x0008U) == 0)
                return -ENOTSUP;
            saw_ss_cap = true;
        } else if (dev_cap_type == USB_CAP_TYPE_SUPERSPEEDPLUS_USB) {
            return -ENOTSUP;
        }

        caps_seen++;
        offset += cap_len;
    }

    if (offset != total_len || caps_seen != num_caps || !saw_ss_cap)
        return -EINVAL;
    return 0;
}

int xhci_hub_set_depth(struct xhci_controller *ctrl, uint8_t slot,
                       uint8_t depth, uint64_t deadline_ns)
{
    if (!ctrl || slot < 1 || slot > XHCI_MAX_MANAGED_SLOTS || depth > 4)
        return -EINVAL;
    struct xhci_setup setup = {
        .type = USB_RT_HUB_OUT,
        .request = USB_REQ_SET_HUB_DEPTH,
        .value = depth,
        .index = 0,
        .length = 0
    };
    return xhci_control_no_data_until(ctrl, slot, &setup, deadline_ns);
}

int xhci_hub_read_info(struct xhci_controller *ctrl, uint8_t slot,
                       uint64_t deadline_ns, struct xhci_hub_info *out_info)
{
    if (!ctrl || !out_info || slot < 1 || slot > XHCI_MAX_MANAGED_SLOTS)
        return -EINVAL;

    const struct xhci_slot *slot_ptr = ctrl->slots[slot];
    bool is_ss = false;
    if (slot_ptr) {
        uint8_t sc = slot_ptr->loc.speed_class ? slot_ptr->loc.speed_class
                                               : slot_ptr->speed;
        is_ss = slot_ptr->hub_ss || (sc == XHCI_PORT_SPEED_SUPER);
    }

    uint8_t want_type = is_ss ? USB_DT_SS_HUB : USB_DT_HUB;
    uint16_t want_len = is_ss ? 12U : 15U;
    struct xhci_setup setup = {
        .type = USB_RT_HUB_IN,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)((uint16_t)want_type << 8),
        .index = 0,
        .length = want_len
    };
    uint8_t buf[15];
    memset(buf, 0, sizeof(buf));
    int rc = xhci_control_in(ctrl, slot, &setup, buf, want_len, deadline_ns);
    int min_rc = is_ss ? 11 : 9;
    if (rc < min_rc) return rc < 0 ? rc : -EIO;

    uint8_t desc_len = buf[0];
    uint8_t desc_type = buf[1];
    uint8_t nports = buf[2];
    if (desc_type != want_type || nports == 0 || nports > 15)
        return -EINVAL;

    uint8_t bitmap_bytes = (uint8_t)((nports + 7U) / 8U);
    uint8_t min_len = is_ss ? (uint8_t)(10U + bitmap_bytes)
                            : (uint8_t)(7U + 2U * bitmap_bytes);
    if (desc_len < min_len || rc < (int)min_len)
        return -EINVAL;

    uint16_t chars = le16_at(buf + 3);
    memset(out_info, 0, sizeof(*out_info));
    out_info->nports = nports;
    out_info->characteristics = chars;
    out_info->pwr_good_2ms = buf[5];
    out_info->ttt = is_ss ? 0U : (uint8_t)((chars >> 5) & 0x3U);
    out_info->superspeed = is_ss;
    out_info->global_overcurrent = (((chars >> 3) & 0x3U) == 0U);
    return 0;
}

int xhci_hub_prepare_ports(struct xhci_controller *ctrl, uint8_t slot,
                           const struct xhci_hub_info *info,
                           uint64_t deadline_ns, uint16_t *candidate_mask)
{
    if (candidate_mask) *candidate_mask = 0;
    if (!ctrl || !info || info->nports == 0 || info->nports > 15)
        return -EINVAL;

    uint16_t hub_st = 0, hub_chg = 0;
    int rc = hub_get_status(ctrl, slot, USB_RT_HUB_IN, 0, deadline_ns, &hub_st, &hub_chg);
    if (rc != 0) return rc;
    if (hub_st & USB_HUB_STAT_OVERCURRENT)
        return -EIO;

    bool powered_any = false;
    uint16_t pwr_bit = info->superspeed ? USB_SS_PORT_STAT_POWER : USB_PORT_STAT_POWER;
    for (uint8_t p = 1; p <= info->nports; p++) {
        uint16_t st = 0, chg = 0;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, p, deadline_ns, &st, &chg);
        if (rc != 0) return rc;
        if (!(st & pwr_bit)) {
            rc = hub_port_feature(ctrl, slot, USB_REQ_SET_FEATURE,
                                  USB_PORT_FEAT_POWER, p, deadline_ns);
            if (rc != 0) return rc;
            powered_any = true;
        }
    }

    if (powered_any && info->pwr_good_2ms > 0) {
        rc = xhci_wait_min_ns((uint64_t)info->pwr_good_2ms * 2000000ULL, deadline_ns);
        if (rc != 0) return rc;
    }

    uint64_t stable_since[16];
    memset(stable_since, 0, sizeof(stable_since));
    uint16_t mask = 0;
    uint64_t now = arch_clocksource_read_ns();
    uint64_t window_end = now + 100000000ULL;

    for (;;) {
        bool pending_debounce = false;
        now = arch_clocksource_read_ns();
        for (uint8_t p = 1; p <= info->nports; p++) {
            uint16_t st = 0, chg = 0;
            rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, p, deadline_ns, &st, &chg);
            if (rc != 0) return rc;
            if (chg & USB_PORT_STAT_C_CONNECTION) {
                rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                                      USB_PORT_FEAT_C_CONNECTION, p, deadline_ns);
                if (rc != 0) return rc;
                stable_since[p] = 0;
            }
            if ((st & USB_PORT_STAT_CONNECTION) && !(st & USB_PORT_STAT_OVERCURRENT)) {
                if (stable_since[p] == 0)
                    stable_since[p] = now;
                if (now - stable_since[p] >= 100000000ULL) {
                    mask |= (uint16_t)(1U << p);
                } else {
                    pending_debounce = true;
                }
            } else {
                stable_since[p] = 0;
            }
        }
        if (now >= window_end && !pending_debounce)
            break;
        if (now >= deadline_ns)
            break;
        rc = xhci_wait_min_ns(10000000ULL, deadline_ns);
        if (rc != 0) break;
    }

    if (candidate_mask) *candidate_mask = mask;
    return 0;
}

int xhci_hub_reset_port(struct xhci_controller *ctrl, uint8_t slot,
                        const struct xhci_hub_info *info, uint8_t port,
                        uint64_t deadline_ns, uint8_t *speed_class,
                        bool *reset_issued)
{
    if (reset_issued) *reset_issued = false;
    if (speed_class) *speed_class = 0;
    if (!ctrl || !info || port < 1 || port > info->nports)
        return -EINVAL;

    uint16_t st = 0, chg = 0;
    int rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st, &chg);
    if (rc != 0) return rc;
    if (!(st & USB_PORT_STAT_CONNECTION) || (st & USB_PORT_STAT_OVERCURRENT))
        return -ENODEV;

    if (chg & USB_PORT_STAT_C_RESET) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_RESET, port, deadline_ns);
        if (rc != 0) return rc;
    }
    if (info->superspeed && (chg & USB_PORT_STAT_C_BH_PORT_RESET)) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_BH_PORT_RESET, port, deadline_ns);
        if (rc != 0) return rc;
    }
    while (st & USB_PORT_STAT_RESET) {
        rc = xhci_wait_min_ns(10000000ULL, deadline_ns);
        if (rc != 0) return rc;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st, &chg);
        if (rc != 0) return rc;
    }

    uint8_t init_pls = (uint8_t)((st >> 5) & 0x0FU);
    bool warm = info->superspeed &&
                (init_pls == XHCI_PLS_INACTIVE || init_pls == XHCI_PLS_COMPLIANCE);
    uint16_t reset_feat = warm ? USB_PORT_FEAT_BH_PORT_RESET : USB_PORT_FEAT_RESET;

    rc = hub_port_feature(ctrl, slot, USB_REQ_SET_FEATURE,
                          reset_feat, port, deadline_ns);
    if (rc != 0) return rc;
    if (reset_issued) *reset_issued = true;

    for (;;) {
        rc = xhci_wait_min_ns(10000000ULL, deadline_ns);
        if (rc != 0) return rc;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st, &chg);
        if (rc != 0) return rc;
        if (!(st & USB_PORT_STAT_RESET) && (chg & USB_PORT_STAT_C_RESET) &&
            (!warm || (chg & USB_PORT_STAT_C_BH_PORT_RESET)))
            break;
        if (!(st & USB_PORT_STAT_CONNECTION) && !(st & USB_PORT_STAT_RESET))
            break;
    }

    if ((st & USB_PORT_STAT_RESET) || !(chg & USB_PORT_STAT_C_RESET) ||
        (warm && !(chg & USB_PORT_STAT_C_BH_PORT_RESET)))
        return (st & USB_PORT_STAT_CONNECTION) ? -EIO : -ENODEV;

    if (chg & USB_PORT_STAT_C_RESET) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_RESET, port, deadline_ns);
        if (rc != 0) return rc;
    }
    if (info->superspeed && (chg & USB_PORT_STAT_C_BH_PORT_RESET)) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_BH_PORT_RESET, port, deadline_ns);
        if (rc != 0) return rc;
    }
    if (chg & USB_PORT_STAT_C_ENABLE) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_ENABLE, port, deadline_ns);
        if (rc != 0) return rc;
    }

    if (info->superspeed) {
        uint16_t st2 = 0, chg2 = 0;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st2, &chg2);
        if (rc != 0) return rc;
        if ((chg2 & USB_PORT_STAT_C_RESET) ||
            (warm && (chg2 & USB_PORT_STAT_C_BH_PORT_RESET)))
            return -EIO;
        if (!(st2 & USB_PORT_STAT_CONNECTION))
            return -ENODEV;
        uint8_t pls = (uint8_t)((st2 >> 5) & 0x0FU);
        if (!(st2 & USB_PORT_STAT_ENABLE) || pls != XHCI_PLS_U0)
            return -EIO;
        if (speed_class) *speed_class = XHCI_PORT_SPEED_SUPER;
        return 0;
    }

    if (!(st & USB_PORT_STAT_CONNECTION) || !(st & USB_PORT_STAT_ENABLE)) {
        uint16_t st2 = 0, chg2 = 0;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st2, &chg2);
        if (rc != 0) return rc;
        if (!(st2 & USB_PORT_STAT_CONNECTION))
            return -ENODEV;
        if (!(st2 & USB_PORT_STAT_ENABLE))
            return -EIO;
        st = st2;
    }

    rc = xhci_wait_min_ns(10000000ULL, deadline_ns);
    if (rc != 0) return rc;

    if (speed_class) {
        if (st & USB_PORT_STAT_HIGH_SPEED)
            *speed_class = XHCI_PORT_SPEED_HIGH;
        else if (st & USB_PORT_STAT_LOW_SPEED)
            *speed_class = XHCI_PORT_SPEED_LOW;
        else
            *speed_class = XHCI_PORT_SPEED_FULL;
    }
    return 0;
}

int xhci_hub_isolate_usb2(struct xhci_controller *ctrl, uint8_t slot,
                          uint8_t port, uint64_t cleanup_deadline_ns)
{
    if (!ctrl || slot < 1 || slot > XHCI_MAX_MANAGED_SLOTS || port < 1)
        return -EINVAL;

    uint16_t st = 0, chg = 0;
    for (;;) {
        int rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port,
                                cleanup_deadline_ns, &st, &chg);
        if (rc != 0) return rc;
        if (!(st & USB_PORT_STAT_RESET))
            break;
        rc = xhci_wait_min_ns(10000000ULL, cleanup_deadline_ns);
        if (rc != 0) return rc;
    }

    if (!(st & USB_PORT_STAT_CONNECTION) && !(st & USB_PORT_STAT_ENABLE))
        return 0;

    if (st & USB_PORT_STAT_ENABLE) {
        int rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                                  USB_PORT_FEAT_ENABLE, port, cleanup_deadline_ns);
        if (rc != 0) return rc;

        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port,
                            cleanup_deadline_ns, &st, &chg);
        if (rc != 0) return rc;
        if ((st & USB_PORT_STAT_RESET) || (st & USB_PORT_STAT_ENABLE))
            return -EIO;
    }
    return 0;
}

static void hub_log_device(const struct xhci_controller *ctrl,
                           const struct xhci_slot *slot)
{
    uint8_t bus = ctrl->pdev ? ctrl->pdev->bus : 0;
    uint8_t dev = ctrl->pdev ? ctrl->pdev->slot : 0;
    uint8_t fn = ctrl->pdev ? ctrl->pdev->fn : 0;
    uint8_t ic = slot->msc_iface.active ? 0x08 : slot->iface.class_code;
    uint8_t isc = slot->msc_iface.active ? 0x06 : slot->iface.subclass_code;
    uint8_t ip = slot->msc_iface.active ? 0x50 : slot->iface.protocol;
    (void)bus; (void)dev; (void)fn; (void)ic; (void)isc; (void)ip;
    log_info("[xhci] device bdf=%02x:%02x.%u root=%u route=0x%05x slot=%u parent=%u port=%u speed=%u speed_id=%u iface=%02x:%02x:%02x\n",
             (unsigned)bus, (unsigned)dev, (unsigned)fn,
             (unsigned)slot->loc.root_port, (unsigned)slot->loc.route,
             (unsigned)slot->slot_id, (unsigned)slot->loc.parent_slot,
             (unsigned)slot->loc.parent_port, (unsigned)slot->loc.speed_class,
             (unsigned)slot->loc.speed_id, (unsigned)ic, (unsigned)isc, (unsigned)ip);
}

static int hub_probe_location(struct xhci_controller *ctrl,
                              const struct xhci_location *loc,
                              uint64_t deadline_ns,
                              const struct xhci_hub_info *parent_hub,
                              uint8_t *hub_queue, uint8_t *q_tail)
{
    uint8_t slot_id = 0;
    int rc = xhci_enumerate_at(ctrl, loc, NULL, deadline_ns, &slot_id);
    if (rc != 0) {
        log_warn("[xhci-hub] root=%u route=0x%05x: enumerate failed (rc=%d)\n",
                 (unsigned)loc->root_port, (unsigned)loc->route, rc);
        return rc;
    }

    struct xhci_slot *slot = ctrl->slots[slot_id];
    struct usb_device_descriptor desc;
    rc = xhci_get_device_descriptor(ctrl, slot_id, &desc);
    if (rc != 0) {
        xhci_free_slot(ctrl, slot_id);
        return rc;
    }

    struct usb_config_descriptor cfg;
    rc = xhci_get_config_descriptor(ctrl, slot_id, &cfg);
    if (rc <= 0) {
        xhci_free_slot(ctrl, slot_id);
        return rc < 0 ? rc : -EIO;
    }
    uint16_t cfg_actual_len = (uint16_t)rc;

    uint8_t hub_status_dci = 0;
    if (desc.bDeviceClass == USB_CLASS_HUB) {
        rc = xhci_hub_validate_config_dci(slot, (const uint8_t *)slot->dma_buf_virt,
                                          cfg_actual_len, &hub_status_dci);
        if (rc != 0) {
            xhci_free_slot(ctrl, slot_id);
            return rc;
        }
    }

    rc = xhci_setup_endpoint_rings(slot, (const uint8_t *)slot->dma_buf_virt,
                                   cfg_actual_len);
    if (rc != 0) {
        xhci_free_slot(ctrl, slot_id);
        return rc;
    }
    if (desc.bDeviceClass == USB_CLASS_HUB) {
        slot->hub_status_dci = hub_status_dci;
    }

    rc = xhci_configure_endpoints(ctrl, slot_id);
    if (rc != 0) {
        xhci_free_slot(ctrl, slot_id);
        return rc;
    }

    rc = xhci_set_configuration(ctrl, slot_id, cfg.bConfigurationValue);
    if (rc != 0) {
        xhci_free_slot(ctrl, slot_id);
        return rc;
    }

    uint16_t total_len = (uint16_t)(sizeof(struct usb_device_descriptor) + cfg_actual_len);
    slot->raw_descriptors = kmalloc(total_len);
    if (slot->raw_descriptors) {
        memcpy(slot->raw_descriptors, &slot->desc, sizeof(struct usb_device_descriptor));
        memcpy(slot->raw_descriptors + sizeof(struct usb_device_descriptor),
               slot->dma_buf_virt, cfg_actual_len);
        slot->raw_descriptors_len = total_len;
    }

    if (desc.bDeviceClass == USB_CLASS_HUB) {
        /* Enumeration above may have spent several seconds logging (e.g. fb scroll);
         * give hub descriptor / BOS / configuration steps a fresh 5s window. */
        deadline_ns = arch_clocksource_read_ns() + 5000000000ULL;
        if (slot->loc.speed_class == XHCI_PORT_SPEED_SUPER) {
            uint8_t bos_hdr[5];
            struct xhci_setup bos_setup = {
                .type = 0x80,
                .request = USB_REQ_GET_DESCRIPTOR,
                .value = ((uint16_t)USB_DT_BOS << 8),
                .index = 0,
                .length = 5
            };
            rc = xhci_control_in(ctrl, slot_id, &bos_setup, bos_hdr, sizeof(bos_hdr),
                                 deadline_ns);
            if (rc < 5 || bos_hdr[0] != 5 || bos_hdr[1] != USB_DT_BOS) {
                log_warn("[xhci-hub] slot %u: BOS header read failed (rc=%d)\n", slot_id, rc);
                xhci_free_slot(ctrl, slot_id);
                return rc < 0 ? rc : -EINVAL;
            }
            uint16_t bos_total = le16_at(bos_hdr + 2);
            if (bos_total < 5 || bos_total > 512) {
                log_warn("[xhci-hub] slot %u: BOS length %u out of bounds\n",
                         slot_id, (unsigned)bos_total);
                xhci_free_slot(ctrl, slot_id);
                return -EINVAL;
            }
            uint8_t *bos_buf = kmalloc(bos_total);
            if (!bos_buf) {
                xhci_free_slot(ctrl, slot_id);
                return -ENOMEM;
            }
            bos_setup.length = bos_total;
            rc = xhci_control_in(ctrl, slot_id, &bos_setup, bos_buf, bos_total,
                                 deadline_ns);
            if (rc >= 0)
                rc = xhci_hub_check_gen1(ctrl, slot_id, bos_buf, (size_t)rc);
            kfree(bos_buf);
            if (rc != 0) {
                log_warn("[xhci-hub] slot %u: SS Gen1x1 verification failed (rc=%d)\n",
                         slot_id, rc);
                xhci_free_slot(ctrl, slot_id);
                return rc;
            }
        }

        struct xhci_hub_info hub_info;
        rc = xhci_hub_read_info(ctrl, slot_id, deadline_ns, &hub_info);
        if (rc != 0) {
            log_warn("[xhci-hub] slot %u: read_info failed (rc=%d)\n", slot_id, rc);
            xhci_free_slot(ctrl, slot_id);
            return rc;
        }

        rc = xhci_configure_hub_slot(ctrl, slot_id, &hub_info, deadline_ns);
        if (rc == -EAGAIN) {
            if (!slot->raw_descriptors) {
                xhci_free_slot(ctrl, slot_id);
                return -ENOMEM;
            }
            uint8_t *id_copy = kmalloc(slot->raw_descriptors_len);
            if (!id_copy) {
                xhci_free_slot(ctrl, slot_id);
                return -ENOMEM;
            }
            memcpy(id_copy, slot->raw_descriptors, slot->raw_descriptors_len);
            struct xhci_enum_seed seed = {
                .hub = hub_info,
                .identity = id_copy,
                .identity_len = slot->raw_descriptors_len
            };
            xhci_free_slot(ctrl, slot_id);
            slot_id = 0;

            if (loc->depth == 0) {
                uint8_t dummy_speed = 0;
                rc = xhci_port_reset_force(ctrl, loc->root_port, &dummy_speed);
            } else if (parent_hub) {
                uint8_t dummy_sc = 0;
                bool dummy_ri = false;
                rc = xhci_hub_reset_port(ctrl, loc->parent_slot, parent_hub,
                                         loc->parent_port, deadline_ns,
                                         &dummy_sc, &dummy_ri);
            } else {
                rc = -EIO;
            }
            if (rc == 0) {
                deadline_ns = arch_clocksource_read_ns() + 5000000000ULL;
                rc = xhci_enumerate_at(ctrl, loc, &seed, deadline_ns, &slot_id);
            }
            kfree(id_copy);
            if (rc != 0) {
                log_warn("[xhci-hub] root=%u route=0x%05x: hub rebuild failed (rc=%d)\n",
                         (unsigned)loc->root_port, (unsigned)loc->route, rc);
                return rc;
            }
            slot = ctrl->slots[slot_id];
            deadline_ns = arch_clocksource_read_ns() + 5000000000ULL;
        } else if (rc != 0) {
            log_warn("[xhci-hub] slot %u: configure_hub_slot failed (rc=%d)\n", slot_id, rc);
            xhci_free_slot(ctrl, slot_id);
            return rc;
        }

        if (hub_info.superspeed) {
            if (loc->depth > 4) {
                xhci_free_slot(ctrl, slot_id);
                return -ENOTSUP;
            }
            rc = xhci_hub_set_depth(ctrl, slot_id, loc->depth, deadline_ns);
            if (rc != 0) {
                log_warn("[xhci-hub] slot %u: SET_HUB_DEPTH failed (rc=%d)\n", slot_id, rc);
                xhci_free_slot(ctrl, slot_id);
                return rc;
            }
        }

        uint8_t bus = ctrl->pdev ? ctrl->pdev->bus : 0;
        uint8_t dev = ctrl->pdev ? ctrl->pdev->slot : 0;
        uint8_t fn = ctrl->pdev ? ctrl->pdev->fn : 0;
        (void)bus; (void)dev; (void)fn;
        log_info("[xhci-hub] ready bdf=%02x:%02x.%u root=%u route=0x%05x slot=%u protocol=%s ports=%u ttt=%u\n",
                 (unsigned)bus, (unsigned)dev, (unsigned)fn,
                 (unsigned)slot->loc.root_port, (unsigned)slot->loc.route,
                 (unsigned)slot->slot_id, hub_info.superspeed ? "usb3" : "usb2",
                 (unsigned)hub_info.nports, (unsigned)hub_info.ttt);

        if (loc->depth < 5 && *q_tail < XHCI_MAX_MANAGED_SLOTS) {
            hub_queue[(*q_tail)++] = slot_id;
        }
    }

    hub_log_device(ctrl, slot);
    return 0;
}

void xhci_hub_probe_tree(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->op_regs) return;

    uint8_t hub_queue[XHCI_MAX_MANAGED_SLOTS];
    uint8_t q_head = 0, q_tail = 0;

    /* Tier 0: root ports */
    for (uint32_t p = 1; p <= ctrl->max_ports; p++) {
        if (ctrl->poisoned || ctrl->default_addr_locked_out)
            return;
        uint8_t speed = 0;
        if (xhci_port_reset(ctrl, p, &speed) != 0)
            continue;
        uint8_t speed_cls = xhci_speed_class(ctrl, (uint8_t)p, speed);
        struct xhci_location loc = {
            .route = 0,
            .root_port = (uint8_t)p,
            .parent_slot = 0,
            .parent_port = 0,
            .depth = 0,
            .speed_id = speed,
            .speed_class = speed_cls,
            .tt_hub_slot = 0,
            .tt_port = 0
        };
        uint64_t dev_deadline = arch_clocksource_read_ns() + 5000000000ULL;
        hub_probe_location(ctrl, &loc, dev_deadline, NULL, hub_queue, &q_tail);
    }

    /* Tiered BFS over downstream hubs */
    while (q_head < q_tail) {
        uint8_t tier_end = q_tail;
        while (q_head < tier_end) {
            uint8_t hub_slot_id = hub_queue[q_head++];
            if (ctrl->poisoned || ctrl->default_addr_locked_out)
                return;
            if (!xhci_slot_transport_usable(ctrl, hub_slot_id))
                continue;
            struct xhci_slot *hub_slot = ctrl->slots[hub_slot_id];
            if (!hub_slot || !hub_slot->is_hub || hub_slot->loc.depth >= 5)
                continue;

            uint64_t hub_deadline = arch_clocksource_read_ns() + 5000000000ULL;
            struct xhci_hub_info hub_info = {
                .nports = hub_slot->hub_nports,
                .pwr_good_2ms = 50,
                .ttt = hub_slot->hub_ttt,
                .superspeed = hub_slot->hub_ss
            };
            int hrc = xhci_hub_read_info(ctrl, hub_slot_id, hub_deadline, &hub_info);
            if (hrc != 0) {
                log_warn("[xhci-hub] slot %u: bfs read_info failed (rc=%d)\n",
                         (unsigned)hub_slot_id, hrc);
                continue;
            }

            hub_deadline = arch_clocksource_read_ns() + 5000000000ULL;
            uint16_t cand_mask = 0;
            hrc = xhci_hub_prepare_ports(ctrl, hub_slot_id, &hub_info,
                                         hub_deadline, &cand_mask);
            if (hrc != 0) {
                log_warn("[xhci-hub] slot %u: prepare_ports failed (rc=%d)\n",
                         (unsigned)hub_slot_id, hrc);
                continue;
            }
            if (cand_mask == 0) {
                log_info("[xhci-hub] slot %u: no downstream ports connected\n",
                         (unsigned)hub_slot_id);
                continue;
            }

            for (uint8_t port = 1; port <= hub_info.nports; port++) {
                if (!(cand_mask & (uint16_t)(1U << port)))
                    continue;
                if (ctrl->poisoned || ctrl->default_addr_locked_out)
                    return;

                uint64_t port_deadline = arch_clocksource_read_ns() + 5000000000ULL;
                uint8_t child_sc = 0;
                bool reset_issued = false;
                int rc = xhci_hub_reset_port(ctrl, hub_slot_id, &hub_info, port,
                                             port_deadline, &child_sc, &reset_issued);
                if (rc != 0) {
                    log_warn("[xhci-hub] slot %u port %u: reset failed (rc=%d, issued=%d)\n",
                             (unsigned)hub_slot_id, (unsigned)port, rc, (int)reset_issued);
                    if (reset_issued) {
                        uint64_t iso_dl = arch_clocksource_read_ns() + 1000000000ULL;
                        if (xhci_hub_isolate_usb2(ctrl, hub_slot_id, port, iso_dl) != 0) {
                            ctrl->default_addr_locked_out = true;
                            return;
                        }
                    }
                    continue;
                }

                struct xhci_location child_loc;
                rc = xhci_child_location(ctrl, hub_slot_id, port, child_sc, &child_loc);
                if (rc != 0) {
                    log_warn("[xhci-hub] slot %u port %u: child_location failed (rc=%d)\n",
                             (unsigned)hub_slot_id, (unsigned)port, rc);
                    uint64_t iso_dl = arch_clocksource_read_ns() + 1000000000ULL;
                    if (xhci_hub_isolate_usb2(ctrl, hub_slot_id, port, iso_dl) != 0) {
                        ctrl->default_addr_locked_out = true;
                        return;
                    }
                    continue;
                }
                port_deadline = arch_clocksource_read_ns() + 5000000000ULL;
                hub_probe_location(ctrl, &child_loc, port_deadline, &hub_info,
                                   hub_queue, &q_tail);
            }
        }
    }
}
