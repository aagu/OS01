/* boot/uefi/arch/aarch64/boot.c — aarch64 arch hooks.
 *
 * Kernel: validated ELF loaded at fixed PT_LOAD physical addresses
 *   inside the configured boot block (entry = AARCH64_KERNEL_ENTRY,
 *   see loader.h / boot_layout.h).
 * Handoff: boot_context, firmware FDT copy, and raw UEFI memory map
 *   are dynamically allocated below AARCH64_BOOT_MAP_END (disjoint
 *   from the boot block) and passed by physical pointer; the 1-page
 *   trampoline sits at AARCH64_TRAMPOLINE_BASE.
 * Memory map: raw UEFI descriptors (BOOT_MEMORY_FORMAT_UEFI_RAW).
 */
#include <uefi.h>
#include "../arch.h"
#include "loader.h"

#define AARCH64_MMAP_SLACK_ENTRIES 32u
#define AARCH64_MMAP_MIN_PAGES     8u
#define AARCH64_FDT_MAGIC          UINT32_C(0xd00dfeed)
#define AARCH64_FDT_HEADER_SIZE    UINT32_C(40)

_Static_assert(sizeof(struct boot_context) <= AARCH64_PAGE_SIZE,
               "boot_context exceeds first handoff page");

/* UEFI Device Tree Table GUID: b1b621d5-f19c-41a5-830b-d9152c69aae0. */
static const efi_guid_t fdt_table_guid = {
    0xb1b621d5U, 0xf19cU, 0x41a5U,
    { 0x83U, 0x0bU, 0xd9U, 0x15U, 0x2cU, 0x69U, 0xaaU, 0xe0U }
};

extern const uint8_t aarch64_handoff_stub_start[];
extern const uint8_t aarch64_handoff_stub_end[];
__attribute__((noreturn)) void aarch64_enter_kernel(uint64_t entry,
                                                    uint64_t context_phys,
                                                    uint64_t trampoline_phys,
                                                    uint64_t beacon_pa,
                                                    uint64_t fb_base,
                                                    uint64_t fb_stride,
                                                    uint64_t fb_pixel);

struct aarch64_fixed_allocation {
    efi_physical_address_t address;
    uintn_t pages;
};

static struct aarch64_fixed_allocation *kernel_allocations;
static uint16_t kernel_allocation_count;
static efi_physical_address_t aarch64_ctx_phys;
static efi_physical_address_t aarch64_fdt_phys;
static uintn_t aarch64_fdt_pages;
static efi_physical_address_t aarch64_mmap_phys;
static uintn_t aarch64_mmap_pages;
static int aarch64_trampoline_reserved;  /* AARCH64_TRAMPOLINE_BASE, 1 page */

#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
/* Framebuffer stage beacons.
 *
 * A board whose UEFI console is HDMI and whose SerialIo is the debug UART
 * leaves neither the serial header nor the screen showing loader progress,
 * so a hang before the handoff is invisible. Each stage paints one 96x40
 * block on the framebuffer; counting the blocks tells how far the loader
 * got. The blocks stop at ExitBootServices (the protocol is gone
 * afterwards); the trampoline's 'H' on the serial UART marks that step.
 * All fills are direct framebuffer stores: calling GOP->Blt inside the
 * ExitBootServices retry loop would itself be a boot service call, and a
 * service that allocates invalidates the memory-map key the loop is
 * retrying with. A second row of small blocks counts loop iterations. */
#define FB_BEACON_BARS 5u
#define FB_BEACON_W    96u
#define FB_BEACON_H    40u

static struct {
    int probed;
    efi_gop_t *gop;
    uint64_t fb_base;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} fb_beacon;

static void fb_beacon_probe(void)
{
    efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_gop_t *gop = NULL;

    if (fb_beacon.probed)
        return;
    fb_beacon.probed = 1;
    if (!BS || !BS->LocateProtocol)
        return;
    if (EFI_ERROR(BS->LocateProtocol(&gop_guid, NULL, (void **)&gop)) ||
        !gop || !gop->Mode || !gop->Mode->Information)
        return;
    fb_beacon.gop = gop;
    fb_beacon.fb_base = (uint64_t)gop->Mode->FrameBufferBase;
    fb_beacon.stride = gop->Mode->Information->PixelsPerScanLine;
    fb_beacon.width = gop->Mode->Information->HorizontalResolution;
    fb_beacon.height = gop->Mode->Information->VerticalResolution;
    fb_beacon.format = (uint32_t)gop->Mode->Information->PixelFormat;
}

