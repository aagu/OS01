// poweroff — ACPI power-off the system
//
// Calls reboot(RB_POWER_OFF).  Falls back to reboot if
// ACPI is not available or fails.
#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>

int main(void)
{
    printf("poweroff: shutting down...\n");
    reboot(RB_POWER_OFF);
    // If we reach here, ACPI poweroff failed — reboot as fallback
    printf("poweroff: poweroff not available, rebooting...\n");
    reboot(RB_AUTOBOOT);
    return 0;
}
