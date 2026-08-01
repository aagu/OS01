#include <stdlib.h>    /* environ (extern) */

extern char **environ;

int __libc_start_main(int (*main)(int, char **, char **),
                       int argc, char **argv,
                       void (*init)(void), void (*fini)(void),
                       void (*rtld_fini)(void), void *stack_end)
{
    (void)init; (void)fini; (void)rtld_fini; (void)stack_end;
    return main(argc, argv, environ);
}
