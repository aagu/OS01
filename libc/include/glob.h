#ifndef _GLOB_H
#define _GLOB_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
    int gl_flags;
    void (*gl_closedir)(void *);
    void *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *, void *);
    int (*gl_stat)(const char *, void *);
} glob_t;

#define GLOB_ERR      1
#define GLOB_MARK     2
#define GLOB_NOSORT   4
#define GLOB_DOOFFS   8
#define GLOB_NOCHECK  16
#define GLOB_APPEND   32
#define GLOB_NOESCAPE 64
#define GLOB_NOSPACE  (-1)
#define GLOB_ABORTED  (-2)
#define GLOB_NOMATCH  (-3)

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob);
void globfree(glob_t *pglob);

#ifdef __cplusplus
}
#endif
#endif
