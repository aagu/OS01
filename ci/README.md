# OS01 CI image (`ci/Dockerfile`)

A custom Docker image that pins the same toolchain as the homeserver dev
box (clang 22.1.8 + qemu 11.1.1 + lld/llvm 22.x) so GitHub Actions and
local builds exercise the same compiler/relocation/qemu behaviour
regardless of which Ubuntu release the github-hosted runner is on.

## What it provides

- `clang`, `clang-22`, `clang++` — LLVM apt repo at
  `https://apt.llvm.org/llvm.sh 22 all`. The `all` flag registers the
  versioned binaries as the system defaults for the bare names via
  `update-alternatives`, so `clang` and `llvm-ar` both resolve without
  per-step overrides.
- `qemu-system-x86_64`, `qemu-system-aarch64` — built from
  `https://download.qemu.org/qemu-11.1.1.tar.xz` with only the two
  arches OS01 needs and every subsystem not used by the project
  disabled (`--disable-user --disable-vnc --disable-spice ...`) to
  keep the layer around ~200 MB instead of the upstream ~1.5 GB.
- CI deps that the OS01 build pipeline expects on PATH:
  `dosfstools` (mkfs.fat), `mtools` (mmd / mcopy — mkdisk needs both),
  `e2fsprogs` (mkfs.ext2 + debugfs), `device-tree-compiler` (dtc —
  the aarch64 SMP harness packs DTBs), `ripgrep` (build_contract.sh
  legacy-component scan), `git`, `wget`, `python3`.
- The build deps (glib/pixman/fdt/zlib/aio headers, ninja) are pulled
  in only for the QEMU build layer, then auto-removed in the same
  layer so the final image doesn't carry them.

## Build (local)

```
podman build -t os01-ci:dev -f ci/Dockerfile .
podman run --rm -it os01-ci:dev bash
```

Inside the container, the toolchain is on PATH:

```
$ clang --version | head -1
Ubuntu clang version 22.1.8
$ qemu-system-x86_64 --version | head -1
QEMU emulator version 11.1.1
$ qemu-system-aarch64 --version | head -1
QEMU emulator version 11.1.1
```

## Push to ghcr.io (CI uses this)

```
podman login ghcr.io -u <github-user>
podman push os01-ci:dev ghcr.io/aagu/os01-ci:latest
```

The image workflow (`.github/workflows/image.yml`) does the same
automatically on every push to master and tags both `:latest` and
`:${{ github.sha }}`. PRs that touch the Dockerfile or the workflow
files also rebuild but don't push (so a PR can't accidentally publish
an unreviewed image).

## CI workflow reference

`.github/workflows/ci.yml` runs every job inside this image via
`container.image: ghcr.io/aagu/os01-ci:latest`. The per-job
`apt-get install` steps from earlier revisions are gone — the image
already provides everything the OS01 build/test pipeline consumes.

## Why this exists

GitHub Actions' `ubuntu-24.04` runner ships qemu 8.2.2 and clang-18.
OS01's CI (issue AAGU-7) ran into two concrete failures on that
toolchain that the homeserver dev box (Arch + qemu 11.1.1 + clang-22)
doesn't reproduce:

1. **clang-18 R_X86_64_32 relocations against `init_task_union`**
   in the kernel link step — clang-22 emits `R_X86_64_64` implicitly,
   clang-18 needs `-fno-pie` to follow suit. The CI worked around
   this with source changes (`init_task_*` moved from header to
   `kernel/core/main.c`), but the divergence between CI and dev
   compilers is a future hazard. Pinning clang-22 in CI keeps them
   aligned.

2. **`qemu-system-aarch64 -machine dumpdtb=` returns non-zero on
   qemu 8.2.2** (Ubuntu 24.04 apt) but works on qemu 11.1.1 (dev).
   Building qemu 11.1.1 from source in CI removes the version drift
   entirely.

## Update cadence

When the homeserver's `clang` or `qemu` major version bumps, update
`QEMU_VERSION` in `ci/Dockerfile` and the `clang-22` argument to
`/tmp/llvm.sh` accordingly, rebuild the image, push `:latest` (and a
sha tag), and let the next CI run pick it up.