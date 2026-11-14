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
#endif

#define XHCI_TIMEOUT_LOOPS 500000

#define XHCI_MAX_CONTROLLERS 4
static struct xhci_controller *s_xhci_controllers[XHCI_MAX_CONTROLLERS];
static uint8_t s_xhci_controller_count = 0;

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

    ctrl->csz = (xhci_read32(ctrl->mmio_virt + XHCI_CAP_HCCPARAMS1) >> 2) & 1;

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
    return xhci_poll_event_internal(ctrl, target_type, 0, 0, out_evt, timeout_loops);
}

int xhci_poll_cmd_completion(struct xhci_controller *ctrl, uint64_t expect_trb_phys,
                             struct xhci_trb *out_evt, uint32_t timeout_loops)
{
    return xhci_poll_event_internal(ctrl, XHCI_TRB_TYPE_CMD_CMPL_EVT, expect_trb_phys, 0,
                                    out_evt, timeout_loops);
}

int xhci_poll_transfer(struct xhci_controller *ctrl, uint8_t slot_id,
                       struct xhci_trb *out_evt, uint32_t timeout_loops)
{
    return xhci_poll_event_internal(ctrl, XHCI_TRB_TYPE_TRANSFER_EVT, 0, slot_id,
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

int xhci_enable_slot(struct xhci_controller *ctrl, uint8_t port_num, uint8_t speed,
                     uint8_t *out_slot_id)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !ctrl->db_regs) return -EINVAL;

    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_TYPE_ENABLE_SLOT << 10) | ctrl->cmd_ring_cycle;

    /* Command Completion events echo the command TRB's physical address:
     * match on it so a completion left by an earlier timed-out command
     * cannot be misattributed to this one. */
    uint64_t expect_trb = ctrl->cmd_ring_phys +
                          (uint64_t)ctrl->cmd_ring_enqueue * sizeof(struct xhci_trb);

    int rc = xhci_cmd_ring_enqueue(ctrl, &trb);
    if (rc != 0) return rc;
    xhci_write32(ctrl->db_regs, 0);

    struct xhci_trb evt;
    rc = xhci_poll_cmd_completion(ctrl, expect_trb, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc != 0) return rc;
    if (((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        log_err("[xhci] enable slot command failed (cc=%u)\n",
                (unsigned)((evt.status >> 24) & 0xFF));
        return -EIO;
    }

    uint8_t slot_id = (evt.control >> 24) & 0xFF;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) return -ERANGE;

    struct xhci_slot *slot = (struct xhci_slot *)kmalloc(sizeof(struct xhci_slot));
    if (!slot) return -ENOMEM;
    memset(slot, 0, sizeof(*slot));
    slot->slot_id = slot_id;
    slot->port_num = port_num;
    slot->speed = speed;
    slot->active = true;
    ctrl->slots[slot_id] = slot;

    log_info("[xhci] slot %u allocated (port %u)\n", slot_id, port_num);
    if (out_slot_id) *out_slot_id = slot_id;
    return 0;
}

int xhci_address_device(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || !ctrl->dcbaa_virt) return -EINVAL;
    if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS || !ctrl->slots[slot_id])
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];

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
    if (rc != 0) goto err_cmd;
    xhci_write32(ctrl->db_regs, 0);

    struct xhci_trb evt;
    rc = xhci_poll_cmd_completion(ctrl, expect_trb, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc != 0) goto err_cmd;
    if (((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        log_err("[xhci] address device command failed (cc=%u)\n",
                (unsigned)((evt.status >> 24) & 0xFF));
        goto err_cmd;
    }

    log_info("[xhci] slot %u addressed successfully\n", slot_id);
    return 0;

err_cmd:
    ctrl->dcbaa_virt[slot_id] = 0;
    /* fallthrough: release pages, keep the slot struct for the caller's
     * xhci_free_slot teardown. */
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
    return rc != 0 ? rc : -EIO;
}

void xhci_free_endpoint_rings(struct xhci_slot *slot)
{
    if (!slot || !slot->ep_rings) return;

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

void xhci_free_slot(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) return;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot) return;

    if (ctrl->dcbaa_virt) ctrl->dcbaa_virt[slot_id] = 0;

    /* Release the dynamically allocated endpoint rings before the base DMA
     * contexts: the endpoint rings are indexed by slot->max_dci. */
    xhci_free_endpoint_rings(slot);

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

    if (slot->ep0_ring_enqueue == XHCI_TRB_RING_SIZE - 1) {
        /* Enqueue pointer sits on the Link TRB: make it valid for the
         * controller, toggle the cycle, and wrap to index 0. */
        struct xhci_trb *link = &slot->ep0_ring_virt[XHCI_TRB_RING_SIZE - 1];
        if (slot->ep0_ring_cycle) {
            link->control |= XHCI_TRB_CYCLE_BIT;
        } else {
            link->control &= ~XHCI_TRB_CYCLE_BIT;
        }
        slot->ep0_ring_cycle ^= 1;
        slot->ep0_ring_enqueue = 0;
    }

    struct xhci_trb *dst = &slot->ep0_ring_virt[slot->ep0_ring_enqueue];
    dst->parameter = trb->parameter;
    dst->status = trb->status;
    dst->control = (trb->control & ~XHCI_TRB_CYCLE_BIT) |
                   (slot->ep0_ring_cycle ? XHCI_TRB_CYCLE_BIT : 0);
    slot->ep0_ring_enqueue++;

    return 0;
}

