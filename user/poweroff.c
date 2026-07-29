// poweroff — signal init (PID 1) to perform graceful shutdown
//
// Sends SIGUSR2 to init, which triggers do_shutdown(RB_POWER_OFF).
// Falls back to direct reboot(RB_POWER_OFF) if init is unreachable.
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/syscall.h>

int main(void)
{
    printf("poweroff: signalling init (SIGUSR2)...\n");

    if (kill(1, SIGUSR2) == 0) {
        // Init got the signal — it will handle the shutdown.
        // Give it some time; if it fails, we'll be killed with everything else.
        return 0;
    }

    // Init unreachable — fall back to direct syscall
    printf("poweroff: init unreachable, forcing power-off...\n");
    reboot(RB_POWER_OFF);
    // If we reach here, ACPI poweroff failed — reboot as fallback
    printf("poweroff: poweroff not available, rebooting...\n");
    reboot(RB_AUTOBOOT);
    return 0;
}
