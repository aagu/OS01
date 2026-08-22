#ifndef LIBC_FLOATCONV_H
#define LIBC_FLOATCONV_H

#include <stddef.h>

#define FLOATCONV_SCRATCH 768

size_t floatconv_render(char *scratch, size_t scap, double d, int w, int p,
                        int fl, int conv, char *sign_out);

#endif /* LIBC_FLOATCONV_H */
