#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_FAILED  ((void *)-1)
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
int   msync(void *addr, size_t length, int flags);
int   shm_open(const char *name, int oflag, mode_t mode);
int   shm_unlink(const char *name);
#ifdef __cplusplus
}
#endif

#endif
