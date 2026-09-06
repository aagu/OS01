# OS01 GNU Make Build Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace OS01's implicit recursive-Make coupling with profile-private artifacts, a single-writer sysroot, read-only external adapters, and stable Make entry points.

**Architecture:** The root Makefile includes focused `mk/` modules and owns every inter-component artifact edge. OS01 component Makefiles only consume an explicit profile configuration and component-private paths; copied BusyBox, mbedTLS, and posix-uefi adapters remain below `build/<profile>/`. x86 rootfs and aarch64 UEFI bring-up remain separate capability graphs.

**Tech Stack:** GNU Make, POSIX shell, Clang/LLVM, lld, QEMU, mtools.

**Spec:** [docs/superpowers/specs/2026-09-02-build-system-design.md](../specs/2026-09-02-build-system-design.md)

## Global Constraints

- Do not add CMake, Ninja, Meson, Bazel, downloads, remote caching, or a compiler-runtime replacement for `-lgcc`.
- All generated objects, archives, copied third parties, temporary files, sysroots, and images are below `build/<profile>/`; source and `thirdpart/` trees stay unchanged.
- Keep all current public targets: `make`, `kernel.bin`, `disk.img`, `lib`, `user`, run/debug/validate targets, all test targets, aarch64 UEFI targets, and `clean`.
- `x86_64-clang` supports `kernel,userland,rootfs,uefi`; `aarch64-clang` supports only `kernel,uefi-bringup`.
- After each Makefile dependency/flag edit, clean the affected profile before verification.

---

### Task 1: Add build-contract regression checks

**Files:**
- Create: `tests/build_contract.sh`
- Modify: `Makefile:342-377`
- Modify: `.gitignore`

**Interfaces:** Produces `tests/build_contract.sh <profile> <x86|aarch64|sysroot|targets>`, which returns nonzero for missing artifacts, shared output paths, stale sysroot paths, or unsupported profile targets.

- [ ] **Step 1: Write the failing profile-artifact check**

Create this executable shell test:

```sh
#!/bin/sh
set -eu
profile=$1
mode=$2
base="build/$profile"
test -d "$base"
test -d "$base/sysroot-generations"
test -L "$base/sysroot"
test ! -e "$base/sysroot/data/data/com.termux/files/usr"
case "$mode" in
x86) test -f "$base/artifacts/kernel.bin"; test -f "$base/image/disk.img" ;;
aarch64) test -f "$base/artifacts/kernel.elf"; test -f "$base/image/aarch64-uefi.img" ;;
sysroot) test -f "$base/sysroot/usr/include/kernel/bootinfo.h"; test -f "$base/sysroot/usr/lib/libc.a" ;;
targets) make -n PROFILE=x86_64-clang kernel.bin disk.img lib user validate test-syscall >/dev/null; ! make -n PROFILE=aarch64-clang user ;;
*) exit 64 ;;
esac
```

- [ ] **Step 2: Verify the red state**

Run: `sh tests/build_contract.sh x86_64-clang x86`

Expected: failure because the current tree has `build/x86_64/`, no profile sysroot generation, and no `artifacts/` directory.

- [ ] **Step 3: Expose the checks and commit**

Add `test-build-contract-x86` and `test-build-contract-aarch64` phony targets to the root Makefile, add `build/` to `.gitignore`, then run `sh -n tests/build_contract.sh && make -n test-build-contract-x86`.

```bash
git add tests/build_contract.sh Makefile .gitignore
git commit -m "test(build): add profile artifact contract checks"
```

### Task 2: Introduce profiles and controlled recursive Make

**Files:**
- Create: `mk/project.mk`, `mk/toolchains/clang.mk`
- Create: `mk/targets/x86_64.mk`, `mk/targets/aarch64.mk`
- Create: `mk/profiles/x86_64-clang.mk`, `mk/profiles/aarch64-clang.mk`
- Modify: `Makefile:1-47`, `toolchain.mk`

