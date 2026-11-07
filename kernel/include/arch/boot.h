#ifndef ARCH_BOOT_H
#define ARCH_BOOT_H

#include <arch/features.h>

#if defined(__x86_64__)
#include <arch/x86_64/boot.h>
#elif defined(__aarch64__)
#include <arch/aarch64/boot.h>
#endif

struct boot_context;

/*
 * Unified Kernel Boot Orchestration Facade
 *
 * Each architecture implements strong symbol overrides for these hooks.
 * Missing implementations result in a link-time error.
 */

/* msg is reserved; implementations emit legacy strings to preserve test harness regex compatibility */
void arch_early_panic(const char *msg) __attribute__((noreturn));
void arch_early_init(const struct boot_context *bootctx);
void arch_boot_memory(const struct boot_context *bootctx);
void arch_pre_subsys_selftest(const struct boot_context *bootctx);
void arch_boot_subsystems(const struct boot_context *bootctx);
void arch_boot_percpu(void);
int arch_boot_device_nodes(void);
void arch_boot_aps(void);
void arch_post_smp_selftest(void);

#if !OS01_HAS_USERSPACE
void arch_start_kernel_idle(void) __attribute__((noreturn));
#endif

#endif /* ARCH_BOOT_H */
