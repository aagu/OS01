/* kernel/bus/pci/core.c — Architecture-agnostic PCI core and enumeration */
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <arch/pci.h>
#include <memory/slab.h>
#include <stdio.h>
#include <string.h>

static struct pci_device *s_pci_devices_head = NULL;
static unsigned int s_pci_device_count = 0;

struct pci_driver_entry {
    const struct pci_driver *driver;
    struct pci_driver_entry *next;
};
static struct pci_driver_entry *s_pci_drivers_head = NULL;
static bool s_pci_enumerated = false;

#define MAX_DOMAINS 16
struct visited_bus_map {
    uint16_t domain;
    uint8_t visited[32]; /* 256 bits */
};
static struct visited_bus_map s_visited[MAX_DOMAINS];
static unsigned int s_visited_domain_count = 0;

static bool bus_is_visited(uint16_t domain, uint8_t bus)
{
    for (unsigned int i = 0; i < s_visited_domain_count; i++) {
        if (s_visited[i].domain == domain) {
            return (s_visited[i].visited[bus / 8] & (1U << (bus % 8))) != 0;
        }
    }
    return false;
}

static void bus_mark_visited(uint16_t domain, uint8_t bus)
{
    for (unsigned int i = 0; i < s_visited_domain_count; i++) {
        if (s_visited[i].domain == domain) {
            s_visited[i].visited[bus / 8] |= (1U << (bus % 8));
            return;
        }
    }
    if (s_visited_domain_count < MAX_DOMAINS) {
        s_visited[s_visited_domain_count].domain = domain;
        memset(s_visited[s_visited_domain_count].visited, 0, 32);
        s_visited[s_visited_domain_count].visited[bus / 8] |= (1U << (bus % 8));
        s_visited_domain_count++;
    }
}

static int insert_device_sorted(struct pci_device *dev)
{
    struct pci_device **curr = &s_pci_devices_head;
    while (*curr) {
        struct pci_device *p = *curr;
        if (dev->domain < p->domain) {
            break;
        } else if (dev->domain == p->domain) {
            if (dev->bus < p->bus) {
                break;
            } else if (dev->bus == p->bus) {
                if (dev->slot < p->slot) {
                    break;
                } else if (dev->slot == p->slot) {
                    if (dev->fn < p->fn) {
                        break;
                    } else if (dev->fn == p->fn) {
                        /* Deduplication: BDF already exists */
                        return -EEXIST;
                    }
                }
            }
        }
        curr = &(*curr)->next;
    }
    dev->next = *curr;
    *curr = dev;
    s_pci_device_count++;
    return 0;
}

static int scan_bus(const struct pci_backend *backend, uint16_t domain, uint8_t bus);

