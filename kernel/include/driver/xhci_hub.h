/* kernel/include/driver/xhci_hub.h — xHCI USB Hub protocol and tiered enumeration */
#ifndef _KERNEL_DRIVER_XHCI_HUB_H
#define _KERNEL_DRIVER_XHCI_HUB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <driver/xhci.h>

#define USB_CLASS_HUB               0x09
#define USB_DT_BOS                  0x0F
#define USB_DT_DEVICE_CAPABILITY    0x10
#define USB_CAP_TYPE_SUPERSPEED_USB 0x0A
#define USB_CAP_TYPE_SUPERSPEEDPLUS_USB 0x0B
#define USB_DT_HUB                  0x29
#define USB_DT_SS_HUB               0x2A
#define USB_REQ_SET_HUB_DEPTH       12

/* Hub class requests */
#define USB_RT_HUB_OUT              0x20
#define USB_RT_HUB_IN               0xA0
#define USB_RT_PORT_IN              0xA3
#define USB_RT_PORT_OUT             0x23

#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_GET_DESCRIPTOR      0x06

/* Hub status bits */
#define USB_HUB_STAT_OVERCURRENT    0x0002U

/* Port status bits (wPortStatus) */
#define USB_PORT_STAT_CONNECTION    0x0001U
#define USB_PORT_STAT_ENABLE        0x0002U
#define USB_PORT_STAT_SUSPEND       0x0004U
#define USB_PORT_STAT_OVERCURRENT   0x0008U
#define USB_PORT_STAT_RESET         0x0010U
#define USB_PORT_STAT_POWER         0x0100U
#define USB_SS_PORT_STAT_POWER      0x0200U
#define USB_PORT_STAT_LOW_SPEED     0x0200U
#define USB_PORT_STAT_HIGH_SPEED    0x0400U

/* Port change bits (wPortChange) */
#define USB_PORT_STAT_C_CONNECTION  0x0001U
#define USB_PORT_STAT_C_ENABLE      0x0002U
#define USB_PORT_STAT_C_SUSPEND     0x0004U
#define USB_PORT_STAT_C_OVERCURRENT 0x0008U
#define USB_PORT_STAT_C_RESET       0x0010U
#define USB_PORT_STAT_C_BH_PORT_RESET 0x0020U

/* Port feature selectors */
#define USB_PORT_FEAT_ENABLE        1U
#define USB_PORT_FEAT_RESET         4U
#define USB_PORT_FEAT_BH_PORT_RESET 28U
#define USB_PORT_FEAT_C_BH_PORT_RESET 29U
#define USB_PORT_FEAT_POWER         8U
#define USB_PORT_FEAT_C_CONNECTION  16U
#define USB_PORT_FEAT_C_ENABLE      17U
#define USB_PORT_FEAT_C_SUSPEND     18U
#define USB_PORT_FEAT_C_OVERCURRENT 19U
#define USB_PORT_FEAT_C_RESET       20U

struct xhci_hub_info {
    uint8_t nports;
    uint8_t pwr_good_2ms;
    uint8_t ttt;
    uint16_t characteristics;
    bool superspeed;
    bool global_overcurrent;
};

struct xhci_enum_seed {
    struct xhci_hub_info hub;
    const uint8_t *identity;
    size_t identity_len;
};

int xhci_hub_validate_config(const struct xhci_slot *slot, const uint8_t *cfg_buf,
                             uint16_t cfg_len);
int xhci_hub_read_info(struct xhci_controller *ctrl, uint8_t slot,
                       uint64_t deadline_ns, struct xhci_hub_info *out_info);
int xhci_hub_prepare_ports(struct xhci_controller *ctrl, uint8_t slot,
                           const struct xhci_hub_info *info,
                           uint64_t deadline_ns, uint16_t *candidate_mask);
int xhci_hub_reset_port(struct xhci_controller *ctrl, uint8_t slot,
                        const struct xhci_hub_info *info, uint8_t port,
                        uint64_t deadline_ns, uint8_t *speed_class,
                        bool *reset_issued);
int xhci_hub_check_gen1(const struct xhci_controller *ctrl, uint8_t slot,
                        const uint8_t *bos, size_t actual);
int xhci_hub_set_depth(struct xhci_controller *ctrl, uint8_t slot,
                       uint8_t depth, uint64_t deadline_ns);
int xhci_hub_isolate_usb2(struct xhci_controller *ctrl, uint8_t slot,
                          uint8_t port, uint64_t cleanup_deadline_ns);
void xhci_hub_probe_tree(struct xhci_controller *ctrl);

#endif /* _KERNEL_DRIVER_XHCI_HUB_H */
