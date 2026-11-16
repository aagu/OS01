/*
 * user/timeit.c — measure the wall-clock (real) time of a command.
 *
 * Usage:
 *     timeit [-o FILE] PROG ARGS...
 *
 * The elapsed time is written to stderr (or to FILE when -o is given)
 * as a single line:
 *
 *     real    0.001234
 *
 * "real" is the GNU-time field name for WALL-CLOCK time: the time
 * between the fork and the child's exit, regardless of how much CPU
 * the child actually used.  No "user" / "sys" fields are printed.
 *
 * WHY THERE IS NO user / sys
 * --------------------------
 * A user/sys breakdown needs per-task CPU time accounting, and OS01
 * has NONE: `struct task_t` (kernel/include/sched/task.h) carries no
 * utime/stime accumulator at all, and the scheduler never charges a
 * task for the CPU time it burns.  Collecting it would mean touching
 * the scheduler's accounting paths, which is a separate piece of work
 * with its own ABI and migration questions.  Rather than print a fake
 * "user 0.000 / sys 0.000" — which would be actively misleading — this
 * program prints the wall clock only.
 *
 * The same reason rules out BusyBox's `time` applet: its miscutils
 * implementation is built on wait3()/wait4() plus `struct rusage`, and
 * OS01 exposes neither the syscall nor the libc structure.
 *
 * NOTES
 * -----
 * * CLOCK_MONOTONIC is used, not CLOCK_REALTIME: OS01 has no real-time
 *   clock (the RTC is absent), so CLOCK_REALTIME would report "time
 *   since boot".  For measuring a duration, the monotonic source is
 *   the correct one anyway — it cannot jump when the clock is set.
 * * The child is terminated with _exit(), which is a bare SYS_exit:
 *   it runs no atexit handler and flushes no stdio buffer.  Using
 *   exit() there would flush the buffers this process inherited from
 *   its parent and could duplicate output.
 * * The PATH search is the libc's execvp() (libc/unistd/execve.c) —
 *   it is NOT reimplemented here.  execvp() already implements the
 *   rules that matter here: a name containing '/' is not searched
 *   for, an unset PATH falls back to the libc default ("/bin"), an
 *   empty PATH means the current directory, an empty PATH segment
 *   means the current directory, EACCES wins over the ENOENT of the
 *   other candidates, and a candidate that exists but is not a
 *   binary is executed through /bin/sh.  That last rule is why
 *   `timeit myscript` — a BARE name that the PATH search finds —
 *   measures a SHELL SCRIPT instead of failing with ENOEXEC.
 *
 *   The scope of that rule matters: in OS01's libc the /bin/sh
 *   fallback belongs to the PATH SEARCH.  A name that CONTAINS A
 *   SLASH (`timeit ./script.sh`, `timeit /tmp/script.sh`) is
 *   therefore executed directly and fails with ENOEXEC.
 *
 *   That is what OS01's execvp() does today, and it is NARROWER
 *   than both POSIX Issue 8 and glibc: POSIX requires that
 *   execlp()/execvp() run a command interpreter whenever they would
 *   fail with ENOEXEC, with no "contains a slash" exception, and
 *   glibc follows that (posix/execvpe.c, maybe_script_execute).
 *   OS01's libc therefore differs from POSIX Issue 8 and from glibc
 *   in this one respect; the difference is a known one, and the
 *   behaviour described here is the one to rely on.
 *
 *   OS01's kernel has no "#!" (shebang) support either, so a script
 *   named by an absolute path is not started by the kernel.
 * * The child's stdout/stderr are inherited untouched: the timing line
 *   goes to fd 2 or to the -o file, never into the command's output.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* The shell convention for a command that could not be started:
 *   127  the command was not found
 *   126  the command was found but cannot be executed
 */
#define TIMEIT_EXIT_NOT_FOUND 127
#define TIMEIT_EXIT_CANNOT_EXEC 126

static void usage(const char *why)
{
    if (why)
        fprintf(stderr, "timeit: %s\n", why);
    fprintf(stderr, "usage: timeit [-o FILE] PROG ARGS...\n");
    exit(2);
}

/* A short, self-contained errno text.  OS01's libc has no strerror(),
 * and the handful of cases below are all a user can act on. */