static int pci_scan_function(const struct pci_backend *backend, uint16_t domain, uint8_t bus,
                             uint8_t slot, uint8_t fn, bool *out_present)
{
    if (out_present) {
        *out_present = false;
    }

    enum pci_error_scope scope = PCI_ERROR_FUNCTION;
    uint32_t id_reg = 0;
    int rc = backend->read32(domain, bus, slot, fn, 0x00, &id_reg, &scope);
    if (rc != 0) {
        if (scope == PCI_ERROR_ROOT) {
            return rc;
        }
        /* Single function backend error */
        struct pci_device *pdev = (struct pci_device *)kmalloc(sizeof(struct pci_device));
        if (!pdev) {
            return -ENOMEM;
        }
        memset(pdev, 0, sizeof(*pdev));
        pdev->dev.id = device_alloc_id();
        snprintf(pdev->dev.name, sizeof(pdev->dev.name), "pci:%04x:%02x:%02x.%u", domain, bus, slot, fn);
        pdev->domain = domain;
        pdev->bus = bus;
        pdev->slot = slot;
        pdev->fn = fn;
        pdev->dev.state = DEV_FAILED;
        pdev->dev.last_error = rc;
        pdev->enum_error = true;
        if (insert_device_sorted(pdev) != 0) {
            kfree(pdev);
        }
        if (out_present) {
            *out_present = true;
        }
        return 0;
    }

    if (id_reg == 0xFFFFFFFFU || (id_reg & 0xFFFF) == 0xFFFF) {
        /* Device absent */
        return 0;
    }

    if (out_present) {
        *out_present = true;
    }

    uint16_t vendor = id_reg & 0xFFFF;
    uint16_t device = (id_reg >> 16) & 0xFFFF;

    /* Read class code and revision at 0x08 */
    uint32_t class_reg = 0;
    rc = backend->read32(domain, bus, slot, fn, 0x08, &class_reg, &scope);
    if (rc != 0) {
        if (scope == PCI_ERROR_ROOT) {
            return rc;
        }
        struct pci_device *pdev = (struct pci_device *)kmalloc(sizeof(struct pci_device));
        if (!pdev) {
            return -ENOMEM;
        }
        memset(pdev, 0, sizeof(*pdev));
        pdev->dev.id = device_alloc_id();
        snprintf(pdev->dev.name, sizeof(pdev->dev.name), "pci:%04x:%02x:%02x.%u", domain, bus, slot, fn);
        pdev->domain = domain;
        pdev->bus = bus;
        pdev->slot = slot;
        pdev->fn = fn;
        pdev->vendor = vendor;
        pdev->device = device;
        pdev->dev.state = DEV_FAILED;
        pdev->dev.last_error = rc;
        pdev->enum_error = true;
        if (insert_device_sorted(pdev) != 0) {
            kfree(pdev);
        }
        return 0;
    }
    uint32_t class_code = (class_reg >> 8) & 0xFFFFFF;

    /* Read header type at 0x0C */
    uint32_t hdr_reg = 0;
    rc = backend->read32(domain, bus, slot, fn, 0x0C, &hdr_reg, &scope);
    if (rc != 0) {
        if (scope == PCI_ERROR_ROOT) {
            return rc;
        }
        struct pci_device *pdev = (struct pci_device *)kmalloc(sizeof(struct pci_device));
        if (!pdev) {
            return -ENOMEM;
        }
        memset(pdev, 0, sizeof(*pdev));
        pdev->dev.id = device_alloc_id();
        snprintf(pdev->dev.name, sizeof(pdev->dev.name), "pci:%04x:%02x:%02x.%u", domain, bus, slot, fn);
        pdev->domain = domain;
        pdev->bus = bus;
        pdev->slot = slot;
        pdev->fn = fn;
        pdev->vendor = vendor;
        pdev->device = device;
        pdev->class_code = class_code;
        pdev->dev.state = DEV_FAILED;
        pdev->dev.last_error = rc;
        pdev->enum_error = true;
        if (insert_device_sorted(pdev) != 0) {
            kfree(pdev);
        }
        return 0;
    }
    uint8_t header_type = (hdr_reg >> 16) & 0xFF;

    struct pci_device *pdev = (struct pci_device *)kmalloc(sizeof(struct pci_device));
    if (!pdev) {
        return -ENOMEM;
    }
    memset(pdev, 0, sizeof(*pdev));
    pdev->dev.id = device_alloc_id();
    snprintf(pdev->dev.name, sizeof(pdev->dev.name), "pci:%04x:%02x:%02x.%u", domain, bus, slot, fn);
    pdev->domain = domain;
    pdev->bus = bus;
    pdev->slot = slot;
    pdev->fn = fn;
    pdev->vendor = vendor;
    pdev->device = device;
    pdev->class_code = class_code;
    pdev->dev.state = DEV_DISCOVERED;

    uint8_t hdr_layout = header_type & 0x7F;
    if (hdr_layout == 0) {
        /* Standard Type 0 header */
        uint32_t sub_reg = 0;
        if (backend->read32(domain, bus, slot, fn, 0x2C, &sub_reg, &scope) == 0) {
            pdev->subvendor = sub_reg & 0xFFFF;
            pdev->subdevice = (sub_reg >> 16) & 0xFFFF;
        }

        bool bad_bar = false;
        for (uint8_t bar_idx = 0; bar_idx < 6; bar_idx++) {
            uint32_t bar_val = 0;
            if (backend->read32(domain, bus, slot, fn, 0x10 + bar_idx * 4, &bar_val, &scope) != 0) {
                bad_bar = true;
                break;
            }
            if (bar_val == 0) {
                pdev->bars[bar_idx].kind = PCI_BAR_NONE;
                pdev->bars[bar_idx].valid = false;
                pdev->bars[bar_idx].address = 0;
                pdev->bars[bar_idx].index = bar_idx;
                continue;
            }
            if (bar_val & 0x01) {
                /* I/O BAR */
                pdev->bars[bar_idx].kind = PCI_BAR_IO;
                pdev->bars[bar_idx].address = bar_val & ~0x03ULL;
                pdev->bars[bar_idx].valid = true;
                pdev->bars[bar_idx].index = bar_idx;
            } else {
                /* Memory BAR */
                uint8_t mem_type = (bar_val >> 1) & 0x03;
                if (mem_type == 0x00) {
                    /* 32-bit MMIO */
                    pdev->bars[bar_idx].kind = PCI_BAR_MMIO32;
                    pdev->bars[bar_idx].address = bar_val & ~0x0FULL;
                    pdev->bars[bar_idx].valid = true;
                    pdev->bars[bar_idx].index = bar_idx;
                } else if (mem_type == 0x02) {
                    /* 64-bit MMIO */
                    if (bar_idx >= 5) {
                        /* 64-bit BAR at BAR5 is invalid: no upper slot */
                        bad_bar = true;
                        break;
                    }
                    uint32_t upper_val = 0;
                    if (backend->read32(domain, bus, slot, fn, 0x10 + (bar_idx + 1) * 4, &upper_val, &scope) != 0) {
                        bad_bar = true;
                        break;
                    }
                    uint64_t full_addr = ((uint64_t)upper_val << 32) | (bar_val & ~0x0FULL);
                    pdev->bars[bar_idx].kind = PCI_BAR_MMIO64;
                    pdev->bars[bar_idx].address = full_addr;
                    pdev->bars[bar_idx].valid = true;
                    pdev->bars[bar_idx].index = bar_idx;

                    /* Upper slot is not independently usable */
                    pdev->bars[bar_idx + 1].kind = PCI_BAR_UPPER;
                    pdev->bars[bar_idx + 1].address = full_addr;
                    pdev->bars[bar_idx + 1].valid = false;
                    pdev->bars[bar_idx + 1].index = bar_idx + 1;

                    bar_idx++;
                } else {
                    /* Reserved type (0x01 or 0x03) */
                    bad_bar = true;
                    break;
                }
            }
        }

        if (bad_bar) {
            pdev->dev.state = DEV_FAILED;
            pdev->dev.last_error = -EINVAL;
            pdev->enum_error = true;
        }
    } else if (hdr_layout == 1) {
        /* PCI-to-PCI Bridge */
        uint32_t bus_reg = 0;
        if (backend->read32(domain, bus, slot, fn, 0x18, &bus_reg, &scope) != 0) {
            pdev->enum_error = true;
            pdev->dev.state = DEV_FAILED;
            pdev->dev.last_error = -EIO;
        } else {
            uint8_t secondary = (bus_reg >> 8) & 0xFF;
            uint8_t subordinate = (bus_reg >> 16) & 0xFF;
            if (secondary == 0 || secondary == bus || secondary > subordinate) {
                /* Invalid bridge, isolate branch */
                pdev->enum_error = true;
            } else {
                int ins_rc = insert_device_sorted(pdev);
                if (ins_rc != 0) {
                    kfree(pdev);
                    return ins_rc;
                }
                for (uint16_t b = secondary; b <= subordinate; b++) {
                    if (!bus_is_visited(domain, (uint8_t)b)) {
                        bus_mark_visited(domain, (uint8_t)b);
                        int b_rc = scan_bus(backend, domain, (uint8_t)b);
                        if (b_rc < 0) {
                            return b_rc;
                        }
                    }
                }
                return 0;
            }
        }
    }

    int ins_rc = insert_device_sorted(pdev);
    if (ins_rc != 0) {
        kfree(pdev);
        return ins_rc;
    }
    return 0;
}

