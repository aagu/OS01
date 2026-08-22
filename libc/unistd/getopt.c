#include <stddef.h>
#include <string.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

static int optpos = 1;

int getopt(int argc, char *const argv[], const char *optstring)
{
    /* optind == 0 is a request to re-initialize the scan. */
    if (optind == 0) {
        optind = 1;
        optpos = 1;
        optarg = NULL;
        optopt = 0;
    }

    int colon_mode = (optstring[0] == ':') ? 1 : 0;

    /* A leading '+' / '-' is a mode flag (not a matchable option). */
    const char *os = optstring;
    char mode = 0;
    if (os[0] == '+' || os[0] == '-') {
        mode = os[0];
        os++;
    } else if (os[0] == ':') {
        os++;
    }

    if (optind >= argc || argv[optind] == NULL)
        return -1;

    const char *arg = argv[optind];

    /* GNU/BusyBox "--" terminator. */
    if (strcmp(arg, "--") == 0) {
        optind++;
        optpos = 1;
        return -1;
    }

    /* Return-in-order mode: a non-option operand is yielded as code 1. */
    if (mode == '-' && (arg[0] != '-' || arg[1] == '\0')) {
        optarg = (char *)argv[optind];
        optind++;
        optpos = 1;
        return 1;
    }

    /* Not an option token. In '+' (POSIX) mode we stop without permuting;
     * otherwise we simply stop. Either way, do not advance optind. */
    if (arg[0] != '-' || arg[1] == '\0') {
        optpos = 1;
        return -1;
    }

    char c = arg[optpos];
    optopt = c;

    const char *p = os;
    while (*p && *p != c)
        p++;

    if (*p != c) {
        optpos++;
        if (arg[optpos] == '\0') {
            optind++;
            optpos = 1;
        }
        return '?';
    }

    int has_arg = (*(p + 1) == ':') ? 1 : 0;

    if (has_arg) {
        if (arg[optpos + 1] != '\0') {
            /* attached value: -dVAL */
            optarg = (char *)&arg[optpos + 1];
            optind++;
            optpos = 1;
        } else if (optind + 1 < argc && argv[optind + 1] != NULL) {
            /* separate value: -d VAL */
            optarg = (char *)argv[optind + 1];
            optind += 2;
            optpos = 1;
        } else {
            /* missing argument */
            optpos = 1;
            optind++;
            return colon_mode ? ':' : '?';
        }
        return c;
    }

    /* No-argument option: advance within a bundled token. */
    optpos++;
    if (arg[optpos] == '\0') {
        optind++;
        optpos = 1;
    }
    return c;
}
