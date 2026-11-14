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
#include <unistd.h>

/* Host pthread prototypes (avoiding shadow from libc/include/pthread.h) */
typedef unsigned long host_pthread_t;
extern int pthread_create(host_pthread_t *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg);
extern int pthread_join(host_pthread_t thread, void **retval);
extern int usleep(unsigned int usec);

static volatile int s_sched_ready = 0;
bool vfs_sched_ready(void)
{
    return __atomic_load_n(&s_sched_ready, __ATOMIC_ACQUIRE) != 0;
}

#define EP_IN  0x81
#define EP_OUT 0x02
#define IFACE_NUM 3

enum fake_op_kind {
    OP_CTRL_RESET = 1,
    OP_CLEAR_HALT = 2,
    OP_BULK_OUT   = 3,
    OP_BULK_IN    = 4,
    OP_RECOVER_TIMEOUT = 5,
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

void *kmalloc(size_t size)
{
    return calloc(1, size);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 0;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
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

static int fake_recover_timeout(void *ctx, uint8_t ep_addr)
{
    struct fake_bot_ctx *f = ctx;
    assert_true(f->step_idx < f->step_count);
    if (f->step_idx >= f->step_count)
        return -EIO;
    struct fake_step *s = &f->steps[f->step_idx++];
    assert_eq(OP_RECOVER_TIMEOUT, s->kind);
    assert_eq(s->ep, ep_addr);
    f->log_kind[f->log_count] = OP_RECOVER_TIMEOUT;
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

static void push_recover_timeout(struct fake_bot_ctx *f, uint8_t ep, int rc)
{
    struct fake_step *s = &f->steps[f->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = OP_RECOVER_TIMEOUT;
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

/* ── SCSI Disk Simulator for Task 2 / Task 3 ─────────────────────── */
struct scsi_sim {
    uint8_t *disk;
    uint32_t last_lba;
    uint32_t block_size;
    int tur_fail_count;
    size_t rc10_len;
    bool short_next_read;
    size_t short_next_read_bytes;
    int fail_next_read_rc;
    int fail_read_count;

    int phase; /* 0=expect CBW, 1=expect data, 2=expect CSW */
    uint32_t cur_tag;
    uint32_t cur_len;
    uint8_t cur_dir;
    uint8_t cur_cdb[16];
    uint8_t cur_status;
    uint32_t cur_residue;

    size_t read_cdb_count;
    uint32_t read_lbas[32];
    uint16_t read_counts[32];

    size_t write_cdb_count;
    uint32_t write_lbas[32];
    uint16_t write_counts[32];
    uint8_t write_cdbs[32][16];
    bool write_short_out;
    size_t write_short_out_bytes;
    uint32_t write_nonzero_residue;
    int fail_next_write_rc;
    bool write_corrupt_csw_after_media;

    size_t sync_cdb_count;
    uint8_t sync_cdbs[16][16];
    bool sync_bad_csw;

    /* Concurrency barrier fields */
    volatile int block_first_read;
    volatile int first_read_entered;
    volatile int release_first_read;
    volatile int in_flight_bot;
    volatile int max_in_flight_bot;
};

static int sim_ctrl(void *ctx, uint8_t req_type, uint8_t req, uint16_t val, uint16_t idx)
{
    (void)ctx;
    (void)req_type;
    (void)req;
    (void)val;
    (void)idx;
    return 0;
}

static int sim_clear_halt(void *ctx, uint8_t ep)
{
    (void)ctx;
    (void)ep;
    return 0;
}

static int sim_bulk(void *ctx, uint8_t ep, void *buf, size_t len, size_t *actual)
{
    struct scsi_sim *s = ctx;
    if (s->phase == 0) {
        assert_eq(EP_OUT, ep);
        assert_eq(31, len);
        int cur = __atomic_add_fetch(&s->in_flight_bot, 1, __ATOMIC_SEQ_CST);
        int prev_max = __atomic_load_n(&s->max_in_flight_bot, __ATOMIC_SEQ_CST);
        if (cur > prev_max)
            __atomic_store_n(&s->max_in_flight_bot, cur, __ATOMIC_SEQ_CST);

        const uint8_t *cbw = buf;
        s->cur_tag = get_le32(cbw + 4);
        s->cur_len = get_le32(cbw + 8);
        s->cur_dir = cbw[12];
        memcpy(s->cur_cdb, cbw + 15, 16);
        s->cur_status = 0;
        s->cur_residue = 0;
        if (s->cur_cdb[0] == 0x28 && s->read_cdb_count < 32) {
            s->read_lbas[s->read_cdb_count] = get_be32(s->cur_cdb + 2);
            s->read_counts[s->read_cdb_count] = get_be16(s->cur_cdb + 7);
            s->read_cdb_count++;
        } else if (s->cur_cdb[0] == 0x2a && s->write_cdb_count < 32) {
            memcpy(s->write_cdbs[s->write_cdb_count], s->cur_cdb, 16);
            s->write_lbas[s->write_cdb_count] = get_be32(s->cur_cdb + 2);
            s->write_counts[s->write_cdb_count] = get_be16(s->cur_cdb + 7);
            s->write_cdb_count++;
        } else if (s->cur_cdb[0] == 0x35 && s->sync_cdb_count < 16) {
            memcpy(s->sync_cdbs[s->sync_cdb_count], s->cur_cdb, 16);
            s->sync_cdb_count++;
        }
        s->phase = (s->cur_len > 0) ? 1 : 2;
        if (actual)
            *actual = 31;
        return 0;
    }

    if (s->phase == 1) {
        uint8_t op = s->cur_cdb[0];
        if (op == 0x2a) { /* WRITE(10) */
            assert_eq(EP_OUT, ep);
            if (s->fail_next_write_rc != 0) {
                int rc = s->fail_next_write_rc;
                s->fail_next_write_rc = 0;
                s->phase = 0;
                __atomic_sub_fetch(&s->in_flight_bot, 1, __ATOMIC_SEQ_CST);
                if (actual)
                    *actual = 0;
                return rc;
            }
            uint32_t lba = get_be32(s->cur_cdb + 2);
            uint16_t cnt = get_be16(s->cur_cdb + 7);
            size_t want = (size_t)cnt * 512u;
            size_t give = (want < len) ? want : len;
            if (s->write_short_out) {
                s->write_short_out = false;
                if (s->write_short_out_bytes < give)
                    give = s->write_short_out_bytes;
            }
            memcpy(s->disk + (size_t)lba * 512u, buf, give);
            s->cur_residue = s->write_nonzero_residue;
            s->write_nonzero_residue = 0;
            if (actual)
                *actual = give;
            s->phase = 2;
            return 0;
        }
        assert_eq(EP_IN, ep);
        if (op == 0x12) { /* INQUIRY */
            uint8_t inq[36];
            memset(inq, ' ', sizeof(inq));
            inq[0] = 0x00;
            memcpy(inq + 8, "OS01    ", 8);
            memcpy(inq + 16, "USB DISK\x01\x7f      ", 16);
            memcpy(inq + 32, "1.0 ", 4);
            size_t cp = (len < 36) ? len : 36;
            memcpy(buf, inq, cp);
            if (actual)
                *actual = cp;
        } else if (op == 0x25) { /* READ CAPACITY(10) */
            uint8_t rc10[8];
            put_be32(rc10 + 0, s->last_lba);
            put_be32(rc10 + 4, s->block_size);
            size_t want = s->rc10_len ? s->rc10_len : 8;
            size_t cp = (len < want) ? len : want;
            memcpy(buf, rc10, cp);
            s->cur_residue = (uint32_t)(len - cp);
            if (actual)
                *actual = cp;
        } else if (op == 0x28) { /* READ(10) */
            if (s->fail_next_read_rc != 0) {
                int rc = s->fail_next_read_rc;
                if (s->fail_read_count > 1) {
                    s->fail_read_count--;
                } else {
                    s->fail_next_read_rc = 0;
                    s->fail_read_count = 0;
                }
                s->phase = 0;
                __atomic_sub_fetch(&s->in_flight_bot, 1, __ATOMIC_SEQ_CST);
                if (actual)
                    *actual = 0;
                return rc;
            }
            if (__atomic_exchange_n(&s->block_first_read, 0, __ATOMIC_SEQ_CST)) {
                __atomic_store_n(&s->first_read_entered, 1, __ATOMIC_RELEASE);
                while (!__atomic_load_n(&s->release_first_read, __ATOMIC_ACQUIRE))
                    usleep(100);
            }
            uint32_t lba = get_be32(s->cur_cdb + 2);
            uint16_t cnt = get_be16(s->cur_cdb + 7);
            size_t want = (size_t)cnt * 512u;
            size_t give = (want < len) ? want : len;
            if (s->short_next_read) {
                s->short_next_read = false;
                if (s->short_next_read_bytes < give)
                    give = s->short_next_read_bytes;
            }
            memcpy(buf, s->disk + (size_t)lba * 512u, give);
            s->cur_residue = (uint32_t)(len - give);
            if (actual)
                *actual = give;
        } else {
            if (actual)
                *actual = 0;
        }
        s->phase = 2;
        return 0;
    }

    /* phase == 2: CSW */
    assert_eq(EP_IN, ep);
    if ((s->cur_cdb[0] == 0x2a && s->write_corrupt_csw_after_media) ||
        (s->cur_cdb[0] == 0x35 && s->sync_bad_csw)) {
        s->write_corrupt_csw_after_media = false;
        s->sync_bad_csw = false;
        s->phase = 0;
        __atomic_sub_fetch(&s->in_flight_bot, 1, __ATOMIC_SEQ_CST);
        if (actual)
            *actual = 0;
        return -EIO;
    }
    if (s->cur_cdb[0] == 0x00 && s->tur_fail_count > 0) {
        s->tur_fail_count--;
        s->cur_status = 1;
    }
    uint8_t csw[13];
    memset(csw, 0, sizeof(csw));
    csw[0] = 0x55;
    csw[1] = 0x53;
    csw[2] = 0x42;
    csw[3] = 0x53;
    put_le32(csw + 4, s->cur_tag);
    put_le32(csw + 8, s->cur_residue);
    csw[12] = s->cur_status;
    size_t cp = (len < 13) ? len : 13;
    memcpy(buf, csw, cp);
    s->phase = 0;
    __atomic_sub_fetch(&s->in_flight_bot, 1, __ATOMIC_SEQ_CST);
    if (actual)
        *actual = cp;
    return 0;
}

static struct usb_msc_transport sim_transport(struct scsi_sim *s)
{
    struct usb_msc_transport t = {
        .ctx = s,
        .control_no_data = sim_ctrl,
        .bulk = sim_bulk,
        .clear_halt = sim_clear_halt,
    };
    return t;
}

TEST_FUNC(probe_publish_and_geometry)
{
    block_device_reset_for_test();

    uint8_t *disk = calloc(256, 512);
    for (uint32_t lba = 0; lba < 256; lba++) {
        for (uint32_t i = 0; i < 512; i++)
            disk[lba * 512 + i] = (uint8_t)((lba * 37u + i) & 0xffu);
    }

    struct scsi_sim sim;
    struct usb_msc_transport t = sim_transport(&sim);
    block_device_t *dev = (block_device_t *)(uintptr_t)0x1234;

    /* 1. Reject truncated RC10 */
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 512;
    sim.rc10_len = 4;
    assert_true(usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev) < 0);
    assert_eq(0, (long)(uintptr_t)dev);
    assert_eq(0, block_device_count());

    /* 2. Reject 4096-byte logical block */
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 4096;
    assert_true(usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev) < 0);
    assert_eq(0, (long)(uintptr_t)dev);
    assert_eq(0, block_device_count());

    /* 3. Reject 0xffffffff last LBA */
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 0xffffffffu;
    sim.block_size = 512;
    assert_true(usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev) < 0);
    assert_eq(0, (long)(uintptr_t)dev);
    assert_eq(0, block_device_count());

    /* 4. TUR fail then pass -> publishes 256-sector read-only BLOCK_DISK */
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 512;
    sim.tur_fail_count = 2;
    assert_eq(0, usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev));
    assert_true(dev != NULL);
    assert_eq(1, block_device_count());
    assert_str_eq("usba", dev->name);
    assert_eq(256, dev->sector_count);
    assert_eq(512, dev->sector_size);
    assert_eq(BLOCK_DISK, dev->kind);
    assert_true(dev->parent == NULL);
    assert_true(dev->ops->read != NULL);
    assert_true(dev->ops->write != NULL);
    assert_true(dev->ops->flush != NULL);

    /* 5. 129-sector read is split 128 + 1 with BE LBA/count */
    uint8_t *rbuf = malloc(129 * 512);
    assert_eq(0, dev->ops->read(dev, 10, 129, rbuf));
    assert_eq(2, sim.read_cdb_count);
    assert_eq(10u, sim.read_lbas[0]);
    assert_eq(128u, sim.read_counts[0]);
    assert_eq(138u, sim.read_lbas[1]);
    assert_eq(1u, sim.read_counts[1]);
    assert_eq(0, memcmp(rbuf, disk + 10 * 512, 129 * 512));

    /* 6. Short direct READ returns -EIO */
    sim.short_next_read = true;
    sim.short_next_read_bytes = 512 * 3 + 100;
    assert_eq(-EIO, dev->ops->read(dev, 0, 4, rbuf));

    /* 7. Count 0 and out-of-range checks */
    assert_eq(0, dev->ops->read(dev, 0, 0, rbuf));
    assert_eq(-EINVAL, dev->ops->read(dev, 256, 1, rbuf));
    assert_eq(-EINVAL, dev->ops->read(dev, 250, 7, rbuf));

    free(rbuf);
    free(disk);
}

TEST_FUNC(read_ahead_window)
{
    block_device_reset_for_test();
    __atomic_store_n(&s_sched_ready, 0, __ATOMIC_RELEASE);

    uint8_t *disk = calloc(256, 512);
    for (uint32_t lba = 0; lba < 256; lba++) {
        for (uint32_t i = 0; i < 512; i++)
            disk[lba * 512 + i] = (uint8_t)((lba * 17u + i) & 0xffu);
    }

    struct scsi_sim sim;
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 512;
    struct usb_msc_transport t = sim_transport(&sim);
    block_device_t *dev = NULL;
    assert_eq(0, usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev));
    assert_true(dev != NULL);

