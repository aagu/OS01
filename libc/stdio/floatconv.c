#include <stdio.h>
#include "stdio_internal.h"
#include "floatconv.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---- fixed-capacity big integer: 80 x uint32_t (~2560 bits), little-endian ---- */
#define BIGINT_WORDS 80

typedef struct { uint32_t w[BIGINT_WORDS]; } bigint;

static void bi_zero(bigint *a) { memset(a, 0, sizeof(*a)); }

/* increment in place (bigint += 1); used for rounding carries */
static void bi_inc(bigint *a)
{
    uint32_t carry = 1;
    for (int i = 0; i < BIGINT_WORDS; i++) {
        uint32_t cur = a->w[i] + carry;
        if (cur >= a->w[i]) carry = 0; else carry = 1;
        a->w[i] = cur;
        if (!carry) break;
    }
}

static void bi_set_u64(bigint *a, uint64_t v)
{
    bi_zero(a);
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
}

static int bi_bitlen(const bigint *a)
{
    for (int i = BIGINT_WORDS - 1; i >= 0; i--)
        if (a->w[i]) {
            uint32_t x = a->w[i];
            int b = 31;
            while (!(x & (1u << b))) b--;
            return i * 32 + b + 1;
        }
    return 0;
}

static void bi_shl1(bigint *a)
{
    uint32_t carry = 0;
    for (int i = 0; i < BIGINT_WORDS; i++) {
        uint32_t cur = a->w[i];
        a->w[i] = (cur << 1) | carry;
        carry = cur >> 31;
    }
}

static void bi_shl(bigint *a, int k)
{
    int words = k >> 5;
    int bits = k & 31;
    int i;
    if (words) {
        for (i = BIGINT_WORDS - 1; i >= words; i--)
            a->w[i] = a->w[i - words];
        for (i = words - 1; i >= 0; i--)
            a->w[i] = 0;
    }
    if (bits) {
        uint32_t carry = 0;
        for (i = 0; i < BIGINT_WORDS; i++) {
            uint32_t cur = a->w[i];
            a->w[i] = (cur << bits) | carry;
            carry = cur >> (32 - bits);
        }
    }
}

/* out = a >> k (k >= 0) */
static void bi_shr(const bigint *a, int k, bigint *out)
{
    int words = k >> 5;
    int bits = k & 31;
    int i;
    for (i = 0; i + words < BIGINT_WORDS; i++)
        out->w[i] = a->w[i + words];
    for (; i < BIGINT_WORDS; i++)
        out->w[i] = 0;
    if (bits) {
        uint32_t carry = 0;
        for (i = BIGINT_WORDS - 1; i >= 0; i--) {
            uint32_t cur = out->w[i];
            out->w[i] = (cur >> bits) | carry;
            carry = cur << (32 - bits);
        }
    }
}

static void bi_sub(bigint *a, const bigint *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < BIGINT_WORDS; i++) {
        uint64_t s = (uint64_t)a->w[i] - b->w[i] - borrow;
        a->w[i] = (uint32_t)s;
        borrow = (s >> 32) ? 1 : 0;
    }
}

static void bi_setbit(bigint *a, int i)
{
    a->w[i >> 5] |= (1u << (i & 31));
}

static int bi_cmp(const bigint *a, const bigint *b)
{
    for (int i = BIGINT_WORDS - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i])
            return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}

