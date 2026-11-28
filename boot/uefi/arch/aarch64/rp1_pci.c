#ifdef OS01_HOST_TEST
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef EFIAPI
#define EFIAPI
#endif

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;
typedef size_t uintn_t;
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} efi_guid_t;

#define EFI_SUCCESS   UINT64_C(0)
#define EFI_ERROR(x)  (((int64_t)(x)) < 0)
#define ByProtocol    2

struct rp1_host_bs {
    efi_status_t (EFIAPI *LocateHandleBuffer)(int, efi_guid_t *, void *, uintn_t *, efi_handle_t **);
    efi_status_t (EFIAPI *HandleProtocol)(efi_handle_t, efi_guid_t *, void **);
    efi_status_t (EFIAPI *FreePool)(void *);
};
extern struct rp1_host_bs g_rp1_host_bs;
#define BS (&g_rp1_host_bs)
#else
#include <uefi.h>
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#ifndef UINT64_MAX
#define UINT64_MAX (~UINT64_C(0))
#endif
#endif

#include "rp1_pci.h"
#include "../../../../kernel/include/core/bootinfo.h"

#define RP1_PCI_VENDOR_ID          0x1de4u
#define RP1_PCI_DEVICE_ID          0x0001u
#define RP1_PCI_REV_C0             0x02u

#define RP1_PCI_CFG_STATUS         0x06u
#define RP1_PCI_STATUS_CAP_LIST    0x0010u
#define RP1_PCI_CFG_REV_ID         0x08u
#define RP1_PCI_CFG_HDR_TYPE       0x0eu
#define RP1_PCI_CFG_BAR0           0x10u
#define RP1_PCI_CFG_BAR1           0x14u
#define RP1_PCI_CFG_CAP_PTR        0x34u

#define RP1_PCI_CAP_ID_MSIX        0x11u
#define RP1_PCI_CAP_MIN            0x40u
#define RP1_PCI_CAP_MAX            0xfcu

#define RP1_ACPI_QWORD_ADDR_TAG    0x8au
#define RP1_ACPI_QWORD_ADDR_LEN    0x002bu
#define RP1_ACPI_RES_TYPE_MEM      0x00u
#define RP1_ACPI_END_TAG           0x79u

enum rp1_efi_pci_io_width {
    RP1_PCI_WIDTH_UINT8  = 0,
    RP1_PCI_WIDTH_UINT16 = 1,
    RP1_PCI_WIDTH_UINT32 = 2,
};

typedef efi_status_t (EFIAPI *rp1_efi_pci_io_cfg_t)(
    void *This, enum rp1_efi_pci_io_width Width,
    uint32_t Offset, uintn_t Count, void *Buffer);

typedef efi_status_t (EFIAPI *rp1_efi_pci_io_get_location_t)(
    void *This, uintn_t *SegmentNumber, uintn_t *BusNumber,
    uintn_t *DeviceNumber, uintn_t *FunctionNumber);

typedef efi_status_t (EFIAPI *rp1_efi_pci_io_get_bar_attr_t)(
    void *This, uint8_t BarIndex, uint64_t *Supports, void **Resources);

struct rp1_efi_pci_io_protocol {
    void *pad0[6];
    rp1_efi_pci_io_cfg_t pci_read;               /* slot 6 */
    void *pad1[7];
    rp1_efi_pci_io_get_location_t get_location;  /* slot 14 */
    void *pad2[1];
    rp1_efi_pci_io_get_bar_attr_t get_bar_attributes; /* slot 16 */
};

_Static_assert(offsetof(struct rp1_efi_pci_io_protocol, pci_read) ==
               6u * sizeof(void *), "Pci.Read must be at slot 6");
_Static_assert(offsetof(struct rp1_efi_pci_io_protocol, get_location) ==
               14u * sizeof(void *), "GetLocation must be at slot 14");
_Static_assert(offsetof(struct rp1_efi_pci_io_protocol, get_bar_attributes) ==
               16u * sizeof(void *), "GetBarAttributes must be at slot 16");

static void rp1_zero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++)
        p[i] = 0;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++)
        v |= (uint64_t)p[i] << (8u * i);
    return v;
}

static int pci_cfg_u8(struct rp1_efi_pci_io_protocol *pci, uint32_t off, uint8_t *out)
{
    efi_status_t st = pci->pci_read(pci, RP1_PCI_WIDTH_UINT8, off, 1u, out);
    return EFI_ERROR(st) ? -1 : 0;
}

static int pci_cfg_u16(struct rp1_efi_pci_io_protocol *pci, uint32_t off, uint16_t *out)
{
    efi_status_t st = pci->pci_read(pci, RP1_PCI_WIDTH_UINT16, off, 1u, out);
    return EFI_ERROR(st) ? -1 : 0;
}

