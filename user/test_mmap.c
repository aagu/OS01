// test_mmap — standalone mmap/munmap/mprotect test
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    printf("test_mmap: testing anonymous mmap...\n");

    // Test 1: basic anon mmap + write + read
    size_t sz = 4096 * 4;  // 16KB
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap returned MAP_FAILED\n");
        return 1;
    }
    printf("  mmap: %p\n", p);

    memset(p, 0xAB, sz);
    if (((unsigned char *)p)[0] != 0xAB) {
        printf("FAIL: write/read mismatch\n");
        return 1;
    }
    printf("  write+read: OK\n");

    int rc = munmap(p, sz);
    if (rc != 0) {
        printf("FAIL: munmap returned %d\n", rc);
        return 1;
    }
    printf("  munmap: OK\n");

    // Test 2: mprotect PROT_NONE (don't touch — would SIGSEGV)
    void *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED) {
        printf("FAIL: mmap2 returned MAP_FAILED\n");
        return 1;
    }
    memset(q, 0x42, 4096);
    rc = mprotect(q, 4096, PROT_NONE);
    if (rc != 0) {
        printf("FAIL: mprotect(PROT_NONE) returned %d\n", rc);
        return 1;
    }
    printf("  mprotect(PROT_NONE): OK\n");
    munmap(q, 4096);

    // Test 3: mprotect PROT_NONE -> PROT_READ preserves data
    void *r = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) {
        printf("FAIL: mmap3 returned MAP_FAILED\n");
        return 1;
    }
    ((unsigned char *)r)[0] = 0x77;
    mprotect(r, 4096, PROT_NONE);
    mprotect(r, 4096, PROT_READ);
    if (((unsigned char *)r)[0] != 0x77) {
        printf("FAIL: PROT_NONE->PROT_READ data lost\n");
        return 1;
    }
    printf("  mprotect(PROT_NONE->PROT_READ): data preserved OK\n");
    munmap(r, 4096);

    printf("PASS: all mmap tests\n");
    return 0;
}