static uint32_t fb_beacon_pixel(uint32_t r, uint32_t g, uint32_t b)
{
    if (fb_beacon.format == (uint32_t)PixelRedGreenBlueReserved8BitPerColor)
        return (r & 0xffu) | ((g & 0xffu) << 8) | ((b & 0xffu) << 16);
    if (fb_beacon.format == (uint32_t)PixelBlueGreenRedReserved8BitPerColor)
        return (b & 0xffu) | ((g & 0xffu) << 8) | ((r & 0xffu) << 16);
    return 0x00ffffffu;   /* PixelBitMask / PixelBltOnly: white */
}

static void fb_beacon_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color)
{
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)fb_beacon.fb_base;

    for (uint32_t row = 0; row < h; ++row) {
        volatile uint32_t *p = fb + (uint64_t)(y + row) * fb_beacon.stride + x;
        for (uint32_t col = 0; col < w; ++col)
            p[col] = color;
    }
}

static void fb_beacon_bar(unsigned stage)
{
    static const uint8_t colors[FB_BEACON_BARS][3] = {
        { 0xff, 0x00, 0x00 }, { 0x00, 0xff, 0x00 },
        { 0x00, 0x00, 0xff }, { 0xff, 0xff, 0x00 },
        { 0xff, 0x00, 0xff },   /* ExitBootServices returned success */
    };
    uint32_t x = 24u + stage * 120u;
    uint32_t y = 24u;

    fb_beacon_probe();
    if (stage >= FB_BEACON_BARS || fb_beacon.fb_base == 0u ||
        fb_beacon.stride == 0u ||
        x + FB_BEACON_W > fb_beacon.width ||
        y + FB_BEACON_H > fb_beacon.height)
        return;
    fb_beacon_fill(x, y, FB_BEACON_W, FB_BEACON_H,
                   fb_beacon_pixel(colors[stage][0], colors[stage][1],
                                   colors[stage][2]));
}

/* Pre-EBS UART probes: one write-only store each, no register read.
 * 'P' goes to the firmware/VideoCore alias the knob names; 'Q' goes to
 * the CPU-side PCIe alias the device tree gives the RP1 UART. Whichever
 * letter reaches the header serial is the address the kernel must use. */
static void pre_ebs_uart_mark(uint64_t pa, char c)
{
    volatile uint32_t *dr = (volatile uint32_t *)(uintptr_t)pa;

    dr[0] = (uint32_t)(unsigned char)c;
}

/* One 48x24 marker per ExitBootServices attempt, second row. More than one
 * means the firmware kept invalidating the memory-map key. */
static void fb_beacon_retry(unsigned attempt)
{
    uint32_t x = 24u + (attempt - 1u) * 60u;
    uint32_t y = 80u;

    fb_beacon_probe();
    if (attempt == 0u || attempt > 16u || fb_beacon.fb_base == 0u ||
        fb_beacon.stride == 0u ||
        x + 48u > fb_beacon.width || y + 24u > fb_beacon.height)
        return;
    fb_beacon_fill(x, y, 48u, 24u, fb_beacon_pixel(0xff, 0xff, 0xff));
}
#else
#define fb_beacon_bar(stage) ((void)0)
#define fb_beacon_retry(attempt) ((void)0)
#define pre_ebs_uart_mark(pa, c) ((void)0)
#endif

static efi_serial_io_protocol_t *serial_io_protocol(void)
{
    static efi_serial_io_protocol_t *cached;
    static int probed;

    if (!probed) {
        efi_guid_t ser_guid = EFI_SERIAL_IO_PROTOCOL_GUID;
        efi_serial_io_protocol_t *ser = NULL;

        probed = 1;
        if (BS && BS->LocateProtocol &&
            !EFI_ERROR(BS->LocateProtocol(&ser_guid, NULL, (void **)&ser)) &&
            ser && ser->Write) {
            cached = ser;
        }
    }
    return cached;
}

static void conout_puts(const char *s)
{
    wchar_t buf[128];
    size_t n = 0;

    if (!ST || !ST->ConOut || !ST->ConOut->OutputString)
        return;

    while (*s) {
        buf[n++] = (wchar_t)(unsigned char)*s++;
        if (n == (sizeof(buf) / sizeof(buf[0])) - 1u) {
            buf[n] = L'\0';
            ST->ConOut->OutputString(ST->ConOut, buf);
            n = 0;
        }
    }
    if (n > 0) {
        buf[n] = L'\0';
        ST->ConOut->OutputString(ST->ConOut, buf);
    }
}

