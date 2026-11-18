#ifndef OS01_AARCH64_BOOT_OFFSETS_H
#define OS01_AARCH64_BOOT_OFFSETS_H

/* Integer-only shared C/assembly ABI. */
#include <config/limits.h>
#include <percpu/percpu.h>  /* pulls in PERCPU_DATA_SIZE for asm context */

#define AARCH64_BOOT_PERCPU_SIZE       48
#define AARCH64_BOOT_SELF_OFFSET       0
#define AARCH64_BOOT_CPU_ID_OFFSET     8
#define AARCH64_BOOT_MPIDR_OFFSET     16
#define AARCH64_BOOT_STACK_OFFSET     24
#define AARCH64_BOOT_ONLINE_OFFSET    32
#define AARCH64_BOOT_GO_OFFSET        36
#define AARCH64_BOOT_RESERVED_OFFSET  40
#define AARCH64_BOOT_STACK_SHIFT     12
#define AARCH64_BOOT_STACK_SIZE      0x1000
#define AARCH64_BOOT_SCTLR           (0x300c00 | (1 << 12) | (1 << 2) | 1)
#define AARCH64_BOOT_CAPACITY         NR_CPUS
#define AARCH64_BOOT_AFFINITY_MASK   0x000000ff00ffffff
#define AARCH64_BOOT_GO_WAIT          0
#define AARCH64_BOOT_GO_TEST          1
#define AARCH64_BOOT_GO_IDLE          2

/* Phase 2 #3: assembly-safe sizeof(percpu_t). Mirrors the value in
 * kernel/include/percpu/percpu.h (the C header is not assembly-safe
 * because it pulls in C function prototypes and struct types).
 * Keep both copies in sync. */
#ifndef PERCPU_DATA_SIZE
#define PERCPU_DATA_SIZE  144
#endif

/* boot_context field offsets the low boot path reads from the handoff
 * (struct boot_context in core/bootinfo.h is not assembly-safe).
 * Pinned by _Static_asserts in bootinfo.h and re-pinned by
 * hosttests/cases/test_bootinfo_abi.c. */
#define BOOT_CTX_MAGIC_OFF             0
#define BOOT_CTX_FLAGS_OFF             12
#define BOOT_CTX_EARLY_CONSOLE_OFF     176
#define BOOT_CTX_EARLY_DEVBASE_OFF     184
#define BOOT_CTX_EARLY_DEVBLOCKS_OFF   192
/* Assembly-safe BOOT_CONTEXT_HAS_EARLY_MMIO (enum in bootinfo.h). */
#define BOOT_CTX_FLAG_EARLY_MMIO       (1 << 7)
#define BOOT_CTX_MAGIC                 0x4f533031

#endif