static const char *errno_text(int e)
{
    switch (e) {
    case ENOENT:  return "no such file or directory";
    case EACCES:  return "permission denied";
    case ENOEXEC: return "exec format error";
    case EISDIR:  return "is a directory";
    case ENOTDIR: return "not a directory";
    case E2BIG:   return "argument list too long";
    case ENOMEM:  return "out of memory";
    case ENAMETOOLONG: return "file name too long";
    case ELOOP:   return "too many symbolic links";
    default:      return "cannot execute";
    }
}

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    int argi = 1;

    /* Option parsing is hand-rolled on purpose: the contract is
     * "everything after the first non-option argument belongs to the
     * measured command, including arguments that look like options",
     * and getopt()'s permutation behaviour is not verified on OS01.
     * Only "-o FILE" (and "-oFILE") are recognised, and only before
     * the first non-option argument. */
    for (; argi < argc; argi++) {
        const char *a = argv[argi];

        if (a[0] != '-' || a[1] == '\0')
            break;                      /* the command starts here */

        if (strcmp(a, "--") == 0) {
            argi++;
            break;
        }
        if (strcmp(a, "-o") == 0) {
            if (argi + 1 >= argc)
                usage("-o requires a file name");
            out_path = argv[++argi];     /* a later -o overrides this one */
            continue;
        }
        if (a[1] == 'o' && a[2] != '\0') {
            out_path = a + 2;
            continue;
        }
        usage("unknown option");
    }

    if (argi >= argc)
        usage("a command is required");

    /* Open the output file BEFORE the fork: a failure here must not
     * run the measured command at all. */
    int out_fd = 2;
    if (out_path) {
        out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "timeit: cannot open %s: %s\n",
                    out_path, errno_text(errno));
            return 2;
        }
    }

    struct timespec t0, t1;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        fprintf(stderr, "timeit: clock_gettime failed\n");
        if (out_fd != 2)
            close(out_fd);
        return 2;
    }

    int64_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "timeit: fork failed\n");
        if (out_fd != 2)
            close(out_fd);
        return 2;
    }

    if (pid == 0) {
        /* ── child ── */
        int e = 0;

        /* execvp() does the PATH search: a name with a '/' is executed
         * directly, an unset PATH falls back to the libc default
         * ("/bin"), an empty PATH means the current directory, and a
         * candidate that exists but is not a binary is run through
         * /bin/sh.  A slash-named candidate that is not a binary
         * therefore fails with ENOEXEC (see the NOTES above).
         * It also passes `environ` to execve() itself. */
        execvp(argv[argi], &argv[argi]);
        e = errno;

        /* exec failed.  Report on stderr and leave with the shell's
         * convention: 127 = not found, 126 = found but not runnable. */
        int code = (e == ENOENT) ? TIMEIT_EXIT_NOT_FOUND
                                 : TIMEIT_EXIT_CANNOT_EXEC;
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
                         "timeit: %s: %s\n", argv[argi], errno_text(e));
        if (n > 0)
            (void)!write(2, msg, (size_t)n);
        _exit(code);
    }

    /* ── parent ── */
    int status = 0;
    int64_t w = waitpid(pid, &status, 0);
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        memset(&t1, 0, sizeof(t1));
    }

    if (w != pid) {
        fprintf(stderr, "timeit: waitpid failed\n");
        if (out_fd != 2)
            close(out_fd);
        return 2;
    }

    /* Exact integer arithmetic: the whole computation stays in signed
     * 64-bit nanoseconds, so the nsec borrow (t1.tv_nsec < t0.tv_nsec)
     * is handled by the signed subtraction and no floating point is
     * involved. */
    int64_t elapsed_ns = ((int64_t)t1.tv_sec - (int64_t)t0.tv_sec) * 1000000000LL
                       + ((int64_t)t1.tv_nsec - (int64_t)t0.tv_nsec);
    if (elapsed_ns < 0)
        elapsed_ns = 0;

    /* Microsecond resolution: three decimals would round a fast
     * command down to "0.000", which hides the fact that it took a
     * measurable amount of time. */
    int64_t sec = elapsed_ns / 1000000000LL;
    int64_t usec = (elapsed_ns % 1000000000LL) / 1000LL;

    char line[64];
    int n = snprintf(line, sizeof(line), "real    %lld.%06lld\n",
                     (long long)sec, (long long)usec);
    if (n > 0)
        (void)!write(out_fd, line, (size_t)n);

    if (out_fd != 2)
        close(out_fd);

    /* Propagate the measured command's exit status unchanged. */
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 2;
}
