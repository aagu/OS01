/* kernel/device/boot.c — Phase 6 device coordinator and root check */
#include <device/boot.h>
#include <device/device.h>
#include <device/test_fault.h>
#include <block/blockdev.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <net/net.h>
#include <net/device.h>
#include <core/debug.h>
#include <core/panic.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

static bool s_device_boot_initialized = false;
static int s_device_boot_result = 0;
static struct device_boot_summary s_summary;

/* Weak reference to declared PCI drivers section */
extern const struct pci_driver *__pci_drivers_start[] __attribute__((weak));
extern const struct pci_driver *__pci_drivers_end[] __attribute__((weak));

#ifdef OS01_HOST_TEST
const struct pci_driver **g_test_pci_drivers_start = NULL;
const struct pci_driver **g_test_pci_drivers_end = NULL;
#define PCI_DRIVERS_START (g_test_pci_drivers_start ? g_test_pci_drivers_start : __pci_drivers_start)
#define PCI_DRIVERS_END   (g_test_pci_drivers_end ? g_test_pci_drivers_end : __pci_drivers_end)

void device_boot_reset_for_test(void)
{
    s_device_boot_initialized = false;
    s_device_boot_result = 0;
    memset(&s_summary, 0, sizeof(s_summary));
}
#else
#define PCI_DRIVERS_START __pci_drivers_start
#define PCI_DRIVERS_END   __pci_drivers_end
#endif

void device_fail_unsafe(struct device *dev, const char *reason)
{
    s_device_boot_result = DEVICE_UNSAFE;
    if (dev) {
        dev->state = DEV_FAILED;
        dev->last_error = DEVICE_UNSAFE;
    }
    kpanic("device: unsafe failure: %s\n", reason ? reason : "unspecified");
}

int device_boot_result(void)
{
    return s_device_boot_result;
}

const struct device_boot_summary *device_boot_get_summary(void)
{
    return &s_summary;
}

int device_boot_init(void)
{
    if (s_device_boot_initialized) {
        return s_device_boot_result;
    }

    memset(&s_summary, 0, sizeof(s_summary));

    /* 1. Core and block subsystems */
    int ret = device_core_init();
    if (ret != 0) {
        s_device_boot_result = ret;
        s_device_boot_initialized = true;
        return ret;
    }

    block_device_init();

    /* 2. Register all declared PCI drivers */
    if (PCI_DRIVERS_START && PCI_DRIVERS_END) {
        unsigned n = (unsigned)(PCI_DRIVERS_END - PCI_DRIVERS_START);
        arch9_fault_on_pci_enumerate_begin(n);
        for (const struct pci_driver **p = PCI_DRIVERS_START; p < PCI_DRIVERS_END; p++) {
            if (*p) {
                int r = pci_register_driver(*p);
                if (r != 0) {
                    s_device_boot_result = r;
                    s_device_boot_initialized = true;
                    return r;
                }
            }
        }
    }

    /* 3. ARCH-9 Task 9: bring up the unified net_device registry BEFORE
     * the PCI probe loop so that NIC probe() callbacks can register
     * their net_device instances into a ready registry.  This replaces
     * the legacy net_hw_init() PCI scan / single-instance initcall path
     * that the coordinator used to call after binding (see git history:
     * that path is gone — drivers self-register via .pci_drivers). */
    net_device_init();

    /* 4. Enumerate PCI devices */
    ret = pci_enumerate();
    if (ret != 0 && ret != BUS_UNAVAILABLE) {
        s_device_boot_result = ret;
        s_device_boot_initialized = true;
        return ret;
    }

    /* 5. Bind drivers to devices */
    if (ret == 0) {
        int bind_rc = pci_bind_all();
        if (bind_rc == DEVICE_UNSAFE) {
            device_fail_unsafe(NULL, "DEVICE_UNSAFE reported during PCI binding");
            s_device_boot_result = DEVICE_UNSAFE;
            s_device_boot_initialized = true;
            return DEVICE_UNSAFE;
        } else if (bind_rc != 0) {
            s_device_boot_result = bind_rc;
            s_device_boot_initialized = true;
            return bind_rc;
        }

        /* 6. Tally summary statistics */
        unsigned dev_count = pci_device_count();
        s_summary.total_devices = dev_count;
        for (unsigned i = 0; i < dev_count; i++) {
            struct pci_device *pdev = pci_device_get(i);
            if (!pdev) continue;
            if (pdev->dev.state == DEV_BOUND) {
                s_summary.bound_devices++;
            } else if (pdev->dev.state == DEV_FAILED) {
                s_summary.failed_devices++;
            } else if (pdev->dev.state == DEV_UNBOUND) {
                s_summary.unbound_devices++;
            }
        }

        /* Track skipped absent drivers (drivers with 0 candidate device matches) */
        if (PCI_DRIVERS_START && PCI_DRIVERS_END) {
            for (const struct pci_driver **p = PCI_DRIVERS_START; p < PCI_DRIVERS_END; p++) {
                const struct pci_driver *drv = *p;
                if (!drv) continue;

                unsigned candidates = 0;
                for (unsigned i = 0; i < dev_count; i++) {
                    struct pci_device *pdev = pci_device_get(i);
                    if (pdev && pci_match_id(drv, pdev) != NULL) {
                        candidates++;
                    }
                }
                if (candidates == 0) {
                    s_summary.skipped_absent_drivers++;
                    debug_block("device-boot: driver '%s' SKIPPED_ABSENT\n", drv->name);
                }
            }
        }
    }

    s_device_boot_result = 0;
    s_device_boot_initialized = true;
    return 0;
}

#ifndef OS01_HOST_TEST
#include <subsys/subsys.h>

static int _device_boot_subsys_init(void)
{
    return device_boot_init();
}

static int _device_boot_register(void)
{
    register_subsys("device-boot", _device_boot_subsys_init,
                    SUBSYS_PHASE_6, 0 /* non-OPTIONAL */);
    return 0;
}
SUBSYS_INITCALL(_device_boot_register);
#endif
