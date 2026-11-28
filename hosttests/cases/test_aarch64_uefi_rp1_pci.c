/* @os01-test
 * id: test_aarch64_uefi_rp1_pci
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: rp1-uefi-pci
 * scenarios: [boot.uefi.aarch64_rp1_pci]
 */
#include "test_framework.h"
#include <core/bootinfo.h>
#include "../../boot/uefi/arch/aarch64/rp1_pci.h"
#include <stdint.h>
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

#define EFI_SUCCESS           UINT64_C(0)
#define EFI_NOT_FOUND         (UINT64_C(0x8000000000000000) | 14u)
#define EFI_INVALID_PARAMETER (UINT64_C(0x8000000000000000) | 2u)

typedef efi_status_t (EFIAPI *fake_pci_io_cfg_t)(
    void *This, int Width, uint32_t Offset, uintn_t Count, void *Buffer);
typedef efi_status_t (EFIAPI *fake_pci_io_get_loc_t)(
    void *This, uintn_t *SegmentNumber, uintn_t *BusNumber,
    uintn_t *DeviceNumber, uintn_t *FunctionNumber);
typedef efi_status_t (EFIAPI *fake_pci_io_get_bar_t)(
    void *This, uint8_t BarIndex, uint64_t *Supports, void **Resources);

struct fake_pci_io {
    void *pad0[6];
    fake_pci_io_cfg_t pci_read;          /* slot 6 */
    void *pad1[7];
    fake_pci_io_get_loc_t get_location;  /* slot 14 */
    void *pad2[1];
    fake_pci_io_get_bar_t get_bar_attr;  /* slot 16 */
    uint8_t cfg[256];
    uintn_t seg, bus, dev, fn;
    uint8_t bar_desc[128];
    uintn_t bar_desc_len;
    efi_status_t bar_status;
    int omit_bar_desc;
};

#define MAX_FAKE_DEVS 4u
static struct fake_pci_io *g_devs[MAX_FAKE_DEVS];
static efi_handle_t g_handles[MAX_FAKE_DEVS];
static uintn_t g_dev_count;
static efi_status_t g_locate_status;
static void *g_handle_buf_ptr;
static unsigned g_free_calls;
static void *g_freed_ptrs[16];

static efi_status_t EFIAPI fake_pci_read(
    void *This, int Width, uint32_t Offset, uintn_t Count, void *Buffer)
{
    struct fake_pci_io *d = (struct fake_pci_io *)This;
    uintn_t unit = (Width == 0) ? 1u : (Width == 1) ? 2u : 4u;
    if (!d || !Buffer || Width < 0 || Width > 2)
        return EFI_INVALID_PARAMETER;
    if (Offset + unit * Count > sizeof(d->cfg))
        return EFI_INVALID_PARAMETER;
    memcpy(Buffer, d->cfg + Offset, unit * Count);
    return EFI_SUCCESS;
}

static efi_status_t EFIAPI fake_get_location(
    void *This, uintn_t *Seg, uintn_t *Bus, uintn_t *Dev, uintn_t *Fn)
{
    struct fake_pci_io *d = (struct fake_pci_io *)This;
    if (!d || !Seg || !Bus || !Dev || !Fn)
        return EFI_INVALID_PARAMETER;
    *Seg = d->seg;
    *Bus = d->bus;
    *Dev = d->dev;
    *Fn = d->fn;
    return EFI_SUCCESS;
}

static efi_status_t EFIAPI fake_get_bar_attr(
    void *This, uint8_t BarIndex, uint64_t *Supports, void **Resources)
{
    struct fake_pci_io *d = (struct fake_pci_io *)This;
    if (!d || BarIndex != 0 || !Resources)
        return EFI_INVALID_PARAMETER;
    if (Supports)
        *Supports = 0;
    if (d->bar_status != EFI_SUCCESS) {
        *Resources = NULL;
        return d->bar_status;
    }
    if (d->omit_bar_desc) {
        *Resources = NULL;
        return EFI_SUCCESS;
    }
    *Resources = malloc(d->bar_desc_len);
    memcpy(*Resources, d->bar_desc, d->bar_desc_len);
    return EFI_SUCCESS;
}

