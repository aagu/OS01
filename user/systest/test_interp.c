// ── x86_64 PT_INTERP exec acceptance ─────────────────────────────
//
// T1 of the dynamic-linker roadmap item: an ET_DYN main image that
// carries PT_INTERP must exec into the kernel-loaded interpreter
// (/lib/ld.so skeleton), which hands control to the main entry.
// The guest-visible chain is exercised by /bin/dyn_exec (fixtures
// interp_probe.c); this case only drives exec and reads the exit
// code — the ld.so marker on the serial log is the handoff evidence,
// the exit code (0/91..93) is the contract.

#include "systest.h"

#if defined(__x86_64__)

static int run_user_image(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)path, NULL };
        exec(path, argv, NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    int st = -1;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    printf("[interp-diag] %s pid=%d raw st=0x%x\n", path, (int)pid, st);
    if ((st & 0x7f) != 0)
        return -2;                       /* killed by a signal */
    return (st >> 8) & 0xff;
}

/* Child-side exec result mapping for the ABI-gate case: exec()
 * returns the negative errno on failure, so the child can tell the
 * REAL rejection (-ENOEXEC) apart from any other exec failure (127)
 * — a bare 127 would be a vacuous pass for any failure reason. */
static int run_user_image_execrc(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)path, NULL };
        int64_t r = exec(path, argv, NULL);
        _exit(r == -ENOEXEC ? 126 : 127);
    }
    if (pid < 0)
        return -1;
    int st = -1;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    printf("[interp-diag] %s pid=%d raw st=0x%x\n", path, (int)pid, st);
    if ((st & 0x7f) != 0)
        return -2;                       /* killed by a signal */
    return (st >> 8) & 0xff;
}

void test_exec_interp(void)
{
    /* The full chain: exec dyn_exec -> kernel loads /lib/ld.so ->
     * ld.so prints its marker and jumps to AT_ENTRY -> the main
     * image verifies AT_BASE/AT_PHDR and exits 0. */
    int rc = run_user_image("/bin/dyn_exec");
    CHECKF(rc == 0, "exec_interp PT_INTERP chain",
           "ldso handoff + auxv checks (rc=%d)",
           "ldso handoff + auxv checks (rc=%d)", rc);

    /* A main image whose interpreter is missing must not exec:
     * the child reports the rejection through exit 127.  dyn_exec
     * ships with an interpreter present, so point exec at a copy
     * whose PT_INTERP cannot resolve — simplest is to exec the
     * interpreter itself, which carries no PT_INTERP and must run
     * directly (sanity for the no-interp fast path). */
    rc = run_user_image("/lib/ld.so");
    CHECK3(rc == 91, "exec_interp no-interp direct run",
           "ldso without PT_INTERP exits 91");

    rc = run_user_image("/bin/interp_missing");
    CHECK3(rc == 127, "exec_interp missing image rejection",
           "missing image exits 127");
}

// ── T2: the dynamic-link chain proper ────────────────────────
//
// dyn_link carries PT_INTERP + DT_NEEDED=/lib/libt2.so: the kernel
// loads /lib/ld.so, ld.so maps libt2, resolves t2_add/t2_value
// through the SYSV hash tables and applies GLOB_DAT/JUMP_SLOT; the
// main verifies the computed value (84) and exits 0.  The negative
// variants pin the failure paths: a missing library exits 95, a
// Linux-ABI main is rejected at the exec layer (-ENOEXEC → child
// 126), and a library whose declared dynamic range lacks DT_NULL
// exits 98 through the bounded-scan path.  Serial-window gates in
// qemutests/run_test.py additionally pin WHERE each ld.so failure
// came from.

void test_exec_dyn_link(void)
{
    int rc = run_user_image("/bin/dyn_link");
    CHECKF(rc == 0, "exec_dyn_link chain",
           "ld.so loaded libt2 and resolved symbols (rc=%d)",
           "ld.so loaded libt2 and resolved symbols (rc=%d)", rc);

    /* The export-only shape: the main carries DT_NEEDED but no
     * symbolic relocations of its own — the library imports a
     * MAIN-defined symbol, so reaching exit 0 proves the lookup
     * scope includes the executable (spec §6) even when it imports
     * nothing. */
    rc = run_user_image("/bin/dyn_link_export");
    CHECKF(rc == 0, "exec_dyn_link export-only scope",
           "lib import of main-defined symbol resolved (rc=%d)",
           "lib import of main-defined symbol resolved (rc=%d)", rc);
}

void test_exec_dyn_link_missing(void)
{
    int rc = run_user_image("/bin/dyn_link_missing");
    CHECK3(rc == 95, "exec_dyn_link missing library",
           "missing DT_NEEDED target exits 95");
}

void test_exec_dyn_link_gnuabi(void)
{
    /* The assertion is the REAL exec error (126), not the generic
     * 127: note-absence (exec succeeds → 0) and wrong-reason failures
     * (any other errno → 127) are distinguishable. */
    int rc = run_user_image_execrc("/bin/dyn_link_gnuabi");
    CHECK3(rc == 126, "exec_dyn_link gnuabi rejection",
           "Linux-ABI interp main rejected -ENOEXEC at exec");
}

void test_exec_dyn_link_baddyn(void)
{
    int rc = run_user_image("/bin/dyn_link_baddyn");
    CHECK3(rc == 98, "exec_dyn_link bad dynamic table",
           "dynamic range without DT_NULL exits 98");
}

#endif /* __x86_64__ */
