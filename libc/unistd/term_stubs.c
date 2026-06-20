#include <stddef.h>
int tcflush(int fd, int q) { (void)fd; (void)q; return -1; }
unsigned int alarm(unsigned int s) { (void)s; return 0; }
