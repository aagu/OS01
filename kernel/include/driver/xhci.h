/* kernel/include/driver/xhci.h — xHCI Host Controller Driver definitions */
#ifndef _DRIVER_XHCI_H
#define _DRIVER_XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

/* Ring and TRB constants */
#define XHCI_TRB_RING_SIZE          256
#define XHCI_ERDP_EHB               (1ULL << 3)
#define XHCI_TRB_TYPE_NORMAL        1
#define XHCI_TRB_TYPE_SETUP_STAGE   2
#define XHCI_TRB_TYPE_DATA_STAGE    3
#define XHCI_TRB_TYPE_STATUS_STAGE  4
#define XHCI_TRB_TYPE_LINK          6
#define XHCI_TRB_TYPE_ENABLE_SLOT   9
#define XHCI_TRB_TYPE_DISABLE_SLOT  10
#define XHCI_TRB_TYPE_ADDRESS_DEV   11
#define XHCI_TRB_TYPE_CONFIG_EP     12
#define XHCI_TRB_TYPE_TRANSFER_EVT   32
#define XHCI_TRB_TYPE_CMD_CMPL_EVT   33
#define XHCI_TRB_TYPE_PORT_STATUS_EVT 34
#define XHCI_TRB_LINK_TC            (1U << 1)
#define XHCI_TRB_CYCLE_BIT          (1U << 0)

/* Completion codes (TRB status bits [31:24]) */
#define XHCI_COMP_INVALID           0
#define XHCI_COMP_SUCCESS           1
#define XHCI_COMP_DATA_BUFFER       2
#define XHCI_COMP_BABBLE            3
#define XHCI_COMP_TX_ERR            4
#define XHCI_COMP_TRB_ERR           5
#define XHCI_COMP_STALL             6
#define XHCI_COMP_SHORT_TX          13

/* PORTSC (Port Status and Control) bits */
#define XHCI_PORTSC_CCS             (1U << 0)   /* Current Connect Status (RO) */
#define XHCI_PORTSC_PED             (1U << 1)   /* Port Enabled/Disabled (RW1C) */
#define XHCI_PORTSC_OCA             (1U << 3)   /* Over-current Active (RO) */
#define XHCI_PORTSC_PR              (1U << 4)   /* Port Reset (RW) */
#define XHCI_PORTSC_PLS_MASK        (0xFU << 5) /* Port Link State */
#define XHCI_PORTSC_PP              (1U << 9)   /* Port Power (RW) */
#define XHCI_PORTSC_SPEED_MASK      (0xFU << 10)/* Port Speed (RO) */
#define XHCI_PORTSC_CSC             (1U << 17)  /* Connect Status Change (RW1C) */
#define XHCI_PORTSC_PEC             (1U << 18)  /* Port Enable/Disable Change (RW1C) */
#define XHCI_PORTSC_WRC             (1U << 19)  /* Warm Port Reset Change (RW1C) */
#define XHCI_PORTSC_OCC             (1U << 20)  /* Over-current Change (RW1C) */
#define XHCI_PORTSC_PRC             (1U << 21)  /* Port Reset Change (RW1C) */
#define XHCI_PORTSC_PLC             (1U << 22)  /* Port Link State Change (RW1C) */
#define XHCI_PORTSC_CEC             (1U << 23)  /* Config Error Change (RW1C) */
#define XHCI_PORTSC_WPR             (1U << 31)  /* Warm Port Reset (RW) */

/* Writing these bits as 1 clears them; writing 0 leaves them unchanged.
 * Control-bit writes must preserve them: (portsc & ~XHCI_PORTSC_RW1C_MASK). */
#define XHCI_PORTSC_RW1C_MASK       (XHCI_PORTSC_PED | XHCI_PORTSC_CSC | \
                                     XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
                                     XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | \
                                     XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

/* Port speed encodings (PORTSC bits [13:10]) */
#define XHCI_PORT_SPEED_FULL        1
#define XHCI_PORT_SPEED_LOW         2
#define XHCI_PORT_SPEED_HIGH        3
#define XHCI_PORT_SPEED_SUPER       4

/* Slots managed by the driver (1-based; array is sized [MAX + 1]) */
#define XHCI_MAX_MANAGED_SLOTS      32

