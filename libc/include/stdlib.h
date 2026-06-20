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

// ── Environment ────────────────────────────────────────────

extern char **environ;

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);
int   clearenv(void);

#ifdef __cplusplus
}
#endif
void *realloc(void *ptr, size_t size);
int rand(void);
char *realpath(const char *path, char *resolved);
void srand(unsigned int seed);

#endif /* _STDLIB_H */
void *realloc(void *ptr, size_t size);
int rand(void);
char *realpath(const char *path, char *resolved);
void srand(unsigned int seed);