static int scan_bus(const struct pci_backend *backend, uint16_t domain, uint8_t bus)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        bool fn0_present = false;
        int rc = pci_scan_function(backend, domain, bus, slot, 0, &fn0_present);
        if (rc < 0) {
            return rc;
        }
        /* When function 0 is present, scan functions 1..7 (supporting Q35 and multifunction devices) */
        if (fn0_present) {
            for (uint8_t fn = 1; fn < 8; fn++) {
                bool dummy_present = false;
                rc = pci_scan_function(backend, domain, bus, slot, fn, &dummy_present);
                if (rc < 0) {
                    return rc;
                }
            }
        }
    }
    return 0;
}

int pci_enumerate(void)
{
    if (s_pci_enumerated) {
        return 0;
    }

    const struct pci_backend *backend = arch_pci_backend();
    if (!backend) {
        return BUS_UNAVAILABLE;
    }

    if (!backend->roots || backend->root_count == 0) {
        s_pci_enumerated = true;
        return 0;
    }

    s_visited_domain_count = 0;

    for (unsigned int i = 0; i < backend->root_count; i++) {
        uint16_t domain = backend->roots[i].domain;
        uint8_t bus = backend->roots[i].bus;

        if (!bus_is_visited(domain, bus)) {
            bus_mark_visited(domain, bus);
            int rc = scan_bus(backend, domain, bus);
            if (rc < 0) {
                return rc;
            }
        }
    }

    s_pci_enumerated = true;
    return 0;
}

