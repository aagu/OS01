/* hosttests/mock/arch9/xhci_runtime.h — mock environment for xHCI host tests */
#ifndef ARCH9_XHCI_RUNTIME_H
#define ARCH9_XHCI_RUNTIME_H

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

#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H
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

#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H 1
#endif
#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H 1
#endif
#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H 1
#endif

#undef Phy_To_Virt
#define Phy_To_Virt(x) ((void*)(uintptr_t)(x))

void mock_xhci_write32(volatile void *addr, uint32_t val);
#define xhci_write32(addr, val) mock_xhci_write32(addr, val)

uint64_t alloc_4k_page(void);
void free_4k_page(uint64_t phys);
void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* ARCH9_XHCI_RUNTIME_H */