static void uefi_puts(const char *s)
{
    efi_serial_io_protocol_t *ser;

    if (!s || !*s)
        return;

    ser = serial_io_protocol();
    if (ser) {
        uintn_t total = 0;
        while (s[total])
            ++total;
        uintn_t off = 0;
        while (off < total) {
            uintn_t chunk = total - off;
            if (EFI_ERROR(ser->Write(ser, &chunk, (void *)(s + off))) ||
                chunk == 0) {
                conout_puts(s + off);
                return;
            }
            off += chunk;
        }
        return;
    }
    conout_puts(s);
}

static uint32_t load_be32(const void *address)
{
    const uint8_t *bytes = (const uint8_t *)address;

    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static uint64_t align_up_8(uint64_t value)
{
    return (value + UINT64_C(7)) & ~UINT64_C(7);
}

static uint64_t read_current_el(void)
{
    uint64_t current_el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    return current_el;
}

static uint64_t read_mpidr(void)
{
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

static void synchronize_code_range(const void *address, uint64_t size)
{
    uint64_t ctr;
    uint64_t data_line;
    uint64_t instruction_line;
    uint64_t cursor;
    uint64_t end = (uint64_t)(uintptr_t)address + size;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    data_line = UINT64_C(4) << ((ctr >> 16) & UINT64_C(0xf));
    instruction_line = UINT64_C(4) << (ctr & UINT64_C(0xf));

    cursor = (uint64_t)(uintptr_t)address & ~(data_line - 1);
    for (; cursor < end; cursor += data_line)
        __asm__ volatile("dc cvau, %0" : : "r"(cursor) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");

    cursor = (uint64_t)(uintptr_t)address & ~(instruction_line - 1);
    for (; cursor < end; cursor += instruction_line)
        __asm__ volatile("ic ivau, %0" : : "r"(cursor) : "memory");
    __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

static void release_kernel_allocations(void)
{
    while (kernel_allocation_count != 0) {
        const struct aarch64_fixed_allocation *allocation;

        kernel_allocation_count--;
        allocation = &kernel_allocations[kernel_allocation_count];
        (void)BS->FreePages(allocation->address, allocation->pages);
    }
    if (kernel_allocations) {
        free(kernel_allocations);
        kernel_allocations = NULL;
    }
}

static EFI_STATUS prepare_trampoline(void)
{
    uint64_t stub_size =
        (uint64_t)(aarch64_handoff_stub_end - aarch64_handoff_stub_start);
    void *destination = (void *)(uintptr_t)AARCH64_TRAMPOLINE_BASE;

    if (!aarch64_handoff_el_supported(read_current_el()) ||
        stub_size == 0 || stub_size > AARCH64_PAGE_SIZE) {
        uefi_puts("UEFI-A64: trampoline unreachable\r\n");
        return EFI_UNSUPPORTED;
    }

    memcpy(destination, aarch64_handoff_stub_start, (size_t)stub_size);
    if (memcmp(destination, aarch64_handoff_stub_start,
               (size_t)stub_size) != 0) {
        uefi_puts("UEFI-A64: trampoline unreachable\r\n");
        return EFI_DEVICE_ERROR;
    }
    synchronize_code_range(destination, stub_size);
    return EFI_SUCCESS;
}

static const void *find_fdt(uint32_t *size_out)
{
    uintn_t index;

    if (!size_out || !ST || !ST->ConfigurationTable)
        return NULL;
    for (index = 0; index < ST->NumberOfTableEntries; index++) {
        const efi_configuration_table_t *table = &ST->ConfigurationTable[index];
        const uint8_t *fdt = (const uint8_t *)table->VendorTable;
        uint32_t total_size;

        if (!guid_equal(&table->VendorGuid, &fdt_table_guid) || !fdt)
            continue;
        if (load_be32(fdt) != AARCH64_FDT_MAGIC)
            return NULL;
        total_size = load_be32(fdt + sizeof(uint32_t));
        if (total_size < AARCH64_FDT_HEADER_SIZE)
            return NULL;
        *size_out = total_size;
        return fdt;
    }

    /* Raspberry Pi 5: the VideoCore firmware loads the device tree at PA
     * 0x1f0000 (its log: "Loading 'bcm2712-rpi-5-b.dtb' ... offset
     * 0x1f0000") even when the UEFI does not install the EFI DTB
     * configuration table. Fall back to that copy. */
    {
        const uint8_t *fw_fdt = (const uint8_t *)(uintptr_t)UINT64_C(0x1f0000);
        uint32_t total_size;

        if (load_be32(fw_fdt) == AARCH64_FDT_MAGIC) {
            total_size = load_be32(fw_fdt + sizeof(uint32_t));
            if (total_size >= AARCH64_FDT_HEADER_SIZE &&
                total_size <= UINT32_C(0x200000)) {
                *size_out = total_size;
                return fw_fdt;
            }
        }
    }
    return NULL;
}

const char_t *arch_kernel_path(void)
{
    return L"\\kernel.elf";
}

static EFI_STATUS alloc_handoff_pages(uintn_t pages,
                                      efi_physical_address_t *addr_out)
{
    uint64_t bytes;
    efi_physical_address_t addr;
    EFI_STATUS status;

    if (!addr_out || pages == 0)
        return EFI_INVALID_PARAMETER;
    if ((uint64_t)pages > (~UINT64_C(0)) / AARCH64_PAGE_SIZE)
        return EFI_OUT_OF_RESOURCES;
    bytes = (uint64_t)pages * AARCH64_PAGE_SIZE;
    if (bytes > AARCH64_BOOT_MAP_END)
        return EFI_OUT_OF_RESOURCES;

    /* Kernel M0 boot page tables only map [0, AARCH64_BOOT_MAP_END),
     * and the boot block [AARCH64_BOOT_BLOCK_BASE, AARCH64_BOOT_BLOCK_END)
     * is reserved for kernel ELF segments + trampoline. Allocate from
     * the highest free pages below AARCH64_BOOT_MAP_END, retrying below
     * AARCH64_BOOT_BLOCK_BASE if the first attempt lands inside B. */
    addr = (efi_physical_address_t)(AARCH64_BOOT_MAP_END - 1);
    status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (!EFI_ERROR(status)) {
        if (addr != 0 &&
            (addr + bytes <= AARCH64_BOOT_BLOCK_BASE ||
             addr >= AARCH64_BOOT_BLOCK_END)) {
            *addr_out = addr;
            return EFI_SUCCESS;
        }
        (void)BS->FreePages(addr, pages);
        if (AARCH64_BOOT_BLOCK_BASE >= bytes + AARCH64_PAGE_SIZE) {
            addr = (efi_physical_address_t)(AARCH64_BOOT_BLOCK_BASE - 1);
            status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                       pages, &addr);
            if (!EFI_ERROR(status)) {
                if (addr != 0 && addr + bytes <= AARCH64_BOOT_BLOCK_BASE) {
                    *addr_out = addr;
                    return EFI_SUCCESS;
                }
                (void)BS->FreePages(addr, pages);
            }
        }
        return EFI_OUT_OF_RESOURCES;
    }
    return status;
}

efi_status_t arch_init_handoff(struct boot_context **ctx_out)
{
    efi_physical_address_t addr;
    struct boot_context *ctx;
    EFI_STATUS status;

    fb_beacon_bar(0);   /* loader main reached */

    status = alloc_handoff_pages(1, &addr);
    if (EFI_ERROR(status))
        return status;
    aarch64_ctx_phys = addr;

    addr = (efi_physical_address_t)AARCH64_TRAMPOLINE_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderCode, 1, &addr);
    if (EFI_ERROR(status) || addr != AARCH64_TRAMPOLINE_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, 1);
        status = EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
        goto fail;
    }
    aarch64_trampoline_reserved = 1;

    memset((void *)(uintptr_t)aarch64_ctx_phys, 0, (size_t)AARCH64_PAGE_SIZE);
    ctx = (struct boot_context *)(uintptr_t)aarch64_ctx_phys;
    boot_context_init(ctx);
    ctx->boot_cpu_id = read_mpidr();
    ctx->flags |= BOOT_CONTEXT_HAS_BOOT_CPU_ID;

    status = prepare_trampoline();   /* pre-EBS so failure is reportable */
    if (EFI_ERROR(status))
        goto fail;

    *ctx_out = ctx;
    return EFI_SUCCESS;

fail:
    /* arch_release() is idempotent (its *_reserved flags clear on release),
     * so this internal fail path and the common main.c fail path calling
     * arch_release() again is safe — the second call is a no-op. */
    arch_release();
    return status;
}

efi_status_t arch_load_kernel(const void *image, uint64_t image_size,
                              uint64_t *entry_out)
{
    const uint8_t *bytes = (const uint8_t *)image;
    const struct aarch64_elf64_ehdr *ehdr;
    uint16_t index;

    release_kernel_allocations();
    {
        int rc = aarch64_elf_validate(image, image_size, entry_out);

        if (rc != 0) {
            /* Only the actual entry-address mismatch (the configured
             * boot block entry vs the ELF's e_entry) gets the dedicated
             * diagnostic: a paired BOOTAA64.EFI + kernel.elf built for
             * different layouts must be unmistakable. */
            if (rc == AARCH64_ELF_ERR_ENTRY_MISMATCH)
                uefi_puts("UEFI-A64: kernel entry mismatch\r\n");
            else
                uefi_puts("UEFI-A64: bad ELF\r\n");
            return EFI_LOAD_ERROR;
        }
    }

    ehdr = (const struct aarch64_elf64_ehdr *)bytes;
    kernel_allocations =
        malloc((size_t)ehdr->e_phnum * sizeof(*kernel_allocations));
    if (!kernel_allocations) {
        uefi_puts("UEFI-A64: load allocation failed\r\n");
        return EFI_OUT_OF_RESOURCES;
    }
    for (index = 0; index < ehdr->e_phnum; index++) {
        const struct aarch64_elf64_phdr *phdr =
            (const struct aarch64_elf64_phdr *)
            (bytes + ehdr->e_phoff +
             (uint64_t)index * ehdr->e_phentsize);
        efi_physical_address_t allocation;
        uint64_t component_end;
        uint64_t component_pages;
        uint64_t component_start;
        uint64_t interval_start;
        uint64_t interval_pages;
        uint16_t previous;
        int changed;
        int previously_allocated = 0;
        EFI_STATUS status;

        if (phdr->p_type != AARCH64_PT_LOAD)
            continue;
        if (aarch64_page_interval(phdr->p_paddr, phdr->p_memsz,
                                  &interval_start, &interval_pages) != 0) {
            release_kernel_allocations();
            uefi_puts("UEFI-A64: bad ELF\r\n");
            return EFI_LOAD_ERROR;
        }
        if (interval_pages == 0)
            continue;

        component_start = interval_start;
        component_end = interval_start + interval_pages * AARCH64_PAGE_SIZE;
        do {
            uint16_t scan;

            changed = 0;
            for (scan = 0; scan < ehdr->e_phnum; scan++) {
                const struct aarch64_elf64_phdr *other =
                    (const struct aarch64_elf64_phdr *)
                    (bytes + ehdr->e_phoff +
                     (uint64_t)scan * ehdr->e_phentsize);
                uint64_t other_end;
                uint64_t other_pages;
                uint64_t other_start;

                if (other->p_type != AARCH64_PT_LOAD ||
                    aarch64_page_interval(other->p_paddr, other->p_memsz,
                                          &other_start, &other_pages) != 0 ||
                    other_pages == 0)
                    continue;
                other_end = other_start + other_pages * AARCH64_PAGE_SIZE;
                if (other_start < component_end &&
                    component_start < other_end) {
                    if (other_start < component_start) {
                        component_start = other_start;
                        changed = 1;
                    }
                    if (other_end > component_end) {
                        component_end = other_end;
                        changed = 1;
                    }
                }
            }
        } while (changed);

        for (previous = 0; previous < index; previous++) {
            const struct aarch64_elf64_phdr *other =
                (const struct aarch64_elf64_phdr *)
                (bytes + ehdr->e_phoff +
                 (uint64_t)previous * ehdr->e_phentsize);
            uint64_t other_end;
            uint64_t other_pages;
            uint64_t other_start;

            if (other->p_type != AARCH64_PT_LOAD ||
                aarch64_page_interval(other->p_paddr, other->p_memsz,
                                      &other_start, &other_pages) != 0 ||
                other_pages == 0)
                continue;
            other_end = other_start + other_pages * AARCH64_PAGE_SIZE;
            if (other_start < component_end && component_start < other_end) {
                previously_allocated = 1;
                break;
            }
        }
        if (previously_allocated)
            continue;

        component_pages =
            (component_end - component_start) / AARCH64_PAGE_SIZE;
        allocation = (efi_physical_address_t)component_start;
        status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                                   (uintn_t)component_pages, &allocation);
        if (EFI_ERROR(status) || allocation != component_start) {
            if (!EFI_ERROR(status)) {
                (void)BS->FreePages(allocation, (uintn_t)component_pages);
            }
            release_kernel_allocations();
            uefi_puts("UEFI-A64: load allocation failed\r\n");
            return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
        }
        kernel_allocations[kernel_allocation_count].address = allocation;
        kernel_allocations[kernel_allocation_count].pages =
            (uintn_t)component_pages;
        kernel_allocation_count++;
    }

    /* Copy only after every coalesced page interval has been reserved. */
    for (index = 0; index < ehdr->e_phnum; index++) {
        const struct aarch64_elf64_phdr *phdr =
            (const struct aarch64_elf64_phdr *)
            (bytes + ehdr->e_phoff +
             (uint64_t)index * ehdr->e_phentsize);

        if (phdr->p_type != AARCH64_PT_LOAD)
            continue;
        memcpy((void *)(uintptr_t)phdr->p_paddr,
               bytes + phdr->p_offset, (size_t)phdr->p_filesz);
        memset((void *)(uintptr_t)(phdr->p_paddr + phdr->p_filesz), 0,
               (size_t)(phdr->p_memsz - phdr->p_filesz));
        if (phdr->p_memsz != 0)
            synchronize_code_range((const void *)(uintptr_t)phdr->p_paddr,
                                   phdr->p_memsz);
    }

    return EFI_SUCCESS;
}

