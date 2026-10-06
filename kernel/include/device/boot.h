/* kernel/include/device/boot.h — Phase 6 device coordinator and boot results */
#ifndef _DEVICE_BOOT_H
#define _DEVICE_BOOT_H

#include <device/device.h>

struct device_boot_summary {
    unsigned int total_devices;
    unsigned int bound_devices;
    unsigned int failed_devices;
    unsigned int unbound_devices;
    unsigned int skipped_absent_drivers;
};

int device_boot_init(void);
int device_boot_result(void);
void device_fail_unsafe(struct device *dev, const char *reason);
const struct device_boot_summary *device_boot_get_summary(void);

#ifdef OS01_HOST_TEST
void device_boot_reset_for_test(void);
#endif

#endif /* _DEVICE_BOOT_H */
