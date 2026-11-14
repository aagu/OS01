/* kernel/driver/xhci.c — xHCI Host Controller Driver (Phase 1) */
#include <driver/xhci.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <errno.h>
#include <string.h>
#include <arch/x86_64/vmm.h>

#ifndef OS01_HOST_TEST
#include <arch/x86_64/pte.h>
#include <memory/vmm.h>
#include <fs/vfs.h>
#endif

#define XHCI_TIMEOUT_LOOPS 500000

#define XHCI_MAX_CONTROLLERS 4
static struct xhci_controller *s_xhci_controllers[XHCI_MAX_CONTROLLERS];
static uint8_t s_xhci_controller_count = 0;

static inline void xhci_dma_wmb(void)
{
    __sync_synchronize();
}

static inline bool xhci_lock(struct xhci_controller *ctrl)
{
    if (!ctrl) return false;
    bool take = vfs_sched_ready();
    if (take) {
        if (!ctrl->io_lock_init) {
            mutex_init(&ctrl->io_lock);
            ctrl->io_lock_init = true;
        }
        mutex_lock(&ctrl->io_lock);
    }
    return take;
}

static inline void xhci_unlock(struct xhci_controller *ctrl, bool locked)
{
    if (locked && ctrl) {
        mutex_unlock(&ctrl->io_lock);
    }
}

static int xhci_ring_enqueue_trb(struct xhci_trb *ring_virt, uint64_t ring_phys,
                                 uint32_t *enqueue, uint8_t *cycle,
                                 const struct xhci_trb *trb, uint64_t *out_trb_phys)
{
    if (!ring_virt || !enqueue || !cycle || !trb) return -EINVAL;

    if (*enqueue == XHCI_TRB_RING_SIZE - 1) {
        struct xhci_trb *link = &ring_virt[XHCI_TRB_RING_SIZE - 1];
        link->control &= ~(1U << 4); /* Link CH=0 */
        if (*cycle) {
            link->control |= XHCI_TRB_CYCLE_BIT;
        } else {
            link->control &= ~XHCI_TRB_CYCLE_BIT;
        }
        xhci_dma_wmb();
        *cycle ^= 1;
        *enqueue = 0;
    }

    uint32_t idx = *enqueue;
    struct xhci_trb *dst = &ring_virt[idx];
    dst->parameter = trb->parameter;
    dst->status = trb->status;
    xhci_dma_wmb();
    dst->control = (trb->control & ~XHCI_TRB_CYCLE_BIT) |
                   (*cycle ? XHCI_TRB_CYCLE_BIT : 0);

    if (out_trb_phys) {
        *out_trb_phys = ring_phys + (uint64_t)idx * sizeof(struct xhci_trb);
    }
    *enqueue = idx + 1;
    return 0;
}

uint8_t xhci_controller_count(void)
{
    return s_xhci_controller_count;
}

struct xhci_controller *xhci_controller_get(uint8_t index)
{
    if (index >= s_xhci_controller_count) return NULL;
    return s_xhci_controllers[index];
}

int xhci_parse_capabilities(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->mmio_virt) return -EINVAL;

    ctrl->cap_length = ctrl->mmio_virt[XHCI_CAP_CAPLENGTH];
    ctrl->hci_version = *(volatile uint16_t *)(ctrl->mmio_virt + XHCI_CAP_HCIVERSION);

    uint32_t hcsparams1 = xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCSPARAMS1);
    ctrl->max_slots = hcsparams1 & 0xFF;
    ctrl->max_intrs = (hcsparams1 >> 8) & 0x7FF;
    ctrl->max_ports = (hcsparams1 >> 24) & 0xFF;

    uint32_t hcsparams2 = xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCSPARAMS2);
    ctrl->max_scratchpads = (((hcsparams2 >> 21) & 0x1F) << 5) | ((hcsparams2 >> 27) & 0x1F);

    uint32_t hccparams1 = xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCCPARAMS1);
    ctrl->csz = (hccparams1 >> 2) & 1;
    ctrl->ac64 = (hccparams1 & 1U) != 0;
    ctrl->ac64_valid = true;

    ctrl->dboff = xhci_read32(ctrl->mmio_virt + XHCI_CAP_DBOFF) & ~0x03U;
    ctrl->rtsoff = xhci_read32(ctrl->mmio_virt + XHCI_CAP_RTSOFF) & ~0x1FU;

    ctrl->op_regs = ctrl->mmio_virt + ctrl->cap_length;
    ctrl->rt_regs = ctrl->mmio_virt + ctrl->rtsoff;
    ctrl->db_regs = ctrl->mmio_virt + ctrl->dboff;

    if (ctrl->max_scratchpads > 0) {
        log_warn("[xhci] scratchpad buffers requested (%u), unsupported in Phase 1\n", ctrl->max_scratchpads);
        return -ENOTSUP;
    }

    return 0;
}

static int xhci_wait_bit(volatile void *reg, uint32_t mask, bool expected, volatile uint8_t *op_regs)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT_LOOPS; i++) {
        uint32_t val = xhci_read32(reg);
        if (op_regs) {
            uint32_t sts = xhci_read32(op_regs + XHCI_OP_USBSTS);
            if (sts & XHCI_STS_HSE) {
                log_err("[xhci] fatal: Host System Error (HSE) asserted\n");
                return -EIO;
            }
        }
        if (((val & mask) != 0) == expected) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

int xhci_reset(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->op_regs) return -EINVAL;

    volatile uint8_t *op = ctrl->op_regs;

    /* Step 1: Wait for CNR == 0 */
    int rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_CNR, false, op);
    if (rc != 0) {
        log_err("[xhci] reset error: CNR wait failed (%d)\n", rc);
        return rc;
    }

    /* Step 2: Ensure controller is halted (HCH == 1) before resetting */
    uint32_t cmd = xhci_read32(op + XHCI_OP_USBCMD);
    uint32_t sts = xhci_read32(op + XHCI_OP_USBSTS);
    if ((cmd & XHCI_CMD_RS) || !(sts & XHCI_STS_HCH)) {
        xhci_write32(op + XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_HCH, true, op);
        if (rc != 0) {
            log_err("[xhci] reset error: HCH halt wait failed (%d)\n", rc);
            return rc;
        }
    }

    /* Step 3: Write HCRST = 1 */
    cmd = xhci_read32(op + XHCI_OP_USBCMD);
    xhci_write32(op + XHCI_OP_USBCMD, cmd | XHCI_CMD_HCRST);

    /* Step 4: Wait for HCRST == 0 */
    rc = xhci_wait_bit(op + XHCI_OP_USBCMD, XHCI_CMD_HCRST, false, op);
    if (rc != 0) {
        log_err("[xhci] reset error: HCRST completion wait failed (%d)\n", rc);
        return rc;
    }

    /* Step 5: Wait for CNR == 0 */
    rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_CNR, false, op);
    if (rc != 0) {
        log_err("[xhci] reset error: post-reset CNR wait failed (%d)\n", rc);
        return rc;
    }

    return 0;
}

int xhci_alloc_dma(struct xhci_controller *ctrl)
{
    if (!ctrl) return -EINVAL;

    /* 1. DCBAA (Device Context Base Address Array) */
    ctrl->dcbaa_phys = alloc_4k_page();
    if (!ctrl->dcbaa_phys) return -ENOMEM;
    ctrl->dcbaa_virt = (uint64_t *)Phy_To_Virt(ctrl->dcbaa_phys);
    memset(ctrl->dcbaa_virt, 0, 4096);

    /* 2. Command Ring (256 TRBs) */
    ctrl->cmd_ring_phys = alloc_4k_page();
    if (!ctrl->cmd_ring_phys) return -ENOMEM;
    ctrl->cmd_ring_virt = (struct xhci_trb *)Phy_To_Virt(ctrl->cmd_ring_phys);
    memset(ctrl->cmd_ring_virt, 0, 4096);
    ctrl->cmd_ring_enqueue = 0;
    ctrl->cmd_ring_cycle = 1;

    /* Configure TRB 255 as Link TRB */
    struct xhci_trb *link = &ctrl->cmd_ring_virt[255];
    link->parameter = ctrl->cmd_ring_phys;
    link->status = 0;
    link->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    /* 3. Event Ring (256 TRBs) */
    ctrl->event_ring_phys = alloc_4k_page();
    if (!ctrl->event_ring_phys) return -ENOMEM;
    ctrl->event_ring_virt = (struct xhci_trb *)Phy_To_Virt(ctrl->event_ring_phys);
    memset(ctrl->event_ring_virt, 0, 4096);
    ctrl->event_ring_dequeue = 0;
    ctrl->event_ring_cycle = 1;

    /* 4. Event Ring Segment Table (ERST) */
    ctrl->erst_phys = alloc_4k_page();
    if (!ctrl->erst_phys) return -ENOMEM;
    ctrl->erst_virt = (struct xhci_erst_entry *)Phy_To_Virt(ctrl->erst_phys);
    memset(ctrl->erst_virt, 0, 4096);
    ctrl->erst_virt[0].ring_segment_base_address = ctrl->event_ring_phys;
    ctrl->erst_virt[0].ring_segment_size = XHCI_TRB_RING_SIZE;

    return 0;
}

void xhci_setup_interrupter0(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->rt_regs) return;

    volatile uint8_t *ir0 = ctrl->rt_regs + 0x20;

    /* Clear pending interrupts (RW1C), keep IE=0 */
    xhci_write32(ir0 + XHCI_INTR_IMAN, XHCI_IMAN_IP);
    xhci_write32(ir0 + XHCI_INTR_IMOD, 0);

    /* Configure ERST */
    xhci_write32(ir0 + XHCI_INTR_ERSTSZ, 1);
    xhci_write64(ir0 + XHCI_INTR_ERSTBA, ctrl->erst_phys);

    /* Configure ERDP with EHB set to clear busy */
    xhci_write64(ir0 + XHCI_INTR_ERDP, ctrl->event_ring_phys | XHCI_ERDP_EHB);
}

void xhci_delay_ms(uint32_t ms)
{
    /* Busy-wait: port reset recovery must not depend on the tick subsystem,
     * which may not be calibrated at probe time. */
    volatile uint32_t spins = ms * 20000u;
    while (spins--)
        __asm__ __volatile__("" ::: "memory");
}

static int xhci_poll_event_internal(struct xhci_controller *ctrl, uint32_t target_type,
                                    uint64_t expect_param, uint8_t expect_slot,
                                    uint8_t expect_dci, const uint64_t *expect_trbs,
                                    size_t expect_count, size_t *out_match_idx,
                                    struct xhci_trb *out_evt, uint32_t timeout_loops)
{
    if (!ctrl || !ctrl->event_ring_virt || !ctrl->rt_regs) return -EINVAL;

    for (uint32_t i = 0; i < timeout_loops; i++) {
        volatile struct xhci_trb *trb = &ctrl->event_ring_virt[ctrl->event_ring_dequeue];
        uint32_t ctrl_word = *(volatile uint32_t *)&trb->control;

        /* Cycle bit not written by the controller yet: no fresh event. */
        if ((ctrl_word & XHCI_TRB_CYCLE_BIT) != ctrl->event_ring_cycle) {
            continue;
        }

        struct xhci_trb evt = *trb;

        ctrl->event_ring_dequeue = (ctrl->event_ring_dequeue + 1) % XHCI_TRB_RING_SIZE;
        if (ctrl->event_ring_dequeue == 0) {
            ctrl->event_ring_cycle ^= 1;
        }

        /* Advance ERDP with EHB set to clear the interrupt-pending state. */
        uint64_t erdp = ctrl->event_ring_phys + ctrl->event_ring_dequeue * sizeof(struct xhci_trb);
        xhci_write64(ctrl->rt_regs + 0x20 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

        uint32_t evt_type = (evt.control >> 10) & 0x3F;
        bool matched = (evt_type == target_type || target_type == 0);
        if (matched && expect_param && evt.parameter != expect_param) {
            /* Completion of an earlier command that timed out: not ours. */
            log_warn("[xhci] dropping stale completion event (trb=%llx, expected %llx)\n",
                     (unsigned long long)evt.parameter, (unsigned long long)expect_param);
            matched = false;
        }
        if (matched && expect_slot && ((evt.control >> 24) & 0xFF) != expect_slot) {
            /* Transfer event belonging to another endpoint/slot. */
            log_warn("[xhci] dropping transfer event for slot %u (waiting for slot %u)\n",
                     (unsigned)((evt.control >> 24) & 0xFF), expect_slot);
            matched = false;
        }
        if (matched && expect_dci && ((evt.control >> 16) & 0x1F) != expect_dci) {
            log_warn("[xhci] dropping transfer event for DCI %u (waiting for DCI %u)\n",
                     (unsigned)((evt.control >> 16) & 0x1F), expect_dci);
            matched = false;
        }
        if (matched && expect_trbs && expect_count > 0) {
            if ((evt.control & (1U << 2)) != 0) {
                matched = false; /* Event Data (ED=1) does not carry a TRB pointer */
            } else {
                bool found = false;
                for (size_t k = 0; k < expect_count; k++) {
                    if (evt.parameter == expect_trbs[k]) {
                        if (out_match_idx) *out_match_idx = k;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    log_warn("[xhci] dropping transfer event with unexpected TRB %llx\n",
                             (unsigned long long)evt.parameter);
                    matched = false;
                }
            }
        }
        if (matched) {
            if (out_evt) *out_evt = evt;
            return 0;
        }
        if (evt_type == XHCI_TRB_TYPE_PORT_STATUS_EVT) {
            /* Intervening asynchronous port event: consumed and acknowledged
             * above; keep waiting for the target. */
            log_debug("[xhci] consumed asynchronous Port Status Change Event (port %u)\n",
                      (unsigned)((evt.control >> 24) & 0xFF));
            continue;
        }
        log_warn("[xhci] unexpected event type %u while waiting for type %u\n",
                 evt_type, target_type);
    }

    return -ETIMEDOUT;
}

int xhci_poll_event_filtered(struct xhci_controller *ctrl, uint32_t target_type,
                             struct xhci_trb *out_evt, uint32_t timeout_loops)
{
    return xhci_poll_event_internal(ctrl, target_type, 0, 0, 0, NULL, 0, NULL,
                                    out_evt, timeout_loops);
}

int xhci_poll_cmd_completion(struct xhci_controller *ctrl, uint64_t expect_trb_phys,
                             struct xhci_trb *out_evt, uint32_t timeout_loops)
{
    return xhci_poll_event_internal(ctrl, XHCI_TRB_TYPE_CMD_CMPL_EVT, expect_trb_phys,
                                    0, 0, NULL, 0, NULL, out_evt, timeout_loops);
}

int xhci_poll_transfer(struct xhci_controller *ctrl, uint8_t slot_id,
                       struct xhci_trb *out_evt, uint32_t timeout_loops)
{
    return xhci_poll_event_internal(ctrl, XHCI_TRB_TYPE_TRANSFER_EVT, 0, slot_id,
                                    0, NULL, 0, NULL, out_evt, timeout_loops);
}

static int xhci_poll_transfer_matched(struct xhci_controller *ctrl, uint8_t slot_id,
                                      uint8_t dci, const uint64_t *expect_trbs,
                                      size_t expect_count, struct xhci_trb *out_evt,
                                      size_t *out_match_idx, uint32_t timeout_loops)
{
    return xhci_poll_event_internal(ctrl, XHCI_TRB_TYPE_TRANSFER_EVT, 0, slot_id,
                                    dci, expect_trbs, expect_count, out_match_idx,
                                    out_evt, timeout_loops);
}

int xhci_port_reset(struct xhci_controller *ctrl, uint32_t port_num, uint8_t *out_speed)
{
    if (!ctrl || !ctrl->op_regs || port_num < 1 || port_num > ctrl->max_ports)
        return -EINVAL;

    volatile uint8_t *port_reg = ctrl->op_regs + 0x400 + (port_num - 1) * 0x10;

    uint32_t portsc = xhci_read32(port_reg);
    if (!(portsc & XHCI_PORTSC_CCS)) {
        return -ENODEV;
    }

    /* Power the port on first. */
    if (!(portsc & XHCI_PORTSC_PP)) {
        xhci_write32(port_reg, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PP);
        xhci_delay_ms(20);
        portsc = xhci_read32(port_reg);
    }

    /* Issue reset, preserving RW1C bits and read-only state. */
    xhci_write32(port_reg, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PR);

    /* Wait for reset completion: PR self-clears and PRC (or PED) reports it. */
    int rc = -ETIMEDOUT;
    for (uint32_t i = 0; i < XHCI_TIMEOUT_LOOPS; i++) {
        portsc = xhci_read32(port_reg);
        if (!(portsc & XHCI_PORTSC_PR) &&
            ((portsc & XHCI_PORTSC_PRC) || (portsc & XHCI_PORTSC_PED))) {
            rc = 0;
            break;
        }
    }
    if (rc != 0) {
        log_err("[xhci] port %u: reset did not complete\n", port_num);
        return rc;
    }

    /* Acknowledge the reset change flag. */
    xhci_write32(port_reg, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PRC);

    portsc = xhci_read32(port_reg);
    if (!(portsc & XHCI_PORTSC_PED)) {
        log_warn("[xhci] port %u: reset completed but port not enabled (PED=0)\n", port_num);
        return -EIO;
    }

    /* USB 2.0 reset recovery (T_RSTRCY >= 10ms). */
    xhci_delay_ms(10);

    if (out_speed) *out_speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;
    log_info("[xhci] port %u: connected, reset complete, speed=%u\n",
             port_num, (unsigned)((portsc & XHCI_PORTSC_SPEED_MASK) >> 10));
    return 0;
}

static int xhci_submit_cmd_locked(struct xhci_controller *ctrl, const struct xhci_trb *trb,
                                  struct xhci_trb *out_evt)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !ctrl->db_regs || !trb) return -EINVAL;
    if (ctrl->poisoned) return -EIO;

    uint64_t expect_trb = ctrl->cmd_ring_phys +
                          (uint64_t)ctrl->cmd_ring_enqueue * sizeof(struct xhci_trb);

    int rc = xhci_cmd_ring_enqueue(ctrl, trb);
    if (rc != 0) return rc;
    xhci_dma_wmb();
    xhci_write32(ctrl->db_regs, 0);

    struct xhci_trb evt;
    rc = xhci_poll_cmd_completion(ctrl, expect_trb, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc == -ETIMEDOUT) {
        ctrl->poisoned = true;
        ctrl->dma_retained = true;
        log_err("[xhci] command timed out (trb=%llx): controller poisoned\n",
                (unsigned long long)expect_trb);
        return -ETIMEDOUT;
    }
    if (rc != 0) return rc;
    if (out_evt) *out_evt = evt;
    return 0;
}

