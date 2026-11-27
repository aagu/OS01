/* kernel/arch/aarch64/bus/pci.c — AArch64 PCIe ECAM MMIO pre-mapping & PCI backend */
#include <arch/pci.h>
#include <bus/pci/pci.h>
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

#define AARCH64_PCIE_MAX_XHCI 4U
#define AARCH64_XHCI_BAR0_SIZE 0x4000ULL

struct aarch64_xhci_bar0_window {
    bool valid;
    uint16_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t fn;
    uint64_t cpu_pa;
    uint64_t size;
};

static struct aarch64_xhci_bar0_window s_xhci_bar0_windows[AARCH64_PCIE_MAX_XHCI];
static uint32_t s_xhci_bar0_count = 0U;

#define GICV2M_MSI_TYPER     0x008ULL
#define GICV2M_MSI_SETSPI_NS 0x040ULL

static bool s_v2m_ready = false;
static uint64_t s_v2m_base = 0ULL;
static uint32_t s_v2m_phandle = 0U;
static uint32_t s_v2m_base_spi = 0U;
static uint32_t s_v2m_num_spis = 0U;

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
    if (!pdev || !out) {
        return -EINVAL;
    }
    const struct aarch64_pcie_root_desc *r = find_ready_root(pdev->domain);
    if (!r) {
        return -ENODEV;
    }
    if (!r->has_int_map || r->int_map_count == 0U) {
        return -ENOTSUP;
    }

    enum pci_error_scope scope = PCI_ERROR_FUNCTION;
    uint32_t reg = 0U;
    int rc = aarch64_pci_read32(pdev->domain, pdev->bus, pdev->slot, pdev->fn,
                                0x3C, &reg, &scope);
    if (rc != 0) {
        return rc;
    }

    uint8_t int_pin = (uint8_t)((reg >> 8) & 0xFFU);
    if (int_pin == 0U || int_pin > 4U) {
        return -ENOTSUP;
    }

    uint32_t phys_hi = ((uint32_t)pdev->bus << 16)
                     | ((uint32_t)pdev->slot << 11)
                     | ((uint32_t)pdev->fn << 8);
    uint32_t masked_hi = phys_hi & r->int_map_mask[0];
    uint32_t masked_pin = (uint32_t)int_pin & r->int_map_mask[3];

    for (uint8_t i = 0U; i < r->int_map_count; i++) {
        const struct aarch64_pcie_intmap_entry *e = &r->int_map[i];
        if (e->addr_hi == masked_hi && e->int_pin == masked_pin) {
            *out = e->gic_intid;
            return 0;
        }
    }
    return -ENOENT;
}

static bool pcie_range_contains(uint64_t win_base, uint64_t win_size,
                                uint64_t addr, uint64_t length,
                                uint64_t *out_off)
{
    if (win_size == 0ULL || addr < win_base) {
        return false;
    }
    uint64_t off = addr - win_base;
    if (off >= win_size || length > win_size - off) {
        return false;
    }
    if (out_off) {
        *out_off = off;
    }
    return true;
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
        uint64_t off = 0ULL;
        if (rng->space != space ||
            !pcie_range_contains(rng->pci_base, rng->size, pci_addr, size, &off)) {
            continue;
        }
        *out_cpu_addr = rng->cpu_base + off;
        return 0;
    }
    return -ENOENT;
}

