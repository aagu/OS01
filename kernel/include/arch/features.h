#ifndef ARCH_FEATURES_H
#define ARCH_FEATURES_H

/*
 * Architecture Feature Gates
 *
 * Explicit 0 or 1 definitions representing platform subsystem availability.
 * Defaults are per-architecture, but can be overridden by build flags.
 */

#ifndef OS01_HAS_VFS
#if defined(__x86_64__)
#define OS01_HAS_VFS        1
#elif defined(__aarch64__)
#define OS01_HAS_VFS        0
#else
#error "Unsupported architecture for OS01_HAS_VFS default"
#endif
#endif

#ifndef OS01_HAS_USERSPACE
#if defined(__x86_64__)
#define OS01_HAS_USERSPACE  1
#elif defined(__aarch64__)
#define OS01_HAS_USERSPACE  0
#else
#error "Unsupported architecture for OS01_HAS_USERSPACE default"
#endif
#endif

#if (OS01_HAS_VFS != 0 && OS01_HAS_VFS != 1)
#error "OS01_HAS_VFS must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_USERSPACE != 0 && OS01_HAS_USERSPACE != 1)
#error "OS01_HAS_USERSPACE must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_USERSPACE && !OS01_HAS_VFS)
#error "OS01_HAS_USERSPACE requires OS01_HAS_VFS"
#endif

#endif /* ARCH_FEATURES_H */
