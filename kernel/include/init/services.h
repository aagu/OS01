#ifndef KERNEL_INIT_SERVICES_H
#define KERNEL_INIT_SERVICES_H

/* Public boot services contract (spec §3 / §4).
 * boot_services_prepare() runs inside the boot creation window before sched_boot_start().
 * boot_services_start() runs after sched_boot_start() under scheduler publication. */
void boot_services_prepare(void);
void boot_services_start(void);

#endif /* KERNEL_INIT_SERVICES_H */
