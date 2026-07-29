// halt — signal init (PID 1) to halt the system
//
// Sends SIGUSR1 to init, which triggers do_shutdown(RB_POWER_OFF).
// Falls back to direct reboot(RB_HALT_SYSTEM) if init is unreachable.
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/syscall.h>

int main(void)
{
    printf("halt: signalling init (SIGUSR1)...\n");

    if (kill(1, SIGUSR1) == 0) {
        return 0;
    }

    // Init unreachable — fall back to direct syscall
    printf("halt: init unreachable, forcing halt...\n");
    reboot(RB_HALT_SYSTEM);
    return 0;
}
