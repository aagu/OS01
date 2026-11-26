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
                                                    uint64_t trampoline_phys);

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
                chunk == 0)
                break;
            off += chunk;
        }
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

static EFI_STATUS fetch_preliminary_mmap(void **buf_out, uint32_t *desc_size_out,
                                         uint32_t *count_out)
{
    uintn_t map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_version = 0;
    void *buf = NULL;
    EFI_STATUS status;

    status = BS->GetMemoryMap(&map_size, NULL, &map_key,
                              &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL || desc_size < 40u)
        return EFI_LOAD_ERROR;

    for (unsigned attempt = 0u; attempt < 4u; attempt++) {
        uintn_t alloc_size = map_size + (uintn_t)desc_size * 8u;
        buf = malloc(alloc_size);
        if (!buf)
            return EFI_OUT_OF_RESOURCES;
        map_size = alloc_size;
        status = BS->GetMemoryMap(&map_size, (efi_memory_descriptor_t *)buf,
                                  &map_key, &desc_size, &desc_version);
        if (!EFI_ERROR(status)) {
            if (desc_size < 40u || (map_size % desc_size) != 0u ||
                map_size == 0u || map_size / desc_size > UINT32_C(0xffffffff)) {
                free(buf);
                return EFI_LOAD_ERROR;
            }
            *buf_out = buf;
            *desc_size_out = (uint32_t)desc_size;
            *count_out = (uint32_t)(map_size / desc_size);
            return EFI_SUCCESS;
        }
        free(buf);
        buf = NULL;
        if (status != EFI_BUFFER_TOO_SMALL)
            return status;
    }
    return EFI_BUFFER_TOO_SMALL;
}

static EFI_STATUS alloc_handoff_pages(uintn_t pages,
                                      efi_physical_address_t *addr_out);

static EFI_STATUS copy_fdt_to_handoff(const void *src, uint32_t fdt_size,
                                      efi_physical_address_t *phys_out,
                                      uintn_t *pages_out)
{
    efi_physical_address_t fdt_phys = 0;
    uintn_t fdt_pages = (uintn_t)(((uint64_t)fdt_size + AARCH64_PAGE_SIZE - 1u) /
                                  AARCH64_PAGE_SIZE);
    EFI_STATUS status;

    status = alloc_handoff_pages(fdt_pages, &fdt_phys);
    if (EFI_ERROR(status))
        return status;
    memset((void *)(uintptr_t)fdt_phys, 0,
           (size_t)fdt_pages * (size_t)AARCH64_PAGE_SIZE);
    memcpy((void *)(uintptr_t)fdt_phys, src, (size_t)fdt_size);
    *phys_out = fdt_phys;
    *pages_out = fdt_pages;
    return EFI_SUCCESS;
}

