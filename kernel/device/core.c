/* kernel/device/core.c — Device subsystem core initialization and management */
#include <device/device.h>
#include <errno.h>
#include <string.h>

static uint64_t s_next_device_id = 1;
static bool s_device_core_initialized = false;

int device_core_init(void)
{
    if (s_device_core_initialized) {
        return 0;
    }
    s_device_core_initialized = true;
    s_next_device_id = 1;
    return 0;
}

void device_quarantine(struct device *dev, void *retained_owner)
{
    if (!dev) {
        return;
    }
    dev->quarantined = true;
    dev->retained_owner = retained_owner;
}

uint64_t device_alloc_id(void)
{
    return s_next_device_id++;
}
