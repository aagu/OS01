#ifndef _LIBGEN_H
#define _LIBGEN_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

char *dirname(char *path);
char *basename(char *path);

#ifdef __cplusplus
}
#endif

#endif