efi_status_t arch_setup_graphics(struct boot_context *ctx,
                                 const struct uefi_boot_config *config)
{
    (void)config;
    /* Best-effort: a missing GOP must not abort the boot. */
    (void)capture_graphics(ctx);
    fb_beacon_bar(1);   /* GOP captured */
    return EFI_SUCCESS;
}

efi_status_t arch_fill_firmware(struct boot_context *ctx,
                                const struct uefi_boot_config *config)
{
    const void *firmware_fdt;
    uint32_t fdt_size = 0;
    EFI_STATUS status;

    (void)config;

    firmware_fdt = find_fdt(&fdt_size);
    if (firmware_fdt) {
        struct aarch64_early_mmio_handoff early_mmio;
        efi_physical_address_t fdt_phys = 0;
        uintn_t fdt_pages = (uintn_t)(((uint64_t)fdt_size + AARCH64_PAGE_SIZE - 1) /
                                      AARCH64_PAGE_SIZE);

        status = alloc_handoff_pages(fdt_pages, &fdt_phys);
        if (EFI_ERROR(status))
            return status;
        aarch64_fdt_phys = fdt_phys;
        aarch64_fdt_pages = fdt_pages;
        memset((void *)(uintptr_t)fdt_phys, 0,
               (size_t)fdt_pages * (size_t)AARCH64_PAGE_SIZE);
        memcpy((void *)(uintptr_t)fdt_phys, firmware_fdt, (size_t)fdt_size);
        ctx->firmware.dtb = (uint64_t)fdt_phys;
        ctx->flags |= BOOT_CONTEXT_HAS_DTB;

        /* Derive the early console PA + Device window from the same
         * firmware FDT (fail-quiet: an unusable derivation leaves the
         * boot_context fields unset so the kernel falls back to its
         * compile-time QEMU defaults). */
        if (aarch64_fdt_early_mmio(firmware_fdt, fdt_size, &early_mmio)) {
            ctx->early_mmio.console_base = early_mmio.console_base;
            ctx->early_mmio.device_base = early_mmio.device_base;
            ctx->early_mmio.device_blocks = early_mmio.device_blocks;
            ctx->early_mmio.reserved = 0;
            ctx->flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
        }
    }
#ifdef AARCH64_FIRMWARE_CONSOLE_PA
    /* The console is the UART the firmware brought up, wherever the device
     * tree put the PL011 (Raspberry Pi 5: the DTB names the SoC UART on the
     * debug connector, the 40-pin header carries RP1's). The kernel maps its
     * block beside the device window; when no window could be derived at
     * all, that block stands in as the window so diagnostics still get out. */
    if ((ctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) == 0u) {
        ctx->early_mmio.device_base =
            (uint64_t)AARCH64_FIRMWARE_CONSOLE_PA & ~UINT64_C(0x1fffff);
        ctx->early_mmio.device_blocks = 1;
        ctx->early_mmio.reserved = 0;
        ctx->flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    }
    ctx->early_mmio.console_base = (uint64_t)AARCH64_FIRMWARE_CONSOLE_PA;
#endif
    /* Best-effort: if EFI_RNG_PROTOCOL is present, fetch 32 bytes
     * of boot entropy for the kernel CSPRNG. Without this, aarch64
     * (RNDR not yet wired) would land in the cycle-counter ^ jiffies
     * fallback and random_init() would fail-closed, aborting every
     * user-space program on its all-zero canary. */
    status = capture_entropy(ctx);
    fb_beacon_bar(2);   /* firmware tables (FDT, early MMIO, entropy) ready */
    return status;
}