int pci_register_driver(const struct pci_driver *driver)
{
    if (!driver || !driver->name) {
        return -EINVAL;
    }

    for (struct pci_driver_entry *e = s_pci_drivers_head; e; e = e->next) {
        if (strcmp(e->driver->name, driver->name) == 0) {
            return -EEXIST;
        }
    }

    struct pci_driver_entry *entry = (struct pci_driver_entry *)kmalloc(sizeof(struct pci_driver_entry));
    if (!entry) {
        return -ENOMEM;
    }
    entry->driver = driver;
    entry->next = NULL;

    if (!s_pci_drivers_head) {
        s_pci_drivers_head = entry;
    } else {
        struct pci_driver_entry *tail = s_pci_drivers_head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = entry;
    }

    return 0;
}

int pci_bind_all(void)
{
    if (!s_pci_enumerated) {
        return -EIO;
    }

    int fatal_result = 0;

    for (struct pci_device *pdev = s_pci_devices_head; pdev; pdev = pdev->next) {
        if (pdev->dev.state == DEV_BOUND || pdev->dev.state == DEV_FAILED || pdev->enum_error) {
            continue;
        }

        const struct pci_driver *best_driver = NULL;
        const struct pci_device_id *best_id = NULL;
        int best_rank = 0; /* 0 = none, 1 = exact, 2 = class */
        bool conflict = false;

        for (const struct pci_driver_entry *e = s_pci_drivers_head; e; e = e->next) {
            const struct pci_driver *drv = e->driver;
            const struct pci_device_id *id = pci_match_id(drv, pdev);
            if (!id) {
                continue;
            }

            int rank = (id->vendor != PCI_ID_ANY && id->device != PCI_ID_ANY) ? 1 : 2;
            if (best_rank == 0 || rank < best_rank) {
                best_driver = drv;
                best_id = id;
                best_rank = rank;
                conflict = false;
            } else if (rank == best_rank) {
                conflict = true;
            }
        }

        if (best_rank == 0) {
            pdev->dev.state = DEV_UNBOUND;
            pdev->driver = NULL;
            continue;
        }

        if (conflict) {
            pdev->dev.state = DEV_FAILED;
            pdev->dev.last_error = -EEXIST;
            pdev->driver = NULL;
            continue;
        }

        pdev->dev.state = DEV_PROBING;
        int rc = best_driver->probe ? best_driver->probe(pdev, best_id) : -ENODEV;
        if (rc == 0) {
            pdev->dev.state = DEV_BOUND;
            pdev->driver = best_driver;
            pdev->dev.last_error = 0;
        } else if (rc == -ENODEV) {
            pdev->dev.state = DEV_UNBOUND;
            pdev->driver = NULL;
            pdev->dev.last_error = -ENODEV;
        } else if (rc == DEVICE_UNSAFE) {
            pdev->dev.state = DEV_FAILED;
            pdev->driver = NULL;
            pdev->dev.last_error = DEVICE_UNSAFE;
            fatal_result = DEVICE_UNSAFE;
        } else {
            pdev->dev.state = DEV_FAILED;
            pdev->driver = NULL;
            pdev->dev.last_error = rc;
        }
    }

    return fatal_result;
}

