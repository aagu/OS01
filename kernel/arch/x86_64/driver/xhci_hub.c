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

int xhci_hub_validate_config(const struct xhci_slot *slot, const uint8_t *cfg_buf,
                             uint16_t cfg_len)
{
    if (!slot || !cfg_buf || cfg_len < sizeof(struct usb_config_descriptor))
        return -EINVAL;

    uint8_t speed_cls = slot->loc.speed_class ? slot->loc.speed_class : slot->speed;
    uint32_t offset = 0;
    bool in_hub_alt0 = false;
    bool found_hub_iface = false;
    bool found_intr_in = false;
    uint8_t ep_count = 0;

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
                /* Accept FS (proto 0) or HS single-TT (proto 0 or 1); reject multi-TT alt0 (proto 2). */
                if (iface->bInterfaceProtocol > 1 || iface->bNumEndpoints != 1)
                    return -EINVAL;
                found_hub_iface = true;
            }
        } else if (type == USB_DESC_TYPE_ENDPOINT && in_hub_alt0) {
            if (len < sizeof(struct usb_endpoint_descriptor))
                return -EINVAL;
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(cfg_buf + offset);
            uint8_t ep_xfer = ep->bmAttributes & 3U;
            bool is_in = (ep->bEndpointAddress & 0x80U) != 0;
            uint16_t mps = ep->wMaxPacketSize & 0x7FFU;
            uint8_t tx_per_uframe = (ep->wMaxPacketSize >> 11) & 0x3U;

            ep_count++;
            if (!is_in || ep_xfer != 3U || ep_count > 1)
                return -EINVAL;
            if (tx_per_uframe == 3U || mps == 0 || mps > 1024)
                return -EINVAL;
            if (speed_cls == XHCI_PORT_SPEED_HIGH) {
                if (ep->bInterval < 1 || ep->bInterval > 16)
                    return -EINVAL;
            } else {
                if (tx_per_uframe != 0 || mps > 64 || ep->bInterval == 0)
                    return -EINVAL;
            }
            found_intr_in = true;
        }
        offset += len;
    }

    return (found_hub_iface && found_intr_in && ep_count == 1) ? 0 : -EINVAL;
}