static int xhci_disable_slot_locked(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS || !ctrl->slots[slot_id])
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (slot->disabled) return 0;
    if (ctrl->poisoned || !ctrl->cmd_ring_virt || !ctrl->db_regs) return -EIO;

    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_DISABLE_SLOT << 10) |
                  ctrl->cmd_ring_cycle;

    struct xhci_trb evt;
    int rc = xhci_submit_cmd_locked(ctrl, &trb, &evt);
    if (rc != 0) return rc;
    if (((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        log_err("[xhci] disable slot %u failed (cc=%u)\n",
                slot_id, (unsigned)((evt.status >> 24) & 0xFF));
        return -EIO;
    }
    slot->disabled = true;
    return 0;
}

int xhci_enable_slot(struct xhci_controller *ctrl, uint8_t port_num, uint8_t speed,
                     uint8_t *out_slot_id)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !ctrl->db_regs) return -EINVAL;
    bool locked = xhci_lock(ctrl);
    if (ctrl->poisoned) {
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_TYPE_ENABLE_SLOT << 10) | ctrl->cmd_ring_cycle;

    struct xhci_trb evt;
    int rc = xhci_submit_cmd_locked(ctrl, &trb, &evt);
    if (rc != 0) {
        xhci_unlock(ctrl, locked);
        return rc;
    }
    if (((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        log_err("[xhci] enable slot command failed (cc=%u)\n",
                (unsigned)((evt.status >> 24) & 0xFF));
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    uint8_t slot_id = (evt.control >> 24) & 0xFF;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) {
        xhci_unlock(ctrl, locked);
        return -ERANGE;
    }

    struct xhci_slot *slot = (struct xhci_slot *)kmalloc(sizeof(struct xhci_slot));
    if (!slot) {
        xhci_unlock(ctrl, locked);
        return -ENOMEM;
    }
    memset(slot, 0, sizeof(*slot));
    slot->slot_id = slot_id;
    slot->port_num = port_num;
    slot->speed = speed;
    slot->active = true;
    ctrl->slots[slot_id] = slot;

    log_info("[xhci] slot %u allocated (port %u)\n", slot_id, port_num);
    if (out_slot_id) *out_slot_id = slot_id;
    xhci_unlock(ctrl, locked);
    return 0;
}

int xhci_address_device(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || !ctrl->dcbaa_virt) return -EINVAL;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS || !ctrl->slots[slot_id])
        return -EINVAL;

    bool locked = xhci_lock(ctrl);
    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (ctrl->poisoned || slot->poisoned || slot->disabled) {
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    /* rc is used by every error path below, including the ones taken before
     * the command is issued: seed it with the allocation failure code. */
    int rc = -ENOMEM;

    /* Strict allocation order with rollback unwind. */
    slot->input_ctx_phys = alloc_4k_page();
    if (!slot->input_ctx_phys) goto err_input;
    slot->input_ctx_virt = (void *)Phy_To_Virt(slot->input_ctx_phys);

    slot->dev_ctx_phys = alloc_4k_page();
    if (!slot->dev_ctx_phys) goto err_dev;
    slot->dev_ctx_virt = (void *)Phy_To_Virt(slot->dev_ctx_phys);

    slot->ep0_ring_phys = alloc_4k_page();
    if (!slot->ep0_ring_phys) goto err_ep0;
    slot->ep0_ring_virt = (struct xhci_trb *)Phy_To_Virt(slot->ep0_ring_phys);

    slot->dma_buf_phys = alloc_4k_page();
    if (!slot->dma_buf_phys) goto err_dma;
    slot->dma_buf_virt = (void *)Phy_To_Virt(slot->dma_buf_phys);

    memset(slot->input_ctx_virt, 0, 4096);
    memset(slot->dev_ctx_virt, 0, 4096);
    memset(slot->ep0_ring_virt, 0, 4096);
    memset(slot->dma_buf_virt, 0, 4096);

    /* Output device context must be registered before the command. */
    ctrl->dcbaa_virt[slot_id] = slot->dev_ctx_phys;

    /* EP0 Transfer Ring: Link TRB wraps index 255 back to the ring base. */
    struct xhci_trb *link = &slot->ep0_ring_virt[XHCI_TRB_RING_SIZE - 1];
    link->parameter = slot->ep0_ring_phys;
    link->status = 0;
    link->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;
    slot->ep0_ring_cycle = 1;
    slot->ep0_ring_enqueue = 0;

    uint16_t max_packet_size;
    switch (slot->speed) {
    case XHCI_PORT_SPEED_LOW:
        max_packet_size = 8;
        break;
    case XHCI_PORT_SPEED_SUPER:
        max_packet_size = 512;
        break;
    default:
        max_packet_size = 64;
        break;
    }
    slot->max_packet_size = max_packet_size;

    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(slot->input_ctx_virt, ctrl->csz);
    ctrl_ctx[0] = 0;                        /* Drop Flags */
    ctrl_ctx[1] = (1U << 0) | (1U << 1);    /* Add Flags: Slot + EP0 */

    uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(slot->input_ctx_virt, ctrl->csz);
    slot_ctx[0] = ((uint32_t)slot->speed << 20) | (1U << 27); /* Speed, Context Entries=1 */
    slot_ctx[1] = ((uint32_t)slot->port_num << 16);           /* Root Hub Port Number */

    uint32_t *ep0_ctx = (uint32_t *)xhci_input_ep0_ctx(slot->input_ctx_virt, ctrl->csz);
    ep0_ctx[0] = 0;                                              /* EP State */
    ep0_ctx[1] = (3U << 1) | (4U << 3) | ((uint32_t)max_packet_size << 16); /* CErr, EP Type=Control, MPS */
    ep0_ctx[2] = (uint32_t)(slot->ep0_ring_phys & 0xFFFFFFF0ULL) | 1U;      /* TR Dequeue Lo, DCS=1 */
    ep0_ctx[3] = (uint32_t)(slot->ep0_ring_phys >> 32);                     /* TR Dequeue Hi */
    ep0_ctx[4] = 8U;                                             /* Average TRB Length */

    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.parameter = slot->input_ctx_phys;
    trb.control = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_ADDRESS_DEV << 10) | ctrl->cmd_ring_cycle;

    uint64_t expect_trb = ctrl->cmd_ring_phys +
                          (uint64_t)ctrl->cmd_ring_enqueue * sizeof(struct xhci_trb);

    rc = xhci_cmd_ring_enqueue(ctrl, &trb);
    if (rc != 0) goto err_unexposed;

    /* Once doorbell 0 is rung, xHC may own DCBAA[slot_id] and all context/ring pages. */
    slot->ctx_published = true;
    xhci_dma_wmb();
    xhci_write32(ctrl->db_regs, 0);

    struct xhci_trb evt;
    rc = xhci_poll_cmd_completion(ctrl, expect_trb, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc == -ETIMEDOUT) {
        ctrl->poisoned = true;
        ctrl->dma_retained = true;
        slot->poisoned = true;
        slot->dma_retained = true;
        log_err("[xhci] slot %u: address device timed out; retaining DMA\n", slot_id);
        xhci_unlock(ctrl, locked);
        return -ETIMEDOUT;
    }
    if (rc != 0 || ((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        if (rc == 0) {
            log_err("[xhci] address device command failed (cc=%u)\n",
                    (unsigned)((evt.status >> 24) & 0xFF));
            rc = -EIO;
        }
        /* Require Disable Slot before clearing DCBAA or freeing context pages. */
        if (xhci_disable_slot_locked(ctrl, slot_id) != 0) {
            slot->poisoned = true;
            slot->dma_retained = true;
            xhci_unlock(ctrl, locked);
            return rc;
        }
        slot->ctx_published = false;
        goto err_unexposed;
    }

    log_info("[xhci] slot %u addressed successfully\n", slot_id);
    xhci_unlock(ctrl, locked);
    return 0;

err_unexposed:
    ctrl->dcbaa_virt[slot_id] = 0;
err_dma:
    if (slot->dma_buf_phys) {
        free_4k_page(slot->dma_buf_phys);
        slot->dma_buf_phys = 0;
        slot->dma_buf_virt = NULL;
    }
err_ep0:
    if (slot->ep0_ring_phys) {
        free_4k_page(slot->ep0_ring_phys);
        slot->ep0_ring_phys = 0;
        slot->ep0_ring_virt = NULL;
    }
err_dev:
    if (slot->dev_ctx_phys) {
        free_4k_page(slot->dev_ctx_phys);
        slot->dev_ctx_phys = 0;
        slot->dev_ctx_virt = NULL;
    }
err_input:
    if (slot->input_ctx_phys) {
        free_4k_page(slot->input_ctx_phys);
        slot->input_ctx_phys = 0;
        slot->input_ctx_virt = NULL;
    }
    xhci_unlock(ctrl, locked);
    return rc != 0 ? rc : -EIO;
}

void xhci_free_endpoint_rings(struct xhci_slot *slot)
{
    if (!slot) return;

    memset(&slot->msc_candidate, 0, sizeof(slot->msc_candidate));
    memset(&slot->msc_iface, 0, sizeof(slot->msc_iface));

    if (!slot->ep_rings) return;

    for (uint8_t dci = XHCI_MIN_ENDPOINT_DCI; dci <= slot->max_dci; dci++) {
        if (slot->ep_rings[dci]) {
            if (slot->ep_rings[dci]->ring_phys) {
                free_4k_page(slot->ep_rings[dci]->ring_phys);
                slot->ep_rings[dci]->ring_phys = 0;
            }
            kfree(slot->ep_rings[dci]);
            slot->ep_rings[dci] = NULL;
        }
    }

    kfree(slot->ep_rings);
    slot->ep_rings = NULL;
    slot->max_dci = 0;
}

static void xhci_free_64k_staging(struct xhci_slot *slot)
{
    if (!slot) return;
#ifdef OS01_HOST_TEST
    if (slot->bulk_staging_page) {
        free(slot->bulk_staging_page);
    } else if (slot->bulk_staging_virt) {
        free(slot->bulk_staging_virt);
    }
#else
    if (slot->bulk_staging_page) {
        free_pages((struct Page *)slot->bulk_staging_page, 1);
    }
#endif
    slot->bulk_staging_page = NULL;
    slot->bulk_staging_virt = NULL;
    slot->bulk_staging_phys = 0;
}

static int xhci_alloc_64k_staging(struct xhci_slot *slot)
{
    if (!slot) return -EINVAL;
    if (slot->bulk_staging_virt) return 0;

#ifdef OS01_HOST_TEST
    void *p = aligned_alloc(65536, 65536);
    if (!p) return -ENOMEM;
    memset(p, 0, 65536);
    slot->bulk_staging_page = p;
    slot->bulk_staging_virt = p;
    slot->bulk_staging_phys = (uint64_t)(uintptr_t)p;
#else
    struct Page *p = alloc_pages(ZONE_NORMAL, 1, PG_Kernel);
    if (!p) return -ENOMEM;
    slot->bulk_staging_page = p;
    slot->bulk_staging_phys = p->phy_address;
    slot->bulk_staging_virt = (void *)Phy_To_Virt(p->phy_address);
    memset(slot->bulk_staging_virt, 0, 65536);
#endif
    return 0;
}

void xhci_free_slot(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) return;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot) return;

    /* Once xHC may know a slot/context, require successful Disable Slot or
     * confirmed controller halt before clearing DCBAA or freeing DMA. */
    if (slot->ctx_published && !ctrl->halted_confirmed && !slot->disabled) {
        if (ctrl->running && !ctrl->poisoned && !slot->poisoned && ctrl->cmd_ring_virt) {
            xhci_disable_slot_locked(ctrl, slot_id);
        }
        if (!slot->disabled &&
            (ctrl->running || ctrl->poisoned || ctrl->dma_retained || slot->poisoned || slot->dma_retained)) {
            slot->poisoned = true;
            slot->dma_retained = true;
            log_warn("[xhci] slot %u: DMA ownership unproven; retaining slot/contexts\n", slot_id);
            return;
        }
    }

    if (ctrl->dcbaa_virt) ctrl->dcbaa_virt[slot_id] = 0;

    /* Release the dynamically allocated endpoint rings before the base DMA
     * contexts: the endpoint rings are indexed by slot->max_dci. */
    xhci_free_endpoint_rings(slot);
    xhci_free_64k_staging(slot);

    if (slot->input_ctx_phys) free_4k_page(slot->input_ctx_phys);
    if (slot->dev_ctx_phys) free_4k_page(slot->dev_ctx_phys);
    if (slot->ep0_ring_phys) free_4k_page(slot->ep0_ring_phys);
    if (slot->dma_buf_phys) free_4k_page(slot->dma_buf_phys);

    if (slot->raw_descriptors) {
        kfree(slot->raw_descriptors);
        slot->raw_descriptors = NULL;
        slot->raw_descriptors_len = 0;
    }

    kfree(slot);
    ctrl->slots[slot_id] = NULL;
}

int xhci_ep0_ring_enqueue(struct xhci_slot *slot, const struct xhci_trb *trb)
{
    if (!slot || !slot->ep0_ring_virt) return -EINVAL;
    return xhci_ring_enqueue_trb(slot->ep0_ring_virt, slot->ep0_ring_phys,
                                 &slot->ep0_ring_enqueue, &slot->ep0_ring_cycle,
                                 trb, NULL);
}

static int xhci_ep0_get_descriptor(struct xhci_controller *ctrl, struct xhci_slot *slot,
                                   uint8_t desc_type, uint16_t length);

