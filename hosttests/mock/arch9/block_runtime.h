/* hosttests/mock/arch9/block_runtime.h — mock environment for block tests */
#ifndef ARCH9_BLOCK_RUNTIME_H
#define ARCH9_BLOCK_RUNTIME_H

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif

#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif

#endif /* ARCH9_BLOCK_RUNTIME_H */
