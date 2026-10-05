/* kernel/bus/pci/platform.c — Weak platform backend provider for PCI subsystem */
#include <arch/pci.h>
#include <stddef.h>
#include <errno.h>

__attribute__((weak)) const struct pci_backend *arch_pci_backend(void)
{
    return NULL;
}

__attribute__((weak)) int arch_pci_msix_map(const struct pci_device *pdev, uint64_t table_phys, void **out_virt)
{
    (void)pdev;
    (void)table_phys;
    (void)out_virt;
    return -ENOTSUP;
}

__attribute__((weak)) void arch_pci_msix_unmap(const struct pci_device *pdev, void *virt)
{
    (void)pdev;
    (void)virt;
}

__attribute__((weak)) uint32_t arch_pci_msi_address(const struct pci_device *pdev)
{
    (void)pdev;
    return 0xFEE00000;
}