int xhci_get_device_descriptor(struct xhci_controller *ctrl, uint8_t slot_id,
                               struct usb_device_descriptor *out_desc)
{
    if (!ctrl || !ctrl->db_regs || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id] || !out_desc)
        return -EINVAL;

    bool locked = xhci_lock(ctrl);
    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (ctrl->poisoned || slot->poisoned || slot->disabled) {
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    int rc = xhci_ep0_get_descriptor(ctrl, slot, USB_DESC_TYPE_DEVICE, 18);
    if (rc < 0) {
        xhci_unlock(ctrl, locked);
        return rc;
    }

    uint32_t actual = (uint32_t)rc;
    if (actual < 8) {
        log_err("[xhci] slot %u: short descriptor read (%u bytes)\n", slot_id, actual);
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    /* Only the bytes the device actually sent are valid: zero the rest so
     * fields beyond the transfer never leak stale DMA data. */
    memset(&slot->desc, 0, sizeof(slot->desc));
    uint32_t copy_len = actual < sizeof(slot->desc) ? actual : sizeof(slot->desc);
    memcpy(&slot->desc, slot->dma_buf_virt, copy_len);
    *out_desc = slot->desc;

    log_info("[xhci] slot %u: USB device %04x:%04x (class %02x, subclass %02x, ep0_max=%u)\n",
             slot_id, slot->desc.idVendor, slot->desc.idProduct,
             slot->desc.bDeviceClass, slot->desc.bDeviceSubClass,
             slot->desc.bMaxPacketSize0);
    xhci_unlock(ctrl, locked);
    return 0;
}

uint8_t xhci_calc_interval(uint8_t speed, uint8_t ep_type, uint8_t bInterval)
{
    /* The Interval field is only meaningful for Interrupt endpoints; every
     * other endpoint type must program it as 0 (xHCI 6.2.3.4). */
    if (ep_type != XHCI_EP_TYPE_INTR_IN && ep_type != XHCI_EP_TYPE_INTR_OUT)
        return 0;

    if (bInterval == 0) return 3;

    if (speed == XHCI_PORT_SPEED_HIGH || speed == XHCI_PORT_SPEED_SUPER) {
        return (bInterval <= 16) ? (uint8_t)(bInterval - 1) : 15;
    }

    /* Full / Low speed Interrupt: the polling period is 2^(interval - 3) ms,
     * so pick the smallest exponent whose period still fits in bInterval ms. */
    uint8_t interval = 3;
    while (interval < 10 && (1U << (interval - 3 + 1)) <= bInterval) {
        interval++;
    }
    return interval;
}

static bool xhci_parse_ss_companion(const uint8_t *buffer, uint32_t total,
                                    uint32_t ep_offset, uint8_t ep_len,
                                    uint8_t *out_max_burst)
{
    uint32_t comp_off = ep_offset + ep_len;
    if (comp_off + 2 > total) return false;
    uint8_t clen = buffer[comp_off];
    uint8_t ctype = buffer[comp_off + 1];
    if (clen < sizeof(struct usb_ss_ep_companion_descriptor) ||
        comp_off + clen > total ||
        ctype != USB_DESC_TYPE_SS_EP_COMPANION) {
        return false;
    }
    const struct usb_ss_ep_companion_descriptor *comp =
        (const struct usb_ss_ep_companion_descriptor *)(buffer + comp_off);
    if (comp->bMaxBurst > 15) return false;
    if (out_max_burst) *out_max_burst = comp->bMaxBurst;
    return true;
}

static bool xhci_msc_rings_valid(const struct xhci_slot *slot,
                                 const struct xhci_msc_iface *msc)
{
    if (!slot || !msc || !msc->active || !slot->ep_rings) return false;
    if (msc->in_dci < XHCI_MIN_ENDPOINT_DCI || msc->in_dci > slot->max_dci) return false;
    if (msc->out_dci < XHCI_MIN_ENDPOINT_DCI || msc->out_dci > slot->max_dci) return false;

    const struct xhci_ep_ring *in_ring = slot->ep_rings[msc->in_dci];
    const struct xhci_ep_ring *out_ring = slot->ep_rings[msc->out_dci];
    if (!in_ring || !in_ring->active || in_ring->ep_type != XHCI_EP_TYPE_BULK_IN ||
        in_ring->max_packet_size == 0)
        return false;
    if (!out_ring || !out_ring->active || out_ring->ep_type != XHCI_EP_TYPE_BULK_OUT ||
        out_ring->max_packet_size == 0)
        return false;
    return true;
}

int xhci_parse_config_descriptor(struct xhci_slot *slot, const uint8_t *buffer,
                                  uint16_t length)
{
    if (!slot || !buffer || length < 2) return 0;

    memset(&slot->iface, 0, sizeof(slot->iface));
    memset(&slot->msc_candidate, 0, sizeof(slot->msc_candidate));
    memset(&slot->msc_iface, 0, sizeof(slot->msc_iface));

    /* 32-bit arithmetic: offset + len must not wrap around the 16-bit
     * boundary, or the bounds check below could be bypassed. */
    uint32_t offset = 0;
    uint32_t total = length;
    uint8_t max_dci = 1; /* EP0 always exists at DCI 1 */
    bool in_iface = false;
    bool iface_alt0 = false;

    bool msc_found = false;
    bool msc_cur_eligible = false;
    uint8_t msc_cur_iface_num = 0;
    uint8_t msc_cur_in_addr = 0, msc_cur_in_dci = 0;
    uint8_t msc_cur_out_addr = 0, msc_cur_out_dci = 0;

    while (offset + 2 <= total) {
        uint8_t len = buffer[offset];
        uint8_t type = buffer[offset + 1];

        /* Corrupt descriptor: a zero/short length or a length that runs past
         * the buffer end makes the rest of the hierarchy untrustworthy. */
        if (len < 2 || offset + len > total) break;

        if (type == USB_DESC_TYPE_INTERFACE) {
            if (len < sizeof(struct usb_interface_descriptor)) break;
            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(buffer + offset);
            in_iface = true;
            iface_alt0 = (iface->bAlternateSetting == 0);

            /* Only the primary interface of the active configuration. */
            if (iface->bInterfaceNumber == 0 && iface->bAlternateSetting == 0) {
                slot->iface.interface_num = iface->bInterfaceNumber;
                slot->iface.class_code = iface->bInterfaceClass;
                slot->iface.subclass_code = iface->bInterfaceSubClass;
                slot->iface.protocol = iface->bInterfaceProtocol;
                slot->iface.num_endpoints = iface->bNumEndpoints;
                slot->iface.active = true;
            }

            if (!msc_found && iface_alt0 &&
                iface->bInterfaceClass == 0x08 &&
                iface->bInterfaceSubClass == 0x06 &&
                iface->bInterfaceProtocol == 0x50) {
                msc_cur_eligible = true;
                msc_cur_iface_num = iface->bInterfaceNumber;
                msc_cur_in_addr = 0;
                msc_cur_in_dci = 0;
                msc_cur_out_addr = 0;
                msc_cur_out_dci = 0;
            } else {
                msc_cur_eligible = false;
            }
        } else if (type == USB_DESC_TYPE_ENDPOINT) {
            if (len < sizeof(struct usb_endpoint_descriptor)) {
                msc_cur_eligible = false;
                break;
            }
            if (!in_iface || !iface_alt0) {
                offset += len;
                continue;
            }
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(buffer + offset);
            uint8_t ep_num = ep->bEndpointAddress & 0x0F;
            uint8_t is_in = (ep->bEndpointAddress & 0x80) != 0;
            /* EP0 is owned by the dedicated EP0 ring: a non-EP0 descriptor
             * claiming endpoint number 0 is corrupt. */
            if (ep_num == 0) {
                offset += len;
                continue;
            }
            uint8_t dci = ep_num * 2 + (is_in ? 1 : 0);
            if (dci < XHCI_MIN_ENDPOINT_DCI || dci > XHCI_MAX_ENDPOINT_DCI) {
                offset += len;
                continue;
            }
            uint8_t ep_xfer = ep->bmAttributes & 3;
            uint16_t mps = ep->wMaxPacketSize & 0x7FF;
            if (ep_xfer == 2 && (mps == 0 || mps > 1024)) {
                msc_cur_eligible = false;
                offset += len;
                continue;
            }
            if (dci > max_dci) max_dci = dci;

            if (ep_xfer == 2 && msc_cur_eligible) {
                if (slot->speed == XHCI_PORT_SPEED_SUPER &&
                    !xhci_parse_ss_companion(buffer, total, offset, len, NULL)) {
                    msc_cur_eligible = false;
                } else {
                    if (is_in && msc_cur_in_dci == 0) {
                        msc_cur_in_addr = ep->bEndpointAddress;
                        msc_cur_in_dci = dci;
                    } else if (!is_in && msc_cur_out_dci == 0) {
                        msc_cur_out_addr = ep->bEndpointAddress;
                        msc_cur_out_dci = dci;
                    }
                    if (msc_cur_in_dci != 0 && msc_cur_out_dci != 0) {
                        slot->msc_candidate.active = true;
                        slot->msc_candidate.interface_num = msc_cur_iface_num;
                        slot->msc_candidate.in_addr = msc_cur_in_addr;
                        slot->msc_candidate.in_dci = msc_cur_in_dci;
                        slot->msc_candidate.out_addr = msc_cur_out_addr;
                        slot->msc_candidate.out_dci = msc_cur_out_dci;
                        msc_found = true;
                        msc_cur_eligible = false;
                    }
                }
            }
        }

        offset += len;
    }

    return max_dci;
}

int xhci_setup_endpoint_rings(struct xhci_slot *slot, const uint8_t *buffer,
                               uint16_t length)
{
    if (!slot || !buffer) return -EINVAL;

    /* Defensive reset: drop any ring discovered by a previous enumeration
     * before the hierarchy is re-parsed, so a stale table can never be
     * reused with a different max_dci. */
    xhci_free_endpoint_rings(slot);

    int max_dci = xhci_parse_config_descriptor(slot, buffer, length);
    if (max_dci < XHCI_MIN_ENDPOINT_DCI) {
        memset(&slot->msc_candidate, 0, sizeof(slot->msc_candidate));
        return 0; /* EP0 only: nothing to ring */
    }

    slot->ep_rings = kmalloc(sizeof(struct xhci_ep_ring *) * (max_dci + 1));
    if (!slot->ep_rings) {
        memset(&slot->msc_candidate, 0, sizeof(slot->msc_candidate));
        return -ENOMEM;
    }
    memset(slot->ep_rings, 0, sizeof(struct xhci_ep_ring *) * (max_dci + 1));
    slot->max_dci = (uint8_t)max_dci;

    /* Second pass: instantiate one transfer ring per active alt-0 endpoint. */
    uint32_t offset = 0;
    uint32_t total = length;
    bool in_iface = false;
    bool iface_alt0 = false;
    while (offset + 2 <= total) {
        uint8_t len = buffer[offset];
        uint8_t type = buffer[offset + 1];
        if (len < 2 || offset + len > total) break;

        if (type == USB_DESC_TYPE_INTERFACE) {
            if (len < sizeof(struct usb_interface_descriptor)) break;
            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(buffer + offset);
            in_iface = true;
            iface_alt0 = (iface->bAlternateSetting == 0);
            offset += len;
            continue;
        }

        if (type == USB_DESC_TYPE_ENDPOINT &&
            len >= sizeof(struct usb_endpoint_descriptor)) {
            if (!in_iface || !iface_alt0) {
                offset += len;
                continue;
            }
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(buffer + offset);
            uint8_t ep_num = ep->bEndpointAddress & 0x0F;
            uint8_t is_in = (ep->bEndpointAddress & 0x80) != 0;
            if (ep_num == 0) {
                offset += len;
                continue;
            }
            uint8_t dci = ep_num * 2 + (is_in ? 1 : 0);
            /* Out of range or already defined: skip, never duplicate. */
            if (dci < XHCI_MIN_ENDPOINT_DCI || dci > max_dci || slot->ep_rings[dci]) {
                offset += len;
                continue;
            }

            uint8_t ep_xfer = ep->bmAttributes & 3;
            uint16_t mps = ep->wMaxPacketSize & 0x7FF;
            if (ep_xfer == 2 && (mps == 0 || mps > 1024)) {
                offset += len;
                continue;
            }

            /* Isochronous endpoints need a bandwidth allocation that this
             * driver does not model, and their Interval field follows a
             * different encoding: leave them unconfigured instead of
             * programming an endpoint context that cannot work. */
            if (ep_xfer == 1) {
                log_warn("[xhci] slot %u: isochronous endpoint DCI %u not supported\n",
                         slot->slot_id, (unsigned)dci);
                offset += len;
                continue;
            }

            uint8_t max_burst = 0;
            if (ep_xfer == 2 && slot->speed == XHCI_PORT_SPEED_SUPER) {
                xhci_parse_ss_companion(buffer, total, offset, len, &max_burst);
            }

            struct xhci_ep_ring *ring = kmalloc(sizeof(struct xhci_ep_ring));
            if (!ring) goto oom;
            memset(ring, 0, sizeof(*ring));

            ring->ring_phys = alloc_4k_page();
            if (!ring->ring_phys) {
                kfree(ring);
                goto oom;
            }
            ring->ring_virt = (struct xhci_trb *)Phy_To_Virt(ring->ring_phys);
            memset(ring->ring_virt, 0, 4096);

            /* Link TRB at index 255 wraps the ring back to its base. */
            struct xhci_trb *link = &ring->ring_virt[XHCI_TRB_RING_SIZE - 1];
            link->parameter = ring->ring_phys;
            link->status = 0;
            link->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

            ring->cycle = 1;
            ring->enqueue = 0;
            ring->dci = dci;

            switch (ep_xfer) {
            case 0: /* Control */
                ring->ep_type = XHCI_EP_TYPE_CONTROL;
                break;
            case 2: /* Bulk */
                ring->ep_type = is_in ? XHCI_EP_TYPE_BULK_IN : XHCI_EP_TYPE_BULK_OUT;
                break;
            default: /* Interrupt (isochronous endpoints are skipped above) */
                ring->ep_type = is_in ? XHCI_EP_TYPE_INTR_IN : XHCI_EP_TYPE_INTR_OUT;
                break;
            }

            ring->interval = xhci_calc_interval(slot->speed, ring->ep_type, ep->bInterval);
            ring->max_packet_size = mps;
            ring->max_burst = max_burst;
            ring->active = true;

            slot->ep_rings[dci] = ring;

            log_info("[xhci] slot %u: endpoint DCI %u configured (type=%u, mps=%u, interval=%u)\n",
                     slot->slot_id, (unsigned)dci, (unsigned)ring->ep_type,
                     (unsigned)ring->max_packet_size, (unsigned)ring->interval);
        }

        offset += len;
    }

    if (slot->msc_candidate.active && !xhci_msc_rings_valid(slot, &slot->msc_candidate)) {
        memset(&slot->msc_candidate, 0, sizeof(slot->msc_candidate));
    }

    return 0;

oom:
    /* Roll back every ring allocated so far: no page and no struct leaks. */
    xhci_free_endpoint_rings(slot);
    return -ENOMEM;
}

/* Standard IN control transfer on EP0: Setup + Data + Status stages.
 * Reads `length` bytes of the requested descriptor into the slot DMA
 * buffer and returns the number of bytes actually transferred, or a
 * negative errno.
 *
 * The completion event is requested on the Data Stage TRB, because that is
 * the TRB whose Transfer Event Length carries the residual of a short
 * transfer.  A controller that only reported the Status Stage TRB would
 * always announce a zero residual and the short-packet guard below could
 * never fire. */
static int xhci_ep0_cc_to_errno(uint32_t cc)
{
    if (cc == XHCI_COMP_STALL) return -EPIPE;
    return -EIO;
}

static int xhci_ep0_get_descriptor(struct xhci_controller *ctrl, struct xhci_slot *slot,
                                   uint8_t desc_type, uint16_t length)
{
    if (!ctrl || !slot || !slot->ep0_ring_virt || !ctrl->db_regs) return -EINVAL;
    if (ctrl->poisoned || slot->poisoned || slot->disabled) return -EIO;

    /* Setup Stage: GET_DESCRIPTOR(desc_type), 8-byte setup packet as
     * immediate data.  Layout: wLength[63:48] wIndex[47:32]
     * wValue[31:16] bRequest[15:8] bmRequestType[7:0]. */
    struct xhci_trb setup;
    memset(&setup, 0, sizeof(setup));
    setup.parameter = ((uint64_t)length << 48) |
                      ((uint64_t)(desc_type << 8) << 16) |
                      ((uint64_t)0x06 << 8) | 0x80ULL;
    setup.status = 8;
    setup.control = (3U << 16) | XHCI_TRB_C_IDT | (XHCI_TRB_TYPE_SETUP_STAGE << 10);

    /* Data Stage: IN, `length` bytes into the slot DMA buffer, IOC=1 so its
     * Transfer Event supplies the actual/residual count. */
    struct xhci_trb data;
    memset(&data, 0, sizeof(data));
    data.parameter = slot->dma_buf_phys;
    data.status = length;
    data.control = XHCI_TRB_C_DIR | XHCI_TRB_C_IOC | (XHCI_TRB_TYPE_DATA_STAGE << 10);

    /* Status Stage: OUT handshake, IOC=1 to retire the request. */
    struct xhci_trb status;
    memset(&status, 0, sizeof(status));
    status.control = XHCI_TRB_C_IOC | (XHCI_TRB_TYPE_STATUS_STAGE << 10);

    uint64_t trbs[3] = { 0, 0, 0 };
    int rc = xhci_ring_enqueue_trb(slot->ep0_ring_virt, slot->ep0_ring_phys,
                                   &slot->ep0_ring_enqueue, &slot->ep0_ring_cycle,
                                   &setup, &trbs[0]);
    if (rc != 0) return rc;
    rc = xhci_ring_enqueue_trb(slot->ep0_ring_virt, slot->ep0_ring_phys,
                               &slot->ep0_ring_enqueue, &slot->ep0_ring_cycle,
                               &data, &trbs[1]);
    if (rc != 0) return rc;
    rc = xhci_ring_enqueue_trb(slot->ep0_ring_virt, slot->ep0_ring_phys,
                               &slot->ep0_ring_enqueue, &slot->ep0_ring_cycle,
                               &status, &trbs[2]);
    if (rc != 0) return rc;

    /* Ring the slot doorbell: Target = 1 (EP0). */
    xhci_dma_wmb();
    xhci_write32(ctrl->db_regs + slot->slot_id * 4, 1);

    /* Wait for Data Stage completion (or Setup/Status error). */
    struct xhci_trb evt;
    size_t match_idx = 0;
    rc = xhci_poll_transfer_matched(ctrl, slot->slot_id, 1, trbs, 3, &evt,
                                    &match_idx, XHCI_TIMEOUT_LOOPS);
    if (rc == -ETIMEDOUT) {
        slot->poisoned = true;
        slot->dma_retained = true;
        return -ETIMEDOUT;
    }
    if (rc != 0) return rc;

    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (match_idx != 1) {
        log_err("[xhci] slot %u: GET_DESCRIPTOR(0x%02x) stage %u error (cc=%u)\n",
                slot->slot_id, desc_type, (unsigned)match_idx, cc);
        return xhci_ep0_cc_to_errno(cc);
    }
    if (cc != XHCI_COMP_SUCCESS && cc != XHCI_COMP_SHORT_TX) {
        log_err("[xhci] slot %u: GET_DESCRIPTOR(0x%02x) data failed (cc=%u)\n",
                slot->slot_id, desc_type, cc);
        return xhci_ep0_cc_to_errno(cc);
    }

    uint32_t residual = evt.status & 0xFFFFFF;
    if (residual > length) {
        log_err("[xhci] slot %u: GET_DESCRIPTOR(0x%02x) residual %u exceeds %u\n",
                slot->slot_id, desc_type, residual, length);
        return -EIO;
    }
    int actual = (int)(length - residual);

    /* Wait for Status Stage retirement before returning or copying data. */
    rc = xhci_poll_transfer_matched(ctrl, slot->slot_id, 1, trbs, 3, &evt,
                                    &match_idx, XHCI_TIMEOUT_LOOPS);
    if (rc == -ETIMEDOUT) {
        slot->poisoned = true;
        slot->dma_retained = true;
        return -ETIMEDOUT;
    }
    if (rc != 0) return rc;

    cc = (evt.status >> 24) & 0xFF;
    if (match_idx != 2 || cc != XHCI_COMP_SUCCESS) {
        log_err("[xhci] slot %u: GET_DESCRIPTOR(0x%02x) status failed (stage=%u, cc=%u)\n",
                slot->slot_id, desc_type, (unsigned)match_idx, cc);
        return xhci_ep0_cc_to_errno(cc);
    }

    return actual;
}

int xhci_get_config_descriptor(struct xhci_controller *ctrl, uint8_t slot_id,
                                struct usb_config_descriptor *out_desc)
{
    if (!ctrl || !ctrl->db_regs || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id])
        return -EINVAL;

    bool locked = xhci_lock(ctrl);
    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot->ep0_ring_virt || !slot->dma_buf_virt) {
        xhci_unlock(ctrl, locked);
        return -EINVAL;
    }
    if (ctrl->poisoned || slot->poisoned || slot->disabled) {
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    /* Stage 1: the 9-byte header carries wTotalLength of the whole
     * configuration descriptor hierarchy. */
    int rc = xhci_ep0_get_descriptor(ctrl, slot, USB_DESC_TYPE_CONFIGURATION, 9);
    if (rc < 0) {
        xhci_unlock(ctrl, locked);
        return rc;
    }
    if (rc < (int)sizeof(struct usb_config_descriptor)) {
        log_err("[xhci] slot %u: short configuration descriptor header (%d bytes)\n",
                slot_id, rc);
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    struct usb_config_descriptor temp_cfg;
    memcpy(&temp_cfg, slot->dma_buf_virt, sizeof(temp_cfg));

    if (temp_cfg.bLength != sizeof(struct usb_config_descriptor) ||
        temp_cfg.bDescriptorType != USB_DESC_TYPE_CONFIGURATION) {
        log_err("[xhci] slot %u: invalid configuration descriptor header "
                "(len=%u type=%u)\n", slot_id, temp_cfg.bLength, temp_cfg.bDescriptorType);
        xhci_unlock(ctrl, locked);
        return -EIO;
    }
    if (temp_cfg.wTotalLength < sizeof(struct usb_config_descriptor) ||
        temp_cfg.wTotalLength > 4096) {
        log_err("[xhci] slot %u: configuration descriptor wTotalLength out of range (%u)\n",
                slot_id, temp_cfg.wTotalLength);
        xhci_unlock(ctrl, locked);
        return -EINVAL;
    }

    /* Stage 2: read the whole hierarchy into the DMA buffer. */
    rc = xhci_ep0_get_descriptor(ctrl, slot, USB_DESC_TYPE_CONFIGURATION,
                                 temp_cfg.wTotalLength);
    if (rc < 0) {
        xhci_unlock(ctrl, locked);
        return rc;
    }

    uint32_t actual = (uint32_t)rc;
    if (actual < sizeof(struct usb_config_descriptor)) {
        log_err("[xhci] slot %u: short configuration descriptor body (%u bytes)\n",
                slot_id, actual);
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    /* Only the bytes the device really sent are valid. */
    uint16_t parse_len = (actual < temp_cfg.wTotalLength) ? (uint16_t)actual
                                                          : temp_cfg.wTotalLength;

    slot->config_desc = temp_cfg;
    if (out_desc) *out_desc = temp_cfg;

    log_info("[xhci] slot %u: config %u total_len=%u interfaces=%u\n",
             slot_id, temp_cfg.bConfigurationValue, temp_cfg.wTotalLength,
             temp_cfg.bNumInterfaces);

    xhci_unlock(ctrl, locked);
    return parse_len;
}

static void xhci_fill_input_ep_ctx(uint32_t *ep_ctx, const struct xhci_ep_ring *ring,
                                   uint64_t deq_phys, uint8_t dcs)
{
    ep_ctx[0] = (uint32_t)ring->interval << 16;                    /* Interval, MaxPStreams=0 */
    ep_ctx[1] = (3U << 1) | ((uint32_t)ring->ep_type << 3) |
                ((uint32_t)ring->max_burst << 8) |
                ((uint32_t)ring->max_packet_size << 16);          /* CErr, EP Type, MaxBurst, MPS */
    ep_ctx[2] = (uint32_t)(deq_phys & 0xFFFFFFF0ULL) | (uint32_t)(dcs & 1U); /* TR Dequeue Lo, DCS */
    ep_ctx[3] = (uint32_t)(deq_phys >> 32);                        /* TR Dequeue Hi */
    ep_ctx[4] = (uint32_t)ring->max_packet_size;                   /* Average TRB Length */
}

int xhci_configure_endpoints(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !ctrl->db_regs ||
        slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS || !ctrl->slots[slot_id])
        return -EINVAL;

    bool locked = xhci_lock(ctrl);
    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot->input_ctx_virt || !slot->input_ctx_phys) {
        xhci_unlock(ctrl, locked);
        return -EINVAL;
    }
    if (ctrl->poisoned || slot->poisoned || slot->disabled) {
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    memset(slot->input_ctx_virt, 0, 4096);

    /* The Configure Endpoint command is what moves the slot from Addressed to
     * Configured, so it is issued even when the device exposes no non-EP0
     * endpoint: in that case only the Slot Context is added.  Context
     * Entries must stay at least 1, because EP0 lives at DCI 1. */
    uint32_t context_entries = slot->max_dci ? slot->max_dci : 1;

    /* Input Control Context: Drop Flags = 0, Add Flags = Slot Context plus
     * every endpoint context that is being configured. */
    uint32_t add_flags = (1U << 0);
    if (slot->ep_rings) {
        for (uint8_t dci = XHCI_MIN_ENDPOINT_DCI; dci <= slot->max_dci; dci++) {
            if (slot->ep_rings[dci] && slot->ep_rings[dci]->active) {
                add_flags |= (1U << dci);
            }
        }
    }

    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(slot->input_ctx_virt, ctrl->csz);
    ctrl_ctx[0] = 0; /* Drop Flags */
    ctrl_ctx[1] = add_flags;

    /* Slot Context: speed, Context Entries and root hub port number. */
    uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(slot->input_ctx_virt, ctrl->csz);
    slot_ctx[0] = ((uint32_t)slot->speed << 20) | (context_entries << 27);
    slot_ctx[1] = ((uint32_t)slot->port_num << 16);

    /* Endpoint Contexts. */
    if (slot->ep_rings) {
        for (uint8_t dci = XHCI_MIN_ENDPOINT_DCI; dci <= slot->max_dci; dci++) {
            struct xhci_ep_ring *ring = slot->ep_rings[dci];
            if (!ring || !ring->active) continue;

            uint32_t *ep_ctx = (uint32_t *)xhci_input_ep_ctx(slot->input_ctx_virt, ctrl->csz, dci);
            xhci_fill_input_ep_ctx(ep_ctx, ring, ring->ring_phys, 1U);
        }
    }

    /* Configure Endpoint command (TRB type 12). */
    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.parameter = slot->input_ctx_phys;
    trb.control = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_CONFIG_EP << 10) |
                  ctrl->cmd_ring_cycle;

    struct xhci_trb evt;
    int rc = xhci_submit_cmd_locked(ctrl, &trb, &evt);
    if (rc != 0) {
        xhci_unlock(ctrl, locked);
        return rc;
    }
    if (((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        log_err("[xhci] slot %u: configure endpoint command failed (cc=%u)\n",
                slot_id, (unsigned)((evt.status >> 24) & 0xFF));
        xhci_unlock(ctrl, locked);
        return -EIO;
    }

    xhci_unlock(ctrl, locked);
    return 0;
}

static int xhci_control_no_data_locked(struct xhci_controller *ctrl, uint8_t slot_id,
                                       uint8_t type, uint8_t req, uint16_t value,
                                       uint16_t index)
{
    if (!ctrl || !ctrl->db_regs || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id])
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot->ep0_ring_virt) return -EINVAL;
    if (ctrl->poisoned || slot->poisoned || slot->disabled) return -EIO;

    /* Setup Stage: 8-byte setup packet as immediate data, TRT = 0 (No Data). */
    struct xhci_trb setup;
    memset(&setup, 0, sizeof(setup));
    setup.parameter = ((uint64_t)index << 32) |
                      ((uint64_t)value << 16) |
                      ((uint64_t)req << 8) |
                      (uint64_t)type;
    setup.status = 8;
    setup.control = (0U << 16) | XHCI_TRB_C_IDT | (XHCI_TRB_TYPE_SETUP_STAGE << 10);

    /* Status Stage: DIR = 1 (IN), IOC = 1. */
    struct xhci_trb status;
    memset(&status, 0, sizeof(status));
    status.control = XHCI_TRB_C_DIR | XHCI_TRB_C_IOC | (XHCI_TRB_TYPE_STATUS_STAGE << 10);

    uint64_t trbs[2] = { 0, 0 };
    int rc = xhci_ring_enqueue_trb(slot->ep0_ring_virt, slot->ep0_ring_phys,
                                   &slot->ep0_ring_enqueue, &slot->ep0_ring_cycle,
                                   &setup, &trbs[0]);
    if (rc != 0) return rc;
    rc = xhci_ring_enqueue_trb(slot->ep0_ring_virt, slot->ep0_ring_phys,
                               &slot->ep0_ring_enqueue, &slot->ep0_ring_cycle,
                               &status, &trbs[1]);
    if (rc != 0) return rc;

    /* Ring the slot doorbell: Target = 1 (EP0). */
    xhci_dma_wmb();
    xhci_write32(ctrl->db_regs + slot->slot_id * 4, 1);

    struct xhci_trb evt;
    size_t match_idx = 0;
    rc = xhci_poll_transfer_matched(ctrl, slot->slot_id, 1, trbs, 2, &evt,
                                    &match_idx, XHCI_TIMEOUT_LOOPS);
    if (rc == -ETIMEDOUT) {
        slot->poisoned = true;
        slot->dma_retained = true;
        return -ETIMEDOUT;
    }
    if (rc != 0) return rc;

    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (match_idx != 1 || cc != XHCI_COMP_SUCCESS) {
        log_err("[xhci] slot %u: control no-data (req=0x%02x) failed (stage=%u, cc=%u)\n",
                slot_id, req, (unsigned)match_idx, cc);
        return xhci_ep0_cc_to_errno(cc);
    }

    return 0;
}

