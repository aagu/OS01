/* boot/uefi/arch/aarch64/boot.c — aarch64 arch hooks.
 *
 * Kernel: validated ELF loaded at fixed PT_LOAD physical addresses
 *   (entry 0x40080000, see loader.h).
 * Handoff: boot_context at 0x401e0000 in a 31-page allocation; the
 *   firmware FDT is copied after the context, and the raw UEFI memory
 *   map is placed after that, up to the trampoline page @ 0x401ff000.
 * Memory map: raw UEFI descriptors (BOOT_MEMORY_FORMAT_UEFI_RAW).
 */
#include <uefi.h>
#include "../arch.h"
#include "loader.h"

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

/* UEFI Device Tree Table GUID: b1b621d5-f19c-41a5-830b-d9152c69aae0. */
static const efi_guid_t fdt_table_guid = {
    0xb1b621d5U, 0xf19cU, 0x41a5U,
    { 0x83U, 0x0bU, 0xd9U, 0x15U, 0x2cU, 0x69U, 0xaaU, 0xe0U }
};

extern const uint8_t aarch64_handoff_stub_start[];
extern const uint8_t aarch64_handoff_stub_end[];
__attribute__((noreturn)) void aarch64_enter_kernel(uint64_t entry,
                                                     uint64_t context_phys);

struct aarch64_fixed_allocation {
    efi_physical_address_t address;
    uintn_t pages;
};

static struct aarch64_fixed_allocation *kernel_allocations;
static uint16_t kernel_allocation_count;
static uint64_t handoff_cursor;
static int aarch64_data_reserved;        /* 0x401e0000, DATA_PAGES */
static int aarch64_trampoline_reserved;  /* 0x401ff000, 1 page */

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

const char_t *arch_kernel_path(void)
{
    return L"\\kernel.elf";
}

efi_status_t arch_init_handoff(struct boot_context **ctx_out)
{
    efi_physical_address_t addr;
    struct boot_context *ctx;
    EFI_STATUS status;

    addr = (efi_physical_address_t)AARCH64_HANDOFF_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                               (uintn_t)AARCH64_HANDOFF_DATA_PAGES, &addr);
    if (EFI_ERROR(status) || addr != AARCH64_HANDOFF_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, (uintn_t)AARCH64_HANDOFF_DATA_PAGES);
        return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
    }
    aarch64_data_reserved = 1;

    addr = (efi_physical_address_t)AARCH64_TRAMPOLINE_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderCode, 1, &addr);
    if (EFI_ERROR(status) || addr != AARCH64_TRAMPOLINE_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, 1);
        status = EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
        goto fail;
    }
    aarch64_trampoline_reserved = 1;

    memset((void *)(uintptr_t)AARCH64_HANDOFF_BASE, 0,
           (size_t)(AARCH64_HANDOFF_END - AARCH64_HANDOFF_BASE));
    ctx = (struct boot_context *)(uintptr_t)AARCH64_HANDOFF_BASE;
    boot_context_init(ctx);
    ctx->boot_cpu_id = read_mpidr();
    ctx->flags |= BOOT_CONTEXT_HAS_BOOT_CPU_ID;
    handoff_cursor = align_up_8(AARCH64_HANDOFF_BASE + sizeof(*ctx));

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

    firmware_fdt = find_fdt(&fdt_size);
    if (!firmware_fdt)
        return EFI_SUCCESS;
    if ((uint64_t)fdt_size > AARCH64_TRAMPOLINE_BASE - handoff_cursor)
        return EFI_BUFFER_TOO_SMALL;
    memcpy((void *)(uintptr_t)handoff_cursor, firmware_fdt, (size_t)fdt_size);
    ctx->firmware.dtb = handoff_cursor;
    ctx->flags |= BOOT_CONTEXT_HAS_DTB;
    handoff_cursor = align_up_8(handoff_cursor + (uint64_t)fdt_size);
    return EFI_SUCCESS;
}

void arch_memory_buffer(efi_physical_address_t *phys_out,
                        uint64_t *capacity_out)
{
    *phys_out = handoff_cursor;
    *capacity_out = AARCH64_TRAMPOLINE_BASE - handoff_cursor;
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
    if (aarch64_data_reserved) {
        (void)BS->FreePages((efi_physical_address_t)AARCH64_HANDOFF_BASE,
                            (uintn_t)AARCH64_HANDOFF_DATA_PAGES);
        aarch64_data_reserved = 0;
    }
}

void arch_puts(const char *s)
{
    pl011_puts(s);
}

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys)
{
    aarch64_enter_kernel(entry, context_phys);
}