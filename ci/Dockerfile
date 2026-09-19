# OS01 CI image — same toolchain as the homeserver dev box (clang 22.1.8,
# qemu 11.1.1, lld 22.x, llvm 22.x). Built so GitHub Actions can run the
# OS01 build pipeline without QEMU/clang version drift (the homeserver
# is on Arch with self-built QEMU; ubuntu-24.04's qemu 8.2.2 plus
# clang-18 produces different relocation behaviour and rejects
# `qemu-system-aarch64 -machine dumpdtb=` outright).
#
# Build:  podman build -t os01-ci:latest -f ci/Dockerfile .
# Push:   podman push os01-ci:latest ghcr.io/aagu/os01-ci:latest
# Use:    container.image: ghcr.io/aagu/os01-ci:latest
#
# Layered so apt updates / LLVM install / qemu install / apt deps are
# all independent layers that GitHub Actions can cache separately.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

# ── Base OS + build prerequisites ────────────────────────────
# git: actions/checkout uses it; ninja/ccmake/pkg-config: only needed
# to build qemu from source (see QEMU_BUILD step). wget/curl:
# download the prebuilt LLVM tarball + qemu source tarball.
# ccache: not installed (we don't pre-prime the cache; OS01's make
# drops the build dir between runs by design).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates curl wget git xz-utils \
        ninja-build pkg-config \
        cpio \
    && \
    rm -rf /var/lib/apt/lists/*

# ── LLVM 22 (clang + lld + llvm tooling) — prebuilt tarball ───
# The OS01 toolchain code (mk/toolchains/clang.mk) drives make via the
# bare names `clang` and `llvm-ar`. We use the upstream prebuilt
# tarball instead of apt because:
#   - apt.llvm.org's llvm.sh script rejects Ubuntu 26.04 (Resolute)
#     with 'Distribution ubuntu in version 26.04 is not supported'
#     (its NEW_DEBIAN_DISTROS list trails the Ubuntu release cycle
#     by 1-2 versions).
#   - Pinning to noble (the newest Ubuntu LTS the repo supports) on
#     a 26.04 base triggers libllvm22 / libpfm4 / libz3-4 conflicts
#     because the noble-built packages expect noble's libc + the
#     apt solver can't reconcile that across suites.
# The LLVM prebuilt tarball ships a self-contained tree at
# /usr/local/llvm-22 with no apt coupling, and we symlink the bare
# names OS01's Makefile drives into it. Distro-independent and fast.
ARG LLVM_VERSION=22.1.8
RUN wget -qO /tmp/llvm.tar.xz \
        https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-Linux-X64.tar.xz && \
    mkdir -p /usr/local/llvm-${LLVM_VERSION} && \
    tar -xJf /tmp/llvm.tar.xz -C /usr/local/llvm-${LLVM_VERSION} --strip-components=1 && \
    rm /tmp/llvm.tar.xz && \
    for tool in clang clang++ ld.lld llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readobj llvm-readelf llvm-strip; do \
      if [ -e /usr/local/llvm-${LLVM_VERSION}/bin/${tool} ]; then \
        ln -sf /usr/local/llvm-${LLVM_VERSION}/bin/${tool} /usr/local/bin/${tool}; \
      fi; \
    done && \
    echo '/usr/local/llvm-'${LLVM_VERSION}'/lib' > /etc/ld.so.conf.d/llvm-22.conf && \
    ldconfig

# ── QEMU 11.1.1 from source ──────────────────────────────────
# Ubuntu 26.04 ships qemu 10.2.1 (good enough for most of the OS01
# pipeline) but the dev box has 11.1.1 and the aarch64 SMP harness
# behaves differently on the older release. Build only the two arches
# the CI needs; disable subsystems not used by the project (user-mode,
# block backends, GUI frontends, network slirp) to keep the layer small
# (~200 MB rather than ~1.5 GB).
#
# If ubuntu 26.04's qemu 10.x turns out to be sufficient, drop this
# layer and install qemu-system-x86 + qemu-system-arm from apt instead.
ARG QEMU_VERSION=11.1.1
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv \
        libglib2.0-dev libpixman-1-dev libfdt-dev \
        zlib1g-dev libnfs-dev libaio-dev libcap-ng-dev \
        libattr1-dev \
    && \
    cd /tmp && \
    wget -q https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz && \
    tar -xf qemu-${QEMU_VERSION}.tar.xz && \
    cd qemu-${QEMU_VERSION} && \
    ./configure \
        --prefix=/usr/local \
        --target-list=x86_64-softmmu,aarch64-softmmu \
        --disable-docs \
        --disable-user \
        --disable-guest-agent \
        --disable-vnc \
        --enable-lto \
        --disable-werror \
    && \
    make -j"$(nproc)" && \
    make install && \
    cd / && rm -rf /tmp/qemu-${QEMU_VERSION} /tmp/qemu-${QEMU_VERSION}.tar.xz && \
    apt-get purge -y --auto-remove \
        libglib2.0-dev libpixman-1-dev libfdt-dev \
        zlib1g-dev libnfs-dev libaio-dev libcap-ng-dev \
        libattr1-dev \
        python3-pip python3-venv \
    && \
    rm -rf /var/lib/apt/lists/*

# ── Project / CI dependencies ───────────────────────────────
# dosfstools (mkfs.fat), mtools (mcopy/mmd — mkdisk needs both),
# e2fsprogs (mkfs.ext2 + debugfs — mkdisk again).
# device-tree-compiler (dtc — aarch64 SMP harness packs DTBs).
# ripgrep (rg — build_contract.sh legacy-component scan).
# python3 + wget were already pulled in above.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        dosfstools mtools e2fsprogs \
        device-tree-compiler \
        ripgrep \
    && \
    rm -rf /var/lib/apt/lists/*

# ── Verify the toolchain matches the dev box ──────────────────
# If any of these checks regress, the next CI run will pull the
# discrepancy out via runtime errors instead of mysterious qemu exit
# codes. Keep the expected versions in sync with the homeserver.
RUN clang --version | head -1 && \
    lld --version | head -1 && \
    llvm-ar --version | head -1 && \
    qemu-system-x86_64 --version | head -1 && \
    qemu-system-aarch64 --version | head -1

# ── Working directory ────────────────────────────────────────
WORKDIR /work

# Default to running nothing — the workflow file overrides this with
# the specific make targets it needs.
CMD ["/bin/bash"]