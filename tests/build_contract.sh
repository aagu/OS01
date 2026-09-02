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
x86) test -f "$base/artifacts/kernel.bin"; test -f "$base/image/disk.img";
     test ! -e kernel/arch/x86_64/trampoline.bin;
     test ! -e libc/libc.a;
     test ! -e libc/libk.a;
     test -f "$base/staging/kernel-headers/manifest";
     test -f "$base/staging/libc/manifest" ;;
aarch64) test -f "$base/artifacts/kernel.elf"; test -f "$base/image/aarch64-uefi.img" ;;
sysroot) test -f "$base/sysroot/usr/include/kernel/bootinfo.h"; test -f "$base/sysroot/usr/lib/libc.a" ;;
targets) make -n PROFILE=x86_64-clang kernel.bin disk.img lib user validate test-syscall >/dev/null; ! make -n PROFILE=aarch64-clang user ;;
*) exit 64 ;;
esac
