/* kernel/include/arch/x86_64/pci.h — x86_64 PCI definitions and backend interface */
#ifndef _ARCH_X86_64_PCI_H
#define _ARCH_X86_64_PCI_H

#include <stdint.h>
#include <arch/pci.h>

const struct pci_backend *x86_pci_backend(void);

#endif /* _ARCH_X86_64_PCI_H */
