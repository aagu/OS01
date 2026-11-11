#ifndef OS01_AARCH64_UACCESS_H
#define OS01_AARCH64_UACCESS_H

#include <arch/aarch64/regs.h>
#include <stdbool.h>
#include <stddef.h>

/* Pre-SMP M4 probe only, with its private TTBR0 installed. Return 0 on
 * success or -EFAULT. Failed preflight copies nothing; a hardware fault
 * can leave a copied prefix. Kernel buffers must remain valid in TTBR1. */
int aarch64_copy_from_user(void *dst, uint64_t src, size_t len);
int aarch64_copy_to_user(uint64_t dst, const void *src, size_t len);

/* Assembly internals: skip software preflight. Use the checked entry
 * points except when exercising a hardware abort in the probe. */
int aarch64_copy_from_user_raw(void *dst, uint64_t src, size_t len);
int aarch64_copy_to_user_raw(uint64_t dst, const void *src, size_t len);

bool aarch64_uaccess_fixup(pt_regs_t *regs, uint64_t esr, uint64_t far);

#endif
