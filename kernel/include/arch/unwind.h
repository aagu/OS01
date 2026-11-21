#ifndef _KERNEL_ARCH_UNWIND_H
#define _KERNEL_ARCH_UNWIND_H

#include <stdbool.h>
#include <stdint.h>

struct unwind_frame {
    uintptr_t next_fp;
    uintptr_t return_pc;
};

bool arch_unwind_read_frame(uintptr_t fp, struct unwind_frame *out);

#endif
