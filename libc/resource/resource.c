#include <sys/resource.h>

int getrlimit(int resource, struct rlimit *rlim)
{
    (void)resource;
    if (rlim) { rlim->rlim_cur = 65536; rlim->rlim_max = 65536; }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
    { (void)resource; (void)rlim; return 0; }