void arch_memory_buffer(efi_physical_address_t *phys_out,
                        uint64_t *capacity_out)
{
    uintn_t map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_version = 0;
    uint64_t slack_bytes;
    uint64_t total_bytes;
    uintn_t pages;
    efi_physical_address_t mmap_phys = 0;
    EFI_STATUS status;

    if (aarch64_mmap_phys) {
        (void)BS->FreePages(aarch64_mmap_phys, aarch64_mmap_pages);
        aarch64_mmap_phys = 0;
        aarch64_mmap_pages = 0;
    }

    status = BS->GetMemoryMap(&map_size, NULL, &map_key,
                              &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL || desc_size == 0) {
        *phys_out = 0;
        *capacity_out = 0;
        return;
    }

    if ((uint64_t)desc_size > (~UINT64_C(0)) / AARCH64_MMAP_SLACK_ENTRIES) {
        *phys_out = 0;
        *capacity_out = 0;
        return;
    }
    slack_bytes = (uint64_t)desc_size * AARCH64_MMAP_SLACK_ENTRIES;
    if ((uint64_t)map_size > (~UINT64_C(0)) - slack_bytes) {
        *phys_out = 0;
        *capacity_out = 0;
        return;
    }
    total_bytes = (uint64_t)map_size + slack_bytes;
    pages = (uintn_t)((total_bytes + AARCH64_PAGE_SIZE - 1) / AARCH64_PAGE_SIZE);
    if (pages < AARCH64_MMAP_MIN_PAGES)
        pages = AARCH64_MMAP_MIN_PAGES;

    status = alloc_handoff_pages(pages, &mmap_phys);
    if (EFI_ERROR(status)) {
        *phys_out = 0;
        *capacity_out = 0;
        return;
    }

    aarch64_mmap_phys = mmap_phys;
    aarch64_mmap_pages = pages;
    *phys_out = mmap_phys;
    *capacity_out = (uint64_t)pages * AARCH64_PAGE_SIZE;
}

