#ifndef _GETOPT_H
#define _GETOPT_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

int    opterr;
int    optind;
int    optopt;
char  *optarg;

int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif
