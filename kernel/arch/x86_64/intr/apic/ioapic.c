#include <intr/apic.h>
#include <arch/x86_64/irq_route.h>
#include <arch/spinlock.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <core/debug.h>
#include <arch/io.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <arch/x86_64/vmm.h>

static bool s_ioapic_ready = false;
static spinlock_T s_ioapic_locks[MAX_IOAPICS] = { {1}, {1}, {1}, {1} };

bool ioapic_available(void)
{
    return s_ioapic_ready;
}

// ──────────────────────────────────────────────
//  I/O APIC register access (indirect index/data)
//  base = physical address; caller must have mapped the 2MB region.
//  selector/data access is serialized per-controller via s_ioapic_locks.
// ──────────────────────────────────────────────

static inline void ioapic_write_reg(uint32_t ioapic_idx, uint8_t reg, uint32_t value)
{
    ioapic_entry_t *ioapic = &apic_info.ioapics[ioapic_idx];
    uint64_t flags = spin_lock_irqsave(&s_ioapic_locks[ioapic_idx]);
    volatile uint32_t *regsel = (volatile uint32_t *)Phy_To_Virt(ioapic->mmio_base);
    volatile uint32_t *iowin  = (volatile uint32_t *)Phy_To_Virt(ioapic->mmio_base + 0x10);

    *regsel = reg;
    *iowin  = value;
    spin_unlock_irqrestore(&s_ioapic_locks[ioapic_idx], flags);
}

static inline uint32_t ioapic_read_reg(uint32_t ioapic_idx, uint8_t reg)
{
    ioapic_entry_t *ioapic = &apic_info.ioapics[ioapic_idx];
    uint64_t flags = spin_lock_irqsave(&s_ioapic_locks[ioapic_idx]);
    volatile uint32_t *regsel = (volatile uint32_t *)Phy_To_Virt(ioapic->mmio_base);
    volatile uint32_t *iowin  = (volatile uint32_t *)Phy_To_Virt(ioapic->mmio_base + 0x10);

    *regsel = reg;
    uint32_t val = *iowin;
    spin_unlock_irqrestore(&s_ioapic_locks[ioapic_idx], flags);
    return val;
}

// ──────────────────────────────────────────────
//  Diagnostics: dump IOAPIC redirection entries
// ──────────────────────────────────────────────

void ioapic_dump_entries(void)
{
    for (uint32_t i = 0; i < apic_info.ioapic_count; i++) {
        ioapic_entry_t *ioapic = &apic_info.ioapics[i];
        debug_irq("IOAPIC[%u]: base=%#010x gsi_base=%u max_redir=%u\n",
                      i, ioapic->mmio_base, ioapic->gsi_base, ioapic->max_redir);
        for (uint32_t n = 0; n <= ioapic->max_redir; n++) {
            uint32_t low  = ioapic_read_reg(i, IOAPIC_REG_REDTBL(n));
            if (OS01_DEBUG_irq) {
                uint32_t high __attribute__((unused)) = ioapic_read_reg(i, IOAPIC_REG_REDTBL(n) + 1);
                debug_irq("  [%2u] %#018lx", n, ((uint64_t)high << 32) | low);
                if (!(low & IOAPIC_RED_MASK))
                    debug_irq(" (enabled vec=%#x dest=%u)",
                              low & 0xFF, (high >> 24) & 0xFF);
                debug_irq("\n");
            }
        }
    }
}

// ──────────────────────────────────────────────
//  I/O APIC → hw_int_controller_t interface
// ──────────────────────────────────────────────

static uint64_t ioapic_install(uint64_t nr, void *arg __attribute__((unused)))
{
    const struct x86_irq_route *route = x86_irq_route_find_by_vector(nr);
    if (route) {
        debug_irq("IOAPIC: install IRQ %u (GSI %u pin %u on IOAPIC %u)\n",
                  (unsigned)nr, route->gsi, route->pin, route->ioapic_idx);
    }
    return 0;
}

static void ioapic_uninstall(uint64_t nr)
{
    const struct x86_irq_route *route = x86_irq_route_find_by_vector(nr);
    if (!route || route->ioapic_idx >= apic_info.ioapic_count)
        return;

    uint32_t low = ioapic_read_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(route->pin));
    ioapic_write_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(route->pin), low | IOAPIC_RED_MASK);

    debug_irq("IOAPIC: uninstall IRQ %u (GSI %u pin %u)\n",
              (unsigned)nr, route->gsi, route->pin);
}

static void ioapic_enable(uint64_t nr)
{
    const struct x86_irq_route *route = x86_irq_route_find_by_vector(nr);
    if (!route || route->ioapic_idx >= apic_info.ioapic_count) {
        debug_irq("IOAPIC: no route found for vector %#lx\n", nr);
        return;
    }

    uint8_t redir = route->pin;
    uint32_t low_flags = route->flags;
    irq_desc_t *desc = irq_desc_for_gsi(route->gsi);
    if (desc) {
        if (desc->flags & IRQF_TRIGGER_LEVEL)
            low_flags |= IOAPIC_RED_TRIG_LEVEL;
        else if (desc->flags & IRQF_TRIGGER_EDGE)
            low_flags &= ~IOAPIC_RED_TRIG_LEVEL;
    }
    low_flags |= (uint32_t)(nr & 0xFF);
    low_flags |= IOAPIC_RED_DEL_FIXED;

    uint32_t bsp_lapic_id = (lapic_read(LAPIC_ID) >> 24) & 0xFF;
    uint32_t high = bsp_lapic_id << 24;

    ioapic_write_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(redir) + 1, high);
    ioapic_write_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(redir), low_flags);

    debug_irq("IOAPIC: enable IRQ %u (GSI %u pin %u) → vector %#x dest=%#x low=%#x\n",
              (unsigned)nr, route->gsi, redir, (unsigned)nr, bsp_lapic_id, low_flags);
}

