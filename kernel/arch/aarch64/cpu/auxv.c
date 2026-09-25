// kernel/arch/aarch64/auxv.c — aarch64 AT_PLATFORM payload.
// Strong override of arch_auxv_* declared in <arch/auxv.h>.
#include <arch/auxv.h>

static const char platform[] = "aarch64"; /* 9 incl NUL */

const char *arch_auxv_platform(void)    { return platform; }
size_t      arch_auxv_payload_size(void) { return sizeof(platform); }