/* a = a * m, m a small constant (m < 2^32) */
static void bi_mul_small(bigint *a, uint64_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < BIGINT_WORDS; i++) {
        uint64_t cur = (uint64_t)a->w[i] * m + carry;
        a->w[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
}

/* a = a * 5^e ; e >= 0 */
static void bi_mul_5pow(bigint *a, int e)
{
    while (e > 0) {
        bi_mul_small(a, 5);
        e--;
    }
}

/* a = 10^e (e >= 0) */
static void bi_pow10(bigint *a, int e)
{
    bi_set_u64(a, 1);
    bi_mul_5pow(a, e);
    bi_shl(a, e);
}

/* restoring long division: q = a / b, r = a % b (a, b >= 0, b != 0) */
static void bi_divmod(const bigint *a, const bigint *b, bigint *q, bigint *r)
{
    bi_zero(q);
    bi_zero(r);
    int n = bi_bitlen(a);
    for (int i = n - 1; i >= 0; i--) {
        bi_shl1(r);
        if ((a->w[i >> 5] >> (i & 31)) & 1) r->w[0] |= 1;
        if (bi_cmp(r, b) >= 0) {
            bi_sub(r, b);
            bi_setbit(q, i);
        }
    }
}

/* convert bigint to decimal ASCII (most-significant first) into buf; returns len */
static int bi_to_dec(const bigint *a, char *buf, size_t cap)
{
    bigint t = *a;
    char tmp[128];
    int n = 0;
    int nonzero = 0;
    for (int i = 0; i < BIGINT_WORDS; i++)
        if (t.w[i]) { nonzero = 1; break; }
    if (!nonzero) {
        if (cap > 0) buf[0] = '0';
        return 1;
    }
    while (nonzero) {
        uint64_t rem = 0;
        for (int i = BIGINT_WORDS - 1; i >= 0; i--) {
            uint64_t cur = ((uint64_t)t.w[i]) + (rem << 32);
            t.w[i] = (uint32_t)(cur / 10);
            rem = cur % 10;
        }
        if (n < (int)sizeof(tmp)) tmp[n++] = (char)('0' + rem);
        nonzero = 0;
        for (int i = 0; i < BIGINT_WORDS; i++)
            if (t.w[i]) { nonzero = 1; break; }
    }
    int len = 0;
    for (int i = 0; i < n; i++)
        if ((size_t)len < cap) buf[len++] = tmp[n - 1 - i];
    return len;
}

/* extract bigint to uint64; returns 1 if it fits, 0 if overflow */
/*
 * Compute round(|value| * 10^unit) as a bigint into *out.
 * value = mant * 2^exp2 (exact). unit may be any integer.
 * Returns 0 on success, 1 on capacity/overflow.
 */
static int round_to_int(uint64_t mant, int exp2, int unit, bigint *out)
{
    bigint a;
    if (unit >= 0) {
        bi_set_u64(&a, mant);
        bi_mul_5pow(&a, unit);
        int shift = exp2 + unit;
        if (shift >= 0) {
            bi_shl(&a, shift);
            *out = a;
            return 0;
        } else {
            int k = -shift;
            bigint q;
            bi_shr(&a, k, &q);
            /* ties-to-even rounding at the half = 2^(k-1) bit of the
             * original value `a`. Inspect bits directly (works for any k). */
            if (k > 0) {
                int half_pos = k - 1;
                int half_bit = (a.w[half_pos >> 5] >> (half_pos & 31)) & 1;
                if (half_bit) {
                    int lower_set = 0;
                    for (int i = 0; i <= (half_pos >> 5); i++) {
                        uint32_t mask = (i == (half_pos >> 5))
                            ? ((1u << (half_pos & 31)) - 1)
                            : 0xFFFFFFFFu;
                        if (a.w[i] & mask) { lower_set = 1; break; }
                    }
                    if (lower_set || (q.w[0] & 1)) {
                        uint32_t c = 1;
                        for (int i = 0; i < BIGINT_WORDS; i++) {
                            uint32_t cur = q.w[i] + c;
                            if (cur >= q.w[i]) c = 0; else c = 1;
                            q.w[i] = cur;
                            if (!c) break;
                        }
                        if (c) return 1;
                    }
                }
            }
            *out = q;
            return 0;
        }
    } else {
        int Q = -unit;
        /* target = value*10^unit = mant*2^exp2 / 10^Q = mant*2^exp2 / (2^Q*5^Q).
         * Cancel the 2^Q with 2^exp2: num = mant * 2^(exp2-Q) (left-shift if
         * exp2>=Q, else shift the 5^Q denominator by the remainder), den = 5^Q.
         * Result q is the true integer target (no residual scale). */
        bigint num, den;
        bi_set_u64(&num, mant);
        bi_set_u64(&den, 1);
        bi_mul_5pow(&den, Q);
        int diff = exp2 - Q;
        if (diff >= 0) bi_shl(&num, diff);
        else           bi_shl(&den, -diff);
        bigint q, r;
        bi_divmod(&num, &den, &q, &r);
        bigint half;
        bi_shr(&den, 1, &half);
        int c = bi_cmp(&r, &half);
        if (c > 0) {
            bi_inc(&q);
        } else if (c == 0 && (q.w[0] & 1)) {
            bi_inc(&q);
        }
        *out = q;
        return 0;
    }
}

/* emit integer `val` zero-padded to width into out; returns len */
static int emit_int(char *out, uint64_t val, int w)
{
    char tmp[32];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val) { tmp[n++] = (char)('0' + val % 10); val /= 10; }
    while (n < w) tmp[n++] = '0';
    int len = 0;
    for (int i = n - 1; i >= 0; i--) out[len++] = tmp[i];
    return len;
}

