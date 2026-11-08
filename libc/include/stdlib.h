#ifndef _STDLIB_H
#define _STDLIB_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void *realloc(void *ptr, size_t size);
int rand(void);
char *realpath(const char *path, char *resolved);
void srand(unsigned int seed);

#define alloca(x) __builtin_alloca(x)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

__attribute__((__noreturn__))
void abort(void);

void* malloc (size_t size);
void* calloc (size_t n, size_t size);
void free(void * ptr);

/* Realloc with an overflow-checked size: the new size is nmemb * size and
 * the product must not wrap around.  A failing call returns NULL and
 * leaves the old block untouched. */
void *reallocarray(void *ptr, size_t nmemb, size_t size);

#ifndef __is_libk
/* Aligned allocation is a user-libc capability: the kernel side (libk)
 * keeps the plain slab allocator and does not define these symbols, so
 * the declarations are hidden in __is_libk mode.  POSIX puts them in
 * <stdlib.h>, so they are declared here as well as in <malloc.h>.
 *
 * posix_memalign requires an alignment that is a power of two and a
 * multiple of sizeof(void *); it reports a failure through its return
 * value (EINVAL / ENOMEM) and never through errno.
 * aligned_alloc additionally requires a size that is a multiple of the
 * alignment. */
extern int posix_memalign(void **, size_t, size_t);
extern void *aligned_alloc(size_t, size_t);
#endif

// ── String conversion ────────────────────────────────────────
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int atoi(const char *nptr);
long atol(const char *nptr);

long long atoll(const char *nptr);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

// ── Sorting ──────────────────────────────────────────────────
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

// ── Exit handlers ────────────────────────────────────────────
int atexit(void (*function)(void));

__attribute__((__noreturn__))
void exit(int status);

// ── Environment ────────────────────────────────────────────
// One environment table.  `environ` is the public array of "NAME=VALUE"
// strings; the libc keeps a parallel record of the entries it owns.
//
//   * getenv() returns the value of the first entry with that name, or
//     NULL when there is none.
//   * setenv() copies the value: the copy belongs to the libc and is
//     released when it is replaced, removed, or the environment cleared.
//     overwrite == 0 leaves an existing value alone and allocates nothing.
//   * unsetenv() removes EVERY entry with that name.
//   * putenv() borrows the caller's string: the libc stores the pointer
//     and never frees it.  A string without '=' removes the variable.
//   * clearenv() empties the environment.  It never frees an inherited
//     string (the block the kernel handed to crt0) nor a borrowed one.
//
// An invalid name (empty, or containing '=') and a NULL value are rejected
// with -1 and errno == EINVAL; a failed allocation returns -1 with
// errno == ENOMEM and leaves the environment unchanged.

extern char **environ;

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);
int   clearenv(void);

double strtod(const char *nptr, char **endptr);
double atof(const char *nptr);

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */
