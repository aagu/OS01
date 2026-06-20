#include <stddef.h>
#include <time.h>
struct tm *localtime_r(const time_t *t, struct tm *r) { (void)t; (void)r; return NULL; }
struct tm *gmtime_r(const time_t *t, struct tm *r) { (void)t; (void)r; return NULL; }
char *ctime_r(const time_t *t, char *b) { (void)t; (void)b; return NULL; }
char *asctime_r(const struct tm *t, char *b) { (void)t; (void)b; return NULL; }
char *ctime(const time_t *t) { (void)t; return NULL; }
char *asctime(const struct tm *t) { (void)t; return NULL; }