    /* 1. 200 one-sector calls from LBA 0 -> exactly 2 READ CDBs (0..127 and 128..255) */
    uint8_t sec[512];
    for (uint32_t lba = 0; lba < 200; lba++) {
        assert_eq(0, dev->ops->read(dev, lba, 1, sec));
        assert_eq(0, memcmp(sec, disk + lba * 512, 512));
    }
    assert_eq(2, sim.read_cdb_count);
    assert_eq(0u, sim.read_lbas[0]);
    assert_eq(128u, sim.read_counts[0]);
    assert_eq(128u, sim.read_lbas[1]);
    assert_eq(128u, sim.read_counts[1]);

    /* 2. LBA 255 of a 256-sector disk sends count=1 (tail clamp) */
    sim.read_cdb_count = 0;
    /* Invalidate/miss by reading LBA 255 after reading LBA 0.. (window is 128..255, so miss at 10 first) */
    assert_eq(0, dev->ops->read(dev, 10, 1, sec)); /* window now [10, 138) */
    assert_eq(0, dev->ops->read(dev, 255, 1, sec));
    assert_eq(0, memcmp(sec, disk + 255 * 512, 512));
    assert_eq(2, sim.read_cdb_count);
    assert_eq(255u, sim.read_lbas[1]);
    assert_eq(1u, sim.read_counts[1]);

