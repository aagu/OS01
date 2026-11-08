#ifndef _KERNEL_CORE_BOOT_FAILURE_H
#define _KERNEL_CORE_BOOT_FAILURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn))
void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu);

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_CORE_BOOT_FAILURE_H */