int xhci_control_no_data(struct xhci_controller *c, uint8_t slot, uint8_t type,
                         uint8_t req, uint16_t value, uint16_t index)
{
    if (!c) return -EINVAL;
    bool locked = xhci_lock(c);
    int rc = xhci_control_no_data_locked(c, slot, type, req, value, index);
    xhci_unlock(c, locked);
    return rc;
}

int xhci_set_configuration(struct xhci_controller *ctrl, uint8_t slot_id, uint8_t config_value)
{
    if (!ctrl) return -EINVAL;
    bool locked = xhci_lock(ctrl);

    int rc = xhci_control_no_data_locked(ctrl, slot_id, 0x00, 0x09, config_value, 0);
    if (rc != 0) {
        xhci_unlock(ctrl, locked);
        return rc;
    }

    struct xhci_slot *slot = ctrl->slots[slot_id];
    slot->configured = true;
    if (xhci_msc_rings_valid(slot, &slot->msc_candidate)) {
        slot->msc_iface = slot->msc_candidate;
    } else {
        memset(&slot->msc_iface, 0, sizeof(slot->msc_iface));
    }

    log_info("[xhci] slot %u: configured with config %u (iface class %02x:%02x, ep_count=%u)\n",
             slot_id, config_value, slot->iface.class_code, slot->iface.subclass_code,
             slot->iface.num_endpoints);

    xhci_unlock(ctrl, locked);
    return 0;
}