    /* 3. Crossing request: window [0, 128), read 4 sectors at LBA 126..129 */
    sim.read_cdb_count = 0;
    assert_eq(0, dev->ops->read(dev, 0, 1, sec)); /* window [0, 128) */
    uint8_t cross[4 * 512];
    assert_eq(0, dev->ops->read(dev, 126, 4, cross));
    assert_eq(0, memcmp(cross, disk + 126 * 512, 4 * 512));
    assert_eq(2, sim.read_cdb_count);
    assert_eq(128u, sim.read_lbas[1]);
    assert_eq(128u, sim.read_counts[1]);

    /* 4. Crossing request with short refill covering suffix (2 full sectors = 1024 bytes) succeeds */
    sim.read_cdb_count = 0;
    assert_eq(0, dev->ops->read(dev, 0, 1, sec)); /* window [0, 128) */
    sim.short_next_read = true;
    sim.short_next_read_bytes = 1024;
    assert_eq(0, dev->ops->read(dev, 126, 4, cross));
    assert_eq(0, memcmp(cross, disk + 126 * 512, 4 * 512));

    /* 5. Crossing request with short refill NOT covering suffix (1023 bytes = 1 full sector) fails -EIO */
    assert_eq(0, dev->ops->read(dev, 0, 1, sec)); /* window [0, 128) */
    sim.short_next_read = true;
    sim.short_next_read_bytes = 1023;
    assert_eq(-EIO, dev->ops->read(dev, 126, 4, cross));

