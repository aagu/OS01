#ifndef _GETOPT_H
#define _GETOPT_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int    opterr;
extern int    optind;
extern int    optopt;
extern char  *optarg;

int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif
