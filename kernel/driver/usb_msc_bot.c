/* kernel/driver/usb_msc_bot.c — USB Mass Storage Bulk-Only Transport (BOT) */
#include <driver/usb_msc_bot.h>
#include <errno.h>
#include <log/log.h>
#include <string.h>

#define BOT_CBW_SIZE      31u
#define BOT_CSW_SIZE      13u
#define BOT_CBW_SIGNATURE 0x43425355u
#define BOT_CSW_SIGNATURE 0x53425355u

#define BOT_REQ_TYPE_RESET 0x21u
#define BOT_REQ_RESET      0xffu

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int bot_reset_recovery(struct usb_msc_bot *b)
{
    int rc = b->transport.control_no_data(b->transport.ctx,
                                          BOT_REQ_TYPE_RESET,
                                          BOT_REQ_RESET,
                                          0,
                                          b->interface_num);
    if (rc != 0) {
        log_err("usb_msc_bot: Mass Storage Reset failed (rc=%d)\n", rc);
        b->dead = true;
        return (rc == -ETIMEDOUT) ? -ETIMEDOUT : -EIO;
    }

    rc = b->transport.clear_halt(b->transport.ctx, b->in_addr);
    if (rc != 0) {
        log_err("usb_msc_bot: clear_halt IN 0x%02x failed (rc=%d)\n", b->in_addr, rc);
        b->dead = true;
        return (rc == -ETIMEDOUT) ? -ETIMEDOUT : -EIO;
    }

    rc = b->transport.clear_halt(b->transport.ctx, b->out_addr);
    if (rc != 0) {
        log_err("usb_msc_bot: clear_halt OUT 0x%02x failed (rc=%d)\n", b->out_addr, rc);
        b->dead = true;
        return (rc == -ETIMEDOUT) ? -ETIMEDOUT : -EIO;
    }

    return 0;
}

static int bot_handle_bulk_timeout(struct usb_msc_bot *b, uint8_t ep_addr,
                                   int attempt, bool retry_safe, bool *should_retry)
{
    *should_retry = false;
    if (attempt > 0 || !b->transport.recover_timeout) {
        b->dead = true;
        return -ETIMEDOUT;
    }

    int rec_rc = b->transport.recover_timeout(b->transport.ctx, ep_addr);
    if (rec_rc != 0) {
        b->dead = true;
        return -ETIMEDOUT;
    }

    int rst_rc = bot_reset_recovery(b);
    if (rst_rc != 0)
        return rst_rc;

    if (retry_safe)
        *should_retry = true;
    return -ETIMEDOUT;
}

