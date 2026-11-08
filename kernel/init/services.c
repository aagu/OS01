/* kernel/init/services.c — boot services preparation and start.
 *
 * Part of BOOT-5 scheduler and services decoupling.
 */

#include <init/services.h>
#include <net/net.h>
#include <log/log.h>

extern int arch_thermal_start_worker(void);

void boot_services_prepare(void)
{
    net_lwip_init();
}

void boot_services_start(void)
{
    int wrc = arch_thermal_start_worker();
    if (wrc != 0) {
        log_warn("[sched] thermal worker start returned %d (cached in thermal cache as READ_ERROR/WORKER_START)\n", wrc);
    }
}
