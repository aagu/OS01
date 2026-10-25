#ifndef _KERNEL_INTR_VECTOR_DISPATCH_H
#define _KERNEL_INTR_VECTOR_DISPATCH_H
#include <stdint.h>
#include <arch/regs.h>
/* Neutral registered-handler lookup; no dispatch recursion or controller ACK. */
void intr_dispatch_registered_vector(pt_regs_t *regs, uint64_t vector);
#endif