    /* 6. Partial prefetch of 1023 bytes exposes at most 1 sector */
    sim.read_cdb_count = 0;
    sim.short_next_read = true;
    sim.short_next_read_bytes = 1023;
    assert_eq(0, dev->ops->read(dev, 200, 1, sec));
    assert_eq(0, memcmp(sec, disk + 200 * 512, 512));
    assert_eq(1, sim.read_cdb_count);
    /* LBA 201 must miss and issue a new READ */
    assert_eq(0, dev->ops->read(dev, 201, 1, sec));
    assert_eq(2, sim.read_cdb_count);

    /* 7. Partial prefetch of 511 bytes returns -EIO and leaves previous window valid */
    sim.read_cdb_count = 0;
    sim.short_next_read = true;
    sim.short_next_read_bytes = 511;
    assert_eq(-EIO, dev->ops->read(dev, 10, 1, sec));
    assert_eq(1, sim.read_cdb_count);
    /* Previous window [201, ...) must still be valid (0 new READs) */
    assert_eq(0, dev->ops->read(dev, 201, 1, sec));
    assert_eq(0, memcmp(sec, disk + 201 * 512, 512));
    assert_eq(1, sim.read_cdb_count);

    /* 8. Failed refill (-EIO) leaves previous window valid */
    sim.read_cdb_count = 0;
    sim.fail_next_read_rc = -EIO;
    sim.fail_read_count = 2;
    assert_eq(-EIO, dev->ops->read(dev, 10, 1, sec));
    assert_eq(0, dev->ops->read(dev, 202, 1, sec));
    assert_eq(0, memcmp(sec, disk + 202 * 512, 512));
    assert_eq(2, sim.read_cdb_count); /* initial attempt + 1 BOT retry, 0 for cache hit at 202 */

