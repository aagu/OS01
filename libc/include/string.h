#ifndef _STRING_H
#define _STRING_H 1

#include <sys/cdefs.h>

#include <stddef.h>

int memcmp(const void*, const void*, size_t);
int bcmp(const void*, const void*, size_t);
void* memcpy(void* __restrict, const void* __restrict, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
char* strcpy(char* dest, const char* src);
char* strdup(const char* src);

// Extended string functions
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **save_ptr);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);

// Error reporting
char *strerror(int errnum);
void perror(const char *s);

#endif
char *strchrnul(const char *s, int c);
void *mempcpy(void *d, const void *s, size_t n);
void *mempcpy(void *d, const void *s, size_t n);
char *stpncpy(char *d, const char *s, size_t n);
char *strsignal(int sig);

char *stpcpy(char *d, const char *s);

/* String comparison ignoring case */
int strcasecmp(const char *s1, const char *s2);
char *strndup(const char *s, size_t n);
int strncasecmp(const char *s1, const char *s2, size_t n);
