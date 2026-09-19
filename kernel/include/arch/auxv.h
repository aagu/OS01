#ifndef _KERNEL_ARCH_AUXV_H
#define _KERNEL_ARCH_AUXV_H

#include <stddef.h>

/* arch_auxv_platform — returns a NUL-terminated string identifying
 * the platform, suitable for AT_PLATFORM in the user-space startup
 * auxv table. The returned pointer references a static buffer (no
 * allocation, IRQ-safe). Per-arch strong override lives in
 * `kernel/arch/<arch>/auxv.c` (AGENTS.md §Directory organization
 * item 3: arch-neutral facade + per-arch strong override). */
const char *arch_auxv_platform(void);

/* arch_auxv_payload_size — byte length of the AT_PLATFORM payload
 * INCLUDING the trailing NUL. Matches strlen(arch_auxv_platform())
 * + 1 at the call site. */
size_t arch_auxv_payload_size(void);

#endif /* _KERNEL_ARCH_AUXV_H */