int xhci_get_device_descriptor(struct xhci_controller *ctrl, uint8_t slot_id,
                               struct usb_device_descriptor *out_desc)
{
    if (!ctrl || !ctrl->db_regs || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id] || !out_desc)
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];

    /* Setup Stage: GET_DESCRIPTOR(DEVICE), 8-byte request as immediate data. */
    struct xhci_trb setup;
    memset(&setup, 0, sizeof(setup));
    setup.parameter = 0x0012000001000680ULL;
    setup.status = 8;
    setup.control = (3U << 16) | (1U << 6) | (XHCI_TRB_TYPE_SETUP_STAGE << 10);

    /* Data Stage: IN, 18 bytes into the slot DMA buffer. */
    struct xhci_trb data;
    memset(&data, 0, sizeof(data));
    data.parameter = slot->dma_buf_phys;
    data.status = 18;
    data.control = (1U << 16) | (XHCI_TRB_TYPE_DATA_STAGE << 10);

    /* Status Stage: OUT handshake, interrupt on completion. */
    struct xhci_trb status;
    memset(&status, 0, sizeof(status));
    status.control = (1U << 5) | (XHCI_TRB_TYPE_STATUS_STAGE << 10);

    int rc = xhci_ep0_ring_enqueue(slot, &setup);
    if (rc != 0) return rc;
    rc = xhci_ep0_ring_enqueue(slot, &data);
    if (rc != 0) return rc;
    rc = xhci_ep0_ring_enqueue(slot, &status);
    if (rc != 0) return rc;

    /* Ring the slot doorbell: Target = 1 (EP0). */
    xhci_write32(ctrl->db_regs + slot_id * 4, 1);

    struct xhci_trb evt;
    rc = xhci_poll_transfer(ctrl, slot_id, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc != 0) return rc;

    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (cc != XHCI_COMP_SUCCESS && cc != XHCI_COMP_SHORT_TX) {
        log_err("[xhci] slot %u: control transfer failed (cc=%u)\n", slot_id, cc);
        return -EIO;
    }

    uint32_t residual = evt.status & 0xFFFFFF;
    uint32_t actual = (residual <= 18) ? (18 - residual) : 0;
    if (actual < 8) {
        log_err("[xhci] slot %u: short descriptor read (%u bytes)\n", slot_id, actual);
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

int xhci_parse_config_descriptor(struct xhci_slot *slot, const uint8_t *buffer,
                                  uint16_t length)
{
    if (!slot || !buffer || length < 2) return 0;

    /* 32-bit arithmetic: offset + len must not wrap around the 16-bit
     * boundary, or the bounds check below could be bypassed. */
    uint32_t offset = 0;
    uint32_t total = length;
    uint8_t max_dci = 1; /* EP0 always exists at DCI 1 */

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
            /* Only the primary interface of the active configuration. */
            if (iface->bInterfaceNumber == 0 && iface->bAlternateSetting == 0) {
                slot->iface.interface_num = iface->bInterfaceNumber;
                slot->iface.class_code = iface->bInterfaceClass;
                slot->iface.subclass_code = iface->bInterfaceSubClass;
                slot->iface.protocol = iface->bInterfaceProtocol;
                slot->iface.num_endpoints = iface->bNumEndpoints;
                slot->iface.active = true;
            }
        } else if (type == USB_DESC_TYPE_ENDPOINT) {
            if (len < sizeof(struct usb_endpoint_descriptor)) break;
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
            if (dci > max_dci) max_dci = dci;
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
    if (max_dci < XHCI_MIN_ENDPOINT_DCI) return 0; /* EP0 only: nothing to ring */

    slot->ep_rings = kmalloc(sizeof(struct xhci_ep_ring *) * (max_dci + 1));
    if (!slot->ep_rings) return -ENOMEM;
    memset(slot->ep_rings, 0, sizeof(struct xhci_ep_ring *) * (max_dci + 1));
    slot->max_dci = (uint8_t)max_dci;

    /* Second pass: instantiate one transfer ring per discovered endpoint. */
    uint32_t offset = 0;
    uint32_t total = length;
    while (offset + 2 <= total) {
        uint8_t len = buffer[offset];
        uint8_t type = buffer[offset + 1];
        if (len < 2 || offset + len > total) break;

        if (type == USB_DESC_TYPE_ENDPOINT &&
            len >= sizeof(struct usb_endpoint_descriptor)) {
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

            /* Isochronous endpoints need a bandwidth allocation that this
             * driver does not model, and their Interval field follows a
             * different encoding: leave them unconfigured instead of
             * programming an endpoint context that cannot work. */
            if ((ep->bmAttributes & 3) == 1) {
                log_warn("[xhci] slot %u: isochronous endpoint DCI %u not supported\n",
                         slot->slot_id, (unsigned)dci);
                offset += len;
                continue;
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

            switch (ep->bmAttributes & 3) {
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
            ring->max_packet_size = ep->wMaxPacketSize & 0x7FF;
            ring->active = true;

            slot->ep_rings[dci] = ring;

            log_info("[xhci] slot %u: endpoint DCI %u configured (type=%u, mps=%u, interval=%u)\n",
                     slot->slot_id, (unsigned)dci, (unsigned)ring->ep_type,
                     (unsigned)ring->max_packet_size, (unsigned)ring->interval);
        }

        offset += len;
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
static int xhci_ep0_get_descriptor(struct xhci_controller *ctrl, struct xhci_slot *slot,
                                   uint8_t desc_type, uint16_t length)
{
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

    /* Data Stage: IN, `length` bytes into the slot DMA buffer.
     *
     * The completion event is requested on the Data Stage TRB: that is the
     * TRB whose Transfer Event Length carries the residual of a short
     * transfer, so the driver can tell how many bytes the device really
     * sent.  A controller that only reported the Status Stage would always
     * announce a zero residual. */
    struct xhci_trb data;
    memset(&data, 0, sizeof(data));
    data.parameter = slot->dma_buf_phys;
    data.status = length;
    data.control = XHCI_TRB_C_DIR | XHCI_TRB_C_IOC | (XHCI_TRB_TYPE_DATA_STAGE << 10);

    /* Status Stage: OUT handshake. */
    struct xhci_trb status;
    memset(&status, 0, sizeof(status));
    status.control = (XHCI_TRB_TYPE_STATUS_STAGE << 10);

    int rc = xhci_ep0_ring_enqueue(slot, &setup);
    if (rc != 0) return rc;
    rc = xhci_ep0_ring_enqueue(slot, &data);
    if (rc != 0) return rc;
    rc = xhci_ep0_ring_enqueue(slot, &status);
    if (rc != 0) return rc;

    /* Ring the slot doorbell: Target = 1 (EP0). */
    xhci_write32(ctrl->db_regs + slot->slot_id * 4, 1);

    struct xhci_trb evt;
    rc = xhci_poll_transfer(ctrl, slot->slot_id, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc != 0) return rc;

    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (cc != XHCI_COMP_SUCCESS && cc != XHCI_COMP_SHORT_TX) {
        log_err("[xhci] slot %u: GET_DESCRIPTOR(0x%02x) failed (cc=%u)\n",
                slot->slot_id, desc_type, cc);
        return -EIO;
    }

    uint32_t residual = evt.status & 0xFFFFFF;
    if (residual > length) {
        log_err("[xhci] slot %u: GET_DESCRIPTOR(0x%02x) residual %u exceeds %u\n",
                slot->slot_id, desc_type, residual, length);
        return -EIO;
    }

    return (int)(length - residual);
}

int xhci_get_config_descriptor(struct xhci_controller *ctrl, uint8_t slot_id,
                                struct usb_config_descriptor *out_desc)
{
    if (!ctrl || !ctrl->db_regs || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id])
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot->ep0_ring_virt || !slot->dma_buf_virt) return -EINVAL;

    /* Stage 1: the 9-byte header carries wTotalLength of the whole
     * configuration descriptor hierarchy. */
    int rc = xhci_ep0_get_descriptor(ctrl, slot, USB_DESC_TYPE_CONFIGURATION, 9);
    if (rc < 0) return rc;
    if (rc < (int)sizeof(struct usb_config_descriptor)) {
        log_err("[xhci] slot %u: short configuration descriptor header (%d bytes)\n",
                slot_id, rc);
        return -EIO;
    }

    struct usb_config_descriptor temp_cfg;
    memcpy(&temp_cfg, slot->dma_buf_virt, sizeof(temp_cfg));

    if (temp_cfg.bLength != sizeof(struct usb_config_descriptor) ||
        temp_cfg.bDescriptorType != USB_DESC_TYPE_CONFIGURATION) {
        log_err("[xhci] slot %u: invalid configuration descriptor header "
                "(len=%u type=%u)\n", slot_id, temp_cfg.bLength, temp_cfg.bDescriptorType);
        return -EIO;
    }
    if (temp_cfg.wTotalLength < sizeof(struct usb_config_descriptor) ||
        temp_cfg.wTotalLength > 4096) {
        log_err("[xhci] slot %u: configuration descriptor wTotalLength out of range (%u)\n",
                slot_id, temp_cfg.wTotalLength);
        return -EINVAL;
    }

    /* Stage 2: read the whole hierarchy into the DMA buffer. */
    rc = xhci_ep0_get_descriptor(ctrl, slot, USB_DESC_TYPE_CONFIGURATION,
                                 temp_cfg.wTotalLength);
    if (rc < 0) return rc;

    uint32_t actual = (uint32_t)rc;
    if (actual < sizeof(struct usb_config_descriptor)) {
        log_err("[xhci] slot %u: short configuration descriptor body (%u bytes)\n",
                slot_id, actual);
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

    return parse_len;
}

int xhci_configure_endpoints(struct xhci_controller *ctrl, uint8_t slot_id)
{
    if (!ctrl || !ctrl->cmd_ring_virt || !ctrl->db_regs ||
        slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS || !ctrl->slots[slot_id])
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot->input_ctx_virt || !slot->input_ctx_phys) return -EINVAL;

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
            ep_ctx[0] = (uint32_t)ring->interval << 16;                    /* Interval */
            ep_ctx[1] = (3U << 1) | ((uint32_t)ring->ep_type << 3) |
                        ((uint32_t)ring->max_packet_size << 16);          /* CErr, EP Type, MPS */
            ep_ctx[2] = (uint32_t)(ring->ring_phys & 0xFFFFFFF0ULL) | 1U; /* TR Dequeue Lo, DCS=1 */
            ep_ctx[3] = (uint32_t)(ring->ring_phys >> 32);                /* TR Dequeue Hi */
            ep_ctx[4] = (uint32_t)ring->max_packet_size;                  /* Average TRB Length */
        }
    }

    /* Configure Endpoint command (TRB type 12). */
    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.parameter = slot->input_ctx_phys;
    trb.control = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_CONFIG_EP << 10) |
                  ctrl->cmd_ring_cycle;

    uint64_t expect_trb = ctrl->cmd_ring_phys +
                          (uint64_t)ctrl->cmd_ring_enqueue * sizeof(struct xhci_trb);

    int rc = xhci_cmd_ring_enqueue(ctrl, &trb);
    if (rc != 0) return rc;
    xhci_write32(ctrl->db_regs, 0);

    struct xhci_trb evt;
    rc = xhci_poll_cmd_completion(ctrl, expect_trb, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc != 0) return rc;
    if (((evt.status >> 24) & 0xFF) != XHCI_COMP_SUCCESS) {
        log_err("[xhci] slot %u: configure endpoint command failed (cc=%u)\n",
                slot_id, (unsigned)((evt.status >> 24) & 0xFF));
        return -EIO;
    }

    return 0;
}

