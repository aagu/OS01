// reboot — signal init (PID 1) to reboot the system
//
// Sends SIGTERM to init, which triggers do_shutdown(RB_AUTOBOOT).
// Falls back to direct reboot(RB_AUTOBOOT) if init is unreachable.
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/syscall.h>

int main(void)
{
    printf("reboot: signalling init (SIGTERM)...\n");

    if (kill(1, SIGTERM) == 0) {
        return 0;
    }

    // Init unreachable — fall back to direct syscall
    printf("reboot: init unreachable, forcing reboot...\n");
    reboot(RB_AUTOBOOT);
    return 0;
}