void arch_build_memory(struct boot_context *ctx,
                       efi_physical_address_t desc_phys,
                       uintn_t desc_size, uintn_t desc_count,
                       uint32_t desc_version)
{
    ctx->memory.entries = desc_phys;
    ctx->memory.entry_count = (uint32_t)desc_count;
    ctx->memory.entry_size = (uint32_t)desc_size;
    ctx->memory.format = BOOT_MEMORY_FORMAT_UEFI_RAW;
    ctx->memory.descriptor_version = desc_version;
    ctx->flags |= BOOT_CONTEXT_HAS_MEMORY_MAP;
#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    {
        /* One round per GetMemoryMap → ExitBootServices attempt. */
        static unsigned ebs_attempts;
        ++ebs_attempts;
        fb_beacon_bar(3);                 /* memory map obtained */
        fb_beacon_retry(ebs_attempts);    /* second-row attempt marker */
        pre_ebs_uart_mark((uint64_t)AARCH64_FIRMWARE_CONSOLE_PA, 'P');
        pre_ebs_uart_mark(UINT64_C(0x1f00030000), 'Q');
    }
#endif
}

void arch_release(void)
{
    release_kernel_allocations();
    if (aarch64_trampoline_reserved) {
        (void)BS->FreePages((efi_physical_address_t)AARCH64_TRAMPOLINE_BASE, 1);
        aarch64_trampoline_reserved = 0;
    }
    if (aarch64_mmap_phys) {
        (void)BS->FreePages(aarch64_mmap_phys, aarch64_mmap_pages);
        aarch64_mmap_phys = 0;
        aarch64_mmap_pages = 0;
    }
    if (aarch64_fdt_phys) {
        (void)BS->FreePages(aarch64_fdt_phys, aarch64_fdt_pages);
        aarch64_fdt_phys = 0;
        aarch64_fdt_pages = 0;
    }
    if (aarch64_ctx_phys) {
        (void)BS->FreePages(aarch64_ctx_phys, 1);
        aarch64_ctx_phys = 0;
    }
}

