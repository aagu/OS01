#include <stdlib.h>    /* environ (extern) */
#include <stdint.h>

extern char **environ;

/* Set by __libc_start_main; consumed by tests now and getauxval/AT_RANDOM
 * later (spec 2026-09-13-user-startup-unification §4.3). */
uint64_t *__libc_auxv;      /* auxv start (points at the FIRST entry) */
void     *__libc_stack_end; /* entry rsp of _start */

int __libc_start_main(int (*main)(int, char **, char **),
                      int argc, char **argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end)
{
    (void)init; (void)fini; (void)rtld_fini;
    __libc_stack_end = stack_end;

    /* argv[argc]==NULL, then envp[]; its terminating NULL is STRICTLY
     * followed by auxv (kernel layout contract, spec §4.1). */
    char **ep = argv + argc + 1;
    environ = ep;
    /* Bounded walk: read ep[0..128] — a 128-entry envp is legal (its
     * terminator sits at index 128, since the kernel caps argc+envc at
     * 128 combined); only a missing terminator within 0..128 is an error. */
    int env_i;
    for (env_i = 0; env_i <= 128 && ep[env_i] != NULL; env_i++)
        ;
    if (env_i > 128) {
        /* envp missing its terminator (kernel layout bug): drop auxv,
         * keep the process alive so tests can report (spec §4.3). */
        __libc_auxv = NULL;
        return main(argc, argv, environ);
    }

    uint64_t *av = (uint64_t *)(ep + env_i + 1);
    __libc_auxv = av;
    for (int i = 0; i < 64; i++) {
        if (av[2 * i] == 0) goto found;    /* AT_NULL */
    }
    __libc_auxv = NULL;                    /* no AT_NULL within 64 pairs */
found:
    return main(argc, argv, environ);
}
