/* @os01-test
 * id: test_usb_msc
 * layer: host
 * subsystem: usb
 * timing: strict
 * tier: nightly
 * link: usb-msc
 * scenarios: [drivers.usb.msc]
 */
#include "test_framework.h"
#include <driver/usb_msc.h>
#include <driver/usb_msc_bot.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EP_IN  0x81
#define EP_OUT 0x02
#define IFACE_NUM 3

enum fake_op_kind {
    OP_CTRL_RESET = 1,
    OP_CLEAR_HALT = 2,
    OP_BULK_OUT   = 3,
    OP_BULK_IN    = 4,
};

struct fake_step {
    enum fake_op_kind kind;
    uint8_t ep;
    int rc;
    size_t actual;
    uint8_t data[64];
    size_t data_len;
    bool auto_csw;
    uint32_t csw_residue;
    uint8_t csw_status;
};

struct fake_bot_ctx {
    struct fake_step steps[32];
    size_t step_count;
    size_t step_idx;

    uint32_t last_tag;
    bool has_last_tag;
    uint32_t seen_tags[16];
    size_t seen_tag_count;
    uint8_t last_cbw[31];
    size_t cbw_count;

    enum fake_op_kind log_kind[32];
    uint8_t log_ep[32];
    size_t log_count;
};

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fake_ctrl(void *ctx, uint8_t req_type, uint8_t req,
                     uint16_t val, uint16_t idx)
{
    struct fake_bot_ctx *f = ctx;
    assert_true(f->step_idx < f->step_count);
    if (f->step_idx >= f->step_count)
        return -EIO;
    struct fake_step *s = &f->steps[f->step_idx++];
    assert_eq(OP_CTRL_RESET, s->kind);
    assert_eq(0x21, req_type);
    assert_eq(0xff, req);
    assert_eq(0, val);
    assert_eq(IFACE_NUM, idx);
    f->log_kind[f->log_count] = OP_CTRL_RESET;
    f->log_ep[f->log_count] = 0;
    f->log_count++;
    return s->rc;
}

static int fake_clear_halt(void *ctx, uint8_t ep_addr)
{
    struct fake_bot_ctx *f = ctx;
    assert_true(f->step_idx < f->step_count);
    if (f->step_idx >= f->step_count)
        return -EIO;
    struct fake_step *s = &f->steps[f->step_idx++];
    assert_eq(OP_CLEAR_HALT, s->kind);
    assert_eq(s->ep, ep_addr);
    f->log_kind[f->log_count] = OP_CLEAR_HALT;
    f->log_ep[f->log_count] = ep_addr;
    f->log_count++;
    return s->rc;
}

static int fake_bulk(void *ctx, uint8_t ep_addr, void *buf, size_t len, size_t *actual)
{
    struct fake_bot_ctx *f = ctx;
    assert_true(f->step_idx < f->step_count);
    if (f->step_idx >= f->step_count)
        return -EIO;
    struct fake_step *s = &f->steps[f->step_idx++];
    enum fake_op_kind expected = (ep_addr & 0x80) ? OP_BULK_IN : OP_BULK_OUT;
    assert_eq(expected, s->kind);
    assert_eq(s->ep, ep_addr);
    f->log_kind[f->log_count] = expected;
    f->log_ep[f->log_count] = ep_addr;
    f->log_count++;

    if (expected == OP_BULK_OUT && len == 31 && buf != NULL) {
        const uint8_t *b = buf;
        if (b[0] == 0x55 && b[1] == 0x53 && b[2] == 0x42 && b[3] == 0x43) {
            memcpy(f->last_cbw, b, 31);
            f->last_tag = get_le32(b + 4);
            f->has_last_tag = true;
            f->seen_tags[f->seen_tag_count++] = f->last_tag;
            f->cbw_count++;
        }
    }

    if (expected == OP_BULK_IN && buf != NULL) {
        if (s->auto_csw) {
            uint8_t csw[13];
            memset(csw, 0, sizeof(csw));
            csw[0] = 0x55;
            csw[1] = 0x53;
            csw[2] = 0x42;
            csw[3] = 0x53;
            put_le32(csw + 4, f->last_tag);
            put_le32(csw + 8, s->csw_residue);
            csw[12] = s->csw_status;
            size_t cp = (len < 13) ? len : 13;
            memcpy(buf, csw, cp);
            if (actual)
                *actual = s->actual ? s->actual : cp;
            return s->rc;
        }
        size_t cp = (s->data_len < len) ? s->data_len : len;
        if (cp > 0)
            memcpy(buf, s->data, cp);
    }

    if (actual)
        *actual = s->actual;
    return s->rc;
}

static void init_bot(struct usb_msc_bot *b, struct fake_bot_ctx *f)
{
    memset(f, 0, sizeof(*f));
    memset(b, 0, sizeof(*b));
    b->transport.ctx = f;
    b->transport.control_no_data = fake_ctrl;
    b->transport.bulk = fake_bulk;
    b->transport.clear_halt = fake_clear_halt;
    b->interface_num = IFACE_NUM;
    b->in_addr = EP_IN;
    b->out_addr = EP_OUT;
}