void arch_puts(const char *s)
{
    uefi_puts(s);
}

/* Cache maintenance for the handoff.
 *
 * The kernel's first instructions run at EL1 with the MMU off, so its
 * fetches and data reads bypass the caches and see DRAM directly. Every
 * byte this loader wrote (kernel image, boot_context, FDT copy, memory
 * map, trampoline) went through cacheable UEFI mappings and may still be
 * dirty in the D-cache. Without a clean to the point of coherency the
 * kernel executes stale DRAM (undefined instruction -> firmware VBAR ->
 * hang) or reads a stale boot_context. QEMU does not model caches, so this
 * only shows up on hardware. Per-VA `dc civac` over the exact regions
 * avoids depending on cache geometry from CCSIDR. */
static uint32_t cache_line_bytes(void)
{
    uint64_t ctr;

    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    return 4u << ((uint32_t)(ctr >> 16) & 0xfu);   /* CTR_EL0.DminLine */
}

static void clean_invalidate_dcache(uint64_t base, uint64_t size, uint32_t line)
{
    uint64_t addr = base & ~(uint64_t)(line - 1u);
    uint64_t end = (base + size + line - 1u) & ~(uint64_t)(line - 1u);

    for (; addr < end; addr += line)
        __asm__ __volatile__("dc civac, %0" :: "r"(addr) : "memory");
}

