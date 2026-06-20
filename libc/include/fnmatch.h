#ifndef _FNMATCH_H
#define _FNMATCH_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FNM_NOMATCH    1
#define FNM_PATHNAME   (1 << 0)
#define FNM_PERIOD     (1 << 1)
#define FNM_NOESCAPE   (1 << 2)
#define FNM_LEADING_DIR (1 << 3)
#define FNM_CASEFOLD    (1 << 4)

int fnmatch(const char *pattern, const char *string, int flags);

#ifdef __cplusplus
}
#endif

#endif