static int pci_cfg_u32(struct rp1_efi_pci_io_protocol *pci, uint32_t off, uint32_t *out)
{
    efi_status_t st = pci->pci_read(pci, RP1_PCI_WIDTH_UINT32, off, 1u, out);
    return EFI_ERROR(st) ? -1 : 0;
}

static int parse_msix_cap(struct rp1_efi_pci_io_protocol *pci,
                          struct boot_rp1_pci_info *info)
{
    uint16_t status = 0;
    uint8_t cap_ptr = 0;
    uint64_t visited = 0;

    if (pci_cfg_u16(pci, RP1_PCI_CFG_STATUS, &status) != 0)
        return -1;
    if ((status & RP1_PCI_STATUS_CAP_LIST) == 0u)
        return -1;
    if (pci_cfg_u8(pci, RP1_PCI_CFG_CAP_PTR, &cap_ptr) != 0)
        return -1;

    cap_ptr &= 0xfcu;
    while (cap_ptr != 0u) {
        uint8_t bit_idx;
        uint32_t dw0 = 0;
        uint8_t cap_id;
        uint8_t next;

        if (cap_ptr < RP1_PCI_CAP_MIN || cap_ptr > RP1_PCI_CAP_MAX ||
            (cap_ptr & 3u) != 0u)
            return -1;
        bit_idx = (uint8_t)(cap_ptr >> 2);
        if ((visited & (UINT64_C(1) << bit_idx)) != 0u)
            return -1;
        visited |= (UINT64_C(1) << bit_idx);

        if (pci_cfg_u32(pci, cap_ptr, &dw0) != 0)
            return -1;
        cap_id = (uint8_t)(dw0 & 0xffu);
        next = (uint8_t)((dw0 >> 8) & 0xfcu);

        if (cap_id == RP1_PCI_CAP_ID_MSIX) {
            uint16_t msgctl = (uint16_t)(dw0 >> 16);
            uint32_t table_dw = 0, pba_dw = 0;
            uint16_t entries;
            uint32_t table_bir, pba_bir;

            if (cap_ptr > 0xf4u) /* 3 DWORDs must fit in 0x100 config space */
                return -1;
            if (pci_cfg_u32(pci, (uint32_t)cap_ptr + 4u, &table_dw) != 0 ||
                pci_cfg_u32(pci, (uint32_t)cap_ptr + 8u, &pba_dw) != 0)
                return -1;

            entries = (uint16_t)((msgctl & 0x7ffu) + 1u);
            table_bir = table_dw & 7u;
            pba_bir = pba_dw & 7u;
            if (table_bir != 0u || pba_bir != 0u)
                return -1;

            info->msix_cap = cap_ptr;
            info->entries = entries;
            info->table_offset = table_dw & ~7u;
            info->pba_offset = pba_dw & ~7u;
            return 0;
        }
        cap_ptr = next;
    }
    return -1;
}

static int parse_bar0_resources(const uint8_t *desc, uint64_t *cpu_out,
                                uint64_t *size_out)
{
    uint16_t len;
    uint64_t addr_min, addr_len;

    if (!desc)
        return -1;
    if (desc[0] != RP1_ACPI_QWORD_ADDR_TAG)
        return -1;
    len = read_le16(desc + 1);
    if (len != RP1_ACPI_QWORD_ADDR_LEN)
        return -1;
    if (desc[3] != RP1_ACPI_RES_TYPE_MEM)
        return -1;
    if (desc[46] != RP1_ACPI_END_TAG)
        return -1;

    addr_min = read_le64(desc + 14);
    addr_len = read_le64(desc + 38);
    if (addr_len == 0u || addr_min > UINT64_MAX - addr_len)
        return -1;

    *cpu_out = addr_min;
    *size_out = addr_len;
    return 0;
}

static int validate_rp1_candidate(struct rp1_efi_pci_io_protocol *pci,
                                  struct boot_rp1_pci_info *out)
{
    uint32_t bar0_lo = 0, bar0_hi = 0;
    uint64_t bar0_pci = 0, bar0_cpu = 0, bar0_size = 0;
    uint64_t table_size, pba_size;
    uintn_t seg = 0, bus = 0, dev = 0, fn = 0;
    void *resources = NULL;
    efi_status_t st;

    if (!pci->get_location || !pci->get_bar_attributes)
        return -1;

    if (pci_cfg_u32(pci, RP1_PCI_CFG_BAR0, &bar0_lo) != 0)
        return -1;
    if ((bar0_lo & 1u) != 0u) /* I/O BAR unsupported */
        return -1;
    if ((bar0_lo & 0x6u) == 0x4u) {
        if (pci_cfg_u32(pci, RP1_PCI_CFG_BAR1, &bar0_hi) != 0)
            return -1;
        if (bar0_hi != 0u) /* 64-bit BAR0 upper bits conflict with 32-bit PCI outbound window */
            return -1;
    } else if ((bar0_lo & 0x6u) != 0u) {
        return -1;
    }
    bar0_pci = ((uint64_t)bar0_hi << 32) | (uint64_t)(bar0_lo & ~0xfu);
    if (bar0_pci == 0u)
        return -1;

