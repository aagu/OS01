// test_cow — COW fork isolation tests
//
// Tests:
//  1. cow_basic:        fork, both write, verify isolation
//  2. cow_fork_of_fork: P1->P2->P3 chain, all write, verify isolation
//  3. cow_mprotect:     fork, child PROT_NONE->restore->write, verify isolation
//  4. cow_exec:         fork, child execs /bin/spin, parent writes (last-ref promote)
//  5. cow_exit:         fork, child exits, parent writes (in-place promote)
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void check(int cond, const char *msg)
{
    if (!cond) { printf("FAIL: %s\n", msg); failures++; }
}

int main(void)
{
    printf("test_cow: COW fork isolation tests\n");

    // -- Test 1: cow_basic --
    {
        printf("  cow_basic: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0x42;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            ((int *)p)[0] = 0x99;
            if (((int *)p)[0] != 0x99) exit(1);
            exit(0);
        }

        int st; waitpid(pid, &st, 0);
        check(((int *)p)[0] == 0x42, "parent sees child write");
        check(st == 0, "child exit 0");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // -- Test 2: cow_fork_of_fork --
    {
        printf("  cow_fork_of_fork: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 1;

        int64_t p1 = fork();   // P1
        check(p1 >= 0, "fork1");

        if (p1 == 0) {
            int64_t p2 = fork(); // P2
            check(p2 >= 0, "fork2");

            if (p2 == 0) {
                // P3: grandchild
                ((int *)p)[0] = 3;
                if (((int *)p)[0] != 3) exit(1);
                exit(0);
            }
            // P2: child
            ((int *)p)[0] = 2;
            if (((int *)p)[0] != 2) exit(1);
            int st2; waitpid(p2, &st2, 0);
            check(st2 == 0, "grandchild exit 0");
            exit(0);
        }

        // P1: parent
        int st1; waitpid(p1, &st1, 0);
        check(st1 == 0, "p2 exit 0");
        check(((int *)p)[0] == 1, "parent isolated from children");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // -- Test 3: cow_mprotect --
    {
        printf("  cow_mprotect: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0x11;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            // Child: stash page via PROT_NONE, then restore and write
            int rc = mprotect(p, 4096, PROT_NONE);
            check(rc == 0, "mprotect PROT_NONE");
            // Page is now stashed (PROTNONE+COW) -- should not fault
            rc = mprotect(p, 4096, PROT_READ | PROT_WRITE);
            check(rc == 0, "mprotect restore");
            // Now write -- COW should allocate a private copy
            ((int *)p)[0] = 0x22;
            if (((int *)p)[0] != 0x22) exit(1);
            exit(0);
        }

        int st; waitpid(pid, &st, 0);
        check(st == 0, "child exit 0");
        check(((int *)p)[0] == 0x11, "parent isolated from mprotect child");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // -- Test 4: cow_exec --
    {
        printf("  cow_exec: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0xAB;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            // Child execs /bin/spin (a small binary that exits 42).
            // exec replaces mm -> child's COW refs are released.
            char *argv[] = {"/bin/spin", NULL};
            execve("/bin/spin", argv, NULL);
            exit(99); // exec failed
        }

        int st; waitpid(pid, &st, 0);
        check(st == (42 << 8), "spin exit 42");
        // Parent writes -- should be last reference, in-place promote.
        ((int *)p)[0] = 0xCD;
        check(((int *)p)[0] == 0xCD, "parent write after exec");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // -- Test 5: cow_exit --
    {
        printf("  cow_exit: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0x77;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            exit(0);  // child exits without writing
        }

        int st; waitpid(pid, &st, 0);
        check(st == 0, "child exit 0");
        // Parent writes -- child is gone, cow_count should be 1.
        // In-place promote (no copy needed).
        ((int *)p)[0] = 0x88;
        check(((int *)p)[0] == 0x88, "parent write after child exit");
        munmap(p, 4096);
        printf("PASS\n");
    }

    if (failures == 0)
        printf("test_cow: ALL PASS\n");
    else
        printf("test_cow: %d FAILURES\n", failures);

    return failures > 0 ? 1 : 0;
}