static int aarch64_pci_map_mmio(uint16_t domain, uint64_t cpu_phys, uint64_t length, void **out_virt)
{
    if (!out_virt || length == 0ULL) {
        return -EINVAL;
    }
    *out_virt = NULL;
    if (cpu_phys > UINT64_MAX - length) {
        return -EINVAL;
    }

    const struct aarch64_pcie_root_desc *r = find_ready_root(domain);
    if (!r) {
        return -ENODEV;
    }

    for (uint8_t i = 0U; i < r->range_count; i++) {
        const struct aarch64_pcie_range *rng = &r->ranges[i];
        if (!rng->mapped) {
            continue;
        }
        if (pcie_range_contains(rng->cpu_base, rng->size, cpu_phys, length, NULL)) {
            *out_virt = (void *)(uintptr_t)(ARCH_PAGE_OFFSET + cpu_phys);
            return 0;
        }
    }
    for (uint32_t i = 0U; i < s_xhci_bar0_count; i++) {
        const struct aarch64_xhci_bar0_window *win = &s_xhci_bar0_windows[i];
        if (win->valid && win->domain == domain &&
            pcie_range_contains(win->cpu_pa, win->size, cpu_phys, length, NULL)) {
            *out_virt = (void *)(uintptr_t)(ARCH_PAGE_OFFSET + cpu_phys);
            return 0;
        }
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
    .map_mmio = aarch64_pci_map_mmio,
};

static bool pcie_map_device_window_2m(uint16_t domain, const char *label,
                                      uint64_t base_pa, uint64_t size,
                                      const void *mmap_ptr, uint32_t stride,
                                      uint32_t desc_count, uint64_t *ttbr1_root)
{
    const uint64_t blk_sz = (uint64_t)PAGE_2M_SIZE;
    const uint64_t blk_mask = (uint64_t)PAGE_2M_MASK;

    if (size == 0ULL || base_pa > UINT64_MAX - size) {
        log_warn("pcie: root domain=%u %s size overflow\n",
                 (unsigned int)domain, label);
        return false;
    }

    uint64_t map_start = base_pa & blk_mask;
    uint64_t last_blk = (base_pa + size - 1ULL) & blk_mask;
    if (last_blk > UINT64_MAX - blk_sz) {
        log_warn("pcie: root domain=%u %s map_end overflow\n",
                 (unsigned int)domain, label);
        return false;
    }
    uint64_t map_end = last_blk + blk_sz;
    if (map_start >= map_end || map_end > AARCH64_PT_PA_LIMIT) {
        log_warn("pcie: root domain=%u %s PA %lx exceeds 1 TiB limit\n",
                 (unsigned int)domain, label, (unsigned long)map_end);
        return false;
    }
    if (map_start < (uint64_t)AARCH64_BOOT_BLOCK_END &&
        map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE) {
        log_warn("pcie: root domain=%u %s overlaps Boot Block B\n",
                 (unsigned int)domain, label);
        return false;
    }

    for (uint64_t blk_pa = map_start; blk_pa < map_end; blk_pa += blk_sz) {
        int ov = aarch64_uefi_range_overlaps_ram(
            mmap_ptr, stride, desc_count, blk_pa, blk_pa + blk_sz);
        if (ov != 0) {
            log_warn("pcie: root domain=%u %s blk %lx overlaps RAM or bad map\n",
                     (unsigned int)domain, label, (unsigned long)blk_pa);
            return false;
        }
        int mrc = aarch64_pt_ensure_device_2m(ttbr1_root, ARCH_PAGE_OFFSET + blk_pa, blk_pa);
        if (mrc != AARCH64_PT_OK) {
            log_warn("pcie: root domain=%u %s map_2m failed blk=%lx rc=%d\n",
                     (unsigned int)domain, label, (unsigned long)blk_pa, mrc);
            return false;
        }
    }
    return true;
}

static void pcie_prepare_xhci_bar0(const struct aarch64_pcie_root_desc *ready,
                                   const void *mmap_ptr, uint32_t stride,
                                   uint32_t desc_count, uint64_t *ttbr1_root)
{
    uint8_t bus = ready->bus_start;
    for (uint8_t slot = 0U; slot < 32U; slot++) {
        for (uint8_t fn = 0U; fn < 8U; fn++) {
            if (s_xhci_bar0_count >= AARCH64_PCIE_MAX_XHCI) {
                return;
            }
            uint32_t id_reg = 0U;
            if (aarch64_pci_read32(ready->domain, bus, slot, fn, 0x00U, &id_reg, NULL) != 0) {
                continue;
            }
            uint16_t vendor = (uint16_t)(id_reg & 0xFFFFU);
            uint16_t device = (uint16_t)((id_reg >> 16) & 0xFFFFU);
            if (vendor != 0x1b36U || device != 0x000dU) {
                continue;
            }

            uint32_t class_reg = 0U;
            if (aarch64_pci_read32(ready->domain, bus, slot, fn, 0x08U, &class_reg, NULL) != 0) {
                continue;
            }
            if (((class_reg >> 8) & 0xFFFFFFU) != 0x0c0330U) {
                continue;
            }

            uint32_t bar0_lo = 0U;
            if (aarch64_pci_read32(ready->domain, bus, slot, fn, 0x10U, &bar0_lo, NULL) != 0) {
                continue;
            }
            if ((bar0_lo & 0x1U) != 0U) {
                continue;
            }

            uint8_t bar_type = (uint8_t)((bar0_lo >> 1) & 0x3U);
            uint64_t pci_addr = 0ULL;
            uint8_t space = PCI_SPACE_MMIO32;
            if (bar_type == 0x2U) {
                uint32_t bar0_hi = 0U;
                if (aarch64_pci_read32(ready->domain, bus, slot, fn, 0x14U, &bar0_hi, NULL) != 0) {
                    continue;
                }
                pci_addr = ((uint64_t)bar0_hi << 32) | (uint64_t)(bar0_lo & ~0xFULL);
                space = PCI_SPACE_MMIO64;
            } else if (bar_type == 0x0U) {
                pci_addr = (uint64_t)(bar0_lo & ~0xFULL);
                space = PCI_SPACE_MMIO32;
            } else {
                continue;
            }

            if (pci_addr == 0ULL || (pci_addr & (AARCH64_XHCI_BAR0_SIZE - 1ULL)) != 0ULL) {
                continue;
            }

            uint64_t cpu_pa = 0ULL;
            if (aarch64_pci_translate_resource(ready->domain, space, pci_addr,
                                               AARCH64_XHCI_BAR0_SIZE, &cpu_pa) != 0) {
                continue;
            }

            if (!pcie_map_device_window_2m(ready->domain, "xHCI-BAR0", cpu_pa,
                                           AARCH64_XHCI_BAR0_SIZE, mmap_ptr, stride,
                                           desc_count, ttbr1_root)) {
                continue;
            }

            s_xhci_bar0_windows[s_xhci_bar0_count++] = (struct aarch64_xhci_bar0_window){
                .valid = true,
                .domain = ready->domain,
                .bus = bus,
                .slot = slot,
                .fn = fn,
                .cpu_pa = cpu_pa,
                .size = AARCH64_XHCI_BAR0_SIZE,
            };
        }
    }
}

bool aarch64_pcie_xhci_bar0_ready(const struct pci_device *pdev)
{
    if (!pdev || pdev->vendor != 0x1b36U || pdev->device != 0x000dU ||
        pdev->class_code != 0x0c0330U) {
        return false;
    }
    const struct aarch64_xhci_bar0_window *win = NULL;
    for (uint32_t i = 0U; i < s_xhci_bar0_count; i++) {
        if (s_xhci_bar0_windows[i].valid &&
            s_xhci_bar0_windows[i].domain == pdev->domain &&
            s_xhci_bar0_windows[i].bus == pdev->bus &&
            s_xhci_bar0_windows[i].slot == pdev->slot &&
            s_xhci_bar0_windows[i].fn == pdev->fn) {
            win = &s_xhci_bar0_windows[i];
            break;
        }
    }
    if (!win) {
        return false;
    }

    uint32_t bar0_lo = 0U;
    if (aarch64_pci_read32(pdev->domain, pdev->bus, pdev->slot, pdev->fn,
                           0x10U, &bar0_lo, NULL) != 0 || (bar0_lo & 0x1U) != 0U) {
        return false;
    }
    uint8_t bar_type = (uint8_t)((bar0_lo >> 1) & 0x3U);
    uint64_t pci_addr = 0ULL;
    uint8_t space = PCI_SPACE_MMIO32;
    if (bar_type == 0x2U) {
        uint32_t bar0_hi = 0U;
        if (aarch64_pci_read32(pdev->domain, pdev->bus, pdev->slot, pdev->fn,
                               0x14U, &bar0_hi, NULL) != 0) {
            return false;
        }
        pci_addr = ((uint64_t)bar0_hi << 32) | (uint64_t)(bar0_lo & ~0xFULL);
        space = PCI_SPACE_MMIO64;
    } else if (bar_type == 0x0U) {
        pci_addr = (uint64_t)(bar0_lo & ~0xFULL);
        space = PCI_SPACE_MMIO32;
    } else {
        return false;
    }

    uint64_t cur_cpu_pa = 0ULL;
    if (aarch64_pci_translate_resource(pdev->domain, space, pci_addr,
                                       win->size, &cur_cpu_pa) != 0) {
        return false;
    }
    if (cur_cpu_pa != win->cpu_pa) {
        return false;
    }
    if (!pdev->bars[0].valid || pdev->bars[0].address != win->cpu_pa) {
        return false;
    }
    return true;
}

int aarch64_pcie_prepare(const struct boot_context *ctx)
{
    s_ready_root_count = 0U;
    s_xhci_bar0_count = 0U;
    for (uint32_t i = 0U; i < AARCH64_PCIE_MAX_XHCI; i++) {
        s_xhci_bar0_windows[i] = (struct aarch64_xhci_bar0_window){0};
    }
    s_aarch64_pci_backend.root_count = 0U;
    s_v2m_ready = false;
    s_v2m_base = 0ULL;
    s_v2m_phandle = 0U;
    s_v2m_base_spi = 0U;
    s_v2m_num_spis = 0U;
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

        if (!pcie_map_device_window_2m(r->domain, "ECAM", r->reg_cpu_base, active_size,
                                       mmap_ptr, stride, desc_count, ttbr1_root)) {
            continue;
        }

        struct aarch64_pcie_root_desc ready = *r;
        for (uint8_t j = 0U; j < AARCH64_PCIE_MAX_RANGES; j++) {
            ready.ranges[j].mapped = false;
        }

        /* Pre-map bounded MMIO32 BAR windows before VMM gate closes;
         * skipped or failed windows keep mapped=false so probe rejects them. */
        for (uint8_t j = 0U; j < ready.range_count && j < AARCH64_PCIE_MAX_RANGES; j++) {
            struct aarch64_pcie_range *rng = &ready.ranges[j];
            if (rng->space != PCI_SPACE_MMIO32 || rng->size == 0ULL) {
                continue;
            }
            if (rng->size > (1ULL << 30)) {
                continue;
            }
            if (pcie_map_device_window_2m(ready.domain, "MMIO32", rng->cpu_base, rng->size,
                                          mmap_ptr, stride, desc_count, ttbr1_root)) {
                rng->mapped = true;
            }
        }

        s_ready_roots[s_ready_root_count] = ready;
        s_pci_roots[s_ready_root_count].domain = ready.domain;
        s_pci_roots[s_ready_root_count].bus = ready.bus_start;
        s_ready_root_count++;
        pcie_prepare_xhci_bar0(&ready, mmap_ptr, stride, desc_count, ttbr1_root);
    }

    if (s_ready_root_count > 0U) {
        aarch64_pcie_dsb_sy_isb();
        s_aarch64_pci_backend.root_count = s_ready_root_count;
        log_info("pcie: ready %u ECAM root(s)\n", (unsigned int)s_ready_root_count);

        if (topo->v2m.valid && topo->v2m.reg_cpu_base <= 0xFFFFFFFFULL - GICV2M_MSI_SETSPI_NS) {
            if (pcie_map_device_window_2m(0U, "GICv2m", topo->v2m.reg_cpu_base,
                                           topo->v2m.reg_size, mmap_ptr, stride,
                                           desc_count, ttbr1_root)) {
                aarch64_pcie_dsb_sy_isb();
                uint32_t typer = aarch64_pcie_mmio_read32(
                    (uintptr_t)(ARCH_PAGE_OFFSET + topo->v2m.reg_cpu_base + GICV2M_MSI_TYPER));
                uint32_t base_spi = topo->v2m.msi_base_spi ? topo->v2m.msi_base_spi
                                                           : ((typer >> 16) & 0x3FFU);
                uint32_t num_spis = topo->v2m.msi_num_spis ? topo->v2m.msi_num_spis
                                                           : (typer & 0x3FFU);
                if (num_spis > 0U && base_spi >= 32U && base_spi + num_spis <= 1020U) {
                    s_v2m_ready = true;
                    s_v2m_base = topo->v2m.reg_cpu_base;
                    s_v2m_phandle = topo->v2m.phandle;
                    s_v2m_base_spi = base_spi;
                    s_v2m_num_spis = num_spis;
                    log_info("pcie: GICv2m @ %lx base_spi=%u num_spis=%u\n",
                             (unsigned long)s_v2m_base,
                             (unsigned int)s_v2m_base_spi,
                             (unsigned int)s_v2m_num_spis);
                }
            }
        }
    }
    return 0;
}

