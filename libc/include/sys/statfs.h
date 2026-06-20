#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H 1

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Filesystem magic numbers */
#define ADFS_SUPER_MAGIC      0xadf5
#define AFFS_SUPER_MAGIC      0xadff
#define BEFS_SUPER_MAGIC      0x42465331
#define BFS_MAGIC             0x1BADFACE
#define CIFS_MAGIC_NUMBER     0xFF534D42
#define CODA_SUPER_MAGIC      0x73757245
#define CRAMFS_MAGIC          0x28cd3d45
#define DEBUGFS_MAGIC         0x64626720
#define DEVPTS_SUPER_MAGIC    0x1cd1
#define EFS_SUPER_MAGIC       0x00414A53
#define EXT_SUPER_MAGIC       0x137D
#define EXT2_OLD_SUPER_MAGIC  0xEF51
#define EXT2_SUPER_MAGIC      0xEF53
#define EXT3_SUPER_MAGIC      0xEF53
#define EXT4_SUPER_MAGIC      0xEF53
#define FUSE_SUPER_MAGIC      0x65735546
#define HUGETLBFS_MAGIC       0x958458f6
#define ISOFS_SUPER_MAGIC     0x9660
#define JFFS2_SUPER_MAGIC     0x72b6
#define JFS_SUPER_MAGIC       0x3153464a
#define MINIX_SUPER_MAGIC     0x137F
#define MINIX_SUPER_MAGIC2    0x138F
#define MINIX2_SUPER_MAGIC    0x2468
#define MINIX2_SUPER_MAGIC2   0x2478
#define MSDOS_SUPER_MAGIC     0x4d44
#define NCP_SUPER_MAGIC       0x564c
#define NFS_SUPER_MAGIC       0x6969
#define NTFS_SB_MAGIC         0x5346544e
#define OPENPROM_SUPER_MAGIC  0x9fa1
#define PROC_SUPER_MAGIC      0x9fa0
#define QNX4_SUPER_MAGIC      0x002f
#define REISERFS_SUPER_MAGIC  0x52654973
#define ROMFS_MAGIC           0x7275
#define SMB_SUPER_MAGIC       0x517B
#define SYSV2_SUPER_MAGIC     0x012FF7B6
#define SYSV4_SUPER_MAGIC     0x012FF7B5
#define TMPFS_MAGIC           0x01021994
#define UDF_SUPER_MAGIC       0x15013346
#define UFS_MAGIC             0x00011954
#define USBDEVICE_SUPER_MAGIC 0x9fa2
#define XENIX_SUPER_MAGIC     0x012FF7B4
#define XFS_SUPER_MAGIC       0x58465342

struct statfs {
    long f_type;     /* type of filesystem */
    long f_bsize;    /* optimal transfer block size */
    long f_blocks;   /* total data blocks in filesystem */
    long f_bfree;    /* free blocks in fs */
    long f_bavail;   /* free blocks available to unprivileged user */
    long f_files;    /* total file nodes in filesystem */
    long f_ffree;    /* free file nodes in fs */
    long f_fsid;     /* filesystem id */
    long f_namelen;  /* maximum length of filenames */
    long f_frsize;   /* fragment size */
    long f_flags;    /* mount flags */
    long f_spare[4]; /* spare for later */
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#ifdef __cplusplus
}
#endif

#endif
