#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    long acc = 0;
    int neg = 0;
    int any = 0;
    long cutoff, cutlim;

    // Skip whitespace
    while (isspace(*s)) s++;

    // Sign
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    // Base detection
    if ((base == 0 || base == 16) && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    // Overflow limits
    cutoff = neg ? LONG_MIN : LONG_MAX;
    cutlim = cutoff % base;
    cutoff /= base;
    if (neg) cutoff = -cutoff;

    while (*s) {
        int digit;
        char c = *s;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            digit = c - 'A' + 10;
        else
            break;

        if (digit >= base) break;

        any = 1;
        if (neg) {
            if (acc < cutoff || (acc == cutoff && digit > cutlim)) {
                acc = LONG_MIN;
                errno = ERANGE;
                break;
            }
            acc = acc * base - digit;
        } else {
            if (acc > cutoff || (acc == cutoff && digit > cutlim)) {
                acc = LONG_MAX;
                errno = ERANGE;
                break;
            }
            acc = acc * base + digit;
        }
        s++;
    }

    if (!any) {
        errno = EINVAL;
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    if (endptr) *endptr = (char *)s;
    return acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    unsigned long acc = 0;
    int any = 0;
    unsigned long cutoff, cutlim;

    while (isspace(*s)) s++;

    if (*s == '+') s++;
    else if (*s == '-') { s++; } // negative accepted but result is unsigned

    if ((base == 0 || base == 16) && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && *s == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    cutoff = ULONG_MAX / (unsigned long)base;
    cutlim = ULONG_MAX % (unsigned long)base;

    while (*s) {
        int digit;
        char c = *s;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            digit = c - 'A' + 10;
        else
            break;

        if (digit >= base) break;

        any = 1;
        if (acc > cutoff || (acc == cutoff && (unsigned long)digit > cutlim)) {
            acc = ULONG_MAX;
            errno = ERANGE;
            break;
        }
        acc = acc * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (!any) {
        errno = EINVAL;
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    if (endptr) *endptr = (char *)s;
    return acc;
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}