    free(disk);
}

struct reader_arg {
    block_device_t *dev;
    uint64_t lba;
    uint8_t buf[512];
    int rc;
};

static void *reader_thread(void *arg)
{
    struct reader_arg *r = arg;
    r->rc = r->dev->ops->read(r->dev, r->lba, 1, r->buf);
    return NULL;
}

TEST_FUNC(read_ahead_concurrent)
{
    block_device_reset_for_test();
    __atomic_store_n(&s_sched_ready, 1, __ATOMIC_RELEASE);

    uint8_t *disk = calloc(256, 512);
    for (uint32_t lba = 0; lba < 256; lba++) {
        for (uint32_t i = 0; i < 512; i++)
            disk[lba * 512 + i] = (uint8_t)((lba * 53u + i) & 0xffu);
    }

    struct scsi_sim sim;
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 512;
    struct usb_msc_transport t = sim_transport(&sim);
    block_device_t *dev = NULL;
    assert_eq(0, usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev));
    assert_true(dev != NULL);

    sim.block_first_read = 1;
    struct reader_arg a = { .dev = dev, .lba = 5, .rc = -1 };
    struct reader_arg b = { .dev = dev, .lba = 180, .rc = -1 };

    host_pthread_t ta, tb;
    assert_eq(0, pthread_create(&ta, NULL, reader_thread, &a));

    while (!__atomic_load_n(&sim.first_read_entered, __ATOMIC_ACQUIRE))
        usleep(100);

    /* Launch second reader while first reader is inside BOT I/O */
    assert_eq(0, pthread_create(&tb, NULL, reader_thread, &b));
    usleep(2000);

    /* Release first reader */
    __atomic_store_n(&sim.release_first_read, 1, __ATOMIC_RELEASE);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    assert_eq(0, a.rc);
    assert_eq(0, b.rc);
    assert_eq(0, memcmp(a.buf, disk + 5 * 512, 512));
    assert_eq(0, memcmp(b.buf, disk + 180 * 512, 512));
    assert_eq(1, sim.max_in_flight_bot);

    __atomic_store_n(&s_sched_ready, 0, __ATOMIC_RELEASE);
    free(disk);
}

