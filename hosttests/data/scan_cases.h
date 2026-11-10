/* hosttests/data/scan_cases.h — the scanf behaviour table shared by the
 * T12 test (hosttests/cases/test_libc_scan.c, linked against the OS01
 * libc) and the standalone glibc oracle (hosttests/oracle/scanf_oracle.c,
 * compiled with NO OS01 include paths).
 *
 * Every row is one (input, format) pair.  The row is evaluated by
 * calling sscanf(input, format, slot0, slot1, slot2, slot3) — always
 * FOUR pointer arguments, whatever the format consumes.
 *
 * That is deliberate: the varargs are just pointers, and the CONVERSION
 * decides how many bytes are written at the target.  Passing four
 * pointers therefore lets one table describe rows with 0..4 arguments
 * (a suppressed conversion consumes none) without any per-row argument
 * plumbing, and it lets the test see exactly WHICH argument each
 * conversion wrote to: a conversion that wrongly consumed a va_arg
 * lands in the wrong slot and the byte comparison notices.
 *
 * Every slot is a 32-byte buffer pre-filled with the 0xA5 poison.  The
 * comparison dumps the first 16 bytes of each slot as hex, so
 *   - a conversion that writes MORE bytes than its target type (the old
 *     sscanf.c wrote an int for %hhd) is caught by the poison that the
 *     extra write destroyed,
 *   - a conversion that writes FEWER bytes (a %3c that wrongly NUL-
 *     terminates) is caught the same way,
 *   - a conversion that is skipped entirely leaves the poison visible.
 *
 * The 'kinds' field marks the long-double slots: their 80-bit value
 * occupies 10 significant bytes and the 6 padding bytes are garbage on
 * both sides, so the padding is masked out of the comparison.
 *
 * Only rows whose semantics glibc FIXES are listed here.  The rows that
 * OS01 deliberately answers differently (an illegal length modifier, an
 * unknown conversion, POSIX character classes, the out-of-range integer
 * results) are NOT in this table — they are asserted directly in the
 * test, where the divergence is documented.
 */
#ifndef OS01_SCAN_CASES_H
#define OS01_SCAN_CASES_H

/* the slot kinds */
#define SK_PLAIN 0     /* an integer, a pointer, a float, a string: raw bytes */
#define SK_LDBL  1     /* a long double: the 6 padding bytes are masked out */

struct scan_case {
    const char *id;
    const char *input;
    const char *fmt;
    unsigned char kinds[4];
};

