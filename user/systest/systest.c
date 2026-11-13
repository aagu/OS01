// OS01 Syscall Test Suite — exercises every syscall at least once.
// Launched by init.elf when built with OS01_SYSTEST=1.
// Prints [PASS] / [FAIL] per test, final summary, exit(fail_count).

#include "systest.h"

int fail_count = 0;
int pass_count = 0;
const char *case_skip_reason = NULL;


// ── Unified startup self-checks (spec 2026-09-13-user-startup-unification) ──
// Weak extern: under the old crt0/csu these resolve to 0, tests FAIL (RED);
// Task 3/4 define them and the same code goes GREEN.
extern uint64_t *__libc_auxv      __attribute__((weak));

extern void     *__libc_stack_end __attribute__((weak));

int g_argc;

char **g_argv;

#define SU_ERR_STACKEND_NULL 0x001
#define SU_ERR_ALIGN         0x002
#define SU_ERR_ARGC_MISMATCH 0x004
#define SU_ERR_ARGV_ADDR     0x008
#define SU_ERR_ENVIRON_ADDR  0x010
#define SU_ERR_ENVIRON_NULL  0x020
#define SU_ERR_AUXV_NULL     0x040
#define SU_ERR_AUXV_ADDR     0x080
#define SU_ERR_AUXV_ATNULL   0x100
#define SU_ERR_AUXV_RANGE    0x200
#define SU_ERR_ENV_WALK      0x400
#define SU_ERR_ARGV_STR      0x800
#define SU_ERR_ENV_STR       0x1000

/* Validate the entry-stack chain of THIS process (spec §6.1). Returns 0 or
 * an OR of SU_ERR_* bits. Never dereferences anything under the old crt0
 * (returns SU_ERR_STACKEND_NULL before touching environ/auxv). */
static int startup_layout_errors(int argc, char **argv)
{
    extern char **environ;
    if (&__libc_stack_end == NULL || __libc_stack_end == NULL)
        return SU_ERR_STACKEND_NULL;

    int errs = 0;
    if ((uintptr_t)__libc_stack_end & 0xF)              errs |= SU_ERR_ALIGN;
    if (*(uint64_t *)__libc_stack_end != (uint64_t)argc) errs |= SU_ERR_ARGC_MISMATCH;
    if (argv != (char **)((uint64_t *)__libc_stack_end + 1)) errs |= SU_ERR_ARGV_ADDR;
    if (argv[argc] != NULL)                              errs |= SU_ERR_ARGV_STR;
    for (int i = 0; i < argc; i++)
        if (argv[i] == NULL || argv[i][0] == '\0')      errs |= SU_ERR_ARGV_STR;

    if (environ == NULL) return errs | SU_ERR_ENVIRON_NULL;
    if (argv + argc + 1 != environ)                      errs |= SU_ERR_ENVIRON_ADDR;

    /* Bounded walk: read environ[0..128]; a 128-entry envp is legal (its
     * terminator sits at index 128), only the absence of a terminator
     * within 0..128 is an error. */
    int envc = 0;
    while (envc <= 128 && environ[envc] != NULL) {
        if (strchr(environ[envc], '=') == NULL)          errs |= SU_ERR_ENV_STR;
        envc++;
    }
    if (envc > 128)                                      errs |= SU_ERR_ENV_WALK;

    if (__libc_auxv == NULL) {
        errs |= SU_ERR_AUXV_NULL;
    } else {
        if ((uint64_t *)(environ + envc + 1) != __libc_auxv) errs |= SU_ERR_AUXV_ADDR;
        /* R9/2.2f: auxv now starts with (AT_PLATFORM, …) so the old
         * "first pair must be AT_NULL" check is wrong. Scan up to 64
         * pairs for the AT_NULL terminator instead. */
        int atnull_i = -1;
        for (int i = 0; i < 64; i++)
            if (__libc_auxv[2 * i] == 0) { atnull_i = i; break; }
        if (atnull_i < 0 || __libc_auxv[2 * atnull_i + 1] != 0)
            errs |= SU_ERR_AUXV_ATNULL;
        if (!((uintptr_t)__libc_auxv > (uintptr_t)__libc_stack_end)) errs |= SU_ERR_AUXV_RANGE;
    }
    return errs;
}