int xhci_bulk_transfer(struct xhci_controller *c, uint8_t slot_id, uint8_t dci,
                       void *cpu_buf, size_t len, size_t *actual)
{
    if (actual) *actual = 0;
    if (!c || !c->db_regs || !cpu_buf || len == 0 || len > 65536) return -EINVAL;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) return -EINVAL;
    if (dci < XHCI_MIN_ENDPOINT_DCI || dci > XHCI_MAX_ENDPOINT_DCI) return -EINVAL;

    bool locked = xhci_lock(c);
    if (c->poisoned) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    struct xhci_slot *slot = c->slots[slot_id];
    if (!slot || !slot->active || slot->poisoned || slot->disabled ||
        !slot->configured || slot->pending_bulk_dci != 0) {
        xhci_unlock(c, locked);
        return -EIO;
    }
    if (!slot->ep_rings || dci > slot->max_dci ||
        !slot->ep_rings[dci] || !slot->ep_rings[dci]->active) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }

    struct xhci_ep_ring *ring = slot->ep_rings[dci];
    if (ring->ep_type != XHCI_EP_TYPE_BULK_IN && ring->ep_type != XHCI_EP_TYPE_BULK_OUT) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }
    bool is_in = (ring->ep_type == XHCI_EP_TYPE_BULK_IN);

    if (!slot->bulk_staging_virt) {
        int alloc_rc = xhci_alloc_64k_staging(slot);
        if (alloc_rc != 0) {
            xhci_unlock(c, locked);
            return alloc_rc;
        }
    }

    uint64_t staging_phys = slot->bulk_staging_phys;
    if ((staging_phys & 0xFFFFULL) != 0) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }

    bool ac64 = true;
    if (c->mmio_virt) {
        ac64 = (xhci_read32(c->mmio_virt + XHCI_CAP_HCCPARAMS1) & 1U) != 0;
    } else if (c->ac64_valid) {
        ac64 = c->ac64;
    }
    if (!ac64 && (staging_phys > 0xFFFFFFFFULL || staging_phys + 65535ULL > 0xFFFFFFFFULL)) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    if (!is_in) {
        memcpy(slot->bulk_staging_virt, cpu_buf, len);
    }

    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.parameter = staging_phys;
    trb.status = (uint32_t)len; /* TD Size=0, Interrupter Target=0 */
    trb.control = (XHCI_TRB_TYPE_NORMAL << 10) | XHCI_TRB_C_IOC |
                  (is_in ? XHCI_TRB_C_ISP : 0);

    uint64_t trb_phys = 0;
    int rc = xhci_ring_enqueue_trb(ring->ring_virt, ring->ring_phys,
                                   &ring->enqueue, &ring->cycle,
                                   &trb, &trb_phys);
    if (rc != 0) {
        xhci_unlock(c, locked);
        return rc;
    }

    xhci_dma_wmb();
    xhci_write32(c->db_regs + slot_id * 4, dci);

    struct xhci_trb evt;
    rc = xhci_poll_transfer_matched(c, slot_id, dci, &trb_phys, 1, &evt,
                                    NULL, XHCI_TIMEOUT_LOOPS);
    if (rc == -ETIMEDOUT) {
        slot->pending_bulk_dci = dci;
        slot->pending_bulk_trb_phys = trb_phys;
        slot->poisoned = true;
        slot->dma_retained = true;
        xhci_unlock(c, locked);
        return -ETIMEDOUT;
    }
    if (rc != 0) {
        xhci_unlock(c, locked);
        return rc;
    }

    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (cc == XHCI_COMP_STALL) {
        xhci_unlock(c, locked);
        return -EPIPE;
    }
    if (cc != XHCI_COMP_SUCCESS && !(is_in && cc == XHCI_COMP_SHORT_TX)) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    uint32_t residual = evt.status & 0xFFFFFF;
    if (residual > len) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    size_t xferred = len - residual;
    if (!is_in && xferred != len) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    if (is_in && xferred > 0) {
        memcpy(cpu_buf, slot->bulk_staging_virt, xferred);
    }
    if (actual) *actual = xferred;

    xhci_unlock(c, locked);
    return 0;
}

static void xhci_ring_normalize_link(struct xhci_ep_ring *ring)
{
    if (ring->enqueue >= XHCI_TRB_RING_SIZE - 1) {
        struct xhci_trb *link = &ring->ring_virt[XHCI_TRB_RING_SIZE - 1];
        link->control &= ~(1U << 4); /* CH=0 */
        if (ring->cycle) {
            link->control |= XHCI_TRB_CYCLE_BIT;
        } else {
            link->control &= ~XHCI_TRB_CYCLE_BIT;
        }
        xhci_dma_wmb();
        ring->cycle ^= 1;
        ring->enqueue = 0;
    }
}

static int xhci_submit_ep_cmd_locked(struct xhci_controller *ctrl, uint32_t type,
                                     uint8_t slot_id, uint8_t dci, uint64_t param)
{
    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.parameter = param;
    trb.control = ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16) |
                  (type << 10) | ctrl->cmd_ring_cycle;

    struct xhci_trb evt;
    int rc = xhci_submit_cmd_locked(ctrl, &trb, &evt);
    if (rc != 0) return rc;
    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (cc != XHCI_COMP_SUCCESS) {
        log_err("[xhci] slot %u dci %u cmd type %u failed (cc=%u)\n",
                slot_id, dci, type, cc);
        return -EIO;
    }
    return 0;
}

