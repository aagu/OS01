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
     test -f "$base/artifacts/user/busybox.elf"; test -f "$base/artifacts/user/init.elf";
     test ! -e kernel/arch/x86_64/trampoline.bin;
     test ! -e libc/libc.a;
     test ! -e libc/libk.a;
     test -f "$base/staging/kernel-headers/manifest";
     test -f "$base/staging/libc/manifest" ;;
aarch64) test -f "$base/artifacts/kernel.elf"; test -f "$base/image/aarch64-uefi.img" ;;
sysroot) test -f "$base/sysroot/usr/include/kernel/bootinfo.h"; test -f "$base/sysroot/usr/lib/libc.a";
         test ! -e "$base/sysroot/usr/include/os01-removed-header.h";
         test -f "$base/sysroot/usr/lib/libk.a";
         test -f "$base/sysroot/usr/lib/libmbedtls.a";
         test -f "$base/sysroot/usr/lib/libm.a";
         test -f "$base/sysroot/usr/lib/librt.a";
         test -n "$(ls "$base/sysroot/usr/include/mbedtls"/*.h 2>/dev/null)" ;
         test "$(readlink "$base/sysroot")" = "sysroot-generations/$(find "$base/sysroot-generations" -maxdepth 1 -mindepth 1 -type d | xargs -n1 basename | sort -n | tail -1)" ;
         test -z "$(ls -A "$base/leases" 2>/dev/null)" ;;
targets) make -n PROFILE=x86_64-clang kernel.bin disk.img lib user validate test-syscall >/dev/null; ! make -n PROFILE=aarch64-clang user ;;
*) exit 64 ;;
esac