/* Entry probe: re-execed /bin/systest lands here BEFORE the test table.
 * Exit codes: 10 + argc*2 + envc on success (10..15, 138 for the envc=128
 * boundary case), 20 for the --pathcheck execvp case, 21 for the
 * --envinherit case, 22 for the --envclear case, 99 on any failure,
 * 127 if execve itself returned. Never runs the test suite or forks. */
static void startup_probe(int argc, char **argv)
{
    int is_probe = (argc == 0) ||
                   (argc >= 1 && argc <= 2 && argv != NULL && argv[0] != NULL &&
                    strcmp(argv[0], "startup-probe") == 0);
    if (!is_probe) return;

    int pathcheck = (argc == 2 && argv[1] != NULL &&
                     strcmp(argv[1], "--pathcheck") == 0);
    int envinherit = (argc == 2 && argv[1] != NULL &&
                      strcmp(argv[1], "--envinherit") == 0);
    int envclear = (argc == 2 && argv[1] != NULL &&
                    strcmp(argv[1], "--envclear") == 0);
    int errs = startup_layout_errors(argc, argv);
    if (argc == 2 && !pathcheck && !envinherit && !envclear &&
        (argv[1] == NULL || strcmp(argv[1], "--envcheck") != 0))
        errs |= SU_ERR_ARGV_STR;
    if (errs) exit(99);

    extern char **environ;
    int envc = 0;
    while (environ[envc] != NULL) envc++;
    if (pathcheck) {
        /* execvp PATH acceptance: the ONLY way this probe was found is
         * execvp parsing our non-default PATH (spec §6.2, plan review #3).
         * Exact env shape: PATH=/pathtest and nothing else. */
        const char *v;
        if (envc != 1 || strcmp(environ[0], "PATH=/pathtest") != 0) exit(99);
        v = getenv("PATH");
        if (v == NULL || strcmp(v, "/pathtest") != 0) exit(99);
        exit(20);
    }
    if (envinherit) {
        /* The parent mutated its environment and handed the result to
         * execve(): the child must see exactly the value the parent
         * expects (this covers both the inherited and the overwritten
         * value, because the parent sets OS01_ENV_EXPECT to the value it
         * just wrote). */
        const char *got = getenv("OS01_ENV_CHILD");
        const char *want = getenv("OS01_ENV_EXPECT");
        if (got == NULL || want == NULL) exit(99);
        if (strcmp(got, want) != 0) exit(99);
        exit(21);
    }
    if (envclear) {
        /* clearenv() empties the environment, but the block the kernel
         * handed to crt0 belongs to the process image: it may not be
         * freed.  The entry is read again AFTER clearenv() to prove the
         * memory is still intact. */
        char *first = environ[0];
        int readable = 0;
        if (first == NULL) exit(99);
        clearenv();
        if (getenv("OS01_ENV_CHILD") != NULL) exit(99);
        if (environ != NULL && environ[0] != NULL) exit(99);
        for (int i = 0; i < 64 && first[i] != '\0'; i++)
            if (first[i] == '=') { readable = 1; break; }
        if (!readable) exit(99);
        exit(22);
    }
    /* getenv must reflect the real envp (spec §6.2): exact value when
     * envc==1, NULL when empty. For envc>1 the '='-shape check in
     * startup_layout_errors covers correctness. */
    if (envc == 1) {
        const char *v = getenv("OS01_TEST_K");
        if (v == NULL || strcmp(v, "veRy42") != 0) exit(99);
    }
    if (envc == 0) {
        if (getenv("OS01_TEST_K") != NULL) exit(99);
    }
    exit(10 + argc * 2 + envc);
}