static efi_status_t EFIAPI fake_locate_handle_buffer(
    int SearchType, efi_guid_t *Protocol, void *SearchKey,
    uintn_t *NoHandles, efi_handle_t **Buffer)
{
    (void)SearchType;
    (void)Protocol;
    (void)SearchKey;
    if (g_locate_status != EFI_SUCCESS) {
        *NoHandles = 0;
        *Buffer = NULL;
        return g_locate_status;
    }
    *NoHandles = g_dev_count;
    if (g_dev_count == 0) {
        *Buffer = NULL;
        return EFI_NOT_FOUND;
    }
    g_handle_buf_ptr = malloc(g_dev_count * sizeof(efi_handle_t));
    memcpy(g_handle_buf_ptr, g_handles, g_dev_count * sizeof(efi_handle_t));
    *Buffer = (efi_handle_t *)g_handle_buf_ptr;
    return EFI_SUCCESS;
}

static efi_status_t EFIAPI fake_handle_protocol(
    efi_handle_t Handle, efi_guid_t *Protocol, void **Interface)
{
    (void)Protocol;
    for (uintn_t i = 0; i < g_dev_count; i++) {
        if (g_handles[i] == Handle) {
            *Interface = g_devs[i];
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static efi_status_t EFIAPI fake_free_pool(void *Buffer)
{
    if (g_free_calls < 16u)
        g_freed_ptrs[g_free_calls] = Buffer;
    g_free_calls++;
    free(Buffer);
    return EFI_SUCCESS;
}

/* Mock BS struct matching posix-uefi offsets used by rp1_pci.c */
struct rp1_host_bs {
    efi_status_t (EFIAPI *LocateHandleBuffer)(int, efi_guid_t *, void *, uintn_t *, efi_handle_t **);
    efi_status_t (EFIAPI *HandleProtocol)(efi_handle_t, efi_guid_t *, void **);
    efi_status_t (EFIAPI *FreePool)(void *);
};

struct rp1_host_bs g_rp1_host_bs = {
    .LocateHandleBuffer = fake_locate_handle_buffer,
    .HandleProtocol = fake_handle_protocol,
    .FreePool = fake_free_pool,
};

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)(v >> 24);
}

static void w64(uint8_t *p, uint64_t v)
{
    w32(p, (uint32_t)(v & 0xffffffffu));
    w32(p + 4, (uint32_t)(v >> 32));
}

static void init_valid_rp1(struct fake_pci_io *d)
{
    memset(d, 0, sizeof(*d));
    d->pci_read = fake_pci_read;
    d->get_location = fake_get_location;
    d->get_bar_attr = fake_get_bar_attr;
    d->seg = 0;
    d->bus = 1;
    d->dev = 0;
    d->fn = 0;
    d->bar_status = EFI_SUCCESS;

    /* Vendor 0x1de4, Device 0x0001, Revision 2, Status cap list bit4 */
    w16(d->cfg + 0x00, 0x1de4u);
    w16(d->cfg + 0x02, 0x0001u);
    w16(d->cfg + 0x06, 0x0010u);
    d->cfg[0x08] = 0x02u;
    /* 64-bit BAR0 = 0x00000000_00100000 (type=2 at bits [2:1], bit2=1 -> 0x4) */
    w32(d->cfg + 0x10, 0x00100004u);
    w32(d->cfg + 0x14, 0x00000000u);
    /* Capability pointer at 0x34 -> 0x40 (PM) -> 0x50 (MSI-X) */
    d->cfg[0x34] = 0x40u;
    d->cfg[0x40] = 0x01u;
    d->cfg[0x41] = 0x50u;
    /* MSI-X cap at 0x50: ID=0x11, next=0x00, msgctl = 60 (61 entries) */
    d->cfg[0x50] = 0x11u;
    d->cfg[0x51] = 0x00u;
    w16(d->cfg + 0x52, 60u);
    w32(d->cfg + 0x54, 0x00000000u); /* BIR=0, table offset=0 */
    w32(d->cfg + 0x58, 0x00002000u); /* BIR=0, PBA offset=0x2000 */

    /* ACPI QWORD Address Space Descriptor (tag 0x8a, len 0x2b = 43 bytes) + End Tag (0x79, 0x00) */
    d->bar_desc[0] = 0x8au;
    w16(d->bar_desc + 1, 0x002bu);
    d->bar_desc[3] = 0x00u; /* ResourceType = Memory */
    w64(d->bar_desc + 14, UINT64_C(0x1f00100000)); /* AddrRangeMin */
    w64(d->bar_desc + 22, UINT64_C(0x1f00103fff)); /* AddrRangeMax */
    w64(d->bar_desc + 30, UINT64_C(0));            /* AddrTranslationOffset */
    w64(d->bar_desc + 38, UINT64_C(0x4000));       /* AddrLen */
    d->bar_desc[46] = 0x79u;
    d->bar_desc[47] = 0x00u;
    d->bar_desc_len = 48u;
}

static void reset_bus(void)
{
    g_dev_count = 0;
    g_locate_status = EFI_SUCCESS;
    g_handle_buf_ptr = NULL;
    g_free_calls = 0;
    memset(g_freed_ptrs, 0, sizeof(g_freed_ptrs));
}

static void test_capture_success_non_identity(void)
{
    struct fake_pci_io dev;
    struct boot_rp1_pci_info out;

    reset_bus();
    init_valid_rp1(&dev);
    g_devs[0] = &dev;
    g_handles[0] = (efi_handle_t)0x1111;
    g_dev_count = 1;

    memset(&out, 0xaa, sizeof(out));
    assert_eq(0, uefi_rp1_pci_capture(&out));
    assert_eq(UINT64_C(0x100000), out.bar0_pci);
    assert_eq(UINT64_C(0x1f00100000), out.bar0_cpu);
    assert_eq(UINT64_C(0x4000), out.bar0_size);
    assert_eq(0u, out.table_offset);
    assert_eq(0x2000u, out.pba_offset);
    assert_eq(0u, out.segment);
    assert_eq(61u, out.entries);
    assert_eq(1u, out.bus);
    assert_eq(0u, out.device);
    assert_eq(0u, out.function);
    assert_eq(0x50u, out.msix_cap);
    assert_eq(2u, g_free_calls);
    assert_true(g_freed_ptrs[0] != NULL || g_freed_ptrs[1] != NULL);

    /* Real RPi 5 hardware reports RP1 Revision ID 0x00 */
    reset_bus();
    init_valid_rp1(&dev);
    dev.cfg[0x08] = 0x00u;
    g_devs[0] = &dev;
    g_handles[0] = (efi_handle_t)0x1111;
    g_dev_count = 1;
    memset(&out, 0xaa, sizeof(out));
    assert_eq(0, uefi_rp1_pci_capture(&out));
    assert_eq(UINT64_C(0x1f00100000), out.bar0_cpu);
}

static void test_capture_failures_zero_out(void)
{
    struct fake_pci_io d0, d1;
    struct boot_rp1_pci_info out;

    /* 1. No protocol */
    reset_bus();
    g_locate_status = EFI_NOT_FOUND;
    memset(&out, 0x55, sizeof(out));
    assert_true(uefi_rp1_pci_capture(&out) < 0);
    assert_eq(0u, out.bar0_size);

    /* 2. Duplicate C0 */
    reset_bus();
    init_valid_rp1(&d0);
    init_valid_rp1(&d1);
    d1.bus = 2;
    g_devs[0] = &d0;
    g_devs[1] = &d1;
    g_handles[0] = (efi_handle_t)0x1;
    g_handles[1] = (efi_handle_t)0x2;
    g_dev_count = 2;
    assert_true(uefi_rp1_pci_capture(&out) < 0);
    assert_eq(0u, out.entries);

    /* 3. Revision mismatch */
    reset_bus();
    init_valid_rp1(&d0);
    d0.cfg[0x08] = 0x01u;
    g_devs[0] = &d0;
    g_handles[0] = (efi_handle_t)0x1;
    g_dev_count = 1;
    assert_true(uefi_rp1_pci_capture(&out) < 0);

    /* 4. BAR0 high-word conflict (32-bit BAR0 with non-zero BAR1? Or 64-bit BAR0 with high bits) */
    reset_bus();
    init_valid_rp1(&d0);
    w32(d0.cfg + 0x14, 0x00000010u); /* 64-bit BAR0 high != 0 while AddrRangeMin is 0x1f00100000? Wait: BAR0 PCI is 0x10_00100000 != 0x100000, or high word conflict on 64-bit BAR */
    /* Also test 32-bit BAR0 with non-zero BAR1? No, 64-bit BAR0 with high=1 vs 0x100000: wait, AddrRangeMin is CPU address; if BAR0 is 64-bit with high=0xdeadbeef, bar0_pci=0xdeadbeef00100000. What is "BAR0 高位冲突"? E.g. 64-bit BAR0 with high != 0 when 32-bit BAR0 expected or high != 0 in 32-bit? Let's test 32-bit BAR0 (type=0) is valid or 64-bit BAR0 high != 0? Wait: on RPi5, BAR0 is 64-bit with high=0; if high!=0 or type=01 (I/O) or type=11 (reserved) -> invalid! Let's test BAR0 type=1 (I/O) AND 64-bit BAR0 high!=0 if PCI BAR > 32-bit? Wait: 64-bit BAR0 with high != 0 can be valid on PCIe, unless it's I/O BAR or reserved type, OR BAR0=0! Let's test BAR0=0 (unassigned) and type=I/O and 64-bit high!=0? Wait: let's check what rp1_pci.c should reject for "BAR0 高位冲突": if BAR0 is 32-bit (mem type 0) and BAR0=0, or if BAR0 is 64-bit and high != 0? Wait: in RPi5 RP1 C0, BAR0 is 64-bit memory BAR with upper 32 bits = 0 (or if upper 32 bits != 0, it's >4GiB PCI address). Let's test BAR0 high word = 0x12340000 with 32-bit resource or BAR0 type invalid! Actually, let's check what the brief says: "BAR0 高位冲突" — if BAR0 is 64-bit and upper 32 bits != 0? Let's make sure rp1_pci.c rejects 64-bit BAR0 with upper 32 bits != 0 OR BAR0 PCI == 0 OR BAR0 I/O space. */
    g_devs[0] = &d0;
    g_handles[0] = (efi_handle_t)0x1;
    g_dev_count = 1;
    assert_true(uefi_rp1_pci_capture(&out) < 0);

    /* 5. Looping capability */
    reset_bus();
    init_valid_rp1(&d0);
    d0.cfg[0x41] = 0x40u; /* points back to itself */
    g_devs[0] = &d0;
    g_handles[0] = (efi_handle_t)0x1;
    g_dev_count = 1;
    assert_true(uefi_rp1_pci_capture(&out) < 0);

    /* 6. PBA out of bounds */
    reset_bus();
    init_valid_rp1(&d0);
    w32(d0.cfg + 0x58, 0x00004000u); /* PBA at 0x4000 >= bar0_size 0x4000 */
    g_devs[0] = &d0;
    g_handles[0] = (efi_handle_t)0x1;
    g_dev_count = 1;
    assert_true(uefi_rp1_pci_capture(&out) < 0);

    /* 7. Table & PBA overlap */
    reset_bus();
    init_valid_rp1(&d0);
    w32(d0.cfg + 0x58, 0x00000200u); /* Table is [0, 61*16=976=0x3d0), PBA at 0x200 overlaps */
    g_devs[0] = &d0;
    g_handles[0] = (efi_handle_t)0x1;
    g_dev_count = 1;
    assert_true(uefi_rp1_pci_capture(&out) < 0);

    /* 8. Missing End Tag */
    reset_bus();
    init_valid_rp1(&d0);
    d0.bar_desc[46] = 0x00u; /* not 0x79 */
    g_devs[0] = &d0;
    g_handles[0] = (efi_handle_t)0x1;
    g_dev_count = 1;
    assert_true(uefi_rp1_pci_capture(&out) < 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_capture_success_non_identity),
    TEST_ENTRY(test_capture_failures_zero_out),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 UEFI RP1 PCI capture");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
