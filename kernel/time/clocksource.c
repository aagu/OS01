#include <kernel/clocksource.h>

bool     clocksource_active = false;
uint32_t clocksource_mult   = 0;
uint32_t clocksource_shift  = 0;
static uint64_t clocksource_freq = 0;

// 找最大 shift 使 mult = (1e9 << shift)/freq 落在 [1, 2^32)，尽量接近 2^31
// 以最大化精度。freq 在 1MHz~10GHz 范围时循环很快（s≈22..34）。
static void compute_mult_shift(uint64_t freq_hz, uint32_t *mult, uint32_t *shift)
{
    uint32_t s = 1;
    uint64_t m = 0;
    // s 上限 64：>1GHz CPU 需要 shift>31（spec §5.2，如 2.994GHz → 33）；
    // mult 必须 < 2^32，由下方 `m >= (1ULL<<32)` break 保证。
    // 注意：`1e9 << s` 在 s>=35 时溢出 uint64_t（1e9<<35 ≈ 3.4e19 > 2^64），
    // 而 ≥4.3GHz 的 TSC 恰好需要 shift>=35；故中间值用 __uint128_t，
    // 使 1e9<<63（<2^93）也在 2^128 内，高频下循环不再提前 wrap。
    for (; s < 64; s++) {
        m = (uint64_t)(((__uint128_t)1000000000ULL << s) / freq_hz);
        if (m >= (1ULL << 32))
            break;
    }
    s -= 1;                       // 退回最后一个不溢出的 shift
    m = (uint64_t)(((__uint128_t)1000000000ULL << s) / freq_hz);
    if (m == 0) m = 1;            // 极低 freq 保护
    *shift = s;
    *mult  = (uint32_t)m;
}

void clocksource_init(void)
{
    clocksource_freq = arch_cycle_freq();
    if (clocksource_freq == 0) {
        clocksource_active = false;
        return;
    }
    compute_mult_shift(clocksource_freq, &clocksource_mult, &clocksource_shift);
    clocksource_active = true;
}

uint64_t clocksource_freq_hz(void) { return clocksource_freq; }
uint64_t clocksource_cycles(void)  { return arch_cycle_counter(); }