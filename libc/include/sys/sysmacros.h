#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H 1

#include <sys/cdefs.h>

#define major(dev)   ((unsigned int)(((dev) >> 8) & 0xfff))
#define minor(dev)   ((unsigned int)(((dev) & 0xff) | (((dev) >> 12) & 0xfff00)))
#define makedev(ma, mi) \
    ((((ma) & 0xfff) << 8) | (((mi) & 0xfff00) << 12) | ((mi) & 0xff))

#endif