/* Parent side: fork + execve actual install path + waitpid.
 * status is the raw waitpid status (exit code << 8). */
int run_startup_probe_case(char **argv, char **envp, int expect_code)
{
    pid_t pid = fork();
    if (pid < 0) return 0;              /* fork failed */
    if (pid == 0) {
        execve("/bin/systest", argv, envp);
        exit(127);                      /* execve returned: failure */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;   /* waitpid failed */
    return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == expect_code;
}

void test_startup_layout(void)
{
    int errs = startup_layout_errors(g_argc, g_argv);
    CHECK3(errs == 0, "startup layout", errs ? "entry chain broken" : "entry chain ok");
}


/* spec §6.2 six-case parity matrix + §6.3 NULL-pointer forms */
void test_startup_matrix(void)
{
    char *argv0[1] = {NULL};
    char *argv1[2] = {(char *)"startup-probe", NULL};
    char *argv2[3] = {(char *)"startup-probe", (char *)"--envcheck", NULL};
    char *env1[2]  = {(char *)"OS01_TEST_K=veRy42", NULL};
    struct { char **av; char **ep; int code; const char *name; } cases[] = {
        {argv0, NULL, 10, "argc=0 envc=0 (even/pad8)"},
        {argv0, env1, 11, "argc=0 envc=1 (odd/pad0)"},
        {argv1, NULL, 12, "argc=1 envc=0 (odd/pad0)"},
        {argv1, env1, 13, "argc=1 envc=1 (even/pad8)"},
        {argv2, NULL, 14, "argc=2 envc=0 (even/pad8)"},
        {argv2, env1, 15, "argc=2 envc=1 (odd/pad0)"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK3(run_startup_probe_case(cases[i].av, cases[i].ep, cases[i].code),
               "startup matrix", cases[i].name);
    CHECK3(run_startup_probe_case(NULL, NULL, 10), "startup NULL argv", "argv=NULL envp=NULL");
    CHECK3(run_startup_probe_case(NULL, env1, 11), "startup NULL argv", "argv=NULL envp=1");

    /* Boundary: argc=0 + envc=128 == combined cap exactly. The libc envp
     * walk must ACCEPT a 128-entry envp (terminator at index 128) and
     * still find auxv — this is the regression test for the off-by-one
     * bound (plan review #3). */
    static char env128_buf[128][16];
    static char *env128[129];
    for (int i = 0; i < 128; i++) {
        sprintf(env128_buf[i], "E%03d=V%03d", i, i);
        env128[i] = env128_buf[i];
    }
    env128[128] = NULL;
    CHECK3(run_startup_probe_case(argv0, env128, 138), "startup boundary", "argc=0 envc=128 (combined cap)");
}


/* execvp PATH acceptance (plan review #3): proving libc execvp actually
 * PARSES the passed PATH — plant a probe symlink in a non-/bin directory,
 * point PATH at it, execvp by bare name. Exit 20 means the probe was
 * found THROUGH our PATH; anything else (incl. the /bin fallback, which
 * has no startup-probe) fails. */
void test_startup_execvp(void)
{
    extern char **environ;
    mkdir("/pathtest", 0755);
    symlink("/bin/systest", "/pathtest/startup-probe");
    char *av[3] = {(char *)"startup-probe", (char *)"--pathcheck", NULL};
    static char *env[2] = {(char *)"PATH=/pathtest", NULL};
    pid_t pid = fork();
    int ok = 0;
    if (pid == 0) {
        environ = env;              /* execvp passes environ to execve */
        execvp("startup-probe", av);
        exit(127);                  /* not found via PATH: failure */
    }
    if (pid > 0) {
        int status = 0;
        ok = waitpid(pid, &status, 0) == pid &&
             (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 20;
    }
    CHECK3(ok, "startup execvp PATH", ok ? "resolved via non-default PATH" : "fallback or not found");
    unlink("/pathtest/startup-probe");
    rmdir("/pathtest");
}


/* Environment mutation (libc completion T3): a variable the parent sets
 * must reach the child through fork+execve, an overwrite must be visible
 * too, and clearenv() must empty the environment WITHOUT freeing the
 * block the kernel handed to crt0.
 *
 * The child runs the startup probe in --envinherit mode (exit 21: the
 * value the child sees equals the value the parent expects) and in
 * --envclear mode (exit 22: the environment is empty and the inherited
 * entry is still readable after clearenv()). */
static int env_probe_exit(char **av, int expect_code)
{
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        extern char **environ;
        execve("/bin/systest", av, environ);
        exit(127);                     /* execve returned: failure */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == expect_code;
}

void test_environ_mutation(void)
{
    extern char **environ;
    char *inherited = NULL;
    int envc = 0;
    const char *v;

    while (environ[envc] != NULL) envc++;
    if (envc > 0) inherited = environ[0];

    /* setenv is visible immediately */
    CHECK3(setenv("OS01_ENV_CHILD", "inherited", 1) == 0,
           "environ setenv", "accepted");
    v = getenv("OS01_ENV_CHILD");
    CHECK3(v != NULL && strcmp(v, "inherited") == 0,
           "environ setenv", "value visible");
    CHECK3(setenv("OS01_ENV_EXPECT", "inherited", 1) == 0,
           "environ setenv", "expectation set");

    /* fork + execve: the child inherits the parent's environment */
    char *av_inherit[3] = {(char *)"startup-probe", (char *)"--envinherit", NULL};
    int ok = env_probe_exit(av_inherit, 21);
    CHECK3(ok, "environ fork/exec",
           ok ? "child inherited the parent's environment" : "child did not see it");

    /* an overwrite reaches the next child as well */
    CHECK3(setenv("OS01_ENV_CHILD", "overridden", 1) == 0,
           "environ overwrite", "accepted");
    v = getenv("OS01_ENV_CHILD");
    CHECK3(v != NULL && strcmp(v, "overridden") == 0,
           "environ overwrite", "new value visible");
    CHECK3(setenv("OS01_ENV_EXPECT", "overridden", 1) == 0,
           "environ overwrite", "expectation set");
    ok = env_probe_exit(av_inherit, 21);
    CHECK3(ok, "environ overwrite",
           ok ? "child saw the overwritten value" : "child saw the old value");

    /* the child clears the environment it inherited from the kernel */
    char *av_clear[3] = {(char *)"startup-probe", (char *)"--envclear", NULL};
    ok = env_probe_exit(av_clear, 22);
    CHECK3(ok, "environ clearenv",
           ok ? "child cleared without freeing the inherited block"
              : "child failed to clear the environment");

    /* clearenv empties the parent's environment, and the entry the
     * kernel handed to crt0 is still readable (it was never freed) */
    CHECK3(clearenv() == 0, "environ clearenv", "accepted");
    CHECK3(getenv("OS01_ENV_CHILD") == NULL,
           "environ clearenv", "environment empty");
    if (inherited != NULL) {
        int readable = 0;
        for (int i = 0; i < 64 && inherited[i] != '\0'; i++)
            if (inherited[i] == '=') { readable = 1; break; }
        CHECK3(readable, "environ clearenv", "inherited block still readable");
    }
}

static const test_case_t tests[] = {
    {"startup_layout",    "startup layout",    test_startup_layout,          1, 0},
    {"startup_matrix",    "startup matrix",    test_startup_matrix,          1, 0},
    {"startup_execvp",    "startup execvp",    test_startup_execvp,          1, 0},
    {"environ_mutation",  "environ mutation",  test_environ_mutation,        1, 0},
    {"realpath_vfs",      "realpath vfs",      test_realpath_vfs,            1, 0},
    {"dir_scan_vfs",      "dir scan vfs",      test_dir_scan_vfs,            1, 0},
    {"vfs_append",        "vfs atomic append", test_vfs_append,             1, 0},
    {"55_libc_alloc",     "libc alloc",         test_libc_alloc,             1, 0},
    {"56_libc_env",       "libc env",           test_libc_env,               1, 0},
    {"57_libc_stdio",     "libc stdio",         test_libc_stdio,             1, 0},
    {"58_libc_integer",   "libc integer",       test_libc_integer,           1, 0},
    {"59_libc_float",     "libc float",         test_libc_float,             1, 0},
    {"60_libc_scan",      "libc scan",          test_libc_scan,              1, 0},
    {"61_libc_time",      "libc time",          test_libc_time,              1, 0},
    {"62_libc_path_exec", "libc path exec",     test_libc_path_exec,         1, 0},
    {"63_libc_dir",       "libc dir",           test_libc_dir,               1, 0},
    {"65_libc_append",    "libc append",        test_libc_append,            1, 0},
    {"stdio_modes",       "stdio modes",       test_stdio_modes,            1, 0},
    {"busybox_lines",     "busybox line applets", test_busybox_lines,      1, 0},
    {"integer_conversions", "integer conversions", test_integer_conversions, 1, 0},
    {"float_precision",   "float precision",   test_float_precision,        1, 0},
    {"time_conversion",   "time conversion",   test_time_conversion,        1, 0},
    {"signal_handler_sync", "signal handler sync", test_signal_handler_sync, 1, 0},
    {"poll",              "poll syscall",      test_poll,                    1, 0},
    {"putchar",           "putchar syscall",   test_putchar,                 1, 1},
    {"sendto_user_boundaries", "sendto user boundaries", test_sendto_user_boundaries, 0, 0},
    {"write",             "write syscall",     test_write,                   1, 1},
    {"brk",               "brk heap",          test_brk,                     1, 0},
    {"getpid_getppid",    "getpid getppid",    test_getpid_getppid,          1, 1},
    {"fork_exec_waitpid", "fork exec waitpid", test_fork_exec_waitpid,       1, 0},
    {"exec_hostile_argv", "exec hostile argv", test_exec_hostile_argv,       1, 0},
    {"exec_hostile_envp", "exec hostile envp", test_exec_hostile_envp,       1, 0},
    {"hostile",           "hostile syscalls",  test_hostile,                 1, 0},
    {"devfs_open_inherit", "devfs open inherit", test_devfs_open_inherited_fg_pgrp, 1, 0},
    {"orphan_reparent",   "orphan reparent",   test_orphan_reparent,         1, 0},
    {"read",              "read syscall",      test_read,                    1, 1},
    {"open_close",        "open close",        test_open_close,              1, 1},
    {"dup_dup2",          "dup dup2",          test_dup_dup2,                1, 0},
    {"pipe",              "pipe syscall",      test_pipe,                    1, 0},
    {"sigaction",         "sigaction register", test_signal_register,        1, 0},
    {"chdir_getcwd",      "chdir getcwd",      test_chdir_getcwd,            1, 0},
    {"stat_fstat",        "stat fstat",        test_stat_fstat,              1, 0},
    {"lseek",             "lseek offsets",     test_lseek,                   1, 0},
    {"fcntl",             "fcntl flags",       test_fcntl,                   1, 0},
    {"getdents64",        "getdents64 entries", test_getdents64,             1, 0},
    {"access",            "access checks",     test_access,                  1, 1},
    {"mkdir_rmdir",       "mkdir rmdir",       test_mkdir_rmdir,             1, 0},
    {"unlink",            "unlink file",       test_unlink,                  1, 0},
    {"readlink",          "readlink stub",     test_readlink,                1, 0},
    {"rename",            "rename file",       test_rename,                  1, 0},
    {"ftruncate_truncate", "ftruncate truncate", test_truncate,              1, 0},
    {"gettimeofday",      "gettimeofday clock", test_gettimeofday,           1, 0},
    {"clock_gettime",     "clock_gettime monotonic", test_clock_gettime,     1, 0},
    {"nanosleep",         "nanosleep duration", test_nanosleep,              1, 0},
    {"nanosleep_eintr",   "nanosleep eintr",   test_nanosleep_eintr,         1, 0},
    {"chmod_fchmod",      "chmod fchmod",      test_chmod,                   1, 0},
    {"times",             "times cpu",         test_times,                   1, 0},
    {"uname",             "uname sysname",     test_uname,                   1, 1},
    {"umask",             "umask mask",        test_umask,                   1, 0},
    {"kill_deliver",      "kill deliver",      test_kill_signal_deliver,     1, 0},
    {"sync",              "sync flush",        test_sync,                    1, 0},
    {"sigprocmask",       "sigprocmask block", test_sigprocmask,             1, 0},
    {"sigreturn_saved_frame", "sigreturn saved frame", test_sigreturn_saved_frame, 1, 0},
    {"ext2_write",        "ext2 write",        test_ext2_write,              1, 0},
    {"boot_fat_mount",    "boot fat mount",    test_boot_fat_mount,          1, 0},
    {"reboot",            "reboot skip",       test_reboot_skip,             0, 0},
    {"select_basic",      "select basic",      test_select_basic,            1, 0},
    {"select_write",      "select write",      test_select_write,            1, 0},
    {"select_timeout",    "select timeout",    test_select_timeout,          1, 0},
    {"select_null_timeout", "select null timeout", test_select_null_timeout, 0, 0},
    {"select_multifd",    "select multifd",    test_select_multifd,          1, 0},
    {"select_zero_timeout", "select zero timeout", test_select_zero_timeout, 1, 0},
    {"select_sleep",      "select sleep",      test_select_sleep,            1, 0},
    {"pselect_sleep",     "pselect sleep",     test_pselect_sleep,           1, 0},
    {"select_invalid_fd", "select invalid fd", test_select_invalid_fd,       0, 0},
    {"pselect_null_sigmask", "pselect null sigmask", test_pselect_null_sigmask, 1, 0},
    {"pselect_bad_ss_len", "pselect bad ss len", test_pselect_bad_ss_len,    1, 0},
    {"rbtree_insert_order", "rbtree insert order", test_wrap_rbtree_insert_order, 1, 0},
    {"rbtree_erase_middle", "rbtree erase middle", test_wrap_rbtree_erase_middle, 1, 0},
    {"rbtree_stress_100", "rbtree stress 100", test_wrap_rbtree_stress_100,  1, 0},
    {"rbtree_last",       "rbtree last",       test_wrap_rbtree_last,        1, 0},
    {"rbtree_prev_traversal", "rbtree prev traversal", test_wrap_rbtree_prev_traversal, 1, 0},
    {"rbtree_prev_null",  "rbtree prev null",  test_wrap_rbtree_prev_null,   1, 0},
    {"eevdf_fork_child",  "eevdf fork child",  test_wrap_eevdf_fork_child_scheduled, 1, 0},
    {"proc_maps",         "proc maps",         test_proc_maps,               1, 0},
    {"proc_fd",           "proc fd",           test_proc_fd,                 1, 0},
    {"termios",           "termios honesty",   test_termios,                 1, 0},
    {"tiocspgrp",         "tiocspgrp roundtrip", test_tiocspgrp_roundtrip,   1, 0},
    {"devfs_open_fg",     "devfs open fg",     test_devfs_open_default_fg_pgrp, 1, 0},
    {"setpgid_getpgid",   "setpgid getpgid",   test_setpgid_getpgid,         1, 0},
    {"setsid",            "setsid session",    test_setsid,                  1, 0},
    {"setpgid_auto_fg",   "setpgid auto fg",   test_setpgid_auto_fg_pgrp,    1, 0},
    {"signal_pgrp_basic", "signal pgrp basic", test_signal_pgrp_basic,       1, 0},
    {"kill_pgrp",         "kill pgrp",         test_kill_neg_pid_pgrp,       1, 0},
    {"getrandom",         "getrandom urandom", test_getrandom,               1, 0},
    {"libc_printf_getopt", "libc printf getopt", test_libc_printf_getopt,    1, 0},
    // ── symlink support (T12) ──
    {"symlink_create_readlink", "symlink create readlink", test_symlink_create_readlink, 1, 0},
    {"symlink_errors",    "symlink errors",    test_symlink_errors,          1, 0},
    {"symlink_stat",      "symlink stat",      test_symlink_stat,            1, 0},
    {"symlink_follow_midpath", "symlink follow midpath", test_symlink_follow_midpath, 1, 0},
    {"symlink_loop_depth", "symlink loop depth", test_symlink_loop_depth,    1, 0},
    {"41_exec_via_symlink", "exec via symlink", test_41_exec_via_symlink,    1, 0},
    {"42_getdents_dt_lnk", "getdents dt lnk",  test_42_getdents_dt_lnk,      1, 0},
    {"43_ssp_guard_seeded", "ssp guard seeded", test_ssp_guard_seeded,       1, 0},
    {"44_ssp_guard_distinct_across_exec", "ssp guard distinct across exec", test_ssp_guard_distinct_across_exec, 1, 0},
    {"45_ssp_trip_sigabrt_exec", "ssp trip sigabrt exec", test_ssp_trip_sigabrt_exec, 1, 0},
    {"46_ssp_trip_sigabrt_fork", "ssp trip sigabrt fork", test_ssp_trip_sigabrt_fork, 1, 0},
    {"47_ssp_no_false_trip", "ssp no false trip", test_ssp_no_false_trip,    1, 0},
    {"48_atexit_lifecycle", "atexit lifecycle", test_atexit_lifecycle,       1, 0},
    {"49_brk_read_fresh_page", "brk read fresh page", test_brk_read_fresh_page, 1, 0},
    {"50_protected_ranges", "protected ranges", test_protected_ranges,       1, 0},
    {"51_fork_brk_isolation", "fork brk isolation", test_fork_brk_isolation, 1, 0},
    {"52_fork_mmap_cow_isolation", "fork mmap cow isolation", test_fork_mmap_cow_isolation, 1, 0},
    {"53_cow_kernel_outputs", "cow kernel outputs", test_cow_kernel_outputs, 1, 0},
    {"54_linux_abi_compat", "linux abi compat", test_linux_abi_compat,       1, 0},
    {"54b_user_fault_divzero", "user fault divzero → SIGFPE",  test_user_fault_divzero,    1, 0},
    {"54c_user_fault_write_ro", "user fault write ro → SIGSEGV", test_user_fault_write_ro,  1, 0},
    {"54d_user_fault_ud2",     "user fault ud2 → SIGILL",      test_user_fault_ud2,        1, 0},
    {"54e_user_fault_gp",      "user fault cli #GP → SIGSEGV", test_user_fault_gp,        1, 0},
    {"mprotect_subrange_cow",  "mprotect subrange cow", test_mprotect_subrange_cow,    1, 0},
    {"mmap_exec_protection",   "mmap execute protection", test_mmap_execute_protection, 1, 0},
    {"fixed_file_replace",     "munmap hole + MAP_FIXED file", test_fixed_file_replace, 1, 0},
    {"fb_fixed_replace",       "framebuffer MAP_FIXED replace", test_fb_fixed_replace, 1, 0},
};

// ── CLI: --list ────────────────────────────────────────────
// Print every table row (id + flags) in table order.  --list is the
// machine-readable mirror of the execution order: both iterate tests[]
// by index, so list/execution parity is structural.
static int print_case_list(void)
{
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    printf("[SYS TEST] %d case(s)\n", n);
    for (int i = 0; i < n; i++) {
        printf("%s\t%s\trequired=%d quick=%d\n",
               tests[i].id, tests[i].name, tests[i].required, tests[i].quick);
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    startup_probe(argc, argv);
    g_argc = argc;
    g_argv = argv;

    // Argument parsing: --list / --case <id> / --quick / --full.
    // Default is the full table.
    const char *want_case = NULL;
    int want_quick = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            return print_case_list();
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            want_case = argv[++i];
        } else if (strcmp(argv[i], "--quick") == 0) {
            want_quick = 1;
        } else if (strcmp(argv[i], "--full") == 0) {
            want_quick = 0;
        }
    }

    printf("[SYS TEST] OS01 Syscall Test Suite\n");
    printf("[SYS TEST] ----------------------------------------\n");

    int n = (int)(sizeof(tests) / sizeof(tests[0]));

    // Build the selection as indices into tests[] (keeps execution and
    // SELECT emission reading from the same table entry).
    static int sel[sizeof(tests) / sizeof(tests[0])];
    int nsel = 0;
    if (want_case != NULL) {
        int found = -1;
        for (int i = 0; i < n; i++) {
            if (strcmp(tests[i].id, want_case) == 0) { found = i; break; }
        }
        if (found < 0) {
            // Unknown case: publish no v1 trace, so the host sees a
            // protocol failure (missing START/END) and archives FAIL.
            printf("[SYS TEST] ERROR: unknown case '%s' (use --list)\n",
                   want_case);
            return 2;
        }
        sel[nsel++] = found;
    } else if (want_quick) {
        for (int i = 0; i < n; i++)
            if (tests[i].quick) sel[nsel++] = i;
    } else {
        for (int i = 0; i < n; i++) sel[nsel++] = i;
    }

    // Protocol v1 (spec §6.1): declare the whole selection before any
    // case runs, then one BEGIN + terminal per case.
    printf("[TEST] START v=1 suite=systest expected=%d\n", nsel);
    for (int k = 0; k < nsel; k++) {
        printf("[TEST] SELECT %s required=%d\n",
               tests[sel[k]].id, tests[sel[k]].required);
    }

    int case_passed = 0, case_failed = 0, case_skipped = 0;
    for (int k = 0; k < nsel; k++) {
        const test_case_t *tc = &tests[sel[k]];
        printf("\n--- %s ---\n", tc->name);
        // [TEST] BEGIN marks the case *start*, so it is emitted before
        // the body runs — parse_v1 anchors the terminal to this open
        // BEGIN, so a body that never returns still yields no terminal.
        printf("[TEST] BEGIN %s\n", tc->id);
        // Per-case status derives from the assertion-failure delta
        // across the case body (no child isolation).  A body that
        // cannot run at all (its prerequisite is absent in this boot
        // environment — e.g. no NIC, so socket() fails) calls
        // skip_case() and publishes a SKIP terminal instead of FAIL:
        // parse_v1 counts a SKIP separately, and only required=1
        // cases make a SKIP fatal downstream.
        case_skip_reason = NULL;
        int before = fail_count;
        tc->fn();
        int delta = fail_count - before;
        if (case_skip_reason != NULL) {
            case_skipped++;
            printf("[TEST] SKIP %s reason=%s\n", tc->id, case_skip_reason);
        } else if (delta == 0) {
            case_passed++;
            printf("[TEST] PASS %s\n", tc->id);
        } else {
            case_failed++;
            printf("[TEST] FAIL %s reason=assertion_failures_%d\n",
                   tc->id, delta);
        }
    }

    // Legacy summary line kept for transition; no longer the gate.
    printf("\n[SYS TEST] RESULT: %d passed, %d failed\n", pass_count, fail_count);
    printf("[TEST] END suite=systest total=%d passed=%d failed=%d skipped=%d\n",
           nsel, case_passed, case_failed, case_skipped);

    return fail_count > 255 ? 255 : (int)fail_count;
}
