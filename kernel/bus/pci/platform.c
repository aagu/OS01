/* kernel/bus/pci/platform.c — Weak platform backend provider for PCI subsystem */
#include <arch/pci.h>
#include <stddef.h>

__attribute__((weak)) const struct pci_backend *arch_pci_backend(void)
{
    return NULL;
}
