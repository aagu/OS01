/* test_i8042_demux.c — PS/2 mouse driver Task 4 hosttest (RED first).
 *
 * Host-compiles the REAL kernel/driver/i8042.c against a mock i8042 port
 * model:
 *   - OBF is a FIFO of (byte, aux-flag) entries; arch_inb(0x64) synthesizes
 *     status from the queue head (bit0=OBF, bit5=AUX) and the IBF flag.
 *   - arch_inb(0x60) pops the head (counted, so "OBF empty never reads 0x60"
 *     is assertable).
 *   - IBF full is a test-settable flag (simulates a stuck controller).
 *   - Every port access advances a virtual cycle clock so the 20 ms
 *     transaction deadlines fire deterministically.
 *   - All writes are recorded into out_w[] for sequence assertions.
 *
 * Covers spec §4 requirements: locked status/data pairing + source demux,
 * pre-0x20 drain of stale K/A bytes, 0x20 response never dispatched to
 * consumers, IBF/OBF timeouts, 0x60 and 0xD4 atomic write pairs, and the
 * freq_hz==0 bail-out.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "test_framework.h"
#include <driver/i8042.h>

/* ═══════════════════════════════════════════════════════════
 *  Mock port model
 * ═══════════════════════════════════════════════════════════ */

#define OBF_CAP 64
#define OUT_CAP 128

typedef struct {
    uint8_t byte;
    int     aux;
} obf_ent_t;

static obf_ent_t obf_q[OBF_CAP];
static int obf_head, obf_tail;          /* FIFO: push at tail, pop at head */

typedef struct {
    uint16_t port;
    uint8_t  val;
} out_ent_t;

static out_ent_t out_w[OUT_CAP];
static int out_n;

static int      in60_reads;             /* arch_inb(0x60) invocations */
static int      ibf_block;              /* force status bit1 stuck on */
static uint64_t mock_cycles;            /* virtual cycle counter */
static uint64_t mock_step = 1000;       /* cycles per port access */
static uint64_t mock_freq = 1000000;    /* 1 MHz => 1 cycle = 1 us */

/* Controller behaviour: auto-enqueue the 0x20 response when the test has
 * armed it (resp_armed). */
static int      resp_armed;
static uint8_t  resp_byte;
static int      kbd_count_at_0x20;      /* kbd deliveries seen when 0x20 issued */
static int      kbd_count;              /* fwd decl — see Consumers below */

/* Lock-depth tracking shared with i8042_test_runtime.h inline spinlocks. */
int i8042_mock_lock_depth;

static void obf_push(uint8_t byte, int aux)
{
    if (obf_tail >= OBF_CAP) return;   /* test-side capacity invariant */
    obf_q[obf_tail].byte = byte;
    obf_q[obf_tail].aux  = aux;
    obf_tail++;
}

static int obf_len(void) { return obf_tail - obf_head; }

static void out_record(uint16_t port, uint8_t val)
{
    if (out_n < OUT_CAP) {
        out_w[out_n].port = port;
        out_w[out_n].val  = val;
        out_n++;
    }
}

/* The mock port functions the production i8042.c links against. */
uint8_t arch_inb(uint16_t port)
{
    mock_cycles += mock_step;
    if (port == 0x64) {
        uint8_t st = 0;
        if (obf_head != obf_tail) {
            st |= 0x01;                              /* OBF */
            if (obf_q[obf_head].aux) st |= 0x20;     /* AUX source */
        }
        if (ibf_block) st |= 0x02;                   /* IBF never empties */
        return st;
    }
    if (port == 0x60) {
        if (obf_head == obf_tail) return 0xFF;       /* unpaired read */
        in60_reads++;
        uint8_t b = obf_q[obf_head].byte;
        obf_head++;
        return b;
    }
    return 0;
}

void arch_outb(uint16_t port, uint8_t val)
{
    mock_cycles += mock_step;
    if (port != 0x64 && port != 0x60) return;
    out_record(port, val);
    if (port == 0x64 && val == 0x20) {
        kbd_count_at_0x20 = kbd_count;
        if (resp_armed) obf_push(resp_byte, 0);      /* no AUX flag */
    }
}

/* Virtual clocksource the production object links against. */
uint64_t clocksource_cycles(void) { return mock_cycles; }
uint64_t clocksource_freq_hz(void) { return mock_freq; }

/* ═══════════════════════════════════════════════════════════
 *  Consumers
 * ═══════════════════════════════════════════════════════════ */

static uint8_t kbd_bytes[OBF_CAP];
static int     kbd_count;
static uint8_t aux_bytes[OBF_CAP];
static int     aux_count;
static int     consumer_max_lock_depth;

