/*
 * libc/stdlib/abs.c — the absolute-value family.
 *
 * abs(), labs() and llabs() return the magnitude of their argument.
 * They are implemented here rather than as compiler builtins for one
 * reason: the implementation must not call itself.
 *
 * Two properties make that safe:
 *
 *   1. The magnitude is computed in UNSIGNED arithmetic, so the negation
 *      itself is defined for every input.
 *   2. The value travels through a `volatile`, so the compiler cannot
 *      recognise the "negative ? -x : x" idiom and rewrite the body into
 *      a CALL to abs().  Such a rewrite is a real hazard: it turns the
 *      function into an endless recursion in the shipped libc, and in a
 *      host test it would silently answer with the HOST libc's abs
 *      instead of this implementation.
 *
 * The one case C leaves undefined is the most negative value of the type
 * (INT_MIN, LONG_MIN, LLONG_MIN): its magnitude is not representable.
 * This libc adds no saturating behaviour of its own there — the standard
 * promises nothing about that case, and inventing a promise here would
 * be one the rest of the C library does not make.
 */

#include <stdlib.h>

int abs(int value)
{
    volatile int v = value;

    if (v < 0)
        return (int)(0u - (unsigned int)v);
    return v;
}

long labs(long value)
{
    volatile long v = value;

    if (v < 0)
        return (long)(0ul - (unsigned long)v);
    return v;
}

long long llabs(long long value)
{
    volatile long long v = value;

    if (v < 0)
        return (long long)(0ull - (unsigned long long)v);
    return v;
}
