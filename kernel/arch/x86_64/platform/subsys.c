// kernel/arch/x86_64/subsys.c

#include <stdint.h>
#include <subsys/subsys.h>

// ── RSDP 地址（由 kernel_main 在调用 arch_register_subsys 前设置） ──
uint64_t arch_boot_rsdp = 0;

// ── Arch 注册入口 ─────────────────────────────────────────
// Drivers register themselves via SUBSYS_INITCALL() in their .c files.
// Each macro places a function pointer in the .subsys_init linker section
// (see kernel/arch/x86_64/linker.ld). We iterate that table here so
// kernel_main doesn't need a hardcoded driver list.
//
// This is libc-free kernel style: __attribute__((constructor)) would
// put pointers in .init_array, but no startup code iterates .init_array
// in this freestanding build. The SUBSYS_INITCALL pattern is what Linux
// uses for the same reason.
void arch_register_subsys(void)
{
    for (subsys_initcall_t *p = __subsys_init_start;
         p < __subsys_init_end; p++) {
        (*p)();
    }
}