int xhci_hub_read_info(struct xhci_controller *ctrl, uint8_t slot,
                       uint64_t deadline_ns, struct xhci_hub_info *out_info)
{
    if (!ctrl || !out_info || slot < 1 || slot > XHCI_MAX_MANAGED_SLOTS)
        return -EINVAL;

    struct xhci_setup setup = {
        .type = USB_RT_HUB_IN,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = ((uint16_t)USB_DT_HUB << 8),
        .index = 0,
        .length = 15
    };
    uint8_t buf[15];
    memset(buf, 0, sizeof(buf));
    int rc = xhci_control_in(ctrl, slot, &setup, buf, sizeof(buf), deadline_ns);
    if (rc < 9) return rc < 0 ? rc : -EIO;

    uint8_t desc_len = buf[0];
    uint8_t desc_type = buf[1];
    uint8_t nports = buf[2];
    if (desc_type != USB_DT_HUB || nports == 0 || nports > 15)
        return -EINVAL;

    uint8_t bitmap_bytes = (uint8_t)((nports + 7U) / 8U);
    uint8_t min_len = (uint8_t)(7U + 2U * bitmap_bytes);
    if (desc_len < min_len || rc < (int)min_len)
        return -EINVAL;

    uint16_t chars = le16_at(buf + 3);
    memset(out_info, 0, sizeof(*out_info));
    out_info->nports = nports;
    out_info->characteristics = chars;
    out_info->pwr_good_2ms = buf[5];
    out_info->ttt = (uint8_t)((chars >> 5) & 0x3U);
    out_info->superspeed = false;
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
    for (uint8_t p = 1; p <= info->nports; p++) {
        uint16_t st = 0, chg = 0;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, p, deadline_ns, &st, &chg);
        if (rc != 0) return rc;
        if (!(st & USB_PORT_STAT_POWER)) {
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
    while (st & USB_PORT_STAT_RESET) {
        rc = xhci_wait_min_ns(10000000ULL, deadline_ns);
        if (rc != 0) return rc;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st, &chg);
        if (rc != 0) return rc;
    }

    rc = hub_port_feature(ctrl, slot, USB_REQ_SET_FEATURE,
                          USB_PORT_FEAT_RESET, port, deadline_ns);
    if (rc != 0) return rc;
    if (reset_issued) *reset_issued = true;

    for (;;) {
        rc = xhci_wait_min_ns(10000000ULL, deadline_ns);
        if (rc != 0) return rc;
        rc = hub_get_status(ctrl, slot, USB_RT_PORT_IN, port, deadline_ns, &st, &chg);
        if (rc != 0) return rc;
        if (!(st & USB_PORT_STAT_RESET) && (chg & USB_PORT_STAT_C_RESET))
            break;
        if (!(st & USB_PORT_STAT_CONNECTION) && !(st & USB_PORT_STAT_RESET))
            break;
    }

    if (chg & USB_PORT_STAT_C_RESET) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_RESET, port, deadline_ns);
        if (rc != 0) return rc;
    }
    if (chg & USB_PORT_STAT_C_ENABLE) {
        rc = hub_port_feature(ctrl, slot, USB_REQ_CLEAR_FEATURE,
                              USB_PORT_FEAT_C_ENABLE, port, deadline_ns);
        if (rc != 0) return rc;
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
    if (rc != 0) return rc;

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

    if (desc.bDeviceClass == USB_CLASS_HUB) {
        rc = xhci_hub_validate_config(slot, (const uint8_t *)slot->dma_buf_virt,
                                      cfg_actual_len);
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
                rc = xhci_port_reset(ctrl, loc->root_port, &dummy_speed);
            } else if (parent_hub) {
                uint8_t dummy_sc = 0;
                bool dummy_ri = false;
                rc = xhci_hub_reset_port(ctrl, loc->parent_slot, parent_hub,
                                         loc->parent_port, deadline_ns,
                                         &dummy_sc, &dummy_ri);
            } else {
                rc = -EIO;
            }
            if (rc == 0)
                rc = xhci_enumerate_at(ctrl, loc, &seed, deadline_ns, &slot_id);
            kfree(id_copy);
            if (rc != 0) return rc;
            slot = ctrl->slots[slot_id];
        } else if (rc != 0) {
            log_warn("[xhci-hub] slot %u: configure_hub_slot failed (rc=%d)\n", slot_id, rc);
            xhci_free_slot(ctrl, slot_id);
            return rc;
        }

        uint8_t bus = ctrl->pdev ? ctrl->pdev->bus : 0;
        uint8_t dev = ctrl->pdev ? ctrl->pdev->slot : 0;
        uint8_t fn = ctrl->pdev ? ctrl->pdev->fn : 0;
        (void)bus; (void)dev; (void)fn;
        log_info("[xhci-hub] ready bdf=%02x:%02x.%u root=%u route=0x%05x slot=%u protocol=usb2 ports=%u ttt=%u\n",
                 (unsigned)bus, (unsigned)dev, (unsigned)fn,
                 (unsigned)slot->loc.root_port, (unsigned)slot->loc.route,
                 (unsigned)slot->slot_id, (unsigned)hub_info.nports,
                 (unsigned)hub_info.ttt);

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
    uint64_t tree_deadline = arch_clocksource_read_ns() + 10000000000ULL;

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
        hub_probe_location(ctrl, &loc, tree_deadline, NULL, hub_queue, &q_tail);
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

            struct xhci_hub_info hub_info = {
                .nports = hub_slot->hub_nports,
                .pwr_good_2ms = 50,
                .ttt = hub_slot->hub_ttt,
                .superspeed = hub_slot->hub_ss
            };
            if (xhci_hub_read_info(ctrl, hub_slot_id, tree_deadline, &hub_info) != 0)
                continue;

            uint16_t cand_mask = 0;
            if (xhci_hub_prepare_ports(ctrl, hub_slot_id, &hub_info,
                                       tree_deadline, &cand_mask) != 0)
                continue;

            for (uint8_t port = 1; port <= hub_info.nports; port++) {
                if (!(cand_mask & (uint16_t)(1U << port)))
                    continue;
                if (ctrl->poisoned || ctrl->default_addr_locked_out)
                    return;

                uint8_t child_sc = 0;
                bool reset_issued = false;
                int rc = xhci_hub_reset_port(ctrl, hub_slot_id, &hub_info, port,
                                             tree_deadline, &child_sc, &reset_issued);
                if (rc != 0) {
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
                if (xhci_child_location(ctrl, hub_slot_id, port, child_sc, &child_loc) != 0) {
                    uint64_t iso_dl = arch_clocksource_read_ns() + 1000000000ULL;
                    if (xhci_hub_isolate_usb2(ctrl, hub_slot_id, port, iso_dl) != 0) {
                        ctrl->default_addr_locked_out = true;
                        return;
                    }
                    continue;
                }
                hub_probe_location(ctrl, &child_loc, tree_deadline, &hub_info,
                                   hub_queue, &q_tail);
            }
        }
    }
}
