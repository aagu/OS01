#include <arch/aarch64/boot_offsets.h>

/* ──────────────────────────────────────────────────────────────────
 * Helpers exposed to high-half C code (smp.c) so it can address the
 * low-half .boot.bss storage without triggering an out-of-range
 * R_AARCH64_ADR_PREL_PG_HI21 relocation.
 *
 * Each helper returns the absolute LOW PHYSICAL address of the
 * corresponding structure in x0, which smp.c may then cast to a
 * volatile pointer.
 * ────────────────────────────────────────────────────────────────── */
.section .text, "ax"

/* x0 = cpu_id; returns &aarch64_boot_percpu[cpu_id] (low phys).
 * aarch64_boot_percpu_t is 48 bytes; stride = 48 = 32 + 16. */
.globl aarch64_percpu_slot_addr
aarch64_percpu_slot_addr:
    ldr     x1, =aarch64_boot_percpu
    mov     x2, #AARCH64_BOOT_PERCPU_SIZE
    madd    x0, x0, x2, x1
    ret

/* x0 = cpu_id; returns stack top (low phys). */
.globl aarch64_boot_stack_top_addr
aarch64_boot_stack_top_addr:
    ldr     x1, =aarch64_boot_stacks
    add     x0, x1, x0, lsl #AARCH64_BOOT_STACK_SHIFT
    add     x0, x0, #AARCH64_BOOT_STACK_SIZE
    ret

/* Returns &aarch64_dtb_mpidr_table (low phys). */
.globl aarch64_dtb_mpidr_table_addr
aarch64_dtb_mpidr_table_addr:
    ldr     x0, =aarch64_dtb_mpidr_table
    ret

/* Returns &aarch64_dtb_cpu_count (low phys). */
.globl aarch64_dtb_cpu_count_addr
aarch64_dtb_cpu_count_addr:
    ldr     x0, =aarch64_dtb_cpu_count
    ret

/* Returns secondary_start (low phys, PSCI cpu_on entry). */
.globl secondary_start_addr
secondary_start_addr:
    ldr     x0, =secondary_start
    ret

/* Returns the pre-MMU PC-relative _start address for arch_early_init. */
.globl aarch64_boot_observed_entry
aarch64_boot_observed_entry:
    ldr     x0, =aarch64_boot_observed_entry_value
    ldr     x0, [x0]
    ret

.globl aarch64_boot_page_tables_addr
aarch64_boot_page_tables_addr:
    ldr     x0, =boot_page_tables
    ret

.globl aarch64_boot_page_tables_end_addr
aarch64_boot_page_tables_end_addr:
    ldr     x0, =boot_page_tables_end
    ret

/* Returns _boot_start (low physical LMA of the kernel image). High-half
 * C cannot take the address directly because adrp+add cannot span the
 * ~2^48 gap to the low physical linker symbol; the ldr literal pool
 * stays within +/-1 MiB of the PC and is link-time resolved. */
.globl aarch64_boot_image_start_addr
aarch64_boot_image_start_addr:
    ldr     x0, =_boot_start
    ret

/* Returns _kernel_lma_end (low physical LMA just past the kernel
 * image). Same rationale as aarch64_boot_image_start_addr. */
.globl aarch64_kernel_lma_end_addr
aarch64_kernel_lma_end_addr:
    ldr     x0, =_kernel_lma_end
    ret

.globl aarch64_runtime_root_address
aarch64_runtime_root_address:
    ldr x0, =runtime_ttbr1_pa
    ret
.globl aarch64_probe_pa_address
aarch64_probe_pa_address:
    ldr x0, =pt_probe_pa
    ret
.globl aarch64_probe_expected_address
aarch64_probe_expected_address:
    ldr x0, =pt_probe_expected
    ret
