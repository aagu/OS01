/* kernel/arch/aarch64/bus/pci.c — AArch64 PCIe ECAM MMIO pre-mapping & PCI backend */
#include <arch/pci.h>
#include <arch/aarch64/pcie.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/uefi_memory_map.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/aarch64/boot_layout.h>
#include <core/bootinfo.h>
#include <arch/boot.h>
#include <arch/mmu.h>
#include <memory/pmm.h>
#include <log/log.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static struct aarch64_pcie_root_desc s_ready_roots[AARCH64_PCIE_MAX_ROOTS];
static struct pci_root s_pci_roots[AARCH64_PCIE_MAX_ROOTS];
static uint32_t s_ready_root_count = 0U;

#if defined(__aarch64__) && !defined(AARCH64_PCIE_HOST_TEST)
static inline uint32_t aarch64_pcie_mmio_read32(uintptr_t va)
{
    uint32_t val = 0U;
    __asm__ __volatile__("ldr %w0, [%1]\n\tdsb ld" : "=r"(val) : "r"(va) : "memory");
    return val;
}

static inline void aarch64_pcie_mmio_write32(uintptr_t va, uint32_t val)
{
    __asm__ __volatile__("str %w0, [%1]\n\tdsb st" :: "r"(val), "r"(va) : "memory");
}

static inline void aarch64_pcie_dsb_sy_isb(void)
{
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
}
#else
static inline uint32_t aarch64_pcie_mmio_read32(uintptr_t va)
{
    uint32_t val = *(const volatile uint32_t *)va;
    __asm__ __volatile__("" ::: "memory");
    return val;
}

static inline void aarch64_pcie_mmio_write32(uintptr_t va, uint32_t val)
{
    *(volatile uint32_t *)va = val;
    __asm__ __volatile__("" ::: "memory");
}

static inline void aarch64_pcie_dsb_sy_isb(void)
{
    __asm__ __volatile__("" ::: "memory");
}
#endif

static const struct aarch64_pcie_root_desc *find_ready_root(uint16_t domain)
{
    for (uint32_t i = 0U; i < s_ready_root_count; i++) {
        if (s_ready_roots[i].valid && s_ready_roots[i].domain == domain) {
            return &s_ready_roots[i];
        }
    }
    return NULL;
}

static int aarch64_pci_read32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                              uint16_t offset, uint32_t *out, enum pci_error_scope *scope)
{
    if (scope) {
        *scope = PCI_ERROR_FUNCTION;
    }
    const struct aarch64_pcie_root_desc *r = find_ready_root(domain);
    if (!r) {
        if (scope) {
            *scope = PCI_ERROR_ROOT;
        }
        return -ENODEV;
    }
    if (!out || bus < r->bus_start || bus > r->bus_end ||
        slot >= 32U || fn >= 8U || (offset & 3U) != 0U || offset > 4092U) {
        return -EINVAL;
    }

    uint64_t ecam_off = ((uint64_t)(bus - r->bus_start) << 20)
                      | ((uint64_t)slot << 15)
                      | ((uint64_t)fn << 12)
                      | (uint64_t)offset;
    uintptr_t va = (uintptr_t)(ARCH_PAGE_OFFSET + r->reg_cpu_base + ecam_off);
    *out = aarch64_pcie_mmio_read32(va);
    return 0;
}

static int aarch64_pci_write32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                               uint16_t offset, uint32_t value, enum pci_error_scope *scope)
{
    if (scope) {
        *scope = PCI_ERROR_FUNCTION;
    }
    const struct aarch64_pcie_root_desc *r = find_ready_root(domain);
    if (!r) {
        if (scope) {
            *scope = PCI_ERROR_ROOT;
        }
        return -ENODEV;
    }
    if (bus < r->bus_start || bus > r->bus_end ||
        slot >= 32U || fn >= 8U || (offset & 3U) != 0U || offset > 4092U) {
        return -EINVAL;
    }

    uint64_t ecam_off = ((uint64_t)(bus - r->bus_start) << 20)
                      | ((uint64_t)slot << 15)
                      | ((uint64_t)fn << 12)
                      | (uint64_t)offset;
    uintptr_t va = (uintptr_t)(ARCH_PAGE_OFFSET + r->reg_cpu_base + ecam_off);
    aarch64_pcie_mmio_write32(va, value);
    return 0;
}