static void push_cbw(struct fake_bot_ctx *f, int rc, size_t actual)
{
    struct fake_step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = OP_BULK_OUT;
    s->ep = EP_OUT;
    s->rc = rc;
    s->actual = actual;
}

static void push_in(struct fake_bot_ctx *f, int rc, size_t actual,
                    const uint8_t *data, size_t data_len)
{
    struct fake_step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = OP_BULK_IN;
    s->ep = EP_IN;
    s->rc = rc;
    s->actual = actual;
    if (data && data_len > 0) {
        memcpy(s->data, data, data_len);
        s->data_len = data_len;
    }
}

static void push_out(struct fake_bot_ctx *f, int rc, size_t actual)
{
    struct fake_step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = OP_BULK_OUT;
    s->ep = EP_OUT;
    s->rc = rc;
    s->actual = actual;
}

static void push_csw_auto(struct fake_bot_ctx *f, uint32_t residue, uint8_t status)
{
    struct fake_step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = OP_BULK_IN;
    s->ep = EP_IN;
    s->rc = 0;
    s->actual = 13;
    s->auto_csw = true;
    s->csw_residue = residue;
    s->csw_status = status;
}

static void push_csw_raw(struct fake_bot_ctx *f, int rc, size_t actual,
                         const uint8_t *raw, size_t raw_len)
{
    push_in(f, rc, actual, raw, raw_len);
}

static void push_reset_recovery(struct fake_bot_ctx *f, int rc_ctrl, int rc_in, int rc_out)
{
    struct fake_step *s1 = &f->steps[f->step_count++];
    memset(s1, 0, sizeof(*s1));
    s1->kind = OP_CTRL_RESET;
    s1->rc = rc_ctrl;
    if (rc_ctrl != 0)
        return;

    struct fake_step *s2 = &f->steps[f->step_count++];
    memset(s2, 0, sizeof(*s2));
    s2->kind = OP_CLEAR_HALT;
    s2->ep = EP_IN;
    s2->rc = rc_in;
    if (rc_in != 0)
        return;

    struct fake_step *s3 = &f->steps[f->step_count++];
    memset(s3, 0, sizeof(*s3));
    s3->kind = OP_CLEAR_HALT;
    s3->ep = EP_OUT;
    s3->rc = rc_out;
}

static void push_clear_halt(struct fake_bot_ctx *f, uint8_t ep, int rc)
{
    struct fake_step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = OP_CLEAR_HALT;
    s->ep = ep;
    s->rc = rc;
}

