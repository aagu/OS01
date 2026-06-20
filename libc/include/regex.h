#ifndef _REGEX_H
#define _REGEX_H

typedef int regex_t;
typedef int regmatch_t;
#define REG_EXTENDED 1
#define REG_NOSUB 0
int regcomp(regex_t *preg, const char *regex, int cflags) { (void)preg; (void)regex; (void)cflags; return 0; }
int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags) { (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags; return 0; }
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) { (void)errcode; (void)preg; (void)errbuf; return errbuf_size; }
void regfree(regex_t *preg) { (void)preg; }

#endif