int xhci_clear_halt(struct xhci_controller *c, uint8_t slot_id, uint8_t ep_addr)
{
    if (!c || !c->db_regs || !c->cmd_ring_virt) return -EINVAL;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) return -EINVAL;

    uint8_t ep_num = ep_addr & 0x0F;
    uint8_t is_in = (ep_addr & 0x80) != 0;
    if (ep_num == 0) return -EINVAL;
    uint8_t dci = (uint8_t)(ep_num * 2 + (is_in ? 1 : 0));
    if (dci < XHCI_MIN_ENDPOINT_DCI || dci > XHCI_MAX_ENDPOINT_DCI) return -EINVAL;

    bool locked = xhci_lock(c);
    if (c->poisoned) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    struct xhci_slot *slot = c->slots[slot_id];
    if (!slot || !slot->active || slot->poisoned || slot->disabled ||
        !slot->configured || !slot->dev_ctx_virt || !slot->input_ctx_virt) {
        xhci_unlock(c, locked);
        return -EIO;
    }
    if (!slot->ep_rings || dci > slot->max_dci ||
        !slot->ep_rings[dci] || !slot->ep_rings[dci]->active) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }

    struct xhci_ep_ring *ring = slot->ep_rings[dci];
    if (ring->ep_type != XHCI_EP_TYPE_BULK_IN && ring->ep_type != XHCI_EP_TYPE_BULK_OUT) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }

    volatile uint32_t *out_ep_ctx =
        (volatile uint32_t *)xhci_dev_ep_ctx(slot->dev_ctx_virt, c->csz, dci);
    uint32_t ep_state = out_ep_ctx[0] & 0x7U;

    xhci_ring_normalize_link(ring);
    uint64_t next_deq = ring->ring_phys + (uint64_t)ring->enqueue * sizeof(struct xhci_trb);
    uint8_t next_dcs = ring->cycle & 1U;

    int rc;
    if (ep_state == XHCI_EP_STATE_HALTED) {
        rc = xhci_submit_ep_cmd_locked(c, XHCI_TRB_TYPE_RESET_EP, slot_id, dci, 0);
        if (rc != 0) goto err_quarantine;

        rc = xhci_control_no_data_locked(c, slot_id, 0x02, 0x01, 0, ep_addr);
        if (rc != 0) goto err_quarantine;

        rc = xhci_submit_ep_cmd_locked(c, XHCI_TRB_TYPE_SET_TR_DEQ, slot_id, dci,
                                       (next_deq & ~0xFULL) | next_dcs);
        if (rc != 0) goto err_quarantine;
    } else if (ep_state == XHCI_EP_STATE_RUNNING || ep_state == XHCI_EP_STATE_STOPPED) {
        rc = xhci_control_no_data_locked(c, slot_id, 0x02, 0x01, 0, ep_addr);
        if (rc != 0) goto err_quarantine;

        memset(slot->input_ctx_virt, 0, 4096);
        uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(slot->input_ctx_virt, c->csz);
        ctrl_ctx[0] = (1U << dci);
        ctrl_ctx[1] = (1U << 0) | (1U << dci);

        volatile uint32_t *out_slot_ctx =
            (volatile uint32_t *)xhci_dev_slot_ctx(slot->dev_ctx_virt, c->csz);
        uint32_t ctx_entries = (out_slot_ctx[0] >> 27) & 0x1FU;
        if (ctx_entries < dci) {
            ctx_entries = (slot->max_dci >= dci) ? slot->max_dci : dci;
        }

        uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(slot->input_ctx_virt, c->csz);
        slot_ctx[0] = ((uint32_t)slot->speed << 20) | (ctx_entries << 27);
        slot_ctx[1] = ((uint32_t)slot->port_num << 16);

        uint32_t *ep_ctx = (uint32_t *)xhci_input_ep_ctx(slot->input_ctx_virt, c->csz, dci);
        xhci_fill_input_ep_ctx(ep_ctx, ring, next_deq, next_dcs);

        rc = xhci_submit_ep_cmd_locked(c, XHCI_TRB_TYPE_CONFIG_EP, slot_id, 0,
                                       slot->input_ctx_phys);
        if (rc != 0) goto err_quarantine;
    } else {
        rc = -EIO;
        goto err_quarantine;
    }

    xhci_unlock(c, locked);
    return 0;

err_quarantine:
    slot->poisoned = true;
    slot->dma_retained = true;
    xhci_unlock(c, locked);
    return rc;
}

int xhci_recover_bulk_timeout(struct xhci_controller *c, uint8_t slot_id, uint8_t dci)
{
    if (!c || !c->db_regs || !c->cmd_ring_virt || !c->event_ring_virt || !c->rt_regs)
        return -EINVAL;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS)
        return -EINVAL;
    if (dci < XHCI_MIN_ENDPOINT_DCI || dci > XHCI_MAX_ENDPOINT_DCI)
        return -EINVAL;

    bool locked = xhci_lock(c);
    if (c->poisoned) {
        xhci_unlock(c, locked);
        return -EIO;
    }

    struct xhci_slot *slot = c->slots[slot_id];
    if (!slot || !slot->active || slot->disabled || !slot->configured ||
        !slot->dev_ctx_virt || slot->pending_bulk_dci != dci ||
        slot->pending_bulk_trb_phys == 0) {
        xhci_unlock(c, locked);
        return -EIO;
    }
    if (!slot->ep_rings || dci > slot->max_dci ||
        !slot->ep_rings[dci] || !slot->ep_rings[dci]->active) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }

    struct xhci_ep_ring *ring = slot->ep_rings[dci];
    if (ring->ep_type != XHCI_EP_TYPE_BULK_IN && ring->ep_type != XHCI_EP_TYPE_BULK_OUT) {
        xhci_unlock(c, locked);
        return -EINVAL;
    }

    uint64_t expect_cmd_trb = c->cmd_ring_phys +
                              (uint64_t)c->cmd_ring_enqueue * sizeof(struct xhci_trb);
    struct xhci_trb stop_trb;
    memset(&stop_trb, 0, sizeof(stop_trb));
    stop_trb.control = ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16) |
                       (XHCI_TRB_TYPE_STOP_EP << 10) | c->cmd_ring_cycle;

    int rc = xhci_cmd_ring_enqueue(c, &stop_trb);
    if (rc != 0) {
        slot->poisoned = true;
        slot->dma_retained = true;
        xhci_unlock(c, locked);
        return rc;
    }
    xhci_dma_wmb();
    xhci_write32(c->db_regs, 0);

    bool saw_stopped = false;
    bool got_cmd = false;
    struct xhci_trb cmd_evt;
    memset(&cmd_evt, 0, sizeof(cmd_evt));

    for (uint32_t i = 0; i < XHCI_TIMEOUT_LOOPS; i++) {
        volatile struct xhci_trb *trb = &c->event_ring_virt[c->event_ring_dequeue];
        uint32_t ctrl_word = *(volatile uint32_t *)&trb->control;
        if ((ctrl_word & XHCI_TRB_CYCLE_BIT) != c->event_ring_cycle)
            continue;

        struct xhci_trb evt = *trb;
        c->event_ring_dequeue = (c->event_ring_dequeue + 1) % XHCI_TRB_RING_SIZE;
        if (c->event_ring_dequeue == 0)
            c->event_ring_cycle ^= 1;

        uint64_t erdp = c->event_ring_phys + c->event_ring_dequeue * sizeof(struct xhci_trb);
        xhci_write64(c->rt_regs + 0x20 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);

        uint32_t evt_type = (evt.control >> 10) & 0x3F;
        if (evt_type == XHCI_TRB_TYPE_TRANSFER_EVT) {
            uint8_t evt_slot = (evt.control >> 24) & 0xFF;
            uint8_t evt_dci = (evt.control >> 16) & 0x1F;
            uint32_t evt_cc = (evt.status >> 24) & 0xFF;
            if (evt_slot == slot_id && evt_dci == dci &&
                (evt_cc == XHCI_COMP_STOPPED ||
                 evt_cc == XHCI_COMP_STOPPED_LENGTH_INVALID ||
                 evt_cc == XHCI_COMP_STOPPED_SHORT_PACKET)) {
                saw_stopped = true;
            }
        } else if (evt_type == XHCI_TRB_TYPE_CMD_CMPL_EVT) {
            if (evt.parameter == expect_cmd_trb) {
                cmd_evt = evt;
                got_cmd = true;
                break;
            }
        }
    }

    if (!got_cmd) {
        c->poisoned = true;
        c->dma_retained = true;
        slot->poisoned = true;
        slot->dma_retained = true;
        xhci_unlock(c, locked);
        return -ETIMEDOUT;
    }

    uint32_t cmd_cc = (cmd_evt.status >> 24) & 0xFF;
    if (cmd_cc == XHCI_COMP_SUCCESS) {
        if (!saw_stopped) {
            slot->poisoned = true;
            slot->dma_retained = true;
            xhci_unlock(c, locked);
            return -EIO;
        }
        xhci_ring_normalize_link(ring);
        uint64_t next_deq = ring->ring_phys + (uint64_t)ring->enqueue * sizeof(struct xhci_trb);
        uint8_t next_dcs = ring->cycle & 1U;
        rc = xhci_submit_ep_cmd_locked(c, XHCI_TRB_TYPE_SET_TR_DEQ, slot_id, dci,
                                       (next_deq & ~0xFULL) | next_dcs);
        if (rc != 0)
            goto err_quarantine;
    } else if (cmd_cc == XHCI_COMP_CONTEXT_STATE_ERR) {
        volatile uint32_t *out_ep_ctx =
            (volatile uint32_t *)xhci_dev_ep_ctx(slot->dev_ctx_virt, c->csz, dci);
        uint32_t ep_state = out_ep_ctx[0] & 0x7U;
        if (ep_state != XHCI_EP_STATE_HALTED) {
            rc = -EIO;
            goto err_quarantine;
        }
        rc = xhci_submit_ep_cmd_locked(c, XHCI_TRB_TYPE_RESET_EP, slot_id, dci, 0);
        if (rc != 0)
            goto err_quarantine;

        uint8_t ep_addr = (uint8_t)((dci >> 1) |
                                    ((ring->ep_type == XHCI_EP_TYPE_BULK_IN) ? 0x80u : 0x00u));
        slot->poisoned = false;
        rc = xhci_control_no_data_locked(c, slot_id, 0x02, 0x01, 0, ep_addr);
        if (rc != 0)
            goto err_quarantine;

        xhci_ring_normalize_link(ring);
        uint64_t next_deq = ring->ring_phys + (uint64_t)ring->enqueue * sizeof(struct xhci_trb);
        uint8_t next_dcs = ring->cycle & 1U;
        rc = xhci_submit_ep_cmd_locked(c, XHCI_TRB_TYPE_SET_TR_DEQ, slot_id, dci,
                                       (next_deq & ~0xFULL) | next_dcs);
        if (rc != 0)
            goto err_quarantine;
    } else {
        rc = -EIO;
        goto err_quarantine;
    }

    slot->pending_bulk_dci = 0;
    slot->pending_bulk_trb_phys = 0;
    slot->poisoned = false;
    slot->dma_retained = false;
    xhci_unlock(c, locked);
    return 0;

err_quarantine:
    slot->poisoned = true;
    slot->dma_retained = true;
    xhci_unlock(c, locked);
    return rc;
}

