/* kernel/include/driver/ahci_lifecycle.h — AHCI lifecycle, deadline and timeout management */
#ifndef _DRIVER_AHCI_LIFECYCLE_H
#define _DRIVER_AHCI_LIFECYCLE_H

#include <stdint.h>
#include <stdbool.h>

struct ahci_port;

/* Fixed budgets (ms) */
#define AHCI_BUDGET_GATE_MS     500
#define AHCI_BUDGET_CMD_MS      500
#define AHCI_BUDGET_STOP_MS     500
#define AHCI_BUDGET_HANDOFF_MS  500

struct ahci_deadline {
    bool boot_phase;
    uint64_t expiry;
};

int  ahci_deadline_start(bool boot_phase, uint32_t timeout_ms, struct ahci_deadline *out);
bool ahci_deadline_expired(const struct ahci_deadline *dl);
int  ahci_port_claim(struct ahci_port *port, const struct ahci_deadline *dl);
void ahci_port_release(struct ahci_port *port);
int  ahci_port_fail(struct ahci_port *port, int cause);
int  ahci_port_fail_escalate(struct ahci_port *port, int cause);

#endif /* _DRIVER_AHCI_LIFECYCLE_H */
