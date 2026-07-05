// test_fork_mmap — fork + mmap isolation test
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("test_fork_mmap: testing fork+mmap isolation...\n");

    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap returned MAP_FAILED\n");
        return 1;
    }

    ((int *)p)[0] = 42;

    int64_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        // Child: write different value
        ((int *)p)[0] = 99;
        printf("  child: p[0]=%d (expected 99)\n", ((int *)p)[0]);
        if (((int *)p)[0] != 99) {
            printf("FAIL: child write failed\n");
            exit(1);
        }
        exit(0);
    }

    // Parent: wait and verify its value wasn't changed
    int status;
    waitpid(pid, &status, 0);
    printf("  parent: p[0]=%d (expected 42)\n", ((int *)p)[0]);
    if (((int *)p)[0] != 42) {
        printf("FAIL: parent saw child's write\n");
        return 1;
    }

    munmap(p, 4096);
    printf("PASS: fork+mmap isolation\n");
    return 0;
}
