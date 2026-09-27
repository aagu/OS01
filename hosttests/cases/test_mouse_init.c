#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include <driver/i8042.h>
#include <driver/mouse_proto.h>
#include <intr/interrupt.h>

extern int mouse_init(void);
extern int mouse_ready(void);

static uint64_t now, freq = 1000000;
static uint64_t pump_step = 1000;
static int keyboard_ok = 1, irq_ok = 1, irq_registered, irq_unregistered;
static uint8_t command_byte, original_command_byte;
static int cmd_writes, aux_writes, fail_aux_at, resend_at, resend_left;
static int bad_source_at, bad_bat, bad_id, probe_reject, wheel_id = 3;
static int reporting, consumer_active, packet_events;
static int immediate_packet;
static int wrong_source_pending, keyboard_ack_count;
static i8042_consumer_t keyboard_consumer;
static int last_wheel_mode, first_command_write;
static void (*irq_handler)(uint64_t, uint64_t, pt_regs_t *);
static i8042_consumer_t consumer;
static uint8_t pending[16];
static int pending_n, pending_pos;

uint64_t clocksource_cycles(void) { return now++; }
uint64_t clocksource_freq_hz(void) { return freq; }
int subsys_status(const char *name) { return strcmp(name, "keyboard") == 0 ? keyboard_ok : -1; }
int register_subsys(const char *name, int (*init)(void), int phase, uint32_t flags)
{ (void)name; (void)init; (void)phase; (void)flags; return 0; }
void kbd_mock_subsys_register_initcall(int (*fn)(void)) { (void)fn; }
int32_t register_irq(uint32_t gsi, void *arg,
        void (*handler)(uint64_t, uint64_t, pt_regs_t *), uint64_t parameter,
        uint32_t flags, const char *name)
{ (void)arg; (void)parameter; (void)flags; (void)name;
  irq_handler = handler;
  assert_eq(12, gsi); irq_registered++; return irq_ok; }
uint32_t unregister_irq(uint32_t gsi)
{ assert_eq(12, gsi); assert_eq(0, command_byte & 2); irq_unregistered++; return 1; }
void i8042_set_aux_consumer(i8042_consumer_t cb) { consumer = cb; consumer_active = cb != 0; }
void i8042_set_kbd_consumer(i8042_consumer_t cb) { keyboard_consumer = cb; }
static void keyboard_ack(uint8_t b) { if (b == 0xFA) keyboard_ack_count++; }
int i8042_pump(void)
{ now += pump_step;
  if (wrong_source_pending) {
      wrong_source_pending = 0;
      if (keyboard_consumer) keyboard_consumer(0xFA);
  }
  while (pending_pos < pending_n) { uint8_t b = pending[pending_pos++]; if (consumer) consumer(b); }
  pending_pos = pending_n = 0; return 0; }
int i8042_write_controller_cmd(uint8_t b) { (void)b; now += 100; return 0; }
int i8042_read_command_byte(uint8_t *out) { *out = command_byte; now += 100; return 0; }
int i8042_write_command_byte(uint8_t b)
{ if (!cmd_writes) first_command_write = b;
  command_byte = b; cmd_writes++; now += 100; return 0; }
int i8042_write_aux_byte(uint8_t b)
{
    aux_writes++; now += 100;
    if (aux_writes == fail_aux_at) return -1;
    pending_n = pending_pos = 0;
    if (aux_writes == bad_source_at) {
        wrong_source_pending = 1; return 0;
    }
    if (resend_at && b == 0xFF && resend_left-- > 0) {
        pending[pending_n++] = 0xFE; return 0;
    }
    pending[pending_n++] = (probe_reject && b == 0xF3) ? 0xFC : 0xFA;
    if (b == 0xFF) { pending[pending_n++] = bad_bat ? 0xFC : 0xAA; pending[pending_n++] = bad_id ? 0xFF : 0; }
    if (b == 0xF2) pending[pending_n++] = bad_id ? 0xFF : wheel_id;
    if (b == 0xF4) reporting = 1;
    if (b == 0xF4 && immediate_packet) {
        pending[pending_n++] = 0x08;
        pending[pending_n++] = 0x01;
        pending[pending_n++] = 0x02;
    }
    if (b == 0xF5) reporting = 0;
    return 0;
}
int mouse_proto_feed(mouse_proto_state_t *s, uint8_t b, mouse_event_t *ev)
{ (void)s; (void)b; (void)ev; packet_events++; return 0; }
void mouse_proto_reset(mouse_proto_state_t *s, bool wheel)
{ s->count = 0; s->wheel_mode = wheel; last_wheel_mode = wheel; }

