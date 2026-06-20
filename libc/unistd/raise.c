#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
int raise(int sig) {
    return (int)syscall(SYS_kill, syscall(SYS_getpid,0,0,0), sig, 0);
}