static EFI_STATUS load_and_copy_fdt(const struct uefi_boot_config *config,
                                    efi_physical_address_t *phys_out,
                                    uintn_t *pages_out,
                                    uint32_t *size_out)
{
    uintn_t index;

    if (!phys_out || !pages_out || !size_out || !ST || !ST->ConfigurationTable)
        return EFI_INVALID_PARAMETER;

    for (index = 0; index < ST->NumberOfTableEntries; index++) {
        const efi_configuration_table_t *table = &ST->ConfigurationTable[index];
        const uint8_t *fdt = (const uint8_t *)table->VendorTable;
        uint32_t total_size;

        if (!guid_equal(&table->VendorGuid, &fdt_table_guid))
            continue;
        if (!fdt || load_be32(fdt) != AARCH64_FDT_MAGIC) {
            uefi_puts("UEFI-A64: corrupt EFI DTB\r\n");
            return EFI_LOAD_ERROR;
        }
        total_size = load_be32(fdt + sizeof(uint32_t));
        if (total_size < AARCH64_FDT_HEADER_SIZE ||
            total_size > UINT32_C(0x200000)) {
            uefi_puts("UEFI-A64: corrupt EFI DTB\r\n");
            return EFI_LOAD_ERROR;
        }
        *size_out = total_size;
        return copy_fdt_to_handoff(fdt, total_size, phys_out, pages_out);
    }

    /* EFI DTB table absent: only explicit RPi5 may fall back to VideoCore's
     * reserved DTB window [0x1f0000, 0x210000) after verifying readability in
     * the preliminary EFI memory map. */
    if (!config || config->platform != UEFI_BOOT_PLATFORM_RPI5) {
        uefi_puts("UEFI-A64: missing DTB\r\n");
        return EFI_NOT_FOUND;
    }

    {
        const uint64_t fw_pa = UINT64_C(0x1f0000);
        const uint8_t *fw_fdt = (const uint8_t *)(uintptr_t)fw_pa;
        void *mmap = NULL;
        uint32_t desc_size = 0, desc_count = 0, total_size = 0;
        EFI_STATUS status;

        status = fetch_preliminary_mmap(&mmap, &desc_size, &desc_count);
        if (EFI_ERROR(status)) {
            uefi_puts("UEFI-A64: preliminary memory map failed\r\n");
            return status;
        }
        if (!aarch64_fw_dtb_readable(mmap, desc_size, desc_count,
                                     fw_pa, AARCH64_FDT_HEADER_SIZE)) {
            free(mmap);
            uefi_puts("UEFI-A64: firmware DTB unreadable\r\n");
            return EFI_NOT_FOUND;
        }
        if (load_be32(fw_fdt) != AARCH64_FDT_MAGIC) {
            free(mmap);
            uefi_puts("UEFI-A64: corrupt firmware DTB\r\n");
            return EFI_LOAD_ERROR;
        }
        total_size = load_be32(fw_fdt + sizeof(uint32_t));
        if (total_size < AARCH64_FDT_HEADER_SIZE ||
            total_size > UINT32_C(0x20000)) {
            free(mmap);
            uefi_puts("UEFI-A64: corrupt firmware DTB\r\n");
            return EFI_LOAD_ERROR;
        }
        if (!aarch64_fw_dtb_readable(mmap, desc_size, desc_count,
                                     fw_pa, total_size)) {
            free(mmap);
            uefi_puts("UEFI-A64: firmware DTB truncated in memory map\r\n");
            return EFI_LOAD_ERROR;
        }
        free(mmap);
        *size_out = total_size;
        return copy_fdt_to_handoff(fw_fdt, total_size, phys_out, pages_out);
    }
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
    return EFI_SUCCESS;
}

