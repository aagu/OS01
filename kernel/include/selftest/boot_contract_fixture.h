#ifndef _OS01_SELFTEST_BOOT_CONTRACT_FIXTURE_H
#define _OS01_SELFTEST_BOOT_CONTRACT_FIXTURE_H

#if defined(BOOT_CONTRACT_TEST)
void boot4_test_register(void);
#else
static inline void boot4_test_register(void) {}
#endif

#endif /* _OS01_SELFTEST_BOOT_CONTRACT_FIXTURE_H */
