#include <uefi.h>

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#ifndef UINT32_MAX
#define UINT32_MAX UINT32_C(0xffffffff)
#endif

#include "../../../kernel/include/kernel/bootinfo.h"
#include "loader.h"

typedef efi_status_t EFI_STATUS;

/* QEMU virt's PL011 UART: physical MMIO at 0x09000000. */
#define PL011_BASE       ((volatile uint32_t *)(uintptr_t)0x09000000U)
#define PL011_DR          0x00U
#define PL011_FR          0x18U
#define PL011_FR_TXFF     (1U << 5)

#define AARCH64_HANDOFF_END        UINT64_C(0x40200000)
#define AARCH64_HANDOFF_PAGES      ((AARCH64_HANDOFF_END - \
                                     AARCH64_HANDOFF_BASE) / \
                                    AARCH64_PAGE_SIZE)
#define AARCH64_TRAMPOLINE_BASE    (AARCH64_HANDOFF_END - \
                                    AARCH64_PAGE_SIZE)
#define AARCH64_HANDOFF_DATA_PAGES (AARCH64_HANDOFF_PAGES - 1)
#define AARCH64_FDT_MAGIC          UINT32_C(0xd00dfeed)
#define AARCH64_FDT_HEADER_SIZE    UINT32_C(40)
#define AARCH64_MEMORY_MAP_SLACK   2U
#define AARCH64_EFI_STATUS_CODE_MASK UINT64_C(0x7fffffffffffffff)

struct aarch64_fixed_allocation {
    efi_physical_address_t address;
    uintn_t pages;
};

static struct aarch64_fixed_allocation *kernel_allocations;
static uint16_t kernel_allocation_count;

/* UEFI Device Tree Table GUID: b1b621d5-f19c-41a5-830b-d9152c69aae0. */
static const efi_guid_t fdt_table_guid = {
    0xb1b621d5U, 0xf19cU, 0x41a5U,
    { 0x83U, 0x0bU, 0xd9U, 0x15U, 0x2cU, 0x69U, 0xaaU, 0xe0U }
};

extern const uint8_t aarch64_handoff_stub_start[];
extern const uint8_t aarch64_handoff_stub_end[];
__attribute__((noreturn)) void aarch64_enter_kernel(uint64_t entry,
                                                     uint64_t context_phys);

static void pl011_putc(char c)
{
    while (PL011_BASE[PL011_FR / sizeof(uint32_t)] & PL011_FR_TXFF)
        ;
    PL011_BASE[PL011_DR / sizeof(uint32_t)] = (uint8_t)c;
}

