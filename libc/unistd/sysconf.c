#include <unistd.h>
#include <stdint.h>

long sysconf(int name) {
    switch (name) {
    case _SC_CLK_TCK:       return 100;
    case _SC_PAGESIZE:      return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return 8;
    default:                return -1;
    }
}