    if (parse_msix_cap(pci, out) != 0)
        return -1;

    st = pci->get_bar_attributes(pci, 0u, NULL, &resources);
    if (EFI_ERROR(st) || !resources) {
        if (resources && BS->FreePool)
            (void)BS->FreePool(resources);
        return -1;
    }
    if (parse_bar0_resources((const uint8_t *)resources, &bar0_cpu, &bar0_size) != 0) {
        (void)BS->FreePool(resources);
        return -1;
    }
    (void)BS->FreePool(resources);

    if (out->entries == 0u)
        return -1;
    table_size = (uint64_t)out->entries * 16u;
    pba_size = ((uint64_t)(out->entries + 63u) / 64u) * 8u;

    if ((uint64_t)out->table_offset > bar0_size ||
        table_size > bar0_size - (uint64_t)out->table_offset)
        return -1;
    if ((uint64_t)out->pba_offset > bar0_size ||
        pba_size > bar0_size - (uint64_t)out->pba_offset)
        return -1;
    if ((uint64_t)out->table_offset + table_size > (uint64_t)out->pba_offset &&
        (uint64_t)out->pba_offset + pba_size > (uint64_t)out->table_offset)
        return -1;

    if (EFI_ERROR(pci->get_location(pci, &seg, &bus, &dev, &fn)))
        return -1;
    if (seg > 0xffffu || bus > 0xffu || dev > 0x1fu || fn > 0x7u)
        return -1;

    out->bar0_pci = bar0_pci;
    out->bar0_cpu = bar0_cpu;
    out->bar0_size = bar0_size;
    out->segment = (uint16_t)seg;
    out->bus = (uint8_t)bus;
    out->device = (uint8_t)dev;
    out->function = (uint8_t)fn;
    return 0;
}

int uefi_rp1_pci_capture(struct boot_rp1_pci_info *out)
{
    /* EFI_PCI_IO_PROTOCOL_GUID: 4cf5b200-68b8-4ca5-9eec-b23e3f50029a */
    efi_guid_t pci_io_guid = {
        0x4cf5b200U, 0x68b8U, 0x4ca5U,
        { 0x9eU, 0xecU, 0xb2U, 0x3eU, 0x3fU, 0x50U, 0x02U, 0x9aU }
    };
    efi_handle_t *handles = NULL;
    uintn_t handle_count = 0;
    struct rp1_efi_pci_io_protocol *match = NULL;
    struct boot_rp1_pci_info tmp;
    efi_status_t st;

    if (!out)
        return -1;
    rp1_zero(out, sizeof(*out));
    rp1_zero(&tmp, sizeof(tmp));

    if (!BS || !BS->LocateHandleBuffer || !BS->HandleProtocol || !BS->FreePool)
        return -1;

    st = BS->LocateHandleBuffer(ByProtocol, &pci_io_guid, NULL,
                                &handle_count, &handles);
    if (EFI_ERROR(st) || !handles || handle_count == 0u) {
        if (handles)
            (void)BS->FreePool(handles);
        return -1;
    }

    for (uintn_t i = 0; i < handle_count; i++) {
        struct rp1_efi_pci_io_protocol *pci = NULL;
        uint16_t vendor = 0, device = 0;
        uint8_t rev = 0, hdr = 0;

        if (EFI_ERROR(BS->HandleProtocol(handles[i], &pci_io_guid, (void **)&pci)) ||
            !pci || !pci->pci_read)
            continue;
        if (pci_cfg_u16(pci, 0x00u, &vendor) != 0 ||
            pci_cfg_u16(pci, 0x02u, &device) != 0)
            continue;
        if (vendor != RP1_PCI_VENDOR_ID || device != RP1_PCI_DEVICE_ID)
            continue;
        if (pci_cfg_u8(pci, RP1_PCI_CFG_REV_ID, &rev) != 0 ||
            rev != RP1_PCI_REV_C0)
            continue;
        if (pci_cfg_u8(pci, RP1_PCI_CFG_HDR_TYPE, &hdr) != 0 ||
            (hdr & 0x7fu) != 0u)
            continue;
        if (match != NULL) {
            (void)BS->FreePool(handles);
            return -1; /* duplicate RP1 C0 */
        }
        match = pci;
    }

    (void)BS->FreePool(handles);
    if (!match)
        return -1;

    if (validate_rp1_candidate(match, &tmp) != 0)
        return -1;

    *out = tmp;
    return 0;
}
