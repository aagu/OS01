/* hosttests/mock/libc_completion_runtime.h
 *
 * Host-test runtime surface for the OS01 libc completion work.
 *
 * ── The production-symbol prefix ────────────────────────────────────────
 * A host test must never mistake the host libc's implementation of a
 * POSIX name for the OS01 implementation.  The completion work therefore
 * compiles the relevant production libc sources under an `os01_` prefix
 * and the test translation units call the prefixed names.
 *
 * ONE mechanism is used, consistently, for both sides:
 *
 *     -D<name>=os01_<name>            (one -D per prefixed function)
 *
 * The flags live in `hosttests/mk/common.mk`
 * (`OS01_LIBC_PREFIX_DEFS`) and are applied to
 *
 *   1. the production sources compiled for the host test
 *      (`$(OS01_LIBC_PREFIX_OBJS)`), and
 *   2. the test translation units that call them.
 *
 * The prefix is a HOST-TEST-ONLY convention: `libc.a` and `libk.a` are
 * built without these flags, so an `os01_` name never reaches a shipped
 * artifact and the userland ABI of the public libc headers is unchanged.
 *
 * Only true boundary functions are replaced with test doubles (the raw
 * syscall entry, read/write and the allocator entry points needed for
 * fault injection).  Everything else under test is real production code.
 */
#ifndef OS01_LIBC_COMPLETION_RUNTIME_H
#define OS01_LIBC_COMPLETION_RUNTIME_H

#include <stddef.h>

/* The prefix itself, and the helper that spells a prefixed name. */
#define OS01_LIBC_PREFIX      "os01_"
#define OS01_LIBC_FN(name)    os01_##name

/* The production entry points the host test links under their prefixed
 * names.  These are the REAL libc string sources, compiled
 * with the prefix flags; they are declared here so a test translation
 * unit can name them without relying on the -D rename being visible in
 * every header it includes.  The signatures match `libc/include/string.h`.
 */
void *os01_memcpy(void *dst, const void *src, size_t n);
void *os01_memmove(void *dst, const void *src, size_t n);
void *os01_memset(void *dst, int c, size_t n);
int    os01_memcmp(const void *a, const void *b, size_t n);
size_t os01_strlen(const char *s);
int    os01_strcmp(const char *a, const char *b);
int    os01_strncmp(const char *a, const char *b, size_t n);
char  *os01_strcpy(char *dst, const char *src);

/* ── Host boundary used by the api probe harness ────────────────────────
 * The api group drives the host compiler to prove the public headers are
 * self-contained, single-definition and C-linkage safe.  `system()` is
 * the only host facility the harness needs; it is declared here because
 * `libc/include/stdlib.h` does not (and must not) expose it.
 */
int system(const char *command);

#endif /* OS01_LIBC_COMPLETION_RUNTIME_H */
