/*
 * floatscan_oracle.c — the STANDALONE glibc oracle for the T11 float
 * table (hosttests/data/floatscan_cases.h).
 *
 * Compiled with the HOST compiler and NO OS01 include paths, so every
 * strtod/strtof/strtold below is the host's.  Prints one line per
 * case:
 *
 *   idx<TAB>f=<8 hex><TAB>d=<16 hex><TAB>ld=<20 hex>/<errno>
 *      <TAB>of=<off>/<od>/<ol>
 *
 * The hex payloads are the raw IEEE bytes (little-endian byte dump of
 * the value's own width: 4 / 8 / the 10 significant bytes of the
 * x87 80-bit long double — the 6 padding bytes are garbage and are
 * not compared).  Endptr offsets follow; -1 would mean endptr
 * untouched (never happens for strtod).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "../data/floatscan_cases.h"

static void dump_hex(char *out, const void *v, int bytes)
{
    const unsigned char *p = v;
    for (int i = 0; i < bytes; i++)
        sprintf(out + 2 * i, "%02x", p[i]);
    out[2 * bytes] = '\0';
}

int main(void)
{
    for (int i = 0; i < FLOAT_CASE_COUNT; i++) {
        const char *s = float_cases[i].input;

        errno = 0;
        float f = strtof(s, NULL);
        int ef = errno;

        errno = 0;
        double d = strtod(s, NULL);
        int ed = errno;

        char *e = NULL;
        errno = 0;
        long double ld = strtold(s, &e);
        int eld = errno;
        long off = e ? (long)(e - s) : -1;

        char hf[16], hd[32], hld[32];
        dump_hex(hf, &f, 4);
        dump_hex(hd, &d, 8);
        dump_hex(hld, &ld, 10);          /* the 80 bits, padding skipped */

        /* the consumed length is identical across the three spellings
         * (same scan; prec only changes the rounding), so one offset
         * column serves all of them */
        printf("%d\tf=%s/%d\td=%s/%d\tld=%s/%d\tof=%ld\n",
               i, hf, ef, hd, ed, hld, eld, off);
    }
    return 0;
}