static int aarch64_pci_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    (void)pdev;
    (void)out;
    return -ENOTSUP;
}

static int aarch64_pci_translate_resource(uint16_t domain, uint8_t space,
                                          uint64_t pci_addr, uint64_t size,
                                          uint64_t *out_cpu_addr)
{
    if (!out_cpu_addr) {
        return -EINVAL;
    }
    const struct aarch64_pcie_root_desc *r = find_ready_root(domain);
    if (!r) {
        return -ENODEV;
    }
    if (size > 0ULL && pci_addr > UINT64_MAX - size) {
        return -EOVERFLOW;
    }

    for (uint8_t i = 0U; i < r->range_count; i++) {
        const struct aarch64_pcie_range *rng = &r->ranges[i];
        if (rng->space != space || pci_addr < rng->pci_base) {
            continue;
        }
        uint64_t off = pci_addr - rng->pci_base;
        if (off >= rng->size || size > rng->size - off) {
            continue;
        }
        *out_cpu_addr = rng->cpu_base + off;
        return 0;
    }
    return -ENOENT;
}

static int aarch64_pci_bus_range(uint16_t domain, uint8_t *out_bus_start, uint8_t *out_bus_end)
{
    const struct aarch64_pcie_root_desc *r = find_ready_root(domain);
    if (!r) {
        return -ENODEV;
    }
    if (out_bus_start) {
        *out_bus_start = r->bus_start;
    }
    if (out_bus_end) {
        *out_bus_end = r->bus_end;
    }
    return 0;
}

static struct pci_backend s_aarch64_pci_backend = {
    .roots = s_pci_roots,
    .root_count = 0,
    .read32 = aarch64_pci_read32,
    .write32 = aarch64_pci_write32,
    .route_gsi = aarch64_pci_route_gsi,
    .max_config_offset = 4092U,
    .translate_resource = aarch64_pci_translate_resource,
    .bus_range = aarch64_pci_bus_range,
};