/* USB descriptor types (bDescriptorType) */
#define USB_DESC_TYPE_DEVICE        1
#define USB_DESC_TYPE_CONFIGURATION 2
#define USB_DESC_TYPE_STRING        3
#define USB_DESC_TYPE_INTERFACE     4
#define USB_DESC_TYPE_ENDPOINT      5
#define USB_DESC_TYPE_HID           0x21
#define USB_DESC_TYPE_REPORT        0x22

/* xHCI endpoint types (Endpoint Context DW1 bits [5:3]) */
#define XHCI_EP_TYPE_INVALID        0
#define XHCI_EP_TYPE_ISOCH_OUT      1
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTR_OUT       3
#define XHCI_EP_TYPE_CONTROL        4
#define XHCI_EP_TYPE_ISOCH_IN       5
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTR_IN        7

/* Device Context Index bounds: EP0 is DCI 1, DCI 0 is the Slot Context */
#define XHCI_MIN_ENDPOINT_DCI       2
#define XHCI_MAX_ENDPOINT_DCI       31

/* Capability Register Offsets (relative to BAR0 MMIO base) */
#define XHCI_CAP_CAPLENGTH          0x00
#define XHCI_CAP_HCIVERSION         0x02
#define XHCI_CAP_HCSPARAMS1         0x04
#define XHCI_CAP_HCSPARAMS2         0x08
#define XHCI_CAP_HCSPARAMS3         0x0C
#define XHCI_CAP_HCCPARAMS1         0x10
#define XHCI_CAP_DBOFF              0x14
#define XHCI_CAP_RTSOFF             0x18

/* Operational Register Offsets (relative to op_regs = mmio_virt + cap_length) */
#define XHCI_OP_USBCMD              0x00
#define XHCI_OP_USBSTS              0x04
#define XHCI_OP_PAGESIZE            0x08
#define XHCI_OP_DNCTRL              0x14
#define XHCI_OP_CRCR                0x18
#define XHCI_OP_DCBAAP              0x30
#define XHCI_OP_CONFIG              0x38

/* USBCMD bits */
#define XHCI_CMD_RS                 (1U << 0)
#define XHCI_CMD_HCRST              (1U << 1)
#define XHCI_CMD_INTE               (1U << 2)
#define XHCI_CMD_HSEE               (1U << 3)

/* USBSTS bits */
#define XHCI_STS_HCH                (1U << 0)
#define XHCI_STS_HSE                (1U << 2)
#define XHCI_STS_CNR                (1U << 11)

/* Runtime & Interrupter 0 Register Offsets (relative to rt_regs + 0x20) */
#define XHCI_INTR_IMAN              0x00
#define XHCI_INTR_IMOD              0x04
#define XHCI_INTR_ERSTSZ            0x08
#define XHCI_INTR_ERSTBA            0x10
#define XHCI_INTR_ERDP              0x18

/* IMAN bits */
#define XHCI_IMAN_IP                (1U << 0)
#define XHCI_IMAN_IE                (1U << 1)

/* 16-byte Generic TRB (Transfer Request Block) */
struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

/* Event Ring Segment Table Entry (16 bytes) */
struct xhci_erst_entry {
    uint64_t ring_segment_base_address;
    uint16_t ring_segment_size;
    uint16_t rsvdz;
    uint32_t rsvdz2;
} __attribute__((packed));

