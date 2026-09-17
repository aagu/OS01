// user/canary_dump.c — guard 可观测性 probe（spec 2026-09-17 Task 1）。
// 打印本进程 canary 后退出；systest 44 号经 pipe 捕获、10 次 exec 比对，
// 证明每次 exec 重新播种。
#include <stdio.h>
#include <sys/ssp.h>

int main(void)
{
    printf("GUARD %016lx\n", (unsigned long)__stack_chk_guard);
    fflush(stdout);                 /* crt0 SYS_exit 不走 atexit */
    return 0;
}