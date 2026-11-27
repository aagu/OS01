/* kernel/arch/aarch64/driver/rp1_xhci.c — WoR RP1 xHCI platform provider */
#include <arch/aarch64/boot_mmio.h>
#include <arch/aarch64/rp1_xhci.h>
#include <arch/mmu.h>
#include <device/boot.h>
#include <device/device.h>
#include <driver/xhci.h>
#include <errno.h>
#include <log/log.h>
#include <memory/slab.h>
#include <stdio.h>
#include <string.h>
#include <subsys/subsys.h>

#define DWC3_GCTL       0xC110U
#define DWC3_GSNPSID    0xC120U
#define DWC3_GHWPARAMS0 0xC140U
#define BCM2712_EXT_CFG_INDEX 0x9000U

void aarch64_rp1_xhci_dma_set_window(uint64_t min_pa, uint64_t limit_pa);
void *aarch64_rp1_xhci_dma_ctx(void);

struct rp1_prepared_ctrl {
    bool valid;
    uint64_t cpu_pa;
    uint64_t size;
};

static struct rp1_prepared_ctrl s_prepared[AARCH64_RP1_MAX_XHCI];
static uint32_t s_prepared_count = 0U;
static struct aarch64_rp1_pcie_resources s_rp1_res;

static inline uint32_t rp1_mmio_read32(void *ctx, uint32_t offset)
{
    uintptr_t base = (uintptr_t)ctx;
    uint32_t val = 0U;
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("ldr %w0, [%1]\n\tdsb ld"
                         : "=r"(val) : "r"(base + offset) : "memory");
#else
    val = *(const volatile uint32_t *)(base + offset);
#endif
    return val;
}

static inline void rp1_mmio_write32(void *ctx, uint32_t offset, uint32_t val)
{
    uintptr_t base = (uintptr_t)ctx;
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("str %w0, [%1]\n\tdsb st"
                         :: "r"(val), "r"(base + offset) : "memory");
#else
    *(volatile uint32_t *)(base + offset) = val;
#endif
}

static uint32_t rp1_cfg_save_select(void *ctx, uint32_t bdf)
{
    uint32_t prev = rp1_mmio_read32(ctx, BCM2712_EXT_CFG_INDEX);
    rp1_mmio_write32(ctx, BCM2712_EXT_CFG_INDEX, bdf);
    return prev;
}

static void rp1_cfg_restore(void *ctx, uint32_t saved)
{
    rp1_mmio_write32(ctx, BCM2712_EXT_CFG_INDEX, saved);
}

int aarch64_rp1_xhci_prepare(const struct boot_context *ctx)
{
    s_prepared_count = 0U;
    memset(s_prepared, 0, sizeof(s_prepared));
    memset(&s_rp1_res, 0, sizeof(s_rp1_res));

    const struct aarch64_rp1_xhci_desc *desc = aarch64_rp1_xhci_desc_get();
    if (!desc || !desc->valid || desc->controller_count == 0U)
        return 0;

    struct aarch64_boot_mmio_ctx mmio_ctx;
    int rc = aarch64_boot_mmio_ctx_init(ctx, &mmio_ctx);
    if (rc != 0) {
        log_warn("[rp1-xhci] invalid boot memory map (%d), skipping\n", rc);
        return 0;
    }

    rc = aarch64_boot_mmio_map(&mmio_ctx, desc->rc_reg_base, desc->rc_reg_size);
    if (rc != 0) {
        log_warn("[rp1-xhci] failed to map BCM2712 RC window (%d)\n", rc);
        return 0;
    }

    void *rc_va = (void *)(uintptr_t)(ARCH_PAGE_OFFSET + desc->rc_reg_base);
    struct aarch64_rp1_pcie_io io = {
        .ctx = rc_va,
        .rc_read32 = rp1_mmio_read32,
        .cfg_save_select = rp1_cfg_save_select,
        .cfg_restore = rp1_cfg_restore,
    };
    rc = aarch64_rp1_pcie_resources(desc, &io, &s_rp1_res);
    if (rc != 0 || !s_rp1_res.valid) {
        log_warn("[rp1-xhci] WoR PCIe/RP1 precondition rejected (%d)\n", rc);
        return 0;
    }

    for (uint32_t i = 0U; i < desc->controller_count && i < AARCH64_RP1_MAX_XHCI; i++) {
        const struct aarch64_rp1_xhci_slot *slot = &desc->controllers[i];
        if (!slot->valid || slot->size < AARCH64_RP1_XHCI_MMIO_SIZE)
            continue;
        if (s_rp1_res.bar1_cpu_base > UINT64_MAX - slot->rp1_offset)
            continue;
        uint64_t cpu_pa = s_rp1_res.bar1_cpu_base + slot->rp1_offset;
        rc = aarch64_boot_mmio_map(&mmio_ctx, cpu_pa, slot->size);
        if (rc != 0) {
            log_warn("[rp1-xhci] failed to map DWC3[%u] @ %lx (%d)\n",
                     i, (unsigned long)cpu_pa, rc);
            continue;
        }
        s_prepared[s_prepared_count++] = (struct rp1_prepared_ctrl){
            .valid = true,
            .cpu_pa = cpu_pa,
            .size = slot->size,
        };
    }

    return 0;
}

static bool dwc3_is_host(volatile uint8_t *mmio)
{
    uint32_t id = xhci_read32(mmio + DWC3_GSNPSID);
    uint32_t core = (id >> 16) & 0xFFFFU;
    if (core != 0x5533U && core != 0x3331U)
        return false;

    uint32_t hw0 = xhci_read32(mmio + DWC3_GHWPARAMS0);
    if ((hw0 & 0x7U) == 0U) /* DWC3_MODE_HOST */
        return true;

    uint32_t gctl = xhci_read32(mmio + DWC3_GCTL);
    uint32_t prtcap = (gctl >> 12) & 0x3U;
    return prtcap == 1U; /* DWC3_GCTL_PRTCAP_HOST */
}