unsigned int pci_device_count(void)
{
    return s_pci_device_count;
}

struct pci_device *pci_device_get(unsigned int index)
{
    struct pci_device *p = s_pci_devices_head;
    while (p && index > 0) {
        p = p->next;
        index--;
    }
    return p;
}

struct pci_device *pci_device_lookup(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn)
{
    for (struct pci_device *p = s_pci_devices_head; p; p = p->next) {
        if (p->domain == domain && p->bus == bus && p->slot == slot && p->fn == fn) {
            return p;
        }
    }
    return NULL;
}

int pci_bar_window(const struct pci_device *pdev, unsigned int index, enum pci_bar_kind required,
                   uint64_t offset, uint64_t length, uint64_t *physical)
{
    if (!pdev || !physical || length == 0) {
        return -EINVAL;
    }
    if (index >= 6) {
        return -EINVAL;
    }
    if (required == PCI_BAR_NONE || required == PCI_BAR_UPPER) {
        return -EINVAL;
    }

    const struct pci_bar *bar = &pdev->bars[index];
    if (!bar->valid || bar->kind == PCI_BAR_NONE || bar->kind == PCI_BAR_UPPER) {
        return -EINVAL;
    }

    /* I/O and MMIO cannot be mixed */
    if (required == PCI_BAR_IO) {
        if (bar->kind != PCI_BAR_IO) {
            return -EINVAL;
        }
    } else if (required == PCI_BAR_MMIO32) {
        if (bar->kind != PCI_BAR_MMIO32 && bar->kind != PCI_BAR_MMIO64) {
            return -EINVAL;
        }
    } else if (required == PCI_BAR_MMIO64) {
        if (bar->kind != PCI_BAR_MMIO64 && bar->kind != PCI_BAR_MMIO32) {
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    /* Check integer overflow for offset + length */
    if (offset + length < offset) {
        return -EINVAL;
    }

    /* Check integer overflow for bar->address + offset + length */
    if (bar->address + offset + length < bar->address) {
        return -EINVAL;
    }

    if (required == PCI_BAR_MMIO32) {
        if (bar->address + offset + length > 0x100000000ULL) {
            return -EOVERFLOW;
        }
    }

    *physical = bar->address + offset;
    return 0;
}

int pci_config_read32(struct pci_device *pdev, uint16_t offset, uint32_t *out)
{
    if (!pdev || !out || (offset & 3) != 0 || offset > 252) {
        return -EINVAL;
    }
    const struct pci_backend *backend = arch_pci_backend();
    if (!backend || !backend->read32) {
        return BUS_UNAVAILABLE;
    }
    enum pci_error_scope scope = PCI_ERROR_FUNCTION;
    return backend->read32(pdev->domain, pdev->bus, pdev->slot, pdev->fn, offset, out, &scope);
}

int pci_config_write32(struct pci_device *pdev, uint16_t offset, uint32_t value)
{
    if (!pdev || (offset & 3) != 0 || offset > 252) {
        return -EINVAL;
    }
    const struct pci_backend *backend = arch_pci_backend();
    if (!backend || !backend->write32) {
        return BUS_UNAVAILABLE;
    }
    enum pci_error_scope scope = PCI_ERROR_FUNCTION;
    return backend->write32(pdev->domain, pdev->bus, pdev->slot, pdev->fn, offset, value, &scope);
}

int pci_set_bus_master(struct pci_device *pdev, bool enabled)
{
    if (!pdev) {
        return -EINVAL;
    }
    uint32_t reg = 0;
    int rc = pci_config_read32(pdev, 0x04, &reg);
    if (rc != 0) {
        return rc;
    }
    if (enabled) {
        reg |= (1U << 2);
    } else {
        reg &= ~(1U << 2);
    }
    return pci_config_write32(pdev, 0x04, reg);
}

int pci_set_decode(struct pci_device *pdev, bool io, bool mmio)
{
    if (!pdev) {
        return -EINVAL;
    }
    uint32_t reg = 0;
    int rc = pci_config_read32(pdev, 0x04, &reg);
    if (rc != 0) {
        return rc;
    }
    if (io) {
        reg |= (1U << 0);
    } else {
        reg &= ~(1U << 0);
    }
    if (mmio) {
        reg |= (1U << 1);
    } else {
        reg &= ~(1U << 1);
    }
    return pci_config_write32(pdev, 0x04, reg);
}

int pci_set_intx(struct pci_device *pdev, bool enabled)
{
    if (!pdev) {
        return -EINVAL;
    }
    uint32_t reg = 0;
    int rc = pci_config_read32(pdev, 0x04, &reg);
    if (rc != 0) {
        return rc;
    }
    if (enabled) {
        reg &= ~(1U << 10);
    } else {
        reg |= (1U << 10);
    }
    return pci_config_write32(pdev, 0x04, reg);
}

int pci_route_gsi(struct pci_device *pdev, uint32_t *out)
{
    if (!pdev || !out) {
        return -EINVAL;
    }
    const struct pci_backend *backend = arch_pci_backend();
    if (!backend || !backend->route_gsi) {
        return BUS_UNAVAILABLE;
    }
    return backend->route_gsi(pdev, out);
}

int pci_msix_enable(struct pci_device *pdev, uint8_t vector)
{
    if (!pdev) {
        return -EINVAL;
    }

    uint32_t cmd_status = 0;
    int rc = pci_config_read32(pdev, 0x04, &cmd_status);
    if (rc != 0) {
        return rc;
    }
    if (!(cmd_status & (1U << 20))) {
        return -ENOTSUP;
    }

    uint32_t cap_ptr_reg = 0;
    rc = pci_config_read32(pdev, 0x34, &cap_ptr_reg);
    if (rc != 0) {
        return rc;
    }
    uint8_t cap_ptr = (uint8_t)(cap_ptr_reg & 0xFF);

    bool visited[256] = {0};
    unsigned int steps = 0;
    uint8_t msix_cap_ptr = 0;
    uint32_t msix_dword = 0;

    while (cap_ptr != 0) {
        if (cap_ptr < 0x40 || (cap_ptr & 3) != 0 || cap_ptr > 0xFC) {
            return -EINVAL;
        }
        if (visited[cap_ptr]) {
            return -ELOOP;
        }
        visited[cap_ptr] = true;
        if (++steps > 48) {
            return -ELOOP;
        }

        uint32_t dword = 0;
        rc = pci_config_read32(pdev, cap_ptr, &dword);
        if (rc != 0) {
            return rc;
        }

        uint8_t cap_id = (uint8_t)(dword & 0xFF);
        if (cap_id == 0x11) {
            msix_cap_ptr = cap_ptr;
            msix_dword = dword;
            break;
        }
        cap_ptr = (uint8_t)((dword >> 8) & 0xFF);
    }

    if (msix_cap_ptr == 0) {
        return -ENOTSUP;
    }

    uint32_t table_reg = 0;
    rc = pci_config_read32(pdev, msix_cap_ptr + 4, &table_reg);
    if (rc != 0) {
        return rc;
    }

    uint8_t bir = (uint8_t)(table_reg & 0x07);
    uint32_t tbl_off = table_reg & ~0x07U;

    if (bir >= 6) {
        return -EINVAL;
    }

    uint64_t table_phys = 0;
    rc = pci_bar_window(pdev, bir, pdev->bars[bir].kind, tbl_off, 16, &table_phys);
    if (rc != 0) {
        return rc;
    }
    if (pdev->bars[bir].kind != PCI_BAR_MMIO32 && pdev->bars[bir].kind != PCI_BAR_MMIO64) {
        return -EINVAL;
    }

    void *table_virt = NULL;
    rc = arch_pci_msix_map(pdev, table_phys, &table_virt);
    if (rc != 0 || !table_virt) {
        return (rc != 0) ? rc : -ENOMEM;
    }

    volatile uint32_t *entry = (volatile uint32_t *)table_virt;
    uint32_t msi_addr = arch_pci_msi_address(pdev);
    entry[0] = msi_addr;
    entry[1] = 0;
    entry[2] = (uint32_t)vector;
    entry[3] = 0;

    arch_pci_msix_unmap(pdev, table_virt);

    msix_dword |= 0x80000000U;
    return pci_config_write32(pdev, msix_cap_ptr, msix_dword);
}

int pci_interrupts_disable(struct pci_device *pdev)
{
    if (!pdev) {
        return -EINVAL;
    }

    uint32_t cmd_status = 0;
    int rc = pci_config_read32(pdev, 0x04, &cmd_status);
    if (rc != 0) {
        return rc;
    }

    if (cmd_status & (1U << 20)) {
        uint32_t cap_ptr_reg = 0;
        rc = pci_config_read32(pdev, 0x34, &cap_ptr_reg);
        if (rc != 0) {
            return rc;
        }
        uint8_t cap_ptr = (uint8_t)(cap_ptr_reg & 0xFF);

        bool visited[256] = {0};
        unsigned int steps = 0;

        while (cap_ptr != 0) {
            if (cap_ptr < 0x40 || (cap_ptr & 3) != 0 || cap_ptr > 0xFC) {
                return -EINVAL;
            }
            if (visited[cap_ptr]) {
                return -ELOOP;
            }
            visited[cap_ptr] = true;
            if (++steps > 48) {
                return -ELOOP;
            }

            uint32_t dword = 0;
            rc = pci_config_read32(pdev, cap_ptr, &dword);
            if (rc != 0) {
                return rc;
            }

            uint8_t cap_id = (uint8_t)(dword & 0xFF);
            if (cap_id == 0x05) {
                if (dword & (1U << 16)) {
                    dword &= ~(1U << 16);
                    rc = pci_config_write32(pdev, cap_ptr, dword);
                    if (rc != 0) {
                        return rc;
                    }
                }
            } else if (cap_id == 0x11) {
                if (dword & (1U << 31)) {
                    dword &= ~(1U << 31);
                    rc = pci_config_write32(pdev, cap_ptr, dword);
                    if (rc != 0) {
                        return rc;
                    }
                }
            }
            cap_ptr = (uint8_t)((dword >> 8) & 0xFF);
        }
    }

    return pci_set_intx(pdev, false);
}

#ifdef OS01_HOST_TEST
void pci_core_reset_for_test(void)
{
    struct pci_device *p = s_pci_devices_head;
    while (p) {
        struct pci_device *next = p->next;
        kfree(p);
        p = next;
    }
    s_pci_devices_head = NULL;
    s_pci_device_count = 0;

    struct pci_driver_entry *e = s_pci_drivers_head;
    while (e) {
        struct pci_driver_entry *next = e->next;
        kfree(e);
        e = next;
    }
    s_pci_drivers_head = NULL;
    s_pci_enumerated = false;
    s_visited_domain_count = 0;
}
#endif