/* 18-byte Standard USB Device Descriptor (USB 2.0 §9.5) */
struct usb_device_descriptor {
    uint8_t  bLength;            /* 18 */
    uint8_t  bDescriptorType;    /* 1 = DEVICE */
    uint16_t bcdUSB;             /* USB Spec release (e.g. 0x0200, 0x0300) */
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;    /* Max packet size for EP0 */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/* USB Standard Configuration Descriptor (USB 2.0 §9.5, 9 bytes) */
struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

/* USB Standard Interface Descriptor (USB 2.0 §9.4, 9 bytes) */
struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* USB Standard Endpoint Descriptor (USB 2.0 §9.3.7, 7 bytes) */
struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;  /* bit 7: 1=IN, 0=OUT; bits 3:0: EP num */
    uint8_t  bmAttributes;      /* bits 1:0: 0=Ctrl, 1=Isoch, 2=Bulk, 3=Intr */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* USB HID Descriptor (USB 2.0 §9.23, 9 bytes) */
struct usb_hid_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

/* Per-endpoint transfer ring instance (dynamically allocated per DCI) */
struct xhci_ep_ring {
    uint64_t ring_phys;             /* 4KB DMA page physical address */
    struct xhci_trb *ring_virt;     /* Virtual address */
    uint32_t enqueue;               /* Enqueue index (0..254) */
    uint8_t cycle;                  /* Producer cycle state */
    uint8_t dci;                    /* Device Context Index (2..31) */
    uint8_t ep_type;                /* XHCI_EP_TYPE_* */
    uint16_t max_packet_size;       /* Endpoint max packet size */
    uint8_t interval;               /* Computed xHCI interval exponent */
    bool active;
};

/* Summary of the primary interface parsed from the descriptor hierarchy */
struct xhci_interface_info {
    uint8_t interface_num;
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t protocol;
    uint8_t num_endpoints;
    bool active;
};

/* Context size helpers: HCCPARAMS1.CSZ selects 32B (csz=0) or 64B (csz=1)
 * context entries. Input context = [ctrl][slot][31 x ep]; device context =
 * [slot][31 x ep]. */
static inline void *xhci_input_ctrl_ctx(void *base, uint8_t csz)
{
    (void)csz;
    return base;
}

static inline void *xhci_input_slot_ctx(void *base, uint8_t csz)
{
    return (uint8_t *)base + (csz ? 64 : 32);
}

static inline void *xhci_input_ep0_ctx(void *base, uint8_t csz)
{
    return (uint8_t *)base + (csz ? 128 : 64);
}

static inline void *xhci_dev_slot_ctx(void *base, uint8_t csz)
{
    (void)csz;
    return base;
}

static inline void *xhci_dev_ep0_ctx(void *base, uint8_t csz)
{
    return (uint8_t *)base + (csz ? 64 : 32);
}

/* Input Context: [ctrl][slot][EP0 (DCI 1)][EP DCI 2..31] — the endpoint
 * context for a given DCI lives at index (1 + dci). */
static inline void *xhci_input_ep_ctx(void *base, uint8_t csz, uint8_t dci)
{
    return (uint8_t *)base + (1 + dci) * (csz ? 64 : 32);
}

/* Output Device Context: [slot][EP0 (DCI 1)][EP DCI 2..31] — the endpoint
 * context for a given DCI lives at index dci. */
static inline void *xhci_dev_ep_ctx(void *base, uint8_t csz, uint8_t dci)
{
    return (uint8_t *)base + dci * (csz ? 64 : 32);
}

/* Per-slot driver state (slot_id is 1-based) */
struct xhci_slot {
    uint8_t slot_id;
    uint8_t port_num;
    uint8_t speed;
    uint16_t max_packet_size;
    bool active;

    /* DMA Allocations (each 4KB page) */
    uint64_t input_ctx_phys;
    void *input_ctx_virt;

    uint64_t dev_ctx_phys;
    void *dev_ctx_virt;

    /* EP0 Transfer Ring (single 4KB page) */
    uint64_t ep0_ring_phys;
    struct xhci_trb *ep0_ring_virt;
    uint32_t ep0_ring_enqueue;
    uint8_t ep0_ring_cycle;

    /* Dedicated 4KB DMA Data Buffer for Control Transfers */
    uint64_t dma_buf_phys;
    void *dma_buf_virt;

    /* Dynamically allocated endpoint ring table: array of pointers sized
     * (max_dci + 1).  Index 0 is unused, index 1 is EP0 (handled by the
     * dedicated EP0 ring), indexes 2..max_dci map to device endpoints. */
    struct xhci_ep_ring **ep_rings;
    uint8_t max_dci;

    /* Cached Device Descriptor */
    struct usb_device_descriptor desc;

    /* Cached Configuration Descriptor & parsed interface summary */
    struct usb_config_descriptor config_desc;
    struct xhci_interface_info iface;

    bool configured;
};

/* xHCI Controller Instance State */
struct xhci_controller {
    struct pci_device *pdev;

    /* MMIO mappings */
    uint64_t mmio_phys;
    uint64_t mmio_len;
    volatile uint8_t *mmio_virt;
    volatile uint8_t *op_regs;   /* mmio_virt + cap_length */
    volatile uint8_t *rt_regs;   /* mmio_virt + rtsoff */
    volatile uint8_t *db_regs;   /* mmio_virt + dboff */

