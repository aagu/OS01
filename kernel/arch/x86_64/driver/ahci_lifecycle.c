/* kernel/driver/ahci_lifecycle.c — AHCI deadline, timeout, and safe revocation */
#include <driver/ahci_lifecycle.h>
#include <driver/ahci.h>
#include <bus/pci/pci.h>
#include <device/device.h>
#include <time/clocksource.h>
#ifndef OS01_HOST_TEST
#include <arch/x86_64/clocksource.h>
#endif
#include <arch/cpu.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

int ahci_deadline_start(bool boot_phase, uint32_t timeout_ms, struct ahci_deadline *out)
{
    if (!out) {
        return -EINVAL;
    }
    if (!clocksource_active || clocksource_freq_hz() == 0) {
        return -ENOTSUP;
    }

    out->boot_phase = boot_phase;
    if (boot_phase) {
        uint64_t freq = clocksource_freq_hz();
        uint64_t delta_cycles = (uint64_t)(((__uint128_t)timeout_ms * freq + 999) / 1000);
        out->expiry = clocksource_cycles() + delta_cycles;
    } else {
        uint64_t now_ns = clocksource_read_ns();
        out->expiry = now_ns + (uint64_t)timeout_ms * 1000000ULL;
    }
    return 0;
}

bool ahci_deadline_expired(const struct ahci_deadline *dl)
{
    if (!dl) {
        return true;
    }
    if (!clocksource_active || clocksource_freq_hz() == 0) {
        return true;
    }

    if (dl->boot_phase) {
        uint64_t now = clocksource_cycles();
        return (int64_t)(now - dl->expiry) >= 0;
    } else {
        uint64_t now_ns = clocksource_read_ns();
        return (int64_t)(now_ns - dl->expiry) >= 0;
    }
}

int ahci_port_claim(struct ahci_port *port, const struct ahci_deadline *dl)
{
    if (!port) {
        return -EINVAL;
    }
    if (port->state == AHCI_PORT_STATE_FAILED || port->quarantined) {
        return -EIO;
    }

    while (1) {
        uint64_t flags = spin_lock_irqsave(&port->lock);
        if (port->state == AHCI_PORT_STATE_FAILED || port->quarantined) {
            spin_unlock_irqrestore(&port->lock, flags);
            return -EIO;
        }
        if (!port->busy) {
            port->busy = true;
            spin_unlock_irqrestore(&port->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&port->lock, flags);

        if (dl && ahci_deadline_expired(dl)) {
            return -EBUSY;
        }
        arch_nop();
    }
}

void ahci_port_release(struct ahci_port *port)
{
    if (!port) return;
    uint64_t flags = spin_lock_irqsave(&port->lock);
    port->busy = false;
    spin_unlock_irqrestore(&port->lock, flags);
}

int ahci_port_fail(struct ahci_port *port, int cause)
{
    if (!port) return cause;
    uint64_t flags = spin_lock_irqsave(&port->lock);
    port->state = AHCI_PORT_STATE_FAILED;
    port->last_error = cause;
    port->busy = false;
    if (port->bdev) {
        block_device_mark_failed(port->bdev, cause);
    }
    spin_unlock_irqrestore(&port->lock, flags);
    return cause;
}

int ahci_port_fail_escalate(struct ahci_port *port, int cause)
{
    if (!port) return cause;
    ahci_port_fail(port, cause);

    bool irq_disabled = false;
    if (port->ctrl && port->ctrl->pdev) {
        if (pci_interrupts_disable(port->ctrl->pdev) == 0) {
            irq_disabled = true;
        }
    }

    bool engine_stopped = false;
    if (port->ctrl && port->ctrl->hba) {
        HBA_PORT *regs = (HBA_PORT *)((uint8_t *)port->ctrl->hba + 0x100 + port->port_num * 0x80);
        if (!(regs->cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR))) {
            engine_stopped = true;
        }
    }

    if (!irq_disabled && !engine_stopped) {
        if (port->ctrl && port->ctrl->pdev) {
            port->ctrl->pdev->dev.state = DEV_FAILED;
            port->ctrl->pdev->dev.last_error = DEVICE_UNSAFE;
            device_quarantine(&port->ctrl->pdev->dev, port);
        }
        return DEVICE_UNSAFE;
    }
    return cause;
}