static void reset_case(uint8_t cb)
{ now = 0; freq = 1000000; pump_step = 1000; keyboard_ok = irq_ok = 1;
  original_command_byte = command_byte = cb; cmd_writes = aux_writes = 0;
  fail_aux_at = resend_at = resend_left = bad_source_at = bad_bat = bad_id = probe_reject = 0;
  wheel_id = 3; reporting = consumer_active = packet_events = 0;
  immediate_packet = 0;
  last_wheel_mode = first_command_write = 0; irq_handler = 0;
  wrong_source_pending = keyboard_ack_count = 0;
  i8042_set_kbd_consumer(keyboard_ack);
  consumer = 0; pending_n = pending_pos = 0;
  irq_registered = irq_unregistered = 0; }
static void check_rollback(void)
{ assert_eq(0, mouse_ready()); assert_eq(0, consumer_active);
  assert_eq(0, command_byte & 2);
  assert_eq(original_command_byte & (uint8_t)~2, command_byte);
  assert_eq(irq_ok ? irq_registered : 0, irq_unregistered);
  assert_true(now <= 500000); }
static void test_success(uint8_t cb)
{ reset_case(cb); assert_eq(0, mouse_init()); assert_eq(1, mouse_ready());
  assert_eq(1, irq_registered); assert_eq(0, irq_unregistered);
  assert_eq(0, command_byte & 0x20); assert_eq(2, command_byte & 2);
  assert_eq(cb & (uint8_t)~0x22, first_command_write);
  assert_eq(cb & (uint8_t)~0x22, command_byte & (uint8_t)~0x22);
  assert_eq(1, consumer_active); assert_eq(0, packet_events);
  assert_eq(wheel_id == 3 && !probe_reject, last_wheel_mode);
  assert_not_null(irq_handler);
  pending[0] = 0x08; pending_n = 1;
  irq_handler(0, 0, 0); assert_eq(1, packet_events);
  assert_true(now <= 500000); }
static void test_failures(uint8_t cb)
{ reset_case(cb); keyboard_ok = 0; assert_true(mouse_init() != 0);
  assert_eq(cb, command_byte); assert_eq(0, consumer_active);
  reset_case(cb); freq = 0; assert_true(mouse_init() != 0);
  assert_eq(cb, command_byte); assert_eq(0, consumer_active);
  reset_case(cb); bad_source_at = 1; assert_true(mouse_init() != 0);
  check_rollback(); assert_eq(1, keyboard_ack_count);
  assert_true(now <= 65000); /* keyboard ACK cannot satisfy AUX deadline */
  reset_case(cb); bad_bat = 1; assert_true(mouse_init() != 0); check_rollback();
  reset_case(cb); bad_id = 1; assert_true(mouse_init() != 0); check_rollback();
  reset_case(cb); resend_at = 1; resend_left = 3; assert_true(mouse_init() != 0); check_rollback();
  reset_case(cb); irq_ok = 0; assert_true(mouse_init() != 0); check_rollback();
  reset_case(cb); fail_aux_at = 10; assert_true(mouse_init() != 0);
  check_rollback(); assert_eq(11, aux_writes); /* F5 bounded attempt */
  reset_case(cb); bad_source_at = 10; assert_true(mouse_init() != 0);
  check_rollback(); assert_eq(11, aux_writes); }
int main(void)
{ TEST_SUITE("mouse initialization"); test_success(0x41); test_success(0x63);
  reset_case(0x41); wheel_id = 0; assert_eq(0, mouse_init()); assert_eq(0, last_wheel_mode);
  reset_case(0x41); probe_reject = 1; assert_eq(0, mouse_init()); assert_eq(0, last_wheel_mode);
  reset_case(0x41); immediate_packet = 1; assert_eq(0, mouse_init());
  assert_eq(3, packet_events); /* F4 ACK and first packet share one pump */
  for (int n = 0; n <= 2; n++) {
      reset_case(0x41); resend_at = 1; resend_left = n;
      assert_eq(0, mouse_init()); assert_eq(n + 10, aux_writes);
  }
  reset_case(0x41); pump_step = 50000;
  assert_true(mouse_init() != 0); check_rollback();
  test_failures(0x41); test_failures(0x63); TEST_RESULTS(); }
