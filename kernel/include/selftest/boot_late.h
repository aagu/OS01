#ifndef KERNEL_SELFTEST_BOOT_LATE_H
#define KERNEL_SELFTEST_BOOT_LATE_H

#ifdef OS01_SELFTEST
void selftest_boot_creation_probe(void);
void selftest_boot_run_late(void);
#else
static inline void selftest_boot_creation_probe(void) {}
static inline void selftest_boot_run_late(void) {}
#endif

#endif /* KERNEL_SELFTEST_BOOT_LATE_H */