TEST_FUNC(write10_payload_and_flush)
{
    block_device_reset_for_test();
    __atomic_store_n(&s_sched_ready, 0, __ATOMIC_RELEASE);

    uint8_t *disk = calloc(256, 512);
    struct scsi_sim sim;
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 512;
    struct usb_msc_transport t = sim_transport(&sim);
    block_device_t *dev = NULL;
    assert_eq(0, usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev));
    assert_true(dev != NULL);
    assert_true(dev->ops->write != NULL);
    assert_true(dev->ops->flush != NULL);

    /* 1. 129-sector WRITE(10) splits into 128 + 1 with BE LBA/count and exact payload */
    uint8_t *wbuf = malloc(129 * 512);
    for (size_t i = 0; i < 129 * 512; i++)
        wbuf[i] = (uint8_t)((i * 7u + 0x5au) & 0xffu);

    assert_eq(0, dev->ops->write(dev, 10, 129, wbuf));
    assert_eq(2, sim.write_cdb_count);
    assert_eq(0x2a, sim.write_cdbs[0][0]);
    assert_eq(10u, sim.write_lbas[0]);
    assert_eq(128u, sim.write_counts[0]);
    assert_eq(0x2a, sim.write_cdbs[1][0]);
    assert_eq(138u, sim.write_lbas[1]);
    assert_eq(1u, sim.write_counts[1]);
    assert_eq(0, memcmp(disk + 10 * 512, wbuf, 129 * 512));

    /* 2. Zero count, NULL buf, overflow, out-of-range produce no command */
    sim.write_cdb_count = 0;
    assert_eq(0, dev->ops->write(dev, 0, 0, wbuf));
    assert_eq(-EINVAL, dev->ops->write(dev, 0, 1, NULL));
    assert_eq(-EINVAL, dev->ops->write(dev, 256, 1, wbuf));
    assert_eq(-EINVAL, dev->ops->write(dev, 250, 7, wbuf));
    assert_eq(-EINVAL, dev->ops->write(dev, UINT64_MAX, 1, wbuf));
    assert_eq(0, sim.write_cdb_count);

    /* 3. Short OUT and nonzero WRITE residue are rejected */
    sim.write_short_out = true;
    sim.write_short_out_bytes = 511;
    assert_eq(-EIO, dev->ops->write(dev, 0, 1, wbuf));
    assert_eq(1, sim.write_cdb_count);

    sim.write_nonzero_residue = 4;
    assert_eq(-EIO, dev->ops->write(dev, 0, 1, wbuf));
    assert_eq(2, sim.write_cdb_count);

    /* 4. SYNCHRONIZE CACHE(10) 0x35 with zero payload and no IMMED */
    assert_eq(0, dev->ops->flush(dev));
    assert_eq(1, sim.sync_cdb_count);
    assert_eq(0x35, sim.sync_cdbs[0][0]);
    assert_eq(0, sim.sync_cdbs[0][1] & 0x02u); /* no IMMED */
    for (size_t i = 1; i < 10; i++)
        assert_eq(0, sim.sync_cdbs[0][i]);

    free(wbuf);
    free(disk);
}