    /* Capability caches */
    uint32_t cap_length;
    uint32_t hci_version;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    uint32_t max_scratchpads;
    uint32_t dboff;
    uint32_t rtsoff;
    uint8_t csz;                 /* context size: 0 = 32B, 1 = 64B */

    /* DCBAA */
    uint64_t dcbaa_phys;
    uint64_t *dcbaa_virt;

    /* Command Ring */
    uint64_t cmd_ring_phys;
    struct xhci_trb *cmd_ring_virt;
    uint32_t cmd_ring_enqueue;
    uint8_t cmd_ring_cycle;

    /* Event Ring */
    uint64_t event_ring_phys;
    struct xhci_trb *event_ring_virt;
    uint32_t event_ring_dequeue;
    uint8_t event_ring_cycle;

    /* ERST */
    uint64_t erst_phys;
    struct xhci_erst_entry *erst_virt;

    /* Managed device slots, indexed by 1-based slot_id */
    struct xhci_slot *slots[XHCI_MAX_MANAGED_SLOTS + 1];

    bool running;
};

/* xHCI MMIO read/write accessors */
#ifndef xhci_read32
static inline uint32_t xhci_read32(volatile const void *addr)
{
    return *(volatile const uint32_t *)addr;
}
#endif

#ifndef xhci_write32
static inline void xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}
#endif

#ifndef xhci_read64
static inline uint64_t xhci_read64(volatile const void *addr)
{
    return *(volatile const uint64_t *)addr;
}
#endif

#ifndef xhci_write64
static inline void xhci_write64(volatile void *addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
}
#endif

/* PCI Driver Callbacks & Public APIs */
int xhci_probe(struct pci_device *pdev, const struct pci_device_id *id);
void xhci_remove(struct pci_device *pdev);
int xhci_parse_capabilities(struct xhci_controller *ctrl);
int xhci_reset(struct xhci_controller *ctrl);
int xhci_alloc_dma(struct xhci_controller *ctrl);
void xhci_free_dma(struct xhci_controller *ctrl);
void xhci_setup_interrupter0(struct xhci_controller *ctrl);
int xhci_start(struct xhci_controller *ctrl);
int xhci_cmd_ring_enqueue(struct xhci_controller *ctrl, const struct xhci_trb *trb);
int xhci_poll_event_filtered(struct xhci_controller *ctrl, uint32_t target_type,
                             struct xhci_trb *out_evt, uint32_t timeout_loops);
int xhci_poll_cmd_completion(struct xhci_controller *ctrl, uint64_t expect_trb_phys,
                             struct xhci_trb *out_evt, uint32_t timeout_loops);
int xhci_poll_transfer(struct xhci_controller *ctrl, uint8_t slot_id,
                       struct xhci_trb *out_evt, uint32_t timeout_loops);
int xhci_port_reset(struct xhci_controller *ctrl, uint32_t port_num, uint8_t *out_speed);
int xhci_enable_slot(struct xhci_controller *ctrl, uint8_t port_num, uint8_t speed,
                     uint8_t *out_slot_id);
int xhci_address_device(struct xhci_controller *ctrl, uint8_t slot_id);
void xhci_free_slot(struct xhci_controller *ctrl, uint8_t slot_id);
void xhci_free_endpoint_rings(struct xhci_slot *slot);
int xhci_ep0_ring_enqueue(struct xhci_slot *slot, const struct xhci_trb *trb);
int xhci_get_device_descriptor(struct xhci_controller *ctrl, uint8_t slot_id,
                               struct usb_device_descriptor *out_desc);
uint8_t xhci_calc_interval(uint8_t speed, uint8_t ep_type, uint8_t bInterval);
int xhci_parse_config_descriptor(struct xhci_slot *slot, const uint8_t *buffer,
                                  uint16_t length);
int xhci_setup_endpoint_rings(struct xhci_slot *slot, const uint8_t *buffer,
                               uint16_t length);
int xhci_get_config_descriptor(struct xhci_controller *ctrl, uint8_t slot_id,
                                struct usb_config_descriptor *out_desc);
int xhci_configure_endpoints(struct xhci_controller *ctrl, uint8_t slot_id);
void xhci_probe_ports(struct xhci_controller *ctrl);
void xhci_delay_ms(uint32_t ms);
extern const struct pci_driver xhci_pci_driver;

#endif /* _DRIVER_XHCI_H */