int rp1_xhci_attach_mmio(uint32_t index, volatile uint8_t *mmio, uint64_t cpu_pa, uint64_t span)
{
    if (!mmio || span < AARCH64_RP1_XHCI_MMIO_SIZE)
        return -EINVAL;

    if (!dwc3_is_host(mmio)) {
        log_warn("[rp1-xhci] ctrl[%u] @ %lx: DWC3 ID/host check failed\n",
                 index, (unsigned long)cpu_pa);
        return -ENODEV;
    }

    struct xhci_controller *ctrl = (struct xhci_controller *)kmalloc(sizeof(*ctrl));
    if (!ctrl)
        return -ENOMEM;
    memset(ctrl, 0, sizeof(*ctrl));
    mutex_init(&ctrl->io_lock);
    ctrl->io_lock_init = true;
    ctrl->mmio_phys = cpu_pa;
    ctrl->mmio_len = AARCH64_RP1_XHCI_MMIO_SIZE;
    ctrl->mmio_virt = mmio;
    ctrl->dma_ops = aarch64_rp1_xhci_dma_ops();
    ctrl->dma_ctx = aarch64_rp1_xhci_dma_ctx();
    snprintf(ctrl->loc_desc, sizeof(ctrl->loc_desc), "rp1-usb%u", index);

    int rc = xhci_parse_capabilities(ctrl);
    if (rc != 0) {
        kfree(ctrl);
        return rc;
    }

    uint32_t usbcmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
    uint32_t usbsts = xhci_read32(ctrl->op_regs + XHCI_OP_USBSTS);
    if ((usbcmd & XHCI_CMD_RS) != 0U || (usbsts & XHCI_STS_HCH) == 0U) {
        log_err("[rp1-xhci] %s firmware controller still running (cmd=0x%x sts=0x%x)\n",
                xhci_ctrl_loc(ctrl), usbcmd, usbsts);
        (void)xhci_stop(ctrl);
        if (!ctrl->halted_confirmed) {
            ctrl->poisoned = true;
            ctrl->dma_retained = true;
        } else {
            kfree(ctrl);
        }
        device_fail_unsafe(NULL, "rp1-xhci: firmware controller still running");
        return DEVICE_UNSAFE;
    }

    rc = xhci_reset(ctrl);
    if (rc != 0) {
        if (!ctrl->halted_confirmed) {
            ctrl->poisoned = true;
            ctrl->dma_retained = true;
            device_fail_unsafe(NULL, "rp1-xhci: controller reset unsafe");
            return DEVICE_UNSAFE;
        }
        kfree(ctrl);
        return rc;
    }

    rc = xhci_alloc_dma(ctrl);
    if (rc != 0) {
        xhci_free_dma(ctrl);
        kfree(ctrl);
        return rc;
    }

    rc = xhci_program_rings(ctrl);
    if (rc != 0) {
        ctrl->halted_confirmed = true;
        xhci_free_dma(ctrl);
        kfree(ctrl);
        return rc;
    }

    rc = xhci_run(ctrl);
    if (rc != 0) {
        if (!ctrl->halted_confirmed || ctrl->dma_retained) {
            device_fail_unsafe(NULL, "rp1-xhci: controller start unsafe");
            return DEVICE_UNSAFE;
        }
        xhci_free_dma(ctrl);
        kfree(ctrl);
        return rc;
    }

    if (xhci_publish_controller(ctrl) != 0)
        log_warn("[rp1-xhci] controller limit reached\n");

    log_info("[rp1-xhci] %s running @ %lx (slots=%u, ports=%u)\n",
             xhci_ctrl_loc(ctrl), (unsigned long)cpu_pa,
             ctrl->max_slots, ctrl->max_ports);

    xhci_probe_ports(ctrl);
    if (ctrl->poisoned) {
        device_fail_unsafe(NULL, "rp1-xhci: controller poisoned during port probe");
        return DEVICE_UNSAFE;
    }
    return 0;
}

static int rp1_xhci_init(void)
{
    if (s_prepared_count == 0U || !s_rp1_res.valid) {
        log_info("[rp1-xhci] no RP1 USB host configured\n");
        return 0;
    }

    log_info("[rp1-xhci] bar1=%lx dma=[%lx,%lx) ctrls=%u\n",
             (unsigned long)s_rp1_res.bar1_cpu_base,
             (unsigned long)s_rp1_res.dma_min_pa,
             (unsigned long)s_rp1_res.dma_limit_pa,
             s_prepared_count);

    aarch64_rp1_xhci_dma_set_window(s_rp1_res.dma_min_pa, s_rp1_res.dma_limit_pa);

    for (uint32_t i = 0U; i < s_prepared_count; i++) {
        if (!s_prepared[i].valid)
            continue;
        volatile uint8_t *mmio = (volatile uint8_t *)(uintptr_t)(ARCH_PAGE_OFFSET + s_prepared[i].cpu_pa);
        (void)rp1_xhci_attach_mmio(i, mmio, s_prepared[i].cpu_pa, s_prepared[i].size);
    }
    return 0;
}

static const char *const rp1_xhci_requires[] = { "device-boot" };

static int rp1_xhci_register(void)
{
    return register_subsys_deps("rp1-xhci", rp1_xhci_init,
                                SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL,
                                rp1_xhci_requires, 1);
}
SUBSYS_INITCALL(rp1_xhci_register);