**Interfaces:** Produces `PROFILE`, `BUILD_DIR`, `SYSROOT`, `TARGET_INCLUDEDIR`, `TARGET_LIBDIR`, component build directories, artifact paths, `PROFILE_CAPABILITIES`, `require_capability`, and `os01_submake`.

- [ ] **Step 1: Add failing profile selection**

Add this parse-time contract in `mk/project.mk` and run `make -f mk/project.mk PROFILE=missing -n`:

```make
PROFILE ?= x86_64-clang
OS01_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OS01_PROFILE_FILE := $(OS01_ROOT)/mk/profiles/$(PROFILE).mk
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error unsupported PROFILE='$(PROFILE)')
endif
include $(OS01_PROFILE_FILE)
define require_capability
$(if $(filter $(1),$(PROFILE_CAPABILITIES)),,$(error PROFILE='$(PROFILE)' lacks capability '$(1)'))
endef
```

Expected: `unsupported PROFILE='missing'`.

- [ ] **Step 2: Implement profile-private paths and capabilities**

In the x86 profile define:

```make
PROFILE_CAPABILITIES := kernel userland rootfs uefi
BUILD_DIR := $(OS01_ROOT)/build/$(PROFILE)
SYSROOT := $(BUILD_DIR)/sysroot
TARGET_INCLUDEDIR := $(SYSROOT)/usr/include
TARGET_LIBDIR := $(SYSROOT)/usr/lib
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel
LIBC_BUILD_DIR := $(BUILD_DIR)/libc
USER_BUILD_DIR := $(BUILD_DIR)/user
UEFI_BUILD_DIR := $(BUILD_DIR)/uefi
UEFI_RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
```

Define the same paths in aarch64 but set `PROFILE_CAPABILITIES := kernel uefi-bringup` and retain its current compiler/QEMU settings. Move validated Clang discovery to `mk/toolchains/clang.mk`; leave `toolchain.mk` as a one-release compatibility include.

In `kernel/Makefile`, `libc/Makefile`, `user/Makefile`, and `kernel/arch/x86_64/make.config`, make profile mode mutually exclusive with legacy mode: when `OS01_PROFILE_FILE` exists, include only it and do not include `toolchain.mk` or recompute `BUILD_DIR`, `SYSROOT`, `TARGET_*`, or tool flags. Retain the old include path only when invoked standalone without a profile.

- [ ] **Step 3: Implement environment isolation**

Create this root-only submake template after removing unapproved assignments from `MAKEOVERRIDES`:

```make
define os01_submake
+env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
  MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
  -C $(1) OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" $(2)
endef
```

`OS01_SUBMAKEFLAGS` retains Make options and jobserver authentication only. Explicitly allow `CLANG`, `UEFI_CLANG`, LLVM tool paths, QEMU paths, `LOG_TARGET`, `KERNEL_SELFTEST`, `OS01_SYSTEST`, `OS01_NETTEST`, and `INITTAB_FILE`; reject all other compile/link/prefix variables.

- [ ] **Step 4: Verify isolation and commit**

Run: `env CFLAGS=-BROKEN PREFIX=/data/data/com.termux/files/usr make PROFILE=x86_64-clang -n kernel.bin | tee /tmp/os01-make.log`

Run: `! grep -F -- -BROKEN /tmp/os01-make.log && ! grep -F -- /data/data/com.termux/files/usr/include /tmp/os01-make.log`

```bash
git add Makefile toolchain.mk mk
git commit -m "refactor(build): introduce explicit Make profiles"
```

### Task 3: Make kernel and libc staging producers

**Files:**
- Modify: `kernel/Makefile:1-272`, `kernel/arch/x86_64/make.config`, `kernel/arch/aarch64/make.config`
- Modify: `libc/Makefile:1-104`
- Create: `mk/components/kernel.mk`
- Modify: `tests/build_contract.sh`

**Interfaces:** Consumes `OS01_PROFILE_FILE`, `KERNEL_BUILD_DIR`, `LIBC_BUILD_DIR`, `INSTALL_ROOT`; produces component-private kernel files, libc archives, and `staging/<component>/manifest`. It does not add a root kernel consumer until Task 4 publishes a sysroot generation.

