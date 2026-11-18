// hello — aarch64 userland build-chain smoke program (M4-remaining Task 1).
// Exercises crt0 (__libc_start_main), libc printf (integer-only on this
// arch), and the SVC syscall path (write + exit).  Not installed into any
// rootfs manifest; consumed by the build-chain verification only.

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("hello: pid=%d\n", (int)getpid());
    return 0;
}
