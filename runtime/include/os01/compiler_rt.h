#ifndef OS01_COMPILER_RT_H
#define OS01_COMPILER_RT_H

#include <stdint.h>

typedef unsigned __int128 os01_u128_t;

typedef union {
    os01_u128_t value;
    struct {
        uint64_t lo;
        uint64_t hi;
    } limb;
} os01_u128_bits_t;

os01_u128_t __udivti3(os01_u128_t dividend, os01_u128_t divisor);

#endif
