/* kernel/include/selftest/boot_services_fixture.h — BOOT-5 fixture header */
#ifndef SELFTEST_BOOT_SERVICES_FIXTURE_H
#define SELFTEST_BOOT_SERVICES_FIXTURE_H

#if defined(BOOT_SERVICES_TEST_OVERLAP) && (BOOT_SERVICES_TEST_OVERLAP == 1)

void boot5_test_arm(void);
void boot5_test_fs_locked(void);
void boot5_test_after_init(void);

#else

static inline void boot5_test_arm(void) {}
static inline void boot5_test_fs_locked(void) {}
static inline void boot5_test_after_init(void) {}

#endif /* BOOT_SERVICES_TEST_OVERLAP */

#endif /* SELFTEST_BOOT_SERVICES_FIXTURE_H */
