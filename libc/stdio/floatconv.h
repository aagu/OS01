#ifndef LIBC_STDIO_FLOATCONV_H
#define LIBC_STDIO_FLOATCONV_H

#include <stddef.h>
#include <stdint.h>

#define FLOATCONV_MAX_PREC 100

/*
 * Render the numeric body (no sign, no width padding) of a binary64 `d` into
 * `scratch` (capacity `scap`). Returns the body length, or SIZE_MAX if it does
 * not fit. `sign_out` receives the sign char ('+','-',' ','\0').
 *
 * `w` is the field width (ignored here; vformatter does width + sign-aware
 * zero padding). `p` is the precision; -1 means "unspecified" (use default).
 * `fl` carries the printf flag bits (SIGN/PLUS/SPACE/SPECIAL/LEFT/ZEROPAD/SMALL
 * from stdio.h). `conv` is the conversion char ('f','F','e','E','g','G').
 */
size_t floatconv_render(char *scratch, size_t scap, double d,
                        int w, int p, int fl, int conv, char *sign_out);

#endif /* LIBC_STDIO_FLOATCONV_H */