void xhci_probe_ports(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->op_regs) return;

    for (uint32_t p = 1; p <= ctrl->max_ports; p++) {
        uint8_t speed = 0;
        if (xhci_port_reset(ctrl, p, &speed) != 0) continue;

        uint8_t slot_id = 0;
        if (xhci_enable_slot(ctrl, (uint8_t)p, speed, &slot_id) != 0) continue;

        if (xhci_address_device(ctrl, slot_id) != 0) {
            xhci_free_slot(ctrl, slot_id);
            continue;
        }

        struct usb_device_descriptor desc;
        if (xhci_get_device_descriptor(ctrl, slot_id, &desc) != 0) {
            xhci_free_slot(ctrl, slot_id);
            continue;
        }

        /* Phase 3: read the configuration descriptor hierarchy, build the
         * endpoint rings, program the endpoint contexts and move the device
         * into the Configured state. */
        struct usb_config_descriptor cfg;
        int rc = xhci_get_config_descriptor(ctrl, slot_id, &cfg);
        if (rc <= 0) {
            xhci_free_slot(ctrl, slot_id);
            continue;
        }
        uint16_t cfg_actual_len = (uint16_t)rc;

        struct xhci_slot *slot = ctrl->slots[slot_id];
        rc = xhci_setup_endpoint_rings(slot, (const uint8_t *)slot->dma_buf_virt,
                                       cfg_actual_len);
        if (rc != 0) {
            xhci_free_slot(ctrl, slot_id);
            continue;
        }

        rc = xhci_configure_endpoints(ctrl, slot_id);
        if (rc != 0) {
            xhci_free_slot(ctrl, slot_id);
            continue;
        }

        rc = xhci_set_configuration(ctrl, slot_id, cfg.bConfigurationValue);
        if (rc != 0) {
            xhci_free_slot(ctrl, slot_id);
            continue;
        }

        /* Phase 4: Cache concatenated binary descriptors (Device + Config hierarchy) */
        uint16_t total_len = (uint16_t)(sizeof(struct usb_device_descriptor) + cfg_actual_len);
        slot->raw_descriptors = kmalloc(total_len);
        if (slot->raw_descriptors) {
            memcpy(slot->raw_descriptors, &slot->desc, sizeof(struct usb_device_descriptor));
            memcpy(slot->raw_descriptors + sizeof(struct usb_device_descriptor),
                   slot->dma_buf_virt, cfg_actual_len);
            slot->raw_descriptors_len = total_len;
        }
    }
}

int xhci_cmd_ring_enqueue(struct xhci_controller *ctrl, const struct xhci_trb *trb)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !trb) return -EINVAL;
    if (ctrl->cmd_ring_enqueue >= 255) return -EINVAL;

    struct xhci_trb *slot = &ctrl->cmd_ring_virt[ctrl->cmd_ring_enqueue];
    slot->parameter = trb->parameter;
    slot->status = trb->status;
    slot->control = (trb->control & ~XHCI_TRB_CYCLE_BIT) | (ctrl->cmd_ring_cycle ? XHCI_TRB_CYCLE_BIT : 0);

    ctrl->cmd_ring_enqueue++;
    if (ctrl->cmd_ring_enqueue == 255) {
        /* Update Link TRB cycle bit and wrap to 0 */
        struct xhci_trb *link = &ctrl->cmd_ring_virt[255];
        link->control = (link->control & ~XHCI_TRB_CYCLE_BIT) | (ctrl->cmd_ring_cycle ? XHCI_TRB_CYCLE_BIT : 0);
        ctrl->cmd_ring_cycle ^= 1;
        ctrl->cmd_ring_enqueue = 0;
    }

    return 0;
}

void xhci_free_dma(struct xhci_controller *ctrl)
{
    if (!ctrl) return;
    if ((ctrl->dma_retained || ctrl->poisoned) && !ctrl->halted_confirmed) {
        log_warn("[xhci] controller halt unproven; retaining controller DMA\n");
        return;
    }
    if (ctrl->erst_phys) {
        free_4k_page(ctrl->erst_phys);
        ctrl->erst_phys = 0;
        ctrl->erst_virt = NULL;
    }
    if (ctrl->event_ring_phys) {
        free_4k_page(ctrl->event_ring_phys);
        ctrl->event_ring_phys = 0;
        ctrl->event_ring_virt = NULL;
    }
    if (ctrl->cmd_ring_phys) {
        free_4k_page(ctrl->cmd_ring_phys);
        ctrl->cmd_ring_phys = 0;
        ctrl->cmd_ring_virt = NULL;
    }
    if (ctrl->dcbaa_phys) {
        free_4k_page(ctrl->dcbaa_phys);
        ctrl->dcbaa_phys = 0;
        ctrl->dcbaa_virt = NULL;
    }
}

int xhci_start(struct xhci_controller *ctrl)
{
    if (!ctrl || !ctrl->op_regs) return -EINVAL;

    volatile uint8_t *op = ctrl->op_regs;

    /* 1. Set MaxSlotsEn */
    uint32_t max_slots_en = ctrl->max_slots;
    if (max_slots_en > 16) max_slots_en = 16;
    xhci_write32(op + XHCI_OP_CONFIG, max_slots_en);

    /* 2. Program DCBAAP */
    xhci_write64(op + XHCI_OP_DCBAAP, ctrl->dcbaa_phys);

    /* 3. Program CRCR with initial RCS = 1 */
    xhci_write64(op + XHCI_OP_CRCR, ctrl->cmd_ring_phys | 1);

    /* 4. Start controller: write USBCMD.RS = 1 */
    uint32_t cmd = xhci_read32(op + XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_RS;
    cmd &= ~XHCI_CMD_INTE; /* Ensure interrupts masked in polling mode */
    xhci_write32(op + XHCI_OP_USBCMD, cmd);

    /* 5. Wait for USBSTS.HCH == 0 */
    int rc = xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_HCH, false, op);
    if (rc != 0) {
        log_err("[xhci] start error: controller did not run (%d)\n", rc);
        xhci_write32(op + XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        if (xhci_wait_bit(op + XHCI_OP_USBSTS, XHCI_STS_HCH, true, op) == 0) {
            ctrl->halted_confirmed = true;
        } else {
            ctrl->halted_confirmed = false;
            ctrl->poisoned = true;
            ctrl->dma_retained = true;
        }
        return rc;
    }

    ctrl->halted_confirmed = false;
    ctrl->running = true;
    return 0;
}

int xhci_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)id;
    if (!pdev) return -EINVAL;

    /* 1. Enable Bus Master & MMIO decode */
    int rc = pci_set_bus_master(pdev, true);
    if (rc != 0) {
        log_err("[xhci] failed to enable PCI bus master (%d)\n", rc);
        return rc;
    }

    rc = pci_set_decode(pdev, false, true);
    if (rc != 0) {
        log_err("[xhci] failed to enable PCI memory decode (%d)\n", rc);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 2. Query BAR0 window (try MMIO32, fallback to MMIO64) */
    uint64_t mmio_phys = 0;
    rc = pci_bar_window(pdev, 0, PCI_BAR_MMIO32, 0, 0x1000, &mmio_phys);
    if (rc != 0) {
        rc = pci_bar_window(pdev, 0, PCI_BAR_MMIO64, 0, 0x1000, &mmio_phys);
    }
    if (rc != 0) {
        log_err("[xhci] failed to query BAR0 MMIO window (%d)\n", rc);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    struct xhci_controller *ctrl = (struct xhci_controller *)kmalloc(sizeof(struct xhci_controller));
    if (!ctrl) {
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return -ENOMEM;
    }
    memset(ctrl, 0, sizeof(*ctrl));

    mutex_init(&ctrl->io_lock);
    ctrl->io_lock_init = true;

    ctrl->pdev = pdev;
    ctrl->mmio_phys = mmio_phys;
    ctrl->mmio_len = 0x1000;

#ifndef OS01_HOST_TEST
    uint64_t bar_page = mmio_phys & PAGE_2M_MASK;
    x86_vmm_map_2m(kernel_map, bar_page, (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_PMD_NOCACHE);
    flush_tlb();
#endif

    ctrl->mmio_virt = (volatile uint8_t *)Phy_To_Virt(mmio_phys);

    /* 3. Parse capabilities */
    rc = xhci_parse_capabilities(ctrl);
    if (rc != 0) {
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 4. Reset controller */
    rc = xhci_reset(ctrl);
    if (rc != 0) {
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 5. Allocate DMA rings */
    rc = xhci_alloc_dma(ctrl);
    if (rc != 0) {
        xhci_free_dma(ctrl);
        kfree(ctrl);
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    /* 6. Setup Interrupter 0 */
    xhci_setup_interrupter0(ctrl);

    /* 7. Start controller */
    rc = xhci_start(ctrl);
    if (rc != 0) {
        if (!ctrl->dma_retained) {
            xhci_free_dma(ctrl);
            kfree(ctrl);
        }
        pci_set_decode(pdev, false, false);
        pci_set_bus_master(pdev, false);
        return rc;
    }

    pdev->driver_data = ctrl;
    if (s_xhci_controller_count < XHCI_MAX_CONTROLLERS) {
        s_xhci_controllers[s_xhci_controller_count++] = ctrl;
    } else {
        log_warn("[xhci] controller limit (%u) reached\n", XHCI_MAX_CONTROLLERS);
    }
    log_info("[xhci] controller initialized and running (slots=%u, ports=%u)\n",
             ctrl->max_slots, ctrl->max_ports);

    xhci_probe_ports(ctrl);

    return 0;
}

void xhci_remove(struct pci_device *pdev)
{
    if (!pdev || !pdev->driver_data) return;

    struct xhci_controller *ctrl = (struct xhci_controller *)pdev->driver_data;

    /* Stop controller */
    bool halt_ok = false;
    if (ctrl->op_regs) {
        uint32_t cmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
        xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        halt_ok = (xhci_wait_bit(ctrl->op_regs + XHCI_OP_USBSTS, XHCI_STS_HCH, true, ctrl->op_regs) == 0);
    }
    ctrl->running = false;
    ctrl->halted_confirmed = halt_ok;
    if (!halt_ok) {
        ctrl->poisoned = true;
        ctrl->dma_retained = true;
    }

    /* Tear down any enumerated device slots */
    for (uint8_t s = 1; s <= XHCI_MAX_MANAGED_SLOTS; s++) {
        xhci_free_slot(ctrl, s);
    }

    /* Free all DMA buffers (no-op if halt_ok is false and dma_retained is true) */
    xhci_free_dma(ctrl);

    /* Unregister controller from global tracking */
    for (uint8_t i = 0; i < s_xhci_controller_count; i++) {
        if (s_xhci_controllers[i] == ctrl) {
            for (uint8_t j = i; j + 1 < s_xhci_controller_count; j++) {
                s_xhci_controllers[j] = s_xhci_controllers[j + 1];
            }
            s_xhci_controller_count--;
            break;
        }
    }

    /* Restore PCI configuration */
    pci_set_decode(pdev, false, false);
    pci_set_bus_master(pdev, false);

    if (halt_ok) {
        kfree(ctrl);
    }
    pdev->driver_data = NULL;
}

static const struct pci_device_id xhci_ids[] = {
    {
        .vendor = PCI_ID_ANY,
        .device = PCI_ID_ANY,
        .subvendor = PCI_ID_ANY,
        .subdevice = PCI_ID_ANY,
        .class_value = (0x0C << 16) | (0x03 << 8) | 0x30,
        .class_mask  = 0xFFFFFF,
    },
};

const struct pci_driver xhci_pci_driver = {
    .name     = "xhci",
    .id_table = xhci_ids,
    .id_count = sizeof(xhci_ids) / sizeof(xhci_ids[0]),
    .probe    = xhci_probe,
    .remove   = xhci_remove,
};

PCI_DRIVER_DECLARE(xhci_pci_driver);
