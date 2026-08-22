/* poll_clocksource_stub.c — host-test stub for clocksource globals.
 * The real poll/select paths call clocksource_read_ns() (inline) which
 * references these exported globals.  In the host harness we keep the
 * clocksource inactive so it falls back to the jiffies timeline. */
#include <stdint.h>
#include <stdbool.h>

bool     clocksource_active = false;
uint32_t clocksource_mult   = 0;
uint32_t clocksource_shift  = 0;
