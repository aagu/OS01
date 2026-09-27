#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include <driver/i8042.h>
#include <driver/mouse_proto.h>
#include <driver/mouse.h>
#include <fs/poll.h>
#include <percpu/percpu.h>
#include <errno.h>
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
static uint8_t packet_bytes[64];
static int immediate_packet;
static int fail_stale_drain, fail_cmd_write_at, fail_cmd_write_from;
static int force_f4_deadline;
static uint64_t f4_timeout_cycle;
static uint64_t f5_write_cost, rollback_write_cost;
static int wrong_source_pending, keyboard_ack_count;
uint32_t num_cpus;
int i8042_mock_lock_depth;
int kbd_mock_this_cpu_calls;
static percpu_t mock_cpu;
percpu_t *this_cpu(void) { kbd_mock_this_cpu_calls++; return &mock_cpu; }
static poll_wait_entry_t poll_entries[12];
static int poll_n;
void poll_wait(poll_table_t *pt, list_t *list, spinlock_T *lock)
{ assert_true(i8042_mock_lock_depth > 0);
  poll_wait_entry_t *e = &poll_entries[poll_n++];
  e->poll_wq = &pt->wq; e->fd_lock = lock;
  list_init(&e->node); list_add_to_before(list, &e->node); }
void wait_queue_wake_all(wait_queue_t *wq)
{ assert_true(i8042_mock_lock_depth > 0); wq->mock++; }
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
  if (fail_stale_drain && cmd_writes == 2 && aux_writes == 9)
      return I8042_ERR_TIMEOUT;
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
  cmd_writes++; now += 100;
  if (cmd_writes >= 3) now += rollback_write_cost;
  if (cmd_writes == fail_cmd_write_at ||
      (fail_cmd_write_from && cmd_writes >= fail_cmd_write_from))
      return I8042_ERR_TIMEOUT;
  command_byte = b; return 0; }
