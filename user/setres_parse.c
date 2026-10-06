/* user/setres_parse.c — see setres_parse.h.  Pure C, no libc beyond
 * string comparison, so both the target build and the host fixture
 * compile it unchanged. */
#include "setres_parse.h"

#include <string.h>

/* Parse one decimal field into a positive uint32_t.  Rejects the empty
 * string, any non-digit (so signs, spaces and suffixes fail) and any
 * value that overflows uint32_t.  Zero is rejected: the spec only
 * accepts positive dimensions. */
static int parse_u32(const char *s, uint32_t *out)
{
    if (!s || *s == '\0') return -1;
    uint64_t v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFull) return -1;   /* 2^32 and up overflow */
    }
    if (v == 0) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* Parse "WxH" with exactly one lowercase 'x' separator and both fields
 * non-empty.  Fields are copied into small buffers so the shared
 * parse_u32() sees a NUL-terminated string; a field longer than 11
 * digits cannot be a valid uint32_t anyway, so it is rejected. */
static int parse_wxh(const char *s, uint32_t *w, uint32_t *h)
{
    const char *x = NULL;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == 'x') {
            if (x) return -1;       /* a second 'x' (suffix) */
            x = p;
        }
    }
    if (!x) return -1;
    if (x == s || x[1] == '\0') return -1;   /* empty field */

    size_t wl = (size_t)(x - s);
    size_t hl = strlen(x + 1);
    char wbuf[12], hbuf[12];
    if (wl >= sizeof(wbuf) || hl >= sizeof(hbuf)) return -1;
    memcpy(wbuf, s, wl);
    wbuf[wl] = '\0';
    memcpy(hbuf, x + 1, hl);
    hbuf[hl] = '\0';

    if (parse_u32(wbuf, w) != 0) return -1;
    if (parse_u32(hbuf, h) != 0) return -1;
    return 0;
}

int setres_parse(int argc, char *const argv[], struct setres_args *out)
{
    if (!argv || !out) return -1;

    if (argc == 2 && argv[1]) {
        if (strcmp(argv[1], "-h") == 0) {
            out->action = SETRES_HELP;
            return 0;
        }
        if (strcmp(argv[1], "-l") == 0) {
            out->action = SETRES_LIST;
            return 0;
        }
        uint32_t w, h;
        if (parse_wxh(argv[1], &w, &h) == 0) {
            out->action = SETRES_SET;
            out->width = w;
            out->height = h;
            return 0;
        }
        return -1;
    }

    if (argc == 3 && argv[1] && argv[2]) {
        uint32_t w, h;
        if (parse_u32(argv[1], &w) == 0 && parse_u32(argv[2], &h) == 0) {
            out->action = SETRES_SET;
            out->width = w;
            out->height = h;
            return 0;
        }
        return -1;
    }

    return -1;
}