TEST_FUNC(write_uncertain_invalidates_window)
{
    block_device_reset_for_test();
    __atomic_store_n(&s_sched_ready, 0, __ATOMIC_RELEASE);

    uint8_t *disk = calloc(256, 512);
    for (uint32_t lba = 0; lba < 256; lba++)
        memset(disk + lba * 512, (int)(lba & 0xffu), 512);

    struct scsi_sim sim;
    memset(&sim, 0, sizeof(sim));
    sim.disk = disk;
    sim.last_lba = 255;
    sim.block_size = 512;
    struct usb_msc_transport t = sim_transport(&sim);
    block_device_t *dev = NULL;
    assert_eq(0, usb_msc_probe(&t, 0, EP_IN, EP_OUT, "usba", &dev));
    assert_true(dev != NULL);

    uint8_t sec[512];
    uint8_t wsec[512];
    memset(wsec, 0xa5, sizeof(wsec));

    /* 1. Fill read-ahead window [0, 128), then fail overlapping WRITE after media update */
    assert_eq(0, dev->ops->read(dev, 0, 1, sec));
    assert_eq(1, sim.read_cdb_count);
    sim.read_cdb_count = 0;

    sim.write_corrupt_csw_after_media = true;
    assert_true(dev->ops->write(dev, 5, 1, wsec) < 0);
    /* Must NOT replay WRITE */
    assert_eq(1, sim.write_cdb_count);
    /* Overlapping read-ahead window must be invalidated -> fresh READ(10) sees updated media */
    assert_eq(0, dev->ops->read(dev, 5, 1, sec));
    assert_eq(1, sim.read_cdb_count);
    assert_eq(0, memcmp(sec, wsec, 512));

    /* 2. Transport error on WRITE also does not replay */
    sim.write_cdb_count = 0;
    sim.fail_next_write_rc = -EIO;
    assert_true(dev->ops->write(dev, 10, 1, wsec) < 0);
    assert_eq(1, sim.write_cdb_count);

    /* 3. Non-overlapping WRITE does NOT invalidate unrelated read-ahead window [0, 128) */
    assert_eq(0, dev->ops->read(dev, 0, 1, sec));
    sim.read_cdb_count = 0;
    assert_eq(0, dev->ops->write(dev, 200, 1, wsec));
    assert_eq(0, dev->ops->read(dev, 15, 1, sec));
    assert_eq(0, sim.read_cdb_count);

    /* 4. Failed FLUSH does NOT replay SYNCHRONIZE CACHE */
    sim.sync_cdb_count = 0;
    sim.sync_bad_csw = true;
    assert_true(dev->ops->flush(dev) < 0);
    assert_eq(1, sim.sync_cdb_count);

    free(disk);
}

