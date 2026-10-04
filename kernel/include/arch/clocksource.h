#ifndef _ARCH_CLOCKSOURCE_H
#define _ARCH_CLOCKSOURCE_H
#include <stdint.h>
/* Current platform monotonic nanosecond source, including its fallback. */
uint64_t arch_clocksource_read_ns(void);
#endif
