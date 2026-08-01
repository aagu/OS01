#include <unistd.h>
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
