#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

/* FILE type - stub for headers that reference it */
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
    __asm__ volatile ("udiv %0, %2, %1" \
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
int ferror_unlocked(void *f);
int clearerr(void *f);
void perror(const char *s);
void *fopen(const char *path, const char *mode);
void *fdopen(int fd, const char *mode);
void *freopen(const char *path, const char *mode, void *f);
int fclose(void *f);
int fseeko(void *f, long off, int whence);
size_t fread(void *ptr, size_t size, size_t nmemb, void *f);
int fflush(void *f);

int vasprintf(char **strp, const char *fmt, va_list ap);
int fileno_unlocked(FILE *f);

#define stdin  ((FILE*)1)
#define stdout ((FILE*)2)
#define stderr ((FILE*)3)

/* Minimal FILE struct shared by fopen/fdopen/fread/fwrite/fgets/getc */
typedef struct {
    int fd;
    int mode;  /* 0=read, 1=write */
} mini_file_t;

int getc_unlocked(void *f);
int putc_unlocked(int c, void *f);
#define fputs_unlocked(s,f) fputs(s,f)

int fputs(const char *s, void *f);
int getchar(void);
int getchar_unlocked(void);
char *fgets_unlocked(char *s, int size, void *f);
ssize_t getline(char **lineptr, size_t *n, void *f);
int getchar_unlocked(void);
char *fgets_unlocked(char *s, int size, void *f);
ssize_t getline(char **lineptr, size_t *n, void *f);

int fflush(void *f);
int vprintf(const char *fmt, __builtin_va_list ap);
int vfprintf(void *f, const char *fmt, __builtin_va_list ap);
int fprintf(void *f, const char *fmt, ...);
int vsnprintf(char *b, unsigned long s, const char *fmt, va_list ap);
int putchar_unlocked(int c);
int fputc(int c, void *f);
size_t fwrite(const void *p, size_t s, size_t n, void *f);

#endif /* _STDIO_H */