static const struct scan_case scan_cases[] = {

    /* ── the integer conversions and their length modifiers ─────── */
    { "i_d",          "42",     "%d",     { SK_PLAIN, 0, 0, 0 } },
    { "i_d_neg",     "-42",     "%d",     { SK_PLAIN, 0, 0, 0 } },
    { "i_d_ws",      "  42",    "%d",     { SK_PLAIN, 0, 0, 0 } },
    { "i_hhd",       "127",     "%hhd",   { SK_PLAIN, 0, 0, 0 } },
    { "i_hhd_big",   "300",     "%hhd",   { SK_PLAIN, 0, 0, 0 } },
    { "i_hhd_neg",   "-300",    "%hhd",   { SK_PLAIN, 0, 0, 0 } },
    { "i_hd",        "32767",   "%hd",    { SK_PLAIN, 0, 0, 0 } },
    { "i_hd_big",    "70000",   "%hd",    { SK_PLAIN, 0, 0, 0 } },
    { "i_ld",        "2147483648", "%ld", { SK_PLAIN, 0, 0, 0 } },
    { "i_lld",       "-9223372036854775807", "%lld", { SK_PLAIN, 0, 0, 0 } },
    { "i_jd",        "1234567890123", "%jd", { SK_PLAIN, 0, 0, 0 } },
    { "i_zu",        "18446744073709551615", "%zu", { SK_PLAIN, 0, 0, 0 } },
    { "i_zd",        "-9223372036854775807", "%zd", { SK_PLAIN, 0, 0, 0 } },
    { "i_td",        "-100",    "%td",    { SK_PLAIN, 0, 0, 0 } },
    { "i_u",         "4294967295", "%u",  { SK_PLAIN, 0, 0, 0 } },
    { "i_u_neg",     "-1",      "%u",     { SK_PLAIN, 0, 0, 0 } },
    { "i_o",         "0755",    "%o",     { SK_PLAIN, 0, 0, 0 } },
    { "i_x",         "deadbeef", "%x",    { SK_PLAIN, 0, 0, 0 } },
    { "i_X",         "DEADBEEF", "%X",    { SK_PLAIN, 0, 0, 0 } },
    { "i_i_hex",     "0x1f",    "%i",     { SK_PLAIN, 0, 0, 0 } },
    { "i_i_oct",     "0755",    "%i",     { SK_PLAIN, 0, 0, 0 } },
    { "i_d_hex",     "0x10",    "%d",     { SK_PLAIN, 0, 0, 0 } },

    /* ── the field width limits the scanned input ───────────────── */
    { "i_w3",        "12345",   "%3d",    { SK_PLAIN, 0, 0, 0 } },
    { "i_w2_2",      "12345",   "%2d%d",  { SK_PLAIN, SK_PLAIN, 0, 0 } },
    { "i_w5_ws",     "   12345", "%5d",   { SK_PLAIN, 0, 0, 0 } },
    { "i_w0",        "12345",   "%0d",    { SK_PLAIN, 0, 0, 0 } },
    { "i_w_neg",     "-12345",  "%3d",    { SK_PLAIN, 0, 0, 0 } },

    /* ── the 64-bit address pair the kernel maps line needs ─────── */
    { "a_pair",      "7f1234567890-7f1234567abc r--p",
      "%lx-%lx %4s", { SK_PLAIN, SK_PLAIN, SK_PLAIN, 0 } },
    { "a_pair_ws",   "  400000-401000 r-xp",
      "%lx-%lx %4s", { SK_PLAIN, SK_PLAIN, SK_PLAIN, 0 } },

    /* ── suppression, literals, %n, several conversions ─────────── */
    { "s_suppress",  "5 7",     "%*d %d", { SK_PLAIN, 0, 0, 0 } },
    { "s_pct",       "100%",    "%d%%",   { SK_PLAIN, 0, 0, 0 } },
    { "s_two",       "1 2",     "%d %d",  { SK_PLAIN, SK_PLAIN, 0, 0 } },
    { "s_n",         "123",     "%d%n",   { SK_PLAIN, SK_PLAIN, 0, 0 } },
    { "s_literal",   "a12b",    "a%db",   { SK_PLAIN, 0, 0, 0 } },

    /* ── the floating-point conversions ─────────────────────────── */
    { "f_f",         "3.5",     "%f",     { SK_PLAIN, 0, 0, 0 } },
    { "f_f_neg",     "-3.5",    "%f",     { SK_PLAIN, 0, 0, 0 } },
    { "f_f_ws",      "  3.5",   "%f",     { SK_PLAIN, 0, 0, 0 } },
    { "f_f_w5",      "3.14159265", "%5f", { SK_PLAIN, 0, 0, 0 } },
    { "f_f_short",   "3",       "%5f",    { SK_PLAIN, 0, 0, 0 } },
    { "f_lf",        "3.5",     "%lf",    { SK_PLAIN, 0, 0, 0 } },
    { "f_e",         "1.5e3",   "%e",     { SK_PLAIN, 0, 0, 0 } },
    { "f_g",         "0.5",     "%g",     { SK_PLAIN, 0, 0, 0 } },
    { "f_Lf",        "3.14159265358979323846", "%Lf", { SK_LDBL, 0, 0, 0 } },
    { "f_a",         "0x1.8p+1", "%a",     { SK_PLAIN, 0, 0, 0 } },
    { "f_la",        "0x1.8p+1", "%la",    { SK_PLAIN, 0, 0, 0 } },
    { "f_La",        "0x1.8p+1", "%La",    { SK_LDBL, 0, 0, 0 } },

    /* ── %c: no whitespace skipping, no NUL terminator ───────────── */
    { "c_c",         "abc",     "%c",     { SK_PLAIN, 0, 0, 0 } },
    { "c_c3",        "abc",     "%3c",    { SK_PLAIN, 0, 0, 0 } },
    { "c_c_ws",      " abc",    "%c",     { SK_PLAIN, 0, 0, 0 } },
    { "c_c2",        "a b",     "%2c",    { SK_PLAIN, 0, 0, 0 } },

    /* ── %s: whitespace delimited, NUL terminated, width limited ── */
    { "t_s",         "hello world", "%s", { SK_PLAIN, 0, 0, 0 } },
    { "t_s_w5",      "hello world", "%5s", { SK_PLAIN, 0, 0, 0 } },
    { "t_s_short",   "ab cd",   "%5s",    { SK_PLAIN, 0, 0, 0 } },
    { "t_s_ws",      "  a b",   "%s",     { SK_PLAIN, 0, 0, 0 } },

    /* ── the scan sets ──────────────────────────────────────────── */
    { "k_bracket",   "]abc",    "%[]]",   { SK_PLAIN, 0, 0, 0 } },
    { "k_neg",       "a1b2",    "%[^0-9]", { SK_PLAIN, 0, 0, 0 } },
    { "k_range",     "abcde",   "%[a-c]", { SK_PLAIN, 0, 0, 0 } },
    { "k_width",     "abcde",   "%3[abc]", { SK_PLAIN, 0, 0, 0 } },
    { "k_fail",      "xyz",     "%[a-c]", { SK_PLAIN, 0, 0, 0 } },
    { "k_dash",      "a-b",     "%[a-]",  { SK_PLAIN, 0, 0, 0 } },

    /* ── the scan set does NOT skip leading whitespace ────────────
     * Whitespace skipping is a property of %s (and of the numeric
     * conversions), NOT of %[: the '[' input item is "a sequence of
     * characters of exactly those in the set", so a leading space that
     * is not in the set is a MATCHING FAILURE.  These rows were taken
     * from the host glibc (probed before the table was written):
     *   " abc" "%[abc]"   -> 0, the destination is left untouched
     *   "  "   "%[a-c]"   -> 0, the destination is left untouched
     *   " abc" "%[ abc]"  -> 1, the space IS a set member and is stored
     *   " \tabc" "%[abc]" -> 0
     *   " abc" "%[^ ]"    -> 0 (the space is excluded by the negation)
     *   ""     "%[a-c]"  -> -1 (an input failure before any conversion)
     */
    { "k_ws_lead",   " abc",    "%[abc]",  { SK_PLAIN, 0, 0, 0 } },
    { "k_ws_only",   "  ",      "%[a-c]",  { SK_PLAIN, 0, 0, 0 } },
    { "k_ws_in_set", " abc",    "%[ abc]", { SK_PLAIN, 0, 0, 0 } },
    { "k_tab_lead",  " \tabc",  "%[abc]",  { SK_PLAIN, 0, 0, 0 } },
    { "k_neg_ws",    " abc",    "%[^ ]",   { SK_PLAIN, 0, 0, 0 } },
    { "k_empty",     "",        "%[a-c]",  { SK_PLAIN, 0, 0, 0 } },

    /* ── %p ─────────────────────────────────────────────────────── */
    { "p_p",         "0x7f001234", "%p", { SK_PLAIN, 0, 0, 0 } },
    { "p_plain",     "7f001234", "%p",   { SK_PLAIN, 0, 0, 0 } },

    /* ── the input-failure and matching-failure answers ─────────── */
    { "e_empty",     "",        "%d",     { SK_PLAIN, 0, 0, 0 } },
    { "e_mismatch",  "abc",     "%d",     { SK_PLAIN, 0, 0, 0 } },
    { "e_ws_only",   "   ",     "%d",     { SK_PLAIN, 0, 0, 0 } },
    { "e_c_short",   "ab",      "%3c",    { SK_PLAIN, 0, 0, 0 } },
};

#define SCAN_CASE_COUNT ((int)(sizeof(scan_cases) / sizeof(scan_cases[0])))

/* the number of bytes of a slot that the comparison looks at, and the
 * mask of the bytes that are garbage on both sides (the long double's
 * padding) */
#define SCAN_SLOT_BYTES 16
#define SCAN_LDBL_BYTES 10

#endif /* OS01_SCAN_CASES_H */
