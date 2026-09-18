#ifndef _KERNEL_CLOCKSOURCE_INTERNAL_H
#define _KERNEL_CLOCKSOURCE_INTERNAL_H

#include <stdint.h>

// Internal API exposed for hosttests (aarch64 Timer Task 1.1 RED). The
// mult/shift computation lives in kernel/time/clocksource.c and is part
// of the time-framework contract; making it non-static lets the host test
// link against the real production implementation (not a duplicate of the
// math that could drift).
//
// SPEC §5.2: 找最大 shift 使 mult = (1e9 << shift)/freq 落在 [1, 2^32)，
// 尽量接近 2^31 以最大化精度。
void clocksource_compute_mult_shift(uint64_t freq_hz,
                                    uint32_t *mult, uint32_t *shift);

#endif
