#ifndef OS01_UEFI_AARCH64_RP1_PCI_H
#define OS01_UEFI_AARCH64_RP1_PCI_H

struct boot_rp1_pci_info;

/* Query UEFI EFI_PCI_IO_PROTOCOL handles for the unique RP1 C0 endpoint
 * (1de4:0001, rev 2), validate its BAR0 attributes and MSI-X capability,
 * and populate *out. Returns 0 on success; on failure zeroes *out and
 * returns a negative value. */
int uefi_rp1_pci_capture(struct boot_rp1_pci_info *out);

#endif /* OS01_UEFI_AARCH64_RP1_PCI_H */
