/* kernel/arch/aarch64/driver/rp1_xhci.c — RP1 xHCI platform provider (stub for T2, hardware in T4) */
#include <subsys/subsys.h>

static int rp1_xhci_init(void)
{
    return 0;
}

static const char *const rp1_xhci_requires[] = { "device-boot" };

static int rp1_xhci_register(void)
{
    return register_subsys_deps("rp1-xhci", rp1_xhci_init,
                                SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL,
                                rp1_xhci_requires, 1);
}
SUBSYS_INITCALL(rp1_xhci_register);
