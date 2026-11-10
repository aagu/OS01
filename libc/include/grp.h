#ifndef _GRP_H
#define _GRP_H 1
#include <sys/cdefs.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
struct group { char *gr_name; gid_t gr_gid; char **gr_mem; };
struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t group);
#ifdef __cplusplus
}
#endif
#endif
