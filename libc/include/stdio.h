#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)

/* FILE is an OPAQUE type: the public header declares the tag only, and the
 * real definition lives in the private header <stdio/file.h>.  A caller can
 * therefore never depend on the layout of a stream. */
typedef struct __FILE FILE;

#define ZEROPAD	1		/* pad with zero */
#define SIGN	2		/* unsigned/signed long */
#define PLUS	4		/* show plus */
#define SPACE	8		/* space if plus */
#define LEFT	16		/* left justified */
#define SPECIAL	32		/* 0x */
#define SMALL	64		/* use 'abcdef' instead of 'ABCDEF' */

/* do_div: per-arch inline asm.
 *   x86_64 : divq — single instruction, quotient in rax, remainder in rdx.
 *   aarch64: udiv + msub — two instructions, quotient via udiv, remainder
 *            via msub = dividend - quotient*divisor. The dividend is read
 *            twice (udiv's source operand Xn + msub's source operand Xa);
 *            no constraint tying is used, so the compiler doesn't merge
 *            those reads — each asm instruction re-loads __qn.
 *   else   : portable C fallback.
 *
 * libc/stdio is x86_64-only on the libc userland side (the Makefile
 * gates libc.a to x86_64), so the asm path covers both x86_64 libc
 * userland and x86_64 libk kernel-side builds. */
#if defined(__x86_64__)
#define do_div(n,base) ({ \
	int __res; \
	__asm__("divq %%rcx":"=a" (n),"=d" (__res):"0" (n),"1" (0),"c" (base)); \
	__res; })
#elif defined(__aarch64__)
#define do_div(n,base) ({ \
    unsigned long __b = (unsigned long)(base); \
    unsigned long long __qn = (unsigned long long)(n); \
    unsigned long long __q; \
    unsigned long __rem; \
    __asm__ volatile ("udiv %0, %1, %2" \
                      : "=r"(__q) \
                      : "r"(__qn), "r"(__b)); \
    __asm__ volatile ("msub %0, %3, %1, %2" \
                      : "=r"(__rem) \
                      : "r"(__q), "r"(__qn), "r"(__b)); \
    (n) = __q; \
    __rem; })
#else
#define do_div(n,base) ({ \
    unsigned long long __q = (n); \
    (n) = __q / (unsigned long)(base); \
    (unsigned long)(__q % (unsigned long)(base)); \
})
#endif
#define is_digit(c)	((c) >= '0' && (c) <= '9')

int printf(const char* __restrict, ...);
int putchar(int);
int puts(const char*);
int vsprintf(char * buf,const char *fmt, va_list args);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sscanf(const char *str, const char *fmt, ...);
int skip_atoi(const char **s);
int dprintf(int fd, const char *fmt, ...);
int vdprintf(int fd, const char *fmt, va_list ap);
void perror(const char *s);
int vasprintf(char **strp, const char *fmt, va_list ap);
int asprintf(char **strp, const char *fmt, ...);
int fileno_unlocked(FILE *f);

/* ── Stream lifecycle ───────────────────────────────────────────────
 *
 * fopen/fdopen hand back a FILE* that owns a real stream state; fclose
 * closes it.  freopen re-points an existing stream at a new file.
 */
/* Seek whence values (POSIX: <stdio.h>). */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int   fclose(FILE *f);
int   fflush(FILE *f);

/* Stream positioning (T9).  fseeko/ftello are the off_t forms; the
 * plain fseek/ftell call them (off_t is long here).  A successful seek
 * discards the pushback byte and clears the EOF indicator; rewind()
 * additionally clears the error indicator.  An fseek(0, SEEK_CUR) on
 * a stream holding an ungetc'd byte re-positions to the stream's
 * LOGICAL position (physical minus the pushed-back byte). */
int   fseek(FILE *f, long off, int whence);
int   fseeko(FILE *f, long off, int whence);
long  ftell(FILE *f);
off_t ftello(FILE *f);
void  rewind(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *p, size_t s, size_t n, FILE *f);

/* ── Stream status indicators ──────────────────────────────────────
 *
 * The EOF indicator is sticky: it is set when a read reaches end-of-file
 * and stays set until clearerr() (or a successful ungetc()) clears it.
 * The error indicator is set by an I/O failure — an error is NOT an
 * end-of-file, so it never sets the EOF indicator.
 */
int  feof(FILE *f);
int  ferror(FILE *f);
void clearerr(FILE *f);
int  feof_unlocked(FILE *f);
int  ferror_unlocked(FILE *f);
void clearerr_unlocked(FILE *f);

/* ungetc pushes one byte back onto a readable stream.  EOF is not a valid
 * pushback character, and only ONE byte may be held at a time.  A
 * successful ungetc clears the EOF indicator. */
int  ungetc(int c, FILE *f);

/* ── Character and string I/O ────────────────────────────────────── */
int   getc_unlocked(FILE *f);
int   putc_unlocked(int c, FILE *f);
#define fputs_unlocked(s,f) fputs(s,f)
int   fputs(const char *s, FILE *f);
int   getchar(void);
int   getchar_unlocked(void);
char *fgets_unlocked(char *s, int size, FILE *f);

/* The standard character spellings (T8).  They share the T5 stream
 * state — resolver, sticky EOF/error indicators, the single pushback
 * byte, capability checks — so getc and getc_unlocked observe exactly
 * the same stream (this libc is single-threaded; the unlocked pair
 * differs in name only, like every other unlocked pair here). */
int   fgetc(FILE *f);
int   getc(FILE *f);
int   putc(int c, FILE *f);
int   fileno(FILE *f);
char *fgets(char *s, int size, FILE *f);

/* Dynamic delimited line reading (T8): getdelim reads until `delim`
 * (inclusive) or EOF, growing the caller's buffer; getline is the
 * newline form.  Returns the byte count including the delimiter, or
 * -1 when nothing was read and the stream hit EOF/error.  On failure
 * *lineptr and *n stay consistent and the buffer remains freeable. */
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f);
ssize_t getline(char **lineptr, size_t *n, FILE *f);

/* ── Formatted output ────────────────────────────────────────────── */
int vprintf(const char *fmt, __builtin_va_list ap);
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap);
int fprintf(FILE *f, const char *fmt, ...);
int vsnprintf(char *b, unsigned long s, const char *fmt, va_list ap);
int putchar_unlocked(int c);
int fputc(int c, FILE *f);

/* ── The standard streams ──────────────────────────────────────────
 *
 * The sentinel VALUES are kept (they are part of the historical ABI and
 * appear in user code), but they are never dereferenced: the libc's
 * resolver maps them onto three real stream states, so stdin, stdout and
 * stderr carry exactly the same EOF / error / pushback state as any
 * fopen()-ed stream. */
#define stdin  ((FILE*)1)
#define stdout ((FILE*)2)
#define stderr ((FILE*)3)

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */
