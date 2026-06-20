#include <glob.h>
#include <stdlib.h>
#include <string.h>

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob) {
    (void)pattern; (void)flags; (void)errfunc;
    if (pglob) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }
    return GLOB_NOMATCH;
}

void globfree(glob_t *pglob) {
    if (pglob && pglob->gl_pathv) {
        free(pglob->gl_pathv);
        pglob->gl_pathv = NULL;
    }
}