int aarch64_pcie_prepare(const struct boot_context *ctx)
{
    s_ready_root_count = 0U;
    s_aarch64_pci_backend.root_count = 0U;
    for (uint32_t i = 0U; i < AARCH64_PCIE_MAX_ROOTS; i++) {
        s_ready_roots[i] = (struct aarch64_pcie_root_desc){0};
        s_pci_roots[i] = (struct pci_root){0};
    }

    const struct aarch64_pcie_topology *topo = aarch64_pcie_topology_get();
    if (!topo || topo->root_count == 0U) {
        return 0;
    }

    /* Validate UEFI raw memory map pointer & M0 bounds before dereferencing. */
    if (!boot_context_valid(ctx) ||
        (ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0u ||
        ctx->memory.format != BOOT_MEMORY_FORMAT_UEFI_RAW ||
        ctx->memory.entries == 0u ||
        ctx->memory.entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE ||
        ctx->memory.entry_count == 0u) {
        log_warn("pcie: invalid UEFI memory map in boot_context, disabling PCIe\n");
        return 0;
    }

    uint32_t stride = ctx->memory.entry_size;
    uint32_t desc_count = ctx->memory.entry_count;
    if ((uint64_t)desc_count > UINT64_MAX / stride) {
        log_warn("pcie: UEFI memory map size overflow, disabling PCIe\n");
        return 0;
    }
    uint64_t map_bytes = (uint64_t)desc_count * stride;
    if (ctx->memory.entries > UINT64_MAX - map_bytes) {
        log_warn("pcie: UEFI memory map end overflow, disabling PCIe\n");
        return 0;
    }
    uint64_t uefi_map_end = ctx->memory.entries + map_bytes;
    if (uefi_map_end > (uint64_t)AARCH64_BOOT_MAP_END ||
        (ctx->memory.entries < (uint64_t)AARCH64_BOOT_BLOCK_END &&
         uefi_map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)) {
        log_warn("pcie: UEFI memory map outside safe M0 window, disabling PCIe\n");
        return 0;
    }

    const void *mmap_ptr = (const void *)(uintptr_t)ctx->memory.entries;
    const uint64_t blk_sz = (uint64_t)PAGE_2M_SIZE;
    const uint64_t blk_mask = (uint64_t)PAGE_2M_MASK;
    uint64_t ttbr_raw = aarch64_read_ttbr1();
    uint64_t root_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *ttbr1_root = (uint64_t *)(uintptr_t)(ARCH_PAGE_OFFSET + root_pa);

    uint32_t limit = topo->root_count;
    if (limit > AARCH64_PCIE_MAX_ROOTS) {
        limit = AARCH64_PCIE_MAX_ROOTS;
    }

    for (uint32_t i = 0U; i < limit; i++) {
        const struct aarch64_pcie_root_desc *r = &topo->roots[i];
        if (!r->valid || r->kind != AARCH64_PCIE_CTRL_ECAM_GENERIC) {
            continue;
        }
        if (r->bus_start > r->bus_end || (r->reg_cpu_base & 0xFFFULL) != 0ULL) {
            log_warn("pcie: skipping invalid root domain=%u\n", (unsigned int)r->domain);
            continue;
        }

        uint64_t active_size = ((uint64_t)(r->bus_end - r->bus_start + 1U)) << 20;
        if (active_size > r->reg_size || r->reg_cpu_base > UINT64_MAX - active_size) {
            log_warn("pcie: root domain=%u active_size overflow\n", (unsigned int)r->domain);
            continue;
        }

        uint64_t map_start = r->reg_cpu_base & blk_mask;
        uint64_t last_blk = (r->reg_cpu_base + active_size - 1ULL) & blk_mask;
        if (last_blk > UINT64_MAX - blk_sz) {
            log_warn("pcie: root domain=%u map_end overflow\n", (unsigned int)r->domain);
            continue;
        }
        uint64_t map_end = last_blk + blk_sz;
        if (map_start >= map_end || map_end > AARCH64_PT_PA_LIMIT) {
            log_warn("pcie: root domain=%u ECAM PA %lx exceeds 1 TiB limit\n",
                     (unsigned int)r->domain, (unsigned long)map_end);
            continue;
        }
        if (map_start < (uint64_t)AARCH64_BOOT_BLOCK_END &&
            map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE) {
            log_warn("pcie: root domain=%u overlaps Boot Block B\n", (unsigned int)r->domain);
            continue;
        }

        bool ok = true;
        for (uint64_t blk_pa = map_start; blk_pa < map_end; blk_pa += blk_sz) {
            int ov = aarch64_uefi_range_overlaps_ram(
                mmap_ptr, stride, desc_count, blk_pa, blk_pa + blk_sz);
            if (ov != 0) {
                log_warn("pcie: root domain=%u ECAM blk %lx overlaps RAM or bad map\n",
                         (unsigned int)r->domain, (unsigned long)blk_pa);
                ok = false;
                break;
            }
            int mrc = aarch64_pt_ensure_device_2m(ttbr1_root, ARCH_PAGE_OFFSET + blk_pa, blk_pa);
            if (mrc != AARCH64_PT_OK) {
                log_warn("pcie: root domain=%u map_2m failed blk=%lx rc=%d\n",
                         (unsigned int)r->domain, (unsigned long)blk_pa, mrc);
                ok = false;
                break;
            }
        }

        if (ok) {
            s_ready_roots[s_ready_root_count] = *r;
            s_pci_roots[s_ready_root_count].domain = r->domain;
            s_pci_roots[s_ready_root_count].bus = r->bus_start;
            s_ready_root_count++;
        }
    }

    if (s_ready_root_count > 0U) {
        aarch64_pcie_dsb_sy_isb();
        s_aarch64_pci_backend.root_count = s_ready_root_count;
        log_info("pcie: ready %u ECAM root(s)\n", (unsigned int)s_ready_root_count);
    }
    return 0;
}

const struct pci_backend *arch_pci_backend(void)
{
    return (s_ready_root_count > 0U) ? &s_aarch64_pci_backend : NULL;
}
