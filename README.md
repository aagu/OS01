# OS01 — x86_64 Operating System from Scratch

A mature hobby x86_64 operating system built entirely from scratch, running on real hardware and QEMU.

## Quick facts

- **Architecture:** x86_64, UEFI boot, higher-half kernel (`0xffff800000000000`)
- **SMP:** Symmetric multi-processing (NR_CPUS=8, default `-smp 2`)
- **Memory:** 2MB huge pages, PMM+VMM+Slab allocator, COW fork, mmap/mprotect/munmap
- **Scheduler:** Preemptive round-robin with CPU affinity, global task list
- **Interrupts:** APIC/IOAPIC/LAPIC timer, IPI (TLB shootdown + resched), 8259A PIC fallback
- **Filesystems:** FAT32 R/W, ext2 read-only + tmpfs + devfs + procfs, GPT dual partition
- **Drivers:** PS/2 keyboard, 16550 serial, PIT, AHCI SATA, framebuffer
- **User-space:** Busybox ash shell + applets, libc (printf/malloc/string/syscall), init
- **Syscalls:** 48 syscalls (0-47), `int $0x80`, including fork/exec/waitpid, mmap, signal, futex
- **Build:** Clang/LLVM, ld.lld, no cross-compiler needed, `x86_64-unknown-none` target
- **Debug:** QEMU + GDB, serial debug output, `DEBUG_CHANNELS` logging framework
- **Init:** Subsystem registration framework for arch-agnostic initialization

## Quick start

```bash
make run       # Build + run (-smp 2, gtk display, serial stdio)
make debug     # Build + QEMU paused, GDB :1234
make clean     # MANDATORY after struct changes (no header deps!)
```

## Dependencies

- clang, llvm, lld, make, dosfstools, mtools, qemu-system-x86_64
- OVMF.fd (UEFI firmware for QEMU)
- Busybox submodule: `git submodule update --init`

## Documentation

Check `docs/` for detailed documentation on architecture, SMP bring-up, scheduler, syscalls, memory management, interrupts, and more.

## License

OS01 is released under the [MIT license](LICENSE).

Third-party components are consumed under their own licenses and are **not**
covered by OS01's license:

| Component | License | Form |
|---|---|---|
| busybox 1.36.1 | GPL-2.0-only | submodule (`thirdpart/busybox-1.36.1`) |
| doomgeneric | GPL-2.0 | submodule (`thirdpart/doomgeneric`) |
| lwIP | BSD-3-Clause | submodule (`thirdpart/lwip`) |
| mbedtls | Apache-2.0 OR GPL-2.0-or-later | submodule (`thirdpart/mbedtls`) |
| LVGL | MIT | submodule (`thirdpart/lvgl`) |
| posix-uefi | MIT | submodule (`thirdpart/posix-uefi`) |
| musl algorithms (floatscan/strtod) | MIT | pinned archive (`thirdpart/musl-libc-algorithms/`, manifest records provenance + SHA256) |

Note on distribution: `disk.img` ships BusyBox/doomgeneric binaries next to the
OS01 kernel — they are separate programs (mere aggregation), each remaining
under its own license.

## References

- 《一个64位操作系统的设计与实现》 (https://www.ituring.com.cn/book/2450)
- osdev wiki (https://wiki.osdev.org/Main_Page)
