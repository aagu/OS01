#include <stddef.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

static int optpos = 1;

int getopt(int argc, char *const argv[], const char *optstring)
{
    if (optind == 0)
        optind = 1;

    while (optind < argc && argv[optind] != NULL) {
        const char *arg = argv[optind];
        if (arg[optpos] == '\0') {
            optind++;
            optpos = 1;
            continue;
        }
        break;
    }

    if (optind >= argc || argv[optind] == NULL)
        goto done;

    const char *arg = argv[optind];

    if (arg[0] != '-' || arg[1] == '\0')
        goto done;

    if (arg[1] == '-' && arg[2] == '\0') {
        optind++;
        optpos = 1;
        goto done;
    }

    char c = arg[optpos];
    optopt = c;

    const char *p = optstring;
    if (*p == ':')
        p++;
    int has_arg = 0;
    while (*p && *p != c)
        p++;
    if (*p == c) {
        if (*(p + 1) == ':')
            has_arg = 1;
    }

    if (arg[optpos + 1] != '\0') {
        optpos++;
    } else {
        optind++;
        optpos = 1;
    }

    if (*p != c)
        return '?';

    if (has_arg) {
        if (optind < argc && argv[optind] != NULL) {
            optarg = (char *)argv[optind];
            optind++;
            optpos = 1;
        } else {
            optarg = NULL;
            return '?';
        }
    }

    return c;

done:
    optpos = 1;
    return -1;
}
