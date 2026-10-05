#ifndef ARCH9_PCI_RUNTIME_H
#define ARCH9_PCI_RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
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

#endif /* ARCH9_PCI_RUNTIME_H */
