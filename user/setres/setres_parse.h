/* user/setres_parse.h — argument parser for the setres CLI.
 *
 * Pure, allocation-free and host-testable: no I/O, no device access.
 * setres.c uses it to decide between help / list / set before opening
 * /dev/fb (help must never touch the device). */
#ifndef _SETRES_PARSE_H
#define _SETRES_PARSE_H

#include <stdint.h>

enum setres_action {
    SETRES_HELP,
    SETRES_LIST,
    SETRES_SET,
};

struct setres_args {
    enum setres_action action;
    uint32_t width, height;   /* SETRES_SET only */
};

/* Parse a full argv (argv[0] = program name).  Recognised forms:
 *   -h                 -> SETRES_HELP
 *   -l                 -> SETRES_LIST
 *   W H                -> SETRES_SET
 *   WxH                -> SETRES_SET
 * Width/height are decimal, must be > 0 and fit in uint32_t.  Any
 * other shape (missing / extra arguments, empty field, a leading
 * sign, zero, overflow, a trailing character, a repeated 'x', ...)
 * is rejected.  Returns 0 and fills *out on success, -1 otherwise. */
int setres_parse(int argc, char *const argv[], struct setres_args *out);

#endif /* _SETRES_PARSE_H */