/* strip trailing zeros (and dangling '.') unless SPECIAL set; returns new len.
 * Handles exponent form: only the fractional digits before 'e'/'E' are stripped,
 * and the exponent suffix is preserved. */
static int strip_zeros(char *s, int len, int fl)
{
    if (fl & SPECIAL) return len;
    int epos = -1;
    for (int i = 0; i < len; i++)
        if (s[i] == 'e' || s[i] == 'E') { epos = i; break; }
    int end = (epos >= 0) ? epos : len;
    int i = end - 1;
    while (i >= 0 && s[i] == '0') i--;
    if (i >= 0 && s[i] == '.') i--;
    int newlen = i + 1;
    if (epos >= 0) {
        for (int j = epos; j < len; j++) s[newlen++] = s[j];
    }
    return newlen;
}

/* build "int.frac" from decimal string ds (len D) with nfrac fractional digits */
static int build_ff(char *out, const char *ds, int D, int nfrac, int fl)
{
    int ilen = D - nfrac;
    if (ilen < 0) ilen = 0;
    int len = 0, i;
    if (ilen > 0) { for (i = 0; i < ilen; i++) out[len++] = ds[i]; }
    else out[len++] = '0';
    if ((fl & SPECIAL) || nfrac > 0) {
        out[len++] = '.';
        if (D < nfrac) {
            for (i = 0; i < nfrac - D; i++) out[len++] = '0';
            for (i = 0; i < D; i++) out[len++] = ds[i];
        } else {
            for (i = ilen; i < D; i++) out[len++] = ds[i];
        }
    }
    return len;
}

/* Returns 1 if mant*2^exp2 >= 10^E, else 0. Only ever calls bi_pow10 with a
 * non-negative exponent (so no negative bigint shifts occur). */
static int value_ge_pow10(uint64_t mant, int exp2, int E)
{
    bigint lhs, rhs;
    if (E >= 0) {
        /* mant*2^exp2 >= 10^E  =>  compare (mant<<max(0,exp2)) vs (10^E<<max(0,-exp2)) */
        bi_set_u64(&lhs, mant);
        bi_pow10(&rhs, E);
        if (exp2 >= 0) bi_shl(&lhs, exp2);
        else           bi_shl(&rhs, -exp2);
    } else {
        int P = -E;
        /* mant*2^exp2 >= 10^-P  <=>  mant*10^P*2^exp2 >= 1.
         * Scale so both sides are integers: lhs = mant*10^P (shifted by exp2 if >=0),
         * rhs = 1 (shifted by |exp2| if exp2<0). */
        bi_set_u64(&lhs, mant);
        bi_mul_5pow(&lhs, P);
        bi_shl(&lhs, P);            /* lhs = mant*10^P */
        bi_set_u64(&rhs, 1);
        if (exp2 >= 0) bi_shl(&lhs, exp2);
        else           bi_shl(&rhs, -exp2);
    }
    return bi_cmp(&lhs, &rhs) >= 0;
}

