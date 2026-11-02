#ifndef _OS01_CONFIG_LIMITS_H
#define _OS01_CONFIG_LIMITS_H

/*
 * kernel/include/config/limits.h — Single source of truth for compile-time
 * capacity limits in OS01 (ARCH-10).
 *
 * Pure integer constants only — no C typedefs, structs, functions, or inline
 * assembly, so this header is safely includable from both C and preprocessed
 * assembly (.S) sources.
 */

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

#ifndef OS01_NOFILE_CAPACITY
#define OS01_NOFILE_CAPACITY 16
#endif

#endif /* _OS01_CONFIG_LIMITS_H */
