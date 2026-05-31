#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <sys/syscall.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read from a file at `path` into `buffer` (up to `size` bytes).
// Returns bytes read, or -1 on error.
int64_t read(const char *path, void *buffer, uint64_t size);

#ifdef __cplusplus
}
#endif

#endif
