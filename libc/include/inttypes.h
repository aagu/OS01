#ifndef _INTTYPES_H
#define _INTTYPES_H 1

#include <sys/cdefs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wide integer conversion (T10).  Same core, caps and errno discipline
 * as the <stdlib.h> strto* family; see strtoll.c. */
intmax_t strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);

#ifdef __cplusplus
}
#endif

/* Printf format macros */

#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "lld"

#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "lli"

#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "llu"

#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  "llo"

#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "llx"

#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "llX"

/* Pointer format macros */
#define PRIdPTR  "ld"
#define PRIiPTR  "li"
#define PRIuPTR  "lu"
#define PRIoPTR  "lo"
#define PRIxPTR  "lx"
#define PRIXPTR  "lX"

/* intmax_t format macros */
#define PRIdMAX  "lld"
#define PRIiMAX  "lli"
#define PRIuMAX  "llu"
#define PRIoMAX  "llo"
#define PRIxMAX  "llx"
#define PRIXMAX  "llX"

#endif /* _INTTYPES_H */
