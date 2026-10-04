#ifndef MOCK_AARCH64_BOOT_LOG_H
#define MOCK_AARCH64_BOOT_LOG_H

/* Shadow of kernel/include/arch/aarch64/boot_log.h for the
 * test_aarch64_color_printk_abi host test. Same surface the production
 * stub sees (void-returning kputs); the definition lives in
 * kputs_mock.c and captures the last string for assertions. */
void kputs(const char *message);

void mock_kputs_clear(void);
const char *mock_kputs_last(void);

#endif