static void kbd_consume(uint8_t b)
{
    if (i8042_mock_lock_depth > consumer_max_lock_depth)
        consumer_max_lock_depth = i8042_mock_lock_depth;
    if (kbd_count < OBF_CAP) kbd_bytes[kbd_count] = b;
    kbd_count++;
}

static void aux_consume(uint8_t b)
{
    if (i8042_mock_lock_depth > consumer_max_lock_depth)
        consumer_max_lock_depth = i8042_mock_lock_depth;
    if (aux_count < OBF_CAP) aux_bytes[aux_count] = b;
    aux_count++;
}

/* ═══════════════════════════════════════════════════════════
 *  Fixture
 * ═══════════════════════════════════════════════════════════ */

static void fixture_reset(void)
{
    memset(obf_q, 0, sizeof(obf_q));
    obf_head = obf_tail = 0;
    memset(out_w, 0, sizeof(out_w));
    out_n = 0;
    in60_reads = 0;
    ibf_block = 0;
    mock_cycles = 0;
    mock_step = 1000;
    mock_freq = 1000000;
    resp_armed = 0;
    resp_byte = 0;
    kbd_count_at_0x20 = -1;
    i8042_mock_lock_depth = 0;
    memset(kbd_bytes, 0, sizeof(kbd_bytes));
    kbd_count = 0;
    memset(aux_bytes, 0, sizeof(aux_bytes));
    aux_count = 0;
    consumer_max_lock_depth = 0;
    i8042_init();
    i8042_set_kbd_consumer(NULL);
    i8042_set_aux_consumer(NULL);
}

