/*
 * kernel/include/driver/bga.h
 *
 * PCI Bochs Graphic Adapter (BGA) Backend Driver for QEMU Standard VGA.
 * (Spec §2.1, §2.2, §2.3, §4)
 */

#ifndef _DRIVER_BGA_H
#define _DRIVER_BGA_H

#include <stdint.h>
#include <stdbool.h>
#include <uapi/fb.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

#define BGA_PCI_VENDOR_ID 0x1234
#define BGA_PCI_DEVICE_ID 0x1111

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID               0x0
#define VBE_DISPI_INDEX_XRES             0x1
#define VBE_DISPI_INDEX_YRES             0x2
#define VBE_DISPI_INDEX_BPP              0x3
#define VBE_DISPI_INDEX_ENABLE           0x4
#define VBE_DISPI_INDEX_BANK             0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH       0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT      0x7
#define VBE_DISPI_INDEX_X_OFFSET         0x8
#define VBE_DISPI_INDEX_Y_OFFSET         0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xA

#define VBE_DISPI_ID0 0xB0C0
#define VBE_DISPI_ID1 0xB0C1
#define VBE_DISPI_ID2 0xB0C2
#define VBE_DISPI_ID3 0xB0C3
#define VBE_DISPI_ID4 0xB0C4
#define VBE_DISPI_ID5 0xB0C5

#define VBE_DISPI_DISABLED    0x00
#define VBE_DISPI_ENABLED     0x01
#define VBE_DISPI_GETCAPS     0x02
#define VBE_DISPI_8BIT_DAC    0x20
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM  0x80

typedef struct bga_caps {
    uint64_t vram_bytes;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_bpp;
} bga_caps_t;

enum bga_result {
    BGA_APPLIED,
    BGA_ROLLED_BACK,
    BGA_FAILED
};

int bga_probe(struct pci_device *pdev, const struct pci_device_id *id);
enum bga_result bga_apply_mode(const struct fb_info *target);
uint32_t bga_filter_modes(const bga_caps_t *caps, uint64_t mapped_size,
                          struct fb_info out[FB_MAX_MODES]);
int fb_install_backend(const bga_caps_t *caps, uint32_t *addr, uint64_t mapped_size);

extern const struct pci_driver bga_pci_driver;

#ifdef OS01_HOST_TEST
typedef uint16_t (*bga_io_read_fn)(uint16_t port);
typedef void (*bga_io_write_fn)(uint16_t port, uint16_t val);
typedef int (*bga_pci_read_fn)(struct pci_device *pdev, uint16_t offset, uint32_t *val);
typedef int (*bga_pci_write_fn)(struct pci_device *pdev, uint16_t offset, uint32_t val);

void bga_set_transport_for_test(bga_io_read_fn r_io, bga_io_write_fn w_io,
                                bga_pci_read_fn r_pci, bga_pci_write_fn w_pci);
void bga_reset_for_test(void);
#endif

#endif /* _DRIVER_BGA_H */
