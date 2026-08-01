#include <grp.h>
#include <sys/types.h>    /* gid_t */

struct group *getgrnam(const char *name) { (void)name; return NULL; }
struct group *getgrgid(gid_t gid)        { (void)gid; return NULL; }

int setgroups(size_t s, const gid_t *l)  { (void)s; (void)l; return 0; }
int initgroups(const char *u, gid_t g)  { (void)u; (void)g; return 0; }
