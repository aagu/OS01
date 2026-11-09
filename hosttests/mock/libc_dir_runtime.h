/*
 * libc_dir_runtime.h — the fake directory boundary of the `dir` host
 * group (T15).  See libc_dir_runtime.c for what is simulated and what the
 * real kernel does.
 */
#ifndef LIBC_DIR_RUNTIME_H
#define LIBC_DIR_RUNTIME_H

#include <stdint.h>

void dir_rt_reset(void);
void dir_rt_add_path(const char *path, int is_dir);
void dir_rt_add_entry(const char *path, const char *name, unsigned char type,
                      uint64_t d_off);
void dir_rt_set_getdents_error(const char *path, int err);
void dir_rt_set_bad_record(const char *path, int mode);
int  dir_rt_fd_is_ours(int fd);

#endif /* LIBC_DIR_RUNTIME_H */