- [ ] **Step 1: Extend the red test**

Add checks:

```sh
test ! -e kernel/arch/x86_64/trampoline.bin
test ! -e libc/libc.a
test ! -e libc/libk.a
test -f "build/$profile/staging/kernel-headers/manifest"
test -f "build/$profile/staging/libc/manifest"
```

Run: `make clean && make kernel.bin && sh tests/build_contract.sh x86_64-clang x86`.

Expected: the profile/staging assertions fail on the old rules.

- [ ] **Step 2: Parameterize output and install paths**

Require `OS01_PROFILE_FILE` in root-driven kernel/libc makes. Replace architecture-derived output directories with profile values. Archive libc under `$(LIBC_BUILD_DIR)/lib/`:

```make
LIBC_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libc.a
$(LIBC_ARCHIVE): $(LIBC_ALL_OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^
```

Require `INSTALL_ROOT` for installs, copy only to `$(INSTALL_ROOT)/usr/include` and `$(INSTALL_ROOT)/usr/lib`, and create `manifest` from sorted installed file paths. Move trampoline generated binaries to `$(KERNEL_BUILD_DIR)/generated/`. Keep the current x86 raw link and `-lgcc` intact; place the `<stdint.h>` injection in an explicit kernel-profile flag.


- [ ] **Step 3: Verify staging-only producers and commit**

Run: `make PROFILE=x86_64-clang clean && $(MAKE) -C kernel PROFILE=x86_64-clang OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk INSTALL_ROOT=$(pwd)/build/x86_64-clang/staging/kernel-headers install-headers && $(MAKE) -C libc PROFILE=x86_64-clang OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk INSTALL_ROOT=$(pwd)/build/x86_64-clang/staging/libc install`

Run: `test -f build/x86_64-clang/staging/kernel-headers/manifest && test -f build/x86_64-clang/staging/libc/manifest && test ! -e libc/libc.a`

```bash
git add kernel libc mk/components/kernel.mk tests/build_contract.sh
git commit -m "refactor(build): stage kernel and libc per profile"
```

### Task 4: Publish atomic sysroot generations

**Files:**
- Create: `mk/components/sysroot.mk`
- Modify: `mk/project.mk`, `Makefile`, `tests/build_contract.sh`

**Interfaces:** Consumes kernel/libc/mbedTLS/compat staging manifests; produces `sysroot-generations/<id>`, the `sysroot` symlink, sysroot stamps, publish locks, and generation leases. It then enables the root kernel artifact consumer.

- [ ] **Step 1: Add a stale-path failure check**

Add `test ! -e "build/$profile/sysroot/usr/include/os01-removed-header.h"` to sysroot mode and run `sh tests/build_contract.sh x86_64-clang sysroot`.

Expected: failure before generation publishing exists.


- [ ] **Step 2: Add mbedTLS staging before publication**

Move the existing mbedTLS compile loop into `sysroot.mk`. Before any copy, a `FORCE`-checked adapter recipe writes the sorted recursive SHA-256 list of `thirdpart/mbedtls`, `config/mbedtls_config.h`, and `libc/network/entropy.c` to `$(BUILD_DIR)/receipts/mbedtls.input.digest`; it compares this to the receipt stored beside the mbedTLS staging stamp, and rebuilds the private copy only when the digest differs. Copy into `$(BUILD_DIR)/thirdparty/mbedtls/`, compile only in `$(BUILD_DIR)/thirdparty/mbedtls-build/`, and install `libmbedtls.a` plus headers into `staging/mbedtls/` with a manifest and receipt. Do not write shared `/tmp`, third-party source files, or the final sysroot.

- [ ] **Step 3: Assemble generations with one writer**

Make `sysroot.mk` create an empty `sysroot-generations/<id>/`, copy only manifest-listed staging files into it, and fail on duplicate destinations. Create `compat-libs` only in `staging/compat-libs/` with its own manifest. Publish with:

