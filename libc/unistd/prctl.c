#include <sys/prctl.h>
#include <errno.h>

int prctl(int option, ...)
{
    (void)option;
    return 0;  // stub: all prctl options succeed
}
