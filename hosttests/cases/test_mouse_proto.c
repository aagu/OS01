#include <driver/mouse_proto.h>
#include "test_framework.h"

static void packet(mouse_proto_state_t *s, mouse_event_t *e,
                   uint8_t a, uint8_t x, uint8_t y, uint8_t w, int wheel)
{
    assert_eq(0, mouse_proto_feed(s, a, e));
    assert_eq(0, mouse_proto_feed(s, x, e));
    assert_eq(wheel ? 0 : 1, mouse_proto_feed(s, y, e));
    if (wheel) assert_eq(1, mouse_proto_feed(s, w, e));
}

int main(void)
{
    mouse_proto_state_t s;
    mouse_event_t e;
    TEST_SUITE("mouse packet parser");
    mouse_proto_reset(&s, false);
    assert_eq(-1, mouse_proto_feed(&s, 0x00, &e));
    packet(&s, &e, 0x0f, 0x01, 0x02, 0, 0);
    assert_eq(7, e.buttons);
    assert_eq(1, e.dx);
    assert_eq(-2, e.dy);
    assert_eq(0, e.wheel);
    assert_eq(0, e.reserved);
    packet(&s, &e, 0x38, 0xff, 0x00, 0, 0);
    assert_eq(-1, e.dx);
    assert_eq(256, e.dy);
    packet(&s, &e, 0x18, 0x00, 0x08, 0, 0);
    assert_eq(-256, e.dx);
    assert_eq(-8, e.dy);
    packet(&s, &e, 0x08, 0xff, 0xff, 0, 0);
    assert_eq(255, e.dx);
    assert_eq(-255, e.dy);
    packet(&s, &e, 0x48, 0x7f, 0x02, 0, 0);
    assert_eq(0, e.dx);
    assert_eq(-2, e.dy);
    packet(&s, &e, 0x88, 0x03, 0x7f, 0, 0);
    assert_eq(3, e.dx);
    assert_eq(0, e.dy);
    mouse_proto_feed(&s, 0x08, &e);
    mouse_proto_feed(&s, 0x11, &e);
    mouse_proto_reset(&s, true);
    packet(&s, &e, 0x09, 0x08, 0x00, 0x01, 1);
    assert_eq(1, e.buttons);
    assert_eq(8, e.dx); /* data byte bit3 set is not a new header */
    assert_eq(1, e.wheel);
    packet(&s, &e, 0x08, 0x00, 0x00, 0x0f, 1);
    assert_eq(-1, e.wheel);
    packet(&s, &e, 0x08, 0x00, 0x00, 0x08, 1);
    assert_eq(-8, e.wheel);
    assert_eq(0, e.reserved);
    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed ? 1 : 0;
}
