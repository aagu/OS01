/*
 * strtox_oracle.c — the STANDALONE glibc oracle for the T10 integer
 * conversion table (hosttests/data/strtox_cases.h).
 *
 * Compiled with the HOST compiler and NO OS01 include paths (the make
 * rule passes none), so every strto* call below resolves to the
 * host's glibc — this process cannot accidentally test our libc.
 *
 * Protocol: reads nothing (the table is compiled in), writes one text
 * line per case to stdout:
 *
 *   idx<TAB>L=<ld>/<off>/<errno><TAB>ULL=<llu>/<off>/<errno>
 *      <TAB>LL=<lld>/<off>/<errno><TAB>UM=<llu>/<off>/<errno>
 *
 * The OS01 test produces byte-identical lines from its own
 * implementations and compares them; the table and the format are the
 * whole contract.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../data/strtox_cases.h"

int main(void)
{
    for (int i = 0; i < STRTOX_CASE_COUNT; i++) {
        int base = strtox_cases[i].base;
        const char *s = strtox_cases[i].input;

        errno = 0;
        long l = strtol(s, NULL, base);
        int el = errno;
        errno = 0;
        unsigned long long ull = strtoull(s, NULL, base);
        int eull = errno;

        /* endptr offsets: the table contract checks exact consumption */
        char *e = NULL;
        errno = 0;
        long long ll = strtoll(s, &e, base);
        int ell = errno;
        long off_ll = e ? (long)(e - s) : -1;
        e = NULL;
        errno = 0;
        uintmax_t um = strtoumax(s, &e, base);
        int eum = errno;
        long off_um = e ? (long)(e - s) : -1;

        /* %llu + cast, so the oracle's rows are byte-identical to the
         * OS01 test's (whose formatter has no %j yet). */
        printf("%d\tL=%ld/%d\tULL=%llu/%d\tLL=%lld/%ld/%d\tUM=%llu/%ld/%d\n",
               i, l, el, ull, eull, ll, off_ll, ell,
               (unsigned long long)um, off_um, eum);
    }
    return 0;
}
