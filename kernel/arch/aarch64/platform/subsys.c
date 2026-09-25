// ── kernel/arch/aarch64/subsys.c ──────────────────────────────
//
// arch_register_subsys() iterator — byte-mirror of x86_64 subsys.c
// minus arch_boot_rsdp (RSDP is x86 ACPI concept; not referenced
// anywhere in aarch64 sources).
//
// Drivers place a function pointer in the .subsys_init linker section
// via SUBSYS_INITCALL(). arch_register_subsys() iterates this range
// and calls each — kernel_main does not need a hardcoded driver list.
//
// This is the libc-free kernel style: __attribute__((constructor))
// puts pointers in .init_array, but no startup code iterates
// .init_array in this freestanding aarch64 build. SUBSYS_INITCALL is
// what Linux uses for the same reason.

#include <stdint.h>
#include <subsys/subsys.h>

void arch_register_subsys(void)
{
    for (subsys_initcall_t *p = __subsys_init_start;
         p < __subsys_init_end; p++) {
        (*p)();
    }
}
