#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H 1
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stddef.h>
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 32
#define MAP_FAILED ((void*)-1)
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
#endif
