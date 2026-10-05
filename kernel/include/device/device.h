/* kernel/include/device/device.h — Generic device representation and lifecycle */
#ifndef _DEVICE_DEVICE_H
#define _DEVICE_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DEVICE_UNSAFE 1

enum device_state {
    DEV_DISCOVERED = 0,
    DEV_PROBING,
    DEV_BOUND,
    DEV_UNBOUND,
    DEV_FAILED,
};

struct device {
    uint64_t id;
    char name[64];
    struct device *parent;
    enum device_state state;
    int last_error;
    bool quarantined;
    void *retained_owner;
};

int device_core_init(void);
void device_quarantine(struct device *dev, void *retained_owner);
uint64_t device_alloc_id(void);

#endif /* _DEVICE_DEVICE_H */