int i8042_write_aux_byte(uint8_t b)
{
    aux_writes++; now += 100;
    if (b == 0xF5) now += f5_write_cost;
    if (aux_writes == fail_aux_at) return -1;
    pending_n = pending_pos = 0;
    if (b == 0xF4 && force_f4_deadline) {
        now = f4_timeout_cycle; reporting = 1; return 0;
    }
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
{ (void)b;
  if (packet_events < (int)sizeof(packet_bytes)) packet_bytes[packet_events] = b;
  packet_events++;
  if (++s->count == 3) { s->count = 0; memset(ev, 0, sizeof(*ev)); ev->dx = packet_events / 3; return 1; }
  return 0; }
void mouse_proto_reset(mouse_proto_state_t *s, bool wheel)
{ s->count = 0; s->wheel_mode = wheel; last_wheel_mode = wheel; }

static void reset_case(uint8_t cb)
{ now = 0; freq = 1000000; pump_step = 1000; keyboard_ok = irq_ok = 1;
  original_command_byte = command_byte = cb; cmd_writes = aux_writes = 0;
  fail_aux_at = resend_at = resend_left = bad_source_at = bad_bat = bad_id = probe_reject = 0;
  wheel_id = 3; reporting = consumer_active = packet_events = 0;
  memset(packet_bytes, 0, sizeof(packet_bytes));
  immediate_packet = 0;
  fail_stale_drain = fail_cmd_write_at = fail_cmd_write_from = force_f4_deadline = 0;
  f4_timeout_cycle = 400000;
  f5_write_cost = rollback_write_cost = 0;
  last_wheel_mode = first_command_write = 0; irq_handler = 0;
  wrong_source_pending = keyboard_ack_count = 0;
  i8042_set_kbd_consumer(keyboard_ack);
  consumer = 0; pending_n = pending_pos = 0;
  irq_registered = irq_unregistered = 0; }
static void inject_event(void)
{ assert_not_null(consumer); consumer(0x08); consumer(0); consumer(0); }
static void test_event_io_and_poll(void)
{
  mouse_event_t events[3];
  reset_case(0x41); wheel_id = 0; assert_eq(0, mouse_init());
  assert_eq(-EAGAIN, mouse_devfs_read(0, 0, sizeof(events), events));
  assert_eq(-EINVAL, mouse_devfs_read(0, 0, 7, events));
  poll_table_t pts[10] = {0}; poll_n = 0;
  for (int i = 0; i < 10; i++)
      assert_eq(0, mouse_poll_dev(0, POLLIN, &pts[i]));
  num_cpus = 0; kbd_mock_this_cpu_calls = 0;
  inject_event();
  for (int i = 0; i < 10; i++) assert_eq(1, pts[i].wq.mock);
  assert_eq(0, kbd_mock_this_cpu_calls);
  assert_eq(POLLIN | POLLRDNORM, mouse_poll_dev(0, POLLIN, &pts[0]));
  assert_eq(8, mouse_devfs_read(0, 0, 15, events));
  assert_eq(1, events[0].dx);
  assert_eq(-EAGAIN, mouse_devfs_read(0, 0, 8, events));
  num_cpus = 1; inject_event(); inject_event();
  assert_eq(2, kbd_mock_this_cpu_calls);
  assert_eq(16, mouse_devfs_read(0, 0, 24, events));
  assert_eq(2, events[0].dx); assert_eq(3, events[1].dx);
  assert_eq(-EAGAIN, mouse_devfs_read(0, 0, 8, events));
  inject_event(); inject_event();
  mouse_event_t reader_a, reader_b;
  assert_eq(8, mouse_devfs_read(0, 0, 8, &reader_a));
  assert_eq(8, mouse_devfs_read(0, 0, 8, &reader_b));
  assert_eq(4, reader_a.dx); assert_eq(5, reader_b.dx);
  assert_eq(-EAGAIN, mouse_devfs_read(0, 0, 8, events));
  reset_case(0x41); wheel_id = 0; assert_eq(0, mouse_init());
  for (int i = 0; i < 66; i++) inject_event();
  assert_eq(2, mouse_dropped_events());
  assert_eq(24, mouse_devfs_read(0, 0, sizeof(events), events));
  assert_eq(3, events[0].dx); assert_eq(4, events[1].dx);
}
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
static void test_payload_values_not_filtered(void)
{
  reset_case(0x41); assert_eq(0, mouse_init());
  const uint8_t packet[] = {0x08, 0xFA, 0xFE, 0x08, 0xAA, 0x01};
  memcpy(pending, packet, sizeof(packet)); pending_n = sizeof(packet);
  irq_handler(0, 0, 0);
  assert_eq(sizeof(packet), packet_events);
  assert_mem_eq(packet, packet_bytes, sizeof(packet));
}
static void test_stale_drain_must_succeed(void)
{
  reset_case(0x41); fail_stale_drain = 1;
  assert_true(mouse_init() != 0); check_rollback();
  assert_eq(0, reporting); assert_eq(9, aux_writes);
}
static void test_f4_timeout_still_sends_f5(void)
{
  reset_case(0x41); force_f4_deadline = 1;
  assert_true(mouse_init() != 0); check_rollback();
  assert_eq(11, aux_writes); assert_eq(0, reporting);
}
static void test_f5_and_rollback_fit_total_deadline(void)
{
  reset_case(0x41);
  force_f4_deadline = 1; bad_source_at = 11; /* F5 has no AUX ACK */
  f4_timeout_cycle = 420000; /* F4 write finishes after active deadline */
  f5_write_cost = rollback_write_cost = 20000;
  assert_true(mouse_init() != 0);
  assert_eq(11, aux_writes);
  assert_eq(1, irq_unregistered);
  assert_true(now <= 500000);
}
static void test_rollback_write_failure_keeps_irq(void)
{
  reset_case(0x41); fail_aux_at = 10; fail_cmd_write_from = 3;
  assert_true(mouse_init() != 0);
  assert_eq(2, command_byte & 2);
  assert_eq(1, irq_registered); assert_eq(0, irq_unregistered);
  assert_eq(0, consumer_active); assert_eq(0, mouse_ready());
}
static void test_command_byte_write_failures(void)
{
  for (int write = 1; write <= 2; write++) {
      reset_case(0x63); fail_cmd_write_at = write;
      assert_true(mouse_init() != 0); check_rollback();
      assert_true(cmd_writes >= write + 1);
  }
}
int main(void)
{ TEST_SUITE("mouse initialization"); test_event_io_and_poll(); test_success(0x41); test_success(0x63);
  test_payload_values_not_filtered();
  test_stale_drain_must_succeed();
  test_f4_timeout_still_sends_f5();
  test_f5_and_rollback_fit_total_deadline();
  test_rollback_write_failure_keeps_irq();
  test_command_byte_write_failures();
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
