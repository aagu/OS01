#ifndef OS01_AARCH64_BOOT_OFFSETS_H
#define OS01_AARCH64_BOOT_OFFSETS_H

/* Integer-only shared C/assembly ABI. */
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
#define AARCH64_BOOT_CAPACITY         8
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

#endif
