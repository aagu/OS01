#pragma once
/* BOOT-2 boot_irq_time mock: trimmed arch/irq.h facade with a traced
 * mock IF flag.  The IF state lives in the mock runtime so the test can
 * prove "preserve entry IF" and "enable latches pending even with
 * IF=0" against the REAL controller code. */
#include <stdint.h>
#include <arch/regs.h>

typedef uint64_t arch_irq_state_t;

struct hw_int_type;
typedef struct hw_int_type hw_int_controller_t;

hw_int_controller_t *arch_irq_select_controller(uint32_t gsi);
uint64_t arch_irq_gsi_to_vector(uint32_t gsi);
uint32_t arch_irq_vector_to_gsi(uint64_t vector);
int arch_irq_route_prepare(uint32_t gsi, uint64_t *vector);
void arch_irq_dispatch(pt_regs_t *regs, uint64_t hwirq);
void irq_install(void);

void arch_local_irq_enable(void);
void arch_local_irq_disable(void);
void arch_irq_install(void);

/* Mock IF flag: 0 = masked, 1 = enabled.  Defined in the runtime. */
extern int boot2_if_flag;
extern unsigned long boot2_irq_enable_calls;

static inline arch_irq_state_t arch_local_irq_save(void)
{
    arch_irq_state_t f = (arch_irq_state_t)boot2_if_flag;
    boot2_if_flag = 0;
    return f;
}

static inline void arch_local_irq_restore(arch_irq_state_t flags)
{
    boot2_if_flag = (int)flags;
}

typedef void (*arch_intr_handler_fn)(uint64_t, uint64_t, pt_regs_t *);
extern arch_intr_handler_fn intr_handler_table[256];
extern void *intr_handler_param[256];
