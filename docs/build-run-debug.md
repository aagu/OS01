# Build, Run, and Debug Guide

This guide details how to build, run, and debug this x86_64 operating system
project. The build is profile-based since the 2026-09-02 GNU Make refactor:
`PROFILE=x86_64-clang` is the default; `PROFILE=aarch64-clang` is the aarch64
UEFI bring-up profile. See [`docs/build.md`](build.md) for the profile
contract and output layout.

## Environment setup

### Required dependencies

1. **Compile toolchain**
   * Clang/LLVM (kernel + userland)
   * GNU Make
   * ld.lld (linker)

2. **Build dependencies**
   * dosfstools (mkfs.vfat)
   * mtools (mmd, mcopy)
   * e2fsprogs (mke2fs, debugfs)

3. **Run and debug**
   * QEMU (`qemu-system-x86_64`; `qemu-system-aarch64` for the aarch64 profile)
   * OVMF.fd (UEFI firmware — QEMU UEFI environment)

### Installing dependencies

#### Ubuntu/Debian

```bash
sudo apt update
sudo apt install clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86

# OVMF.fd, if needed
wget https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O boot/uefi/OVMF.fd
```

#### Arch Linux

```bash
sudo pacman -S clang llvm lld make dosfstools mtools e2fsprogs qemu-system-x86_64 edk2-ovmf
sudo cp /usr/share/edk2/x64/OVMF.fd boot/uefi/
```

## Build steps

### 1. Clone the project

```bash
git clone <repo-url>
cd OS01
git submodule update --init    # busybox + posix-uefi submodules
```

### 2. Build the whole project

```bash
make
```

This is equivalent to `make PROFILE=x86_64-clang disk.img`. It publishes a
sysroot generation, builds the UEFI bootloader (BOOTX64.EFI via the posix-uefi
adapter), the kernel, all user programs, BusyBox, and the GPT dual-partition
disk image `build/x86_64-clang/image/disk.img` (with the project-root
`disk.img` as a content-guarded compat copy).

### 3. Build a single component

```bash
make kernel.bin     # kernel (project-root compat copy)
make lib            # sysroot libraries (publishes a sysroot generation)
make user           # user ELFs + BusyBox
make image          # disk image artifact (variant-aware)
```

## Run steps

### 1. Run the system (x86_64)

```bash
make run
```

Runs QEMU (`-M q35 -smp 2`) with OVMF firmware and the disk image, serial
output on stdio. Capability note: `run` requires the `rootfs` capability, so
`make PROFILE=aarch64-clang run` fails at the capability gate.

```bash
make run-kvm        # with KVM acceleration
make run-virtio     # virtio-net instead of e1000e
```

### 2. Debug

```bash
make debug
```

Starts QEMU paused with a GDB remote server on :1234. In another terminal:

```bash
gdb build/x86_64-clang/kernel/kernel.elf
# target remote localhost:1234
# break kernel_main / continue / ...
```

The VS Code configuration under `.vscode` also drives `make debug`.

### 3. aarch64 UEFI bring-up

```bash
make PROFILE=aarch64-clang run-aarch64-uefi
```

Runs `qemu-system-aarch64 -M virt -display none -serial stdio` with the
64 MiB FAT bring-up image (BOOTAA64.EFI + kernel.elf + firmware). Boot
signatures on serial: `aarch64 uefi handoff ok`, then `phase1 boot ok`.
`make PROFILE=aarch64-clang aarch64-uefi` builds the image + firmware;
`aarch64-uefi-kernel` builds the kernel ELF. All three require the
`uefi-bringup` capability (they fail cleanly under the x86 profile).

## Tests

### Host tests

```bash
make test
```

Runs the host-side test suites in `test/` (`Suites: 16 | Failed: 0`).

### QEMU E2E tests (variant-isolated images)

Each x86 E2E target builds its image **variant** into an isolated directory
and runs `tests/run_test.py` against that exact image via the `DISK_IMG`
environment variable. Variant builds **never delete or overwrite the normal
image** `build/x86_64-clang/image/disk.img`: before and after each variant
build the normal image's sha256 is recorded (`image/normal.before` /
`image/normal.after`) and compared, so a variant build that touched the
normal image fails loudly.

| Target | Variant image | Suite |
|--------|---------------|-------|
| `make test-phase-0` | normal `build/<profile>/image/disk.img` | boot + shell prompt |
| `make test-syscall` | `build/<profile>/image/systest/disk.img` | syscall E2E (`OS01_SYSTEST=1`) |
| `make test-inittab` | `build/<profile>/image/inittab-test/disk.img` | inittab phase dispatch (`INITTAB_FILE=config/inittab.test`) |
| `make test-network` | `build/<profile>/image/nettest/disk.img` | network regression (`OS01_NETTEST=1`) |

The systest variant is **compile-affecting**: `OS01_SYSTEST=1` adds
`-DOS01_SYSTEST` to the user CFLAGS, so the variant's user programs build
into their own object/artifact dirs
(`build/<profile>/user/systest`, `build/<profile>/artifacts/user/systest`)
and the variant image contains a systest-compiled `init.elf` that spawns
`/bin/systest` instead of the BusyBox shell. The other two variants
(nettest, inittab-test) only change the inittab file and image dir; their
user binaries are shared with the normal build.

You can also build a variant image directly:

```bash
make OS01_SYSTEST=1 image          # → build/x86_64-clang/image/systest/disk.img
make OS01_NETTEST=1 image          # → build/x86_64-clang/image/nettest/disk.img
make INITTAB_FILE=config/inittab.test image   # → .../image/inittab-test/disk.img
```

## Config files

The system behavior is configured via `config/` (BusyBox config, the
`config/rootfs.mk` disk-image manifest, and the inittab templates
`config/inittab`, `config/inittab.systest`, `config/inittab.nettest`,
`config/inittab.test`).

## Project structure

* `boot/` - bootloader (`uefi/` UEFI bootloader)
* `kernel/` - kernel (arch/, driver/, fs/, intr/, memory/, sched/, subsys/, time/, tty/)
* `libc/` - system library (libk for the kernel, libc for user programs)
* `user/` - user-space programs
* `mk/` - build modules (project.mk, profiles/, targets/, toolchains/, components/)
* `config/` - configuration files
* `test/` - host test code
* `tests/` - E2E scripts (`run_test.py`)
* `tools/` - build tools (mkdisk)
* `docs/` - documentation

### Build artifacts

* `boot/uefi/BOOTX64.EFI` - UEFI bootloader
* `kernel.bin` - kernel binary (project root, default-profile compat copy)
* `build/x86_64-clang/artifacts/kernel.bin` - kernel artifact
* `build/x86_64-clang/kernel/kernel.elf` - kernel ELF (debug symbols, for GDB)
* `build/x86_64-clang/artifacts/user/*.elf` - user program ELFs
* `disk.img` - GPT dual-partition disk image (project root compat copy)
* `build/x86_64-clang/image/disk.img` - disk image artifact

## Common issues

### Build fails: `PROFILE='...' lacks capability '...'`

The target is not available for the selected profile (e.g. `run` /
`test-*` under `aarch64-clang`, or `aarch64-uefi` under the x86 profile).
Use the matching profile/target.

### QEMU cannot start

Check that QEMU is installed and that `boot/uefi/OVMF.fd` exists.

### No output after boot

Check the serial connection and that `serial_printk` is invoked.