size_t floatconv_render(char *scratch, size_t scap, double d,
                        int w, int p, int fl, int conv, char *sign_out)
{
    (void)w;
    *sign_out = 0;

    union { double d; uint64_t u; } u;
    u.d = d;
    uint64_t bits = u.u;
    int neg = (int)((bits >> 63) & 1);
    int e_field = (int)((bits >> 52) & 0x7ff);
    uint64_t frac = bits & 0xfffffffffffffULL;

    int is_inf = (e_field == 0x7ff) && (frac == 0);
    int is_nan = (e_field == 0x7ff) && (frac != 0);
    int is_zero = (e_field == 0) && (frac == 0);

    if (is_nan) {
        *sign_out = neg ? '-' : 0;
        const char *s = (conv == 'F' || conv == 'E' || conv == 'G') ? "NAN" : "nan";
        int l = (int)strlen(s);
        if ((size_t)l > scap) return SIZE_MAX;
        memcpy(scratch, s, l);
        return (size_t)l;
    }
    if (is_inf) {
        *sign_out = neg ? '-' : 0;
        const char *s = (conv == 'F' || conv == 'E' || conv == 'G') ? "INF" : "inf";
        int l = (int)strlen(s);
        if ((size_t)l > scap) return SIZE_MAX;
        memcpy(scratch, s, l);
        return (size_t)l;
    }

    int N;
    if (conv == 'g' || conv == 'G') {
        N = (p < 0) ? 6 : p;
        if (N == 0) N = 1;
    } else {
        N = (p < 0) ? 6 : p;
    }
    if (N > FLOATCONV_MAX_PREC) N = FLOATCONV_MAX_PREC;
    if (p > FLOATCONV_MAX_PREC) p = FLOATCONV_MAX_PREC;
    int pfrac = (p < 0) ? 6 : p;

    uint64_t mant;
    int exp2;
    if (e_field == 0) {
        mant = frac;
        exp2 = -1074;
    } else {
        mant = (1ULL << 52) | frac;
        exp2 = e_field - 1075;
    }

    /* decimal exponent E for %e/%g: largest E with value >= 10^E. */
    bigint r;
    int E;
    if (is_zero) {
        E = 0;
    } else {
        int eb = exp2 + 53;
        double lge = eb * 0.30102999566398114 - 0.0000001;
        E = (int)lge;
        if (E > 300) E = 300;
        if (E < -300) E = -300;
        for (int guard = 0; guard < 700; guard++) {
            int ge  = value_ge_pow10(mant, exp2, E);
            int ge1 = value_ge_pow10(mant, exp2, E + 1);
            if (ge && !ge1) break;
            if (ge) E++; else E--;
        }
    }

    char ds[512];
    int D;
    int res;
    int len = 0;
    char eletter = ((conv == 'E') || (conv == 'G')) ? 'E' : 'e';

    if (conv == 'f' || conv == 'F') {
        res = round_to_int(mant, exp2, pfrac, &r);
        if (res) return SIZE_MAX;
        D = bi_to_dec(&r, ds, sizeof(ds));
        if (D < 1) D = 1;
        len = build_ff(scratch, ds, D, pfrac, fl);
        if ((size_t)len > scap) return SIZE_MAX;
        *sign_out = neg ? '-' : (fl & PLUS ? '+' : (fl & SPACE ? ' ' : 0));
        return (size_t)len;
    }

    if (conv == 'e' || conv == 'E') {
        int unit = pfrac - E;
        res = round_to_int(mant, exp2, unit, &r);
        if (res) return SIZE_MAX;
        D = bi_to_dec(&r, ds, sizeof(ds));
        if (D < 1) D = 1;
        if (D > pfrac + 1) { D = pfrac + 1; E++; }
        scratch[len++] = ds[0];
        scratch[len++] = '.';
        for (int i = 1; i < D; i++) scratch[len++] = ds[i];
        scratch[len++] = eletter;
        scratch[len++] = (E >= 0) ? '+' : '-';
        int ae = E < 0 ? -E : E;
        len += emit_int(scratch + len, (uint64_t)ae, 2);
        if (len > (int)scap) return SIZE_MAX;
        *sign_out = neg ? '-' : (fl & PLUS ? '+' : (fl & SPACE ? ' ' : 0));
        return (size_t)len;
    }

    /* %g / %G */
    {
        int use_e = (E < -4) || (E >= N);
        if (!use_e) {
            int nfrac = N - 1 - E;
            if (nfrac < 0) nfrac = 0;
            res = round_to_int(mant, exp2, nfrac, &r);
            if (res) return SIZE_MAX;
            D = bi_to_dec(&r, ds, sizeof(ds));
            if (D < 1) D = 1;
            len = build_ff(scratch, ds, D, nfrac, fl);
            len = strip_zeros(scratch, len, fl);
        } else {
            int unit = (N - 1) - E;
            res = round_to_int(mant, exp2, unit, &r);
            if (res) return SIZE_MAX;
            D = bi_to_dec(&r, ds, sizeof(ds));
            if (D < 1) D = 1;
            if (D > N) { D = N; E++; }
            scratch[len++] = ds[0];
            scratch[len++] = '.';
            for (int i = 1; i < D; i++) scratch[len++] = ds[i];
            scratch[len++] = eletter;
            scratch[len++] = (E >= 0) ? '+' : '-';
            int ae = E < 0 ? -E : E;
            len += emit_int(scratch + len, (uint64_t)ae, 2);
            len = strip_zeros(scratch, len, fl);
        }
        if (is_zero && (fl & SPECIAL)) {
            len = 0;
            scratch[len++] = '0';
            scratch[len++] = '.';
            for (int i = 0; i < N - 1; i++) scratch[len++] = '0';
        }
        if ((size_t)len > scap) return SIZE_MAX;
        *sign_out = neg ? '-' : (fl & PLUS ? '+' : (fl & SPACE ? ' ' : 0));
        return (size_t)len;
    }
}
