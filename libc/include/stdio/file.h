#ifndef _STDIO_FILE_H
#define _STDIO_FILE_H 1

/*
 * libc/include/stdio/file.h — the PRIVATE stream-state header.
 *
 * The public <stdio.h> keeps FILE opaque: it declares
 * `typedef struct __FILE FILE;` and nothing else, so a caller can never
 * depend on the layout of a stream.  The real definition lives here, in a
 * private header, and is reached only by the libc's own translation units.
 *
 * Why the state has to be a real object
 * -------------------------------------
 * The historical representation was `mini_file_t { int fd; int mode; }`,
 * published in the public header.  It could not express anything a
 * stdio stream is required to remember:
 *
 *   - the EOF indicator (sticky once a read hits end-of-file),
 *   - the error indicator (set by an I/O failure, NOT by end-of-file),
 *   - the single pushback byte ungetc() may hold,
 *   - whether the stream may be read from / written to,
 *   - the direction of the last I/O operation,
 *   - whether the stream has been closed.
 *
 * Those flags are what feof()/ferror()/clearerr()/ungetc() report, and
 * what distinguishes a standard stream from a plain file.  The state is
 * therefore a first-class object, and the standard streams get one each
 * (see the sentinel mapping in libc/stdio/stdio_file.c).
 *
 * Single-threaded by design: OS01's userspace runs one thread per
 * process, so the stream state needs no locking.
 */

#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a stream may be used for.  A stream created by fopen("r") is
 * read-only; fopen("w")/fopen("a") are write-only; the "+" forms are
 * both.  The capability — not the raw fd number — is what decides
 * whether fread()/fwrite() accept the stream: a *registered* stream
 * whose underlying fd happens to be 1 is a perfectly good writable
 * stream, and must not be rejected because its fd looks like stdout. */
#define __STDIO_READ  0x01
#define __STDIO_WRITE 0x02

/* Direction of the most recent I/O operation on the stream. */
#define __STDIO_OP_NONE  0
#define __STDIO_OP_READ  1
#define __STDIO_OP_WRITE 2

/* The stream state.  Every fopen()/fdopen() result is one of these, and
 * so are the three standard streams. */
struct __FILE {
    int fd;        /* underlying kernel file descriptor */
    int caps;      /* __STDIO_READ / __STDIO_WRITE */
    int eof;       /* sticky end-of-file indicator */
    int error;     /* sticky I/O-error indicator */
    int pending;   /* the single pushback byte, or -1 when there is none */
    int last_op;   /* __STDIO_OP_* — direction of the last I/O */
    int closed;    /* the stream was closed and must not be used again */
    int owns_fd;   /* fclose() closes the underlying fd */
};

/* ── The internal resolver ───────────────────────────────────────────
 *
 * __stdio_resolve() is the ONE place that maps a FILE* to its state:
 *
 *   stdin/stdout/stderr sentinel → the matching standard-stream state
 *   a registered stream          → its own state
 *   unknown / closed / stale     → NULL
 *
 * Every FILE consumer goes through it (directly or through
 * __stdio_fd()), so an unregistered or already-closed FILE* can never be
 * dereferenced as a stream.  __stdio_fd() is the "give me the fd"
 * shortcut and returns -1 for everything the resolver rejects.
 */
FILE *__stdio_resolve(FILE *f);
int  __stdio_fd(FILE *f);

/* ── Shared stream I/O helpers ───────────────────────────────────────
 *
 * These update the stream state as a side effect (EOF / error flags,
 * last I/O direction), which is what makes the flags observable by
 * feof()/ferror() no matter which entry point performed the I/O.
 */

/* Write len bytes to the stream.  Returns the number of bytes written,
 * or -1 on failure (the stream's error indicator is then set). */
long __stdio_write(FILE *f, const char *buf, size_t len);

/* Read one byte.  Returns the byte (0..255), or EOF when the stream hit
 * end-of-file (EOF indicator set) or an I/O error (error indicator set).
 * A pending pushback byte is returned first. */
int  __stdio_read_byte(FILE *f);

/* Write one byte.  Returns the byte, or EOF on failure. */
int  __stdio_write_byte(FILE *f, int c);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_FILE_H */