/* Assert out_w[] equals the given (port, val) sequence. */
static void assert_out_seq(const out_ent_t *exp, int n)
{
    assert_eq(n, out_n);
    for (int i = 0; i < n && i < out_n; i++) {
        assert_eq(exp[i].port, out_w[i].port);
        assert_eq(exp[i].val,  out_w[i].val);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════ */

/* K/A interleaving is demuxed to the right consumers, in order,
 * with consumers invoked while the i8042 lock is held. */
static void test_pump_interleaved_kbd_aux(void)
{
    fixture_reset();
    i8042_set_kbd_consumer(kbd_consume);
    i8042_set_aux_consumer(aux_consume);

    obf_push(0x1E, 0);   /* keyboard */
    obf_push(0xFA, 1);   /* aux */
    obf_push(0x1F, 0);
    obf_push(0x08, 1);

    assert_eq(0, i8042_pump());

    assert_eq(2, kbd_count);
    assert_eq(0x1E, kbd_bytes[0]);
    assert_eq(0x1F, kbd_bytes[1]);
    assert_eq(2, aux_count);
    assert_eq(0xFA, aux_bytes[0]);
    assert_eq(0x08, aux_bytes[1]);
    assert_eq(0, obf_len());                 /* fully drained */
    assert_true(consumer_max_lock_depth > 0);/* dispatched in-lock */
}

/* OBF empty => 0x60 is never read. */
static void test_pump_obf_empty_no_0x60_read(void)
{
    fixture_reset();
    i8042_set_kbd_consumer(kbd_consume);
    i8042_set_aux_consumer(aux_consume);

    assert_eq(0, i8042_pump());
    assert_eq(0, in60_reads);
    assert_eq(0, kbd_count);
    assert_eq(0, aux_count);
}

/* No aux consumer => aux byte dropped, does not block later kbd bytes. */
static void test_pump_drops_aux_without_consumer(void)
{
    fixture_reset();
    i8042_set_kbd_consumer(kbd_consume);
    /* no aux consumer */

    obf_push(0x11, 0);
    obf_push(0xFA, 1);   /* stale aux, must be dropped */
    obf_push(0x12, 0);

    assert_eq(0, i8042_pump());
    assert_eq(2, kbd_count);
    assert_eq(0x11, kbd_bytes[0]);
    assert_eq(0x12, kbd_bytes[1]);
    assert_eq(0, obf_len());
}

/* Pre-existing K+A bytes: K dispatched normally, A dropped (no consumer),
 * 0x20 issued only AFTER the drain, response returned via *out and never
 * dispatched to any consumer. */
static void test_read_command_byte_drains_preexisting(void)
{
    fixture_reset();
    i8042_set_kbd_consumer(kbd_consume);
    /* no aux consumer */
    resp_armed = 1;
    resp_byte = 0xAB;

    obf_push(0x21, 0);   /* stale keyboard byte */
    obf_push(0x33, 1);   /* stale aux byte */

    uint8_t v = 0x55;
    assert_eq(0, i8042_read_command_byte(&v));
    assert_eq(0xAB, v);

    /* keyboard byte delivered, aux byte dropped */
    assert_eq(1, kbd_count);
    assert_eq(0x21, kbd_bytes[0]);
    assert_eq(0, aux_count);
    assert_eq(0, obf_len());

    /* 0x20 was sent only after the keyboard byte had been dispatched */
    assert_eq(1, kbd_count_at_0x20);

    /* exactly one transaction: 0x20 out, response in; no 0x60 write-back */
    const out_ent_t exp[] = { {0x64, 0x20} };
    assert_out_seq(exp, 1);

    /* drain read both stale bytes + the response */
    assert_eq(3, in60_reads);
}

/* 0x20 response has no AUX flag and must bypass BOTH consumers. */
static void test_read_command_byte_response_not_dispatched(void)
{
    fixture_reset();
    i8042_set_kbd_consumer(kbd_consume);
    i8042_set_aux_consumer(aux_consume);
    resp_armed = 1;
    resp_byte = 0x5A;

    uint8_t v = 0;
    assert_eq(0, i8042_read_command_byte(&v));
    assert_eq(0x5A, v);
    assert_eq(0, kbd_count);
    assert_eq(0, aux_count);
    assert_eq(0, obf_len());
    assert_eq(1, in60_reads);   /* only the response itself */
}

/* Stuck IBF => transaction times out negatively, command never written. */
static void test_ibf_timeout_returns_negative(void)
{
    fixture_reset();
    ibf_block = 1;

    assert_true(i8042_write_controller_cmd(0xA8) < 0);
    assert_eq(0, out_n);        /* nothing reached the port */

    uint8_t v = 0x77;
    assert_true(i8042_read_command_byte(&v) < 0);
    assert_eq(0x77, v);         /* *out untouched on failure */
    assert_eq(0, out_n);

    assert_true(i8042_write_command_byte(0x03) < 0);
    assert_true(i8042_write_aux_byte(0xF4) < 0);
    assert_eq(0, out_n);
}

/* No response to 0x20 => OBF wait times out; command byte is NOT
 * written back (no 0x60 write in the transaction). */
static void test_obf_timeout_no_command_byte_writeback(void)
{
    fixture_reset();
    resp_armed = 0;   /* controller never answers */

    uint8_t v = 0x66;
    assert_true(i8042_read_command_byte(&v) < 0);
    assert_eq(0x66, v);

    const out_ent_t exp[] = { {0x64, 0x20} };
    assert_out_seq(exp, 1);     /* 0x20 sent, no 0x60 write */
}

/* Plain controller command: single write to 0x64. */
static void test_write_controller_cmd_sequence(void)
{
    fixture_reset();
    assert_eq(0, i8042_write_controller_cmd(0xA8));
    const out_ent_t exp[] = { {0x64, 0xA8} };
    assert_out_seq(exp, 1);
}

/* 0x60 command byte write: 0x64<-0x60 then 0x60<-value, atomic pair. */
static void test_write_command_byte_sequence(void)
{
    fixture_reset();
    assert_eq(0, i8042_write_command_byte(0x61));
    const out_ent_t exp[] = { {0x64, 0x60}, {0x60, 0x61} };
    assert_out_seq(exp, 2);
}

/* 0xD4 aux write: 0x64<-0xD4 then 0x60<-data, atomic pair. */
static void test_write_aux_byte_sequence(void)
{
    fixture_reset();
    assert_eq(0, i8042_write_aux_byte(0xF6));
    const out_ent_t exp[] = { {0x64, 0xD4}, {0x60, 0xF6} };
    assert_out_seq(exp, 2);
}

/* Uncalibrated clocksource => every transaction fails immediately,
 * without touching the ports. */
static void test_zero_freq_fails_transactions(void)
{
    fixture_reset();
    mock_freq = 0;

    assert_true(i8042_write_controller_cmd(0xA8) < 0);
    uint8_t v = 0x99;
    assert_true(i8042_read_command_byte(&v) < 0);
    assert_eq(0x99, v);
    assert_true(i8042_write_command_byte(0x41) < 0);
    assert_true(i8042_write_aux_byte(0xFF) < 0);
    assert_eq(0, out_n);
}

/* ═══════════════════════════════════════════════════════════ */

TEST_LIST_BEGIN
TEST_ENTRY(test_pump_interleaved_kbd_aux),
TEST_ENTRY(test_pump_obf_empty_no_0x60_read),
TEST_ENTRY(test_pump_drops_aux_without_consumer),
TEST_ENTRY(test_read_command_byte_drains_preexisting),
TEST_ENTRY(test_read_command_byte_response_not_dispatched),
TEST_ENTRY(test_ibf_timeout_returns_negative),
TEST_ENTRY(test_obf_timeout_no_command_byte_writeback),
TEST_ENTRY(test_write_controller_cmd_sequence),
TEST_ENTRY(test_write_command_byte_sequence),
TEST_ENTRY(test_write_aux_byte_sequence),
TEST_ENTRY(test_zero_freq_fails_transactions),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return (__test_stats.failed > 0) ? 1 : 0;
}