static void flush_handoff_caches(void)
{
    uint32_t line = cache_line_bytes();

    clean_invalidate_dcache((uint64_t)AARCH64_BOOT_BLOCK_BASE,
                            (uint64_t)(AARCH64_BOOT_BLOCK_END -
                                       AARCH64_BOOT_BLOCK_BASE), line);
    clean_invalidate_dcache((uint64_t)AARCH64_TRAMPOLINE_BASE,
                            (uint64_t)AARCH64_PAGE_SIZE, line);
    if (aarch64_ctx_phys != 0u)
        clean_invalidate_dcache((uint64_t)aarch64_ctx_phys,
                                (uint64_t)AARCH64_PAGE_SIZE, line);
    if (aarch64_fdt_phys != 0u && aarch64_fdt_pages != 0u)
        clean_invalidate_dcache((uint64_t)aarch64_fdt_phys,
                                (uint64_t)aarch64_fdt_pages *
                                    (uint64_t)AARCH64_PAGE_SIZE, line);
    if (aarch64_mmap_phys != 0u && aarch64_mmap_pages != 0u)
        clean_invalidate_dcache((uint64_t)aarch64_mmap_phys,
                                (uint64_t)aarch64_mmap_pages *
                                    (uint64_t)AARCH64_PAGE_SIZE, line);
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("ic iallu" ::: "memory");
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
}

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys)
{
    uint64_t beacon_pa = 0;
    uint64_t fb_base = 0, fb_stride = 0, fb_pixel = 0;

    flush_handoff_caches();

#if defined(AARCH64_FIRMWARE_CONSOLE_PA) && AARCH64_FW_CONSOLE_BEACON
    /* Bring-up beacon: hand the firmware-initialized UART's PA to the
     * trampoline, which prints 'H' there with the MMU off. A board whose
     * UEFI console is HDMI otherwise gives no serial evidence that the
     * loader reached the handoff. The store is write-only: a host with no
     * device at that address drops it instead of faulting, so QEMU runs of
     * the same image stay usable. */
    beacon_pa = (uint64_t)AARCH64_FIRMWARE_CONSOLE_PA;
    /* Past ExitBootServices: no protocol calls, only the cached
     * framebuffer fill. Magenta means the handoff is being entered. */
    fb_beacon_bar(4);
    /* The trampoline paints a white 16x16 block after its UART store, so
     * a visible block proves the store did not wedge the CPU. */
    if (fb_beacon.fb_base != 0u && fb_beacon.stride != 0u) {
        fb_base = fb_beacon.fb_base;
        fb_stride = fb_beacon.stride;
        fb_pixel = fb_beacon_pixel(0xff, 0xff, 0xff);
    }
#endif
    aarch64_enter_kernel(entry, context_phys, AARCH64_TRAMPOLINE_BASE, beacon_pa,
                         fb_base, fb_stride, fb_pixel);
}
