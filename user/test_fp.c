/* user/test_fp.c — floating-point and SIMD smoke test (AArch64 & x86_64). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int check_arith(void)
{
    volatile double a = 3.5;
    volatile double b = 2.25;
    volatile double sum = a + b;
    volatile double prod = a * b;
    volatile double quot = a / b;
    volatile float fa = 1.5f;
    volatile float fb = 0.25f;
    volatile float fsum = fa + fb;

    if (sum != 5.75) {
        printf("FAIL: sum=%f expected 5.75\n", sum);
        return 1;
    }
    if (prod != 7.875) {
        printf("FAIL: prod=%f expected 7.875\n", prod);
        return 1;
    }
    if (quot < 1.555555 || quot > 1.555556) {
        printf("FAIL: quot=%f expected 1.555556\n", quot);
        return 1;
    }
    if (fsum != 1.75f) {
        printf("FAIL: fsum=%f expected 1.75\n", (double)fsum);
        return 1;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f", prod);
    if (strcmp(buf, "7.8750") != 0) {
        printf("FAIL: snprintf=%.4f got '%s'\n", prod, buf);
        return 1;
    }
    return 0;
}

static int check_fork_and_switch(void)
{
    volatile double parent_val = 42.125;

#ifdef __aarch64__
    /* Load callee-saved d8 and caller-saved d20 with known bit patterns. */
    uint64_t expected_d8  = 0x400921fb54442d18ULL; /* pi */
    uint64_t expected_d20 = 0x4005bf0a8b145769ULL; /* e */
    __asm__ volatile(
        "fmov d8,  %0\n\t"
        "fmov d20, %1\n\t"
        :: "r"(expected_d8), "r"(expected_d20)
        : "d8", "d20"
    );
#endif

    int pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed (%d)\n", pid);
        return 1;
    }
    if (pid == 0) {
#ifdef __aarch64__
        uint64_t got_d8 = 0, got_d20 = 0;
        __asm__ volatile(
            "fmov %0, d8\n\t"
            "fmov %1, d20\n\t"
            : "=r"(got_d8), "=r"(got_d20)
        );
        if (got_d8 != expected_d8 || got_d20 != expected_d20) {
            printf("FAIL: child did not inherit FP regs (%llx, %llx)\n",
                   (unsigned long long)got_d8, (unsigned long long)got_d20);
            _exit(2);
        }
        /* Clobber FP registers in child before yielding and exiting. */
        uint64_t poison = 0xdeadbeefcafebabeULL;
        __asm__ volatile(
            "fmov d8,  %0\n\t"
            "fmov d20, %0\n\t"
            :: "r"(poison)
            : "d8", "d20"
        );
#endif
        volatile double child_val = parent_val * 2.0;
        if (child_val != 84.25)
            _exit(3);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L };
        /* aarch64 hides the time syscall group (roadmap 能力齐平):
         * nanosleep returns -ENOSYS immediately, which is fine — the
         * exit path itself forces the switch under test. */
        (void)nanosleep(&ts, NULL);
        _exit(0);
    }

    int status = 0;
    int w = waitpid(pid, &status, 0);
    if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: waitpid w=%d status=0x%x\n", w, status);
        return 1;
    }

#ifdef __aarch64__
    uint64_t after_d8 = 0, after_d20 = 0;
    __asm__ volatile(
        "fmov %0, d8\n\t"
        "fmov %1, d20\n\t"
        : "=r"(after_d8), "=r"(after_d20)
    );
    if (after_d8 != expected_d8 || after_d20 != expected_d20) {
        printf("FAIL: parent FP regs clobbered (%llx, %llx)\n",
               (unsigned long long)after_d8, (unsigned long long)after_d20);
        return 1;
    }
#endif
    if (parent_val != 42.125) {
        printf("FAIL: parent_val=%f\n", parent_val);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (check_arith() != 0)
        return 1;
    if (check_fork_and_switch() != 0)
        return 1;
    printf("test_fp: PASS\n");
    return 0;
}
