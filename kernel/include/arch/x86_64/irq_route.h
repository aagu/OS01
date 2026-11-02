#ifndef _ARCH_X86_64_IRQ_ROUTE_H
#define _ARCH_X86_64_IRQ_ROUTE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <intr/interrupt.h>

#define X86_IRQ_ROUTE_LOW_GSI_MAX    24u
#define X86_IRQ_ROUTE_LOW_VEC_BASE   0x20u

#define X86_IRQ_ROUTE_EXT_VEC_FIRST  0x42u
#define X86_IRQ_ROUTE_EXT_VEC_LAST   0x7fu
#define X86_IRQ_ROUTE_EXT_COUNT      (X86_IRQ_ROUTE_EXT_VEC_LAST - X86_IRQ_ROUTE_EXT_VEC_FIRST + 1) /* 62 */

#define X86_IRQ_VECTOR_INVALID       UINT64_MAX
#define X86_GSI_INVALID              UINT32_MAX

struct pt_regs;
typedef struct pt_regs pt_regs_t;
typedef void (*arch_intr_handler_fn)(uint64_t nr, uint64_t param, pt_regs_t *regs);

struct x86_irq_route {
    uint32_t gsi;
    uint64_t vector;
    uint8_t  ioapic_idx;
    uint8_t  pin;
    uint16_t flags;
    bool     allocated;
};

int x86_irq_routes_init(const struct irq_gsi_range *ranges, size_t count);
void x86_irq_routes_reset(void);
int x86_irq_route_prepare(uint32_t gsi, uint64_t *vector);
bool x86_irq_route_lookup(uint64_t vector, uint32_t *gsi);

const struct x86_irq_route *x86_irq_route_find_by_gsi(uint32_t gsi);
const struct x86_irq_route *x86_irq_route_find_by_vector(uint64_t vector);

/* Fixed MSI-X message slot on vector 0x39 (X86_VECTOR_FIXED_MSIX) */
int x86_fixed_msix_register(const void *owner, arch_intr_handler_fn handler, uint64_t param);
int x86_fixed_msix_unregister(const void *owner);
void x86_fixed_msix_reset(void);
void x86_fixed_msix_dispatch(pt_regs_t *regs, uint64_t vector);
bool x86_fixed_msix_is_registered(void);

static inline bool x86_is_extended_irq_vector(uint64_t vector)
{
    return vector >= X86_IRQ_ROUTE_EXT_VEC_FIRST && vector <= X86_IRQ_ROUTE_EXT_VEC_LAST;
}

#endif
