#include <libgen.h>

char *dirname(char *path)  { (void)path; return "/"; }
char *basename(char *path) { (void)path; return ""; }