```make
ln -s "sysroot-generations/$$id" "$(BUILD_DIR)/.sysroot.next"
mv -Tf "$(BUILD_DIR)/.sysroot.next" "$(SYSROOT)"
```


- [ ] **Step 4: Define the complete lock and lease protocol**

Use `build/.locks/$(PROFILE)/publish` as an atomic `mkdir` lock. Write PID, target, and start time to its owner file. Retry for 60 seconds with 100 ms sleeps, then print that owner file and fail; remove the lock through a trap on failed publication. While holding the lock, allocate an id by atomically renaming an incremented `next-generation` counter. Every artifact recipe must use one `+$(SHELL) -ec '...'` line: while locked resolve `sysroot`, create `<generation>.<pid>.<artifact>` lease, release lock, run the component with immutable `SYSROOT_GENERATION_DIR`, and remove the lease in a shell trap. `clean` takes the same lock and fails if leases exist. Add `unlock-profile` that prints owner data and only removes a lock when `FORCE_UNLOCK=1`.


- [ ] **Step 5: Add the root kernel consumer, verify, and commit**

In `mk/components/kernel.mk`, make `$(KERNEL_ARTIFACT)` depend on the sysroot stamp and invoke `kernel/Makefile` under the generation-lease wrapper; check its ELF header for `EM_X86_64` before publishing the binary. Make project-root `kernel.bin` depend on the default profile artifact.

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang lib && sh tests/build_contract.sh x86_64-clang sysroot`

Run terminal A: `OS01_BUILD_HOLD=5 make PROFILE=x86_64-clang kernel.bin`; within five seconds run terminal B: `make PROFILE=x86_64-clang clean`. The lease wrapper must sleep after acquisition when the test-only `OS01_BUILD_HOLD` is nonempty.

Expected: B fails while A holds a lease; after A finishes, clean succeeds.

```bash
git add mk/project.mk mk/components/sysroot.mk mk/components/kernel.mk Makefile tests/build_contract.sh
git commit -m "refactor(build): publish sysroot generations atomically"
```

### Task 5: Isolate userland and third-party adapters

**Files:**
- Modify: `user/Makefile:1-105`
- Create: `mk/components/user.mk`, `thirdpart/busybox.manifest`
- Create: `config/busybox.overlay/applets/Kbuild.src`, `config/busybox.overlay/applets/crt0.S`, `config/busybox.overlay/applets/sigreturn_trampoline.S`
- Modify: `tests/build_contract.sh`

**Interfaces:** Produces `artifacts/user/*.elf` and `artifacts/user/busybox.elf` without modifying third-party sources.

- [ ] **Step 1: Write immutable-source checks and show the red state**

Create build-private before/after snapshots, preserving any pre-existing user changes:

```sh
find thirdpart/busybox-1.36.1 -type f -print0 | sort -z | xargs -0 sha256sum > "build/$profile/busybox.before"
find thirdpart/mbedtls -type f -print0 | sort -z | xargs -0 sha256sum > "build/$profile/mbedtls.before"
```

Run the build, take the same two snapshots as `*.after`, and run `cmp build/$profile/busybox.before build/$profile/busybox.after && cmp build/$profile/mbedtls.before build/$profile/mbedtls.after`.

Expected: this red test records the current changes; after implementation the before/after snapshots match even if the user already has submodule modifications.

- [ ] **Step 2: Make user programs profile-private**

Have `user/Makefile` include the profile and use `USER_BUILD_DIR` plus leased `SYSROOT_GENERATION_DIR`. In `user.mk`, enumerate each user ELF explicitly and publish verified copies under `artifacts/user/`.


- [ ] **Step 3: Implement the BusyBox adapter**

The manifest records BusyBox source path and expected revision. A `FORCE`-checked adapter recipe calculates a sorted recursive SHA-256 digest across the actual BusyBox worktree, `config/busybox.config.in`, all tracked overlay files, `user/crt0.S`, and `user/sigreturn_trampoline.S`; it writes the digest to `$(BUILD_DIR)/receipts/busybox.input.digest` and compares it with the adapter-stamp receipt before deciding whether to recopy/rebuild. Thus an allowed local source modification invalidates the adapter even though its git revision did not change. Copy BusyBox to `build/<profile>/thirdparty/busybox/`, apply only the tracked crt0/sigreturn/Kbuild overlay, generate `.config` there, and run its Make there. BusyBox depends on the `compat-libs` stamp and never creates `libm.a` or `librt.a` itself.

- [ ] **Step 4: Verify and commit**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang user disk.img && cmp build/x86_64-clang/busybox.before build/x86_64-clang/busybox.after && cmp build/x86_64-clang/mbedtls.before build/x86_64-clang/mbedtls.after`

```bash
git add user mk/components thirdpart config/busybox.overlay tests/build_contract.sh
git commit -m "refactor(build): isolate userland third-party adapters"
```

### Task 6: Track UEFI runtime inputs and split image graphs

**Files:**
- Modify: `boot/uefi/Makefile:1-112`, `Makefile:105-311`, `tools/mkdisk.c`, `tools/Makefile`
- Create: `mk/components/uefi.mk`, `mk/components/image.mk`, `config/rootfs.mk`
- Create: `thirdpart/posix-uefi.manifest`
- Create: `config/posix-uefi/0001-clang-int8.patch`, `config/posix-uefi/0002-runtime-make-overlay.patch`

**Interfaces:** Produces UEFI runtime stamps, `BOOTX64.EFI`/`BOOTAA64.EFI`, manifest-built x86 disk image, and an aarch64 FAT bring-up image.

- [ ] **Step 1: Test current UEFI mutation behavior**

Run: `make -n -C boot/uefi ARCH=x86_64 | grep -F 'sed -i'`.

Expected: direct untracked runtime mutation appears.

- [ ] **Step 2: Build a tracked UEFI adapter**

Use the existing `thirdpart/posix-uefi` gitlink at commit `8d50e069f31dbfad6a19b426fdd1fe9cfa4e95ab`; do not create `.gitmodules` or add a second submodule. A `FORCE`-checked adapter recipe verifies the submodule is initialized, its `HEAD` equals the gitlink, and `git -C thirdpart/posix-uefi status --porcelain` is empty (including untracked `.o`/`.a` files). If any check fails it stops with the explicit initialization or clean-worktree diagnostic. It computes a canonical receipt digest over submodule path, gitlink SHA, clean status, manifest, both patches, profile, UEFI compiler identity, and boot source file hashes; it compares that digest to `$(BUILD_DIR)/receipts/uefi-runtime.input.digest` before deciding to recopy/rebuild. Copy it to `UEFI_RUNTIME_DIR`, apply `0001-clang-int8.patch` to change posix-uefi's fallback `int8_t` to signed char, then apply `0002-runtime-make-overlay.patch` to add `OUTDIR=` and `-DUEFI_NO_UTF8` to both copied Makefiles. Store the receipt beside the atomically written runtime stamp. Keep upstream `-j1` inside the adapter. x86 remains COFF/Clang; aarch64 retains independent Clang/lld behavior.

- [ ] **Step 3: Replace the inline disk file list with rootfs data**

Define in `config/rootfs.mk`:

```make
ROOTFS_FILES := /bin/init=$(USER_ARTIFACT_DIR)/init.elf:0755 /bin/busybox=$(USER_ARTIFACT_DIR)/busybox.elf:0755 /bin/spin=$(USER_ARTIFACT_DIR)/spin.elf:0755 /bin/sigtest=$(USER_ARTIFACT_DIR)/sigtest.elf:0755 /bin/poweroff=$(USER_ARTIFACT_DIR)/poweroff.elf:0755 /bin/halt=$(USER_ARTIFACT_DIR)/halt.elf:0755 /bin/reboot=$(USER_ARTIFACT_DIR)/reboot.elf:0755 /bin/systest=$(USER_ARTIFACT_DIR)/systest.elf:0755 /bin/test_mmap=$(USER_ARTIFACT_DIR)/test_mmap.elf:0755 /bin/test_fork_mmap=$(USER_ARTIFACT_DIR)/test_fork_mmap.elf:0755 /bin/test_cow=$(USER_ARTIFACT_DIR)/test_cow.elf:0755 /bin/terminal=$(USER_ARTIFACT_DIR)/terminal.elf:0755 /bin/smp_stress=$(USER_ARTIFACT_DIR)/smp_stress.elf:0755 /bin/socktest=$(USER_ARTIFACT_DIR)/socktest.elf:0755 /bin/udptest=$(USER_ARTIFACT_DIR)/udptest.elf:0755 /bin/ipaddr=$(USER_ARTIFACT_DIR)/ipaddr.elf:0755 /bin/nettest=$(USER_ARTIFACT_DIR)/nettest.elf:0755 /bin/tetris=$(USER_ARTIFACT_DIR)/tetris.elf:0755 /kernel.bin=$(KERNEL_ARTIFACT):0644 /etc/inittab=$(INITTAB_FILE):0644
ROOTFS_SYMLINKS := /bin/wget=busybox /bin/login=busybox /bin/sh=busybox /bin/[=busybox /bin/[[=busybox /bin/cat=busybox /bin/cp=busybox /bin/mv=busybox /bin/rm=busybox /bin/mkdir=busybox /bin/rmdir=busybox /bin/echo=busybox /bin/printf=busybox /bin/sort=busybox /bin/ps=busybox /bin/kill=busybox /bin/mount=busybox /bin/grep=busybox /bin/sed=busybox /bin/awk=busybox /bin/find=busybox /bin/xargs=busybox /bin/tar=busybox /bin/gzip=busybox /bin/gunzip=busybox /bin/ping=busybox /bin/ifconfig=busybox /bin/clear=busybox /bin/dmesg=busybox
```

Make `image.mk` parse each file item as `destination=source:mode`, copy to a fresh `rootfs.next/`, apply `chmod`, and emit `$(BUILD_DIR)/image/rootfs.manifest` with tab-separated `file<TAB>destination<TAB>source<TAB>mode` and `symlink<TAB>destination<TAB>target` rows. Change mkdisk to require `--output <image>`, `--temp-dir <dir>`, and `--rootfs-manifest <file>`; replace every `fopen("disk.img")`, `dd ... of=disk.img`, self-check open, and `/tmp/_mkdisk_*` path with these arguments. While filling the ext2 temporary image, mkdisk reads each manifest row, uses `debugfs write` for `file` entries followed by `debugfs set_inode_field <destination> mode 010<mode>`, and uses `debugfs symlink <destination> <target>` for `symlink` entries. Build the host binary as `$(BUILD_DIR)/host-tools/mkdisk`, not `tools/mkdisk`, and require `--temp-dir $(BUILD_DIR)/image/tmp` so parallel profiles cannot collide.

- [ ] **Step 4: Preserve aarch64 bring-up separation**

Move the existing 64 MiB FAT commands to `image.mk` under `uefi-bringup`; require exactly `BOOTAA64.EFI`, `kernel.elf`, and firmware. Reject BusyBox, mbedTLS, `user`, and rootfs dependencies for this capability.

- [ ] **Step 5: Verify both graphs and commit**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang disk.img validate-uefi`

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi && sh tests/build_contract.sh aarch64-clang aarch64`

Run: `! make -n PROFILE=aarch64-clang aarch64-uefi | grep -E 'busybox|mbedtls|make -C libc|make -C user'`

Run: `mdir -i build/x86_64-clang/image/disk.img ::/EFI/BOOT`.

Run: `dd if=build/x86_64-clang/image/disk.img of=build/x86_64-clang/image/rootfs.check.ext2 bs=512 skip=133120 count=262144 status=none && debugfs -R 'ls -l /bin' build/x86_64-clang/image/rootfs.check.ext2 | grep -E 'init|tetris|nettest|dmesg' && debugfs -R 'stat /bin/dmesg' build/x86_64-clang/image/rootfs.check.ext2 | grep -F 'Type: symlink' && debugfs -R 'stat /bin/dmesg' build/x86_64-clang/image/rootfs.check.ext2 | grep -F 'Fast link dest: "busybox"' && debugfs -R 'stat /bin/init' build/x86_64-clang/image/rootfs.check.ext2 | grep -F 'Mode:  0755' && debugfs -R 'stat /kernel.bin' build/x86_64-clang/image/rootfs.check.ext2 | grep -F 'Mode:  0644' && debugfs -R 'stat /etc/inittab' build/x86_64-clang/image/rootfs.check.ext2 | grep -F 'Mode:  0644'`.

Expected: all former rootfs programs and applet links are present; no image/temp file is created outside the profile build directory.

```bash
git add Makefile boot/uefi mk/components config thirdpart/posix-uefi.manifest tools/mkdisk.c tools/Makefile
git commit -m "refactor(build): model UEFI and image artifacts"
```

### Task 7: Restore public aliases, variants, validation, and documentation

**Files:**
- Create: `mk/components/run.mk`
- Modify: `Makefile:58-377`, `docs/build.md`, `docs/build-run-debug.md`, `docs/build/toolchain.md`
- Modify: `tests/build_contract.sh`, `docs/changelog.md`

**Interfaces:** Produces stable aliases, variant-isolated images, profile-aware validation, and documented override rules.

- [ ] **Step 1: Make each alias capability-aware**

Put QEMU, debug, test, validation, and compatibility recipes in `run.mk`. `run`, `run-kvm`, `run-virtio`, `debug`, and x86 tests require `rootfs`; `aarch64-uefi`, `aarch64-uefi-kernel`, and `run-aarch64-uefi` require `uefi-bringup`. Root `kernel.bin`/`disk.img` remain concrete default-profile compatibility copies. `clean` only removes root compatibility files for `PROFILE=$(DEFAULT_PROFILE)`.

- [ ] **Step 2: Isolate test image variants**

Map `OS01_SYSTEST=1`, `OS01_NETTEST=1`, and `INITTAB_FILE=config/inittab.test` to separate directories such as `build/x86_64-clang/image/systest/disk.img`. Pass the chosen image to `tests/run_test.py` as `DISK_IMG`; never delete or overwrite the normal disk image. Before each variant build record `sha256sum build/x86_64-clang/image/disk.img > build/x86_64-clang/image/normal.before`; after it record `normal.after` and require `cmp normal.before normal.after`.

- [ ] **Step 3: Preserve validation with profile artifacts**

Keep checks for kernel undefined symbols, no `INTERP`/`DYNAMIC`, `EM_X86_64`, `_start`, `kernel_main`, `_text`, and UEFI COFF exports. Add:

```make
validate-profile:
	@printf 'profile=%s triple=%s sysroot=%s capabilities=%s\n' "$(PROFILE)" "$(TARGET_TRIPLE)" "$(SYSROOT)" "$(PROFILE_CAPABILITIES)"
```

- [ ] **Step 4: Run the final matrix**

Run: `make PROFILE=x86_64-clang clean && make -j2 PROFILE=x86_64-clang disk.img validate test-build-contract-x86`

Run: `timeout 25 make PROFILE=aarch64-clang run-aarch64-uefi > /tmp/os01-aarch64.log 2>&1; status=$?; test "$status" -eq 124; grep -F 'aarch64 uefi handoff ok' /tmp/os01-aarch64.log; grep -F 'phase1 boot ok' /tmp/os01-aarch64.log`

Run: `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall && make PROFILE=x86_64-clang OS01_NETTEST=1 test-network`

Expected: x86 validates, aarch64 reaches both boot signatures before its expected QEMU timeout, and test variants leave the normal disk image intact.

- [ ] **Step 5: Document and commit**

Document profiles, output paths, allowed overrides, capability errors, variants, and deferred Termux `-lgcc` work. Record completion in the changelog only after the final matrix succeeds.

```bash
git add Makefile mk/components/run.mk docs tests/build_contract.sh
git commit -m "refactor(build): expose profile-aware Make interface"
```