int xhci_set_configuration(struct xhci_controller *ctrl, uint8_t slot_id, uint8_t config_value)
{
    if (!ctrl || !ctrl->db_regs || slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS ||
        !ctrl->slots[slot_id])
        return -EINVAL;

    struct xhci_slot *slot = ctrl->slots[slot_id];
    if (!slot->ep0_ring_virt) return -EINVAL;

    /* Setup Stage: SET_CONFIGURATION(config_value), no data stage.
     * Layout: wLength[63:48] wIndex[47:32] wValue[31:16]
     * bRequest[15:8] bmRequestType[7:0]. */
    struct xhci_trb setup;
    memset(&setup, 0, sizeof(setup));
    setup.parameter = ((uint64_t)config_value << 16) | (0x09ULL << 8) | 0x00ULL;
    setup.status = 8;
    setup.control = (0U << 16) | (1U << 6) | (XHCI_TRB_TYPE_SETUP_STAGE << 10);

    /* Status Stage: DIR = 1 (IN), interrupt on completion. */
    struct xhci_trb status;
    memset(&status, 0, sizeof(status));
    status.control = (1U << 16) | (1U << 5) | (XHCI_TRB_TYPE_STATUS_STAGE << 10);

    int rc = xhci_ep0_ring_enqueue(slot, &setup);
    if (rc != 0) return rc;
    rc = xhci_ep0_ring_enqueue(slot, &status);
    if (rc != 0) return rc;

    /* Ring the slot doorbell: Target = 1 (EP0). */
    xhci_write32(ctrl->db_regs + slot->slot_id * 4, 1);

    struct xhci_trb evt;
    rc = xhci_poll_transfer(ctrl, slot->slot_id, &evt, XHCI_TIMEOUT_LOOPS);
    if (rc != 0) return rc;

    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (cc != XHCI_COMP_SUCCESS) {
        log_err("[xhci] slot %u: SET_CONFIGURATION(%u) failed (cc=%u)\n",
                slot_id, config_value, cc);
        return -EIO;
    }

    slot->configured = true;

    log_info("[xhci] slot %u: configured with config %u (iface class %02x:%02x, ep_count=%u)\n",
             slot_id, config_value, slot->iface.class_code, slot->iface.subclass_code,
             slot->iface.num_endpoints);

    return 0;
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

        struct xhci_slot *slot = ctrl->slots[slot_id];
        rc = xhci_setup_endpoint_rings(slot, (const uint8_t *)slot->dma_buf_virt,
                                       (uint16_t)rc);
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
        uint16_t total_len = sizeof(struct usb_device_descriptor) + slot->config_desc.wTotalLength;
        slot->raw_descriptors = kmalloc(total_len);
        if (slot->raw_descriptors) {
            memcpy(slot->raw_descriptors, &slot->desc, sizeof(struct usb_device_descriptor));
            memcpy(slot->raw_descriptors + sizeof(struct usb_device_descriptor),
                   slot->dma_buf_virt, slot->config_desc.wTotalLength);
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
        return rc;
    }

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

    ctrl->pdev = pdev;
    ctrl->mmio_phys = mmio_phys;
    ctrl->mmio_len = 0x1000;

#ifndef OS01_HOST_TEST
    uint64_t bar_page = mmio_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, bar_page, (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_PMD_NOCACHE);
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
        xhci_free_dma(ctrl);
        kfree(ctrl);
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
    if (ctrl->op_regs) {
        uint32_t cmd = xhci_read32(ctrl->op_regs + XHCI_OP_USBCMD);
        xhci_write32(ctrl->op_regs + XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
        xhci_wait_bit(ctrl->op_regs + XHCI_OP_USBSTS, XHCI_STS_HCH, true, ctrl->op_regs);
    }
    ctrl->running = false;

    /* Tear down any enumerated device slots */
    for (uint8_t s = 1; s <= XHCI_MAX_MANAGED_SLOTS; s++) {
        xhci_free_slot(ctrl, s);
    }

    /* Free all DMA buffers */
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

    kfree(ctrl);
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