TEST_FUNC(bot_wire_and_csw)
{
    struct usb_msc_bot b;
    struct fake_bot_ctx f;
    init_bot(&b, &f);

    /* 1. IN command wire format + partial IN trustworthy length min(actual, len - residue) */
    uint8_t cdb[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    uint8_t payload[16];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(0xa0 + i);
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    size_t actual = 999;

    push_cbw(&f, 0, 31);
    push_in(&f, 0, 12, payload, 12); /* actual=12, len=16, residue=6 -> min(12, 10) = 10 */
    push_csw_auto(&f, 6, 0);

    int rc = usb_msc_bot_command(&b, cdb, 10, USB_MSC_IN, buf, 16, true, &actual);
    assert_eq(0, rc);
    assert_eq(10, actual);
    assert_eq(f.step_count, f.step_idx);
    assert_eq(0, memcmp(buf, payload, 12));

    /* Verify CBW bytes */
    assert_eq(0x55, f.last_cbw[0]);
    assert_eq(0x53, f.last_cbw[1]);
    assert_eq(0x42, f.last_cbw[2]);
    assert_eq(0x43, f.last_cbw[3]);
    assert_eq(16u, get_le32(f.last_cbw + 8));
    assert_eq(0x80, f.last_cbw[12]);
    assert_eq(0x00, f.last_cbw[13]);
    assert_eq(10, f.last_cbw[14]);
    assert_eq(0, memcmp(f.last_cbw + 15, cdb, 10));
    for (size_t i = 25; i < 31; i++)
        assert_eq(0, f.last_cbw[i]);

    /* 2. Status 1 fails command without resetting */
    init_bot(&b, &f);
    uint8_t tur[6] = { 0 };
    push_cbw(&f, 0, 31);
    push_csw_auto(&f, 0, 1);
    actual = 77;
    rc = usb_msc_bot_command(&b, tur, 6, USB_MSC_NONE, NULL, 0, true, &actual);
    assert_eq(-EIO, rc);
    assert_eq(0, actual);
    assert_false(b.dead);
    assert_eq(1, f.cbw_count);
    assert_eq(f.step_count, f.step_idx);

    /* 3. Invalid inputs rejected before issuing CBW */
    init_bot(&b, &f);
    assert_eq(-EINVAL, usb_msc_bot_command(&b, tur, 0, USB_MSC_NONE, NULL, 0, true, &actual));
    assert_eq(-EINVAL, usb_msc_bot_command(&b, tur, 17, USB_MSC_NONE, NULL, 0, true, &actual));
    assert_eq(-EINVAL, usb_msc_bot_command(&b, tur, 6, USB_MSC_NONE, buf, 4, true, &actual));
    assert_eq(-EINVAL, usb_msc_bot_command(&b, tur, 6, USB_MSC_IN, buf, 0, true, &actual));
    assert_eq(0, f.cbw_count);

    /* 4. OUT command: residue != 0 fails even with CSW status 0 */
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_out(&f, 0, 16);
    push_csw_auto(&f, 4, 0);
    rc = usb_msc_bot_command(&b, cdb, 10, USB_MSC_OUT, buf, 16, false, &actual);
    assert_eq(-EIO, rc);
    assert_eq(0, actual);
    assert_false(b.dead);
}

TEST_FUNC(bot_recovery)
{
    struct usb_msc_bot b;
    struct fake_bot_ctx f;
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t buf[36];
    size_t actual;

    /* 1. Data STALL -> clear data pipe -> CSW (no full reset) */
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_in(&f, -EPIPE, 0, NULL, 0);
    push_clear_halt(&f, EP_IN, 0);
    push_csw_auto(&f, 36, 0);
    int rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 36, true, &actual);
    assert_eq(0, rc);
    assert_eq(0, actual);
    assert_eq(1, f.cbw_count);
    assert_eq(f.step_count, f.step_idx);

    /* 2. First CSW STALL -> clear IN -> read CSW */
    init_bot(&b, &f);
    uint8_t data[4] = { 1, 2, 3, 4 };
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_raw(&f, -EPIPE, 0, NULL, 0);
    push_clear_halt(&f, EP_IN, 0);
    push_csw_auto(&f, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(0, rc);
    assert_eq(4, actual);
    assert_eq(1, f.cbw_count);

    /* 3. Repeated CSW STALL -> full Reset Recovery + 1 fresh-tag retry */
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_raw(&f, -EPIPE, 0, NULL, 0);
    push_clear_halt(&f, EP_IN, 0);
    push_csw_raw(&f, -EPIPE, 0, NULL, 0);
    push_reset_recovery(&f, 0, 0, 0);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_auto(&f, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(0, rc);
    assert_eq(4, actual);
    assert_eq(2, f.cbw_count);
    assert_true(f.seen_tags[1] > f.seen_tags[0]);

    /* 4. Phase error (status 2) ignores residue, does not expose data, resets + retries */
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_auto(&f, 0, 2);
    push_reset_recovery(&f, 0, 0, 0);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_auto(&f, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(0, rc);
    assert_eq(4, actual);
    assert_eq(2, f.cbw_count);

    /* 5. Bad CSW (wrong tag / bad sig / wrong len / illegal status / residue > len) -> reset */
    uint8_t bad_csw[13] = { 0x55, 0x53, 0x42, 0x53, 0x99, 0, 0, 0, 0, 0, 0, 0, 0 };
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_raw(&f, 0, 13, bad_csw, 13); /* stale tag */
    push_reset_recovery(&f, 0, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, false, &actual);
    assert_eq(-EIO, rc);
    assert_eq(0, actual);
    assert_eq(1, f.cbw_count);

    /* residue > len */
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_auto(&f, 5, 0);
    push_reset_recovery(&f, 0, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, false, &actual);
    assert_eq(-EIO, rc);
    assert_eq(0, actual);

    /* 6. Short CBW (actual != 31): no data stage, runs full recovery, then retries */
    init_bot(&b, &f);
    push_cbw(&f, 0, 16);
    push_reset_recovery(&f, 0, 0, 0);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_auto(&f, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(0, rc);
    assert_eq(OP_BULK_OUT, f.log_kind[0]);
    assert_eq(OP_CTRL_RESET, f.log_kind[1]);

    /* 7. Second desync on replay: runs full recovery, no third CBW */
    init_bot(&b, &f);
    push_cbw(&f, -EIO, 0);
    push_reset_recovery(&f, 0, 0, 0);
    push_cbw(&f, 0, 31);
    push_in(&f, -EIO, 0, NULL, 0);
    push_reset_recovery(&f, 0, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-EIO, rc);
    assert_eq(0, actual);
    assert_eq(2, f.cbw_count);
    assert_eq(f.step_count, f.step_idx);

    /* 8. Recovery failure marks dead and prevents any later CBW */
    init_bot(&b, &f);
    push_cbw(&f, -EIO, 0);
    push_reset_recovery(&f, -EIO, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-EIO, rc);
    assert_true(b.dead);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-EIO, rc);
    assert_eq(1, f.cbw_count);

    /* 9. M1 timeout: immediately terminal, no reset recovery, marks dead */
    init_bot(&b, &f);
    push_cbw(&f, 0, 31);
    push_in(&f, -ETIMEDOUT, 0, NULL, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-ETIMEDOUT, rc);
    assert_true(b.dead);
    assert_eq(f.step_count, f.step_idx);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-EIO, rc);
}

TEST_LIST_BEGIN
    TEST_ENTRY(bot_wire_and_csw),
    TEST_ENTRY(bot_recovery),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("usb_msc");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
