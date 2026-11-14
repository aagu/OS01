/* kernel/arch/x86_64/driver/xhci_msc.c — xHCI USB Mass Storage adapter */
#include <driver/usb_msc.h>
#include <driver/xhci.h>
#include <errno.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <subsys/subsys.h>

#define XHCI_MSC_MAX_DISKS 26

struct xhci_msc_ctx {
    struct xhci_controller *ctrl;
    uint8_t slot_id;
    uint8_t in_addr;
    uint8_t out_addr;
    uint8_t in_dci;
    uint8_t out_dci;
};

static struct xhci_msc_ctx s_msc_ctxs[XHCI_MSC_MAX_DISKS];

static int xhci_msc_control_no_data(void *ctx, uint8_t request_type, uint8_t request,
                                    uint16_t value, uint16_t index)
{
    struct xhci_msc_ctx *c = ctx;
    if (!c || !c->ctrl)
        return -EINVAL;
    return xhci_control_no_data(c->ctrl, c->slot_id, request_type, request, value, index);
}

static int xhci_msc_bulk(void *ctx, uint8_t ep_addr, void *buf, size_t len, size_t *actual)
{
    struct xhci_msc_ctx *c = ctx;
    if (!c || !c->ctrl)
        return -EINVAL;

    uint8_t dci;
    if (ep_addr == c->in_addr)
        dci = c->in_dci;
    else if (ep_addr == c->out_addr)
        dci = c->out_dci;
    else
        return -EINVAL;

    return xhci_bulk_transfer(c->ctrl, c->slot_id, dci, buf, len, actual);
}

static int xhci_msc_clear_halt(void *ctx, uint8_t ep_addr)
{
    struct xhci_msc_ctx *c = ctx;
    if (!c || !c->ctrl)
        return -EINVAL;
    if (ep_addr != c->in_addr && ep_addr != c->out_addr)
        return -EINVAL;
    return xhci_clear_halt(c->ctrl, c->slot_id, ep_addr);
}

static int xhci_msc_recover_timeout(void *ctx, uint8_t ep_addr)
{
    struct xhci_msc_ctx *c = ctx;
    if (!c || !c->ctrl)
        return -EINVAL;

    uint8_t dci;
    if (ep_addr == c->in_addr)
        dci = c->in_dci;
    else if (ep_addr == c->out_addr)
        dci = c->out_dci;
    else
        return -EINVAL;

    return xhci_recover_bulk_timeout(c->ctrl, c->slot_id, dci);
}

static int xhci_msc_init(void)
{
    uint8_t next_disk = 0;
    uint8_t nctrl = xhci_controller_count();

    for (uint8_t ci = 0; ci < nctrl; ci++) {
        struct xhci_controller *ctrl = xhci_controller_get(ci);
        if (!ctrl || !ctrl->running || ctrl->poisoned)
            continue;

        for (uint8_t si = 1; si <= XHCI_MAX_MANAGED_SLOTS; si++) {
            struct xhci_slot *slot = ctrl->slots[si];
            if (!slot || !slot->active || !slot->configured ||
                slot->disabled || slot->poisoned || !slot->msc_iface.active)
                continue;

            if (next_disk >= XHCI_MSC_MAX_DISKS) {
                log_warn("xhci_msc: disk name limit (usba..usbz) reached\n");
                return 0;
            }

            struct xhci_msc_ctx *ctx = &s_msc_ctxs[next_disk];
            ctx->ctrl = ctrl;
            ctx->slot_id = slot->slot_id;
            ctx->in_addr = slot->msc_iface.in_addr;
            ctx->out_addr = slot->msc_iface.out_addr;
            ctx->in_dci = slot->msc_iface.in_dci;
            ctx->out_dci = slot->msc_iface.out_dci;

            struct usb_msc_transport transport = {
                .ctx = ctx,
                .control_no_data = xhci_msc_control_no_data,
                .bulk = xhci_msc_bulk,
                .clear_halt = xhci_msc_clear_halt,
                .recover_timeout = xhci_msc_recover_timeout,
            };

            char name[5] = { 'u', 's', 'b', (char)('a' + next_disk), '\0' };
            int rc = usb_msc_probe(&transport,
                                   slot->msc_iface.interface_num,
                                   slot->msc_iface.in_addr,
                                   slot->msc_iface.out_addr,
                                   name,
                                   NULL);
            if (rc == 0)
                next_disk++;
        }
    }

    return 0;
}

static const char *const xhci_msc_requires[] = { "device-boot" };

static int xhci_msc_register(void)
{
    return register_subsys_deps("usb-msc", xhci_msc_init,
                                SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL,
                                xhci_msc_requires, 1);
}
SUBSYS_INITCALL(xhci_msc_register);
