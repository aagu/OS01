#include <arch/syscall.h>
#include <arch/x86_64/hw.h>
#include <core/printk.h>
#include <driver/serial.h>
#include <intr/apic.h>
#include <sched/task.h>
#include <uapi/syscall.h>
#include <log/log.h>

int64_t arch_syscall_putchar(uint64_t ch)
{
    // putchar(int c) — write one char to framebuffer AND serial
    char c = (char)ch;
    color_printk(WHITE, BLACK, "%c", c);
    {
        // Hold serial_lock across the putchar so the byte is
        // emitted atomically with respect to other writers.
        // Use write_serial_unlocked to avoid re-locking deadlock
        // (write_serial() now acquires serial_lock internally —
        // see kernel/driver/serial.c).
        uint64_t sf = spin_lock_irqsave(&serial_lock);
        write_serial_unlocked(c);  // also echo to serial for interactive shell
        spin_unlock_irqrestore(&serial_lock, sf);
    }
    return (unsigned char)c;
}

void arch_syscall_reboot(int cmd)
{
    log_info("syscall: reboot(cmd=%d) from pid=%d\n",
             cmd, (int)current->pid);

    // ── ACPI power-off ─────────────────────────────────
    if (cmd == RB_POWER_OFF && apic_info.pm1a_port) {
        // SLP_EN (bit 13) toggles sleep.  SLP_TYPa=0 for
        // S5 on QEMU q35 (the \_S5 object reports {0, 0}).
        log_info("ACPI: powering off via PM1a=%#x\n",
                 (unsigned)apic_info.pm1a_port);
        outw(apic_info.pm1a_port, 0x2000);
        // Block — platform powers off asynchronously.
        while (1) __asm__ __volatile__("hlt");
    }

    // ── ACPI reboot / halt fallback ────────────────────
    // Keyboard controller pulse-reset ($0xFE → port $0x64)
    while ((inb(0x64) & 0x02) != 0) { /* wait */ }
    outb(0xFE, 0x64);
    while (1) __asm__ __volatile__("hlt");
}
