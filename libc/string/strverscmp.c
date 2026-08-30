#include <string.h>
#include <ctype.h>

/*
 * Compare version strings the way GNU/Linux sort -V expects:
 * digit runs compare numerically ("1.10" > "1.9"), and a longer run wins
 * over a shorter one with equal digits ("1.00" > "1.0"); everything else
 * compares bytewise.  Returns <0, 0, >0 like strcmp.
 */
int strverscmp(const char *s1, const char *s2)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (*p1 || *p2) {
        /* Skip a run of non-digits, comparing bytewise. */
        while (*p1 && *p2 && !isdigit(*p1) && !isdigit(*p2)) {
            if (*p1 != *p2)
                return *p1 - *p2;
            p1++;
            p2++;
        }
        /* One string ended, or one is at a digit and the other is not. */
        if (!isdigit(*p1) || !isdigit(*p2)) {
            if (*p1 == *p2)
                continue;             /* both hit a non-digit simultaneously */
            return *p1 - *p2;
        }
        /* Both at a digit run: compare numerically. */
        while (*p1 == '0')
            p1++;                     /* skip leading zeros */
        while (*p2 == '0')
            p2++;
        const unsigned char *q1 = p1;
        const unsigned char *q2 = p2;
        while (isdigit(*p1))
            p1++;
        while (isdigit(*p2))
            p2++;
        size_t l1 = (size_t)(p1 - q1);
        size_t l2 = (size_t)(p2 - q2);
        if (l1 != l2)                 /* longer run (after zeros) is larger */
            return l1 < l2 ? -1 : 1;
        int c = strncmp((const char *)q1, (const char *)q2, l1);
        if (c)
            return c;
    }
    return 0;
}
