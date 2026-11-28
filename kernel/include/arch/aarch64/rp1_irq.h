#ifndef ARCH_AARCH64_RP1_IRQ_H
#define ARCH_AARCH64_RP1_IRQ_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*rp1_irq_handler_fn)(void *arg);

enum rp1_irq_trigger {
    RP1_IRQ_LEVEL_HIGH = 0,
    RP1_IRQ_EDGE_RISING = 1,
};

struct rp1_irq;

int rp1_irq_prepare(uint32_t source, enum rp1_irq_trigger trigger,
                    rp1_irq_handler_fn fn, void *arg,
                    struct rp1_irq **out);

int rp1_irq_commit(struct rp1_irq *irq);

int rp1_irq_abort(struct rp1_irq *irq, bool device_mask_verified);

uint32_t rp1_irq_intid(const struct rp1_irq *irq);

#endif /* ARCH_AARCH64_RP1_IRQ_H */
