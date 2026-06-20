#include <fnmatch.h>
#include <stddef.h>

/* Simple fnmatch implementation - supports *, ?, and [...] patterns */
int fnmatch(const char *pattern, const char *string, int flags)
{
    const char *p = pattern;
    const char *s = string;
    const char *p_retry = NULL;
    const char *s_retry = NULL;

    (void)flags; /* most flags not supported in this minimal impl */

    while (*s) {
        if (*p == '?') {
            p++;
            s++;
        } else if (*p == '*') {
            /* Skip consecutive '*' */
            while (*p == '*') p++;
            if (*p == '\0') return 0;  /* trailing * matches everything */
            p_retry = p;
            s_retry = s + 1;
            s++;
        } else if (*p == '[' && p[1] != '\0') {
            /* Character class */
            int matched = 0;
            int negate = 0;
            p++;
            if (*p == '!' || *p == '^') {
                negate = 1;
                p++;
            }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (*s >= *p && *s <= p[2]) matched = 1;
                    p += 3;
                } else {
                    if (*s == *p) matched = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (negate) matched = !matched;
            if (!matched) {
                if (p_retry) {
                    p = p_retry;
                    s = s_retry;
                } else {
                    return FNM_NOMATCH;
                }
            } else {
                s++;
            }
        } else if (*p == '\\' && !(flags & FNM_NOESCAPE)) {
            p++;
            if (*p != *s) {
                if (p_retry) { p = p_retry; s = s_retry; }
                else return FNM_NOMATCH;
            } else { p++; s++; }
        } else {
            if (*p != *s) {
                if (p_retry) {
                    p = p_retry;
                    s = s_retry;
                } else {
                    return FNM_NOMATCH;
                }
            } else {
                p++;
                s++;
            }
        }
    }

    /* Consume trailing '*' */
    while (*p == '*') p++;

    return (*p == '\0') ? 0 : FNM_NOMATCH;
}