static void ioapic_disable(uint64_t nr)
{
    const struct x86_irq_route *route = x86_irq_route_find_by_vector(nr);
    if (!route || route->ioapic_idx >= apic_info.ioapic_count)
        return;

    uint8_t redir = route->pin;
    uint32_t low = ioapic_read_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(redir));
    ioapic_write_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(redir), low | IOAPIC_RED_MASK);

    debug_irq("IOAPIC: disable IRQ %u (GSI %u pin %u)\n",
              (unsigned)nr, route->gsi, redir);
}

static void ioapic_ack(uint64_t nr __attribute__((unused)))
{
    // Acknowledge via LAPIC EOI register
    lapic_eoi();
}

// ──────────────────────────────────────────────
//  The I/O APIC controller instance
// ──────────────────────────────────────────────

hw_int_controller_t ioapic_controller = {
    .enable    = ioapic_enable,
    .disable   = ioapic_disable,
    .install   = ioapic_install,
    .uninstall = ioapic_uninstall,
    .ack       = ioapic_ack,
};

hw_int_controller_t * get_ioapic_controller(void)
{
    return &ioapic_controller;
}

// ──────────────────────────────────────────────
//  Initialization
// ──────────────────────────────────────────────

int ioapic_init(void)
{
    for (uint32_t k = 0; k < MAX_IOAPICS; k++) {
        spin_init(&s_ioapic_locks[k]);
    }

    if (apic_info.ioapic_count == 0) {
        debug_irq("IOAPIC: no I/O APICs found in MADT\n");
        s_ioapic_ready = false;
        return 0;
    }

    if (apic_info.ioapic_count > MAX_IOAPICS) {
        s_ioapic_ready = false;
        return -E2BIG;
    }

    // 1. Map MMIO, validate topology and pin index representation across controllers
    for (uint32_t i = 0; i < apic_info.ioapic_count; i++) {
        ioapic_entry_t *ioapic = &apic_info.ioapics[i];

        // Map the I/O APIC's 2MB MMIO region
        uint64_t ioapic_page = ioapic->mmio_base & PAGE_2M_MASK;
        vmm_map_page(kernel_map, ioapic_page,
                     (uintptr_t)Phy_To_Virt(ioapic_page), PAGE_KERNEL_PMD_NOCACHE);

        // Read version to get maximum redirection entry — cache it
        uint32_t ver = ioapic_read_reg(i, IOAPIC_REG_VER);
        ioapic->max_redir = IOAPIC_VER_MAX_REDIR(ver);

        // Check endpoint addition overflow
        uint64_t end = (uint64_t)ioapic->gsi_base + ioapic->max_redir + 1ULL;
        if (end > UINT32_MAX + 1ULL)
            return -EOVERFLOW;

        // Check 8-bit redirection register index representation
        if (0x10 + 2 * (uint32_t)ioapic->max_redir + 1 > 0xFF)
            return -EINVAL;

        // Check range overlap with prior controllers
        for (uint32_t j = 0; j < i; j++) {
            uint64_t end_j = (uint64_t)apic_info.ioapics[j].gsi_base +
                             apic_info.ioapics[j].max_redir + 1ULL;
            if (ioapic->gsi_base < end_j && end > apic_info.ioapics[j].gsi_base)
                return -EINVAL;
        }

        debug_irq("IOAPIC[%u]: id=%u base=%#010x version=%#x max_redir=%u\n",
                  i, ioapic->apic_id, ioapic->mmio_base, ver, ioapic->max_redir);

        // Mask all redirection entries initially
        for (uint32_t n = 0; n <= ioapic->max_redir; n++) {
            ioapic_write_reg(i, IOAPIC_REG_REDTBL(n), IOAPIC_RED_MASK);
        }

        debug_irq("IOAPIC[%u]: %u redirection entries masked\n", i, ioapic->max_redir + 1);
    }

    // 2. Prepare descriptor registry and x86 routing table
    struct irq_gsi_range ranges[MAX_IOAPICS];
    size_t range_count = apic_info.ioapic_count;
    for (uint32_t i = 0; i < apic_info.ioapic_count; i++) {
        ranges[i].base = apic_info.ioapics[i].gsi_base;
        ranges[i].count = apic_info.ioapics[i].max_redir + 1;
    }
#if defined(CAPACITY_TEST) && CAPACITY_TEST
    if (range_count < MAX_IOAPICS) {
        ranges[range_count].base = 32;
        ranges[range_count].count = 24;
        range_count++;
    }
#endif

    int rc = irq_registry_init_ranges(ranges, range_count);
    if (rc < 0)
        return rc;

    rc = x86_irq_routes_init(ranges, range_count);
    if (rc < 0)
        return rc;

    // ── Switch from PIC to APIC mode via IMCR ──────────────
    arch_outb(0x22, 0x70);
    arch_outb(0x23, 0x01);
    debug_irq("IOAPIC: IMCR set to APIC mode\n");

    s_ioapic_ready = true;
    return 1;
}

bool ioapic_irq_unmasked(uint32_t gsi)
{
    if (!s_ioapic_ready)
        return false;
    const struct x86_irq_route *route = x86_irq_route_find_by_gsi(gsi);
    if (!route || route->ioapic_idx >= apic_info.ioapic_count)
        return false;
    ioapic_entry_t *ioapic = &apic_info.ioapics[route->ioapic_idx];
    if (route->pin > ioapic->max_redir)
        return false;
    uint32_t low = ioapic_read_reg(route->ioapic_idx, IOAPIC_REG_REDTBL(route->pin));
    return !(low & IOAPIC_RED_MASK);
}
