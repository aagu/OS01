/* kernel/arch/x86_64/driver/xhci_hid_desc.c — xHCI Boot HID config descriptor scanner */
#include <driver/xhci.h>
#include <driver/xhci_hid.h>
#include <errno.h>
#include <string.h>

static bool hid_ep_valid(const struct usb_endpoint_descriptor *ep,
                         uint8_t speed_class, enum xhci_hid_kind kind,
                         struct xhci_hid_candidate *out_cand, uint8_t iface_num)
{
    uint8_t ep_num = ep->bEndpointAddress & 0x0FU;
    uint8_t rsvd_addr = ep->bEndpointAddress & 0x70U;
    bool is_in = (ep->bEndpointAddress & 0x80U) != 0;
    if (ep_num == 0 || rsvd_addr != 0 || !is_in)
        return false;

    uint8_t dci = (uint8_t)(ep_num * 2U + 1U);
    if (dci < XHCI_MIN_ENDPOINT_DCI || dci > XHCI_MAX_ENDPOINT_DCI)
        return false;

    if ((ep->bmAttributes & 0x03U) != 3U)
        return false;

    /* Boot HID uses single-transaction Interrupt IN: bits [15:11] must be 0. */
    if ((ep->wMaxPacketSize & 0xF800U) != 0)
        return false;

    uint16_t mps = ep->wMaxPacketSize & 0x7FFU;
    uint16_t min_mps = (kind == XHCI_HID_KEYBOARD) ? 8U : 3U;
    if (mps < min_mps)
        return false;

    uint8_t interval = ep->bInterval;
    switch (speed_class) {
    case XHCI_PORT_SPEED_LOW:
        if (mps > 8U)
            return false;
        /* Many real-world Low-Speed HID devices advertise bInterval < 10 ms
         * (e.g. 8 ms or 1 ms) despite USB 2.0 §9.6.6; clamp to 10 ms. */
        if (interval < 10U)
            interval = 10U;
        break;
    case XHCI_PORT_SPEED_FULL:
        if (mps > 64U || interval < 1U)
            return false;
        break;
    case XHCI_PORT_SPEED_HIGH:
        if (mps > 1024U || interval < 1U || interval > 16U)
            return false;
        break;
    default:
        return false;
    }

    out_cand->valid = true;
    out_cand->iface = iface_num;
    out_cand->dci = dci;
    out_cand->ep_addr = ep->bEndpointAddress;
    out_cand->interval = interval;
    out_cand->mps = mps;
    return true;
}

static void hid_commit_iface(struct xhci_hid_candidate out[XHCI_HID_KINDS],
                             bool is_boot_hid, enum xhci_hid_kind kind,
                             uint8_t declared_eps, uint8_t seen_eps,
                             bool ep_valid, const struct xhci_hid_candidate *cand,
                             uint32_t non_hid_dcis)
{
    if (!is_boot_hid || declared_eps != 1U || seen_eps != 1U || !ep_valid || !cand->valid)
        return;
    if ((non_hid_dcis & (1U << cand->dci)) != 0)
        return;

    if (!out[kind].valid || cand->iface < out[kind].iface) {
        out[kind] = *cand;
    }
}

