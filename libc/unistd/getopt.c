#include <stddef.h>
char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;
int getopt(int argc, char *const argv[], const char *optstring) {
    (void)argc; (void)argv; (void)optstring;
    return -1;
}
