#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H

/* Single source: kernel/include/uapi/auxv.h (installed to libc sysroot)
 *
 * User-space getauxval() and the kernel's setup_user_stack() share a single
 * vocabulary. The canonical list of AT_* constants lives in
 * kernel/include/uapi/auxv.h; this header pulls it in verbatim so libc TUs
 * never diverge from kernel emits. */

#include <uapi/auxv.h>

unsigned long getauxval(unsigned long type);

#endif /* _SYS_AUXV_H */