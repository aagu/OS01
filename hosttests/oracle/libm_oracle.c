/* hosttests/oracle/libm_oracle.c — the glibc reference for the libm test.
 *
 * A standalone host program with NO OS01 include paths: it can only use
 * the host's <math.h> and the host's libm.  It walks the shared case
 * table and prints one row per case:
 *
 *     <id> <fn> <width> <result-bits-hex> <errno>
 *
 * errno is set to a sentinel before every call, so the reference's own
 * error discipline is visible in the transcript.  The OS01 error contract
 * is judged by the case table, not by this output: the two are checked
 * independently on purpose.
 *
 * The build puts -lm AFTER this source file: the link order matters, a
 * -lm before the object file would leave the references unresolved.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../data/libm_cases.h"

#define SENTINEL 0x5a5a

static double bits_to_double(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static float bits_to_float(uint64_t u)
{
    float f;
    uint32_t v = (uint32_t)u;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static uint64_t double_to_bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static uint32_t float_to_bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

int main(void)
{
    for (int i = 0; i < LIBM_CASE_COUNT; i++) {
        const struct libm_case *c = &libm_cases[i];
        uint64_t result = 0;

        errno = SENTINEL;
        if (c->width == 64) {
            double x = bits_to_double(c->a);
            double y = bits_to_double(c->b);
            double r;
            if (!strcmp(c->fn, "fabs"))           r = fabs(x);
            else if (!strcmp(c->fn, "copysign"))  r = copysign(x, y);
            else if (!strcmp(c->fn, "floor"))     r = floor(x);
            else if (!strcmp(c->fn, "ceil"))      r = ceil(x);
            else if (!strcmp(c->fn, "trunc"))     r = trunc(x);
            else if (!strcmp(c->fn, "round"))     r = round(x);
            else if (!strcmp(c->fn, "fmod"))      r = fmod(x, y);
            else if (!strcmp(c->fn, "sqrt"))      r = sqrt(x);
            else { fprintf(stderr, "unknown fn %s\n", c->fn); return 2; }
            result = double_to_bits(r);
        } else {
            float x = bits_to_float(c->a);
            float y = bits_to_float(c->b);
            float r;
            if (!strcmp(c->fn, "fabsf"))           r = fabsf(x);
            else if (!strcmp(c->fn, "copysignf"))  r = copysignf(x, y);
            else if (!strcmp(c->fn, "floorf"))     r = floorf(x);
            else if (!strcmp(c->fn, "ceilf"))     r = ceilf(x);
            else if (!strcmp(c->fn, "truncf"))    r = truncf(x);
            else if (!strcmp(c->fn, "roundf"))    r = roundf(x);
            else if (!strcmp(c->fn, "fmodf"))     r = fmodf(x, y);
            else if (!strcmp(c->fn, "sqrtf"))     r = sqrtf(x);
            else { fprintf(stderr, "unknown fn %s\n", c->fn); return 2; }
            result = float_to_bits(r);
        }
        printf("%s %s %d %016llx %d\n", c->id, c->fn, c->width,
               (unsigned long long)result, errno);
    }
    return 0;
}
