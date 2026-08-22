#ifndef LIBC_FLOAT_H
#define LIBC_FLOAT_H

#define FLT_RADIX 2

#define DBL_MANT_DIG 53
#define DBL_MAX_EXP 1024
#define DBL_MIN_EXP (-1021)

#define DBL_MAX 1.79769313486231570815e+308
#define DBL_MIN 2.22507385850720138309e-308

_Static_assert(sizeof(double) == 8, "binary64 required");

#endif /* LIBC_FLOAT_H */