static bool aarch64_v2m_matches_dev(const struct pci_device *pdev)
{
    if (!s_v2m_ready || !pdev) {
        return false;
    }
    const struct aarch64_pcie_root_desc *r = find_ready_root(pdev->domain);
    if (!r) {
        return false;
    }
    if (r->msi_phandle != 0U && s_v2m_phandle != 0U && r->msi_phandle != s_v2m_phandle) {
        return false;
    }
    return true;
}

int arch_pci_msix_map(const struct pci_device *pdev, uint64_t table_phys, void **out_virt)
{
    if (!pdev || !out_virt) {
        return -EINVAL;
    }
    *out_virt = NULL;
    if (!aarch64_v2m_matches_dev(pdev)) {
        return -ENOTSUP;
    }
    return aarch64_pci_map_mmio(pdev->domain, table_phys, 16ULL, out_virt);
}

void arch_pci_msix_unmap(const struct pci_device *pdev, void *virt)
{
    (void)pdev;
    (void)virt;
}

uint32_t arch_pci_msi_address(const struct pci_device *pdev)
{
    if (!aarch64_v2m_matches_dev(pdev)) {
        return 0U;
    }
    return (uint32_t)(s_v2m_base + GICV2M_MSI_SETSPI_NS);
}

const struct pci_backend *arch_pci_backend(void)
{
    return (s_ready_root_count > 0U) ? &s_aarch64_pci_backend : NULL;
}