int xhci_hid_scan_config(const uint8_t *cfg, uint16_t len, uint8_t speed_class,
                         struct xhci_hid_candidate out[XHCI_HID_KINDS],
                         uint32_t *hid_skip_dcis)
{
    if (hid_skip_dcis)
        *hid_skip_dcis = 0;
    if (!cfg || !out || len < sizeof(struct usb_config_descriptor))
        return -EINVAL;

    memset(out, 0, sizeof(struct xhci_hid_candidate) * XHCI_HID_KINDS);

    uint8_t cfg_len = cfg[0];
    uint8_t cfg_type = cfg[1];
    if (cfg_len < sizeof(struct usb_config_descriptor) || cfg_len > len ||
        cfg_type != USB_DESC_TYPE_CONFIGURATION) {
        return -EINVAL;
    }

    const struct usb_config_descriptor *cd = (const struct usb_config_descriptor *)cfg;
    if (cd->wTotalLength != len)
        return -EINVAL;

    /* Pass 1: validate descriptor framing and collect MSC BOT / DCI ownership. */
    bool in_iface = false;
    bool cur_alt0 = false;
    bool cur_is_hid = false;
    bool has_msc_bot = false;
    uint32_t non_hid_dcis = 0;
    uint32_t all_hid_dcis = 0;

    uint32_t offset = cfg_len;
    while (offset < len) {
        if (offset + 2U > len)
            return -EINVAL;
        uint8_t dlen = cfg[offset];
        uint8_t dtype = cfg[offset + 1U];
        if (dlen < 2U || offset + dlen > len)
            return -EINVAL;

        if (dtype == USB_DESC_TYPE_INTERFACE) {
            if (dlen < sizeof(struct usb_interface_descriptor))
                return -EINVAL;
            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(cfg + offset);
            in_iface = true;
            cur_alt0 = (iface->bAlternateSetting == 0);
            cur_is_hid = (iface->bInterfaceClass == USB_CLASS_HID);
            if (cur_alt0 &&
                iface->bInterfaceClass == 0x08U &&
                iface->bInterfaceSubClass == 0x06U &&
                iface->bInterfaceProtocol == 0x50U) {
                has_msc_bot = true;
            }
        } else if (dtype == USB_DESC_TYPE_ENDPOINT) {
            if (!in_iface || dlen < sizeof(struct usb_endpoint_descriptor))
                return -EINVAL;
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(cfg + offset);
            uint8_t ep_num = ep->bEndpointAddress & 0x0FU;
            bool is_in = (ep->bEndpointAddress & 0x80U) != 0;
            if (ep_num != 0) {
                uint8_t dci = (uint8_t)(ep_num * 2U + (is_in ? 1U : 0U));
                if (dci >= XHCI_MIN_ENDPOINT_DCI && dci <= XHCI_MAX_ENDPOINT_DCI) {
                    if (cur_is_hid)
                        all_hid_dcis |= (1U << dci);
                    else if (cur_alt0)
                        non_hid_dcis |= (1U << dci);
                }
            }
        }

        offset += dlen;
    }

    if (has_msc_bot) {
        if (hid_skip_dcis)
            *hid_skip_dcis = all_hid_dcis & ~non_hid_dcis;
        return 0;
    }

    /* Pass 2: validate Boot HID alt-0 interfaces and select lowest iface per kind. */
    bool cur_boot_hid = false;
    enum xhci_hid_kind cur_kind = XHCI_HID_KEYBOARD;
    uint8_t cur_iface_num = 0;
    uint8_t cur_declared_eps = 0;
    uint8_t cur_seen_eps = 0;
    bool cur_ep_valid = false;
    struct xhci_hid_candidate cur_cand;
    memset(&cur_cand, 0, sizeof(cur_cand));

    offset = cfg_len;
    while (offset < len) {
        uint8_t dlen = cfg[offset];
        uint8_t dtype = cfg[offset + 1U];

        if (dtype == USB_DESC_TYPE_INTERFACE) {
            hid_commit_iface(out, cur_boot_hid, cur_kind, cur_declared_eps,
                             cur_seen_eps, cur_ep_valid, &cur_cand, non_hid_dcis);

            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(cfg + offset);
            cur_boot_hid = false;
            cur_iface_num = iface->bInterfaceNumber;
            cur_declared_eps = iface->bNumEndpoints;
            cur_seen_eps = 0;
            cur_ep_valid = false;
            memset(&cur_cand, 0, sizeof(cur_cand));

            if (iface->bAlternateSetting == 0 &&
                iface->bInterfaceClass == USB_CLASS_HID &&
                iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT) {
                if (iface->bInterfaceProtocol == USB_HID_PROTO_KEYBOARD) {
                    cur_boot_hid = true;
                    cur_kind = XHCI_HID_KEYBOARD;
                } else if (iface->bInterfaceProtocol == USB_HID_PROTO_MOUSE) {
                    cur_boot_hid = true;
                    cur_kind = XHCI_HID_MOUSE;
                }
            }
        } else if (dtype == USB_DESC_TYPE_ENDPOINT && cur_boot_hid) {
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(cfg + offset);
            cur_seen_eps++;
            if (cur_seen_eps == 1U) {
                cur_ep_valid = hid_ep_valid(ep, speed_class, cur_kind,
                                            &cur_cand, cur_iface_num);
            } else {
                cur_ep_valid = false;
            }
        }

        offset += dlen;
    }

    hid_commit_iface(out, cur_boot_hid, cur_kind, cur_declared_eps,
                     cur_seen_eps, cur_ep_valid, &cur_cand, non_hid_dcis);

    /* Reject DCI collision between selected keyboard and mouse candidates. */
    if (out[XHCI_HID_KEYBOARD].valid && out[XHCI_HID_MOUSE].valid &&
        out[XHCI_HID_KEYBOARD].dci == out[XHCI_HID_MOUSE].dci) {
        memset(out, 0, sizeof(struct xhci_hid_candidate) * XHCI_HID_KINDS);
    }

    uint32_t selected_dcis = 0;
    for (int k = 0; k < XHCI_HID_KINDS; k++) {
        if (out[k].valid)
            selected_dcis |= (1U << out[k].dci);
    }
    if (hid_skip_dcis)
        *hid_skip_dcis = all_hid_dcis & ~selected_dcis & ~non_hid_dcis;

    return 0;
}