TEST_FUNC(stop_endpoint_recovery)
{
    struct usb_msc_bot b;
    struct fake_bot_ctx f;
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t data[4] = { 1, 2, 3, 4 };
    uint8_t buf[36];
    size_t actual = 0;

    /* 1. Recovered bulk timeout -> full BOT reset + 2 cleared pipes + 1 safe retry with fresh tag */
    init_bot(&b, &f);
    b.transport.recover_timeout = fake_recover_timeout;
    push_cbw(&f, 0, 31);
    push_in(&f, -ETIMEDOUT, 0, NULL, 0);
    push_recover_timeout(&f, EP_IN, 0);
    push_reset_recovery(&f, 0, 0, 0);
    push_cbw(&f, 0, 31);
    push_in(&f, 0, 4, data, 4);
    push_csw_auto(&f, 0, 0);
    int rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(0, rc);
    assert_eq(4, actual);
    assert_false(b.dead);
    assert_eq(2, f.cbw_count);
    assert_true(f.seen_tags[1] > f.seen_tags[0]);
    assert_eq(f.step_count, f.step_idx);

    /* 2. WRITE/FLUSH (retry_safe=false): recovered timeout resets BOT, NEVER replays */
    init_bot(&b, &f);
    b.transport.recover_timeout = fake_recover_timeout;
    uint8_t wcdb[10] = { 0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    push_cbw(&f, 0, 31);
    push_out(&f, -ETIMEDOUT, 0);
    push_recover_timeout(&f, EP_OUT, 0);
    push_reset_recovery(&f, 0, 0, 0);
    rc = usb_msc_bot_command(&b, wcdb, 10, USB_MSC_OUT, buf, 16, false, &actual);
    assert_eq(-ETIMEDOUT, rc);
    assert_eq(1, f.cbw_count);
    assert_eq(f.step_count, f.step_idx);

    /* 3. Unrecovered bulk timeout (recover_timeout < 0): no BOT reset, marks dead */
    init_bot(&b, &f);
    b.transport.recover_timeout = fake_recover_timeout;
    push_cbw(&f, 0, 31);
    push_in(&f, -ETIMEDOUT, 0, NULL, 0);
    push_recover_timeout(&f, EP_IN, -EIO);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-ETIMEDOUT, rc);
    assert_true(b.dead);
    assert_eq(1, f.cbw_count);
    assert_eq(f.step_count, f.step_idx);

    /* 4. EP0/command timeout during reset recovery remains terminal without recover_timeout */
    init_bot(&b, &f);
    b.transport.recover_timeout = fake_recover_timeout;
    push_cbw(&f, -EIO, 0);
    push_reset_recovery(&f, -ETIMEDOUT, 0, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-ETIMEDOUT, rc);
    assert_true(b.dead);
    assert_eq(f.step_count, f.step_idx);

    /* 5. Second timeout after safe replay does NOT attempt another Stop/replay; marks dead */
    init_bot(&b, &f);
    b.transport.recover_timeout = fake_recover_timeout;
    push_cbw(&f, 0, 31);
    push_in(&f, -ETIMEDOUT, 0, NULL, 0);
    push_recover_timeout(&f, EP_IN, 0);
    push_reset_recovery(&f, 0, 0, 0);
    push_cbw(&f, 0, 31);
    push_in(&f, -ETIMEDOUT, 0, NULL, 0);
    rc = usb_msc_bot_command(&b, cdb, 6, USB_MSC_IN, buf, 4, true, &actual);
    assert_eq(-ETIMEDOUT, rc);
    assert_true(b.dead);
    assert_eq(2, f.cbw_count);
    assert_eq(f.step_count, f.step_idx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(bot_wire_and_csw),
    TEST_ENTRY(bot_recovery),
    TEST_ENTRY(probe_publish_and_geometry),
    TEST_ENTRY(read_ahead_window),
    TEST_ENTRY(read_ahead_concurrent),
    TEST_ENTRY(write10_payload_and_flush),
    TEST_ENTRY(write_uncertain_invalidates_window),
    TEST_ENTRY(stop_endpoint_recovery),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("usb_msc");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
