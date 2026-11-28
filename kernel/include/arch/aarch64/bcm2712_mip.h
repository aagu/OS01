#ifndef ARCH_AARCH64_BCM2712_MIP_H
#define ARCH_AARCH64_BCM2712_MIP_H

#include <arch/aarch64/gic.h>
#include <arch/aarch64/rp1_pcie.h>
#include <stdbool.h>
#include <stdint.h>

struct bcm2712_mip_route {
    bool valid;
    bool quarantined;
    const struct aarch64_rp1_irq_platform *plat;
    uint32_t slot;
    uint32_t hw_slot;
    uint32_t intid;
    uint64_t address;
    uint32_t data;
    gic_handler_fn handler;
    uint64_t param;
    struct gic_spi_state saved_gic;
    bool saved_mip_edge;
    bool saved_mip_host_mask;
    bool saved_mip_vpu_mask;
};

int bcm2712_mip_reserve(const struct aarch64_rp1_irq_platform *p,
                        gic_handler_fn handler, uint64_t param,
                        struct bcm2712_mip_route *out);

int bcm2712_mip_enable(struct bcm2712_mip_route *route);
int bcm2712_mip_disable(struct bcm2712_mip_route *route);
bool bcm2712_mip_pending(const struct bcm2712_mip_route *route);
int bcm2712_mip_clear_probe(struct bcm2712_mip_route *route);
int bcm2712_mip_release(struct bcm2712_mip_route *route, bool quiescent);

#endif /* ARCH_AARCH64_BCM2712_MIP_H */
