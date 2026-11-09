/*
 * scanf_oracle.c — the STANDALONE glibc oracle for the T12 scan table
 * (hosttests/data/scan_cases.h).
 *
 * Compiled with the HOST compiler and NO OS01 include paths, so every
 * sscanf below is the host's.  One line per case:
 *
 *   id<TAB>count<TAB>slot0<TAB>slot1<TAB>slot2<TAB>slot3
 *
 * The count is sscanf's return value (the number of ASSIGNED
 * conversions, EOF for an input failure before the first conversion).
 * Each slot is the first 16 bytes of a 32-byte poison-filled buffer,
 * as hex: the conversion decides how many bytes are written, so a
 * conversion that writes the wrong number of bytes (or writes into the
 * wrong argument) shows up as a difference.  The 6 padding bytes of an
 * 80-bit long double are masked out ("xx") — they are garbage on both
 * sides.
 */

#include <stdio.h>
#include <string.h>

#include "../data/scan_cases.h"

/* a slot is 8-byte aligned so that a double/long double store is
 * aligned the way the callee expects */
union scan_slot {
    unsigned char b[32];
    long         l;
    double       d;
    long double  ld;
};

static void dump_slot(char *out, const union scan_slot *slot, int kind)
{
    for (int i = 0; i < SCAN_SLOT_BYTES; i++) {
        if (kind == SK_LDBL && i >= SCAN_LDBL_BYTES) {
            out[2 * i]     = 'x';
            out[2 * i + 1] = 'x';
            continue;
        }
        sprintf(out + 2 * i, "%02x", slot->b[i]);
    }
    out[2 * SCAN_SLOT_BYTES] = '\0';
}

int main(void)
{
    for (int i = 0; i < SCAN_CASE_COUNT; i++) {
        union scan_slot slot[4];
        char hex[4][2 * SCAN_SLOT_BYTES + 1];
        int n;

        memset(slot, 0xA5, sizeof(slot));

        n = sscanf(scan_cases[i].input, scan_cases[i].fmt,
                   &slot[0], &slot[1], &slot[2], &slot[3]);

        for (int k = 0; k < 4; k++)
            dump_slot(hex[k], &slot[k], scan_cases[i].kinds[k]);

        printf("%s\t%d\t%s\t%s\t%s\t%s\n",
               scan_cases[i].id, n, hex[0], hex[1], hex[2], hex[3]);
    }
    return 0;
}
