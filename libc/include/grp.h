#ifndef _GRP_H
#define _GRP_H 1
#include <sys/cdefs.h>
#include <sys/types.h>
struct group { char *gr_name; gid_t gr_gid; char **gr_mem; };
struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
#endif
