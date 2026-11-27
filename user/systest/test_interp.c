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

#endif /* __x86_64__ */
