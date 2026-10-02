// kernel/arch/x86_64/memory/boot.c — x86 boot-time memory subsystem
// stage, extracted from kernel/core/main.c as part of the kernel-main
// refactor (Task 4).
//
// Owns x86_64_boot_memory(); see kernel/include/arch/x86_64/boot.h for
// the public contract.  This file lives under arch/x86_64/memory/ (next
// to pmm_arch.c) because the stage wires up the E820-derived PMMngr
// boundary fields that pmm_arch.c later translates into MEMORY_RANGE[],
// and because the framebuffer remap is a per-arch direct-PDE write into
// PUD[0] (see core/printk.h comments on VIRT_FRAMEBUFFER_OFFSET).
//
// Call order in kernel_main:
//   x86_64_boot_early(bootctx);       // IDT + serial + NXE
//   x86_64_boot_memory(bootctx);      // PMMngr + early FB + logo + PMM/VMM + FB remap
//   x86_64_boot_subsystems(bootctx);  // arch_boot_rsdp + register + init_all
//   random_init(bootctx);             // stays in main.c
//
// All log lines and call ordering from the legacy lines 110-123 of
// kernel_main() are preserved verbatim.

#include <arch/x86_64/boot.h>

#include <core/printk.h>            /* Pos, frame_buffer_*,
                                       color_printk                       */
#include <driver/logo.h>            /* boot_logo_show                     */
#include <memory/memory.h>          /* pmm_init, vmm_init                 */
#include <memory/pmm.h>             /* struct Physical_Memory_Manager     */
#include <arch/boot_memory.h>
#include <arch/cpu.h>
#include <log/log.h>
#include <sched/task.h>             /* _text / _etext / _edata / _erodata
                                       / _end externs                     */

// Stage 2 of 3: physical-memory bookkeeping, PMM + VMM bring-up, and
// the framebuffer remap.  Mirrors legacy lines 110-123 of kernel_main().
// The five _text / _etext / _edata / _erodata / _end symbols come from
// the linker script and mark the kernel's resident code/rodata/data
// segments — pmm_init() uses them to carve out kernel-reserved pages
// from the bitmap.  boot_logo_show() must run after frame_buffer_early_init()
// because Pos.FB_addr is only valid after the early direct-PDE map.
void x86_64_boot_memory(const struct boot_context *bootctx)
{
    PMMngr.start_code  = (uint64_t)&_text;
    PMMngr.end_code    = (uint64_t)&_etext;
    PMMngr.end_data    = (uint64_t)&_edata;
    PMMngr.end_rodata  = (uint64_t)&_erodata;
    PMMngr.start_brk   = (uint64_t)&_end;

    frame_buffer_early_init();
    boot_logo_show();                 // OS01 boot logo

    pmm_init(bootctx);                       // physical page allocator

    int rc = arch_boot_direct_map_init();
    if (rc != 0) {
        log_err("boot direct map failed: %d", rc);
        for (;;) arch_cpu_halt();
    }

    frame_buffer_init();                 // remap FB at VIRT_FRAMEBUFFER_OFFSET
    color_printk(GREEN, BLACK, "frame buffer remap succeed\n");
}
