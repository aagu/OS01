#include <os01/compiler_rt.h>

static int limb_is_zero(const os01_u128_bits_t *value)
{
    return value->limb.hi == 0 && value->limb.lo == 0;
}

static int limb_ge(const os01_u128_bits_t *left, const os01_u128_bits_t *right)
{
    return left->limb.hi > right->limb.hi ||
           (left->limb.hi == right->limb.hi &&
            left->limb.lo >= right->limb.lo);
}

static void limb_sub(os01_u128_bits_t *left, const os01_u128_bits_t *right)
{
    uint64_t borrow = left->limb.lo < right->limb.lo;

    left->limb.lo -= right->limb.lo;
    left->limb.hi -= right->limb.hi;
    left->limb.hi -= borrow;
}

static uint64_t limb_shl1_add_bit(os01_u128_bits_t *value, uint64_t bit)
{
    uint64_t carry = value->limb.hi >> 63;

    value->limb.hi = (value->limb.hi << 1) | (value->limb.lo >> 63);
    value->limb.lo = (value->limb.lo << 1) | bit;
    return carry;
}

static void limb_set_bit(os01_u128_bits_t *value, int bit)
{
    if (bit >= 64)
        value->limb.hi |= UINT64_C(1) << (bit - 64);
    else
        value->limb.lo |= UINT64_C(1) << bit;
}

static uint64_t limb_get_bit(const os01_u128_bits_t *value, int bit)
{
    if (bit >= 64)
        return (value->limb.hi >> (bit - 64)) & 1;
    return (value->limb.lo >> bit) & 1;
}

os01_u128_t __udivti3(os01_u128_t dividend, os01_u128_t divisor)
{
    os01_u128_bits_t n = { .value = dividend };
    os01_u128_bits_t d = { .value = divisor };
    os01_u128_bits_t q;
    os01_u128_bits_t rem;

    q.limb.lo = 0;
    q.limb.hi = 0;
    rem.limb.lo = 0;
    rem.limb.hi = 0;

    if (limb_is_zero(&d))
        __builtin_trap();

    for (int bit = 127; bit >= 0; --bit) {
        uint64_t carry = limb_shl1_add_bit(&rem, limb_get_bit(&n, bit));

        if (carry || limb_ge(&rem, &d)) {
            /*
             * rem is conceptually 129 bits when carry is set.  Because the
             * previous remainder is smaller than d, subtracting nonzero d
             * clears that top bit and leaves a valid two-limb remainder.
             */
            limb_sub(&rem, &d);
            limb_set_bit(&q, bit);
        }
    }

    return q.value;
}
