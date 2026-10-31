#ifndef _ARCH_IDENTITY_H
#define _ARCH_IDENTITY_H

/* Arch identity constants.
 *
 *   ARCH_NAME           — short architecture name ("x86_64" / "aarch64")
 *   ARCH_BOOT_LOGO_LABEL — the caption drawn under the OS01 boot wordmark
 *
 * The per-arch headers define the macros; this facade only selects them.
 */

#ifdef __x86_64__
#include <arch/x86_64/identity.h>
#elif defined(__aarch64__)
#include <arch/aarch64/identity.h>
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_IDENTITY_H */
