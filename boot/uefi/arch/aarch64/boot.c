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

efi_status_t arch_setup_graphics(struct boot_context *ctx)
{
    /* Best-effort: a missing GOP must not abort the boot. */
    (void)capture_graphics(ctx);
    return EFI_SUCCESS;
}

efi_status_t arch_fill_firmware(struct boot_context *ctx)
{
    const void *firmware_fdt;
    uint32_t fdt_size = 0;
    EFI_STATUS status;

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
    /* Best-effort: if EFI_RNG_PROTOCOL is present, fetch 32 bytes
     * of boot entropy for the kernel CSPRNG. Without this, aarch64
     * (RNDR not yet wired) would land in the cycle-counter ^ jiffies
     * fallback and random_init() would fail-closed, aborting every
     * user-space program on its all-zero canary. */
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

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys)
{
    aarch64_enter_kernel(entry, context_phys, AARCH64_TRAMPOLINE_BASE);
}
