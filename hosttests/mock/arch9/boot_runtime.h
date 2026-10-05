#ifndef ARCH9_BOOT_RUNTIME_H
#define ARCH9_BOOT_RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif
#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H
#endif

#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif

#ifndef debug_pci
#define debug_pci(...) do {} while (0)
#endif

#ifndef log_info
#define log_info(...) do {} while (0)
#endif

#ifndef log_warn
#define log_warn(...) do {} while (0)
#endif

#ifndef log_err
#define log_err(...) do {} while (0)
#endif

#ifndef log_debug
#define log_debug(...) do {} while (0)
#endif

#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H 1
#endif
#ifndef _ARCH_X86_64_SPINLOCK_H
#define _ARCH_X86_64_SPINLOCK_H 1
#endif

/* Pre-GS tracking for test_pre_gs_boot */
extern int fake_pre_gs_cpu_id_calls;

#define this_cpu() (fake_pre_gs_cpu_id_calls++, (void *)0)
#define cpu_id() (fake_pre_gs_cpu_id_calls++, 0)
#define smp_processor_id() (fake_pre_gs_cpu_id_calls++, 0)

#endif /* ARCH9_BOOT_RUNTIME_H */