efi_status_t arch_fill_firmware(struct boot_context *ctx,
                                const struct uefi_boot_config *config)
{
    efi_physical_address_t fdt_phys = 0;
    uintn_t fdt_pages = 0;
    uint32_t fdt_size = 0;
    const char *required_compat = NULL;
    uint64_t fw_console_pa = 0u;
    struct aarch64_early_mmio_handoff early_mmio;
    enum aarch64_fdt_scan_result scan_rc;
    EFI_STATUS status;

    status = load_and_copy_fdt(config, &fdt_phys, &fdt_pages, &fdt_size);
    if (EFI_ERROR(status))
        return status;

    aarch64_fdt_phys = fdt_phys;
    aarch64_fdt_pages = fdt_pages;
    ctx->firmware.dtb = (uint64_t)fdt_phys;
    ctx->flags |= BOOT_CONTEXT_HAS_DTB;

    if (config) {
        if (config->platform == UEFI_BOOT_PLATFORM_QEMU_VIRT)
            required_compat = "linux,dummy-virt";
        else if (config->platform == UEFI_BOOT_PLATFORM_RPI5)
            required_compat = "raspberrypi,5-model-b";

        if (config->console == UEFI_BOOT_CONSOLE_RP1_HEADER)
            fw_console_pa = UINT64_C(0x1f00030000);
    }

    scan_rc = aarch64_fdt_boot_scan((const void *)(uintptr_t)fdt_phys, fdt_size,
                                    required_compat, fw_console_pa, &early_mmio);
    if (scan_rc == AARCH64_FDT_SCAN_PLATFORM_MISMATCH) {
        uefi_puts("UEFI-A64: platform mismatch\r\n");
        return EFI_LOAD_ERROR;
    }
    if (scan_rc != AARCH64_FDT_SCAN_OK) {
        uefi_puts("UEFI-A64: DTB missing early MMIO\r\n");
        return EFI_LOAD_ERROR;
    }

    ctx->early_mmio.console_base = early_mmio.console_base;
    ctx->early_mmio.device_base = early_mmio.device_base;
    ctx->early_mmio.device_blocks = early_mmio.device_blocks;
    ctx->early_mmio.reserved = 0u;
    ctx->flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    if (fw_console_pa != 0u)
        ctx->flags |= BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED;

    status = capture_entropy(ctx);
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

static int range_overlaps_mmio(uint64_t start, uint64_t size,
                               const struct boot_context *ctx)
{
    uint64_t end;
    uint64_t win_base = ctx->early_mmio.device_base;
    uint64_t win_end = win_base + ((uint64_t)ctx->early_mmio.device_blocks << 21);

    if (size == 0u || start > (~UINT64_C(0)) - size)
        return 1;
    end = start + size;
    if (start < win_end && win_base < end)
        return 1;
    if ((ctx->flags & BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED) != 0u) {
        uint64_t cbase = ctx->early_mmio.console_base & ~UINT64_C(0x1fffff);
        uint64_t cend = cbase + UINT64_C(0x200000);
        if (start < cend && cbase < end)
            return 1;
    }
    return 0;
}

efi_status_t arch_build_memory(struct boot_context *ctx,
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

    if (!aarch64_early_mmio_map_safe(ctx) ||
        range_overlaps_mmio((uint64_t)AARCH64_TRAMPOLINE_BASE,
                            (uint64_t)AARCH64_PAGE_SIZE, ctx) ||
        range_overlaps_mmio((uint64_t)aarch64_ctx_phys,
                            (uint64_t)AARCH64_PAGE_SIZE, ctx) ||
        range_overlaps_mmio((uint64_t)aarch64_fdt_phys,
                            (uint64_t)aarch64_fdt_pages * AARCH64_PAGE_SIZE, ctx) ||
        range_overlaps_mmio((uint64_t)aarch64_mmap_phys,
                            (uint64_t)aarch64_mmap_pages * AARCH64_PAGE_SIZE, ctx))
        return EFI_SECURITY_VIOLATION;

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
    return EFI_SUCCESS;
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
#include "cache_handoff.h"

static uint64_t hw_read_ctr(void *ctx)
{
    (void)ctx;
    uint64_t ctr;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    return ctr;
}

static void hw_dc_civac(void *ctx, uint64_t addr)
{
    (void)ctx;
    __asm__ __volatile__("dc civac, %0" :: "r"(addr) : "memory");
}

static void hw_dsb_sy(void *ctx)
{
    (void)ctx;
    __asm__ __volatile__("dsb sy" ::: "memory");
}

static void hw_ic_iallu(void *ctx)
{
    (void)ctx;
    __asm__ __volatile__("ic iallu" ::: "memory");
}

static void hw_isb(void *ctx)
{
    (void)ctx;
    __asm__ __volatile__("isb" ::: "memory");
}

static void flush_handoff_caches(void)
{
    const struct aarch64_handoff_cache_regions regions = {
        .boot_block_base = (uint64_t)AARCH64_BOOT_BLOCK_BASE,
        .boot_block_size = (uint64_t)(AARCH64_BOOT_BLOCK_END -
                                      AARCH64_BOOT_BLOCK_BASE),
        .trampoline_base = (uint64_t)AARCH64_TRAMPOLINE_BASE,
        .page_size = (uint64_t)AARCH64_PAGE_SIZE,
        .ctx_phys = (uint64_t)aarch64_ctx_phys,
        .fdt_phys = (uint64_t)aarch64_fdt_phys,
        .fdt_pages = (uint64_t)aarch64_fdt_pages,
        .mmap_phys = (uint64_t)aarch64_mmap_phys,
        .mmap_pages = (uint64_t)aarch64_mmap_pages,
    };
    const struct aarch64_cache_ops hw_ops = {
        .read_ctr = hw_read_ctr,
        .dc_civac = hw_dc_civac,
        .dsb_sy = hw_dsb_sy,
        .ic_iallu = hw_ic_iallu,
        .isb = hw_isb,
    };

    aarch64_flush_handoff_caches(&regions, &hw_ops, NULL);
}

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys)
{
    flush_handoff_caches();
    aarch64_enter_kernel(entry, context_phys, AARCH64_TRAMPOLINE_BASE);
}
