#ifndef ARCH_FEATURES_H
#define ARCH_FEATURES_H

/*
 * Architecture Feature Gates
 *
 * Explicit 0 or 1 definitions representing platform subsystem availability.
 * Defaults are per-architecture, but can be overridden by build flags.
 */

#ifndef OS01_HAS_VFS
#if defined(__x86_64__)
#define OS01_HAS_VFS        1
#elif defined(__aarch64__)
/* 2026-11-18: M4-remaining "aarch64 opens the VFS gate" — the arch-neutral
 * fs/ stack (vfs/devfs/ext2/fat/tmpfs/procfs), tty core and block layer now
 * link into the aarch64 product kernel.  /bin/init userspace stays behind
 * OS01_HAS_USERSPACE (still 0) until the aarch64 userland build chain
 * (crt0/linker/libc) exists. */
#define OS01_HAS_VFS        1
#else
#error "Unsupported architecture for OS01_HAS_VFS default"
#endif
#endif

#ifndef OS01_HAS_USERSPACE
#if defined(__x86_64__)
#define OS01_HAS_USERSPACE  1
#elif defined(__aarch64__)
#define OS01_HAS_USERSPACE  0
#else
#error "Unsupported architecture for OS01_HAS_USERSPACE default"
#endif
#endif

/* OS01_HAS_NET: the lwIP-backed socket fd paths inside kernel/fs/file.c and
 * kernel/fs/poll.c.  aarch64 does not build lwIP yet, so those FD_SOCKET
 * code blocks are compiled out while the fs syscalls stay hidden behind
 * OS01_SYSCALL_A64_SUBSET's non-fs entries. */
#ifndef OS01_HAS_NET
#if defined(__x86_64__)
#define OS01_HAS_NET        1
#elif defined(__aarch64__)
#define OS01_HAS_NET        0
#else
#error "Unsupported architecture for OS01_HAS_NET default"
#endif
#endif

/* OS01_HAS_PCI / OS01_HAS_USB: device-node registration surfaces on top of
 * devfs (pci_devfs_register_all / usb_devfs_register_all).  The xHCI, PCI
 * bus backends and USB stack are x86-only today; aarch64 registers no PCI/
 * USB device nodes. */
#ifndef OS01_HAS_PCI
#if defined(__x86_64__)
#define OS01_HAS_PCI        1
#elif defined(__aarch64__)
#define OS01_HAS_PCI        0
#else
#error "Unsupported architecture for OS01_HAS_PCI default"
#endif
#endif

#ifndef OS01_HAS_USB
#if defined(__x86_64__)
#define OS01_HAS_USB        1
#elif defined(__aarch64__)
#define OS01_HAS_USB        0
#else
#error "Unsupported architecture for OS01_HAS_USB default"
#endif
#endif

#if (OS01_HAS_VFS != 0 && OS01_HAS_VFS != 1)
#error "OS01_HAS_VFS must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_USERSPACE != 0 && OS01_HAS_USERSPACE != 1)
#error "OS01_HAS_USERSPACE must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_USERSPACE && !OS01_HAS_VFS)
#error "OS01_HAS_USERSPACE requires OS01_HAS_VFS"
#endif

#if (OS01_HAS_NET != 0 && OS01_HAS_NET != 1)
#error "OS01_HAS_NET must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_PCI != 0 && OS01_HAS_PCI != 1)
#error "OS01_HAS_PCI must be explicitly defined as 0 or 1"
#endif

#if (OS01_HAS_USB != 0 && OS01_HAS_USB != 1)
#error "OS01_HAS_USB must be explicitly defined as 0 or 1"
#endif

#endif /* ARCH_FEATURES_H */
