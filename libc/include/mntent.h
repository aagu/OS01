#ifndef _MNTENT_H
#define _MNTENT_H 1

#include <sys/cdefs.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MNTTYPE_FUSE "fuse"

struct mntent {
    char *mnt_fsname;   /* device or server */
    char *mnt_dir;      /* mount point */
    char *mnt_type;     /* filesystem type */
    char *mnt_opts;     /* mount options */
    int   mnt_freq;     /* dump frequency */
    int   mnt_passno;   /* fsck pass number */
};

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
int endmntent(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
