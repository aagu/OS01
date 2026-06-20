#include <stdlib.h>

#define ATEXIT_MAX 32

static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*function)(void))
{
    if (atexit_count >= ATEXIT_MAX)
        return -1;
    if (!function)
        return 0;
    atexit_funcs[atexit_count++] = function;
    return 0;
}

// Called by exit() before the syscall — not currently wired into do_exit.
// BusyBox will call it if it exists.
void __call_atexit_handlers(void)
{
    // Call in reverse order of registration
    for (int i = atexit_count - 1; i >= 0; i--) {
        if (atexit_funcs[i])
            atexit_funcs[i]();
    }
    atexit_count = 0;
}
