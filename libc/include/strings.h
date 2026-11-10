#ifndef _STRINGS_H
#define _STRINGS_H 1

/*
 * libc/include/strings.h — the case-insensitive string comparisons.
 *
 * These two calls have always been part of the OS01 libc, but they were
 * only reachable through <string.h>.  The standard puts them in
 * <strings.h>, so that is where they are declared now.  The declaration
 * in <string.h> is kept: it is identical, so a consumer may include
 * either header — or both — and always see the same type.
 *
 * This header is NOT a complete BSD <strings.h>: it carries the two
 * comparisons the libc actually implements and nothing else.  The type
 * dependencies are explicit (<stddef.h> for size_t); the header does not
 * pull in <string.h> for the caller.
 */

#include <sys/cdefs.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compare two strings, ignoring the case of the characters.  The result
 * is < 0, 0 or > 0, like strcmp(). */
int strcasecmp(const char *s1, const char *s2);

/* Compare at most `n` characters of two strings, ignoring the case of
 * the characters.  The result is < 0, 0 or > 0, like strncmp(). */
int strncasecmp(const char *s1, const char *s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* _STRINGS_H */
