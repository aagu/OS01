/* smp_stress.c — SMP load balance verification
 *
 * Spawns N CPU-bound processes that compute prime numbers in a tight
 * loop. Each process periodically prints [pid] <progress> so you can
 * watch concurrent execution across cores.
 *
 * Build (inside OS01 userspace):
 *   x86_64-elf-gcc -o smp_stress.elf smp_stress.c -O2
 *
 * Run:
 *   /smp_stress.elf [num_procs] [duration_sec]
 *
 * Watch kernel debug output for balance activity:
 *   -smp 2 -serial stdio -no-reboot
 *
 * Expected behaviour with 2 CPUs and 4 procs:
 *   - All 4 processes make progress concurrently (interleaved output)
 *   - Kernel log shows "balance: CPU0 ← N tasks from CPU1" etc.
 *   - nr_running stabilises near 2 per CPU
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdint.h>

/* ── Naive prime sieve (purely CPU-bound, no I/O) ─────────── */
static int is_prime(uint64_t n)
{
    if (n < 2)  return 0;
    if (n < 4)  return 1;        /* 2, 3 */
    if (n % 2 == 0) return 0;
    for (uint64_t d = 3; d * d <= n; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

static uint64_t count_primes(uint64_t limit)
{
    uint64_t cnt = 0;
    for (uint64_t n = 2; n <= limit; n++)
        if (is_prime(n)) cnt++;
    return cnt;
}

/* ── Worker process ───────────────────────────────────────── */
static void worker(int id, int duration_sec)
{
    pid_t pid = getpid();
    uint64_t base = (uint64_t)id * 100000ULL;
    uint64_t step = 5000ULL;
    int      rounds = 0;

    printf("[%d] pid=%d  start  base=%lu\n", id, (int)pid,
           (unsigned long)base);

    for (;;) {
        /* CPU-bound work: count primes in [base, base+step) */
        uint64_t cnt = count_primes(base + step);
        (void)cnt;  /* prevent optimiser from removing the loop */
        rounds++;
        base += step;

        /* Progress line every ~0.5 s of work (tuned for QEMU) */
        if (rounds % 20 == 0) {
            printf("[%d] pid=%d  round=%d  n=%lu  primes=%lu\n",
                   id, (int)pid, rounds, (unsigned long)(base + step),
                   (unsigned long)cnt);
        }

        /* Check elapsed wall time crudely via getpid count —
         * nanosleep is NOT called here so the task stays
         * on the runqueue and is eligible for migration. */
        if (rounds > duration_sec * 40)
            break;
    }

    printf("[%d] pid=%d  DONE  rounds=%d\n", id, (int)pid, rounds);
    _exit(0);
}

/* ── Entry ────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    int nproc   = 4;
    int dur_sec = 10;

    /* Minimal argument parser: -n <procs>  -d <seconds>  -h  positional */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == 'n' && argv[i][2] == '\0' && i + 1 < argc)
                nproc   = atoi(argv[++i]);
            else if (argv[i][1] == 'd' && argv[i][2] == '\0' && i + 1 < argc)
                dur_sec = atoi(argv[++i]);
            else if (argv[i][1] == 'h' && argv[i][2] == '\0') {
                printf("usage: smp_stress [-n procs] [-d sec] [procs] [sec]\n");
                return 0;
            }
            /* ignore unknown flags */
        } else {
            /* positional fallback */
            if (i == 1) nproc   = atoi(argv[i]);
            if (i == 2) dur_sec = atoi(argv[i]);
        }
    }

    if (nproc < 1)  nproc = 1;
    if (dur_sec < 1) dur_sec = 1;

    printf("smp_stress: %d procs, %d sec each (-h for help)\n", nproc, dur_sec);

    pid_t *children = (pid_t *)calloc((size_t)nproc, sizeof(pid_t));
    if (!children) { perror("calloc"); return 1; }

    for (int i = 0; i < nproc; i++) {
        pid_t p = fork();
        if (p < 0) {
            perror("fork");
            return 1;
        }
        if (p == 0) {
            /* Child: don't free(children) — it was allocated before
             * fork and the COW pages belong to both processes.
             * Just work, then _exit. */
            worker(i, dur_sec);
            /* unreachable */
        }
        children[i] = p;
    }

    /* Parent: wait for all children */
    for (int i = 0; i < nproc; i++) {
        int status;
        waitpid(children[i], &status, 0);
    }
    free(children);

    printf("\nsmp_stress: all %d procs finished\n", nproc);
    return 0;
}