static void pl011_puts(const char *s)
{
    while (*s)
        pl011_putc(*s++);
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

static void release_handoff_allocations(int data_reserved,
                                        int trampoline_reserved)
{
    if (trampoline_reserved)
        (void)BS->FreePages((efi_physical_address_t)AARCH64_TRAMPOLINE_BASE, 1);
    if (data_reserved) {
        (void)BS->FreePages((efi_physical_address_t)AARCH64_HANDOFF_BASE,
                            (uintn_t)AARCH64_HANDOFF_DATA_PAGES);
    }
}

/* The staged POSIX-UEFI UEFI_NO_UTF8 adapter converts this standard status
 * number back to EFIERR(number) at the application entry boundary. */
static int main_error_code(EFI_STATUS status)
{
    uint32_t code =
        (uint32_t)((uint64_t)status & AARCH64_EFI_STATUS_CODE_MASK);

    if (code == 0) {
        code = (uint32_t)((uint64_t)EFI_LOAD_ERROR &
                          AARCH64_EFI_STATUS_CODE_MASK);
    }
    return (int)code;
}

static EFI_STATUS handoff_overflow(void)
{
    pl011_puts("UEFI-A64: handoff overflow\r\n");
    return EFI_BUFFER_TOO_SMALL;
}

static int guid_equal(const efi_guid_t *left, const efi_guid_t *right)
{
    uint32_t index;

    if (left->Data1 != right->Data1 || left->Data2 != right->Data2 ||
        left->Data3 != right->Data3)
        return 0;
    for (index = 0; index < sizeof(left->Data4); index++) {
        if (left->Data4[index] != right->Data4[index])
            return 0;
    }
    return 1;
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

static EFI_STATUS prepare_trampoline(void)
{
    uint64_t stub_size =
        (uint64_t)(aarch64_handoff_stub_end - aarch64_handoff_stub_start);
    void *destination = (void *)(uintptr_t)AARCH64_TRAMPOLINE_BASE;

    if (!aarch64_handoff_el_supported(read_current_el()) ||
        stub_size == 0 || stub_size > AARCH64_PAGE_SIZE) {
        pl011_puts("UEFI-A64: trampoline unreachable\r\n");
        return EFI_UNSUPPORTED;
    }

    memcpy(destination, aarch64_handoff_stub_start, (size_t)stub_size);
    if (memcmp(destination, aarch64_handoff_stub_start,
               (size_t)stub_size) != 0) {
        pl011_puts("UEFI-A64: trampoline unreachable\r\n");
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

static void copy_gop(struct boot_context *context)
{
    efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_gop_t *gop = NULL;
    EFI_STATUS status;

    status = BS->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->Mode ||
        !gop->Mode->Information || gop->Mode->FrameBufferBase == 0 ||
        gop->Mode->FrameBufferSize == 0)
        return;

    context->graphics.HorizontalResolution =
        gop->Mode->Information->HorizontalResolution;
    context->graphics.VerticalResolution =
        gop->Mode->Information->VerticalResolution;
    context->graphics.PixelsPerScanLine =
        gop->Mode->Information->PixelsPerScanLine;
    context->graphics.FrameBufferBase =
        (uint64_t)gop->Mode->FrameBufferBase;
    context->graphics.FrameBufferSize =
        (uint64_t)gop->Mode->FrameBufferSize;
    context->flags |= BOOT_CONTEXT_HAS_FRAMEBUFFER;
}

EFI_STATUS aarch64_load_kernel(const void *image, uint64_t image_size,
                               uint64_t *entry_out)
{
    const uint8_t *bytes = (const uint8_t *)image;
    const struct aarch64_elf64_ehdr *ehdr;
    uint16_t index;

    release_kernel_allocations();
    if (aarch64_elf_validate(image, image_size, entry_out) != 0) {
        pl011_puts("UEFI-A64: bad ELF\r\n");
        return EFI_LOAD_ERROR;
    }

    ehdr = (const struct aarch64_elf64_ehdr *)bytes;
    kernel_allocations =
        malloc((size_t)ehdr->e_phnum * sizeof(*kernel_allocations));
    if (!kernel_allocations) {
        pl011_puts("UEFI-A64: load allocation failed\r\n");
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
            pl011_puts("UEFI-A64: bad ELF\r\n");
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
            pl011_puts("UEFI-A64: load allocation failed\r\n");
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

EFI_STATUS aarch64_build_handoff(efi_handle_t image_handle,
                                 uint64_t *context_phys_out)
{
    efi_physical_address_t allocation;
    struct boot_context *context;
    const void *firmware_fdt;
    uint32_t fdt_size = 0;
    uint64_t cursor;
    uint64_t map_capacity;
    efi_memory_descriptor_t *memory_map;
    uintn_t required_size = 0;
    uintn_t map_key = 0;
    uintn_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    EFI_STATUS status;
    int data_reserved = 0;
    int trampoline_reserved = 0;

    if (!context_phys_out)
        return EFI_INVALID_PARAMETER;

    allocation = (efi_physical_address_t)AARCH64_HANDOFF_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                               (uintn_t)AARCH64_HANDOFF_DATA_PAGES,
                               &allocation);
    if (EFI_ERROR(status) || allocation != AARCH64_HANDOFF_BASE) {
        if (!EFI_ERROR(status)) {
            (void)BS->FreePages(allocation,
                                (uintn_t)AARCH64_HANDOFF_DATA_PAGES);
        }
        pl011_puts("UEFI-A64: handoff allocation failed\r\n");
        return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
    }
    data_reserved = 1;

    allocation = (efi_physical_address_t)AARCH64_TRAMPOLINE_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderCode, 1, &allocation);
    if (EFI_ERROR(status) || allocation != AARCH64_TRAMPOLINE_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(allocation, 1);
        pl011_puts("UEFI-A64: handoff allocation failed\r\n");
        status = EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
        goto fail;
    }
    trampoline_reserved = 1;

    memset((void *)(uintptr_t)AARCH64_HANDOFF_BASE, 0,
           (size_t)(AARCH64_HANDOFF_END - AARCH64_HANDOFF_BASE));
    context = (struct boot_context *)(uintptr_t)AARCH64_HANDOFF_BASE;
    boot_context_init(context);
    context->boot_cpu_id = read_mpidr();
    context->flags |= BOOT_CONTEXT_HAS_BOOT_CPU_ID;
    copy_gop(context);

    cursor = align_up_8(AARCH64_HANDOFF_BASE + sizeof(*context));
    firmware_fdt = find_fdt(&fdt_size);
    if (firmware_fdt) {
        if ((uint64_t)fdt_size > AARCH64_TRAMPOLINE_BASE - cursor) {
            status = handoff_overflow();
            goto fail;
        }
        memcpy((void *)(uintptr_t)cursor, firmware_fdt, (size_t)fdt_size);
        context->firmware.dtb = cursor;
        context->flags |= BOOT_CONTEXT_HAS_DTB;
        cursor = align_up_8(cursor + (uint64_t)fdt_size);
    }

    status = prepare_trampoline();
    if (EFI_ERROR(status))
        goto fail;
    if (!boot_context_valid(context)) {
        pl011_puts("UEFI-A64: corrupt handoff\r\n");
        status = EFI_LOAD_ERROR;
        goto fail;
    }

    if (cursor >= AARCH64_TRAMPOLINE_BASE) {
        status = handoff_overflow();
        goto fail;
    }
    map_capacity = AARCH64_TRAMPOLINE_BASE - cursor;
    memory_map = (efi_memory_descriptor_t *)(uintptr_t)cursor;

    status = BS->GetMemoryMap(&required_size, NULL, &map_key,
                              &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0)
        goto fail_with_status;
    if ((uint64_t)required_size > map_capacity ||
        descriptor_size > (uintn_t)map_capacity ||
        AARCH64_MEMORY_MAP_SLACK * descriptor_size >
            (uintn_t)map_capacity - required_size) {
        status = handoff_overflow();
        goto fail;
    }

    for (;;) {
        uintn_t final_size = (uintn_t)map_capacity;

        status = BS->GetMemoryMap(&final_size, memory_map, &map_key,
                                  &descriptor_size, &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            status = handoff_overflow();
            goto fail;
        }
        if (EFI_ERROR(status))
            goto fail;
        if (descriptor_size == 0 || final_size > (uintn_t)map_capacity ||
            final_size % descriptor_size != 0 ||
            final_size / descriptor_size > UINT32_MAX) {
            pl011_puts("UEFI-A64: corrupt handoff\r\n");
            status = EFI_LOAD_ERROR;
            goto fail;
        }

        context->memory.entries = cursor;
        context->memory.entry_count =
            (uint32_t)(final_size / descriptor_size);
        context->memory.entry_size = (uint32_t)descriptor_size;
        context->memory.format = BOOT_MEMORY_FORMAT_UEFI_RAW;
        context->memory.descriptor_version = descriptor_version;
        context->flags |= BOOT_CONTEXT_HAS_MEMORY_MAP;
        if (!boot_context_valid(context)) {
            pl011_puts("UEFI-A64: corrupt handoff\r\n");
            status = EFI_LOAD_ERROR;
            goto fail;
        }

        status = BS->ExitBootServices(image_handle, map_key);
        if (status == EFI_INVALID_PARAMETER)
            continue;
        if (EFI_ERROR(status)) {
            pl011_puts("UEFI-A64: ExitBootServices failed\r\n");
            goto fail;
        }

        *context_phys_out = AARCH64_HANDOFF_BASE;
        return EFI_SUCCESS;
    }

fail_with_status:
    if (!EFI_ERROR(status))
        status = EFI_LOAD_ERROR;
fail:
    release_handoff_allocations(data_reserved, trampoline_reserved);
    return status;
}

static EFI_STATUS read_kernel_image(void **image_out, uint64_t *size_out)
{
    FILE *file;
    void *image;
    long size;

    if (!image_out || !size_out)
        return EFI_INVALID_PARAMETER;

    file = fopen(L"\\kernel.elf", L"r");
    if (!file)
        return EFI_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return EFI_LOAD_ERROR;
    }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return EFI_LOAD_ERROR;
    }

    image = malloc((size_t)size);
    if (!image) {
        fclose(file);
        return EFI_OUT_OF_RESOURCES;
    }
    if (fread(image, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(image);
        return EFI_LOAD_ERROR;
    }
    fclose(file);

    *image_out = image;
    *size_out = (uint64_t)size;
    return EFI_SUCCESS;
}

int main(int argc, char_t **argv)
{
    EFI_STATUS status;
    void *image = NULL;
    uint64_t image_size = 0;
    uint64_t entry = 0;
    uint64_t context_phys = 0;

    (void)argc;
    (void)argv;

    status = read_kernel_image(&image, &image_size);
    if (EFI_ERROR(status)) {
        pl011_puts("UEFI-A64: bad ELF\r\n");
        return main_error_code(status);
    }

    status = aarch64_load_kernel(image, image_size, &entry);
    free(image);
    if (EFI_ERROR(status))
        return main_error_code(status);

    status = aarch64_build_handoff(IM, &context_phys);
    if (EFI_ERROR(status)) {
        release_kernel_allocations();
        return main_error_code(status);
    }

    aarch64_enter_kernel(entry, context_phys);
}
