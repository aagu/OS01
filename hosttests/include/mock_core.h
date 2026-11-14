#ifndef OS01_HOSTTEST_MOCK_CORE_H
#define OS01_HOSTTEST_MOCK_CORE_H

#include <stddef.h>

void mock_reset_all(void);
void mock_fail_next(const char *symbol, int n);
long mock_calls(const char *symbol);
const char *mock_printk_last(void);

#endif /* OS01_HOSTTEST_MOCK_CORE_H */