static int bot_validate_args(const struct usb_msc_bot *b, const uint8_t *cdb,
                             uint8_t cdb_len, enum usb_msc_dir dir,
                             const void *buf, uint32_t len)
{
    if (!b || !cdb)
        return -EINVAL;
    if (!b->transport.control_no_data || !b->transport.bulk || !b->transport.clear_halt)
        return -EINVAL;
    if (cdb_len < 1 || cdb_len > 16)
        return -EINVAL;

    switch (dir) {
    case USB_MSC_NONE:
        if (len != 0)
            return -EINVAL;
        break;
    case USB_MSC_IN:
    case USB_MSC_OUT:
        if (len == 0 || !buf)
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int usb_msc_bot_command(struct usb_msc_bot *b, const uint8_t *cdb, uint8_t cdb_len,
                        enum usb_msc_dir dir, void *buf, uint32_t len,
                        bool retry_safe, size_t *actual)
{
    if (actual)
        *actual = 0;

    int rc = bot_validate_args(b, cdb, cdb_len, dir, buf, len);
    if (rc != 0)
        return rc;

    if (b->dead)
        return -EIO;

    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t cbw[BOT_CBW_SIZE];
        uint8_t csw[BOT_CSW_SIZE];
        uint32_t tag = ++b->next_tag;
        size_t cbw_actual = 0;
        size_t data_actual = 0;
        size_t csw_actual = 0;
        bool need_reset = false;

        memset(cbw, 0, sizeof(cbw));
        put_le32(cbw + 0, BOT_CBW_SIGNATURE);
        put_le32(cbw + 4, tag);
        put_le32(cbw + 8, len);
        cbw[12] = (dir == USB_MSC_IN) ? 0x80u : 0x00u;
        cbw[13] = 0x00u;
        cbw[14] = cdb_len;
        memcpy(cbw + 15, cdb, cdb_len);

        rc = b->transport.bulk(b->transport.ctx, b->out_addr, cbw, BOT_CBW_SIZE, &cbw_actual);
        if (rc == -ETIMEDOUT) {
            bool should_retry = false;
            int trc = bot_handle_bulk_timeout(b, b->out_addr, attempt, retry_safe, &should_retry);
            if (should_retry)
                continue;
            return trc;
        }
        if (rc != 0 || cbw_actual != BOT_CBW_SIZE) {
            need_reset = true;
            goto handle_reset;
        }

        if (dir != USB_MSC_NONE) {
            uint8_t data_ep = (dir == USB_MSC_IN) ? b->in_addr : b->out_addr;
            rc = b->transport.bulk(b->transport.ctx, data_ep, buf, len, &data_actual);
            if (rc == -ETIMEDOUT) {
                bool should_retry = false;
                int trc = bot_handle_bulk_timeout(b, data_ep, attempt, retry_safe, &should_retry);
                if (should_retry)
                    continue;
                return trc;
            }
            if (rc == -EPIPE) {
                if (data_actual > len)
                    data_actual = len;
                int clr_rc = b->transport.clear_halt(b->transport.ctx, data_ep);
                if (clr_rc != 0) {
                    b->dead = true;
                    return (clr_rc == -ETIMEDOUT) ? -ETIMEDOUT : -EIO;
                }
            } else if (rc != 0) {
                need_reset = true;
                goto handle_reset;
            } else {
                if (data_actual > len)
                    data_actual = len;
            }
        }

        rc = b->transport.bulk(b->transport.ctx, b->in_addr, csw, BOT_CSW_SIZE, &csw_actual);
        if (rc == -ETIMEDOUT) {
            bool should_retry = false;
            int trc = bot_handle_bulk_timeout(b, b->in_addr, attempt, retry_safe, &should_retry);
            if (should_retry)
                continue;
            return trc;
        }
        if (rc == -EPIPE) {
            int clr_rc = b->transport.clear_halt(b->transport.ctx, b->in_addr);
            if (clr_rc != 0) {
                b->dead = true;
                return (clr_rc == -ETIMEDOUT) ? -ETIMEDOUT : -EIO;
            }
            csw_actual = 0;
            rc = b->transport.bulk(b->transport.ctx, b->in_addr, csw, BOT_CSW_SIZE, &csw_actual);
            if (rc == -ETIMEDOUT) {
                bool should_retry = false;
                int trc = bot_handle_bulk_timeout(b, b->in_addr, attempt, retry_safe, &should_retry);
                if (should_retry)
                    continue;
                return trc;
            }
            if (rc != 0) {
                need_reset = true;
                goto handle_reset;
            }
        } else if (rc != 0) {
            need_reset = true;
            goto handle_reset;
        }

        if (csw_actual != BOT_CSW_SIZE ||
            get_le32(csw + 0) != BOT_CSW_SIGNATURE ||
            get_le32(csw + 4) != tag ||
            csw[12] > 2) {
            need_reset = true;
            goto handle_reset;
        }

        uint32_t residue = get_le32(csw + 8);
        uint8_t status = csw[12];

        if (status == 2) {
            need_reset = true;
            goto handle_reset;
        }

        if (residue > len) {
            need_reset = true;
            goto handle_reset;
        }

        if (status == 1)
            return -EIO;

        if (dir == USB_MSC_NONE) {
            if (residue != 0) {
                need_reset = true;
                goto handle_reset;
            }
            if (actual)
                *actual = 0;
            return 0;
        }

        if (dir == USB_MSC_OUT) {
            if (data_actual != len || residue != 0)
                return -EIO;
            if (actual)
                *actual = len;
            return 0;
        }

        /* USB_MSC_IN */
        size_t csw_len = (size_t)(len - residue);
        if (actual)
            *actual = (data_actual < csw_len) ? data_actual : csw_len;
        return 0;

handle_reset:
        if (need_reset) {
            int rst_rc = bot_reset_recovery(b);
            if (rst_rc != 0)
                return rst_rc;
            if (attempt == 0 && retry_safe)
                continue;
            return -EIO;
        }
    }

    return -EIO;
}
